#include "obs_video_sink.h"

#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#include "obs_bridge.h"
#include "obs_video_gpu.h"
#include "util/log.h"

#ifdef _WIN32
# include <windows.h>
# include <libavutil/hwcontext_d3d11va.h>
#else
# error "The OBS video bridge is Windows-only"
#endif

/** Downcast frame_sink to sc_obs_video_sink */
#define DOWNCAST(SINK) container_of(SINK, struct sc_obs_video_sink, frame_sink)

// The shared memory name (visible in the plugin) comes from --obs-video-name,
// see sc_obs_video_sink_init().

// Log the first N frames in detail, then one summary line every N-th frame
#define OBS_VIDEO_LOG_FIRST_FRAMES 3
#define OBS_VIDEO_LOG_PERIOD 300

static bool
build_map_name(const char *suffix, char *map_name, size_t len) {
    const char *s = suffix && *suffix ? suffix : "0";

    int r = snprintf(map_name, len, "%s%s", SC_OBS_VIDEO_NAME_PREFIX, s);
    if (r < 0 || (size_t) r >= len) {
        LOGE("OBS video: invalid shared memory name: %s", s);
        return false;
    }
    return true;
}

/*
 * Producer exclusivity (F04), same protocol as the audio bridge: a named
 * mutex held for the whole run. WAIT_TIMEOUT means a live producer is
 * publishing under this name and we must refuse (sc_obs_video_sink_init
 * failing aborts the scrcpy run, which is the intended behaviour for a
 * duplicate --obs-video-name); WAIT_ABANDONED_0 is a dead previous owner,
 * i.e. the legitimate reconnect case, and we re-initialize the header below.
 */
static bool
acquire_producer_mutex(const char *mtx_name, HANDLE *out_mutex) {
    HANDLE mutex = CreateMutexA(NULL, FALSE, mtx_name);
    if (!mutex) {
        LOGE("OBS video: could not create producer mutex %s (error %lu)",
             mtx_name, GetLastError());
        return false;
    }

    DWORD r = WaitForSingleObject(mutex, 0);
    if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED_0) {
        if (r == WAIT_ABANDONED_0) {
            LOGW("OBS video: previous producer of %s died without cleanup, "
                 "taking over the section", mtx_name);
        }
        *out_mutex = mutex;
        return true;
    }

    if (r == WAIT_TIMEOUT) {
        LOGE("OBS video: another producer is already publishing to this "
             "section (%s): refusing to take over a live producer. Use a "
             "different --obs-video-name or stop the other scrcpy first.",
             mtx_name);
    } else {
        LOGE("OBS video: could not acquire producer mutex %s (wait returned "
             "%lu, error %lu)", mtx_name, r, GetLastError());
    }
    CloseHandle(mutex);
    return false;
}

static void
release_producer_mutex(HANDLE mutex) {
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
}

static void
log_frame_shape(const AVFrame *frame) {
    const char *fmt_name = av_get_pix_fmt_name(frame->format);
    const char *space_name = av_color_space_name(frame->colorspace);
    const char *range_name = av_color_range_name(frame->color_range);

    // For D3D11VA frames, data[0] is a ID3D11Resource* (texture array) and
    // data[1] is the slice index; for software frames, data[0] is the first
    // plane pointer and linesize[0] the row stride.
    // Note: no %zu -- the MinGW runtime here does not support it, so cast the
    // crop offsets explicitly.
    LOGI("OBS video sink frame shape: format=%d (%s) size=%dx%d "
         "crop(l,t)=%" PRIu64 ",%" PRIu64 " data[0]=%p data[1]=%p "
         "linesize[0]=%d hw_frames_ctx=%s colorspace=%s range=%s",
         frame->format, fmt_name ? fmt_name : "?",
         frame->width, frame->height,
         (uint64_t) frame->crop_left, (uint64_t) frame->crop_top,
         (void *) frame->data[0], (void *) frame->data[1],
         frame->linesize[0],
         frame->hw_frames_ctx ? "yes" : "no",
         space_name ? space_name : "?", range_name ? range_name : "?");
}

