#ifndef SC_OBS_VIDEO_PROTO_H
#define SC_OBS_VIDEO_PROTO_H

/*
 * Shared-memory protocol between scrcpy (producer) and the OBS plugin
 * (consumer) for the new video bridge.
 *
 * This header is the single source of truth: the OBS plugin build includes it
 * directly from this directory (obs-plugin/build.ps1 adds app/src to the
 * include path), so the two sides cannot diverge.
 *
 * Unlike the audio protocol (obs_shm_proto.h), the VIDEO path is mostly a
 * handle-passing protocol: the actual pixels live in D3D11 shared textures
 * created by scrcpy on its renderer device, and OBS opens them with
 * gs_texture_open_shared(handle). This section therefore carries only geometry
 * + a small array of legacy (32-bit) D3D11 shared handles.
 *
 * Why a separate section instead of extending the audio header:
 *   - the CPU fallback path needs a ~55 MB frame arena, which the GPU path
 *     must not commit at all (a named section is committed as soon as it is
 *     created);
 *   - the two have different lifetimes and different failure modes;
 *   - the audio protocol stays untouched.
 *
 * Layout inside the named section (GPU path):
 *
 *     [ struct sc_obs_video_header ]        (header_size bytes)
 *
 * and in the CPU fallback path additionally:
 *
 *     [ header ][ slot 0 BGRA ][ slot 1 BGRA ] ...   (arena_offset/slot_bytes)
 *
 * Conventions shared with the audio protocol:
 *   - all counters are 32-bit and may wrap; only unsigned differences are used;
 *   - the producer is the only writer of the sequence counters; the consumer
 *     never writes anything except nothing at all (read-only);
 *   - `magic` is written LAST, after an atomic release fence, as the validity
 *     token: a consumer that sees a valid magic is guaranteed to see a fully
 *     initialized header (a consumer must never connect to a section whose
 *     format fields are still zero);
 *   - `video_header_seq` is the "geometry generation" token: it changes whenever
 *     the size or the shared textures are rebuilt (device rotation, resolution
 *     change, new producer). The consumer must release every gs_texture_t and
 *     reopen from the new handles when it changes;
 *   - `handles[]` are stable while `video_header_seq` does not change, which is
 *     the whole point of publishing the array instead of a single "current
 *     handle": a 3-slot rotation with a single field would force the consumer to
 *     OpenSharedResource + create/destroy a GPU texture on every frame.
 *
 * No event object is used on the video side: the OBS consumer polls from
 * video_tick(), which already runs once per rendered frame, so there is nothing
 * to wait on (the audio side needed an event because its reader thread must
 * sleep between packets).
 */

#include <stdint.h>

#define SC_OBS_VIDEO_MAGIC 0x53435644u // 'S', 'C', 'V', 'D'
#define SC_OBS_VIDEO_VERSION 1u

// Three shared textures, rotated by the producer. The consumer draws the slot
// named by `video_slot`; that slot is only rewritten after two further frames
// have been published, i.e. there are two frame periods of slack for the GPU
// to finish the draw (~66 ms at 30 fps). Deliberately NOT "consumer lags one
// buffer": that would add a full frame (~33 ms) of video latency, which fights
// the A/V sync goal. If tearing is ever measured, raise this to 4 (costs one
// more texture, adds no latency) rather than lagging.
#define SC_OBS_VIDEO_SLOT_COUNT 3u

// Maximum length of the user-provided shared-memory name suffix (same limit as
// the audio protocol, so one name can drive both).
#define SC_OBS_VIDEO_NAME_MAX 64

// Section name prefix (the suffix comes from --obs-video-name)
#define SC_OBS_VIDEO_NAME_PREFIX "Local\\scrcpy_video_v1_"

// Pixel formats. BGRA8_UNORM is what the D3D11 video processor produces and
// what gs_texture_open_shared can map; anything else is negotiated later.
#define SC_OBS_VIDEO_FORMAT_BGRA8 0u
#define SC_OBS_VIDEO_FORMAT_NONE 0xFFFFFFFFu // not ready yet

// A D3D11 legacy shared handle is a 32-bit value; 0 means "slot not ready".
// OBS additionally treats (uint32_t)-1 as invalid (GS_INVALID_HANDLE), so the
// consumer must reject both.
#define SC_OBS_VIDEO_HANDLE_INVALID 0u
#define SC_OBS_VIDEO_GS_INVALID_HANDLE 0xFFFFFFFFu

struct sc_obs_video_header {
    // Written last (release fence before it): validity token.
    uint32_t magic; // SC_OBS_VIDEO_MAGIC
    uint32_t version; // SC_OBS_VIDEO_VERSION
    uint32_t header_size; // sizeof(struct sc_obs_video_header)
    uint32_t slot_count; // SC_OBS_VIDEO_SLOT_COUNT

    // Geometry generation: +1 whenever size/handles are rebuilt.
    volatile uint32_t video_header_seq;
    volatile uint32_t format; // SC_OBS_VIDEO_FORMAT_*

    // Visible size (crop already applied): what the source reports as its size.
    volatile uint32_t width;
    volatile uint32_t height;
    volatile uint32_t row_pitch; // BGRA row stride in bytes (width * 4 in the GPU path)

    // Legacy D3D11 shared handles, one per slot. Stable while video_header_seq
    // is unchanged. Only meaningful when format != NONE.
    volatile uint32_t handles[SC_OBS_VIDEO_SLOT_COUNT];

    // Frame publication: frame_seq counts published frames (never reset while
    // the producer lives), video_slot is the slot the newest frame went into.
    volatile uint32_t frame_seq;
    volatile uint32_t video_slot;

    // Timestamps of that frame, both on clocks the plugin already uses:
    // video_pts_us is the device (phone) timeline, video_qpc_us is the host
    // QueryPerformanceCounter in microseconds -- exactly the same timebase the
    // audio slots use (sc_tick_now() == os_gettime_ns() / 1000).
    volatile int64_t video_pts_us;
    volatile int64_t video_qpc_us;

    // Session identity: session_seq is bumped once per producer start, so the
    // consumer can tell "new scrcpy instance reusing the same name" from
    // "same producer, no new frames" and reset its state (and clear the black
    // timeout) accordingly.
    volatile uint32_t session_seq;
    uint32_t producer_pid;

    // Adapter identity. Zero-copy handle sharing only works between devices on
    // the SAME adapter (DXGI OpenSharedResource), so the consumer compares this
    // with its own adapter LUID (from gs_get_device_obj()) and, on mismatch,
    // refuses the GPU path and says so instead of silently drawing nothing.
    uint32_t adapter_luid_low;
    int32_t adapter_luid_high;

    // CPU fallback only: slot_bytes == 0 means "use the shared handles"; when
    // non-zero the frames are plain BGRA in this section at arena_offset.
    uint32_t arena_offset;
    uint32_t slot_bytes;

    // Host clock at section creation (diagnostics only).
    int64_t session_start_qpc_us;
};

#endif
