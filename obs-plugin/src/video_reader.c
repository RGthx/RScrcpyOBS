#include "video_reader.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <windows.h>

// Name buffer size (the section name is prefix + suffix + NUL)
#define READER_NAME_MAX 128

static bool
build_name(const char *suffix, char *out) {
    const char *s = suffix && *suffix ? suffix : "0";
    int n = snprintf(out, READER_NAME_MAX, "%s%s", SC_OBS_VIDEO_NAME_PREFIX, s);
    return n > 0 && n < READER_NAME_MAX;
}

bool
sc_obs_video_reader_handle_usable(uint32_t handle) {
    return handle != SC_OBS_VIDEO_HANDLE_INVALID
        && handle != SC_OBS_VIDEO_GS_INVALID_HANDLE;
}

void
sc_obs_video_reader_disconnect(struct sc_obs_video_reader *r) {
    if (r->base) {
        UnmapViewOfFile(r->base);
    }
    if (r->mapping) {
        CloseHandle((HANDLE) r->mapping);
    }

    r->base = NULL;
    r->mapping = NULL;
    r->hdr = NULL;
    r->connected = false;
}

bool
sc_obs_video_reader_connect(struct sc_obs_video_reader *r, const char *name) {
    if (r->connected) {
        return true;
    }

    char map_name[READER_NAME_MAX];
    if (!build_name(name, map_name)) {
        return false;
    }

    HANDLE mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, map_name);
    if (!mapping) {
        // No producer (yet): the normal case while scrcpy is not running
        return false;
    }

    uint8_t *base = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!base) {
        CloseHandle(mapping);
        return false;
    }

    struct sc_obs_video_header *hdr = (struct sc_obs_video_header *) base;

    // Refuse anything we do not understand instead of reading garbage. The
    // producer writes `magic` last, so a valid magic also means the geometry
    // and the handles were fully written (same convention as the audio side).
    if (hdr->magic != SC_OBS_VIDEO_MAGIC || hdr->version != SC_OBS_VIDEO_VERSION
            || hdr->header_size < sizeof(struct sc_obs_video_header)
            || hdr->slot_count < 1
            || hdr->slot_count > SC_OBS_VIDEO_SLOT_COUNT) {
        UnmapViewOfFile(base);
        CloseHandle(mapping);
        return false;
    }

    // The section could be smaller than the header claims (a leftover section
    // created by an older build); verify before trusting it.
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(base, &mbi, sizeof(mbi))
            || mbi.RegionSize < sizeof(struct sc_obs_video_header)) {
        UnmapViewOfFile(base);
        CloseHandle(mapping);
        return false;
    }

    strncpy(r->name, name && *name ? name : "0", sizeof(r->name) - 1);
    r->name[sizeof(r->name) - 1] = '\0';
    r->mapping = mapping;
    r->base = base;
    r->hdr = hdr;
    r->session_seq = hdr->session_seq;
    r->connected = true;
    return true;
}

bool
sc_obs_video_reader_is_alive(const struct sc_obs_video_reader *r) {
    return r->connected && r->hdr && r->hdr->magic == SC_OBS_VIDEO_MAGIC;
}

bool
sc_obs_video_reader_snapshot(struct sc_obs_video_reader *r,
                             struct sc_obs_video_snapshot *out) {
    memset(out, 0, sizeof(*out));

    if (!r->connected || !r->hdr) {
        return false;
    }

    struct sc_obs_video_header *hdr = r->hdr;

    // The producer clears the magic when it exits: that is our "producer gone"
    // signal even though the section (and our mapping of it) stays alive.
    if (hdr->magic != SC_OBS_VIDEO_MAGIC) {
        return false;
    }

    // Read everything first, then fence, then re-validate: if a new producer
    // took the section over in the middle of this, session_seq will have moved
    // and the snapshot is thrown away instead of mixing two generations.
    uint32_t video_header_seq = hdr->video_header_seq;
    uint32_t format = hdr->format;
    uint32_t width = hdr->width;
    uint32_t height = hdr->height;
    uint32_t row_pitch = hdr->row_pitch;
    uint32_t slot_count = hdr->slot_count;
    uint32_t handles[SC_OBS_VIDEO_SLOT_COUNT];
    for (uint32_t i = 0; i < SC_OBS_VIDEO_SLOT_COUNT; i++) {
        handles[i] = hdr->handles[i];
    }
    uint32_t frame_seq = hdr->frame_seq;
    uint32_t video_slot = hdr->video_slot;
    int64_t video_pts_us = hdr->video_pts_us;
    int64_t video_qpc_us = hdr->video_qpc_us;
    uint32_t session_seq = hdr->session_seq;
    uint32_t producer_pid = hdr->producer_pid;
    uint32_t luid_low = hdr->adapter_luid_low;
    int32_t luid_high = hdr->adapter_luid_high;
    uint32_t arena_offset = hdr->arena_offset;
    uint32_t slot_bytes = hdr->slot_bytes;

    // Pairs with the producer's release fence before it publishes frame_seq.
    atomic_thread_fence(memory_order_acquire);

    if (hdr->magic != SC_OBS_VIDEO_MAGIC || hdr->session_seq != session_seq) {
        return false;
    }

    out->valid = true;
    out->video_header_seq = video_header_seq;
    out->format = format;
    out->width = width;
    out->height = height;
    out->row_pitch = row_pitch;
    out->slot_count = slot_count;
    memcpy(out->handles, handles, sizeof(handles));
    out->frame_seq = frame_seq;
    out->video_slot = video_slot;
    out->video_pts_us = video_pts_us;
    out->video_qpc_us = video_qpc_us;
    out->session_seq = session_seq;
    out->producer_pid = producer_pid;
    out->adapter_luid_low = luid_low;
    out->adapter_luid_high = luid_high;
    out->arena_offset = arena_offset;
    out->slot_bytes = slot_bytes;

    r->session_seq = session_seq;
    r->snapshots++;
    return true;
}