// Publish a new geometry generation: the size and the shared handles, then the
// generation token. The handles are only written here (never per frame).
static void
publish_geometry(struct sc_obs_video_sink *ovs, uint32_t width, uint32_t height,
                 const uint32_t handles[SC_OBS_VIDEO_SLOT_COUNT]) {
    struct sc_obs_video_header *hdr = ovs->hdr;

    if (!ovs->geometry_ready) {
        // Adapter identity, so the consumer can refuse the GPU path instead of
        // silently drawing nothing when OBS renders on another adapter. Written
        // before the generation token below, which is what makes it readable.
        uint32_t low;
        int32_t high;
        if (sc_obs_video_gpu_adapter_luid(ovs->gpu, &low, &high)) {
            hdr->adapter_luid_low = low;
            hdr->adapter_luid_high = high;
        }
    }

    hdr->width = width;
    hdr->height = height;
    hdr->row_pitch = width * 4;
    for (unsigned i = 0; i < SC_OBS_VIDEO_SLOT_COUNT; ++i) {
        hdr->handles[i] = handles[i];
    }
    hdr->format = SC_OBS_VIDEO_FORMAT_BGRA8;

    // Publish everything above before the generation token: the consumer
    // re-reads the geometry whenever video_header_seq changes.
    atomic_thread_fence(memory_order_release);
    hdr->video_header_seq++;

    if (!ovs->geometry_ready) {
        char adapter[256] = {0};
        if (!sc_obs_video_gpu_adapter_name(ovs->gpu, adapter,
                                           sizeof(adapter))) {
            adapter[0] = '\0';
        }
        LOGI("OBS video: published geometry %" PRIu32 "x%" PRIu32
             " (handles 0x%08" PRIx32 " 0x%08" PRIx32 " 0x%08" PRIx32
             ", adapter \"%s\")",
             width, height, handles[0], handles[1], handles[2], adapter);
    } else {
        LOGI("OBS video: geometry changed, republished %" PRIu32 "x%" PRIu32
             " (handles 0x%08" PRIx32 " 0x%08" PRIx32 " 0x%08" PRIx32 ")",
             width, height, handles[0], handles[1], handles[2]);
    }
    ovs->geometry_ready = true;
}

// Publish the frame that was just written into `slot`. Called after the GPU
// work has been submitted (sc_obs_video_gpu_flush()).
static void
publish_frame(struct sc_obs_video_sink *ovs, uint32_t slot, int64_t pts_us,
              sc_tick qpc_us) {
    struct sc_obs_video_header *hdr = ovs->hdr;

    hdr->video_pts_us = pts_us;
    hdr->video_qpc_us = (int64_t) qpc_us;

    // The consumer must not see the frame counters advance before the
    // timestamps above. There is no per-frame synchronisation primitive: the
    // 3-slot rotation (a slot is only rewritten after two further frames) is
    // what keeps this safe, see obs_video_proto.h.
    atomic_thread_fence(memory_order_release);
    hdr->video_slot = slot;
    atomic_thread_fence(memory_order_release);
    hdr->frame_seq++;
}

