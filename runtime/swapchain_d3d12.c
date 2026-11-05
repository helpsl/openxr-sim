// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// DIRECT3D 12 SWAPCHAINS (XR_KHR_D3D12_enable): committed textures on the application's device, copied at
// release into a readback buffer by a command list of the runtime's own, submitted to the queue the
// application named in its binding, and waited for with a fence of the runtime's own -- then read from the
// mapped buffer (DESIGN.md, "Frames: copied, not shared").
//
// THE STATES XR_KHR_D3D12_enable fixes: a colour image is in RENDER_TARGET when the runtime gives it out and
// must be back in it when the application releases it, a depth image in DEPTH_WRITE. So the copy moves the
// image to COPY_SOURCE and back. Submitting on the application's queue orders the copy after its drawing
// without the runtime knowing anything of the application's own fences.
#include "internal.h"

#include <stdlib.h>
#include <string.h>

struct d3d {
    ID3D12Device *device;
    ID3D12CommandQueue *queue;
    ID3D12CommandAllocator *allocator;
    ID3D12GraphicsCommandList *list;
    ID3D12Fence *fence;
    UINT64 fence_value;
    HANDLE event;
};

// The per-swapchain readback: one buffer holding every layer's first mip, at the footprints the device
// chose (each row 256-byte aligned).
struct readback {
    ID3D12Resource *buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout[16];
    UINT64 size;
};

