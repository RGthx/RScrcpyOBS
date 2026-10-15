#include "obs_video_gpu.h"

#include <inttypes.h>
#include <new>
#include <stdio.h>
#include <string.h>

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <SDL3/SDL_log.h>

// util/log.h pulls in C-only headers (options.h) which do not compile as C++,
// so use SDL logging directly here.
#define LOGI(...) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, __VA_ARGS__)
#define LOGW(...) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, __VA_ARGS__)
#define LOGE(...) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, __VA_ARGS__)

extern "C" {

struct sc_obs_video_gpu {
    // Either the borrowed application device (renderer/decoder) or one we
    // created ourselves when there is no renderer (--no-window).
    ID3D11Device *device;
    ID3D11DeviceContext *ctx;
    bool owns_device;

    ID3D11VideoDevice *video_device; // owned (QueryInterface)
    ID3D11VideoContext *video_ctx; // owned (QueryInterface)

    // NV12 -> BGRA conversion pipeline (only when a decoded surface is used)
    ID3D11VideoProcessorEnumerator *vp_enum; // owned
    ID3D11VideoProcessor *vp; // owned
    ID3D11Texture2D *nv12_tex; // owned, surface size
    ID3D11VideoProcessorInputView *in_view; // owned

    // Rotating shared BGRA textures (the ones OBS opens)
    ID3D11Texture2D *bgra_tex[SC_OBS_VIDEO_SLOT_COUNT];
    ID3D11VideoProcessorOutputView *out_view[SC_OBS_VIDEO_SLOT_COUNT];
    uint32_t handles[SC_OBS_VIDEO_SLOT_COUNT];

    uint32_t surf_w;
    uint32_t surf_h;
    uint32_t frame_w;
    uint32_t frame_h;

    bool gpu_failed; // a deterministic error was already logged
    bool logged_device;
};

// Enumerate the DXGI adapters (logging them) and return the index of the
// preferred one: the dedicated GPU OBS renders on.
//
// Legacy shared handles (D3D11_RESOURCE_MISC_SHARED) can only be opened by a
// device on the *same* adapter, so when we have to create our own device
// (--no-window: no renderer, hence no renderer device) we must not land on the
// integrated GPU by accident while OBS renders on the discrete one.
static int
log_and_pick_adapter(void) {
    IDXGIFactory1 *factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **) &factory);
    if (FAILED(hr)) {
        LOGW("OBS video: CreateDXGIFactory1 failed (0x%08lx)",
             (unsigned long) hr);
        return -1;
    }

    int best_index = -1;
    SIZE_T best_vram = 0;

    IDXGIAdapter1 *adapter = nullptr;
    for (UINT i = 0;
            factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            char name[256];
            int n = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                        name, sizeof(name), nullptr, nullptr);
            if (n <= 0) {
                name[0] = '\0';
            }
            bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            LOGI("OBS video: DXGI adapter %u: \"%s\" vendor=0x%04x "
                 "luid=0x%08lx%08lx vram=%lluMB software=%d",
                 i, name, desc.VendorId, (unsigned long) desc.AdapterLuid.HighPart,
                 (unsigned long) desc.AdapterLuid.LowPart,
                 (unsigned long long) (desc.DedicatedVideoMemory
                                       / (1024 * 1024)),
                 software ? 1 : 0);
            if (!software && desc.DedicatedVideoMemory > best_vram) {
                best_vram = desc.DedicatedVideoMemory;
                best_index = (int) i;
            }
        }
        adapter->Release();
    }

    factory->Release();

    if (best_index >= 0) {
        LOGI("OBS video: selected DXGI adapter %d (dedicated, %lluMB)",
             best_index, (unsigned long long) (best_vram / (1024 * 1024)));
    } else {
        LOGW("OBS video: no hardware DXGI adapter found, using default");
    }
    return best_index;
}