// Create (or reuse) the YUV420P -> BGRA converter for the given frame size.
// Only called from the worker thread.
static bool
obs_video_sink_prepare_sws(struct sc_obs_video_sink *ovs, const AVFrame *frame) {
    unsigned width = frame->width;
    unsigned height = frame->height;

    if (ovs->sws && ovs->conv_w == width && ovs->conv_h == height) {
        return true;
    }

    uint8_t *bgra = realloc(ovs->bgra, (size_t) width * height * 4);
    if (!bgra) {
        LOG_OOM();
        return false;
    }
    ovs->bgra = bgra;

    ovs->sws = sws_getCachedContext(ovs->sws, width, height, AV_PIX_FMT_YUV420P,
                                    width, height, AV_PIX_FMT_BGRA, SWS_BILINEAR,
                                    NULL, NULL, NULL);
    if (!ovs->sws) {
        LOGE("OBS video: could not create swscale context");
        return false;
    }

    int space;
    switch (frame->colorspace) {
        case AVCOL_SPC_BT709:
            space = SWS_CS_ITU709;
            break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            space = SWS_CS_BT2020;
            break;
        default:
            // BT470BG / SMPTE170M (and unspecified) map to BT.601
            space = SWS_CS_ITU601;
            break;
    }

    // YUV source may be limited range; the BGRA destination is full range RGB.
    const int *table = sws_getCoefficients(space);
    int src_range = frame->color_range == AVCOL_RANGE_JPEG; // 1 = full
    sws_setColorspaceDetails(ovs->sws, table, src_range, table, 1,
                             0, 1 << 16, 1 << 16);

    ovs->conv_w = width;
    ovs->conv_h = height;

    return true;
}

static void
warn_once(struct sc_obs_video_sink *ovs, const char *msg) {
    if (!ovs->warned) {
        LOGE("OBS video: %s", msg);
        ovs->warned = true;
    }
}

// Create the GPU pipeline on the first frame, choosing the device the frames
// actually come from.
static bool
ensure_gpu(struct sc_obs_video_sink *ovs, const AVFrame *frame) {
    if (ovs->gpu) {
        return true;
    }

    void *device = ovs->d3d11_device;
    if (!device && frame->format == AV_PIX_FMT_D3D11 && frame->hw_frames_ctx) {
        // No renderer device (--no-window), but the frames are hardware
        // decoded: take the decoder's own device, so the decoded NV12 textures
        // belong to the device we convert with (no cross-device copy).
        const AVHWFramesContext *frames =
            (const AVHWFramesContext *) frame->hw_frames_ctx->data;
        if (frames->device_ref) {
            const AVHWDeviceContext *dev_ctx =
                (const AVHWDeviceContext *) frames->device_ref->data;
            const AVD3D11VADeviceContext *d3d11 = dev_ctx->hwctx;
            if (d3d11 && d3d11->device) {
                device = d3d11->device;
                LOGI("OBS video: using the decoder's D3D11 device");
            }
        }
    }

    if (!sc_obs_video_gpu_create(&ovs->gpu, device)) {
        warn_once(ovs, "could not create the D3D11 pipeline");
        return false;
    }

    return true;
}

