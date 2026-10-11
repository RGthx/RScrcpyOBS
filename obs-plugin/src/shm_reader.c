#include "shm_reader.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <windows.h>

// Same geometry as the producer (obs_bridge.c); the values themselves always
// come from the header, these are only the name buffer sizes.
#define READER_NAME_MAX 128

static bool
build_names(const char *suffix, char *map_name, char *evt_name) {
    const char *s = suffix && *suffix ? suffix : "0";

    int n = snprintf(map_name, READER_NAME_MAX, "%s%s",
                     SC_OBS_SHM_NAME_PREFIX, s);
    if (n < 0 || n >= READER_NAME_MAX) {
        return false;
    }

    n = snprintf(evt_name, READER_NAME_MAX, "%s%s_evt",
                 SC_OBS_SHM_NAME_PREFIX, s);
    if (n < 0 || n >= READER_NAME_MAX) {
        return false;
    }

    return true;
}

static inline struct sc_obs_shm_slot_hdr *
slot_hdr(struct sc_obs_shm_reader *r, uint32_t index) {
    // The sequence numbers are absolute (and wrap): fold them back onto the
    // physical ring, exactly like the producer does. Forgetting this modulo
    // walks past the end of the section as soon as write_seq >= slot_count.
    uint32_t slot = index % r->hdr->slot_count;
    return (struct sc_obs_shm_slot_hdr *) (r->base + r->hdr->arena_offset
                                         + (size_t) slot * r->stride);
}

static inline float *
slot_data(struct sc_obs_shm_reader *r, uint32_t index) {
    return (float *) ((uint8_t *) slot_hdr(r, index)
                    + sizeof(struct sc_obs_shm_slot_hdr));
}

void
sc_obs_shm_reader_disconnect(struct sc_obs_shm_reader *r) {
    if (r->base) {
        UnmapViewOfFile(r->base);
    }
    if (r->mapping) {
        CloseHandle((HANDLE) r->mapping);
    }
    if (r->event) {
        CloseHandle((HANDLE) r->event);
    }

    r->base = NULL;
    r->mapping = NULL;
    r->event = NULL;
    r->hdr = NULL;
    r->connected = false;
}

bool
sc_obs_shm_reader_connect(struct sc_obs_shm_reader *r, const char *name) {
    if (r->connected) {
        return true;
    }

    char map_name[READER_NAME_MAX];
    char evt_name[READER_NAME_MAX];
    if (!build_names(name, map_name, evt_name)) {
        return false;
    }

    HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, map_name);
    if (!mapping) {
        // The producer is not running (yet): not an error
        return false;
    }

    uint8_t *base = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!base) {
        CloseHandle(mapping);
        return false;
    }

    struct sc_obs_shm_header *hdr = (struct sc_obs_shm_header *) base;

    // Refuse anything we do not understand instead of reading garbage
    if (hdr->magic != SC_OBS_SHM_MAGIC
            || hdr->version != SC_OBS_SHM_VERSION) {
        UnmapViewOfFile(base);
        CloseHandle(mapping);
        return false;
    }

    uint32_t stride = hdr->slot_hdr_size + hdr->slot_bytes;
    if (hdr->header_size > hdr->arena_offset || !stride
            || !hdr->slot_count || !hdr->slot_frames) {
        UnmapViewOfFile(base);
        CloseHandle(mapping);
        return false;
    }

    // The section could be smaller than what the header claims (a leftover
    // section from an older build); verify before trusting the geometry.
    MEMORY_BASIC_INFORMATION mbi;
    size_t needed = (size_t) hdr->arena_offset
                  + (size_t) hdr->slot_count * stride;
    if (!VirtualQuery(base, &mbi, sizeof(mbi)) || mbi.RegionSize < needed) {
        UnmapViewOfFile(base);
        CloseHandle(mapping);
        return false;
    }

    // A session can be valid and still unconfigured: the producer clears the
    // header when it starts, sets the magic, and only fills in the format once
    // the audio decoder is ready. Do not attach during that window - otherwise
    // we would hand libobs "0 Hz, 0 channels" and, worse, emit packets carrying
    // that bogus format. A retry a moment later finds a complete header.
    if (hdr->sample_rate < 8000 || hdr->sample_rate > 384000
            || hdr->channels < 1
            || hdr->channels > SC_OBS_SHM_CHANNELS_ALLOC) {
        UnmapViewOfFile(base);
        CloseHandle(mapping);
        return false;
    }

    HANDLE event = OpenEventA(SYNCHRONIZE, FALSE, evt_name);
    if (!event) {
        UnmapViewOfFile(base);
        CloseHandle(mapping);
        return false;
    }

    strncpy(r->name, name ? name : "0", sizeof(r->name) - 1);
    r->name[sizeof(r->name) - 1] = '\0';
    r->mapping = mapping;
    r->event = event;
    r->base = base;
    r->hdr = hdr;
    r->stride = stride;
    r->session_seq = hdr->session_seq;
    r->header_seq = hdr->header_seq;

    // Start from the live edge: never replay what is already buffered, this
    // is a live stream (up to 5.4 s of stale audio could be sitting there).
    uint32_t write_seq = hdr->write_seq;
    atomic_thread_fence(memory_order_acquire);
    r->read_seq = write_seq;
    hdr->read_seq = write_seq;

    r->sample_rate = hdr->sample_rate;
    r->channels = hdr->channels;
    r->overrun_seen = hdr->overrun;
    r->last_data_ms = GetTickCount64();
    r->connected = true;
    return true;
}

