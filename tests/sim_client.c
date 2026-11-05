// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// AN OPENXR APPLICATION FOR THE SIMULATED HEADSET'S TEST (tests/sim-test.sh). Through the real loader, on
// whatever Vulkan device there is (lavapipe will do): a session run to FOCUSED, two eyes drawn in known
// colours -- the left red, the right green, each with a blue square at its top left so a flipped or
// swapped picture shows -- touch-controller bindings, and the simulator's scripted move read back (the
// head turned 30 degrees left, the right trigger and A held). Then the session is ended as an
// application ends one, and the simulator writes what it saw. Exit 0 when everything this side held.
//
// --gl: the same in desktop OpenGL instead, on an EGL surfaceless context through XR_MNDX_egl_enable --
// the runtime's other readback, whose rows come back bottom to top.
// --gl-win32: that context again, but named only by an XrGraphicsBindingOpenGLWin32KHR with no handles in
// it -- what a Windows OpenGL program's session looks like by the time wineopenxr hands it on: the
// runtime must find the context current on the thread itself.
// --layers (with either API): composition layers over the eyes, placed for the frame after the scripted
// head turn -- a yellow quad in STAGE straight ahead of the turned head, a magenta cylinder above the
// centre for the left eye only, a cyan equirect below it for the right eye only, and a half-transparent
// white quad at the bottom left, unpremultiplied. Each layer's image has the blue square at its top left
// too, so a layer drawn mirrored or upside down shows. tests/sim-test.sh checks where each one landed.
// --hands: XR_EXT_hand_tracking -- both hands' joints located once the scripted move has arrived, and
// checked against the grip poses: each bone along its joint's -Z, the back of each hand outwards, the
// thumbs on the inside, the right index curled by the trigger and the right thumb down for A.
// --hand-interaction: XR_EXT_hand_interaction -- only its profile suggested, so the controllers claim to
// be hands: the right pinch's value is the scripted trigger and its ready_ext true, and each hand's pinch
// and poke poses located against the grip (and, with --hands, against the open left hand's joints). Without
// it, the extension's profile and paths are refused.
//
// ON WINDOWS (2026-11-05): Vulkan only, and with NO LOADER -- the client opens openxr_sim.dll itself
// (OPENXR_SIM_RUNTIME, else the one beside it), negotiates with it as the loader would, and takes every
// OpenXR function from it, and every Vulkan one from vulkan-1.dll. What is tested is the runtime and the
// simulator, which is all a loader would have handed over to.
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_OPENGL
#ifdef _WIN32
#define OXS_DIRECT 1
#define XR_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <windows.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>
// XR_MNDX_egl_enable's binding, mirrored: there is no EGL here, and --gl is refused.
typedef struct { XrStructureType type; const void *next; void *getProcAddress, *display, *config, *context; } XrGraphicsBindingEGLMNDX;
#define XR_MNDX_EGL_ENABLE_EXTENSION_NAME "XR_MNDX_egl_enable"
#else
#define XR_USE_PLATFORM_EGL
#include <vulkan/vulkan.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#endif

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef OXS_DIRECT
#define XR_FUNCTIONS(X) X(xrAcquireSwapchainImage) X(xrAttachSessionActionSets) X(xrBeginFrame) X(xrBeginSession) \
    X(xrCreateAction) X(xrCreateActionSet) X(xrCreateActionSpace) X(xrCreateInstance) X(xrCreateReferenceSpace) \
    X(xrCreateSession) X(xrCreateSwapchain) X(xrDestroyInstance) X(xrDestroySession) X(xrDestroySwapchain) X(xrEndFrame) \
    X(xrEndSession) X(xrEnumerateSwapchainImages) X(xrGetActionStateBoolean) X(xrGetActionStateFloat) \
    X(xrGetCurrentInteractionProfile) X(xrGetSystem) X(xrGetSystemProperties) X(xrLocateSpace) X(xrLocateViews) \
    X(xrPathToString) X(xrPollEvent) X(xrReleaseSwapchainImage) X(xrRequestExitSession) X(xrStringToPath) \
    X(xrSuggestInteractionProfileBindings) X(xrSyncActions) X(xrWaitFrame) X(xrWaitSwapchainImage)
#define VK_FUNCTIONS(X) X(vkAllocateCommandBuffers) X(vkAllocateMemory) X(vkBeginCommandBuffer) X(vkBindBufferMemory) \
    X(vkCmdClearColorImage) X(vkCmdCopyBufferToImage) X(vkCmdPipelineBarrier) X(vkCreateBuffer) X(vkCreateCommandPool) \
    X(vkCreateFence) X(vkDestroyBuffer) X(vkDestroyCommandPool) X(vkDestroyDevice) X(vkDestroyFence) X(vkDestroyInstance) \
    X(vkDeviceWaitIdle) X(vkEndCommandBuffer) X(vkFreeMemory) X(vkGetBufferMemoryRequirements) X(vkGetDeviceQueue) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkMapMemory) X(vkQueueSubmit) X(vkResetCommandBuffer) X(vkResetFences) X(vkWaitForFences)
#define DECLARE(name) static PFN_##name name;
static PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr;
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
XR_FUNCTIONS(DECLARE)
VK_FUNCTIONS(DECLARE)
#undef DECLARE

// THE RUNTIME, as the loader would open it: its one export, xrNegotiateLoaderRuntimeInterface, gives
// xrGetInstanceProcAddr, and that gives everything else -- the three global functions before there is an
// instance, the rest once there is (direct_instance_functions).
static int direct_open(void)
{
    wchar_t path[MAX_PATH];
    const wchar_t *env = _wgetenv(L"OPENXR_SIM_RUNTIME");
    if (env != NULL && env[0] != L'\0') wcsncpy_s(path, MAX_PATH, env, _TRUNCATE);
    else {
        GetModuleFileNameW(NULL, path, MAX_PATH);
        wchar_t *slash = wcsrchr(path, L'\\');
        wcscpy_s(slash != NULL ? slash + 1 : path, MAX_PATH - (slash != NULL ? (size_t)(slash + 1 - path) : 0), L"openxr_sim.dll");
    }
    HMODULE rt = LoadLibraryW(path);
    if (rt == NULL) { printf("  could not load %ls (error %lu)\n", path, GetLastError()); return 1; }
    PFN_xrNegotiateLoaderRuntimeInterface negotiate =
        (PFN_xrNegotiateLoaderRuntimeInterface)(void *)GetProcAddress(rt, "xrNegotiateLoaderRuntimeInterface");
    if (negotiate == NULL) { printf("  %ls exports no xrNegotiateLoaderRuntimeInterface\n", path); return 1; }
    XrNegotiateLoaderInfo li = { .structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, .structVersion = XR_LOADER_INFO_STRUCT_VERSION,
        .structSize = sizeof li, .minInterfaceVersion = 1, .maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION,
        .minApiVersion = XR_MAKE_VERSION(1, 0, 0), .maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff) };
    XrNegotiateRuntimeRequest rq = { .structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST,
        .structVersion = XR_RUNTIME_INFO_STRUCT_VERSION, .structSize = sizeof rq };
    XrResult r = negotiate(&li, &rq);
    if (XR_FAILED(r) || rq.getInstanceProcAddr == NULL) { printf("  negotiation failed: %d\n", r); return 1; }
    xrGetInstanceProcAddr = rq.getInstanceProcAddr;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&xrCreateInstance);
    if (xrCreateInstance == NULL) { printf("  the runtime gives no xrCreateInstance\n"); return 1; }
    HMODULE vk = LoadLibraryW(L"vulkan-1.dll");
    if (vk != NULL) vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(vk, "vkGetInstanceProcAddr");
    return 0;
}