// Perform the actual conversion. Only called from the worker thread.
static void
obs_video_sink_send_frame(struct sc_obs_video_sink *ovs, const AVFrame *frame,
                          int64_t pts_us, sc_tick qpc_us) {
    if (!ensure_gpu(ovs, frame)) {
        return;
    }

    uint32_t handles[SC_OBS_VIDEO_SLOT_COUNT];
    uint32_t slot = ovs->hdr->frame_seq % SC_OBS_VIDEO_SLOT_COUNT;

    if (frame->format == AV_PIX_FMT_D3D11) {
        // GPU path: hardware-decoded NV12 frames are converted to BGRA by the
        // D3D11 video processor, writing directly into the shared texture.
        if (!frame->hw_frames_ctx) {
            warn_once(ovs, "D3D11 frame without hw_frames_ctx, skipping");
            return;
        }

        const AVHWFramesContext *frames =
            (const AVHWFramesContext *) frame->hw_frames_ctx->data;

        void *resource = frame->data[0];
        uint32_t slice = (uint32_t) (uintptr_t) frame->data[1];
        uint32_t surf_w = frames->width;
        uint32_t surf_h = frames->height;
        uint32_t crop_left = (uint32_t) frame->crop_left;
        uint32_t crop_top = (uint32_t) frame->crop_top;
        uint32_t w = frame->width;
        uint32_t h = frame->height;

        if (!resource || !w || !h
                || crop_left + w > surf_w || crop_top + h > surf_h) {
            warn_once(ovs, "inconsistent D3D11 frame geometry, skipping");
            return;
        }

        if (sc_obs_video_gpu_ensure(ovs->gpu, surf_w, surf_h, w, h, handles)) {
            publish_geometry(ovs, w, h, handles);
        }

        int ycbcr_bt709 = frame->colorspace == AVCOL_SPC_BT709;
        int full_range = frame->color_range == AVCOL_RANGE_JPEG;

        if (!sc_obs_video_gpu_blt_d3d11(ovs->gpu, resource, slice, crop_left,
                                        crop_top, w, h, slot, ycbcr_bt709,
                                        full_range)) {
            warn_once(ovs, "VideoProcessorBlt failed");
            return;
        }

        sc_obs_video_gpu_flush(ovs->gpu);
        publish_frame(ovs, slot, pts_us, qpc_us);
        return;
    }

    if (frame->format != AV_PIX_FMT_YUV420P) {
        // Only software decoding (YUV420P) is handled on the CPU path
        if (!ovs->warned) {
            const char *fmt_name = av_get_pix_fmt_name(frame->format);
            LOGW("OBS video: unsupported frame format %d (%s), skipping",
                 frame->format, fmt_name ? fmt_name : "?");
            ovs->warned = true;
        }
        return;
    }

    // Software path: convert to BGRA and upload into the shared texture.
    if (!obs_video_sink_prepare_sws(ovs, frame)) {
        return;
    }

    const uint8_t *src[4] = {
        frame->data[0], frame->data[1], frame->data[2], NULL,
    };
    int src_stride[4] = {
        frame->linesize[0], frame->linesize[1], frame->linesize[2], 0,
    };
    uint8_t *dst[4] = { ovs->bgra, NULL, NULL, NULL };
    int dst_stride[4] = { (int) (ovs->conv_w * 4), 0, 0, 0 };

    int converted = sws_scale(ovs->sws, src, src_stride, 0, frame->height,
                              dst, dst_stride);
    if (converted <= 0) {
        warn_once(ovs, "sws_scale() failed");
        return;
    }

    uint32_t w = ovs->conv_w;
    uint32_t h = ovs->conv_h;
    uint32_t pitch = w * 4;

    if (sc_obs_video_gpu_ensure(ovs->gpu, 0, 0, w, h, handles)) {
        publish_geometry(ovs, w, h, handles);
    }

    if (!sc_obs_video_gpu_upload_bgra(ovs->gpu, ovs->bgra, pitch, h, slot)) {
        warn_once(ovs, "could not upload the frame to the shared texture");
        return;
    }

    sc_obs_video_gpu_flush(ovs->gpu);
    publish_frame(ovs, slot, pts_us, qpc_us);
}

