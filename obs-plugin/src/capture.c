/*
 * rscrcpy_capture - OBS video source for the scrcpy video bridge.
 *
 * One source instance == one scrcpy instance: the source attaches to the video
 * section published by scrcpy (--obs-video --obs-video-name=<name>) and draws
 * the D3D11 shared texture scrcpy writes every frame.
 *
 * The same source also carries the audio (shm_reader.c), so one source == one
 * device == picture + sound, with a single `offset_ms` to trim the A/V relation.
 * `rscrcpy_audio` stays for "audio only" setups.
 *
 * User decisions implemented here:
 *   - when the producer disappears the picture goes BLACK immediately (no
 *     "keep the last frame"): a stale frame should never look live;
 *   - no new frames for `offline_mark_s` (default 10 s) => black + "offline";
 *     for `offline_release_s` (default 30 s) => release every texture and drop
 *     to a slow reconnect probe. The scene object is never touched;
 *   - while the video is not ready, get_width/get_height return the user
 *     editable default size, which therefore only applies until the first frame
 *     arrives; once real geometry is known it is cached.
 *
 * Threading: the video half is on the graphics thread only (video_tick(), which
 * runs once per rendered frame while the source is active; texture
 * create/destroy must be inside obs_enter_graphics()/obs_leave_graphics()).
 * The audio half runs on its own reader thread, exactly like rscrcpy_audio:
 * audio must not depend on the video tick rate, and it must survive the source
 * sitting in a scene that is not the current program scene.
 */

#include <obs-module.h>
#include <graphics/graphics.h>

// mingw-w64 keeps the C-style COM helpers (ID3D11Device_QueryInterface, ...)
// behind COBJMACROS; without it the only way to call a method is through
// ->lpVtbl->, which reads badly for the two-line LUID lookup below.
#define COBJMACROS
#include <d3d11.h>
#include <dxgi.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "shm_reader.h"
#include "video_reader.h"

#define CAPTURE_LOG_PREFIX "[rscrcpy-capture] "

// Audio: how long the reader thread blocks on the producer's "data available"
// event before it re-checks the section, and how often it retries while no
// producer exists at all
#define AUDIO_READER_WAIT_MS 50
#define AUDIO_CONNECT_POLL_MS 200
#define AUDIO_PACKET_BATCH 8
// A connected reader with nothing arriving for this long is reported as stalled
// rather than "capturing": the producer may be long gone and the old text never
// said so (reported from a real session where it kept saying 采集正常).
#define AUDIO_STALE_MS 2000
// offset_ms is applied to the audio timestamps (nanoseconds on the OBS clock)
#define NS_PER_MS 1000000

// How often to retry attaching while no producer exists at all
#define CONNECT_POLL_MS 250
// ... and after we released everything (the user's "cut the source loose"
// behaviour): a slow probe, no churn
#define RELEASED_POLL_MS 1000
// Status heartbeat
#define LOG_PERIOD_MS 1000
// How often a *failed* generation open is retried
#define REOPEN_RETRY_MS 500

enum capture_state {
    CAPTURE_WAITING, // no producer
    CAPTURE_LIVE, // frames are arriving
    CAPTURE_OFFLINE, // producer present but silent: black, not yet released
    CAPTURE_RELEASED, // textures released, slow probe
    CAPTURE_REFUSED, // frames arrive but the GPU path is refused (LUID mismatch)
};

struct rscrcpy_capture {
    obs_source_t *source;
    struct sc_obs_video_reader reader;

    // settings
    char shm_name[SC_OBS_VIDEO_NAME_MAX + 1];
    uint32_t default_w;
    uint32_t default_h;
    uint32_t offline_mark_s;
    uint32_t offline_release_s;

    // geometry reported to OBS (real size once known, defaults before that)
    uint32_t width;
    uint32_t height;
    bool have_real_geometry;

    // Open textures for the current generation. Index == video_slot.
    gs_texture_t *tex[SC_OBS_VIDEO_SLOT_COUNT];
    uint32_t tex_handles[SC_OBS_VIDEO_SLOT_COUNT];
    uint32_t tex_width;
    uint32_t tex_height;
    uint32_t tex_slot_count;
    uint32_t tex_header_seq;
    uint32_t tex_session_seq;
    bool tex_open;

    // Generation we already reported on, so "open OK/FAILED" is logged once
    uint32_t logged_generation;
    // Generation whose open attempt failed (retried at REOPEN_RETRY_MS)
    uint32_t failed_generation;
    uint64_t last_reopen_ms;

    // Producer session bookkeeping
    uint32_t cur_session_seq;
    uint32_t last_frame_seq;
    uint32_t last_video_slot;
    uint64_t last_frame_ms;
    uint64_t frames_seen;

    // Offline ladder state (one-shot logging)
    bool offline_marked;
    bool gone_logged;
    bool released;
    uint64_t last_connect_ms;

    // Adapter identity: zero-copy sharing only works on the same adapter, so a
    // mismatch is reported loudly instead of showing a black picture for ever.
    uint64_t own_luid;
    bool own_luid_known;
    bool adapter_ok;
    bool adapter_mismatch_logged;

    // Draw decision, computed in video_tick and consumed by video_render
    int draw_slot;

    // --- audio (the same source carries the sound) -------------------------
    struct sc_obs_shm_reader audio_reader;
    // The UI's audio section name may be empty ("same as the video"), so this
    // holds the *effective* name: never empty once apply_settings() has run
    char audio_shm_name[SC_OBS_SHM_NAME_MAX + 1];
    bool audio_enabled;
    int offset_ms; // A/V trim; positive = the audio is played later
    // Last measured A/V relation (written by the graphics thread, read by the
    // properties dialog): video/audio staleness on the shared QPC clock
    int last_video_age_ms;
    int last_audio_age_ms;
    int last_sync_delta_ms;
    // QPC of the newest video frame, so the properties dialog can compute the
    // age on the spot instead of showing whatever the last 1 Hz tick cached
    uint64_t last_video_qpc_us;
    HANDLE audio_thread;
    volatile LONG audio_stop;
    uint64_t audio_packets_out;

