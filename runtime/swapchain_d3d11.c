// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// DIRECT3D 11 SWAPCHAINS (XR_KHR_D3D11_enable): textures on the application's device, copied at release
// into a staging twin and read from there (DESIGN.md, "Frames: copied, not shared"), on the device's
// immediate context. That context is the application's and is not thread-safe: OpenXR has the application
// call xrReleaseSwapchainImage from its rendering, which is where this runs -- the same assumption as
// swapchain_gl.c's "the context is current on this thread".
//
// Also what both Direct3Ds share: the formats, and the adapter the runtime asks for.
#include "internal.h"

#include <dxgi1_2.h>
#include <stdlib.h>
#include <string.h>

// ---- shared with swapchain_d3d12.c ----------------------------------------------------------------

// sRGB first, which is what OpenXR applications are told to prefer; then the UNORM ones, then depth.
static const int64_t FORMATS[] = {
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM,
    DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
};

const int64_t *d3d_formats(uint32_t *n)
{
    *n = sizeof FORMATS / sizeof FORMATS[0];
    return FORMATS;
}

bool d3d_is_color(int64_t f)
{
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_UNORM ||
           f == DXGI_FORMAT_B8G8R8A8_UNORM;
}

void d3d_byte_order(struct swapchain *sc)
{
    const int64_t f = sc->info.format;
    sc->bgra = f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM;
    sc->linear = f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM;
}

// A depth texture the application will also sample has to be created TYPELESS, with views of their own
// format made from it: a D32_FLOAT texture can be a depth buffer but not a shader resource.
int64_t d3d_typeless_depth(int64_t f)
{
    switch (f) {
        case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
        case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
        default: return f;
    }
}

// THE ADAPTER: the first DXGI enumerates, which is the one Windows puts its desktop on. Looked up at run
// time like everything else the runtime uses of a graphics API -- it links no loader, and an application
// that never asks for Direct3D never loads dxgi.dll because of this.
bool d3d_default_adapter(LUID *luid)
{
    static const char *const dxgi[] = { "dxgi.dll", NULL };
    typedef HRESULT(WINAPI * create_factory)(REFIID, void **);
    create_factory create = (create_factory)plat_symbol(dxgi, "CreateDXGIFactory1");
    if (create == NULL) return false;
    IDXGIFactory1 *factory = NULL;
    if (FAILED(create(&IID_IDXGIFactory1, (void **)&factory))) return false;
    IDXGIAdapter1 *adapter = NULL;
    bool ok = false;
    if (SUCCEEDED(IDXGIFactory1_EnumAdapters1(factory, 0, &adapter))) {
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &desc))) {
            *luid = desc.AdapterLuid;
            char name[128];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name, NULL, NULL);
            say("Direct3D adapter: %s", name);
            ok = true;
        }
        IDXGIAdapter1_Release(adapter);
    }
    IDXGIFactory1_Release(factory);
    return ok;
}

// THE DEVICE ON ANOTHER ADAPTER: XR_KHR_D3D11_enable and _D3D12_enable have the application make its device
// on the adapter the requirements named, and the session refused (XR_ERROR_GRAPHICS_DEVICE_INVALID) when it
// did not. With two GPUs, an application that ignores the LUID would otherwise fail later and far from the
// cause; with one, every device passes.
bool d3d_same_adapter(const struct instance *in, LUID device, const char *api)
{
    if (device.LowPart == in->d3d_luid.LowPart && device.HighPart == in->d3d_luid.HighPart) return true;
    fprintf(stderr, "openxr-sim: xrCreateSession: the %s device is not on the adapter xrGet%sGraphicsRequirementsKHR named "
            "(LUID %08lx:%08lx, not %08lx:%08lx)\n", api, api, (unsigned long)device.HighPart, (unsigned long)device.LowPart,
            (unsigned long)in->d3d_luid.HighPart, (unsigned long)in->d3d_luid.LowPart);
    return false;
}

// ---- Direct3D 11 -----------------------------------------------------------------------------------

struct d3d {
    ID3D11Device *device;
    ID3D11DeviceContext *context;
};

XrResult d3d11_session_init(struct session *s, const XrGraphicsBindingD3D11KHR *b)
{
    if (b->device == NULL) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    // Its adapter, through DXGI: a Direct3D 11 device is also an IDXGIDevice.
    IDXGIDevice *dx = NULL;
    IDXGIAdapter *adapter = NULL;
    DXGI_ADAPTER_DESC desc = { 0 };
    bool known = SUCCEEDED(ID3D11Device_QueryInterface(b->device, &IID_IDXGIDevice, (void **)&dx)) &&
                 SUCCEEDED(IDXGIDevice_GetAdapter(dx, &adapter)) && SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &desc));
    if (adapter != NULL) IDXGIAdapter_Release(adapter);
    if (dx != NULL) IDXGIDevice_Release(dx);
    if (!known) say("Direct3D 11: the device's adapter could not be found; not checked");
    else if (!d3d_same_adapter(s->in, desc.AdapterLuid, "D3D11")) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    struct d3d *d = calloc(1, sizeof *d);
    if (d == NULL) return XR_ERROR_OUT_OF_MEMORY;
    d->device = b->device;
    ID3D11Device_AddRef(d->device);
    ID3D11Device_GetImmediateContext(d->device, &d->context);
    s->d3d = d;
    say("Direct3D 11: feature level 0x%x", (unsigned)ID3D11Device_GetFeatureLevel(d->device));
    return XR_SUCCESS;
}