// Worker thread: consume the latest frame and publish it to the OBS plugin.
//
// This keeps the (heavy) GPU work off the decoder thread, so decoding is never
// throttled by the sink and the latency does not accumulate.
static int
run_obs_video(void *data) {
    struct sc_obs_video_sink *ovs = data;

    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        LOG_OOM();
        return -1;
    }

    for (;;) {
        sc_tick wait_start = sc_tick_now();

        sc_mutex_lock(&ovs->mutex);
        while (!ovs->stopped && !sc_frame_buffer_has_frame(&ovs->fb)) {
            sc_cond_wait(&ovs->cond, &ovs->mutex);
        }

        if (ovs->stopped) {
            sc_mutex_unlock(&ovs->mutex);
            break;
        }

        assert(sc_frame_buffer_has_frame(&ovs->fb));
        sc_frame_buffer_consume(&ovs->fb, frame);
        int64_t pts_us = ovs->pending_pts;
        sc_tick qpc_us = ovs->pending_qpc;
        sc_mutex_unlock(&ovs->mutex);

        sc_tick send_start = sc_tick_now();
        ovs->stat_wait_us += send_start - wait_start;

        obs_video_sink_send_frame(ovs, frame, pts_us, qpc_us);
        av_frame_unref(frame);

        sc_tick send_end = sc_tick_now();
        sc_tick send_us = send_end - send_start;
        ovs->stat_send_us += send_us;
        if (send_us > ovs->stat_send_max_us) {
            ovs->stat_send_max_us = send_us;
        }

        ++ovs->sent_count;

        // Log the conversion rate plus the drop count once per second: a
        // positive drop count means the worker cannot keep up with the device.
        sc_tick now = sc_tick_now();
        sc_tick elapsed = now - ovs->send_fps_last_tick;
        if (elapsed >= SC_TICK_FROM_SEC(1)) {
            uint64_t frames = ovs->sent_count - ovs->send_fps_last_count;
            double fps = (double) frames * (double) SC_TICK_FREQ
                       / (double) elapsed;
            double avg_send_ms = frames
                               ? (double) ovs->stat_send_us / frames / 1000.0
                               : 0.0;
            double avg_wait_ms = frames
                               ? (double) ovs->stat_wait_us / frames / 1000.0
                               : 0.0;
            LOGI("OBS video sink: %.1f fps (sent=%" PRIu64 " received=%" PRIu64
                 " dropped=%" PRIu64 ") convert=%.2fms max=%.2fms wait=%.2fms "
                 "frame_seq=%" PRIu32,
                 fps, ovs->sent_count, ovs->frame_count, ovs->dropped,
                 avg_send_ms, (double) ovs->stat_send_max_us / 1000.0,
                 avg_wait_ms, ovs->hdr->frame_seq);
            ovs->send_fps_last_tick = now;
            ovs->send_fps_last_count = ovs->sent_count;
            ovs->stat_send_us = 0;
            ovs->stat_send_max_us = 0;
            ovs->stat_wait_us = 0;
        }
    }

    av_frame_free(&frame);
    LOGD("OBS video thread ended");

    return 0;
}

static bool
sc_obs_video_sink_open(struct sc_frame_sink *sink, const AVCodecContext *ctx,
                       const struct sc_stream_session *session) {
    struct sc_obs_video_sink *ovs = DOWNCAST(sink);
    (void) session;

    const char *fmt_name = av_get_pix_fmt_name(ctx->pix_fmt);
    const char *space_name = av_color_space_name(ctx->colorspace);
    const char *range_name = av_color_range_name(ctx->color_range);

    LOGI("OBS video sink open: codec=%s pix_fmt=%d (%s) size=%dx%d "
         "hw_device_ctx=%s hw_frames_ctx=%s colorspace=%s range=%s",
         ctx->codec ? ctx->codec->name : "?",
         ctx->pix_fmt, fmt_name ? fmt_name : "?",
         ctx->width, ctx->height,
         ctx->hw_device_ctx ? "yes" : "no",
         ctx->hw_frames_ctx ? "yes" : "no",
         space_name ? space_name : "?", range_name ? range_name : "?");

    ovs->frame_count = 0;
    ovs->sent_count = 0;
    ovs->dropped = 0;
    ovs->frame_shape_logged = false;
    ovs->warned = false;
    ovs->fps_last_tick = sc_tick_now();
    ovs->fps_last_count = 0;
    ovs->send_fps_last_tick = sc_tick_now();
    ovs->send_fps_last_count = 0;
    ovs->latency_init = false;
    ovs->pending_pts = 0;
    ovs->pending_qpc = 0;

    sc_mutex_lock(&ovs->mutex);
    ovs->stopped = false;
    sc_mutex_unlock(&ovs->mutex);

    if (!sc_thread_create(&ovs->thread, run_obs_video, "scrcpy-obsvideo",
                          ovs)) {
        LOGE("OBS video: could not start the worker thread");
        sc_mutex_lock(&ovs->mutex);
        ovs->stopped = true;
        sc_mutex_unlock(&ovs->mutex);
        return false;
    }

    return true;
}