    // 1 Hz audio accounting, reset on every audio session
    uint64_t audio_log_last_ms;
    uint64_t audio_log_last_packets;
    uint64_t audio_log_last_frames;
    uint64_t audio_log_last_dropped;
    uint64_t audio_log_last_missed;
    bool audio_log_stalled;

    // Statistics
    uint64_t last_log_ms;
    uint64_t last_log_frames;
    enum capture_state state;
};

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static const char *
state_text(enum capture_state state) {
    switch (state) {
        case CAPTURE_LIVE:
            return obs_module_text("Capture.State.Connected");
        case CAPTURE_OFFLINE:
            return obs_module_text("Capture.State.Offline");
        case CAPTURE_RELEASED:
            return obs_module_text("Capture.State.Released");
        case CAPTURE_REFUSED:
            return obs_module_text("Capture.State.AdapterRefused");
        case CAPTURE_WAITING:
        default:
            return obs_module_text("Capture.State.Waiting");
    }
}

static void
set_state(struct rscrcpy_capture *ctx, enum capture_state state) {
    if (ctx->state == state) {
        return;
    }
    ctx->state = state;
    blog(LOG_INFO, CAPTURE_LOG_PREFIX "source \"%s\": state -> %s",
         obs_source_get_name(ctx->source), state_text(state));
}

/* Release every open texture. Must be called with the graphics lock held. */
static void
destroy_textures_locked(struct rscrcpy_capture *ctx) {
    for (uint32_t i = 0; i < SC_OBS_VIDEO_SLOT_COUNT; i++) {
        if (ctx->tex[i]) {
            gs_texture_destroy(ctx->tex[i]);
            ctx->tex[i] = NULL;
        }
        ctx->tex_handles[i] = 0;
    }
    ctx->tex_open = false;
    ctx->tex_header_seq = 0;
    ctx->tex_session_seq = 0;
    ctx->tex_slot_count = 0;
    ctx->tex_width = 0;
    ctx->tex_height = 0;
}

static void
release_textures(struct rscrcpy_capture *ctx) {
    if (!ctx->tex_open && !ctx->tex[0]) {
        return;
    }
    obs_enter_graphics();
    destroy_textures_locked(ctx);
    obs_leave_graphics();
}

/*
 * Reads OBS's own adapter LUID. Called once, under the graphics lock, from the
 * first reopen attempt. Everything here is a read-only COM query.
 */
static bool
query_own_luid(struct rscrcpy_capture *ctx) {
    void *obj = gs_get_device_obj();
    if (!obj) {
        return false;
    }

    ID3D11Device *device = (ID3D11Device *) obj;
    IDXGIDevice *dxgi_device = NULL;
    if (FAILED(ID3D11Device_QueryInterface(device, &IID_IDXGIDevice,
                                           (void **) &dxgi_device))
            || !dxgi_device) {
        return false;
    }

    bool ok = false;
    IDXGIAdapter *adapter = NULL;
    if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi_device, &adapter)) && adapter) {
        DXGI_ADAPTER_DESC desc;
        if (SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &desc))) {
            ctx->own_luid = ((uint64_t) (uint32_t) desc.AdapterLuid.HighPart
                             << 32)
                          | (uint64_t) desc.AdapterLuid.LowPart;
            ok = true;
        }
        IDXGIAdapter_Release(adapter);
    }
    IDXGIDevice_Release(dxgi_device);
    return ok;
}

