#include "obs_bridge.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "util/log.h"
#include "util/tick.h"

#ifdef _WIN32
# include <windows.h>
#else
# error "The OBS audio bridge is Windows-only"
#endif

#define SC_OBS_BRIDGE_NAME_MAX 128

// Offsets/geometry helpers -------------------------------------------------

/*
 * Producer exclusivity (F04): the section name is user-chosen, so two scrcpy
 * instances started with the same --obs-audio-name would both create/open the
 * same section and the second one would memset the header the first one is
 * publishing through. A named mutex, held for the whole run, gives the section
 * a single owner:
 *
 *   - CreateMutex + WaitForSingleObject(0): WAIT_OBJECT_0 means nobody else
 *     holds it (fresh, or the previous owner exited cleanly);
 *   - WAIT_ABANDONED_0 means the previous owner died without releasing:
 *     we get ownership, which is exactly the "take over a dead session"
 *     case, and the header is re-initialized below;
 *   - WAIT_TIMEOUT means a live producer holds the section: refuse.
 *
 * Checking only a pid in the header would race (the other instance could
 * take the section between the check and the memset); holding the mutex
 * from before the first byte is touched until destroy removes the race.
 */
static bool
acquire_producer_mutex(const char *mtx_name, HANDLE *out_mutex) {
    HANDLE mutex = CreateMutexA(NULL, FALSE, mtx_name);
    if (!mutex) {
        LOGE("OBS bridge: could not create producer mutex %s (error %lu)",
             mtx_name, GetLastError());
        return false;
    }

    DWORD r = WaitForSingleObject(mutex, 0);
    if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED_0) {
        // Owned (fresh or abandoned: a dead owner's section is re-initialized
        // below, so the consumer cannot mix old-session data into ours)
        if (r == WAIT_ABANDONED_0) {
            LOGW("OBS bridge: previous producer of %s died without cleanup, "
                 "taking over the section", mtx_name);
        }
        *out_mutex = mutex;
        return true;
    }

    if (r == WAIT_TIMEOUT) {
        LOGE("OBS bridge: another producer is already publishing to this "
             "section (%s): refusing to take over a live producer. Use a "
             "different --obs-audio-name or stop the other scrcpy first.",
             mtx_name);
    } else {
        LOGE("OBS bridge: could not acquire producer mutex %s (wait returned "
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

static inline size_t
slot_stride(const struct sc_obs_bridge *bridge) {
    return bridge->slot_stride;
}

static inline struct sc_obs_shm_slot_hdr *
slot_hdr(struct sc_obs_bridge *bridge, uint32_t index) {
    uint8_t *p = bridge->base + bridge->hdr->arena_offset
               + (size_t) index * slot_stride(bridge);
    return (struct sc_obs_shm_slot_hdr *) p;
}

static inline uint8_t *
slot_data(struct sc_obs_bridge *bridge, uint32_t index) {
    return (uint8_t *) slot_hdr(bridge, index)
         + sizeof(struct sc_obs_shm_slot_hdr);
}

// Implementation -----------------------------------------------------------

static bool
build_names(const char *suffix, char *map_name, char *evt_name,
             char *mtx_name) {
    const char *s = suffix && *suffix ? suffix : "0";

    int r = snprintf(map_name, SC_OBS_BRIDGE_NAME_MAX, "%s%s",
                     SC_OBS_SHM_NAME_PREFIX, s);
    if (r < 0 || r >= SC_OBS_BRIDGE_NAME_MAX) {
        LOGE("OBS bridge: invalid shared memory name: %s", s);
        return false;
    }

    r = snprintf(evt_name, SC_OBS_BRIDGE_NAME_MAX, "%s%s_evt",
                 SC_OBS_SHM_NAME_PREFIX, s);
    if (r < 0 || r >= SC_OBS_BRIDGE_NAME_MAX) {
        LOGE("OBS bridge: invalid event name: %s", s);
        return false;
    }

    r = snprintf(mtx_name, SC_OBS_BRIDGE_NAME_MAX, "%s%s_mtx",
                 SC_OBS_SHM_NAME_PREFIX, s);
    if (r < 0 || r >= SC_OBS_BRIDGE_NAME_MAX) {
        LOGE("OBS bridge: invalid mutex name: %s", s);
        return false;
    }

    return true;
}

bool
sc_obs_bridge_init(struct sc_obs_bridge *bridge, const char *name) {
    memset(bridge, 0, sizeof(*bridge));

    char map_name[SC_OBS_BRIDGE_NAME_MAX];
    char evt_name[SC_OBS_BRIDGE_NAME_MAX];
    char mtx_name[SC_OBS_BRIDGE_NAME_MAX];
    if (!build_names(name, map_name, evt_name, mtx_name)) {
        return false;
    }

    // Single-owner guarantee: take the producer mutex before touching the
    // section (see acquire_producer_mutex above). Held until destroy().
    HANDLE mutex = INVALID_HANDLE_VALUE;
    if (!acquire_producer_mutex(mtx_name, &mutex)) {
        return false;
    }

    uint32_t slot_bytes =
        SC_OBS_SHM_CHANNELS_ALLOC * SC_OBS_SHM_SLOT_FRAMES * 4;
    uint32_t hdr_size = (uint32_t) sizeof(struct sc_obs_shm_header);
    // Align the arena (and therefore every PCM block) on 64 bytes
    uint32_t arena_offset = (hdr_size + 63u) & ~63u;
    size_t stride = sizeof(struct sc_obs_shm_slot_hdr) + slot_bytes;
    uint64_t total = (uint64_t) arena_offset
                   + (uint64_t) SC_OBS_SHM_SLOT_COUNT * stride;

    if (total > 0xFFFFFFFFull) {
        LOGE("OBS bridge: section too large");
        return false;
    }

    HANDLE event = CreateEventA(NULL, FALSE, FALSE, evt_name);
    if (!event) {
        LOGE("OBS bridge: could not create event %s (error %lu)",
             evt_name, GetLastError());
        release_producer_mutex(mutex);
        return false;
    }

    HANDLE mapping =
        CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                           (DWORD) (total >> 32),
                           (DWORD) (total & 0xFFFFFFFFull), map_name);
    if (!mapping) {
        LOGE("OBS bridge: could not create shared memory %s (error %lu)",
             map_name, GetLastError());
        CloseHandle(event);
        release_producer_mutex(mutex);
        return false;
    }

    bool existed = GetLastError() == ERROR_ALREADY_EXISTS;

    uint8_t *base = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!base) {
        LOGE("OBS bridge: could not map shared memory %s (error %lu)",
             map_name, GetLastError());
        CloseHandle(mapping);
        CloseHandle(event);
        release_producer_mutex(mutex);
        return false;
    }

    // A section left over from another version could be smaller than what we
    // need: verify before touching the header.
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(base, &mbi, sizeof(mbi)) || mbi.RegionSize < total) {
        LOGE("OBS bridge: existing section %s has an unexpected size", map_name);
        UnmapViewOfFile(base);
        CloseHandle(mapping);
        CloseHandle(event);
        release_producer_mutex(mutex);
        return false;
    }

    struct sc_obs_shm_header *hdr = (struct sc_obs_shm_header *) base;
    uint32_t session_seq = existed ? hdr->session_seq : 0;

    memset(hdr, 0, hdr_size);
    hdr->version = SC_OBS_SHM_VERSION;
    hdr->header_size = hdr_size;
    hdr->slot_hdr_size = (uint32_t) sizeof(struct sc_obs_shm_slot_hdr);
    hdr->arena_offset = arena_offset;
    hdr->slot_bytes = slot_bytes;
    hdr->slot_frames = SC_OBS_SHM_SLOT_FRAMES;
    hdr->slot_count = SC_OBS_SHM_SLOT_COUNT;
    hdr->sample_rate = 0;
    hdr->channels = 0;
    hdr->header_seq = 1;
    hdr->write_seq = 0;
    hdr->read_seq = 0;
    hdr->overrun = 0;
    hdr->session_seq = session_seq + 1;
    hdr->session_start_qpc_us = sc_tick_now();
    hdr->producer_pid = (uint32_t) GetCurrentProcessId();

    // Publish the validity token last, like sh->seq on every slot: a consumer
    // only accepts a header whose other fields are already in place. The format
    // is still unknown here and is filled in by sc_obs_bridge_set_format().
    atomic_thread_fence(memory_order_release);
    hdr->magic = SC_OBS_SHM_MAGIC;

    bridge->mutex = mutex;
    bridge->mapping = mapping;
    bridge->event = event;
    bridge->base = base;
    bridge->hdr = hdr;
    bridge->slot_stride = stride;
    bridge->ready = true;

    LOGI("OBS bridge: created %s (%" PRIu64 " bytes, %u slots of %u frames)",
         map_name, total, SC_OBS_SHM_SLOT_COUNT, SC_OBS_SHM_SLOT_FRAMES);
    return true;
}