// Create our own device on the preferred adapter (only used when the caller did
// not provide the renderer device).
static bool
create_own_device(ID3D11Device **out_device, ID3D11DeviceContext **out_ctx) {
    int adapter_index = log_and_pick_adapter();

    IDXGIFactory1 *factory = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    if (adapter_index >= 0) {
        HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                        (void **) &factory);
        if (SUCCEEDED(hr)) {
            if (FAILED(factory->EnumAdapters1((UINT) adapter_index, &adapter))) {
                adapter = nullptr;
            }
        }
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN
                                                    : D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr, flags, nullptr, 0,
                                   D3D11_SDK_VERSION, out_device, &level,
                                   out_ctx);
    if (adapter) {
        adapter->Release();
    }
    if (factory) {
        factory->Release();
    }

    if (FAILED(hr)) {
        LOGE("OBS video: D3D11CreateDevice failed (0x%08lx)",
             (unsigned long) hr);
        return false;
    }

    LOGI("OBS video: created our own D3D11 device (feature level 0x%04x)",
         (unsigned) level);
    return true;
}

// Create a device for the caller to own (see the header): the decoder and the
// sink then share it, which is what keeps the whole path on one device.
void *
sc_obs_video_gpu_create_device(void) {
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    if (!create_own_device(&device, &ctx)) {
        return nullptr;
    }
    // The immediate context is only needed by the sink, which asks the device
    // for its own (GetImmediateContext) when it borrows it.
    if (ctx) {
        ctx->Release();
    }
    return device;
}

void
sc_obs_video_gpu_release_device(void *d3d11_device) {
    if (d3d11_device) {
        ((ID3D11Device *) d3d11_device)->Release();
    }
}

// Best-effort: log which DXGI adapter a device belongs to, and return its LUID.
static bool
device_adapter_info(ID3D11Device *device, char *name, size_t name_len,
                    uint32_t *luid_low, int32_t *luid_high) {
    IDXGIDevice *dxgi_device = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice),
                                      (void **) &dxgi_device))) {
        return false;
    }

    IDXGIAdapter *adapter = nullptr;
    bool ok = false;
    if (SUCCEEDED(dxgi_device->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC desc;
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
            if (name && name_len) {
                int n = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                            name, (int) name_len, nullptr,
                                            nullptr);
                if (n <= 0) {
                    name[0] = '\0';
                }
            }
            if (luid_low) {
                *luid_low = desc.AdapterLuid.LowPart;
            }
            if (luid_high) {
                *luid_high = (int32_t) desc.AdapterLuid.HighPart;
            }
            ok = true;
        }
        adapter->Release();
    }

    dxgi_device->Release();
    return ok;
}

static void
release_pipeline(struct sc_obs_video_gpu *gpu) {
    for (unsigned i = 0; i < SC_OBS_VIDEO_SLOT_COUNT; ++i) {
        if (gpu->out_view[i]) {
            gpu->out_view[i]->Release();
            gpu->out_view[i] = nullptr;
        }
        if (gpu->bgra_tex[i]) {
            gpu->bgra_tex[i]->Release();
            gpu->bgra_tex[i] = nullptr;
        }
        gpu->handles[i] = SC_OBS_VIDEO_HANDLE_INVALID;
    }
    if (gpu->in_view) {
        gpu->in_view->Release();
        gpu->in_view = nullptr;
    }
    if (gpu->nv12_tex) {
        gpu->nv12_tex->Release();
        gpu->nv12_tex = nullptr;
    }
    if (gpu->vp) {
        gpu->vp->Release();
        gpu->vp = nullptr;
    }
    if (gpu->vp_enum) {
        gpu->vp_enum->Release();
        gpu->vp_enum = nullptr;
    }
    gpu->surf_w = 0;
    gpu->surf_h = 0;
    gpu->frame_w = 0;
    gpu->frame_h = 0;
}

