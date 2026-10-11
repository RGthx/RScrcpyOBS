#ifndef RSCRCPY_VIDEO_READER_H
#define RSCRCPY_VIDEO_READER_H

/*
 * Consumer side of the scrcpy <-> OBS video bridge.
 *
 * Where the audio reader (shm_reader.h) consumes PCM out of a ring, this one
 * only consumes *geometry and handles*: the pixels live in D3D11 shared
 * textures created by scrcpy and are opened with gs_texture_open_shared().
 *
 * Deliberately free of any libobs dependency (like shm_reader.c) so that it can
 * be exercised on its own. The protocol is defined in obs_video_proto.h, which
 * is included straight from the scrcpy source tree (see obs-plugin/build.ps1),
 * so the two sides cannot diverge.
 *
 * Read-only contract: the consumer never writes anything into the section
 * (unlike the audio ring, whose producer asks for a read_seq hint). Everything
 * here is load + acquire fence + re-validate.
 */

#include <stdbool.h>
#include <stdint.h>

#include "obs_video_proto.h"

/*
 * One coherent look at the producer's header.
 *
 * `valid` is false when there is no producer right now (section still mapped
 * but magic cleared) or when the header changed under us while reading it (a
 * new producer took the section over). Callers must treat an invalid snapshot
 * as "nothing to show", never as "keep the previous values".
 */
struct sc_obs_video_snapshot {
    bool valid;

    uint32_t video_header_seq; // geometry/handle generation
    uint32_t format; // SC_OBS_VIDEO_FORMAT_*
    uint32_t width;
    uint32_t height;
    uint32_t row_pitch;
    uint32_t slot_count;
    uint32_t handles[SC_OBS_VIDEO_SLOT_COUNT];

    uint32_t frame_seq; // published frames (0 = nothing published yet)
    uint32_t video_slot; // slot holding the newest frame

    int64_t video_pts_us; // device (phone) timeline
    int64_t video_qpc_us; // host QPC, microseconds (same base as audio)

    uint32_t session_seq; // +1 per producer start
    uint32_t producer_pid;

    uint32_t adapter_luid_low;
    int32_t adapter_luid_high;

    uint32_t arena_offset; // CPU fallback only
    uint32_t slot_bytes; // 0 => the GPU handle path
};

struct sc_obs_video_reader {
    char name[SC_OBS_VIDEO_NAME_MAX + 1];

    void *mapping; // HANDLE of the section
    uint8_t *base; // mapped view
    struct sc_obs_video_header *hdr;

    bool connected;
    uint32_t session_seq; // session observed when the last snapshot was taken

    uint64_t snapshots; // diagnostics
};

// Attach to "<prefix><name>". Returns false when there is no such section
// (producer not running yet), which is not an error: just retry later.
bool
sc_obs_video_reader_connect(struct sc_obs_video_reader *r, const char *name);

void
sc_obs_video_reader_disconnect(struct sc_obs_video_reader *r);

// Whether the section is still published by a producer (magic intact), i.e.
// whether the producer process is still alive / has not torn the section down.
bool
sc_obs_video_reader_is_alive(const struct sc_obs_video_reader *r);

// Take a coherent snapshot. Returns false (and sets out->valid = false) when
// there is nothing usable right now.
bool
sc_obs_video_reader_snapshot(struct sc_obs_video_reader *r,
                             struct sc_obs_video_snapshot *out);

// A handle is usable only when it is neither "slot not ready" (0) nor OBS's own
// invalid sentinel (0xFFFFFFFF).
bool
sc_obs_video_reader_handle_usable(uint32_t handle);

#endif