static int direct_instance_functions(XrInstance instance)
{
    int missing = 0;
#define LOAD(name) if (XR_FAILED(xrGetInstanceProcAddr(instance, #name, (PFN_xrVoidFunction *)&name))) { printf("  no %s\n", #name); missing++; }
    XR_FUNCTIONS(LOAD)
#undef LOAD
    return missing;
}

static int direct_vulkan_functions(VkInstance instance)
{
    int missing = 0;
#define LOAD(name) if ((name = (PFN_##name)vkGetInstanceProcAddr(instance, #name)) == NULL) { printf("  no %s\n", #name); missing++; }
    VK_FUNCTIONS(LOAD)
#undef LOAD
    return missing;
}
#endif

#define EYE 256
#define SQUARE 64

static int failures;
static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) failures++;
}
#define XR(call) do { XrResult r_ = (call); if (XR_FAILED(r_)) { printf("  %s: %d\n", #call, r_); return 1; } } while (0)
#define VK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { printf("  %s: %d\n", #call, r_); return 1; } } while (0)

static XrInstance in;
static XrPath path(const char *s) { XrPath p = XR_NULL_PATH; xrStringToPath(in, s, &p); return p; }

static bool gl, win32, layers, hands, hand_interaction;

// XrGraphicsBindingOpenGLWin32KHR, mirrored: openxr_platform.h declares it only with windows.h.
struct binding_win32 { XrStructureType type; const void *next; void *hDC, *hGLRC; };

// DESKTOP GL, the few calls the test makes, through eglGetProcAddress.
typedef unsigned int GLenum_, GLuint_;
#define GL_FRAMEBUFFER_ 0x8D40
#define GL_COLOR_ATTACHMENT0_ 0x8CE0
#define GL_TEXTURE_2D_ 0x0DE1
#define GL_COLOR_BUFFER_BIT_ 0x4000
#define GL_SCISSOR_TEST_ 0x0C11
#define GL_SRGB8_ALPHA8_ 0x8C43
static struct {
    void (*GenFramebuffers)(int, GLuint_ *);
    void (*BindFramebuffer)(GLenum_, GLuint_);
    void (*FramebufferTexture2D)(GLenum_, GLenum_, GLenum_, GLuint_, int);
    void (*ClearColor)(float, float, float, float);
    void (*Clear)(unsigned);
    void (*Scissor)(int, int, int, int);
    void (*Enable)(GLenum_);
    void (*Disable)(GLenum_);
    void (*Finish)(void);
} G;
static GLuint_ fbo;

// GL's rows run bottom to top, so the square at the picture's top left is at the TOP of the texture's y.
static void draw_gl(GLuint_ texture, int height, float r, float g, float b, float a)
{
    G.BindFramebuffer(GL_FRAMEBUFFER_, fbo);
    G.FramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, texture, 0);
    G.ClearColor(r, g, b, a);
    G.Clear(GL_COLOR_BUFFER_BIT_);
    G.Enable(GL_SCISSOR_TEST_);
    G.Scissor(0, height - SQUARE, SQUARE, SQUARE);
    G.ClearColor(0, 0, 1, 1);
    G.Clear(GL_COLOR_BUFFER_BIT_);
    G.Disable(GL_SCISSOR_TEST_);
    G.BindFramebuffer(GL_FRAMEBUFFER_, 0);
    G.Finish();
}

static int setup_gl(XrSystemId sys, XrGraphicsBindingEGLMNDX *binding)
{
#ifdef OXS_DIRECT
    (void)sys; (void)binding;
    printf("  --gl needs EGL, which Windows has not\n");
    return 1;
#else
    PFN_xrGetOpenGLGraphicsRequirementsKHR reqs;
    XR(xrGetInstanceProcAddr(in, "xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&reqs));
    XrGraphicsRequirementsOpenGLKHR gr = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
    XR(reqs(in, sys, &gr));
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay d = platform_display != NULL ? platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, NULL, NULL)) { printf("  no surfaceless EGL display\n"); return 1; }
    eglBindAPI(EGL_OPENGL_API);
    const EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(d, ca, &cfg, 1, &n) || n == 0) { printf("  no EGL config\n"); return 1; }
    const EGLint xa[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                          EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
    EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa);
    if (c == EGL_NO_CONTEXT || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) { printf("  no GL context\n"); return 1; }
    printf("  GL: %s\n", ((const char *(*)(GLenum_))eglGetProcAddress("glGetString"))(0x1F01));
#define L(name) *(void **)&G.name = (void *)eglGetProcAddress("gl" #name)
    L(GenFramebuffers); L(BindFramebuffer); L(FramebufferTexture2D); L(ClearColor); L(Clear); L(Scissor); L(Enable); L(Disable); L(Finish);
#undef L
    G.GenFramebuffers(1, &fbo);
    *binding = (XrGraphicsBindingEGLMNDX){ .type = XR_TYPE_GRAPHICS_BINDING_EGL_MNDX, .getProcAddress = eglGetProcAddress,
        .display = d, .config = cfg, .context = c };
    return 0;
#endif
}

static VkDevice dev;
static VkQueue queue;
static VkCommandBuffer cmd;
static VkFence fence;
static VkBuffer square;