bool
sc_obs_video_gpu_create(struct sc_obs_video_gpu **out, void *d3d11_device) {
    sc_obs_video_gpu *gpu = new (std::nothrow) sc_obs_video_gpu();
    if (!gpu) {
        LOGE("OOM: could not allocate the OBS video GPU pipeline");
        return false;
    }

    memset(gpu, 0, sizeof(*gpu));
    for (unsigned i = 0; i < SC_OBS_VIDEO_SLOT_COUNT; ++i) {
        gpu->handles[i] = SC_OBS_VIDEO_HANDLE_INVALID;
    }

    if (d3d11_device) {
        // Borrow the application device (renderer, shared with the D3D11VA
        // decoder): the decoded textures then live on the same device we use
        // for the conversion, so no cross-device copy is involved.
        gpu->device = (ID3D11Device *) d3d11_device;
        gpu->device->AddRef();
        gpu->device->GetImmediateContext(&gpu->ctx);
        gpu->owns_device = false;
        LOGI("OBS video: using the renderer's D3D11 device");
    } else {
        if (!create_own_device(&gpu->device, &gpu->ctx)) {
            delete gpu;
            return false;
        }
        gpu->owns_device = true;
    }

    if (!gpu->ctx) {
        LOGE("OBS video: no immediate D3D11 context");
        sc_obs_video_gpu_destroy(gpu);
        return false;
    }

    HRESULT hr = gpu->device->QueryInterface(__uuidof(ID3D11VideoDevice),
                                             (void **) &gpu->video_device);
    if (FAILED(hr) || !gpu->video_device) {
        LOGE("OBS video: ID3D11VideoDevice unavailable (0x%08lx)",
             (unsigned long) hr);
        sc_obs_video_gpu_destroy(gpu);
        return false;
    }
    hr = gpu->ctx->QueryInterface(__uuidof(ID3D11VideoContext),
                                  (void **) &gpu->video_ctx);
    if (FAILED(hr) || !gpu->video_ctx) {
        LOGE("OBS video: ID3D11VideoContext unavailable (0x%08lx)",
             (unsigned long) hr);
        sc_obs_video_gpu_destroy(gpu);
        return false;
    }

    char name[256] = {0};
    uint32_t luid_low = 0;
    int32_t luid_high = 0;
    if (device_adapter_info(gpu->device, name, sizeof(name), &luid_low,
                            &luid_high)) {
        LOGI("OBS video: D3D11 device on adapter \"%s\" luid=0x%08lx%08lx",
             name, (unsigned long) (uint32_t) luid_high,
             (unsigned long) luid_low);
        gpu->logged_device = true;
    }

    *out = gpu;
    return true;
}

void
sc_obs_video_gpu_destroy(struct sc_obs_video_gpu *gpu) {
    if (!gpu) {
        return;
    }

    release_pipeline(gpu);

    if (gpu->video_ctx) {
        gpu->video_ctx->Release();
    }
    if (gpu->video_device) {
        gpu->video_device->Release();
    }
    if (gpu->ctx) {
        gpu->ctx->Release();
    }
    if (gpu->device) {
        gpu->device->Release();
    }

    delete gpu;
}

bool
sc_obs_video_gpu_adapter_luid(struct sc_obs_video_gpu *gpu, uint32_t *low,
                              int32_t *high) {
    if (!gpu || !gpu->device) {
        return false;
    }
    return device_adapter_info(gpu->device, nullptr, 0, low, high);
}

bool
sc_obs_video_gpu_adapter_name(struct sc_obs_video_gpu *gpu, char *buf,
                              size_t len) {
    if (!gpu || !gpu->device) {
        return false;
    }
    return device_adapter_info(gpu->device, buf, len, nullptr, nullptr);
}