void d3d11_session_fini(struct session *s)
{
    struct d3d *d = s->d3d;
    if (d == NULL) return;
    if (d->context != NULL) ID3D11DeviceContext_Release(d->context);
    if (d->device != NULL) ID3D11Device_Release(d->device);
    free(d);
    s->d3d = NULL;
}

XrResult d3d11_swapchain_init(struct swapchain *sc)
{
    struct d3d *d = sc->session->d3d;
    const XrSwapchainUsageFlags u = sc->info.usageFlags;
    sc->color = d3d_is_color(sc->info.format);
    d3d_byte_order(sc);
    UINT bind = 0;
    if (sc->color) {
        if ((u & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) || u == 0) bind |= D3D11_BIND_RENDER_TARGET;
    } else {
        bind |= D3D11_BIND_DEPTH_STENCIL;
    }
    if (u & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) bind |= D3D11_BIND_SHADER_RESOURCE;
    if (u & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) bind |= D3D11_BIND_UNORDERED_ACCESS;
    const bool typeless = !sc->color && (bind & D3D11_BIND_SHADER_RESOURCE);
    D3D11_TEXTURE2D_DESC desc = {
        .Width = sc->info.width, .Height = sc->info.height, .MipLevels = sc->info.mipCount, .ArraySize = sc->info.arraySize,
        .Format = (DXGI_FORMAT)(typeless ? d3d_typeless_depth(sc->info.format) : sc->info.format),
        .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = bind,
    };
    for (uint32_t i = 0; i < sc->count; i++) {
        ID3D11Texture2D *t = NULL;
        const HRESULT hr = ID3D11Device_CreateTexture2D(d->device, &desc, NULL, &t);
        if (FAILED(hr)) { say("Direct3D 11: CreateTexture2D failed (0x%08lx)", (unsigned long)hr); return XR_ERROR_RUNTIME_FAILURE; }
        sc->d3d_image[i] = t;
    }
    if (!sc->color) return XR_SUCCESS;
    // THE COPY's: one staging texture of the first mip of every layer, and the pixels it is read into.
    D3D11_TEXTURE2D_DESC staging = desc;
    staging.MipLevels = 1;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *st = NULL;
    const HRESULT hr = ID3D11Device_CreateTexture2D(d->device, &staging, NULL, &st);
    if (FAILED(hr)) { say("Direct3D 11: the staging texture failed (0x%08lx)", (unsigned long)hr); return XR_ERROR_RUNTIME_FAILURE; }
    sc->d3d_readback = st;
    const size_t bytes = (size_t)sc->info.width * sc->info.height * 4 * sc->info.arraySize;
    for (uint32_t i = 0; i < sc->count; i++)
        if ((sc->pixels[i] = malloc(bytes)) == NULL) return XR_ERROR_OUT_OF_MEMORY;
    return XR_SUCCESS;
}

void d3d11_swapchain_fini(struct swapchain *sc)
{
    for (uint32_t i = 0; i < 4; i++) {
        if (sc->d3d_image[i] != NULL) ID3D11Texture2D_Release((ID3D11Texture2D *)sc->d3d_image[i]);
        sc->d3d_image[i] = NULL;
        free(sc->pixels[i]);
        sc->pixels[i] = NULL;
    }
    if (sc->d3d_readback != NULL) ID3D11Texture2D_Release((ID3D11Texture2D *)sc->d3d_readback);
    sc->d3d_readback = NULL;
}

XrResult d3d11_enumerate_images(struct swapchain *sc, XrSwapchainImageBaseHeader *images)
{
    XrSwapchainImageD3D11KHR *out = (XrSwapchainImageD3D11KHR *)images;
    for (uint32_t i = 0; i < sc->count; i++) {
        if (out[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) return XR_ERROR_VALIDATION_FAILURE;
        out[i].texture = (ID3D11Texture2D *)sc->d3d_image[i];
    }
    return XR_SUCCESS;
}

// Each layer's first mip into the staging texture, then mapped row by row into the pixels: Map waits for
// the copy, and the row pitch is the driver's.
XrResult d3d11_swapchain_read(struct swapchain *sc, uint32_t image)
{
    struct d3d *d = sc->session->d3d;
    ID3D11Resource *src = (ID3D11Resource *)sc->d3d_image[image];
    ID3D11Resource *dst = (ID3D11Resource *)sc->d3d_readback;
    const uint32_t w = sc->info.width, h = sc->info.height;
    for (uint32_t layer = 0; layer < sc->info.arraySize; layer++)
        ID3D11DeviceContext_CopySubresourceRegion(d->context, dst, layer, 0, 0, 0, src, layer * sc->info.mipCount, NULL);
    for (uint32_t layer = 0; layer < sc->info.arraySize; layer++) {
        D3D11_MAPPED_SUBRESOURCE m;
        const HRESULT hr = ID3D11DeviceContext_Map(d->context, dst, layer, D3D11_MAP_READ, 0, &m);
        if (FAILED(hr)) { say("Direct3D 11: Map failed (0x%08lx)", (unsigned long)hr); return XR_ERROR_RUNTIME_FAILURE; }
        uint8_t *out = sc->pixels[image] + (size_t)layer * w * h * 4;
        for (uint32_t y = 0; y < h; y++) memcpy(out + (size_t)y * w * 4, (const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
        ID3D11DeviceContext_Unmap(d->context, dst, layer);
    }
    return XR_SUCCESS;
}