static void
sc_obs_video_sink_close(struct sc_frame_sink *sink) {
    struct sc_obs_video_sink *ovs = DOWNCAST(sink);

    sc_mutex_lock(&ovs->mutex);
    ovs->stopped = true;
    sc_cond_signal(&ovs->cond);
    sc_mutex_unlock(&ovs->mutex);

    sc_thread_join(&ovs->thread, NULL);

    LOGI("OBS video sink closed: sent=%" PRIu64 " received=%" PRIu64
         " dropped=%" PRIu64, ovs->sent_count, ovs->frame_count, ovs->dropped);

    if (ovs->gpu) {
        sc_obs_video_gpu_destroy(ovs->gpu);
        ovs->gpu = NULL;
    }

    // No video until the next session publishes a geometry again: a consumer
    // reconnecting between two sessions must not try to open handles that we
    // just released (it would fail and report a spurious error).
    if (ovs->hdr && ovs->geometry_ready) {
        ovs->hdr->format = SC_OBS_VIDEO_FORMAT_NONE;
        atomic_thread_fence(memory_order_release);
        ovs->hdr->video_header_seq++;
        ovs->geometry_ready = false;
    }
}

static enum sc_sink_result
sc_obs_video_sink_push(struct sc_frame_sink *sink, const AVFrame *frame) {
    struct sc_obs_video_sink *ovs = DOWNCAST(sink);

    uint64_t count = ovs->frame_count++;

    sc_tick now = sc_tick_now();

    // End-to-end latency probe: compare the wall-clock elapsed time with the
    // stream pts elapsed time since the first frame. The difference is the
    // accumulated pipeline latency: if it grows, the frames pushed by the
    // decoder are increasingly late (the decoder cannot keep up).
    int64_t pts = frame->pts; // microseconds
    bool has_pts = pts != AV_NOPTS_VALUE;
    if (has_pts && !ovs->latency_init) {
        ovs->latency_init = true;
        ovs->latency_start_tick = now;
        ovs->latency_start_pts = pts;
    }

    if (has_pts) {
        // Keep the video clock carried by the audio bridge up to date (A/V sync
        // calibration by the OBS plugin, --obs-audio)
        sc_obs_bridge_set_video_clock(ovs->obs_bridge, pts, now);
    }

    // Log the instantaneous frame rate once per second to observe fluctuations.
    sc_tick elapsed = now - ovs->fps_last_tick;
    if (elapsed >= SC_TICK_FROM_SEC(1)) {
        uint64_t frames = ovs->frame_count - ovs->fps_last_count;
        double fps = (double) frames * (double) SC_TICK_FREQ / (double) elapsed;
        if (has_pts) {
            double wall_ms = (double) (now - ovs->latency_start_tick) / 1000.0;
            double stream_ms = (double) (pts - ovs->latency_start_pts) / 1000.0;
            LOGI("OBS video sink fps: %.1f (frames=%" PRIu64 ", total=%" PRIu64
                 "), latency=%.0f ms (wall=%.0f stream=%.0f)",
                 fps, frames, ovs->frame_count, wall_ms - stream_ms,
                 wall_ms, stream_ms);
        } else {
            LOGI("OBS video sink fps: %.1f (frames=%" PRIu64 ", total=%" PRIu64
                 ")", fps, frames, ovs->frame_count);
        }
        ovs->fps_last_tick = now;
        ovs->fps_last_count = ovs->frame_count;
    }

    if (!ovs->frame_shape_logged || count < OBS_VIDEO_LOG_FIRST_FRAMES
            || count % OBS_VIDEO_LOG_PERIOD == 0) {
        if (!ovs->frame_shape_logged) {
            // First frame: full shape, once per session (also covers a
            // resolution change, which produces a new session)
            log_frame_shape(frame);
            ovs->frame_shape_logged = true;
        } else {
            LOGI("OBS video sink frame #%" PRIu64 ": format=%d size=%dx%d "
                 "hw_frames_ctx=%s",
                 count, frame->format, frame->width, frame->height,
                 frame->hw_frames_ctx ? "yes" : "no");
        }
    }

    // Lightweight: store the latest frame (dropping any unconsumed previous
    // one) and wake the worker thread. Never blocks the decoder thread.
    sc_mutex_lock(&ovs->mutex);

    if (ovs->stopped) {
        sc_mutex_unlock(&ovs->mutex);
        return SC_SINK_STOPPED;
    }

    if (sc_frame_buffer_has_frame(&ovs->fb)) {
        // A previous frame has not been consumed yet: it will be dropped
        ++ovs->dropped;
    }

    // The worker publishes the arrival time of the frame it actually converts,
    // so these must travel with the frame (same mutex, same critical section).
    ovs->pending_pts = has_pts ? pts : 0;
    ovs->pending_qpc = now;

    bool ok = sc_frame_buffer_push(&ovs->fb, frame);
    if (ok) {
        sc_cond_signal(&ovs->cond);
    }

    sc_mutex_unlock(&ovs->mutex);

    return ok ? SC_SINK_OK : SC_SINK_KO;
}