// Create (or recreate) the shared BGRA textures and, when a decoded surface is
// used, the NV12 -> BGRA conversion pipeline.
static bool
create_pipeline(struct sc_obs_video_gpu *gpu, uint32_t surf_w, uint32_t surf_h,
                uint32_t width, uint32_t height) {
    release_pipeline(gpu);

    if (surf_w && surf_h) {
        // Intermediate NV12 texture matching the decoded surface. The visible
        // region may be a sub-rectangle (crop), handled by the processor stream
        // rect, so the texture is sized to the full surface.
        D3D11_TEXTURE2D_DESC nd = {};
        nd.Width = surf_w;
        nd.Height = surf_h;
        nd.MipLevels = 1;
        nd.ArraySize = 1;
        nd.Format = DXGI_FORMAT_NV12;
        nd.SampleDesc.Count = 1;
        nd.Usage = D3D11_USAGE_DEFAULT;
        nd.BindFlags = D3D11_BIND_RENDER_TARGET;
        HRESULT hr = gpu->device->CreateTexture2D(&nd, nullptr,
                                                  &gpu->nv12_tex);
        if (FAILED(hr)) {
            LOGE("OBS video: could not create NV12 texture %ux%u (0x%08lx)",
                 surf_w, surf_h, (unsigned long) hr);
            release_pipeline(gpu);
            return false;
        }

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = surf_w;
        cd.InputHeight = surf_h;
        cd.OutputWidth = width;
        cd.OutputHeight = height;
        cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        hr = gpu->video_device->CreateVideoProcessorEnumerator(&cd,
                                                              &gpu->vp_enum);
        if (FAILED(hr)) {
            LOGE("OBS video: CreateVideoProcessorEnumerator failed (0x%08lx)",
                 (unsigned long) hr);
            release_pipeline(gpu);
            return false;
        }

        hr = gpu->video_device->CreateVideoProcessor(gpu->vp_enum, 0,
                                                     &gpu->vp);
        if (FAILED(hr)) {
            LOGE("OBS video: CreateVideoProcessor failed (0x%08lx)",
                 (unsigned long) hr);
            release_pipeline(gpu);
            return false;
        }

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
        ivd.FourCC = 0;
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivd.Texture2D.ArraySlice = 0;
        ivd.Texture2D.MipSlice = 0;
        hr = gpu->video_device->CreateVideoProcessorInputView(gpu->nv12_tex,
                                                             gpu->vp_enum,
                                                             &ivd,
                                                             &gpu->in_view);
        if (FAILED(hr)) {
            LOGE("OBS video: CreateVideoProcessorInputView failed (0x%08lx)",
                 (unsigned long) hr);
            release_pipeline(gpu);
            return false;
        }
    }

    // The shared BGRA textures OBS opens. MiscFlags = MISC_SHARED is what gives
    // us a legacy 32-bit handle (the shape gs_texture_open_shared expects);
    // BIND_RENDER_TARGET is required to use a texture as a video processor
    // output, BIND_SHADER_RESOURCE so OBS can sample it.
    for (unsigned i = 0; i < SC_OBS_VIDEO_SLOT_COUNT; ++i) {
        D3D11_TEXTURE2D_DESC bd = {};
        bd.Width = width;
        bd.Height = height;
        bd.MipLevels = 1;
        bd.ArraySize = 1;
        bd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        bd.SampleDesc.Count = 1;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        HRESULT hr = gpu->device->CreateTexture2D(&bd, nullptr,
                                                  &gpu->bgra_tex[i]);
        if (FAILED(hr)) {
            LOGE("OBS video: could not create shared BGRA texture %ux%u "
                 "(0x%08lx)", width, height, (unsigned long) hr);
            release_pipeline(gpu);
            return false;
        }

        IDXGIResource *res = nullptr;
        hr = gpu->bgra_tex[i]->QueryInterface(__uuidof(IDXGIResource),
                                              (void **) &res);
        if (FAILED(hr) || !res) {
            LOGE("OBS video: IDXGIResource unavailable on slot %u (0x%08lx)",
                 i, (unsigned long) hr);
            release_pipeline(gpu);
            return false;
        }

        HANDLE handle = nullptr;
        hr = res->GetSharedHandle(&handle);
        res->Release();
        // Note: GetSharedHandle() can return S_OK with a NULL handle when the
        // texture was created without D3D11_RESOURCE_MISC_SHARED, so check the
        // handle itself, not just hr.
        if (FAILED(hr) || !handle || (uint32_t) (uintptr_t) handle == 0) {
            LOGE("OBS video: no legacy shared handle on slot %u "
                 "(hr=0x%08lx handle=%p)", i, (unsigned long) hr, handle);
            release_pipeline(gpu);
            return false;
        }
        gpu->handles[i] = (uint32_t) (uintptr_t) handle;

        if (surf_w && surf_h) {
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
            ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            ovd.Texture2D.MipSlice = 0;
            hr = gpu->video_device->CreateVideoProcessorOutputView(
                gpu->bgra_tex[i], gpu->vp_enum, &ovd, &gpu->out_view[i]);
            if (FAILED(hr)) {
                LOGE("OBS video: CreateVideoProcessorOutputView slot %u "
                     "failed (0x%08lx)", i, (unsigned long) hr);
                release_pipeline(gpu);
                return false;
            }
        }
    }

    gpu->surf_w = surf_w;
    gpu->surf_h = surf_h;
    gpu->frame_w = width;
    gpu->frame_h = height;

    LOGI("OBS video: %s %ux%u (surface %ux%u), handles 0x%08x 0x%08x 0x%08x",
         (surf_w && surf_h) ? "conversion pipeline ready for" : "textures ready",
         width, height, surf_w, surf_h, gpu->handles[0], gpu->handles[1],
         gpu->handles[2]);
    return true;
}

