#ifndef RSCRCPY_SHM_READER_H
#define RSCRCPY_SHM_READER_H

/*
 * Consumer side of the scrcpy <-> OBS shared-memory audio ring.
 *
 * Deliberately free of any libobs dependency so that it can be exercised on
 * its own; the plugin only feeds the packets it returns to
 * obs_source_output_audio().
 *
 * The ring protocol is defined in obs_shm_proto.h, which is included directly
 * from the scrcpy source tree (see obs-plugin/build.ps1) so that the two sides
 * can never diverge.
 */

#include <stdbool.h>
#include <stdint.h>

#include "obs_shm_proto.h"

struct sc_obs_shm_reader {
    char name[SC_OBS_SHM_NAME_MAX + 1];

    void *mapping; // HANDLE of the section
    void *event; // HANDLE of the auto-reset "data available" event
    uint8_t *base; // mapped view
    struct sc_obs_shm_header *hdr;
    uint32_t stride; // slot header + PCM payload

    bool connected;
    uint32_t session_seq; // producer start token (any change = new session)
    uint32_t header_seq; // audio format token (any change = new format)
    uint32_t read_seq; // next slot to consume (only this side writes it)
    uint32_t sample_rate;
    uint32_t channels;

    // diagnostics
    uint64_t packets;
    uint64_t frames;
    uint64_t missed; // slots whose validity token did not match
    uint64_t dropped; // slots overwritten before being read
    uint64_t last_qpc_us; // qpc of the last packet handed out
    uint64_t last_data_ms; // GetTickCount64() at the last packet
    uint32_t overrun_seen; // producer-reported overrun at connect time
};

struct sc_obs_audio_packet {
    // Interleaved float32 PCM, points into the shared section: only valid
    // until the next sc_obs_shm_reader_poll() call.
    const float *data;
    uint32_t frames;
    uint32_t channels;
    uint32_t sample_rate;
    // Host QPC timestamp in nanoseconds (OBS timebase)
    uint64_t ts_ns;
};

// Try to attach to the section named "<prefix><name>". Returns false when the
// producer is not running yet (which is not an error: just retry later).
bool
sc_obs_shm_reader_connect(struct sc_obs_shm_reader *r, const char *name);

void
sc_obs_shm_reader_disconnect(struct sc_obs_shm_reader *r);

// Block until at least one slot may be available, or the timeout expires.
// Returns false when the connection must be re-established (producer gone or
// a new session started).
bool
sc_obs_shm_reader_wait(struct sc_obs_shm_reader *r, uint32_t timeout_ms);

// Consume up to max packets, advancing the ring. Returns the number of valid
// packets written to out (0 when nothing new arrived).
uint32_t
sc_obs_shm_reader_poll(struct sc_obs_shm_reader *r,
                       struct sc_obs_audio_packet *out, uint32_t max);

// Whether the consumer is attached and has received audio recently.
bool
sc_obs_shm_reader_is_stalled(const struct sc_obs_shm_reader *r,
                             uint32_t threshold_ms);

#endif