/* Whether every slot we are about to open carries a usable handle */
static bool
handles_usable(const struct sc_obs_video_snapshot *snap) {
    if (snap->slot_count < 1 || snap->slot_count > SC_OBS_VIDEO_SLOT_COUNT) {
        return false;
    }
    for (uint32_t i = 0; i < snap->slot_count; i++) {
        if (!sc_obs_video_reader_handle_usable(snap->handles[i])) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* generation (re)open                                                       */
/* ------------------------------------------------------------------------- */

/*
 * Opens the textures of a new generation. Everything libobs-side (destroy +
 * create) happens inside the graphics lock; the log lines happen after leaving
 * it so the lock is never extended by I/O.
 */
static void
reopen_generation(struct rscrcpy_capture *ctx,
                  const struct sc_obs_video_snapshot *snap) {
    gs_texture_t *opened[SC_OBS_VIDEO_SLOT_COUNT] = {0};
    uint32_t n_opened = 0;
    uint32_t got_w = 0;
    uint32_t got_h = 0;
    bool available = false;
    bool luid_ok = true;
    uint64_t own_luid = 0;

    obs_enter_graphics();

    destroy_textures_locked(ctx);

    available = gs_shared_texture_available();

    if (!ctx->own_luid_known) {
        ctx->own_luid_known = query_own_luid(ctx);
    }
    own_luid = ctx->own_luid;

    uint64_t producer_luid =
        ((uint64_t) (uint32_t) snap->adapter_luid_high << 32)
        | (uint64_t) snap->adapter_luid_low;
    if (ctx->own_luid_known && producer_luid
            && own_luid != producer_luid) {
        // Refuse instead of opening textures that can never work: a mismatch
        // means the two processes are not on the same GPU (hybrid laptops).
        luid_ok = false;
    }

    if (available && luid_ok) {
        for (uint32_t i = 0; i < snap->slot_count; i++) {
            opened[i] = gs_texture_open_shared(snap->handles[i]);
            if (opened[i]) {
                n_opened++;
                if (!got_w) {
                    got_w = gs_texture_get_width(opened[i]);
                    got_h = gs_texture_get_height(opened[i]);
                }
            }
        }
    }

    ctx->adapter_ok = luid_ok && available;

    if (n_opened == snap->slot_count) {
        for (uint32_t i = 0; i < SC_OBS_VIDEO_SLOT_COUNT; i++) {
            ctx->tex[i] = opened[i];
            ctx->tex_handles[i] = snap->handles[i];
        }
        ctx->tex_open = true;
        ctx->tex_width = got_w;
        ctx->tex_height = got_h;
        ctx->tex_slot_count = snap->slot_count;
        ctx->tex_header_seq = snap->video_header_seq;
        ctx->tex_session_seq = snap->session_seq;
    } else {
        // Partial open: drop whatever came back, we draw nothing this round
        for (uint32_t i = 0; i < SC_OBS_VIDEO_SLOT_COUNT; i++) {
            if (opened[i]) {
                gs_texture_destroy(opened[i]);
            }
        }
    }

    obs_leave_graphics();

    // One log line per generation, so a repeatedly failing open (retried below)
    // cannot flood the log; the generation token is reset on every new session.
    if (ctx->logged_generation == snap->video_header_seq) {
        return;
    }
    ctx->logged_generation = snap->video_header_seq;

    if (!available) {
        blog(LOG_ERROR, CAPTURE_LOG_PREFIX
             "source \"%s\": gs_shared_texture_available() is false: this OBS "
             "graphics backend cannot open shared textures",
             obs_source_get_name(ctx->source));
        return;
    }

    if (!luid_ok) {
        if (!ctx->adapter_mismatch_logged) {
            ctx->adapter_mismatch_logged = true;
            blog(LOG_ERROR, CAPTURE_LOG_PREFIX
                 "source \"%s\": adapter LUID mismatch (OBS=0x%llX "
                 "producer=0x%llX): the two processes are on different GPUs, "
                 "so the zero-copy path is refused. Make OBS and scrcpy use the "
                 "same adapter (or use the CPU fallback)",
                 obs_source_get_name(ctx->source),
                 (unsigned long long) own_luid,
                 (unsigned long long) producer_luid);
        }
        return;
    }

    if (n_opened == snap->slot_count) {
        blog(LOG_INFO, CAPTURE_LOG_PREFIX
             "source \"%s\": OPEN OK generation=%u tex=%ux%u producer=%ux%u "
             "slots=%u frame_seq=%u adapter_luid=0x%llX pid=%u",
             obs_source_get_name(ctx->source), snap->video_header_seq, got_w,
             got_h, snap->width, snap->height, snap->slot_count,
             snap->frame_seq, (unsigned long long) producer_luid,
             snap->producer_pid);
        if (got_w != snap->width || got_h != snap->height) {
            blog(LOG_WARNING, CAPTURE_LOG_PREFIX
                 "source \"%s\": size mismatch: libobs reports %ux%u but the "
                 "producer wrote %ux%u",
                 obs_source_get_name(ctx->source), got_w, got_h, snap->width,
                 snap->height);
        }
    } else {
        blog(LOG_WARNING, CAPTURE_LOG_PREFIX
             "source \"%s\": could only open %u of %u shared textures "
             "(generation=%u, first handle=0x%08X); drawing nothing until the "
             "next generation",
             obs_source_get_name(ctx->source), n_opened, snap->slot_count,
             snap->video_header_seq, snap->handles[0]);
    }
}

/* ------------------------------------------------------------------------- */
/* tick                                                                      */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* A/V trim measurement                                                      */
/* ------------------------------------------------------------------------- */

// Host QPC in microseconds. The video frames (video_qpc_us) and the audio
// packets (ts_ns / 1000) carry this very clock, so the two "ages" printed by
// the heartbeat are comparable and their difference is the A/V trim the user
// should dial into offset_ms.
static int64_t
qpc_now_us(void) {
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    if (freq.QuadPart <= 0) {
        return 0;
    }
    return (int64_t) ((double) count.QuadPart * 1000000.0
                      / (double) freq.QuadPart);
}

/* ------------------------------------------------------------------------- */
/* audio half (own reader thread, mirrors rscrcpy_audio)                     */
/* ------------------------------------------------------------------------- */

// The producer only ever configures 1 or 2 channels
static enum speaker_layout
speakers_from_channels(uint32_t channels) {
    return channels == 1 ? SPEAKERS_MONO : SPEAKERS_STEREO;
}

static void
capture_audio_log(struct rscrcpy_capture *ctx) {
    uint64_t now = GetTickCount64();
    if (now - ctx->audio_log_last_ms < LOG_PERIOD_MS) {
        return;
    }

    uint64_t packets = ctx->audio_reader.packets;
    uint64_t frames = ctx->audio_reader.frames;
    uint64_t dropped = ctx->audio_reader.dropped;
    uint64_t missed = ctx->audio_reader.missed;

    bool advanced = packets != ctx->audio_log_last_packets;
    // Only "stalled" while attached: a disconnected reader is reported by the
    // "producer gone" message instead
    bool stalled = ctx->audio_reader.connected
                && sc_obs_shm_reader_is_stalled(&ctx->audio_reader, 2000);

    if (advanced) {
        if (ctx->audio_log_stalled) {
            ctx->audio_log_stalled = false;
            blog(LOG_INFO, CAPTURE_LOG_PREFIX
                 "source \"%s\": audio resumed (total=%" PRIu64 ")",
                 obs_source_get_name(ctx->source), packets);
        }
        blog(LOG_INFO, CAPTURE_LOG_PREFIX
             "source \"%s\": audio %" PRIu64 " pkt/s, %" PRIu64 " frames/s, "
             "total=%" PRIu64 " dropped=%" PRIu64 " missed=%" PRIu64
             ", offset=%d ms",
             obs_source_get_name(ctx->source),
             (packets - ctx->audio_log_last_packets) * 1000
                 / (now - ctx->audio_log_last_ms),
             (frames - ctx->audio_log_last_frames) * 1000
                 / (now - ctx->audio_log_last_ms),
             packets, dropped, missed, ctx->offset_ms);
    } else if (stalled && !ctx->audio_log_stalled) {
        ctx->audio_log_stalled = true;
        blog(LOG_WARNING, CAPTURE_LOG_PREFIX
             "source \"%s\": no audio for 2 s (total=%" PRIu64
             " dropped=%" PRIu64 " missed=%" PRIu64 ")",
             obs_source_get_name(ctx->source), packets, dropped, missed);
    }

    ctx->audio_log_last_ms = now;
    ctx->audio_log_last_packets = packets;
    ctx->audio_log_last_frames = frames;
    ctx->audio_log_last_dropped = dropped;
    ctx->audio_log_last_missed = missed;
}

static DWORD WINAPI
capture_audio_thread(LPVOID param) {
    struct rscrcpy_capture *ctx = param;
    ctx->audio_log_last_ms = GetTickCount64();

    while (!InterlockedCompareExchange(&ctx->audio_stop, 0, 0)) {
        // Removing the source does not necessarily destroy it: the OBS frontend
        // (audio mixer, scene collection) can keep the object alive, in which
        // case our destroy callback never runs. Stop on our own instead of
        // leaking a thread that spins on a source nobody listens to.
        if (obs_source_removed(ctx->source)) {
            blog(LOG_INFO, CAPTURE_LOG_PREFIX
                 "source \"%s\": audio reader stopping (source removed)",
                 obs_source_get_name(ctx->source));
            break;
        }

        if (!ctx->audio_reader.connected) {
            if (!sc_obs_shm_reader_connect(&ctx->audio_reader,
                                           ctx->audio_shm_name)) {
                // No producer yet: retry quietly
                Sleep(AUDIO_CONNECT_POLL_MS);
                continue;
            }
            blog(LOG_INFO, CAPTURE_LOG_PREFIX
                 "source \"%s\": audio connected to %s%s (rate=%u channels=%u)",
                 obs_source_get_name(ctx->source), SC_OBS_SHM_NAME_PREFIX,
                 ctx->audio_shm_name, ctx->audio_reader.sample_rate,
                 ctx->audio_reader.channels);
        }

        if (!sc_obs_shm_reader_wait(&ctx->audio_reader, AUDIO_READER_WAIT_MS)) {
            blog(LOG_INFO, CAPTURE_LOG_PREFIX
                 "source \"%s\": audio producer gone, waiting for %s%s",
                 obs_source_get_name(ctx->source), SC_OBS_SHM_NAME_PREFIX,
                 ctx->audio_shm_name);
            sc_obs_shm_reader_disconnect(&ctx->audio_reader);
            continue;
        }

        struct sc_obs_audio_packet packets[AUDIO_PACKET_BATCH];
        uint32_t count = sc_obs_shm_reader_poll(&ctx->audio_reader, packets,
                                                AUDIO_PACKET_BATCH);
        for (uint32_t i = 0; i < count; i++) {
            const struct sc_obs_audio_packet *p = &packets[i];

            struct obs_source_audio out = {0};
            out.data[0] = (const uint8_t *) p->data;
            out.frames = p->frames;
            out.speakers = speakers_from_channels(p->channels);
            out.format = AUDIO_FORMAT_FLOAT;
            out.samples_per_sec = p->sample_rate;
            // Already on the OBS timeline (host QPC, nanoseconds) — the very
            // same clock the video timestamps use — so offset_ms is all that is
            // needed to trim the A/V relation.
            int64_t ts = (int64_t) p->ts_ns
                       + (int64_t) ctx->offset_ms * NS_PER_MS;
            out.timestamp = (uint64_t) (ts < 0 ? 0 : ts);

            obs_source_output_audio(ctx->source, &out);
            ctx->audio_packets_out++;
        }

        capture_audio_log(ctx);
    }

    sc_obs_shm_reader_disconnect(&ctx->audio_reader);
    return 0;
}

static bool
capture_audio_start(struct rscrcpy_capture *ctx) {
    if (!ctx->audio_enabled || ctx->audio_thread) {
        return false;
    }

    InterlockedExchange(&ctx->audio_stop, 0);

    // Fresh session: restart the 1 Hz accounting
    ctx->audio_log_last_ms = GetTickCount64();
    ctx->audio_log_last_packets = 0;
    ctx->audio_log_last_frames = 0;
    ctx->audio_log_last_dropped = 0;
    ctx->audio_log_last_missed = 0;
    ctx->audio_log_stalled = false;
    ctx->audio_packets_out = 0;

    ctx->audio_thread = CreateThread(NULL, 0, capture_audio_thread, ctx, 0,
                                     NULL);
    if (!ctx->audio_thread) {
        blog(LOG_ERROR, CAPTURE_LOG_PREFIX "could not start the audio thread");
        return false;
    }

    blog(LOG_INFO, CAPTURE_LOG_PREFIX
         "source \"%s\": audio enabled (section %s%s, offset %d ms)",
         obs_source_get_name(ctx->source), SC_OBS_SHM_NAME_PREFIX,
         ctx->audio_shm_name, ctx->offset_ms);
    return true;
}

static void
capture_audio_stop(struct rscrcpy_capture *ctx) {
    if (!ctx->audio_thread) {
        return;
    }

    InterlockedExchange(&ctx->audio_stop, 1);
    DWORD r = WaitForSingleObject(ctx->audio_thread, 5000);
    if (r != WAIT_OBJECT_0) {
        blog(LOG_WARNING, CAPTURE_LOG_PREFIX "audio thread did not stop");
    }
    CloseHandle(ctx->audio_thread);
    ctx->audio_thread = NULL;
}

static void
capture_tick(void *data, float seconds) {
    UNUSED_PARAMETER(seconds);

    struct rscrcpy_capture *ctx = data;
    uint64_t now = GetTickCount64();

    // A removed source can be kept alive by the OBS frontend, in which case
    // destroy() is never called. Give up what we hold instead of
    // keeping a mapping (and GPU textures) alive for a source nobody uses.
    if (obs_source_removed(ctx->source)) {
        if (ctx->reader.connected || ctx->tex_open) {
            blog(LOG_INFO, CAPTURE_LOG_PREFIX
                 "source \"%s\" was removed from OBS, releasing video state",
                 obs_source_get_name(ctx->source));
            release_textures(ctx);
            sc_obs_video_reader_disconnect(&ctx->reader);
            ctx->released = true;
            ctx->state = CAPTURE_RELEASED;
        }
        ctx->draw_slot = -1;
        return;
    }

    if (!ctx->reader.connected) {
        uint32_t poll = ctx->released ? RELEASED_POLL_MS : CONNECT_POLL_MS;
        if (now - ctx->last_connect_ms < poll) {
            ctx->draw_slot = -1;
            return;
        }
        ctx->last_connect_ms = now;

        if (!sc_obs_video_reader_connect(&ctx->reader, ctx->shm_name)) {
            ctx->draw_slot = -1;
            set_state(ctx, CAPTURE_WAITING);
            return;
        }

        blog(LOG_INFO, CAPTURE_LOG_PREFIX
             "source \"%s\": attached to %s%s",
             obs_source_get_name(ctx->source), SC_OBS_VIDEO_NAME_PREFIX,
             ctx->shm_name);
        // Fresh attachment: force a full re-evaluation of the next snapshot
        ctx->cur_session_seq = 0;
        ctx->last_frame_seq = 0;
        ctx->last_log_frames = 0;
        ctx->last_frame_ms = now;
        ctx->gone_logged = false;
        ctx->offline_marked = false;
        ctx->released = false;
        ctx->logged_generation = 0;
    }

    struct sc_obs_video_snapshot snap;
    if (!sc_obs_video_reader_snapshot(&ctx->reader, &snap)) {
        if (sc_obs_video_reader_is_alive(&ctx->reader)) {
            // Transitional (a new producer is mid-initialisation): keep the
            // current state for this tick rather than flapping to black.
            return;
        }

        if (!ctx->gone_logged) {
            ctx->gone_logged = true;
            blog(LOG_WARNING, CAPTURE_LOG_PREFIX
                 "source \"%s\": producer gone (section magic cleared); "
                 "picture is black until a producer appears again",
                 obs_source_get_name(ctx->source));
        }

        release_textures(ctx);
        sc_obs_video_reader_disconnect(&ctx->reader);
        ctx->released = true;
        ctx->draw_slot = -1;
        set_state(ctx, CAPTURE_RELEASED);
        return;
    }

    // --- a new producer session (possibly reusing the same section) ---------
    if (snap.session_seq != ctx->cur_session_seq) {
        blog(LOG_INFO, CAPTURE_LOG_PREFIX
             "source \"%s\": producer session %u (pid=%u, %ux%u, generation=%u)",
             obs_source_get_name(ctx->source), snap.session_seq,
             snap.producer_pid, snap.width, snap.height,
             snap.video_header_seq);
        ctx->cur_session_seq = snap.session_seq;
        ctx->last_frame_seq = snap.frame_seq;
        // Keep the 1 Hz fps counter in step with the new session's counters
        ctx->last_log_frames = snap.frame_seq;
        ctx->last_log_ms = now;
        ctx->last_frame_ms = now;
        ctx->frames_seen = 0;
        ctx->gone_logged = false;
        ctx->offline_marked = false;
        ctx->released = false;
        ctx->adapter_mismatch_logged = false;
        ctx->logged_generation = 0;
    }

    // --- geometry ----------------------------------------------------------
    if (snap.width && snap.height) {
        ctx->width = snap.width;
        ctx->height = snap.height;
        ctx->have_real_geometry = true;
    } else {
        ctx->width = ctx->default_w;
        ctx->height = ctx->default_h;
        ctx->have_real_geometry = false;
    }

    // --- (re)open the textures when the generation changed ------------------
    bool ready = snap.format != SC_OBS_VIDEO_FORMAT_NONE
              && snap.video_header_seq != 0 && handles_usable(&snap);
    if (ready) {
        bool changed = !ctx->tex_open
                    || ctx->tex_header_seq != snap.video_header_seq
                    || ctx->tex_session_seq != snap.session_seq
                    || ctx->tex_slot_count != snap.slot_count
                    || ctx->tex_width != snap.width
                    || ctx->tex_height != snap.height;
        for (uint32_t i = 0; !changed && i < snap.slot_count; i++) {
            if (ctx->tex_handles[i] != snap.handles[i]) {
                changed = true;
            }
        }

        // A failed generation is retried, but at a sane rate: opening shared
        // textures every frame while the other process is still starting up
        // would be pure churn.
        bool may_retry = true;
        if (ctx->failed_generation == snap.video_header_seq
                && snap.video_header_seq != 0
                && now - ctx->last_reopen_ms < REOPEN_RETRY_MS) {
            may_retry = false;
        }

        if (changed && may_retry) {
            reopen_generation(ctx, &snap);
            ctx->last_reopen_ms = now;
            ctx->failed_generation = ctx->tex_open ? 0 : snap.video_header_seq;
        }
    }

    // --- frame progress -----------------------------------------------------
    if (snap.frame_seq != ctx->last_frame_seq) {
        if (ctx->offline_marked || ctx->state != CAPTURE_LIVE) {
            blog(LOG_INFO, CAPTURE_LOG_PREFIX "source \"%s\": video resumed",
                 obs_source_get_name(ctx->source));
        }
        ctx->last_frame_seq = snap.frame_seq;
        ctx->last_frame_ms = now;
        ctx->offline_marked = false;
        ctx->released = false;
        set_state(ctx, CAPTURE_LIVE);
    }
    ctx->last_video_slot = snap.video_slot;
    ctx->last_video_qpc_us = snap.video_qpc_us;

    // --- offline ladder -----------------------------------------------------
    uint64_t silent_ms = now - ctx->last_frame_ms;
    uint64_t mark_ms = (uint64_t) ctx->offline_mark_s * 1000ull;
    uint64_t release_ms = (uint64_t) ctx->offline_release_s * 1000ull;

    if (release_ms && silent_ms >= release_ms) {
        if (!ctx->released) {
            ctx->released = true;
            release_textures(ctx);
            blog(LOG_WARNING, CAPTURE_LOG_PREFIX
                 "source \"%s\": no video for %" PRIu64 " s: released all "
                 "textures, probing for a producer every %u ms",
                 obs_source_get_name(ctx->source), silent_ms / 1000ull,
                 RELEASED_POLL_MS);
        }
        set_state(ctx, CAPTURE_RELEASED);
    } else if (mark_ms && silent_ms >= mark_ms) {
        if (!ctx->offline_marked) {
            ctx->offline_marked = true;
            blog(LOG_WARNING, CAPTURE_LOG_PREFIX
                 "source \"%s\": no video for %" PRIu64 " s: showing black",
                 obs_source_get_name(ctx->source), silent_ms / 1000ull);
        }
        set_state(ctx, CAPTURE_OFFLINE);
    }

    // --- decide what to draw (video_render must not do syscalls) ------------
    ctx->draw_slot = -1;
    if (!ctx->released && ctx->tex_open && ctx->adapter_ok && !ctx->offline_marked
            && snap.frame_seq != 0) {
        uint32_t slot = snap.video_slot;
        if (slot >= ctx->tex_slot_count) {
            slot = 0;
        }
        if (ctx->tex[slot]) {
            ctx->draw_slot = (int) slot;
        }
    }

    // --- 1 Hz heartbeat -----------------------------------------------------
    if (now - ctx->last_log_ms >= LOG_PERIOD_MS) {
        uint64_t frames = snap.frame_seq;
        // A counter that went backwards means a new producer session (or a
        // wrap): never subtract, an unsigned underflow here once printed
        // "18446744073709549568.0 fps" in a reconnect test.
        uint64_t delta = frames >= ctx->last_log_frames
                       ? frames - ctx->last_log_frames
                       : frames;
        uint64_t dt = now - ctx->last_log_ms;
        double fps = dt ? (double) delta * 1000.0 / (double) dt : 0.0;

        if (delta || ctx->state != CAPTURE_LIVE) {
            // How stale the newest picture / the newest sound are, on one clock:
            // "delta" is what offset_ms has to cancel to line them up
            int64_t now_us = qpc_now_us();
            int video_age = snap.video_qpc_us > 0
                          ? (int) ((now_us - snap.video_qpc_us) / 1000) : -1;
            int audio_age = -1;
            uint64_t audio_qpc = ctx->audio_reader.last_qpc_us;
            if (ctx->audio_reader.connected && audio_qpc > 0) {
                audio_age = (int) ((now_us - (int64_t) audio_qpc) / 1000);
            }

            ctx->last_video_age_ms = video_age;
            ctx->last_audio_age_ms = audio_age;
            ctx->last_sync_delta_ms = (video_age >= 0 && audio_age >= 0)
                                    ? video_age - audio_age : 0;

            blog(LOG_INFO, CAPTURE_LOG_PREFIX
                 "source \"%s\": %.1f fps, frames=%" PRIu64 " generation=%u "
                 "size=%ux%u handles=%u state=%s video_age=%dms audio_age=%dms "
                 "delta=%dms offset=%dms",
                 obs_source_get_name(ctx->source), fps, frames,
                 snap.video_header_seq, ctx->width, ctx->height,
                 ctx->tex_open ? ctx->tex_slot_count : 0,
                 state_text(ctx->state), video_age, audio_age,
                 (video_age >= 0 && audio_age >= 0) ? video_age - audio_age : 0,
                 ctx->offset_ms);
        }
        ctx->last_log_frames = frames;
        ctx->last_log_ms = now;
        ctx->frames_seen = frames;
    }
}

static void
capture_render(void *data, gs_effect_t *effect) {
    UNUSED_PARAMETER(effect);

    struct rscrcpy_capture *ctx = data;
    if (!ctx || ctx->draw_slot < 0) {
        // Black on purpose: no frame, producer gone, or offline for too long
        return;
    }

    gs_texture_t *tex = ctx->tex[ctx->draw_slot];
    if (!tex) {
        return;
    }

    // Already inside the graphics context here: never call obs_enter_graphics()
    obs_source_draw(tex, 0, 0, ctx->tex_width, ctx->tex_height, false);
}

/* ------------------------------------------------------------------------- */
/* Consumer only: no start/stop, no device enumeration, no process ownership.
 * scrcpy is started from the command line; this source just reads what it
 * publishes. */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* source callbacks                                                          */
/* ------------------------------------------------------------------------- */

static const char *
capture_get_name(void *type_data) {
    UNUSED_PARAMETER(type_data);
    return obs_module_text("Capture.Name");
}

static void
capture_get_defaults(obs_data_t *settings) {
    obs_data_set_default_string(settings, "shm_name", "dev1");
    obs_data_set_default_int(settings, "default_width", 1920);
    obs_data_set_default_int(settings, "default_height", 1080);
    obs_data_set_default_int(settings, "offline_mark_s", 10);
    obs_data_set_default_int(settings, "offline_release_s", 30);
    obs_data_set_default_bool(settings, "audio_enabled", true);
    // Empty == "same section as the video": scrcpy is given one name and both
    // sides read it (see the protocol notes in README).
    obs_data_set_default_string(settings, "audio_shm_name", "");
    obs_data_set_default_int(settings, "offset_ms", 0);
}

static void
apply_settings(struct rscrcpy_capture *ctx, obs_data_t *settings) {
    const char *name = obs_data_get_string(settings, "shm_name");
    if (!name || !*name) {
        name = "dev1";
    }
    if (strlen(name) > SC_OBS_VIDEO_NAME_MAX) {
        blog(LOG_WARNING, CAPTURE_LOG_PREFIX
             "shared memory name too long, truncating to %u characters",
             SC_OBS_VIDEO_NAME_MAX);
    }
    strncpy(ctx->shm_name, name, sizeof(ctx->shm_name) - 1);
    ctx->shm_name[sizeof(ctx->shm_name) - 1] = '\0';

    int64_t def_w = obs_data_get_int(settings, "default_width");
    int64_t def_h = obs_data_get_int(settings, "default_height");
    ctx->default_w = (uint32_t) (def_w < 16 ? 1920 : def_w);
    ctx->default_h = (uint32_t) (def_h < 16 ? 1080 : def_h);

    int64_t mark = obs_data_get_int(settings, "offline_mark_s");
    int64_t release = obs_data_get_int(settings, "offline_release_s");
    // Both are "seconds of silence"; a release shorter than the mark would be
    // nonsense, and 0 would black out immediately, so clamp both.
    uint32_t mark_s = (uint32_t) (mark < 1 ? 1 : mark > 3600 ? 3600 : mark);
    uint32_t release_s =
        (uint32_t) (release < (int64_t) mark_s ? mark_s
                                               : release > 3600 ? 3600
                                                                : release);
    ctx->offline_mark_s = mark_s;
    ctx->offline_release_s = release_s;

    ctx->audio_enabled = obs_data_get_bool(settings, "audio_enabled");

    const char *aname = obs_data_get_string(settings, "audio_shm_name");
    if (!aname || !*aname) {
        // "Same as the video": keep following the video name
        strncpy(ctx->audio_shm_name, ctx->shm_name,
                sizeof(ctx->audio_shm_name) - 1);
        ctx->audio_shm_name[sizeof(ctx->audio_shm_name) - 1] = '\0';
    } else {
        if (strlen(aname) > SC_OBS_SHM_NAME_MAX) {
            blog(LOG_WARNING, CAPTURE_LOG_PREFIX
                 "audio shared memory name too long, truncating to %u characters",
                 SC_OBS_SHM_NAME_MAX);
        }
        strncpy(ctx->audio_shm_name, aname,
                sizeof(ctx->audio_shm_name) - 1);
        ctx->audio_shm_name[sizeof(ctx->audio_shm_name) - 1] = '\0';
    }

    int64_t offset = obs_data_get_int(settings, "offset_ms");
    ctx->offset_ms = (int) (offset < -500 ? -500 : offset > 500 ? 500 : offset);

    /* Only the settings read above are used; OBS ignores any other key a scene
     * may carry. */
}

static obs_properties_t *
capture_properties(void *data) {
    struct rscrcpy_capture *ctx = data;
    obs_properties_t *props = obs_properties_create();

    /* Consumer only: every field below says WHICH shared memory to read and how
     * to present it. How the producer was started (device, bitrate, window, ...)
     * is the command line's business, not this panel's. */

    /* ---- 1. video: which shared section to read ------------------------ */
    obs_properties_t *vid = obs_properties_create();
    obs_properties_add_text(vid, "shm_name",
                            obs_module_text("Capture.Video.ShmName"),
                            OBS_TEXT_DEFAULT);
    obs_properties_add_int(vid, "default_width",
                           obs_module_text("Capture.DefaultWidth"), 16, 16384, 1);
    obs_properties_add_int(vid, "default_height",
                           obs_module_text("Capture.DefaultHeight"), 16, 16384,
                           1);
    obs_properties_add_group(props, "video", obs_module_text("Capture.Group.Video"),
                             OBS_GROUP_NORMAL, vid);

    /* ---- 3. audio: what we read, and what scrcpy should send ---------- */
    obs_properties_t *audio = obs_properties_create();
    obs_properties_add_bool(audio, "audio_enabled",
                            obs_module_text("Capture.Audio.Enabled"));
    obs_properties_add_text(audio, "audio_shm_name",
                            obs_module_text("Capture.Audio.ShmName"),
                            OBS_TEXT_DEFAULT);
    obs_properties_add_int(audio, "offset_ms",
                           obs_module_text("Capture.Audio.Offset"), -500, 500,
                           1);

    char astatus[512];
    if (ctx && !ctx->audio_enabled) {
        snprintf(astatus, sizeof(astatus), "%s: %s",
                 obs_module_text("Capture.Label.Audio"),
                 obs_module_text("Capture.Audio.Off"));
    } else if (ctx && ctx->audio_reader.connected) {
        // Judge FRESHNESS, not just "attached": audio_reader.connected stays true
        // for as long as the ring is mapped, i.e. long after the producer is
        // gone. The old text therefore kept saying "capturing" on a dead device,
        // which is the worst possible thing to tell somebody debugging one.
        // Both ages are computed right here, on the spot, so the dialog never
        // shows a number frozen by the 1 Hz tick.
        int64_t now_us = qpc_now_us();
        int v_age = ctx->last_video_qpc_us
                  ? (int) ((now_us - (int64_t) ctx->last_video_qpc_us) / 1000) : -1;
        int a_age = ctx->audio_reader.last_qpc_us
                  ? (int) ((now_us - (int64_t) ctx->audio_reader.last_qpc_us) / 1000) : -1;
        bool fresh = (a_age >= 0 && a_age < AUDIO_STALE_MS);
        const char *aword = fresh ? obs_module_text("Capture.Audio.On")
                                  : obs_module_text("Capture.Audio.Stalled");
        snprintf(astatus, sizeof(astatus),
                 "%s: %s\n%u Hz, %u ch, %" PRIu64 " pkt, offset %d ms\n"
                 "%s: %d / %d ms   (%s %d ms)\n%s",
                 obs_module_text("Capture.Label.Audio"), aword,
                 ctx->audio_reader.sample_rate, ctx->audio_reader.channels,
                 ctx->audio_packets_out, ctx->offset_ms,
                 obs_module_text("Capture.Label.Sync"), v_age, a_age,
                 obs_module_text("Capture.Label.Delta"),
                 (v_age >= 0 && a_age >= 0) ? v_age - a_age : 0,
                 obs_module_text("Capture.Sync.Hint"));
    } else {
        snprintf(astatus, sizeof(astatus), "%s: %s (%s%s)",
                 obs_module_text("Capture.Label.Audio"),
                 obs_module_text("Capture.State.Waiting"),
                 SC_OBS_SHM_NAME_PREFIX, ctx ? ctx->audio_shm_name : "");
    }
    obs_properties_add_text(audio, "audio_status", astatus, OBS_TEXT_INFO);
    obs_properties_add_text(audio, "audio_hint",
                            obs_module_text("Capture.Audio.Hint"), OBS_TEXT_INFO);

    obs_properties_add_group(props, "audio",
                             obs_module_text("Capture.Group.Audio"),
                             OBS_GROUP_NORMAL, audio);

    /* ---- 4. advanced -------------------------------------------------- */
    obs_properties_t *adv = obs_properties_create();
    obs_properties_add_int(adv, "offline_mark_s",
                           obs_module_text("Capture.OfflineMark"), 1, 3600, 1);
    obs_properties_add_int(adv, "offline_release_s",
                           obs_module_text("Capture.OfflineRelease"), 1, 3600,
                           1);
    obs_properties_add_group(props, "advanced",
                             obs_module_text("Capture.Group.Advanced"),
                             OBS_GROUP_NORMAL, adv);

    /* ---- 5. status (read-only) ---------------------------------------- */
    obs_properties_t *ctl = obs_properties_create();

    char status[512];
    if (ctx && ctx->reader.connected) {
        snprintf(status, sizeof(status),
                 "%s: %s\n%s: %ux%u\n%s: %u  (%s %u)\n%s: %u\n%s: %s",
                 obs_module_text("Capture.Label.State"),
                 state_text(ctx->state),
                 obs_module_text("Capture.Label.Picture"),
                 ctx->tex_open ? ctx->tex_width : ctx->width,
                 ctx->tex_open ? ctx->tex_height : ctx->height,
                 obs_module_text("Capture.Label.Handles"),
                 ctx->tex_open ? ctx->tex_slot_count : 0,
                 obs_module_text("Capture.Label.Generation"),
                 ctx->tex_header_seq,
                 obs_module_text("Capture.Label.Producer"),
                 ctx->cur_session_seq,
                 obs_module_text("Capture.Label.Adapter"),
                 ctx->adapter_ok ? obs_module_text("Capture.Adapter.Same")
                                 : obs_module_text("Capture.Adapter.Unknown"));
    } else {
        snprintf(status, sizeof(status), "%s: %s\n%s",
                 obs_module_text("Capture.Label.State"),
                 obs_module_text("Capture.State.Waiting"),
                 obs_module_text("Capture.Status.Hint"));
    }
    obs_properties_add_text(ctl, "status", status, OBS_TEXT_INFO);

    obs_properties_add_group(props, "ctl", obs_module_text("Capture.Group.Status"),
                             OBS_GROUP_NORMAL, ctl);

    return props;
}

static void *
capture_create(obs_data_t *settings, obs_source_t *source) {
    struct rscrcpy_capture *ctx = bzalloc(sizeof(*ctx));
    ctx->source = source;
    ctx->draw_slot = -1;
    ctx->adapter_ok = true;
    ctx->state = CAPTURE_WAITING;
    apply_settings(ctx, settings);
    ctx->width = ctx->default_w;
    ctx->height = ctx->default_h;
    ctx->last_log_ms = GetTickCount64();
    ctx->last_frame_ms = ctx->last_log_ms;
    ctx->last_connect_ms = 0;

    blog(LOG_INFO, CAPTURE_LOG_PREFIX
         "source \"%s\" created (video section %s%s)",
         obs_source_get_name(source), SC_OBS_VIDEO_NAME_PREFIX, ctx->shm_name);
    if (ctx->audio_enabled) {
        capture_audio_start(ctx);
    }
    return ctx;
}

static void
capture_destroy(void *data) {
    struct rscrcpy_capture *ctx = data;
    if (!ctx) {
        return;
    }

    // The audio thread may still be using ctx->source (and the section): stop it
    // first, before the video teardown below
    capture_audio_stop(ctx);

    // Textures belong to the graphics context: free them under the lock, and
    // only if one still exists (on shutdown this can run after the device died)
    if (ctx->tex_open || ctx->tex[0]) {
        obs_enter_graphics();
        destroy_textures_locked(ctx);
        obs_leave_graphics();
    }
    sc_obs_video_reader_disconnect(&ctx->reader);

    blog(LOG_INFO, CAPTURE_LOG_PREFIX "source destroyed");
    bfree(ctx);
}

static void
capture_update(void *data, obs_data_t *settings) {
    struct rscrcpy_capture *ctx = data;

    char previous[sizeof(ctx->shm_name)];
    strncpy(previous, ctx->shm_name, sizeof(previous));
    previous[sizeof(previous) - 1] = '\0';
    uint32_t prev_mark = ctx->offline_mark_s;
    uint32_t prev_release = ctx->offline_release_s;
    char previous_audio[sizeof(ctx->audio_shm_name)];
    strncpy(previous_audio, ctx->audio_shm_name, sizeof(previous_audio));
    previous_audio[sizeof(previous_audio) - 1] = '\0';
    bool prev_audio_enabled = ctx->audio_enabled;
    int prev_offset = ctx->offset_ms;

    apply_settings(ctx, settings);

    // The audio reader only needs a restart when it is enabled and its section
    // changed, or when it was just turned on/off; offset_ms is read live by the
    // thread, so changing it is free
    bool audio_restart = prev_audio_enabled != ctx->audio_enabled
        || (ctx->audio_enabled
            && strcmp(previous_audio, ctx->audio_shm_name) != 0);
    if (audio_restart) {
        capture_audio_stop(ctx);
        if (ctx->audio_enabled) {
            capture_audio_start(ctx);
        } else {
            blog(LOG_INFO, CAPTURE_LOG_PREFIX "audio disabled");
        }
    } else if (prev_offset != ctx->offset_ms) {
        blog(LOG_INFO, CAPTURE_LOG_PREFIX "audio offset: %d ms -> %d ms",
             prev_offset, ctx->offset_ms);
    }

    if (strcmp(previous, ctx->shm_name) != 0) {
        blog(LOG_INFO, CAPTURE_LOG_PREFIX
             "shared memory name changed: \"%s\" -> \"%s\"",
             previous, ctx->shm_name);
        release_textures(ctx);
        sc_obs_video_reader_disconnect(&ctx->reader);
        ctx->cur_session_seq = 0;
        ctx->last_frame_seq = 0;
        ctx->last_log_frames = 0;
        ctx->released = false;
        ctx->offline_marked = false;
        ctx->gone_logged = false;
        ctx->logged_generation = 0;
        ctx->last_frame_ms = GetTickCount64();
        ctx->last_connect_ms = 0;
        set_state(ctx, CAPTURE_WAITING);
    }

    if (!ctx->have_real_geometry) {
        ctx->width = ctx->default_w;
        ctx->height = ctx->default_h;
    }

    if (prev_mark != ctx->offline_mark_s || prev_release != ctx->offline_release_s) {
        blog(LOG_INFO, CAPTURE_LOG_PREFIX
             "offline timings: mark %u s, release %u s", ctx->offline_mark_s,
             ctx->offline_release_s);
    }
}

static uint32_t
capture_get_width(void *data) {
    struct rscrcpy_capture *ctx = data;
    return ctx ? ctx->width : 1920;
}

static uint32_t
capture_get_height(void *data) {
    struct rscrcpy_capture *ctx = data;
    return ctx ? ctx->height : 1080;
}

static struct obs_source_info rscrcpy_capture_info = {
    .id = "rscrcpy_capture",
    .type = OBS_SOURCE_TYPE_INPUT,
    // Video and audio in one source. Video is synchronous (OBS calls
    // video_render every frame, so the texture open/close stays ours); audio is
    // pushed by our own reader thread.
    .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO
                  | OBS_SOURCE_DO_NOT_DUPLICATE,
    .get_name = capture_get_name,
    .create = capture_create,
    .destroy = capture_destroy,
    .update = capture_update,
    .get_defaults = capture_get_defaults,
    .get_properties = capture_properties,
    .get_width = capture_get_width,
    .get_height = capture_get_height,
    .video_tick = capture_tick,
    .video_render = capture_render,
};

void
rscrcpy_capture_register(void) {
    obs_register_source(&rscrcpy_capture_info);
}