void
sc_obs_bridge_destroy(struct sc_obs_bridge *bridge) {
    if (!bridge->ready) {
        return;
    }

    uint64_t packets = bridge->hdr->write_seq;
    uint32_t overrun = bridge->hdr->overrun;

    // Let the consumer notice that the producer is gone
    bridge->hdr->magic = 0;
    atomic_thread_fence(memory_order_release);

    UnmapViewOfFile(bridge->base);
    CloseHandle((HANDLE) bridge->mapping);
    CloseHandle((HANDLE) bridge->event);
    release_producer_mutex((HANDLE) bridge->mutex);

    LOGI("OBS bridge: closed (packets=%" PRIu64 ", overrun=%" PRIu32 ")",
         packets, overrun);

    memset(bridge, 0, sizeof(*bridge));
}

bool
sc_obs_bridge_configure(struct sc_obs_bridge *bridge, uint32_t sample_rate,
                        uint32_t channels) {
    if (!bridge->ready) {
        return false;
    }

    if (!sample_rate) {
        LOGE("OBS bridge: invalid sample rate");
        return false;
    }

    if (!channels || channels > SC_OBS_SHM_CHANNELS_ALLOC) {
        LOGE("OBS bridge: %u channels not supported (max %u)",
             channels, SC_OBS_SHM_CHANNELS_ALLOC);
        return false;
    }

    // Realign the ring so that the consumer sees an empty queue for the new
    // session (write_seq is owned by us, and moving it back is safe: the
    // consumer only ever compares read_seq with write_seq).
    bridge->hdr->write_seq = bridge->hdr->read_seq;
    atomic_thread_fence(memory_order_release);

    bridge->hdr->sample_rate = sample_rate;
    bridge->hdr->channels = channels;
    atomic_thread_fence(memory_order_release);
    bridge->hdr->header_seq++;

    LOGI("OBS bridge: session configured (rate=%" PRIu32 " channels=%" PRIu32
         ")", sample_rate, channels);
    return true;
}

