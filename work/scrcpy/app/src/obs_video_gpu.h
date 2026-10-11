#ifndef SC_OBS_VIDEO_GPU_H
#define SC_OBS_VIDEO_GPU_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>

#include "obs_video_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * D3D11 side of the OBS video bridge (Windows only).
 *
 * Owns the conversion pipeline (NV12 -> BGRA) and the rotating set of
 * D3D11_RESOURCE_MISC_SHARED textures whose legacy handles are published to the
 * OBS plugin. The BGRA target textures are shareable and their legacy handles
 * are published directly, so OBS opens the very same GPU texture instead of a
 * copy.
 *
 * All functions must be called from the same thread (the sink worker thread).
 */
struct sc_obs_video_gpu;

/**
 * Create the pipeline wrapper.
 *
 * d3d11_device is the renderer/decoder ID3D11Device (borrowed), or NULL to let
 * this module create its own device on the preferred adapter (which is what
 * happens with --no-window, where there is no renderer and the frames are
 * software-decoded: they are then uploaded from system memory).
 */
bool
sc_obs_video_gpu_create(struct sc_obs_video_gpu **out, void *d3d11_device);

/**
 * Create a D3D11 device on the preferred adapter, exactly as this module would on
 * its own, and hand it to the caller (who owns that reference).
 *
 * Why it exists: with --no-window scrcpy creates no renderer, so there would be
 * no D3D11 device at all and the decoder would fall back to SOFTWARE decoding,
 * which defeats the shared-texture path entirely.
 *
 * So scrcpy.c creates the device up front and passes it to both the decoder
 * (D3D11VA decoding into D3D11 textures) and the sink (which borrows it). That is
 * what makes the conversion zero-copy: decoded textures and the shared textures
 * end up on the same device.
 *
 * Returns NULL on failure; the caller then keeps software decoding.
 */
void *
sc_obs_video_gpu_create_device(void);

/** Release a device obtained from sc_obs_video_gpu_create_device(). */
void
sc_obs_video_gpu_release_device(void *d3d11_device);

void
sc_obs_video_gpu_destroy(struct sc_obs_video_gpu *gpu);

// Adapter identity of the device actually used, for the protocol header.
bool
sc_obs_video_gpu_adapter_luid(struct sc_obs_video_gpu *gpu, uint32_t *low,
                              int32_t *high);

// Human-readable adapter name (for logs); returns false if unknown.
bool
sc_obs_video_gpu_adapter_name(struct sc_obs_video_gpu *gpu, char *buf,
                              size_t len);

/**
 * Ensure the shared textures match the given geometry.
 *
 * surface_w/surface_h is the decoded NV12 surface (0 for the software path,
 * where no conversion pipeline is needed); width/height is the visible size
 * that OBS will draw.
 *
 * Returns true if the textures were (re)built -- in which case `handles` is
 * filled with the new legacy shared handles and the caller must bump
 * video_header_seq. Returns false when the geometry was already correct, or on
 * failure (in which case handles must not be published).
 */
bool
sc_obs_video_gpu_ensure(struct sc_obs_video_gpu *gpu, uint32_t surface_w,
                        uint32_t surface_h, uint32_t width, uint32_t height,
                        uint32_t handles[SC_OBS_VIDEO_SLOT_COUNT]);

// Convert a decoded D3D11 NV12 frame (texture array + slice) into shared slot.
bool
sc_obs_video_gpu_blt_d3d11(struct sc_obs_video_gpu *gpu, void *resource,
                           uint32_t slice, uint32_t crop_left,
                           uint32_t crop_top, uint32_t width, uint32_t height,
                           uint32_t slot, int ycbcr_bt709, int full_range);

// Upload a CPU-side BGRA buffer into shared slot (software decoding path).
bool
sc_obs_video_gpu_upload_bgra(struct sc_obs_video_gpu *gpu, const uint8_t *bgra,
                             uint32_t pitch, uint32_t height, uint32_t slot);

// Flush the immediate context so the frame is actually submitted before the
// producer fence + frame_seq publication (there is no keyed mutex: legacy
// shared textures do not support it).
void
sc_obs_video_gpu_flush(struct sc_obs_video_gpu *gpu);

#ifdef __cplusplus
}
#endif

#endif
