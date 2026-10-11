#ifndef SC_OBS_SHM_PROTO_H
#define SC_OBS_SHM_PROTO_H

/*
 * Shared-memory protocol between scrcpy (producer) and the OBS plugin
 * (consumer) for the OBS audio bridge.
 *
 * This header is the single source of truth: the OBS plugin build includes it
 * directly from this directory (obs-plugin/build.ps1 adds app/src to the
 * include path), so the two sides cannot diverge.
 *
 * Layout inside the named section:
 *
 *     [ struct sc_obs_shm_header ]   (hdr->arena_offset bytes, 64-byte aligned)
 *     [ slot 0 hdr ][ slot 0 pcm ]
 *     [ slot 1 hdr ][ slot 1 pcm ]
 *     ...
 *
 * The ring indices are plain volatile counters published with explicit
 * release/acquire fences instead of C11 atomics: this keeps the struct layout
 * identical for any C11 compiler (the two sides may be built with different
 * compilers) and avoids relying on any lock-free guarantee for shared memory.
 * All counters are 32-bit and may wrap; consumers must use unsigned
 * differences only.
 *
 * The producer is the only writer of write_seq, the consumer the only writer
 * of read_seq. The ring is a single-producer/single-consumer queue: if the
 * consumer is too slow, the producer overwrites the oldest slots (live-stream
 * semantics) and counts it in overrun.
 *
 * PCM payload is interleaved float32 (native endianness), which is exactly
 * what OBS expects (AUDIO_FORMAT_FLOAT).
 */

#include <stdint.h>

#define SC_OBS_SHM_MAGIC 0x53434F42u // 'S', 'C', 'O', 'B'
#define SC_OBS_SHM_VERSION 1u

// Android audio is mono or stereo; the arena is sized for this many channels
#define SC_OBS_SHM_CHANNELS_ALLOC 2u

// Maximum PCM frames per slot (Opus 120 ms @ 48 kHz = 5760)
#define SC_OBS_SHM_SLOT_FRAMES 8192u

// Ring length: 32 * 8192 / 48000 = 5.46 s of buffering at 48 kHz
#define SC_OBS_SHM_SLOT_COUNT 32u

// Maximum length of the user-provided shared-memory name suffix
#define SC_OBS_SHM_NAME_MAX 64

// Section/event name prefixes (the suffix comes from --obs-audio-name)
#define SC_OBS_SHM_NAME_PREFIX "Local\\scrcpy_audio_v1_"

struct sc_obs_shm_slot_hdr {
    // PTS on the device (phone) timeline, microseconds (diagnostic/calibration)
    volatile int64_t pts_us;
    // Host QueryPerformanceCounter at queue time, microseconds (playback clock)
    volatile int64_t qpc_us;
    // Number of valid PCM frames in this slot
    volatile uint32_t frames;
    // The write_seq value this slot currently holds (validity token)
    volatile uint32_t seq;
};

struct sc_obs_shm_header {
    uint32_t magic; // SC_OBS_SHM_MAGIC
    uint32_t version; // SC_OBS_SHM_VERSION
    uint32_t header_size; // sizeof(struct sc_obs_shm_header)
    uint32_t slot_hdr_size; // sizeof(struct sc_obs_shm_slot_hdr)
    uint32_t arena_offset; // offset of the first slot from the section start
    uint32_t slot_bytes; // PCM payload bytes per slot
    uint32_t slot_frames; // PCM frames per slot
    uint32_t slot_count; // number of slots in the ring

    // Session audio parameters (updated on every new session)
    volatile uint32_t sample_rate;
    volatile uint32_t channels;
    volatile uint32_t header_seq; // incremented on every session change

    // Ring state
    volatile uint32_t write_seq; // slots published by the producer
    volatile uint32_t read_seq; // slots consumed by the consumer
    volatile uint32_t overrun; // slots overwritten before being read
    volatile uint32_t session_seq; // incremented once per producer start

    // Video clock, written by the OBS video sink on every sent frame (A/V
    // calibration support; not used for playback scheduling)
    volatile int64_t video_pts_us;
    volatile int64_t video_qpc_us;
    volatile uint32_t video_seq;
    uint32_t reserved0;

    int64_t session_start_qpc_us; // host clock at section creation
    uint32_t producer_pid;
    uint32_t reserved1;
};

#endif
