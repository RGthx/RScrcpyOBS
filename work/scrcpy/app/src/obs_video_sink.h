#ifndef SC_OBS_VIDEO_SINK_H
#define SC_OBS_VIDEO_SINK_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>
#include <libavcodec/avcodec.h>

#include "frame_buffer.h"
#include "obs_video_proto.h"
#include "trait/frame_sink.h"
#include "util/tick.h"
#include "util/thread.h"

struct sc_obs_bridge;
struct sc_obs_video_gpu;
struct SwsContext;

/**
 * OBS video sink (producer side).
 *
 * Publishes decoded video to the OBS plugin directly:
 * decoded D3D11 (NV12) frames are converted to BGRA by an ID3D11VideoProcessor
 * *directly into* a rotating set of D3D11_RESOURCE_MISC_SHARED textures whose
 * legacy shared handles are published in a named shared memory section (see
 * obs_video_proto.h). Software-decoded frames take the same shared textures,
 * uploaded from system memory.
 *
 * The conversion and the GPU work run on a dedicated
 * worker thread fed by a single-slot "latest frame" buffer: doing this on the
 * decoder thread would throttle decoding and make the latency grow without
 * bound (the decoder queue is unbounded).
 */
struct sc_obs_video_sink {
    struct sc_frame_sink frame_sink; // frame sink trait

    // Borrowed ID3D11Device of the renderer (shared with the D3D11VA decoder),
    // or NULL when there is no renderer (--no-window). Keeping the renderer
    // device means the decoded textures are on the device we convert with, so
    // no cross-device copy is involved.
    void *d3d11_device;

    // Optional OBS audio bridge: keeps the video clock it carries up to date
    // (A/V calibration support, --obs-audio)
    struct sc_obs_bridge *obs_bridge;

    // Shared memory name suffix (--obs-video-name)
    const char *name;

    // Section and its header (created once for the whole run, like the audio
    // bridge, so the plugin keeps a valid handle across reconnects)
    void *mapping; // HANDLE
    uint8_t *base; // mapped view
    struct sc_obs_video_header *hdr;

    // Worker thread and its single-slot "latest frame" queue
    sc_mutex mutex;
    sc_cond cond;
    sc_thread thread;
    struct sc_frame_buffer fb;
    bool stopped;
    uint64_t dropped; // frames overwritten before being sent

    // Timestamps of the frame currently queued, so that the worker publishes
    // the *arrival* host time of the frame it actually converts (protected by
    // mutex, written together with the frame)
    sc_tick pending_qpc;
    int64_t pending_pts;

    // Worker-thread-only state
    struct sc_obs_video_gpu *gpu; // created lazily on the first frame
    struct SwsContext *sws; // YUV420P -> BGRA converter (software path)
    uint8_t *bgra; // conversion output buffer
    unsigned conv_w; // converter input size
    unsigned conv_h;
    bool geometry_ready; // whether handles have been published
    bool warned; // whether a repeated error has already been logged

    // state
    uint64_t frame_count; // frames received in push()
    uint64_t sent_count; // frames actually converted by the worker
    bool frame_shape_logged;

    // fps tracking (sampled once per second)
    sc_tick fps_last_tick;
    uint64_t fps_last_count;

    // conversion fps tracking (worker thread, sampled once per second)
    sc_tick send_fps_last_tick;
    uint64_t send_fps_last_count;

    // worker timing stats, reset every second (worker thread only)
    sc_tick stat_send_us; // cumulative time spent converting one frame
    sc_tick stat_send_max_us; // max single conversion time
    sc_tick stat_wait_us; // cumulative queue wait time

    // latency tracking: wall clock elapsed vs stream pts elapsed
    bool latency_init;
    sc_tick latency_start_tick;
    int64_t latency_start_pts;
};

// d3d11_device is the renderer's ID3D11Device* (as void*, may be NULL).
// obs_bridge is optional (NULL when --obs-audio is off).
// name is the shared memory name suffix (--obs-video-name).
bool
sc_obs_video_sink_init(struct sc_obs_video_sink *ovs, void *d3d11_device,
                       struct sc_obs_bridge *obs_bridge, const char *name);

void
sc_obs_video_sink_destroy(struct sc_obs_video_sink *ovs);

#endif