bool
sc_obs_video_sink_init(struct sc_obs_video_sink *ovs, void *d3d11_device,
                       struct sc_obs_bridge *obs_bridge, const char *name) {
    static const struct sc_frame_sink_ops ops = {
        .open = sc_obs_video_sink_open,
        .close = sc_obs_video_sink_close,
        .push = sc_obs_video_sink_push,
    };

    memset(ovs, 0, sizeof(*ovs));

    ovs->frame_sink.ops = &ops;
    ovs->d3d11_device = d3d11_device;
    ovs->obs_bridge = obs_bridge;
    ovs->name = name;
    ovs->stopped = true;

    char map_name[SC_OBS_VIDEO_NAME_MAX + 64];
    if (!build_map_name(name, map_name, sizeof(map_name))) {
        return false;
    }

    // Single-owner guarantee: take the producer mutex before touching the
    // section (see acquire_producer_mutex above). Held until destroy().
    char mtx_name[SC_OBS_VIDEO_NAME_MAX + 64];
    int mr = snprintf(mtx_name, sizeof(mtx_name), "%s_mtx", map_name);
    if (mr < 0 || (size_t) mr >= sizeof(mtx_name)) {
        LOGE("OBS video: invalid mutex name: %s", map_name);
        return false;
    }
    HANDLE producer_mutex = INVALID_HANDLE_VALUE;
    if (!acquire_producer_mutex(mtx_name, &producer_mutex)) {
        return false;
    }

    uint32_t hdr_size = (uint32_t) sizeof(struct sc_obs_video_header);
    // GPU path only: the section holds the header and nothing else (the pixels
    // live in the shared textures). A named section is committed as soon as it
    // is created, so the CPU fallback arena must not be allocated here.
    size_t total = hdr_size;

    HANDLE mapping =
        CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                           (DWORD) (total >> 32),
                           (DWORD) (total & 0xFFFFFFFFull), map_name);
    if (!mapping) {
        LOGE("OBS video: could not create shared memory %s (error %lu)",
             map_name, GetLastError());
        release_producer_mutex(producer_mutex);
        return false;
    }

    bool existed = GetLastError() == ERROR_ALREADY_EXISTS;

    uint8_t *base = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!base) {
        LOGE("OBS video: could not map shared memory %s (error %lu)",
             map_name, GetLastError());
        CloseHandle(mapping);
        release_producer_mutex(producer_mutex);
        return false;
    }

    // A section left over from another version could be smaller than what we
    // need: verify before touching the header.
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(base, &mbi, sizeof(mbi)) || mbi.RegionSize < total) {
        LOGE("OBS video: existing section %s has an unexpected size", map_name);
        UnmapViewOfFile(base);
        CloseHandle(mapping);
        release_producer_mutex(producer_mutex);
        return false;
    }

    struct sc_obs_video_header *hdr = (struct sc_obs_video_header *) base;
    uint32_t session_seq = existed ? hdr->session_seq : 0;

    memset(hdr, 0, hdr_size);
    hdr->version = SC_OBS_VIDEO_VERSION;
    hdr->header_size = hdr_size;
    hdr->slot_count = SC_OBS_VIDEO_SLOT_COUNT;
    hdr->video_header_seq = 0;
    hdr->format = SC_OBS_VIDEO_FORMAT_NONE;
    hdr->frame_seq = 0;
    hdr->video_slot = 0;
    hdr->session_seq = session_seq + 1;
    hdr->session_start_qpc_us = sc_tick_now();
    hdr->producer_pid = (uint32_t) GetCurrentProcessId();
    // GPU path: no arena. The handles and the adapter identity are published
    // with the first geometry (publish_geometry()).
    hdr->arena_offset = 0;
    hdr->slot_bytes = 0;

    // Publish the validity token last: a consumer only accepts a header whose
    // other fields are already in place (same convention as the audio bridge
    // and the per-slot sequence numbers).
    atomic_thread_fence(memory_order_release);
    hdr->magic = SC_OBS_VIDEO_MAGIC;

    ovs->producer_mutex = producer_mutex;
    ovs->mapping = mapping;
    ovs->base = base;
    ovs->hdr = hdr;

    if (!sc_mutex_init(&ovs->mutex)) {
        hdr->magic = 0;
        UnmapViewOfFile(base);
        CloseHandle(mapping);
        release_producer_mutex(producer_mutex);
        ovs->producer_mutex = NULL;
        ovs->mapping = NULL;
        ovs->base = NULL;
        ovs->hdr = NULL;
        return false;
    }

    if (!sc_cond_init(&ovs->cond)) {
        sc_mutex_destroy(&ovs->mutex);
        hdr->magic = 0;
        UnmapViewOfFile(base);
        CloseHandle(mapping);
        release_producer_mutex(producer_mutex);
        ovs->producer_mutex = NULL;
        ovs->mapping = NULL;
        ovs->base = NULL;
        ovs->hdr = NULL;
        return false;
    }

    if (!sc_frame_buffer_init(&ovs->fb)) {
        sc_cond_destroy(&ovs->cond);
        sc_mutex_destroy(&ovs->mutex);
        hdr->magic = 0;
        UnmapViewOfFile(base);
        CloseHandle(mapping);
        release_producer_mutex(producer_mutex);
        ovs->producer_mutex = NULL;
        ovs->mapping = NULL;
        ovs->base = NULL;
        ovs->hdr = NULL;
        return false;
    }

    LOGI("OBS video: created %s (%u bytes, %u shared texture slots)",
         map_name, (unsigned) total, SC_OBS_VIDEO_SLOT_COUNT);
    return true;
}

void
sc_obs_video_sink_destroy(struct sc_obs_video_sink *ovs) {
    if (ovs->gpu) {
        sc_obs_video_gpu_destroy(ovs->gpu);
        ovs->gpu = NULL;
    }
    if (ovs->sws) {
        sws_freeContext(ovs->sws);
        ovs->sws = NULL;
    }
    free(ovs->bgra);
    ovs->bgra = NULL;

    if (ovs->hdr) {
        // Let the consumer notice that the producer is gone
        ovs->hdr->magic = 0;
        atomic_thread_fence(memory_order_release);
    }

    if (ovs->base) {
        UnmapViewOfFile(ovs->base);
        ovs->base = NULL;
    }
    if (ovs->mapping) {
        CloseHandle((HANDLE) ovs->mapping);
        ovs->mapping = NULL;
    }
    ovs->hdr = NULL;

    // Release the producer mutex last: until here we still own the section
    release_producer_mutex((HANDLE) ovs->producer_mutex);
    ovs->producer_mutex = NULL;

    sc_frame_buffer_destroy(&ovs->fb);
    sc_cond_destroy(&ovs->cond);
    sc_mutex_destroy(&ovs->mutex);
}
