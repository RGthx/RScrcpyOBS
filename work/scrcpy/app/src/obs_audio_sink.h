#ifndef SC_OBS_AUDIO_SINK_H
#define SC_OBS_AUDIO_SINK_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>
#include <libavcodec/avcodec.h>

#include "obs_bridge.h"
#include "trait/frame_sink.h"
#include "util/tick.h"

struct SwrContext;

/**
 * OBS audio sink: forwards decoded audio to the OBS audio bridge.
 *
 * Decoded frames (whatever their sample format) are converted to interleaved
 * float32 - the format expected by the OBS plugin - without changing the
 * sample rate or the channel layout (OBS remixes/resamples as needed), then
 * queued into the shared memory ring buffer together with their PTS (device
 * timeline) and the host QPC timestamp used for playback scheduling.
 *
 * The bridge is owned by the caller (scrcpy) and outlives stream sessions.
 */
struct sc_obs_audio_sink {
    struct sc_frame_sink frame_sink; // frame sink trait

    struct sc_obs_bridge *bridge; // not owned

    // Resampler: decoded format -> packed float32, same rate and layout
    struct SwrContext *swr;
    uint8_t *buf;
    size_t buf_size;
    uint32_t sample_rate;
    uint32_t channels;
    size_t frame_size; // channels * sizeof(float)

    // Stats (logged once per second)
    uint64_t packets;
    uint64_t frames;
    uint64_t dropped;
    sc_tick stat_last_tick;
    uint64_t stat_last_packets;
    uint64_t stat_last_frames;
    bool warned;

    // Latency probe: compares host elapsed time with PTS elapsed time
    bool probe_init;
    int64_t probe_pts;
    int64_t probe_qpc;
};

void
sc_obs_audio_sink_init(struct sc_obs_audio_sink *oas,
                       struct sc_obs_bridge *bridge);

#endif
