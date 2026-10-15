/*
 * rscrcpy-audio - OBS plugin receiving scrcpy audio through shared memory
 *
 * One source instance == one scrcpy instance: every source reads its own named
 * section, so several devices can be captured at the same time (each scrcpy
 * must then be started with its own --obs-audio-name=<name> and
 * --obs-video-name=<name>).
 *
 * The wire protocol is obs_shm_proto.h (audio) and obs_video_proto.h (video).
 */

#include <obs-module.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <windows.h>

#include "shm_reader.h"

#define PLUGIN_NAME "rscrcpy-audio"
#define PLUGIN_VERSION "0.1.2"
#define PLUGIN_LOG_PREFIX "[" PLUGIN_NAME "] "

/* rscrcpy_capture: the merged source fed by scrcpy's shared textures
 * (video) and its PCM ring (audio). It lives in capture.c / video_reader.c /
 * shm_reader.c and shares nothing with rscrcpy_audio other than this module. */
extern void rscrcpy_capture_register(void);

// OBS only ever passes its own timeline: our packets carry the host
// QueryPerformanceCounter time, which is the same clock (see protocol notes)
#define READER_WAIT_MS 50
#define LOG_PERIOD_MS 1000
#define PACKET_BATCH 8

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

struct rscrcpy_audio {
    obs_source_t *source;

    HANDLE thread;
    volatile LONG stop;

    struct sc_obs_shm_reader reader;

    // Copy of the "shm_name" setting, also read by the properties callback
    char shm_name[SC_OBS_SHM_NAME_MAX + 1];

    // 1 Hz logging, reset on every session
    uint64_t log_last_ms;
    uint64_t log_last_packets;
    uint64_t log_last_frames;
    uint64_t log_last_dropped;
    uint64_t log_last_missed;
    // Edge trigger so a stalled source is reported once, not every second
    bool log_stalled;
};

static enum speaker_layout
speakers_from_channels(uint32_t channels) {
    switch (channels) {
        case 1:
            return SPEAKERS_MONO;
        case 2:
            return SPEAKERS_STEREO;
        default:
            // The producer only ever configures 1 or 2 channels
            return SPEAKERS_STEREO;
    }
}

static void
apply_settings(struct rscrcpy_audio *ctx, obs_data_t *settings) {
    const char *name = obs_data_get_string(settings, "shm_name");
    if (!name || !*name) {
        name = "0";
    }

    if (strlen(name) > SC_OBS_SHM_NAME_MAX) {
        blog(LOG_WARNING, PLUGIN_LOG_PREFIX
             "shared memory name too long, truncating to %u characters",
             SC_OBS_SHM_NAME_MAX);
    }

    strncpy(ctx->shm_name, name, sizeof(ctx->shm_name) - 1);
    ctx->shm_name[sizeof(ctx->shm_name) - 1] = '\0';
}