uint8_t *
sc_obs_bridge_begin_write(struct sc_obs_bridge *bridge, uint32_t *index) {
    if (!bridge->ready) {
        return NULL;
    }

    struct sc_obs_shm_header *hdr = bridge->hdr;
    uint32_t w = hdr->write_seq;

    if (w - hdr->read_seq >= hdr->slot_count) {
        // Ring full: the oldest slot will be overwritten
        hdr->overrun++;
    }

    *index = w;
    return slot_data(bridge, w % hdr->slot_count);
}

void
sc_obs_bridge_publish(struct sc_obs_bridge *bridge, uint32_t index,
                      uint32_t frames, int64_t pts_us, int64_t qpc_us) {
    if (!bridge->ready) {
        return;
    }

    struct sc_obs_shm_header *hdr = bridge->hdr;
    struct sc_obs_shm_slot_hdr *sh = slot_hdr(bridge, index % hdr->slot_count);

    sh->pts_us = pts_us;
    sh->qpc_us = qpc_us;
    sh->frames = frames;
    // Publish the payload before the validity token
    atomic_thread_fence(memory_order_release);
    sh->seq = index;
    // Publish the slot before advancing the shared write index
    atomic_thread_fence(memory_order_release);
    hdr->write_seq = index + 1;
    atomic_thread_fence(memory_order_release);

    SetEvent((HANDLE) bridge->event);
}

void
sc_obs_bridge_set_video_clock(struct sc_obs_bridge *bridge, int64_t pts_us,
                              int64_t qpc_us) {
    if (!bridge || !bridge->ready) {
        return;
    }

    bridge->hdr->video_pts_us = pts_us;
    bridge->hdr->video_qpc_us = qpc_us;
    atomic_thread_fence(memory_order_release);
    bridge->hdr->video_seq++;
}