static int draw(VkImage image, float r, float g, float b, float a)
{
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VK(vkResetCommandBuffer(cmd, 0));
    VK(vkBeginCommandBuffer(cmd, &bi));
    const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    // OpenXR hands a Vulkan colour image over in COLOR_ATTACHMENT_OPTIMAL and wants it back that way.
    VkImageMemoryBarrier to = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image, .subresourceRange = range };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to);
    const VkClearColorValue c = { .float32 = { r, g, b, a } };
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    const VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { SQUARE, SQUARE, 1 } };
    vkCmdCopyBufferToImage(cmd, square, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier back = to;
    back.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    back.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &back);
    VK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    VK(vkResetFences(dev, 1, &fence));
    VK(vkQueueSubmit(queue, 1, &si, fence));
    VK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    return 0;
}

// v turned by q.
static XrVector3f rotate(XrQuaternionf q, XrVector3f v)
{
    const float tx = 2 * (q.y * v.z - q.z * v.y), ty = 2 * (q.z * v.x - q.x * v.z), tz = 2 * (q.x * v.y - q.y * v.x);
    return (XrVector3f){ v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz),
                         v.z + q.w * tz + (q.x * ty - q.y * tx) };
}
static XrVector3f minus(XrVector3f a, XrVector3f b) { return (XrVector3f){ a.x - b.x, a.y - b.y, a.z - b.z }; }
static float dot(XrVector3f a, XrVector3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float length(XrVector3f a) { return sqrtf(dot(a, a)); }
// Point p in the frame of `pose`.
static XrVector3f local(XrPosef pose, XrVector3f p)
{
    const XrQuaternionf inv = { -pose.orientation.x, -pose.orientation.y, -pose.orientation.z, pose.orientation.w };
    return rotate(inv, minus(p, pose.position));
}

// The yaw of a quaternion (about +Y, positive to the left), in degrees.
static double yaw_of(XrQuaternionf q)
{
    return atan2(2.0 * (q.w * q.y + q.x * q.z), 1.0 - 2.0 * (q.y * q.y + q.x * q.x)) * 180 / M_PI;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        win32 |= strcmp(argv[i], "--gl-win32") == 0;
        gl |= strcmp(argv[i], "--gl") == 0;
        layers |= strcmp(argv[i], "--layers") == 0;
        hands |= strcmp(argv[i], "--hands") == 0;
        hand_interaction |= strcmp(argv[i], "--hand-interaction") == 0;
    }
    gl |= win32;
    const char *exts[8] = { gl ? XR_KHR_OPENGL_ENABLE_EXTENSION_NAME : XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
    uint32_t nexts = 1;
    if (gl) exts[nexts++] = XR_MNDX_EGL_ENABLE_EXTENSION_NAME;
    if (layers) {
        exts[nexts++] = XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME;
        exts[nexts++] = XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME;
    }
    if (hands) exts[nexts++] = XR_EXT_HAND_TRACKING_EXTENSION_NAME;
    if (hand_interaction) exts[nexts++] = XR_EXT_HAND_INTERACTION_EXTENSION_NAME;
    XrInstanceCreateInfo ci = { .type = XR_TYPE_INSTANCE_CREATE_INFO, .enabledExtensionCount = nexts, .enabledExtensionNames = exts };
    snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "%s%s%s%s",
             win32 ? "sim-test-win32" : gl ? "sim-test-gl" : "sim-test", layers ? "-layers" : "", hands ? "-hands" : "",
             hand_interaction ? "-interaction" : "");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
#ifdef OXS_DIRECT
    if (direct_open() != 0) return 1;
#endif
    XR(xrCreateInstance(&ci, &in));
#ifdef OXS_DIRECT
    if (direct_instance_functions(in) != 0) return 1;
#endif
    XrSystemGetInfo gi = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    XrResult r = xrGetSystem(in, &gi, &sys);
    check(r == XR_SUCCESS, "a headset: the simulated one");
    if (r != XR_SUCCESS) return 1;

    // HAND TRACKING is there only when enabled.
    PFN_xrCreateHandTrackerEXT create_tracker = NULL;
    PFN_xrDestroyHandTrackerEXT destroy_tracker = NULL;
    PFN_xrLocateHandJointsEXT locate_joints = NULL;
    r = xrGetInstanceProcAddr(in, "xrCreateHandTrackerEXT", (PFN_xrVoidFunction *)&create_tracker);
    if (!hands) check(r == XR_ERROR_FUNCTION_UNSUPPORTED, "no hand tracking unless it is enabled");
    else {
        check(r == XR_SUCCESS, "hand tracking's functions");
        XR(xrGetInstanceProcAddr(in, "xrDestroyHandTrackerEXT", (PFN_xrVoidFunction *)&destroy_tracker));
        XR(xrGetInstanceProcAddr(in, "xrLocateHandJointsEXT", (PFN_xrVoidFunction *)&locate_joints));
        XrSystemHandTrackingPropertiesEXT htp = { .type = XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT };
        XrSystemProperties sp = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &htp };
        XR(xrGetSystemProperties(in, sys, &sp));
        check(htp.supportsHandTracking == XR_TRUE, "the system says it tracks hands");
    }

    XrGraphicsBindingEGLMNDX egl_binding;
    XrGraphicsBindingVulkan2KHR gb = { .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR };
    VkInstance vk = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (gl) {
        if (setup_gl(sys, &egl_binding) != 0) return 1;
    } else {
        PFN_xrGetVulkanGraphicsRequirements2KHR reqs;
        PFN_xrCreateVulkanInstanceKHR mk_instance;
        PFN_xrGetVulkanGraphicsDevice2KHR get_device;
        PFN_xrCreateVulkanDeviceKHR mk_device;
        XR(xrGetInstanceProcAddr(in, "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction *)&reqs));
        XR(xrGetInstanceProcAddr(in, "xrCreateVulkanInstanceKHR", (PFN_xrVoidFunction *)&mk_instance));
        XR(xrGetInstanceProcAddr(in, "xrGetVulkanGraphicsDevice2KHR", (PFN_xrVoidFunction *)&get_device));
        XR(xrGetInstanceProcAddr(in, "xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction *)&mk_device));
        XrGraphicsRequirementsVulkan2KHR gr = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
        XR(reqs(in, sys, &gr));

        VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "sim-test", .apiVersion = VK_API_VERSION_1_1 };
        VkInstanceCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
        XrVulkanInstanceCreateInfoKHR xvi = { .type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR, .systemId = sys,
            .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, .vulkanCreateInfo = &vci };
        VkResult vr = VK_SUCCESS;
        XR(mk_instance(in, &xvi, &vk, &vr));
        VK(vr);
#ifdef OXS_DIRECT
        if (direct_vulkan_functions(vk) != 0) return 1;
#endif
        XrVulkanGraphicsDeviceGetInfoKHR dgi = { .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR, .systemId = sys, .vulkanInstance = vk };
        VkPhysicalDevice phys = VK_NULL_HANDLE;
        XR(get_device(in, &dgi, &phys));
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(phys, &pp);
        printf("  Vulkan device: %s\n", pp.deviceName);
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
        VkQueueFamilyProperties qf[16];
        if (nq > 16) nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
        uint32_t family = 0;
        while (family < nq && !(qf[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) family++;
        const float prio = 1;
        VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &prio };
        VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
        XrVulkanDeviceCreateInfoKHR xdi = { .type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR, .systemId = sys,
            .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, .vulkanPhysicalDevice = phys, .vulkanCreateInfo = &dci };
        XR(mk_device(in, &xdi, &dev, &vr));
        VK(vr);
        vkGetDeviceQueue(dev, family, 0, &queue);
        VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family };
        VK(vkCreateCommandPool(dev, &pci, NULL, &pool));
        VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VK(vkAllocateCommandBuffers(dev, &cai, &cmd));
        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VK(vkCreateFence(dev, &fci, NULL, &fence));

        // THE BLUE SQUARE's pixels, sRGB-encoded blue in R8G8B8A8.
        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = SQUARE * SQUARE * 4,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
        VK(vkCreateBuffer(dev, &bci, NULL, &square));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(dev, square, &mr);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        uint32_t type = 0;
        while (type < mp.memoryTypeCount && (!(mr.memoryTypeBits & (1u << type)) ||
               (mp.memoryTypes[type].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) !=
                   (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)))
            type++;
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = type };
        VK(vkAllocateMemory(dev, &mai, NULL, &mem));
        VK(vkBindBufferMemory(dev, square, mem, 0));
        uint8_t *px;
        VK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&px));
        for (int i = 0; i < SQUARE * SQUARE; i++) { px[i * 4] = 0; px[i * 4 + 1] = 0; px[i * 4 + 2] = 255; px[i * 4 + 3] = 255; }
        gb = (XrGraphicsBindingVulkan2KHR){ .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR, .instance = vk, .physicalDevice = phys,
            .device = dev, .queueFamilyIndex = family, .queueIndex = 0 };
    }

    // ACTIONS: the trigger and A on the right hand, as an application suggests them for Touch.
    XrActionSetCreateInfo asi = { .type = XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy(asi.actionSetName, "test");
    strcpy(asi.localizedActionSetName, "Test");
    XrActionSet set;
    XR(xrCreateActionSet(in, &asi, &set));
    XrActionCreateInfo aci = { .type = XR_TYPE_ACTION_CREATE_INFO, .actionType = XR_ACTION_TYPE_FLOAT_INPUT };
    strcpy(aci.actionName, "trigger");
    strcpy(aci.localizedActionName, "Trigger");
    XrAction trigger, abutton, grip;
    XR(xrCreateAction(set, &aci, &trigger));
    aci.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    strcpy(aci.actionName, "a");
    strcpy(aci.localizedActionName, "A");
    XR(xrCreateAction(set, &aci, &abutton));
    // Each hand's grip pose, which --hands compares the joints with.
    const XrPath hand_path[2] = { path("/user/hand/left"), path("/user/hand/right") };
    aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strcpy(aci.actionName, "grip");
    strcpy(aci.localizedActionName, "Grip");
    aci.countSubactionPaths = 2;
    aci.subactionPaths = hand_path;
    XR(xrCreateAction(set, &aci, &grip));
    // The pinch and poke poses (--hand-interaction), each hand's.
    XrAction pinch = XR_NULL_HANDLE, poke = XR_NULL_HANDLE;
    strcpy(aci.actionName, "pinch");
    strcpy(aci.localizedActionName, "Pinch");
    XR(xrCreateAction(set, &aci, &pinch));
    strcpy(aci.actionName, "poke");
    strcpy(aci.localizedActionName, "Poke");
    XR(xrCreateAction(set, &aci, &poke));
    const XrActionSuggestedBinding sb[] = {
        { trigger, path("/user/hand/right/input/trigger/value") },
        { abutton, path("/user/hand/right/input/a/click") },
        { grip, path("/user/hand/left/input/grip/pose") },
        { grip, path("/user/hand/right/input/grip/pose") },
    };
    XrInteractionProfileSuggestedBinding ipsb = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
        .interactionProfile = path("/interaction_profiles/oculus/touch_controller"), .countSuggestedBindings = 4, .suggestedBindings = sb };
    // A HAND'S PINCH IN PLACE OF THE TRIGGER, ITS READINESS IN PLACE OF A: the hand interaction profile alone,
    // so it is the one chosen -- with a controller's profile suggested too, the controller's would be.
    const XrActionSuggestedBinding hb[] = {
        { trigger, path("/user/hand/right/input/pinch_ext/value") },
        { abutton, path("/user/hand/right/input/pinch_ext/ready_ext") },
        { grip, path("/user/hand/left/input/grip/pose") },
        { grip, path("/user/hand/right/input/grip/pose") },
        { pinch, path("/user/hand/left/input/pinch_ext/pose") },
        { pinch, path("/user/hand/right/input/pinch_ext/pose") },
        { poke, path("/user/hand/left/input/poke_ext/pose") },
        { poke, path("/user/hand/right/input/poke_ext/pose") },
    };
    XrInteractionProfileSuggestedBinding hand_ipsb = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
        .interactionProfile = path("/interaction_profiles/ext/hand_interaction_ext"), .countSuggestedBindings = 8, .suggestedBindings = hb };
    // The extension also adds the pinch and poke poses to the controllers' profiles: Daydream's here, which
    // is never chosen while a profile the runtime prefers was suggested.
    const XrActionSuggestedBinding db[] = { { poke, path("/user/hand/right/input/poke_ext/pose") } };
    XrInteractionProfileSuggestedBinding daydream = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
        .interactionProfile = path("/interaction_profiles/google/daydream_controller"), .countSuggestedBindings = 1, .suggestedBindings = db };
    r = xrSuggestInteractionProfileBindings(in, &hand_ipsb);
    if (!hand_interaction) {
        check(r == XR_ERROR_PATH_UNSUPPORTED, "no hand interaction profile unless it is enabled");
        check(xrSuggestInteractionProfileBindings(in, &daydream) == XR_ERROR_PATH_UNSUPPORTED, "nor a controller's poke pose");
        XR(xrSuggestInteractionProfileBindings(in, &ipsb));
    } else {
        check(r == XR_SUCCESS, "the hand interaction profile's bindings");
        check(xrSuggestInteractionProfileBindings(in, &daydream) == XR_SUCCESS, "a controller's poke pose");
    }
    const char *const expected_profile = hand_interaction ? "/interaction_profiles/ext/hand_interaction_ext" : "/interaction_profiles/oculus/touch_controller";

    const struct binding_win32 win32_binding = { .type = XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
    XrSessionCreateInfo sci = { .type = XR_TYPE_SESSION_CREATE_INFO,
        .next = win32 ? (const void *)&win32_binding : gl ? (const void *)&egl_binding : (const void *)&gb, .systemId = sys };
    XrSession session;
    XR(xrCreateSession(in, &sci, &session));
    XrSessionActionSetsAttachInfo att = { .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, .countActionSets = 1, .actionSets = &set };
    XR(xrAttachSessionActionSets(session, &att));
    XrReferenceSpaceCreateInfo rsi = { .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE,
        .poseInReferenceSpace = { .orientation = { 0, 0, 0, 1 } } };
    XrSpace stage;
    XR(xrCreateReferenceSpace(session, &rsi, &stage));
    XrSpace grip_space[2], pinch_space[2], poke_space[2];
    XrHandTrackerEXT tracker[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo gsi = { .type = XR_TYPE_ACTION_SPACE_CREATE_INFO, .action = grip, .subactionPath = hand_path[h],
            .poseInActionSpace = { .orientation = { 0, 0, 0, 1 } } };
        XR(xrCreateActionSpace(session, &gsi, &grip_space[h]));
        gsi.action = pinch;
        XR(xrCreateActionSpace(session, &gsi, &pinch_space[h]));
        gsi.action = poke;
        XR(xrCreateActionSpace(session, &gsi, &poke_space[h]));
        if (!hands) continue;
        XrHandTrackerCreateInfoEXT hci = { .type = XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT, .hand = h == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT,
            .handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT };
        XR(create_tracker(session, &hci, &tracker[h]));
    }
    if (hands) {
        XrHandTrackerCreateInfoEXT bad = { .type = XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT, .hand = 0, .handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT };
        XrHandTrackerEXT none;
        check(create_tracker(session, &bad, &none) == XR_ERROR_VALIDATION_FAILURE, "a hand tracker for no hand is refused");
    }
    // The joints and grips as last located once the scripted move had arrived.
    XrHandJointLocationEXT joint[2][XR_HAND_JOINT_COUNT_EXT];
    XrHandJointVelocityEXT joint_velocity[2][XR_HAND_JOINT_COUNT_EXT];
    XrHandJointLocationsEXT joints[2];
    XrSpaceLocation grip_at[2], pinch_at[2], poke_at[2];
    bool located = false, short_refused = false;

    XrSwapchain sc[2];
    XrSwapchainImageVulkan2KHR images[2][4];
    XrSwapchainImageOpenGLKHR gl_images[2][4];
    uint32_t nimages[2];
    for (int e = 0; e < 2; e++) {
        XrSwapchainCreateInfo swci = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
            .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
            .format = gl ? GL_SRGB8_ALPHA8_ : VK_FORMAT_R8G8B8A8_SRGB, .sampleCount = 1, .width = EYE, .height = EYE, .faceCount = 1, .arraySize = 1, .mipCount = 1 };
        XR(xrCreateSwapchain(session, &swci, &sc[e]));
        for (int i = 0; i < 4; i++) {
            images[e][i] = (XrSwapchainImageVulkan2KHR){ .type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR };
            gl_images[e][i] = (XrSwapchainImageOpenGLKHR){ .type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR };
        }
        XR(xrEnumerateSwapchainImages(sc[e], 4, &nimages[e], gl ? (XrSwapchainImageBaseHeader *)gl_images[e] : (XrSwapchainImageBaseHeader *)images[e]));
    }

    // THE LAYERS' IMAGES (--layers): each its colour, alpha, and size; the blue square at its top left.
    enum { QUAD_A, CYLINDER, EQUIRECT, QUAD_B, PANELS };
    static const struct { int w, h; float r, g, b, a; } panel[PANELS] = {
        [QUAD_A] = { 128, 128, 1, 1, 0, 1 }, [CYLINDER] = { 256, 64, 1, 0, 1, 1 },
        [EQUIRECT] = { 256, 128, 0, 1, 1, 1 }, [QUAD_B] = { 128, 64, 1, 1, 1, 0.5f },
    };
    XrSwapchain psc[PANELS];
    XrSwapchainImageVulkan2KHR pimages[PANELS][4];
    XrSwapchainImageOpenGLKHR pgl_images[PANELS][4];
    XrSpace view = XR_NULL_HANDLE;
    if (layers) {
        rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        XR(xrCreateReferenceSpace(session, &rsi, &view));
        for (int p = 0; p < PANELS; p++) {
            XrSwapchainCreateInfo swci = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
                .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
                .format = gl ? GL_SRGB8_ALPHA8_ : VK_FORMAT_R8G8B8A8_SRGB, .sampleCount = 1, .width = panel[p].w,
                .height = panel[p].h, .faceCount = 1, .arraySize = 1, .mipCount = 1 };
            XR(xrCreateSwapchain(session, &swci, &psc[p]));
            for (int i = 0; i < 4; i++) {
                pimages[p][i] = (XrSwapchainImageVulkan2KHR){ .type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR };
                pgl_images[p][i] = (XrSwapchainImageOpenGLKHR){ .type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR };
            }
            uint32_t n = 0;
            XR(xrEnumerateSwapchainImages(psc[p], 4, &n, gl ? (XrSwapchainImageBaseHeader *)pgl_images[p] : (XrSwapchainImageBaseHeader *)pimages[p]));
        }
    }

    bool running = false, focused = false, exiting = false, done = false, saw_trigger = false, saw_a = false, saw_turn = false;
    const char *profile_seen = "";
    char profile[XR_MAX_PATH_LENGTH] = "";
    int frames = 0;
    double yaw = 0;
    while (!done) {
        XrEventDataBuffer ev = { .type = XR_TYPE_EVENT_DATA_BUFFER };
        while (xrPollEvent(in, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const XrSessionState s = ((XrEventDataSessionStateChanged *)&ev)->state;
                if (s == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo sbi = { .type = XR_TYPE_SESSION_BEGIN_INFO, .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO };
                    XR(xrBeginSession(session, &sbi));
                    running = true;
                } else if (s == XR_SESSION_STATE_FOCUSED) {
                    focused = true;
                } else if (s == XR_SESSION_STATE_STOPPING) {
                    XR(xrEndSession(session));
                    running = false;
                } else if (s == XR_SESSION_STATE_EXITING || s == XR_SESSION_STATE_LOSS_PENDING) {
                    done = true;
                }
            } else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
                XrInteractionProfileState ips = { .type = XR_TYPE_INTERACTION_PROFILE_STATE };
                if (xrGetCurrentInteractionProfile(session, path("/user/hand/right"), &ips) == XR_SUCCESS && ips.interactionProfile != XR_NULL_PATH) {
                    uint32_t n = 0;
                    xrPathToString(in, ips.interactionProfile, sizeof profile, &n, profile);
                    profile_seen = profile;
                }
            }
            ev = (XrEventDataBuffer){ .type = XR_TYPE_EVENT_DATA_BUFFER };
        }
        if (!running) continue;

        XrFrameState fs = { .type = XR_TYPE_FRAME_STATE };
        XR(xrWaitFrame(session, NULL, &fs));
        XR(xrBeginFrame(session, NULL));
        XrViewLocateInfo vli = { .type = XR_TYPE_VIEW_LOCATE_INFO, .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            .displayTime = fs.predictedDisplayTime, .space = stage };
        XrViewState vs = { .type = XR_TYPE_VIEW_STATE };
        XrView views[2] = { { .type = XR_TYPE_VIEW }, { .type = XR_TYPE_VIEW } };
        uint32_t nv = 0;
        XR(xrLocateViews(session, &vli, &vs, 2, &nv, views));
        yaw = yaw_of(views[0].pose.orientation);
        saw_turn |= fabs(yaw - 30) < 1;

        if (focused) {
            const XrActiveActionSet active = { set, XR_NULL_PATH };
            XrActionsSyncInfo sync = { .type = XR_TYPE_ACTIONS_SYNC_INFO, .countActiveActionSets = 1, .activeActionSets = &active };
            XR(xrSyncActions(session, &sync));
            XrActionStateGetInfo agi = { .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = trigger };
            XrActionStateFloat tf = { .type = XR_TYPE_ACTION_STATE_FLOAT };
            XR(xrGetActionStateFloat(session, &agi, &tf));
            saw_trigger |= tf.isActive && tf.currentState > 0.99f;
            agi.action = abutton;
            XrActionStateBoolean ab = { .type = XR_TYPE_ACTION_STATE_BOOLEAN };
            XR(xrGetActionStateBoolean(session, &agi, &ab));
            saw_a |= ab.isActive && ab.currentState;
        }

        if ((hands || hand_interaction) && saw_trigger && saw_a && saw_turn) {
            for (int h = 0; h < 2; h++) {
                grip_at[h] = (XrSpaceLocation){ .type = XR_TYPE_SPACE_LOCATION };
                XR(xrLocateSpace(grip_space[h], stage, fs.predictedDisplayTime, &grip_at[h]));
                pinch_at[h] = poke_at[h] = (XrSpaceLocation){ .type = XR_TYPE_SPACE_LOCATION };
                XR(xrLocateSpace(pinch_space[h], stage, fs.predictedDisplayTime, &pinch_at[h]));
                XR(xrLocateSpace(poke_space[h], stage, fs.predictedDisplayTime, &poke_at[h]));
                if (!hands) continue;
                const XrHandJointsLocateInfoEXT hli = { .type = XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT, .baseSpace = stage, .time = fs.predictedDisplayTime };
                XrHandJointVelocitiesEXT hv = { .type = XR_TYPE_HAND_JOINT_VELOCITIES_EXT, .jointCount = XR_HAND_JOINT_COUNT_EXT,
                    .jointVelocities = joint_velocity[h] };
                joints[h] = (XrHandJointLocationsEXT){ .type = XR_TYPE_HAND_JOINT_LOCATIONS_EXT, .next = &hv,
                    .jointCount = XR_HAND_JOINT_COUNT_EXT, .jointLocations = joint[h] };
                XR(locate_joints(tracker[h], &hli, &joints[h]));
                joints[h].next = NULL;
                XrHandJointLocationsEXT few = { .type = XR_TYPE_HAND_JOINT_LOCATIONS_EXT, .jointCount = XR_HAND_JOINT_COUNT_EXT - 1,
                    .jointLocations = joint[h] };
                short_refused = locate_joints(tracker[h], &hli, &few) == XR_ERROR_VALIDATION_FAILURE;
            }
            located = true;
        }

        XrCompositionLayerProjectionView pv[2];
        for (int e = 0; e < 2; e++) {
            uint32_t idx = 0;
            XR(xrAcquireSwapchainImage(sc[e], NULL, &idx));
            XrSwapchainImageWaitInfo wi = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION };
            XR(xrWaitSwapchainImage(sc[e], &wi));
            if (gl) draw_gl(gl_images[e][idx].image, EYE, e == 0 ? 1 : 0, e == 0 ? 0 : 1, 0, 1);
            else if (draw(images[e][idx].image, e == 0 ? 1 : 0, e == 0 ? 0 : 1, 0, 1) != 0) return 1;
            XR(xrReleaseSwapchainImage(sc[e], NULL));
            pv[e] = (XrCompositionLayerProjectionView){ .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, .pose = views[e].pose, .fov = views[e].fov,
                .subImage = { .swapchain = sc[e], .imageRect = { { 0, 0 }, { EYE, EYE } } } };
        }
        XrCompositionLayerProjection layer = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = stage, .viewCount = 2, .views = pv };
        const XrCompositionLayerBaseHeader *submitted[1 + PANELS] = { (XrCompositionLayerBaseHeader *)&layer };
        uint32_t nsubmitted = 1;
        XrCompositionLayerQuad quad_a, quad_b;
        XrCompositionLayerCylinderKHR cylinder;
        XrCompositionLayerEquirect2KHR equirect;
        if (layers) {
            XrSwapchainSubImage sub[PANELS];
            for (int p = 0; p < PANELS; p++) {
                uint32_t idx = 0;
                XR(xrAcquireSwapchainImage(psc[p], NULL, &idx));
                XrSwapchainImageWaitInfo wi = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION };
                XR(xrWaitSwapchainImage(psc[p], &wi));
                if (gl) draw_gl(pgl_images[p][idx].image, panel[p].h, panel[p].r, panel[p].g, panel[p].b, panel[p].a);
                else if (draw(pimages[p][idx].image, panel[p].r, panel[p].g, panel[p].b, panel[p].a) != 0) return 1;
                XR(xrReleaseSwapchainImage(psc[p], NULL));
                sub[p] = (XrSwapchainSubImage){ .swapchain = psc[p], .imageRect = { { 0, 0 }, { panel[p].w, panel[p].h } } };
            }
            // A metre square, two metres from the head along where the simulator turns it (30 degrees
            // left), turned to face it: in the middle of both eyes once the turn has arrived.
            const float yaw = 30 * (float)M_PI / 180;
            quad_a = (XrCompositionLayerQuad){ .type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = stage, .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
                .subImage = sub[QUAD_A], .pose = { { 0, sinf(yaw / 2), 0, cosf(yaw / 2) }, { -2 * sinf(yaw), 1.6f, -2 * cosf(yaw) } },
                .size = { 1, 1 } };
            // Above the centre, round the head: a band 0.3 m high, 0.45 to 0.75 m up, 2 m away, 0.6 rad wide.
            cylinder = (XrCompositionLayerCylinderKHR){ .type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR, .space = view,
                .eyeVisibility = XR_EYE_VISIBILITY_LEFT, .subImage = sub[CYLINDER], .pose = { { 0, 0, 0, 1 }, { 0, 0.6f, 0 } },
                .radius = 2, .centralAngle = 0.6f, .aspectRatio = 4 };
            // Below the centre, infinitely far: 0.5 rad wide, from 0.35 to 0.6 rad down.
            equirect = (XrCompositionLayerEquirect2KHR){ .type = XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR, .space = view,
                .eyeVisibility = XR_EYE_VISIBILITY_RIGHT, .subImage = sub[EQUIRECT], .pose = { { 0, 0, 0, 1 }, { 0, 0, 0 } },
                .radius = 0, .centralHorizontalAngle = 0.5f, .upperVerticalAngle = -0.35f, .lowerVerticalAngle = -0.6f };
            // Bottom left, a metre ahead: white at half alpha, not premultiplied.
            quad_b = (XrCompositionLayerQuad){ .type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = view, .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
                .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT,
                .subImage = sub[QUAD_B], .pose = { { 0, 0, 0, 1 }, { -0.55f, -0.5f, -1 } }, .size = { 0.4f, 0.2f } };
            submitted[nsubmitted++] = (XrCompositionLayerBaseHeader *)&quad_a;
            submitted[nsubmitted++] = (XrCompositionLayerBaseHeader *)&cylinder;
            submitted[nsubmitted++] = (XrCompositionLayerBaseHeader *)&equirect;
            submitted[nsubmitted++] = (XrCompositionLayerBaseHeader *)&quad_b;
        }
        XrFrameEndInfo fei = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = fs.predictedDisplayTime,
            .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = fs.shouldRender ? nsubmitted : 0, .layers = submitted };
        XR(xrEndFrame(session, &fei));
        frames++;
        // Done once the scripted move has arrived, or after ten seconds of frames regardless.
        if (!exiting && ((saw_trigger && saw_a && saw_turn && frames > 30) || frames > 720)) {
            XR(xrRequestExitSession(session));
            exiting = true;
        }
    }
    check(focused, "the session reached FOCUSED");
    check(frames > 30, "it drew frames");
    check(strcmp(profile_seen, expected_profile) == 0, hand_interaction ? "the controllers claim to be hands, as suggested" : "the controllers claim to be Touch, as suggested");
    check(saw_turn, "the simulator's head turn (30 degrees left) reached xrLocateViews");
    check(saw_trigger, hand_interaction ? "the right trigger reached the pinch's value" : "the right trigger reached the float action");
    check(saw_a, hand_interaction ? "the right pinch is ready" : "A reached the boolean action");
    printf("  frames %d, last yaw %.1f\n", frames, yaw);

    if (hands) {
        check(located, "the hands were located after the scripted move");
        check(short_refused, "fewer than 26 joints is refused");
        bool active = true, flags = true, radii = true, bones = true, palm_near = true, backs = true, thumbs_in = true;
        const XrSpaceLocationFlags all = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                         XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        XrVector3f index_tip[2], thumb_tip[2];
        for (int h = 0; located && h < 2; h++) {
            const XrHandJointLocationEXT *j = joint[h];
            active &= joints[h].isActive == XR_TRUE && (grip_at[h].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
            for (int i = 0; i < XR_HAND_JOINT_COUNT_EXT; i++) {
                flags &= (j[i].locationFlags & all) == all;
                radii &= j[i].radius >= 0.003f && j[i].radius <= 0.03f;
            }
            // Every bone, metacarpal to tip, runs down its joint's -Z.
            for (int i = XR_HAND_JOINT_THUMB_METACARPAL_EXT; i < XR_HAND_JOINT_COUNT_EXT; i++) {
                const bool tip = i == XR_HAND_JOINT_THUMB_TIP_EXT || i == XR_HAND_JOINT_INDEX_TIP_EXT || i == XR_HAND_JOINT_MIDDLE_TIP_EXT ||
                                 i == XR_HAND_JOINT_RING_TIP_EXT || i == XR_HAND_JOINT_LITTLE_TIP_EXT;
                if (tip) continue;
                const XrVector3f bone = minus(j[i + 1].pose.position, j[i].pose.position);
                const XrVector3f ahead = rotate(j[i].pose.orientation, (XrVector3f){ 0, 0, -1 });
                bones &= dot(bone, ahead) > 0.9f * length(bone);
            }
            const XrPosef palm = j[XR_HAND_JOINT_PALM_EXT].pose;
            palm_near &= length(minus(palm.position, grip_at[h].pose.position)) < 0.06f;
            // The back of the hand (the palm's +Y) is the grip's +X on the right, -X on the left.
            const XrVector3f back = rotate(palm.orientation, (XrVector3f){ 0, 1, 0 });
            const XrVector3f out = rotate(grip_at[h].pose.orientation, (XrVector3f){ h == 0 ? -1.0f : 1.0f, 0, 0 });
            backs &= dot(back, out) > 0.9f;
            index_tip[h] = local(palm, j[XR_HAND_JOINT_INDEX_TIP_EXT].pose.position);
            thumb_tip[h] = local(palm, j[XR_HAND_JOINT_THUMB_TIP_EXT].pose.position);
            // The palm's X is the right-hand rule's, so the thumb is on -X of the right palm, +X of the left.
            thumbs_in &= h == 0 ? thumb_tip[h].x > 0.02f : thumb_tip[h].x < -0.02f;
        }
        // In the room: the open left hand reaches ahead from its wrist (the simulator faces -Z, the head
        // turned only 30 degrees), as a hand held out on a controller does.
        const XrVector3f reach = located ? minus(joint[0][XR_HAND_JOINT_MIDDLE_TIP_EXT].pose.position, joint[0][XR_HAND_JOINT_WRIST_EXT].pose.position)
                                         : (XrVector3f){ 0, 0, 0 };
        check(-reach.z > 0.6f * length(reach) && length(reach) > 0.12f, "the open left hand reaches ahead from its wrist");
        check(active, "both hands active, as their grips are");
        check(flags, "all 26 joints of each located and tracked");
        check(radii, "every joint's radius between 3 mm and 3 cm");
        check(bones, "every bone runs down its joint's -Z");
        check(palm_near, "each palm within 6 cm of its grip");
        check(backs, "the backs of the hands face outwards");
        check(thumbs_in, "the thumbs on the inside of the palms");
        printf("  index tips from the palm: left %.3f, right %.3f m; thumb tips below it: left %.3f, right %.3f m\n",
               length(index_tip[0]), length(index_tip[1]), -thumb_tip[0].y, -thumb_tip[1].y);
        // Closer to the palm, and on its side: a finger bent backwards would only be the first.
        check(located && length(index_tip[1]) < length(index_tip[0]) - 0.02f && index_tip[1].y < -0.02f,
              "the trigger curls the right index into the palm");
        check(located && thumb_tip[1].y < thumb_tip[0].y - 0.005f, "A puts the right thumb down");
        XR(destroy_tracker(tracker[0]));
        const XrHandJointsLocateInfoEXT hli = { .type = XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT, .baseSpace = stage, .time = 1 };
        check(locate_joints(tracker[0], &hli, &joints[0]) == XR_ERROR_HANDLE_INVALID, "a destroyed tracker is gone");
    }

    if (hand_interaction) {
        check(located, "the pinch and poke poses were located after the scripted move");
        const XrSpaceLocationFlags all = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                         XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        bool flags = true, reach = true, poke_up = true, poke_ahead = true, pinch_level = true, pinch_ahead = true;
        XrVector3f pinch_on[2], poke_on[2];
        for (int h = 0; located && h < 2; h++) {
            const XrPosef g = grip_at[h].pose, pi = pinch_at[h].pose, po = poke_at[h].pose;
            flags &= (pinch_at[h].locationFlags & all) == all && (poke_at[h].locationFlags & all) == all;
            pinch_on[h] = local(g, pi.position);
            poke_on[h] = local(g, po.position);
            reach &= length(poke_on[h]) > 0.05f && length(poke_on[h]) < 0.25f && length(pinch_on[h]) > 0.03f && length(pinch_on[h]) < 0.25f;
            // The poke's +Y is the back of the finger (the grip's +X on the right, -X on the left), and its +Z
            // runs back along the bone, so its -Z points away from the hand.
            const XrVector3f out = rotate(g.orientation, (XrVector3f){ h == 0 ? -1.0f : 1.0f, 0, 0 });
            poke_up &= dot(rotate(po.orientation, (XrVector3f){ 0, 1, 0 }), out) > 0.7f;
            const XrVector3f from_grip = minus(po.position, g.position);
            poke_ahead &= dot(rotate(po.orientation, (XrVector3f){ 0, 0, -1 }), from_grip) > 0.5f * length(from_grip);
            // The pinch's +Y is up when the palms face each other: across the back of the hand, not along it.
            pinch_level &= fabsf(dot(rotate(pi.orientation, (XrVector3f){ 0, 1, 0 }), out)) < 0.5f;
            const XrVector3f pinch_from_grip = minus(pi.position, g.position);
            pinch_ahead &= dot(rotate(pi.orientation, (XrVector3f){ 0, 0, -1 }), pinch_from_grip) > 0;
        }
        check(flags, "both hands' pinch and poke located and tracked");
        check(reach, "each a hand's length from its grip");
        check(poke_up, "the poke's +Y the back of the finger");
        check(poke_ahead, "the poke's -Z away from the hand");
        check(pinch_level, "the pinch's +Y across the back of the hand");
        check(pinch_ahead, "the pinch's -Z away from the hand");
        // A fixed offset from the grip, mirrored: the trigger curling the right index does not move it.
        check(located && fabsf(poke_on[0].x + poke_on[1].x) < 0.001f && fabsf(poke_on[0].y - poke_on[1].y) < 0.001f &&
              fabsf(poke_on[0].z - poke_on[1].z) < 0.001f && fabsf(pinch_on[0].x + pinch_on[1].x) < 0.001f &&
              fabsf(pinch_on[0].y - pinch_on[1].y) < 0.001f && fabsf(pinch_on[0].z - pinch_on[1].z) < 0.001f,
              "the left hand's pinch and poke the right's, mirrored");
        printf("  right poke on the grip %.3f %.3f %.3f, pinch %.3f %.3f %.3f m\n", poke_on[1].x, poke_on[1].y, poke_on[1].z,
               pinch_on[1].x, pinch_on[1].y, pinch_on[1].z);
        if (hands && located) {
            // And where the open left hand's joints are: the poke on the index's tip, the pinch between it and the thumb's.
            const XrHandJointLocationEXT *j = joint[0];
            const XrPosef it = j[XR_HAND_JOINT_INDEX_TIP_EXT].pose;
            const XrVector3f ahead = rotate(it.orientation, (XrVector3f){ 0, 0, -j[XR_HAND_JOINT_INDEX_TIP_EXT].radius });
            const XrVector3f surface = { it.position.x + ahead.x, it.position.y + ahead.y, it.position.z + ahead.z };
            const XrVector3f tt = j[XR_HAND_JOINT_THUMB_TIP_EXT].pose.position;
            const XrVector3f between = { (it.position.x + tt.x) / 2, (it.position.y + tt.y) / 2, (it.position.z + tt.z) / 2 };
            const float poke_off = length(minus(poke_at[0].pose.position, surface)), pinch_off = length(minus(pinch_at[0].pose.position, between));
            printf("  left poke %.4f m from the index tip's surface, pinch %.4f m from between the tips\n", poke_off, pinch_off);
            check(poke_off < 0.002f, "the left poke at the open index's tip");
            check(pinch_off < 0.002f, "the left pinch between the open index's and thumb's tips");
        }
    }

    for (int e = 0; e < 2; e++) xrDestroySwapchain(sc[e]);
    for (int p = 0; layers && p < PANELS; p++) xrDestroySwapchain(psc[p]);
    xrDestroySession(session);
    xrDestroyInstance(in);
    if (gl) return failures == 0 ? 0 : 1;
    vkDeviceWaitIdle(dev);
    vkDestroyBuffer(dev, square, NULL);
    vkFreeMemory(dev, mem, NULL);
    vkDestroyFence(dev, fence, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(vk, NULL);
    return failures == 0 ? 0 : 1;
}