static DWORD WINAPI
reader_thread(LPVOID param) {
    struct rscrcpy_audio *ctx = param;

    ctx->log_last_ms = GetTickCount64();

    while (!InterlockedCompareExchange(&ctx->stop, 0, 0)) {
        // Removing the source does not necessarily destroy it: the OBS frontend
        // (audio mixer, scene collection) can keep the object alive, in which
        // case our destroy callback never runs. Stop on our own instead of
        // leaking a thread that spins on a source nobody listens to.
        if (obs_source_removed(ctx->source)) {
            blog(LOG_INFO, PLUGIN_LOG_PREFIX
                 "source \"%s\" was removed from OBS, stopping reader",
                 obs_source_get_name(ctx->source));
            break;
        }

        if (!ctx->reader.connected) {
            if (!sc_obs_shm_reader_connect(&ctx->reader, ctx->shm_name)) {
                // No producer yet: retry quietly
                Sleep(200);
                continue;
            }
            blog(LOG_INFO, PLUGIN_LOG_PREFIX
                 "source \"%s\": connected to %s%s (rate=%u channels=%u)",
                 obs_source_get_name(ctx->source), SC_OBS_SHM_NAME_PREFIX,
                 ctx->shm_name, ctx->reader.sample_rate,
                 ctx->reader.channels);
        }

        if (!sc_obs_shm_reader_wait(&ctx->reader, READER_WAIT_MS)) {
            blog(LOG_INFO, PLUGIN_LOG_PREFIX
                 "source \"%s\": producer gone, waiting for %s%s",
                 obs_source_get_name(ctx->source), SC_OBS_SHM_NAME_PREFIX,
                 ctx->shm_name);
            sc_obs_shm_reader_disconnect(&ctx->reader);
            continue;
        }

        struct sc_obs_audio_packet packets[PACKET_BATCH];
        uint32_t count = sc_obs_shm_reader_poll(&ctx->reader, packets,
                                                PACKET_BATCH);
        for (uint32_t i = 0; i < count; i++) {
            const struct sc_obs_audio_packet *p = &packets[i];

            struct obs_source_audio out = {0};
            out.data[0] = (const uint8_t *) p->data;
            out.frames = p->frames;
            out.speakers = speakers_from_channels(p->channels);
            out.format = AUDIO_FORMAT_FLOAT;
            out.samples_per_sec = p->sample_rate;
            // Already on the OBS timeline (host QPC, nanoseconds)
            out.timestamp = p->ts_ns;

            obs_source_output_audio(ctx->source, &out);
        }

        uint64_t now = GetTickCount64();
        if (now - ctx->log_last_ms >= LOG_PERIOD_MS) {
            uint64_t packets = ctx->reader.packets;
            uint64_t frames = ctx->reader.frames;
            uint64_t dropped = ctx->reader.dropped;
            uint64_t missed = ctx->reader.missed;

            bool advanced = packets != ctx->log_last_packets;
            // Only "stalled" while attached: a disconnected reader is reported
            // by the "producer gone" message instead
            bool stalled = ctx->reader.connected
                        && sc_obs_shm_reader_is_stalled(&ctx->reader, 2000);

            if (advanced) {
                if (ctx->log_stalled) {
                    ctx->log_stalled = false;
                    blog(LOG_INFO, PLUGIN_LOG_PREFIX
                         "source \"%s\": audio resumed (total=%" PRIu64 ")",
                         obs_source_get_name(ctx->source), packets);
                }
                blog(LOG_INFO, PLUGIN_LOG_PREFIX
                     "source \"%s\": %" PRIu64 " pkt/s, %" PRIu64 " frames/s, "
                     "total=%" PRIu64 " dropped=%" PRIu64 " missed=%" PRIu64,
                     obs_source_get_name(ctx->source),
                     (packets - ctx->log_last_packets) * 1000
                         / (now - ctx->log_last_ms),
                     (frames - ctx->log_last_frames) * 1000
                         / (now - ctx->log_last_ms),
                     packets, dropped, missed);
            } else if (stalled && !ctx->log_stalled) {
                ctx->log_stalled = true;
                blog(LOG_WARNING, PLUGIN_LOG_PREFIX
                     "source \"%s\": no audio for 2 s (total=%" PRIu64
                     " dropped=%" PRIu64 " missed=%" PRIu64 ")",
                     obs_source_get_name(ctx->source), packets, dropped, missed);
            }

            ctx->log_last_packets = packets;
            ctx->log_last_frames = frames;
            ctx->log_last_dropped = dropped;
            ctx->log_last_missed = missed;
            ctx->log_last_ms = now;
        }
    }

    sc_obs_shm_reader_disconnect(&ctx->reader);
    return 0;
}

static bool
start_thread(struct rscrcpy_audio *ctx) {
    InterlockedExchange(&ctx->stop, 0);

    // Fresh session: restart the 1 Hz accounting
    ctx->log_last_ms = GetTickCount64();
    ctx->log_last_packets = 0;
    ctx->log_last_frames = 0;
    ctx->log_last_dropped = 0;
    ctx->log_last_missed = 0;
    ctx->log_stalled = false;

    ctx->thread = CreateThread(NULL, 0, reader_thread, ctx, 0, NULL);
    if (!ctx->thread) {
        blog(LOG_ERROR, PLUGIN_LOG_PREFIX "could not start reader thread");
        return false;
    }
    return true;
}

static void
stop_thread(struct rscrcpy_audio *ctx) {
    if (!ctx->thread) {
        return;
    }

    InterlockedExchange(&ctx->stop, 1);
    DWORD r = WaitForSingleObject(ctx->thread, 5000);
    if (r != WAIT_OBJECT_0) {
        blog(LOG_WARNING, PLUGIN_LOG_PREFIX "reader thread did not stop");
    }
    CloseHandle(ctx->thread);
    ctx->thread = NULL;
}

/* ------------------------------------------------------------------------- */
/* source callbacks                                                          */
/* ------------------------------------------------------------------------- */