bool
sc_obs_video_gpu_ensure(struct sc_obs_video_gpu *gpu, uint32_t surface_w,
                        uint32_t surface_h, uint32_t width, uint32_t height,
                        uint32_t handles[SC_OBS_VIDEO_SLOT_COUNT]) {
    if (!gpu || !width || !height) {
        return false;
    }

    if (gpu->bgra_tex[0] && gpu->surf_w == surface_w
            && gpu->surf_h == surface_h && gpu->frame_w == width
            && gpu->frame_h == height) {
        return false; // unchanged: the published handles are still valid
    }

    if (!create_pipeline(gpu, surface_w, surface_h, width, height)) {
        return false;
    }

    memcpy(handles, gpu->handles, sizeof(gpu->handles));
    return true;
}

bool
sc_obs_video_gpu_blt_d3d11(struct sc_obs_video_gpu *gpu, void *resource,
                           uint32_t slice, uint32_t crop_left,
                           uint32_t crop_top, uint32_t width, uint32_t height,
                           uint32_t slot, int ycbcr_bt709, int full_range) {
    if (!gpu || !resource || !gpu->nv12_tex || !gpu->out_view[slot]) {
        return false;
    }

    if (gpu->gpu_failed) {
        return false;
    }

    ID3D11Resource *src = (ID3D11Resource *) resource;

    // Copies the decoded surface slice into our own NV12 texture. Both are the
    // same size, so copy the whole resource (pBox = NULL): the alignment
    // padding is discarded later by the processor source rect.
    gpu->ctx->CopySubresourceRegion(gpu->nv12_tex, 0, 0, 0, 0, src, slice,
                                    nullptr);

    RECT src_rect = {
        (LONG) crop_left,
        (LONG) crop_top,
        (LONG) (crop_left + width),
        (LONG) (crop_top + height),
    };
    gpu->video_ctx->VideoProcessorSetStreamSourceRect(gpu->vp, 0, TRUE,
                                                      &src_rect);

    RECT dst_rect = { 0, 0, (LONG) width, (LONG) height };
    gpu->video_ctx->VideoProcessorSetStreamDestRect(gpu->vp, 0, TRUE, &dst_rect);

    // Input: NV12, BT.601/BT.709 matrix, full (0-255) or studio (16-235) range.
    // The enum values are 1 = NOMINAL_RANGE_16_235 and 2 = NOMINAL_RANGE_0_255,
    // so use the named constants rather than those numbers.
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs = {};
    in_cs.Usage = 0; // playback
    in_cs.YCbCr_Matrix = ycbcr_bt709 ? 1 : 0;
    in_cs.Nominal_Range = full_range
                              ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
                              : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    gpu->video_ctx->VideoProcessorSetStreamColorSpace(gpu->vp, 0, &in_cs);

    // Output: full-range RGB (RGB_Range = 0), matching the CPU path.
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs = {};
    out_cs.Usage = 0; // playback
    out_cs.RGB_Range = 0; // 0-255 (full range)
    gpu->video_ctx->VideoProcessorSetOutputColorSpace(gpu->vp, &out_cs);

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.OutputIndex = 0;
    stream.InputFrameOrField = 0;
    stream.PastFrames = 0;
    stream.FutureFrames = 0;
    stream.pInputSurface = gpu->in_view;

    HRESULT hr = gpu->video_ctx->VideoProcessorBlt(gpu->vp, gpu->out_view[slot],
                                                   0, 1, &stream);
    if (FAILED(hr)) {
        LOGE("OBS video: VideoProcessorBlt failed (0x%08lx)",
             (unsigned long) hr);
        gpu->gpu_failed = true;
        return false;
    }

    return true;
}

bool
sc_obs_video_gpu_upload_bgra(struct sc_obs_video_gpu *gpu, const uint8_t *bgra,
                             uint32_t pitch, uint32_t height, uint32_t slot) {
    if (!gpu || !bgra || !gpu->bgra_tex[slot]) {
        return false;
    }

    // UploadSubresource would be cheaper but cannot be created with
    // D3D11_USAGE_DYNAMIC (invalid for shared textures); UpdateSubresource is
    // the supported path. It returns no HRESULT, so there is no error code to
    // check here.
    gpu->ctx->UpdateSubresource(gpu->bgra_tex[slot], 0, nullptr, bgra, pitch,
                                pitch * height);
    return true;
}

void
sc_obs_video_gpu_flush(struct sc_obs_video_gpu *gpu) {
    if (gpu && gpu->ctx) {
        gpu->ctx->Flush();
    }
}

} // extern "C"