bool
sc_obs_shm_reader_wait(struct sc_obs_shm_reader *r, uint32_t timeout_ms) {
    if (!r->connected) {
        return false;
    }

    if (r->hdr->magic != SC_OBS_SHM_MAGIC
            || r->hdr->session_seq != r->session_seq) {
        // Producer exited, or a new producer took the section over
        return false;
    }

    // Auto-reset event pulsed by the producer on every published slot; a
    // timeout simply means no audio arrived, which is not an error.
    WaitForSingleObject((HANDLE) r->event, timeout_ms);

    return r->hdr->magic == SC_OBS_SHM_MAGIC
        && r->hdr->session_seq == r->session_seq;
}

uint32_t
sc_obs_shm_reader_poll(struct sc_obs_shm_reader *r,
                       struct sc_obs_audio_packet *out, uint32_t max) {
    if (!r->connected || !max) {
        return 0;
    }

    struct sc_obs_shm_header *hdr = r->hdr;
    uint32_t write_seq = hdr->write_seq;
    atomic_thread_fence(memory_order_acquire);

    if (hdr->header_seq != r->header_seq) {
        // New session format (sample rate / channels): do not replay across it
        r->header_seq = hdr->header_seq;
        r->sample_rate = hdr->sample_rate;
        r->channels = hdr->channels;
        r->read_seq = write_seq;
    }

    uint32_t available = write_seq - r->read_seq; // unsigned: wraps safely

    if (available > hdr->slot_count) {
        // The producer ran ahead and overwrote slots we had not read yet: the
        // ring only guarantees the most recent slot_count slots. Note that the
        // producer never advances read_seq, so resynchronising is our job.
        uint32_t skip = available - hdr->slot_count;
        r->read_seq += skip;
        r->dropped += skip;
        available = hdr->slot_count;
    }

    uint32_t produced = 0;
    while (produced < max && r->read_seq != write_seq) {
        uint32_t index = r->read_seq;
        struct sc_obs_shm_slot_hdr *sh = slot_hdr(r, index);

        if (sh->seq != index) {
            // Not published (yet), or overwritten in the meantime
            r->missed++;
            r->read_seq++;
            continue;
        }

        // Pairs with the producer's release fence before sh->seq, so that the
        // payload reads below cannot be hoisted above the token check.
        atomic_thread_fence(memory_order_acquire);

        uint32_t frames = sh->frames;
        if (!frames || frames > hdr->slot_frames) {
            r->missed++;
            r->read_seq++;
            continue;
        }

        out[produced].data = slot_data(r, index);
        out[produced].frames = frames;
        out[produced].channels = r->channels;
        out[produced].sample_rate = r->sample_rate;
        out[produced].ts_ns = (uint64_t) sh->qpc_us * 1000ull;

        r->last_qpc_us = (uint64_t) sh->qpc_us;
        produced++;
        r->read_seq++;
    }

    // Let the producer know how far we got (used only for its overrun count)
    hdr->read_seq = r->read_seq;
    atomic_thread_fence(memory_order_release);

    if (produced) {
        r->packets += produced;
        r->last_data_ms = GetTickCount64();
        for (uint32_t i = 0; i < produced; i++) {
            r->frames += out[i].frames;
        }
    }

    return produced;
}

bool
sc_obs_shm_reader_is_stalled(const struct sc_obs_shm_reader *r,
                             uint32_t threshold_ms) {
    if (!r->connected) {
        return true;
    }
    return GetTickCount64() - r->last_data_ms > threshold_ms;
}
