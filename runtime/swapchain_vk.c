// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// VULKAN SWAPCHAINS: images on the application's own device, and the copy at release -- a transfer on
// the application's queue into memory the CPU can read, waited for on a fence (DESIGN.md, "Frames:
// copied, not shared"). XR_KHR_vulkan_enable makes the application keep the queue to itself while it
// calls into the runtime, so submitting there is allowed; DXVK under wineopenxr does the same for its
// own queue.
//
// LAYOUTS: OpenXR hands an image over in COLOR_ATTACHMENT_OPTIMAL (DEPTH_STENCIL_ATTACHMENT_OPTIMAL for
// depth) and takes it back in the same, so a new image is moved there once when it is created, and the
// copy moves it to TRANSFER_SRC_OPTIMAL and back.
#include "internal.h"

#include <stdio.h>

#include <stdlib.h>
#include <string.h>

// sRGB first: an application takes the first format it can use, and sRGB is what it should want.
static const VkFormat CANDIDATES[] = {
    VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
    VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM, VK_FORMAT_D32_SFLOAT_S8_UINT,
};

static bool is_color(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_B8G8R8A8_UNORM; }
static bool has_stencil(VkFormat f) { return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }

const int64_t *vk_formats(struct session *s, uint32_t *n)
{
    *n = s->vk.nformats;
    return s->vk.formats;
}