XrResult d3d12_session_init(struct session *s, const XrGraphicsBindingD3D12KHR *b)
{
    if (b->device == NULL || b->queue == NULL) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    struct d3d *d = calloc(1, sizeof *d);
    if (d == NULL) return XR_ERROR_OUT_OF_MEMORY;
    s->d3d = d;
    d->device = b->device;
    ID3D12Device_AddRef(d->device);
    d->queue = b->queue;
    ID3D12CommandQueue_AddRef(d->queue);
    D3D12_COMMAND_QUEUE_DESC qd;
    ID3D12CommandQueue_GetDesc(d->queue, &qd);
    HRESULT hr = ID3D12Device_CreateCommandAllocator(d->device, qd.Type, &IID_ID3D12CommandAllocator, (void **)&d->allocator);
    if (SUCCEEDED(hr))
        hr = ID3D12Device_CreateCommandList(d->device, 0, qd.Type, d->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&d->list);
    if (SUCCEEDED(hr)) hr = ID3D12GraphicsCommandList_Close(d->list);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateFence(d->device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&d->fence);
    d->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (FAILED(hr) || d->event == NULL) {
        say("Direct3D 12: the runtime's command list or fence could not be made (0x%08lx)", (unsigned long)hr);
        d3d12_session_fini(s);   // CreateSession forgets the API of a session that failed, so it is let go of here
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    // A copy queue cannot take a render target's barriers into and out of COPY_SOURCE from RENDER_TARGET.
    if (qd.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) say("Direct3D 12: the binding's queue is not a direct queue (type %d)", (int)qd.Type);
    say("Direct3D 12: a session on the application's queue");
    return XR_SUCCESS;
}

void d3d12_session_fini(struct session *s)
{
    struct d3d *d = s->d3d;
    if (d == NULL) return;
    // Nothing of ours may still be on the queue: every copy was waited for at its release.
    if (d->event != NULL) CloseHandle(d->event);
    if (d->fence != NULL) ID3D12Fence_Release(d->fence);
    if (d->list != NULL) ID3D12GraphicsCommandList_Release(d->list);
    if (d->allocator != NULL) ID3D12CommandAllocator_Release(d->allocator);
    if (d->queue != NULL) ID3D12CommandQueue_Release(d->queue);
    if (d->device != NULL) ID3D12Device_Release(d->device);
    free(d);
    s->d3d = NULL;
}

XrResult d3d12_swapchain_init(struct swapchain *sc)
{
    struct d3d *d = sc->session->d3d;
    const XrSwapchainUsageFlags u = sc->info.usageFlags;
    sc->color = d3d_is_color(sc->info.format);
    d3d_byte_order(sc);
    if (sc->info.arraySize > 16) return XR_ERROR_VALIDATION_FAILURE;
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    if (sc->color) flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    else flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (u & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (!sc->color && !(u & XR_SWAPCHAIN_USAGE_SAMPLED_BIT)) flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    const bool typeless = !sc->color && (u & XR_SWAPCHAIN_USAGE_SAMPLED_BIT);
    const D3D12_RESOURCE_DESC desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, .Width = sc->info.width, .Height = sc->info.height,
        .DepthOrArraySize = (UINT16)sc->info.arraySize, .MipLevels = (UINT16)sc->info.mipCount,
        .Format = (DXGI_FORMAT)(typeless ? d3d_typeless_depth(sc->info.format) : sc->info.format),
        .SampleDesc = { 1, 0 }, .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN, .Flags = flags,
    };
    const D3D12_HEAP_PROPERTIES heap = { .Type = D3D12_HEAP_TYPE_DEFAULT };
    const D3D12_RESOURCE_STATES state = sc->color ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_DEPTH_WRITE;
    // A clear value the application is likely to use makes its clears the fast kind; any other still works.
    D3D12_CLEAR_VALUE clear = { .Format = (DXGI_FORMAT)sc->info.format };
    if (!sc->color) clear.DepthStencil.Depth = 1.0f;
    for (uint32_t i = 0; i < sc->count; i++) {
        ID3D12Resource *r = NULL;
        const HRESULT hr = ID3D12Device_CreateCommittedResource(d->device, &heap, D3D12_HEAP_FLAG_NONE, &desc, state, &clear,
                                                                &IID_ID3D12Resource, (void **)&r);
        if (FAILED(hr)) { say("Direct3D 12: CreateCommittedResource failed (0x%08lx)", (unsigned long)hr); return XR_ERROR_RUNTIME_FAILURE; }
        sc->d3d_image[i] = r;
    }
    if (!sc->color) return XR_SUCCESS;

    struct readback *rb = calloc(1, sizeof *rb);
    if (rb == NULL) return XR_ERROR_OUT_OF_MEMORY;
    sc->d3d_readback = rb;
    // The footprints of mip 0 of each layer: subresources layer * mipCount.
    for (uint32_t layer = 0; layer < sc->info.arraySize; layer++) {
        UINT64 at = rb->size, bytes = 0;
        ID3D12Device_GetCopyableFootprints(d->device, &desc, layer * sc->info.mipCount, 1, at, &rb->layout[layer], NULL, NULL, &bytes);
        rb->layout[layer].Offset = at;
        rb->size = (at + bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~(UINT64)(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    }
    const D3D12_HEAP_PROPERTIES rheap = { .Type = D3D12_HEAP_TYPE_READBACK };
    const D3D12_RESOURCE_DESC bdesc = { .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = rb->size, .Height = 1,
        .DepthOrArraySize = 1, .MipLevels = 1, .Format = DXGI_FORMAT_UNKNOWN, .SampleDesc = { 1, 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
    const HRESULT hr = ID3D12Device_CreateCommittedResource(d->device, &rheap, D3D12_HEAP_FLAG_NONE, &bdesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                            NULL, &IID_ID3D12Resource, (void **)&rb->buffer);
    if (FAILED(hr)) { say("Direct3D 12: the readback buffer failed (0x%08lx)", (unsigned long)hr); return XR_ERROR_RUNTIME_FAILURE; }
    const size_t bytes = (size_t)sc->info.width * sc->info.height * 4 * sc->info.arraySize;
    for (uint32_t i = 0; i < sc->count; i++)
        if ((sc->pixels[i] = malloc(bytes)) == NULL) return XR_ERROR_OUT_OF_MEMORY;
    return XR_SUCCESS;
}

void d3d12_swapchain_fini(struct swapchain *sc)
{
    for (uint32_t i = 0; i < 4; i++) {
        if (sc->d3d_image[i] != NULL) ID3D12Resource_Release((ID3D12Resource *)sc->d3d_image[i]);
        sc->d3d_image[i] = NULL;
        free(sc->pixels[i]);
        sc->pixels[i] = NULL;
    }
    struct readback *rb = sc->d3d_readback;
    if (rb != NULL) {
        if (rb->buffer != NULL) ID3D12Resource_Release(rb->buffer);
        free(rb);
    }
    sc->d3d_readback = NULL;
}

XrResult d3d12_enumerate_images(struct swapchain *sc, XrSwapchainImageBaseHeader *images)
{
    XrSwapchainImageD3D12KHR *out = (XrSwapchainImageD3D12KHR *)images;
    for (uint32_t i = 0; i < sc->count; i++) {
        if (out[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR) return XR_ERROR_VALIDATION_FAILURE;
        out[i].texture = (ID3D12Resource *)sc->d3d_image[i];
    }
    return XR_SUCCESS;
}

static D3D12_RESOURCE_BARRIER transition(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

XrResult d3d12_swapchain_read(struct swapchain *sc, uint32_t image)
{
    struct d3d *d = sc->session->d3d;
    struct readback *rb = sc->d3d_readback;
    ID3D12Resource *src = (ID3D12Resource *)sc->d3d_image[image];
    HRESULT hr = ID3D12CommandAllocator_Reset(d->allocator);
    if (SUCCEEDED(hr)) hr = ID3D12GraphicsCommandList_Reset(d->list, d->allocator, NULL);
    if (FAILED(hr)) { say("Direct3D 12: the command list could not be reset (0x%08lx)", (unsigned long)hr); return XR_ERROR_RUNTIME_FAILURE; }
    D3D12_RESOURCE_BARRIER b = transition(src, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_ResourceBarrier(d->list, 1, &b);
    for (uint32_t layer = 0; layer < sc->info.arraySize; layer++) {
        const D3D12_TEXTURE_COPY_LOCATION to = { .pResource = rb->buffer, .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
            .PlacedFootprint = rb->layout[layer] };
        const D3D12_TEXTURE_COPY_LOCATION from = { .pResource = src, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = layer * sc->info.mipCount };
        ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &to, 0, 0, 0, &from, NULL);
    }
    b = transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12GraphicsCommandList_ResourceBarrier(d->list, 1, &b);
    hr = ID3D12GraphicsCommandList_Close(d->list);
    if (FAILED(hr)) { say("Direct3D 12: the copy could not be recorded (0x%08lx)", (unsigned long)hr); return XR_ERROR_RUNTIME_FAILURE; }
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)d->list };
    ID3D12CommandQueue_ExecuteCommandLists(d->queue, 1, lists);
    const UINT64 v = ++d->fence_value;
    hr = ID3D12CommandQueue_Signal(d->queue, d->fence, v);
    if (SUCCEEDED(hr) && ID3D12Fence_GetCompletedValue(d->fence) < v) {
        hr = ID3D12Fence_SetEventOnCompletion(d->fence, v, d->event);
        // A device lost would never signal: a second is far longer than any copy of an eye takes.
        if (SUCCEEDED(hr) && WaitForSingleObject(d->event, 1000) != WAIT_OBJECT_0) {
            say("Direct3D 12: the copy did not finish (device removed: 0x%08lx)", (unsigned long)ID3D12Device_GetDeviceRemovedReason(d->device));
            return XR_ERROR_RUNTIME_FAILURE;
        }
    }
    if (FAILED(hr)) { say("Direct3D 12: waiting for the copy failed (0x%08lx)", (unsigned long)hr); return XR_ERROR_RUNTIME_FAILURE; }
    uint8_t *mapped = NULL;
    const D3D12_RANGE whole = { 0, (SIZE_T)rb->size };
    hr = ID3D12Resource_Map(rb->buffer, 0, &whole, (void **)&mapped);
    if (FAILED(hr)) { say("Direct3D 12: Map failed (0x%08lx)", (unsigned long)hr); return XR_ERROR_RUNTIME_FAILURE; }
    const uint32_t w = sc->info.width, h = sc->info.height;
    for (uint32_t layer = 0; layer < sc->info.arraySize; layer++) {
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT *f = &rb->layout[layer];
        uint8_t *out = sc->pixels[image] + (size_t)layer * w * h * 4;
        for (uint32_t y = 0; y < h; y++)
            memcpy(out + (size_t)y * w * 4, mapped + f->Offset + (size_t)y * f->Footprint.RowPitch, (size_t)w * 4);
    }
    const D3D12_RANGE none = { 0, 0 };
    ID3D12Resource_Unmap(rb->buffer, 0, &none);
    return XR_SUCCESS;
}
