#ifndef SC_HWDEC_H
#define SC_HWDEC_H

#include "common.h"

#include <stdbool.h>

#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <SDL3/SDL.h>

struct sc_hwdec {
    const char *name; // must be statically allocated (e.g. a string literal)

    enum AVHWDeviceType hw_type;
    // If true, the hw_type is mandatory, no fallback to software decoding is
    // permitted
    bool hw_forced;
    AVBufferRef *hw_device_ctx;
};

// d3d11_device: an ID3D11Device the caller owns, used by the D3D11VA path when
// there is no renderer (--no-window + --obs-video: scrcpy.c creates the device so
// the decoder and the OBS sink can share it). NULL otherwise.
bool
sc_hwdec_init(struct sc_hwdec *hwdec, enum AVHWDeviceType hw_type,
              const char *hw_device, bool hw_forced, SDL_Renderer *renderer,
              void *d3d11_device);

const AVCodec *
sc_hwdec_find_decoder(struct sc_hwdec *hwdec, enum AVCodecID codec_id);

bool
sc_hwdec_configure(struct sc_hwdec *hwdec, AVCodecContext *ctx);

void
sc_hwdec_destroy(struct sc_hwdec *hwdec);

#endif
