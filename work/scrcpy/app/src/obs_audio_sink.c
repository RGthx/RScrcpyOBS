#include "obs_audio_sink.h"

#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include "util/log.h"

/** Downcast frame_sink to sc_obs_audio_sink */
#define DOWNCAST(SINK) container_of(SINK, struct sc_obs_audio_sink, frame_sink)

// Log the first N packets in detail, then one summary line per second
#define SC_OBS_AUDIO_LOG_FIRST_PACKETS 5

static bool
sc_obs_audio_sink_open(struct sc_frame_sink *sink, const AVCodecContext *ctx,
                       const struct sc_stream_session *session) {
    (void) session;
    struct sc_obs_audio_sink *oas = DOWNCAST(sink);

    assert(ctx->ch_layout.nb_channels > 0);
    assert(ctx->sample_rate > 0);

    // The bridge is optional: any failure here must not prevent mirroring
    if (!sc_obs_bridge_is_ready(oas->bridge)) {
        if (!oas->warned) {
            LOGW("OBS audio sink: bridge unavailable, audio is not sent to OBS");
            oas->warned = true;
        }
        return true;
    }

    uint32_t channels = ctx->ch_layout.nb_channels;
    uint32_t sample_rate = ctx->sample_rate;

    if (channels > SC_OBS_SHM_CHANNELS_ALLOC) {
        LOGW("OBS audio sink: %" PRIu32 " channels not supported (max %u)",
             channels, SC_OBS_SHM_CHANNELS_ALLOC);
        return true;
    }

    if (!sc_obs_bridge_configure(oas->bridge, sample_rate, channels)) {
        return true;
    }

    SwrContext *swr = swr_alloc();
    if (!swr) {
        LOG_OOM();
        return true;
    }

    av_opt_set_chlayout(swr, "in_chlayout", &ctx->ch_layout, 0);
    av_opt_set_chlayout(swr, "out_chlayout", &ctx->ch_layout, 0);
    av_opt_set_int(swr, "in_sample_rate", ctx->sample_rate, 0);
    av_opt_set_int(swr, "out_sample_rate", ctx->sample_rate, 0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt", ctx->sample_fmt, 0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

    int ret = swr_init(swr);
    if (ret < 0) {
        LOGE("OBS audio sink: could not initialize resampler: %s",
             av_err2str(ret));
        swr_free(&swr);
        return true;
    }

    size_t frame_size = (size_t) channels * sizeof(float);
    size_t buf_size = (size_t) SC_OBS_SHM_SLOT_FRAMES * frame_size;
    uint8_t *buf = malloc(buf_size);
    if (!buf) {
        LOG_OOM();
        swr_free(&swr);
        return true;
    }

    oas->swr = swr;
    oas->buf = buf;
    oas->buf_size = buf_size;
    oas->sample_rate = sample_rate;
    oas->channels = channels;
    oas->frame_size = frame_size;
    oas->packets = 0;
    oas->frames = 0;
    oas->dropped = 0;
    oas->stat_last_tick = sc_tick_now();
    oas->stat_last_packets = 0;
    oas->stat_last_frames = 0;
    oas->probe_init = false;

    LOGI("OBS audio sink: open (rate=%" PRIu32 ", channels=%" PRIu32
         ", in_fmt=%s)", sample_rate, channels,
         av_get_sample_fmt_name(ctx->sample_fmt));

    return true;
}

static void
sc_obs_audio_sink_close(struct sc_frame_sink *sink) {
    struct sc_obs_audio_sink *oas = DOWNCAST(sink);

    if (oas->swr) {
        swr_free(&oas->swr);
    }

    free(oas->buf);
    oas->buf = NULL;
    oas->buf_size = 0;

    LOGI("OBS audio sink: closed (packets=%" PRIu64 ", frames=%" PRIu64
         ", dropped=%" PRIu64 ")", oas->packets, oas->frames, oas->dropped);
}

static enum sc_sink_result
sc_obs_audio_sink_push(struct sc_frame_sink *sink, const AVFrame *frame) {
    struct sc_obs_audio_sink *oas = DOWNCAST(sink);

    if (!oas->swr) {
        return SC_SINK_OK;
    }

    // Input and output sample rates are the same, so no rescaling is needed;
    // reserve some room for the resampler internal delay.
    int swr_delay = swr_get_delay(oas->swr, oas->sample_rate);
    int dst_nb_samples = swr_delay + frame->nb_samples + 256;

    size_t needed = (size_t) dst_nb_samples * oas->frame_size;
    if (needed > oas->buf_size) {
        uint8_t *buf = realloc(oas->buf, needed);
        if (!buf) {
            LOG_OOM();
            return SC_SINK_OK;
        }
        oas->buf = buf;
        oas->buf_size = needed;
    }

    int ret = swr_convert(oas->swr, &oas->buf, dst_nb_samples,
                          (const uint8_t **) frame->data, frame->nb_samples);
    if (ret < 0) {
        LOGE("OBS audio sink: resampling failed: %s", av_err2str(ret));
        return SC_SINK_OK;
    }

    uint32_t samples = MIN((uint32_t) ret, (uint32_t) dst_nb_samples);
    if (samples > SC_OBS_SHM_SLOT_FRAMES) {
        // Should never happen: a slot holds SC_OBS_SHM_SLOT_FRAMES frames
        samples = SC_OBS_SHM_SLOT_FRAMES;
        oas->dropped++;
    }

    sc_tick now = sc_tick_now();
    bool has_pts = frame->pts != AV_NOPTS_VALUE;
    int64_t pts = has_pts ? frame->pts : 0;

    uint32_t index;
    uint8_t *dst = sc_obs_bridge_begin_write(oas->bridge, &index);
    if (!dst) {
        return SC_SINK_OK;
    }

    size_t bytes = (size_t) samples * oas->frame_size;
    if (bytes) {
        memcpy(dst, oas->buf, bytes);
    }

    sc_obs_bridge_publish(oas->bridge, index, samples, pts, now);

    oas->packets++;
    oas->frames += samples;

    // "latency" probe: host elapsed time minus PTS elapsed time since the
    // first packet. It must stay constant (a growing value means the audio
    // pipeline is falling behind).
    int64_t latency = 0;
    if (has_pts) {
        if (!oas->probe_init) {
            oas->probe_init = true;
            oas->probe_pts = pts;
            oas->probe_qpc = now;
        }
        latency = (now - oas->probe_qpc) - (pts - oas->probe_pts);
    }

    if (oas->packets <= SC_OBS_AUDIO_LOG_FIRST_PACKETS) {
        LOGI("OBS audio sink: packet #%" PRIu64 ": pts=%" PRIi64
             " qpc=%" PRIi64 " frames=%" PRIu32 " latency=%" PRIi64 " ms",
             oas->packets, pts, (int64_t) now, samples, latency / 1000);
    }

    sc_tick elapsed = now - oas->stat_last_tick;
    if (elapsed >= SC_TICK_FROM_SEC(1)) {
        uint64_t packets = oas->packets - oas->stat_last_packets;
        uint64_t frames = oas->frames - oas->stat_last_frames;

        double pps = (double) packets * SC_TICK_FREQ / (double) elapsed;
        double fps = (double) frames * SC_TICK_FREQ / (double) elapsed;

        uint32_t overrun = oas->bridge->hdr->overrun;
        LOGI("OBS audio sink: %.1f packets/s, %.0f frames/s, "
             "latency=%" PRIi64 " ms, dropped=%" PRIu64 ", overrun=%" PRIu32,
             pps, fps, latency / 1000, oas->dropped, overrun);

        oas->stat_last_tick = now;
        oas->stat_last_packets = oas->packets;
        oas->stat_last_frames = oas->frames;
    }

    return SC_SINK_OK;
}

void
sc_obs_audio_sink_init(struct sc_obs_audio_sink *oas,
                       struct sc_obs_bridge *bridge) {
    memset(oas, 0, sizeof(*oas));
    oas->bridge = bridge;

    static const struct sc_frame_sink_ops ops = {
        .open = sc_obs_audio_sink_open,
        .close = sc_obs_audio_sink_close,
        .push = sc_obs_audio_sink_push,
    };

    oas->frame_sink.ops = &ops;
}