static const char *
rscrcpy_audio_get_name(void *type_data) {
    UNUSED_PARAMETER(type_data);
    return obs_module_text("Source.Name");
}

static void
rscrcpy_audio_get_defaults(obs_data_t *settings) {
    obs_data_set_default_string(settings, "shm_name", "0");
}

static obs_properties_t *
rscrcpy_audio_properties(void *data) {
    struct rscrcpy_audio *ctx = data;
    obs_properties_t *props = obs_properties_create();

    obs_properties_add_text(props, "shm_name",
                            obs_module_text("ShmName"), OBS_TEXT_DEFAULT);

    // Read-only status, captured when the dialog is opened
    char status[320];
    if (ctx->reader.connected) {
        snprintf(status, sizeof(status), "%s: %s%s\n%s: %u Hz, %u ch\n"
                 "packets=%llu frames=%llu dropped=%llu missed=%llu",
                 obs_module_text("Status.Connected"),
                 SC_OBS_SHM_NAME_PREFIX, ctx->shm_name,
                 obs_module_text("Status.Format"),
                 ctx->reader.sample_rate, ctx->reader.channels,
                 (unsigned long long) ctx->reader.packets,
                 (unsigned long long) ctx->reader.frames,
                 (unsigned long long) ctx->reader.dropped,
                 (unsigned long long) ctx->reader.missed);
    } else {
        snprintf(status, sizeof(status), "%s: %s%s\n(%s)",
                 obs_module_text("Status.Waiting"),
                 SC_OBS_SHM_NAME_PREFIX, ctx->shm_name,
                 obs_module_text("Status.Waiting.Hint"));
    }
    obs_properties_add_text(props, "status", status, OBS_TEXT_INFO);

    return props;
}

static void *
rscrcpy_audio_create(obs_data_t *settings, obs_source_t *source) {
    struct rscrcpy_audio *ctx = bzalloc(sizeof(*ctx));
    ctx->source = source;
    apply_settings(ctx, settings);

    blog(LOG_INFO, PLUGIN_LOG_PREFIX
         "source \"%s\" created (shared memory suffix \"%s\")",
         obs_source_get_name(source), ctx->shm_name);

    start_thread(ctx);
    return ctx;
}

static void
rscrcpy_audio_destroy(void *data) {
    struct rscrcpy_audio *ctx = data;

    // The thread must be gone before the source can be released
    stop_thread(ctx);
    sc_obs_shm_reader_disconnect(&ctx->reader);

    blog(LOG_INFO, PLUGIN_LOG_PREFIX "source destroyed");
    bfree(ctx);
}

static void
rscrcpy_audio_update(void *data, obs_data_t *settings) {
    struct rscrcpy_audio *ctx = data;

    char previous[sizeof(ctx->shm_name)];
    strncpy(previous, ctx->shm_name, sizeof(previous));
    previous[sizeof(previous) - 1] = '\0';

    apply_settings(ctx, settings);

    if (strcmp(previous, ctx->shm_name) != 0) {
        blog(LOG_INFO, PLUGIN_LOG_PREFIX
             "shared memory name changed: \"%s\" -> \"%s\"",
             previous, ctx->shm_name);
        stop_thread(ctx);
        sc_obs_shm_reader_disconnect(&ctx->reader);
        start_thread(ctx);
    }
}

static struct obs_source_info rscrcpy_audio_info = {
    .id = "rscrcpy_audio",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_AUDIO,
    .get_name = rscrcpy_audio_get_name,
    .create = rscrcpy_audio_create,
    .destroy = rscrcpy_audio_destroy,
    .update = rscrcpy_audio_update,
    .get_defaults = rscrcpy_audio_get_defaults,
    .get_properties = rscrcpy_audio_properties,
};

/* ------------------------------------------------------------------------- */
/* module entry points                                                       */
/* ------------------------------------------------------------------------- */

bool
obs_module_load(void)
{
    obs_register_source(&rscrcpy_audio_info);
    rscrcpy_capture_register();

    blog(LOG_INFO, PLUGIN_LOG_PREFIX "module loaded: version %s, libobs API %u",
         PLUGIN_VERSION, LIBOBS_API_VER);

    /* That is the whole module: two consumer sources. scrcpy is started from
     * the command line and this plugin only reads what it publishes. */
    return true;
}

void
obs_module_unload(void)
{
    blog(LOG_INFO, PLUGIN_LOG_PREFIX "module unloaded");
}
