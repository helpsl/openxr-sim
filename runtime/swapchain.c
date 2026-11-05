// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// SWAPCHAINS, whatever the graphics API: the image count, acquire/wait/release, and the copy at release
// (DESIGN.md, "Frames: copied, not shared"). The images themselves are swapchain_vk.c's, swapchain_gl.c's,
// and on Windows swapchain_d3d11.c's and swapchain_d3d12.c's.
//
// THE ORDER OpenXR insists on: an image is acquired, then waited for, then released, oldest first; an
// application may hold several. Waiting never blocks here -- the runtime has finished with an image by
// the time it is released, because the copy is done then and there.
#include "internal.h"

#include <stdlib.h>
#include <string.h>

static const int64_t *formats_of(struct session *s, uint32_t *n)
{
    if (s->gfx == GFX_VULKAN) return vk_formats(s, n);
    if (s->gfx == GFX_GL) return gl_formats(n);
#ifdef _WIN32
    if (s->gfx == GFX_D3D11 || s->gfx == GFX_D3D12) return d3d_formats(n);
#endif
    *n = 0;   // a headless session has nothing to draw into
    return NULL;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_EnumerateSwapchainFormats(XrSession session, uint32_t cap, uint32_t *count, int64_t *out)
{
    struct session *s = (struct session *)find(H(session), MAGIC_SESSION);
    if (s == NULL) return XR_ERROR_HANDLE_INVALID;
    uint32_t n = 0;
    const int64_t *f = formats_of(s, &n);
    TWO_CALL(cap, count, out, f, n);
    return XR_SUCCESS;
}

static void swapchain_fini(struct object *o)
{
    struct swapchain *sc = (struct swapchain *)o;
    if (sc->session->gfx == GFX_VULKAN) vk_swapchain_fini(sc);
    if (sc->session->gfx == GFX_GL) gl_swapchain_fini(sc);
#ifdef _WIN32
    if (sc->session->gfx == GFX_D3D11) d3d11_swapchain_fini(sc);
    if (sc->session->gfx == GFX_D3D12) d3d12_swapchain_fini(sc);
#endif
}

// Each API's own part of a swapchain, by the session's API.
static XrResult api_init(struct swapchain *sc)
{
    switch (sc->session->gfx) {
        case GFX_VULKAN: return vk_swapchain_init(sc);
        case GFX_GL: return gl_swapchain_init(sc);
#ifdef _WIN32
        case GFX_D3D11: return d3d11_swapchain_init(sc);
        case GFX_D3D12: return d3d12_swapchain_init(sc);
#endif
        default: return XR_ERROR_FEATURE_UNSUPPORTED;
    }
}

static XrResult api_enumerate(struct swapchain *sc, XrSwapchainImageBaseHeader *images)
{
    switch (sc->session->gfx) {
        case GFX_VULKAN: return vk_enumerate_images(sc, images);
        case GFX_GL: return gl_enumerate_images(sc, images);
#ifdef _WIN32
        case GFX_D3D11: return d3d11_enumerate_images(sc, images);
        case GFX_D3D12: return d3d12_enumerate_images(sc, images);
#endif
        default: return XR_ERROR_FEATURE_UNSUPPORTED;
    }
}

static XrResult api_read(struct swapchain *sc, uint32_t image)
{
    switch (sc->session->gfx) {
        case GFX_VULKAN: return vk_swapchain_read(sc, image);
        case GFX_GL: return gl_swapchain_read(sc, image);
#ifdef _WIN32
        case GFX_D3D11: return d3d11_swapchain_read(sc, image);
        case GFX_D3D12: return d3d12_swapchain_read(sc, image);
#endif
        default: return XR_ERROR_FEATURE_UNSUPPORTED;
    }
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_CreateSwapchain(XrSession session, const XrSwapchainCreateInfo *info, XrSwapchain *out)
{
    struct session *s = (struct session *)find(H(session), MAGIC_SESSION);
    if (s == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || out == NULL || info->type != XR_TYPE_SWAPCHAIN_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (s->gfx == GFX_NONE) return XR_ERROR_FEATURE_UNSUPPORTED;
    uint32_t n = 0;
    const int64_t *f = formats_of(s, &n);
    bool known = false;
    for (uint32_t i = 0; i < n; i++) known |= f[i] == info->format;
    if (!known) return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    if ((info->createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT) != 0) return XR_ERROR_FEATURE_UNSUPPORTED;
    if (info->width == 0 || info->height == 0 || info->width > 4096 || info->height > 4096 || info->arraySize == 0 ||
        info->mipCount == 0 || info->sampleCount != 1 || info->faceCount != 1)
        return XR_ERROR_VALIDATION_FAILURE;
    struct swapchain *sc = make(sizeof *sc, MAGIC_SWAPCHAIN, &s->in->o, &s->o);
    if (sc == NULL) return XR_ERROR_OUT_OF_MEMORY;
    sc->session = s;
    sc->info = *info;
    sc->info.next = NULL;
    sc->count = (info->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0 ? 1 : 3;
    sc->last_released = -1;
    const XrResult r = api_init(sc);
    if (r != XR_SUCCESS) {
        destroy(&sc->o);
        return r;
    }
    sc->o.fini = swapchain_fini;
    say("swapchain %ux%u x%u, format %lld, %s", info->width, info->height, info->arraySize, (long long)info->format,
        sc->color ? "colour" : "depth");
    *out = (XrSwapchain)(uintptr_t)sc;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_DestroySwapchain(XrSwapchain swapchain)
{
    struct object *o = find(H(swapchain), MAGIC_SWAPCHAIN);
    if (o == NULL) return XR_ERROR_HANDLE_INVALID;
    destroy(o);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_EnumerateSwapchainImages(XrSwapchain swapchain, uint32_t cap, uint32_t *count,
                                                                  XrSwapchainImageBaseHeader *images)
{
    struct swapchain *sc = (struct swapchain *)find(H(swapchain), MAGIC_SWAPCHAIN);
    if (sc == NULL) return XR_ERROR_HANDLE_INVALID;
    if (count == NULL) return XR_ERROR_VALIDATION_FAILURE;
    *count = sc->count;
    if (cap == 0) return XR_SUCCESS;
    if (cap < sc->count || images == NULL) return XR_ERROR_SIZE_INSUFFICIENT;
    return api_enumerate(sc, images);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_AcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo *info,
                                                               uint32_t *index)
{
    struct swapchain *sc = (struct swapchain *)find(H(swapchain), MAGIC_SWAPCHAIN);
    if (sc == NULL) return XR_ERROR_HANDLE_INVALID;
    if ((info != NULL && info->type != XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO) || index == NULL) return XR_ERROR_VALIDATION_FAILURE;
    if (sc->nacquired == sc->count) return XR_ERROR_CALL_ORDER_INVALID;
    // A static image is drawn once.
    if (sc->count == 1 && (sc->info.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0 && sc->last_released >= 0)
        return XR_ERROR_CALL_ORDER_INVALID;
    *index = sc->next_acquire;
    sc->acquired[sc->nacquired++] = sc->next_acquire;
    sc->next_acquire = (sc->next_acquire + 1) % sc->count;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_WaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo *info)
{
    struct swapchain *sc = (struct swapchain *)find(H(swapchain), MAGIC_SWAPCHAIN);
    if (sc == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || info->type != XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (sc->nwaited >= sc->nacquired) return XR_ERROR_CALL_ORDER_INVALID;
    sc->nwaited++;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_ReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *info)
{
    struct swapchain *sc = (struct swapchain *)find(H(swapchain), MAGIC_SWAPCHAIN);
    if (sc == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info != NULL && info->type != XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (sc->nwaited == 0) return XR_ERROR_CALL_ORDER_INVALID;
    const uint32_t image = sc->acquired[0];
    memmove(sc->acquired, sc->acquired + 1, (sc->nacquired - 1) * sizeof sc->acquired[0]);
    sc->nacquired--;
    sc->nwaited--;
    // THE COPY, while the application's device and context are guaranteed to be ours to use. Only colour,
    // and only when there is a window to show it in.
    if (sc->color && sc->session->device_open) {
        const XrResult r = api_read(sc, image);
        if (r != XR_SUCCESS) say("could not read image %u back: %d", image, (int)r);
    }
    sc->last_released = (int)image;
    return XR_SUCCESS;
}

const struct entry swapchain_functions[] = {
    ENTRY(EnumerateSwapchainFormats) ENTRY(CreateSwapchain) ENTRY(DestroySwapchain) ENTRY(EnumerateSwapchainImages)
    ENTRY(AcquireSwapchainImage) ENTRY(WaitSwapchainImage) ENTRY(ReleaseSwapchainImage)
    { NULL, NULL }
};