XrResult vk_session_init(struct session *s, const XrGraphicsBindingVulkanKHR *b)
{
    struct vk *vk = &s->vk;
    if (b->instance == VK_NULL_HANDLE || b->physicalDevice == VK_NULL_HANDLE || b->device == VK_NULL_HANDLE) {
        fprintf(stderr, "openxr-sim: xrCreateSession: the Vulkan binding names no instance, physical device or device\n");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    vk->gipa = vk_loader(s->in);
    if (vk->gipa == NULL) return XR_ERROR_RUNTIME_FAILURE;
    vk->instance = b->instance;
    vk->physical = b->physicalDevice;
    vk->device = b->device;
    vk->family = b->queueFamilyIndex;
    vk->gdpa = (PFN_vkGetDeviceProcAddr)vk->gipa(vk->instance, "vkGetDeviceProcAddr");
    PFN_vkGetPhysicalDeviceMemoryProperties memprops =
        (PFN_vkGetPhysicalDeviceMemoryProperties)vk->gipa(vk->instance, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkGetPhysicalDeviceFormatProperties fmtprops =
        (PFN_vkGetPhysicalDeviceFormatProperties)vk->gipa(vk->instance, "vkGetPhysicalDeviceFormatProperties");
    if (vk->gdpa == NULL || memprops == NULL || fmtprops == NULL) return XR_ERROR_RUNTIME_FAILURE;
#define VK_FN(name)                                                                                   \
    vk->name = (PFN_##name)vk->gdpa(vk->device, #name);                                               \
    if (vk->name == NULL) { say("Vulkan: no %s", #name); return XR_ERROR_RUNTIME_FAILURE; }
#include "vk_functions.h"
#undef VK_FN
    memprops(vk->physical, &vk->memory);
    vk->vkGetDeviceQueue(vk->device, b->queueFamilyIndex, b->queueIndex, &vk->queue);
    if (vk->queue == VK_NULL_HANDLE) {
        fprintf(stderr, "openxr-sim: xrCreateSession: the device has no queue %u in family %u\n", b->queueIndex, b->queueFamilyIndex);
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }

    // WHAT THIS DEVICE CAN DO with each candidate: draw into it and copy from it, or use it as depth.
    vk->nformats = 0;
    for (size_t i = 0; i < sizeof CANDIDATES / sizeof CANDIDATES[0]; i++) {
        VkFormatProperties p;
        fmtprops(vk->physical, CANDIDATES[i], &p);
        const VkFormatFeatureFlags need = is_color(CANDIDATES[i])
            ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT
            : VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if ((p.optimalTilingFeatures & need) == need) vk->formats[vk->nformats++] = CANDIDATES[i];
    }

    const VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = vk->family,
    };
    if (vk->vkCreateCommandPool(vk->device, &pci, NULL, &vk->pool) != VK_SUCCESS) return XR_ERROR_RUNTIME_FAILURE;
    const VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = vk->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vk->vkAllocateCommandBuffers(vk->device, &cai, &vk->cmd) != VK_SUCCESS) return XR_ERROR_RUNTIME_FAILURE;
    const VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vk->vkCreateFence(vk->device, &fci, NULL, &vk->fence) != VK_SUCCESS) return XR_ERROR_RUNTIME_FAILURE;
    return XR_SUCCESS;
}

void vk_session_fini(struct session *s)
{
    struct vk *vk = &s->vk;
    if (vk->device == VK_NULL_HANDLE || vk->vkDestroyFence == NULL) return;
    if (vk->fence != VK_NULL_HANDLE) vk->vkDestroyFence(vk->device, vk->fence, NULL);
    if (vk->pool != VK_NULL_HANDLE) vk->vkDestroyCommandPool(vk->device, vk->pool, NULL);   // frees vk->cmd
    vk->fence = VK_NULL_HANDLE;
    vk->pool = VK_NULL_HANDLE;
}

static int memory_type(const struct vk *vk, uint32_t allowed, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < vk->memory.memoryTypeCount; i++)
        if ((allowed & (1u << i)) != 0 && (vk->memory.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

// One command buffer, run to completion.
static XrResult run(struct vk *vk, void (*record)(struct vk *, void *), void *arg)
{
    vk->vkResetFences(vk->device, 1, &vk->fence);
    vk->vkResetCommandBuffer(vk->cmd, 0);
    const VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vk->vkBeginCommandBuffer(vk->cmd, &bi) != VK_SUCCESS) return XR_ERROR_RUNTIME_FAILURE;
    record(vk, arg);
    if (vk->vkEndCommandBuffer(vk->cmd) != VK_SUCCESS) return XR_ERROR_RUNTIME_FAILURE;
    const VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &vk->cmd };
    if (vk->vkQueueSubmit(vk->queue, 1, &si, vk->fence) != VK_SUCCESS) return XR_ERROR_RUNTIME_FAILURE;
    if (vk->vkWaitForFences(vk->device, 1, &vk->fence, VK_TRUE, 2000000000ull) != VK_SUCCESS) return XR_ERROR_RUNTIME_FAILURE;
    return XR_SUCCESS;
}

static VkImageAspectFlags aspect_of(const struct swapchain *sc)
{
    const VkFormat f = (VkFormat)sc->info.format;
    if (is_color(f)) return VK_IMAGE_ASPECT_COLOR_BIT;
    return VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil(f) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}

static void record_initial(struct vk *vk, void *arg)
{
    struct swapchain *sc = arg;
    for (uint32_t i = 0; i < sc->count; i++) {
        const VkImageMemoryBarrier b = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = sc->vk_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = sc->vk_image[i],
            .subresourceRange = { aspect_of(sc), 0, sc->info.mipCount, 0, sc->info.arraySize },
        };
        vk->vkCmdPipelineBarrier(vk->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                 0, NULL, 0, NULL, 1, &b);
    }
}

XrResult vk_swapchain_init(struct swapchain *sc)
{
    struct vk *vk = &sc->session->vk;
    const XrSwapchainUsageFlags u = sc->info.usageFlags;
    const VkFormat format = (VkFormat)sc->info.format;
    sc->color = is_color(format);
    sc->bgra = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
    sc->linear = format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM;
    VkImageUsageFlags usage = 0;
    if (u & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (u & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (u & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (u & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (u & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (u & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (u & XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_KHR) usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    if (sc->color) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;   // the copy
    if (usage == 0) usage = sc->color ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    sc->vk_layout = (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                  : (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                  : VK_IMAGE_LAYOUT_GENERAL;

    for (uint32_t i = 0; i < sc->count; i++) {
        const VkImageCreateInfo ici = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .flags = (u & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = { sc->info.width, sc->info.height, 1 },
            .mipLevels = sc->info.mipCount,
            .arrayLayers = sc->info.arraySize,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        if (vk->vkCreateImage(vk->device, &ici, NULL, &sc->vk_image[i]) != VK_SUCCESS) return XR_ERROR_RUNTIME_FAILURE;
        VkMemoryRequirements req;
        vk->vkGetImageMemoryRequirements(vk->device, sc->vk_image[i], &req);
        int type = memory_type(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type < 0) type = memory_type(vk, req.memoryTypeBits, 0);
        const VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)type };
        if (type < 0 || vk->vkAllocateMemory(vk->device, &ai, NULL, &sc->vk_memory[i]) != VK_SUCCESS ||
            vk->vkBindImageMemory(vk->device, sc->vk_image[i], sc->vk_memory[i], 0) != VK_SUCCESS)
            return XR_ERROR_RUNTIME_FAILURE;
        if (!sc->color) continue;

        // WHERE THE COPY GOES: cached host memory if there is any, since the CPU reads all of it.
        const VkBufferCreateInfo bci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = (VkDeviceSize)sc->info.width * sc->info.height * 4 * sc->info.arraySize,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        if (vk->vkCreateBuffer(vk->device, &bci, NULL, &sc->vk_buffer[i]) != VK_SUCCESS) return XR_ERROR_RUNTIME_FAILURE;
        vk->vkGetBufferMemoryRequirements(vk->device, sc->vk_buffer[i], &req);
        type = memory_type(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (type < 0) type = memory_type(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type < 0) return XR_ERROR_RUNTIME_FAILURE;
        sc->vk_coherent = (vk->memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        const VkMemoryAllocateInfo bai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)type };
        void *mapped = NULL;
        if (vk->vkAllocateMemory(vk->device, &bai, NULL, &sc->vk_buffer_memory[i]) != VK_SUCCESS ||
            vk->vkBindBufferMemory(vk->device, sc->vk_buffer[i], sc->vk_buffer_memory[i], 0) != VK_SUCCESS ||
            vk->vkMapMemory(vk->device, sc->vk_buffer_memory[i], 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
            return XR_ERROR_RUNTIME_FAILURE;
        sc->pixels[i] = mapped;
    }
    return run(vk, record_initial, sc);
}

void vk_swapchain_fini(struct swapchain *sc)
{
    struct vk *vk = &sc->session->vk;
    if (vk->device == VK_NULL_HANDLE || vk->vkDestroyImage == NULL) return;
    for (uint32_t i = 0; i < 4; i++) {
        if (sc->pixels[i] != NULL) vk->vkUnmapMemory(vk->device, sc->vk_buffer_memory[i]);
        if (sc->vk_buffer[i] != VK_NULL_HANDLE) vk->vkDestroyBuffer(vk->device, sc->vk_buffer[i], NULL);
        if (sc->vk_buffer_memory[i] != VK_NULL_HANDLE) vk->vkFreeMemory(vk->device, sc->vk_buffer_memory[i], NULL);
        if (sc->vk_image[i] != VK_NULL_HANDLE) vk->vkDestroyImage(vk->device, sc->vk_image[i], NULL);
        if (sc->vk_memory[i] != VK_NULL_HANDLE) vk->vkFreeMemory(vk->device, sc->vk_memory[i], NULL);
        sc->pixels[i] = NULL;
    }
}

XrResult vk_enumerate_images(struct swapchain *sc, XrSwapchainImageBaseHeader *images)
{
    XrSwapchainImageVulkanKHR *out = (XrSwapchainImageVulkanKHR *)images;
    for (uint32_t i = 0; i < sc->count; i++) {
        if (out[i].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) return XR_ERROR_VALIDATION_FAILURE;
        out[i].image = sc->vk_image[i];
    }
    return XR_SUCCESS;
}

struct copy { struct swapchain *sc; uint32_t image; };

static void record_copy(struct vk *vk, void *arg)
{
    const struct copy *c = arg;
    struct swapchain *sc = c->sc;
    const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, sc->info.arraySize };
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = sc->vk_layout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = sc->vk_image[c->image],
        .subresourceRange = range,
    };
    vk->vkCmdPipelineBarrier(vk->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    const VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, sc->info.arraySize },
        .imageExtent = { sc->info.width, sc->info.height, 1 },
    };
    vk->vkCmdCopyImageToBuffer(vk->cmd, b.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc->vk_buffer[c->image], 1, &region);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = sc->vk_layout;
    const VkBufferMemoryBarrier host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = sc->vk_buffer[c->image],
        .size = VK_WHOLE_SIZE,
    };
    vk->vkCmdPipelineBarrier(vk->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &host, 1, &b);
}

XrResult vk_swapchain_read(struct swapchain *sc, uint32_t image)
{
    struct vk *vk = &sc->session->vk;
    struct copy c = { sc, image };
    const XrResult r = run(vk, record_copy, &c);
    if (r == XR_SUCCESS && !sc->vk_coherent) {
        const VkMappedMemoryRange range = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = sc->vk_buffer_memory[image], .size = VK_WHOLE_SIZE };
        vk->vkInvalidateMappedMemoryRanges(vk->device, 1, &range);
    }
    return r;
}
