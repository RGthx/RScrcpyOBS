#ifndef SC_OBS_BRIDGE_H
#define SC_OBS_BRIDGE_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>

#include "obs_shm_proto.h"

/**
 * OBS audio bridge (producer side)
 *
 * Owns the named shared memory section and its signalling event. The audio
 * sink writes decoded PCM into it, and an OBS plugin reads it out.
 *
 * The section is created once for the whole scrcpy run (not per stream
 * session), so that the OBS plugin keeps a valid handle across reconnects and
 * resolution changes.
 */
struct sc_obs_bridge {
    void *mapping; // HANDLE (void* to keep windows.h out of this header)
    void *event; // HANDLE
    uint8_t *base; // mapped view
    struct sc_obs_shm_header *hdr;
    size_t slot_stride; // sizeof(slot header) + slot_bytes
    bool ready;
};

// Create the shared memory section and its event. On failure, the bridge is
// left not-ready and all other functions are no-ops (the bridge is optional:
// scrcpy must keep working without it).
bool
sc_obs_bridge_init(struct sc_obs_bridge *bridge, const char *name);

void
sc_obs_bridge_destroy(struct sc_obs_bridge *bridge);

static inline bool
sc_obs_bridge_is_ready(const struct sc_obs_bridge *bridge) {
    return bridge->ready;
}

// Publish the audio parameters of a new session (called from sink open())
bool
sc_obs_bridge_configure(struct sc_obs_bridge *bridge, uint32_t sample_rate,
                        uint32_t channels);

/**
 * Acquire the PCM buffer of the next slot to fill.
 *
 * Returns NULL if the bridge is not ready. The returned index must be passed
 * back to sc_obs_bridge_publish() once the PCM data has been written.
 */
uint8_t *
sc_obs_bridge_begin_write(struct sc_obs_bridge *bridge, uint32_t *index);

// Publish a filled slot to the consumer.
void
sc_obs_bridge_publish(struct sc_obs_bridge *bridge, uint32_t index,
                      uint32_t frames, int64_t pts_us, int64_t qpc_us);

// Record the last video frame timestamp (A/V calibration support).
void
sc_obs_bridge_set_video_clock(struct sc_obs_bridge *bridge, int64_t pts_us,
                              int64_t qpc_us);

#endif
