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
// --spatial: XR_EXT_spatial_entity with XR_EXT_spatial_marker_tracking and XR_EXT_spatial_anchor (and
// XR_EXT_future) -- the room's markers as they start (common/xrmath.h, oxs_room_home) discovered before the
// head turn (the QR code, the Micro QR code and the ArUco marker; the AprilTag on the left wall is out of
// view), the discovery recommended once the turn brings the AprilTag into view, and discovered then; an anchor
// made and found again; the Micro QR code, which the simulator takes away with the turn, STOPPED; and the
// errors for configurations, futures and queries that are wrong.
//
// --d3d11, --d3d12 (Windows): the same eyes drawn in Direct3D -- cleared, and the blue square put in by
// UpdateSubresource (11) or a clear of its rectangle (12) -- on the adapter the runtime names.
//
// ON WINDOWS (2026-11-05): Vulkan or Direct3D, and with NO LOADER -- the client opens openxr_sim.dll itself
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
#ifndef COBJMACROS
#define COBJMACROS
#endif
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_2.h>
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
    X(xrCreateSession) X(xrCreateSwapchain) X(xrDestroyInstance) X(xrDestroySession) X(xrDestroySpace) X(xrDestroySwapchain) X(xrEndFrame) \
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

static bool gl, win32, layers, hands, hand_interaction, spatial;
static int d3d;   // 11 or 12 with --d3d11, --d3d12; else 0

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

#ifdef OXS_DIRECT
// ---- DIRECT3D ------------------------------------------------------------------------------------------
static ID3D11Device *d11;
static ID3D11DeviceContext *d11c;
static ID3D12Device *d12;
static ID3D12CommandQueue *d12q;
static ID3D12CommandAllocator *d12a;
static ID3D12GraphicsCommandList *d12l;
static ID3D12Fence *d12f;
static UINT64 d12v;
static HANDLE d12e;
static ID3D12DescriptorHeap *d12h;
#define HR(call) do { HRESULT h_ = (call); if (FAILED(h_)) { printf("  %s: 0x%08lx\n", #call, (unsigned long)h_); return 1; } } while (0)

// The adapter the runtime asked for, by its LUID.
static IDXGIAdapter1 *adapter_of(LUID luid)
{
    IDXGIFactory1 *f = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f))) return NULL;
    IDXGIAdapter1 *a = NULL;
    for (UINT i = 0; IDXGIFactory1_EnumAdapters1(f, i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        IDXGIAdapter1_GetDesc1(a, &d);
        if (d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart) {
            printf("  Direct3D adapter: %ls\n", d.Description);
            break;
        }
        IDXGIAdapter1_Release(a);
        a = NULL;
    }
    IDXGIFactory1_Release(f);
    return a;
}

static int setup_d3d(XrSystemId sys, XrGraphicsBindingD3D11KHR *b11, XrGraphicsBindingD3D12KHR *b12)
{
    LUID luid;
    D3D_FEATURE_LEVEL level;
    if (d3d == 11) {
        PFN_xrGetD3D11GraphicsRequirementsKHR reqs;
        XR(xrGetInstanceProcAddr(in, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction *)&reqs));
        XrGraphicsRequirementsD3D11KHR gr = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
        XR(reqs(in, sys, &gr));
        luid = gr.adapterLuid;
        level = gr.minFeatureLevel;
    } else {
        PFN_xrGetD3D12GraphicsRequirementsKHR reqs;
        XR(xrGetInstanceProcAddr(in, "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction *)&reqs));
        XrGraphicsRequirementsD3D12KHR gr = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
        XR(reqs(in, sys, &gr));
        luid = gr.adapterLuid;
        level = gr.minFeatureLevel;
    }
    IDXGIAdapter1 *adapter = adapter_of(luid);
    if (adapter == NULL) { printf("  no adapter with the runtime's LUID\n"); return 1; }
    if (d3d == 11) {
        HR(D3D11CreateDevice((IDXGIAdapter *)adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, &level, 1, D3D11_SDK_VERSION, &d11, NULL, &d11c));
        *b11 = (XrGraphicsBindingD3D11KHR){ .type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR, .device = d11 };
    } else {
        HR(D3D12CreateDevice((IUnknown *)adapter, level, &IID_ID3D12Device, (void **)&d12));
        const D3D12_COMMAND_QUEUE_DESC qd = { .Type = D3D12_COMMAND_LIST_TYPE_DIRECT };
        HR(ID3D12Device_CreateCommandQueue(d12, &qd, &IID_ID3D12CommandQueue, (void **)&d12q));
        HR(ID3D12Device_CreateCommandAllocator(d12, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&d12a));
        HR(ID3D12Device_CreateCommandList(d12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, d12a, NULL, &IID_ID3D12GraphicsCommandList, (void **)&d12l));
        HR(ID3D12GraphicsCommandList_Close(d12l));
        HR(ID3D12Device_CreateFence(d12, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&d12f));
        d12e = CreateEventW(NULL, FALSE, FALSE, NULL);
        const D3D12_DESCRIPTOR_HEAP_DESC hd = { .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV, .NumDescriptors = 1 };
        HR(ID3D12Device_CreateDescriptorHeap(d12, &hd, &IID_ID3D12DescriptorHeap, (void **)&d12h));
        *b12 = (XrGraphicsBindingD3D12KHR){ .type = XR_TYPE_GRAPHICS_BINDING_D3D12_KHR, .device = d12, .queue = d12q };
    }
    IDXGIAdapter1_Release(adapter);
    return 0;
}

static const D3D11_RENDER_TARGET_VIEW_DESC rtv11 = { .Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D };
static const D3D12_RENDER_TARGET_VIEW_DESC rtv12 = { .Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, .ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D };

// Direct3D's rows run top to bottom, as Vulkan's do: the square is at (0, 0).
static int draw_d3d11(ID3D11Texture2D *t, float r, float g, float b, float a)
{
    ID3D11RenderTargetView *rtv = NULL;
    HR(ID3D11Device_CreateRenderTargetView(d11, (ID3D11Resource *)t, &rtv11, &rtv));
    const float c[4] = { r, g, b, a };
    ID3D11DeviceContext_ClearRenderTargetView(d11c, rtv, c);
    static uint8_t blue[SQUARE * SQUARE * 4];
    for (int i = 0; i < SQUARE * SQUARE; i++) { blue[i * 4 + 2] = 255; blue[i * 4 + 3] = 255; }
    const D3D11_BOX box = { 0, 0, 0, SQUARE, SQUARE, 1 };
    ID3D11DeviceContext_UpdateSubresource(d11c, (ID3D11Resource *)t, 0, &box, blue, SQUARE * 4, 0);
    ID3D11RenderTargetView_Release(rtv);
    return 0;
}

// The image is in RENDER_TARGET, as the runtime gives it out and wants it back (XR_KHR_D3D12_enable).
static int draw_d3d12(ID3D12Resource *t, float r, float g, float b, float a)
{
    HR(ID3D12CommandAllocator_Reset(d12a));
    HR(ID3D12GraphicsCommandList_Reset(d12l, d12a, NULL));
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d12h, &h);
    ID3D12Device_CreateRenderTargetView(d12, t, &rtv12, h);
    const float c[4] = { r, g, b, a }, blue[4] = { 0, 0, 1, 1 };
    ID3D12GraphicsCommandList_ClearRenderTargetView(d12l, h, c, 0, NULL);
    const D3D12_RECT square = { 0, 0, SQUARE, SQUARE };
    ID3D12GraphicsCommandList_ClearRenderTargetView(d12l, h, blue, 1, &square);
    HR(ID3D12GraphicsCommandList_Close(d12l));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)d12l };
    ID3D12CommandQueue_ExecuteCommandLists(d12q, 1, lists);
    HR(ID3D12CommandQueue_Signal(d12q, d12f, ++d12v));
    if (ID3D12Fence_GetCompletedValue(d12f) < d12v) {
        HR(ID3D12Fence_SetEventOnCompletion(d12f, d12v, d12e));
        WaitForSingleObject(d12e, INFINITE);
    }
    return 0;
}
#endif

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

// A SWAPCHAIN'S IMAGES, as whichever API the session has gives them.
struct images {
    XrSwapchainImageVulkan2KHR vk[4];
    XrSwapchainImageOpenGLKHR gl[4];
#ifdef OXS_DIRECT
    XrSwapchainImageD3D11KHR d11[4];
    XrSwapchainImageD3D12KHR d12[4];
#endif
};

static XrSwapchainImageBaseHeader *images_for_enumerate(struct images *im)
{
    for (int i = 0; i < 4; i++) {
        im->vk[i] = (XrSwapchainImageVulkan2KHR){ .type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR };
        im->gl[i] = (XrSwapchainImageOpenGLKHR){ .type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR };
#ifdef OXS_DIRECT
        im->d11[i] = (XrSwapchainImageD3D11KHR){ .type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR };
        im->d12[i] = (XrSwapchainImageD3D12KHR){ .type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR };
#endif
    }
#ifdef OXS_DIRECT
    if (d3d == 11) return (XrSwapchainImageBaseHeader *)im->d11;
    if (d3d == 12) return (XrSwapchainImageBaseHeader *)im->d12;
#endif
    return gl ? (XrSwapchainImageBaseHeader *)im->gl : (XrSwapchainImageBaseHeader *)im->vk;
}

// The sRGB RGBA format, in the session's API.
static int64_t swapchain_format(void)
{
    if (d3d != 0) return 29;   // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
    return gl ? GL_SRGB8_ALPHA8_ : VK_FORMAT_R8G8B8A8_SRGB;
}

static int draw_image(struct images *im, uint32_t idx, int height, float r, float g, float b, float a)
{
#ifdef OXS_DIRECT
    if (d3d == 11) return draw_d3d11(im->d11[idx].texture, r, g, b, a);
    if (d3d == 12) return draw_d3d12(im->d12[idx].texture, r, g, b, a);
#endif
    if (gl) { draw_gl(im->gl[idx].image, height, r, g, b, a); return 0; }
    return draw(im->vk[idx].image, r, g, b, a);
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

// ---- --spatial -------------------------------------------------------------------------------------

// A call that has to succeed, in a function with nothing to return: a failure is counted.
#define SPX(call) check((call) == XR_SUCCESS, #call)

#define SPATIAL_FUNCTIONS(X) X(xrPollFutureEXT) X(xrCancelFutureEXT) X(xrEnumerateSpatialCapabilitiesEXT) \
    X(xrEnumerateSpatialCapabilityComponentTypesEXT) X(xrEnumerateSpatialCapabilityFeaturesEXT) X(xrCreateSpatialContextAsyncEXT) \
    X(xrCreateSpatialContextCompleteEXT) X(xrDestroySpatialContextEXT) X(xrCreateSpatialDiscoverySnapshotAsyncEXT) \
    X(xrCreateSpatialDiscoverySnapshotCompleteEXT) X(xrQuerySpatialComponentDataEXT) X(xrDestroySpatialSnapshotEXT) \
    X(xrCreateSpatialEntityFromIdEXT) X(xrDestroySpatialEntityEXT) X(xrCreateSpatialUpdateSnapshotEXT) \
    X(xrGetSpatialBufferStringEXT) X(xrGetSpatialBufferUint8EXT) X(xrCreateSpatialAnchorEXT)
#define SP_DECLARE(name) static PFN_##name sp_##name;
SPATIAL_FUNCTIONS(SP_DECLARE)
#undef SP_DECLARE

static int spatial_functions(void)
{
#define SP_GET(name) if (xrGetInstanceProcAddr(in, #name, (PFN_xrVoidFunction *)&sp_##name) != XR_SUCCESS) { printf("  no %s\n", #name); return 1; }
    SPATIAL_FUNCTIONS(SP_GET)
#undef SP_GET
    return 0;
}

// What a discovery or an update found: each entity's id, state, marker data, bounds and anchor location.
#define FOUND_MAX 16
struct found {
    uint32_t n;
    XrSpatialEntityIdEXT id[FOUND_MAX];
    XrSpatialEntityTrackingStateEXT state[FOUND_MAX];
    XrSpatialMarkerDataEXT marker[FOUND_MAX];
    XrSpatialBounded2DDataEXT bounds[FOUND_MAX];
    XrPosef anchor[FOUND_MAX];
    char text[FOUND_MAX][64];   // a QR code's
    bool ok;
};

// The snapshot's entities with the components named, and their data: `which` C-style flags, 1 marker,
// 2 bounded, 4 anchor.
static void query(XrSpatialSnapshotEXT snap, unsigned which, struct found *f)
{
    memset(f, 0, sizeof *f);
    XrSpatialComponentTypeEXT types[3];
    uint32_t nt = 0;
    if (which & 1) types[nt++] = XR_SPATIAL_COMPONENT_TYPE_MARKER_EXT;
    if (which & 2) types[nt++] = XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT;
    if (which & 4) types[nt++] = XR_SPATIAL_COMPONENT_TYPE_ANCHOR_EXT;
    const XrSpatialComponentDataQueryConditionEXT cond = { .type = XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_CONDITION_EXT,
        .componentTypeCount = nt, .componentTypes = types };
    XrSpatialComponentDataQueryResultEXT res = { .type = XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_RESULT_EXT };
    if (sp_xrQuerySpatialComponentDataEXT(snap, &cond, &res) != XR_SUCCESS || res.entityIdCountOutput > FOUND_MAX) return;
    const uint32_t n = res.entityIdCountOutput;
    XrSpatialComponentMarkerListEXT ml = { .type = XR_TYPE_SPATIAL_COMPONENT_MARKER_LIST_EXT, .markerCount = n, .markers = f->marker };
    XrSpatialComponentBounded2DListEXT bl = { .type = XR_TYPE_SPATIAL_COMPONENT_BOUNDED_2D_LIST_EXT, .boundCount = n, .bounds = f->bounds };
    XrSpatialComponentAnchorListEXT al = { .type = XR_TYPE_SPATIAL_COMPONENT_ANCHOR_LIST_EXT, .locationCount = n, .locations = f->anchor };
    // What the runtime leaves alone (the data of an entity not TRACKING) stays as this.
    for (uint32_t i = 0; i < n; i++) {
        f->bounds[i].extents.width = -1;
        f->anchor[i].position.x = -999;
    }
    void *chain = NULL;
    if (which & 1) { ml.next = chain; chain = &ml; }
    if (which & 2) { bl.next = chain; chain = &bl; }
    if (which & 4) { al.next = chain; chain = &al; }
    res = (XrSpatialComponentDataQueryResultEXT){ .type = XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_RESULT_EXT, .next = chain,
        .entityIdCapacityInput = n, .entityIds = f->id, .entityStateCapacityInput = n, .entityStates = f->state };
    if (n > 0 && sp_xrQuerySpatialComponentDataEXT(snap, &cond, &res) != XR_SUCCESS) return;
    f->n = n;
    for (uint32_t i = 0; i < n; i++) {
        if (!(which & 1) || f->marker[i].data.bufferType != XR_SPATIAL_BUFFER_TYPE_STRING_EXT) continue;
        const XrSpatialBufferGetInfoEXT bgi = { .type = XR_TYPE_SPATIAL_BUFFER_GET_INFO_EXT, .bufferId = f->marker[i].data.bufferId };
        uint32_t len = 0;
        if (sp_xrGetSpatialBufferStringEXT(snap, &bgi, sizeof f->text[i], &len, f->text[i]) != XR_SUCCESS) return;
    }
    f->ok = true;
}

// A discovery, start to finish: the future polled, then completed.
static XrSpatialSnapshotEXT discover(XrSpatialContextEXT ctx, XrSpace base, XrTime t)
{
    const XrSpatialDiscoverySnapshotCreateInfoEXT dci = { .type = XR_TYPE_SPATIAL_DISCOVERY_SNAPSHOT_CREATE_INFO_EXT };
    XrFutureEXT fut = XR_NULL_FUTURE_EXT;
    if (sp_xrCreateSpatialDiscoverySnapshotAsyncEXT(ctx, &dci, &fut) != XR_SUCCESS) return XR_NULL_HANDLE;
    const XrFuturePollInfoEXT pi = { .type = XR_TYPE_FUTURE_POLL_INFO_EXT, .future = fut };
    XrFuturePollResultEXT pr = { .type = XR_TYPE_FUTURE_POLL_RESULT_EXT };
    if (sp_xrPollFutureEXT(in, &pi, &pr) != XR_SUCCESS || pr.state != XR_FUTURE_STATE_READY_EXT) return XR_NULL_HANDLE;
    const XrCreateSpatialDiscoverySnapshotCompletionInfoEXT cci = { .type = XR_TYPE_CREATE_SPATIAL_DISCOVERY_SNAPSHOT_COMPLETION_INFO_EXT,
        .baseSpace = base, .time = t, .future = fut };
    XrCreateSpatialDiscoverySnapshotCompletionEXT cc = { .type = XR_TYPE_CREATE_SPATIAL_DISCOVERY_SNAPSHOT_COMPLETION_EXT };
    if (sp_xrCreateSpatialDiscoverySnapshotCompleteEXT(ctx, &cci, &cc) != XR_SUCCESS || cc.futureResult != XR_SUCCESS) return XR_NULL_HANDLE;
    return cc.snapshot;
}

static int index_of(const struct found *f, XrSpatialCapabilityEXT cap)
{
    for (uint32_t i = 0; i < f->n; i++)
        if (f->marker[i].capability == cap) return (int)i;
    return -1;
}

static int index_of_id(const struct found *f, XrSpatialEntityIdEXT id)
{
    for (uint32_t i = 0; i < f->n; i++)
        if (f->id[i] == id) return (int)i;
    return -1;
}

static bool near3(XrVector3f a, float x, float y, float z) { return fabsf(a.x - x) < 0.001f && fabsf(a.y - y) < 0.001f && fabsf(a.z - z) < 0.001f; }

// The capabilities and what is wrong with a configuration, before the context the test uses is made.
static void spatial_capabilities(XrSystemId sys, XrSession session)
{
    XrSpatialCapabilityEXT caps[8];
    uint32_t n = 0;
    check(sp_xrEnumerateSpatialCapabilitiesEXT(in, sys, 8, &n, caps) == XR_SUCCESS && n == 5, "five capabilities: four kinds of marker and anchors");
    XrSpatialComponentTypeEXT comps[4];
    XrSpatialCapabilityComponentTypesEXT ct = { .type = XR_TYPE_SPATIAL_CAPABILITY_COMPONENT_TYPES_EXT, .componentTypeCapacityInput = 4, .componentTypes = comps };
    check(sp_xrEnumerateSpatialCapabilityComponentTypesEXT(in, sys, XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT, &ct) == XR_SUCCESS &&
          ct.componentTypeCountOutput == 2, "a QR code has the marker and bounded 2D components");
    check(sp_xrEnumerateSpatialCapabilityComponentTypesEXT(in, sys, XR_SPATIAL_CAPABILITY_ANCHOR_EXT, &ct) == XR_SUCCESS &&
          ct.componentTypeCountOutput == 1 && comps[0] == XR_SPATIAL_COMPONENT_TYPE_ANCHOR_EXT, "an anchor has the anchor component");
    check(sp_xrEnumerateSpatialCapabilityComponentTypesEXT(in, sys, XR_SPATIAL_CAPABILITY_PLANE_TRACKING_EXT, &ct) ==
          XR_ERROR_SPATIAL_CAPABILITY_UNSUPPORTED_EXT, "no plane tracking");

    const XrSpatialComponentTypeEXT marker_comps[] = { XR_SPATIAL_COMPONENT_TYPE_MARKER_EXT, XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT };
    const XrSpatialComponentTypeEXT anchor_comp[] = { XR_SPATIAL_COMPONENT_TYPE_ANCHOR_EXT };
    XrSpatialCapabilityConfigurationQrCodeEXT qr = { .type = XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_QR_CODE_EXT,
        .capability = XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT, .enabledComponentCount = 2, .enabledComponents = marker_comps };
    XrSpatialCapabilityConfigurationQrCodeEXT qr_anchor = qr;
    qr_anchor.enabledComponentCount = 1;
    qr_anchor.enabledComponents = anchor_comp;
    XrSpatialCapabilityConfigurationPlaneTrackingEXT plane = { .type = XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_PLANE_TRACKING_EXT,
        .capability = XR_SPATIAL_CAPABILITY_PLANE_TRACKING_EXT, .enabledComponentCount = 1, .enabledComponents = marker_comps + 1 };
    XrSpatialCapabilityConfigurationArucoMarkerEXT aruco_bad = { .type = XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_ARUCO_MARKER_EXT,
        .capability = XR_SPATIAL_CAPABILITY_MARKER_TRACKING_ARUCO_MARKER_EXT, .enabledComponentCount = 2, .enabledComponents = marker_comps,
        .arUcoDict = 99 };
    const XrSpatialCapabilityConfigurationBaseHeaderEXT *twice[] = { (const void *)&qr, (const void *)&qr };
    const XrSpatialCapabilityConfigurationBaseHeaderEXT *wrong_comp[] = { (const void *)&qr_anchor };
    const XrSpatialCapabilityConfigurationBaseHeaderEXT *planes[] = { (const void *)&plane };
    const XrSpatialCapabilityConfigurationBaseHeaderEXT *bad_dict[] = { (const void *)&aruco_bad };
    XrSpatialContextCreateInfoEXT sci = { .type = XR_TYPE_SPATIAL_CONTEXT_CREATE_INFO_EXT };
    XrFutureEXT fut;
    check(sp_xrCreateSpatialContextAsyncEXT(session, &sci, &fut) == XR_ERROR_SPATIAL_CAPABILITY_CONFIGURATION_INVALID_EXT, "a context of no capability is refused");
    sci.capabilityConfigCount = 2; sci.capabilityConfigs = twice;
    check(sp_xrCreateSpatialContextAsyncEXT(session, &sci, &fut) == XR_ERROR_SPATIAL_CAPABILITY_CONFIGURATION_INVALID_EXT, "a capability twice is refused");
    sci.capabilityConfigCount = 1; sci.capabilityConfigs = wrong_comp;
    check(sp_xrCreateSpatialContextAsyncEXT(session, &sci, &fut) == XR_ERROR_SPATIAL_COMPONENT_UNSUPPORTED_FOR_CAPABILITY_EXT, "a QR code with an anchor component is refused");
    sci.capabilityConfigs = planes;
    check(sp_xrCreateSpatialContextAsyncEXT(session, &sci, &fut) == XR_ERROR_SPATIAL_CAPABILITY_UNSUPPORTED_EXT, "plane tracking is refused");
    sci.capabilityConfigs = bad_dict;
    check(sp_xrCreateSpatialContextAsyncEXT(session, &sci, &fut) == XR_ERROR_VALIDATION_FAILURE, "an ArUco dictionary that is none is refused");
}

// The context the test uses: every marker kind (ArUco 4x4_50, AprilTag 36h11) and anchors.
static XrSpatialContextEXT spatial_context(XrSession session)
{
    const XrSpatialComponentTypeEXT marker_comps[] = { XR_SPATIAL_COMPONENT_TYPE_MARKER_EXT, XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT };
    const XrSpatialComponentTypeEXT anchor_comp[] = { XR_SPATIAL_COMPONENT_TYPE_ANCHOR_EXT };
    const XrSpatialCapabilityConfigurationQrCodeEXT qr = { .type = XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_QR_CODE_EXT,
        .capability = XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT, .enabledComponentCount = 2, .enabledComponents = marker_comps };
    const XrSpatialCapabilityConfigurationMicroQrCodeEXT micro = { .type = XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_MICRO_QR_CODE_EXT,
        .capability = XR_SPATIAL_CAPABILITY_MARKER_TRACKING_MICRO_QR_CODE_EXT, .enabledComponentCount = 2, .enabledComponents = marker_comps };
    const XrSpatialCapabilityConfigurationArucoMarkerEXT aruco = { .type = XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_ARUCO_MARKER_EXT,
        .capability = XR_SPATIAL_CAPABILITY_MARKER_TRACKING_ARUCO_MARKER_EXT, .enabledComponentCount = 2, .enabledComponents = marker_comps,
        .arUcoDict = XR_SPATIAL_MARKER_ARUCO_DICT_4X4_50_EXT };
    const XrSpatialCapabilityConfigurationAprilTagEXT april = { .type = XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_APRIL_TAG_EXT,
        .capability = XR_SPATIAL_CAPABILITY_MARKER_TRACKING_APRIL_TAG_EXT, .enabledComponentCount = 2, .enabledComponents = marker_comps,
        .aprilDict = XR_SPATIAL_MARKER_APRIL_TAG_DICT_36H11_EXT };
    const XrSpatialCapabilityConfigurationAnchorEXT anchor = { .type = XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_ANCHOR_EXT,
        .capability = XR_SPATIAL_CAPABILITY_ANCHOR_EXT, .enabledComponentCount = 1, .enabledComponents = anchor_comp };
    const XrSpatialCapabilityConfigurationBaseHeaderEXT *all[] = { (const void *)&qr, (const void *)&micro, (const void *)&aruco,
                                                                 (const void *)&april, (const void *)&anchor };
    const XrSpatialContextCreateInfoEXT sci = { .type = XR_TYPE_SPATIAL_CONTEXT_CREATE_INFO_EXT, .capabilityConfigCount = 5, .capabilityConfigs = all };
    XrFutureEXT fut = XR_NULL_FUTURE_EXT;
    check(sp_xrCreateSpatialContextAsyncEXT(session, &sci, &fut) == XR_SUCCESS && fut != XR_NULL_FUTURE_EXT, "a context for every marker and anchors");
    XrCreateSpatialContextCompletionEXT cc = { .type = XR_TYPE_CREATE_SPATIAL_CONTEXT_COMPLETION_EXT };
    check(sp_xrCreateSpatialContextCompleteEXT(session, fut, &cc) == XR_SUCCESS && cc.futureResult == XR_SUCCESS, "its future completed");
    check(sp_xrCreateSpatialContextCompleteEXT(session, fut, &cc) == XR_ERROR_FUTURE_INVALID_EXT, "and is gone once completed");
    const XrFuturePollInfoEXT pi = { .type = XR_TYPE_FUTURE_POLL_INFO_EXT, .future = fut };
    XrFuturePollResultEXT pr = { .type = XR_TYPE_FUTURE_POLL_RESULT_EXT };
    check(sp_xrPollFutureEXT(in, &pi, &pr) == XR_ERROR_FUTURE_INVALID_EXT, "a completed future cannot be polled");
    return cc.futureResult == XR_SUCCESS ? cc.spatialContext : XR_NULL_HANDLE;
}

// BEFORE THE TURN: the QR code, the Micro QR code and the ArUco marker ahead; not the AprilTag on the left.
// And an anchor made a metre to the right and ahead; another in a space turned and moved from STAGE.
struct spatial_state {
    XrSpatialContextEXT ctx;
    bool first_done, second_done, recommended_after_first;
    XrSpatialEntityIdEXT qr, micro, aruco, anchor;
    XrSpatialEntityEXT qr_e, micro_e, aruco_e, anchor_e, anchor_again;
};

// In a space turned 90 degrees to the left of STAGE and 1 m right, 2 m back: an anchor made at (0, 1, -1)
// is at (0, 1, 2) in STAGE, and the QR code at (0, 1.5, -2) in STAGE is at (4, 1.5, -1) in it.
static void spatial_turned(struct spatial_state *st, XrSession session, XrSpace stage, XrTime t)
{
    const float h = 0.70710678f;
    const XrReferenceSpaceCreateInfo rsi = { .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE,
        .poseInReferenceSpace = { { 0, h, 0, h }, { 1, 0, 2 } } };
    XrSpace turned = XR_NULL_HANDLE;
    SPX(xrCreateReferenceSpace(session, &rsi, &turned));
    const XrSpatialAnchorCreateInfoEXT aci = { .type = XR_TYPE_SPATIAL_ANCHOR_CREATE_INFO_EXT, .baseSpace = turned, .time = t,
        .pose = { { 0, 0, 0, 1 }, { 0, 1, -1 } } };
    XrSpatialEntityIdEXT id = 0;
    XrSpatialEntityEXT e = XR_NULL_HANDLE;
    check(sp_xrCreateSpatialAnchorEXT(st->ctx, &aci, &id, &e) == XR_SUCCESS, "an anchor made in a turned space");
    XrSpatialSnapshotEXT snap = discover(st->ctx, stage, t);
    struct found an, f;
    query(snap, 4, &an);
    const int i = index_of_id(&an, id);
    check(i >= 0 && near3(an.anchor[i].position, 0, 1, 2), "it is where that space puts it in STAGE");
    check(i >= 0 && fabs(yaw_of(an.anchor[i].orientation) - 90) < 0.1, "and turned as that space is");
    SPX(sp_xrDestroySpatialSnapshotEXT(snap));
    snap = discover(st->ctx, turned, t);
    query(snap, 3, &f);
    const int q = index_of(&f, XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT);
    check(q >= 0 && near3(f.bounds[q].center.position, 4, 1.5f, -1), "the QR code where it is in the turned space");
    SPX(sp_xrDestroySpatialSnapshotEXT(snap));
    SPX(sp_xrDestroySpatialEntityEXT(e));   // and gone, so that the discoveries after find only the first anchor
    SPX(xrDestroySpace(turned));
}

static void spatial_first(struct spatial_state *st, XrSession session, XrSpace stage, XrTime t)
{
    st->first_done = true;
    XrSpatialSnapshotEXT snap = discover(st->ctx, stage, t);
    check(snap != XR_NULL_HANDLE, "the first discovery");
    if (snap == XR_NULL_HANDLE) return;
    struct found f;
    query(snap, 3, &f);
    const int q = index_of(&f, XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT);
    const int mq = index_of(&f, XR_SPATIAL_CAPABILITY_MARKER_TRACKING_MICRO_QR_CODE_EXT);
    const int a = index_of(&f, XR_SPATIAL_CAPABILITY_MARKER_TRACKING_ARUCO_MARKER_EXT);
    printf("  before the turn: %u markers seen\n", f.n);
    check(f.ok && f.n == 3 && q >= 0 && mq >= 0 && a >= 0, "before the turn: the QR, Micro QR and ArUco markers, not the AprilTag");
    check(q >= 0 && strcmp(f.text[q], "openxr-sim") == 0 && f.marker[q].markerId == 0, "the QR code says \"openxr-sim\"");
    check(q >= 0 && near3(f.bounds[q].center.position, 0, 1.5f, -2) && fabsf(f.bounds[q].extents.width - 0.2f) < 1e-5f &&
          fabsf(f.bounds[q].extents.height - 0.2f) < 1e-5f, "on the wall 2 m ahead, 20 cm square");
    check(a >= 0 && f.marker[a].markerId == 7 && f.marker[a].data.bufferId == XR_NULL_SPATIAL_BUFFER_ID_EXT, "the ArUco marker is number 7, with no buffer");
    // Its face up: the marker's +Z is STAGE's +Y.
    const XrVector3f up = a >= 0 ? rotate(f.bounds[a].center.orientation, (XrVector3f){ 0, 0, 1 }) : (XrVector3f){ 0, 0, 0 };
    check(up.y > 0.999f, "the ArUco marker lies face up");
    XrSpatialBufferGetInfoEXT bgi = { .type = XR_TYPE_SPATIAL_BUFFER_GET_INFO_EXT, .bufferId = q >= 0 ? f.marker[q].data.bufferId : 0 };
    uint32_t len = 0;
    uint8_t bytes[64];
    check(sp_xrGetSpatialBufferUint8EXT(snap, &bgi, sizeof bytes, &len, bytes) == XR_ERROR_VALIDATION_FAILURE, "a string buffer is not bytes");
    bgi.bufferId = 0x7777;
    char text[8];
    check(sp_xrGetSpatialBufferStringEXT(snap, &bgi, sizeof text, &len, text) == XR_ERROR_SPATIAL_BUFFER_ID_INVALID_EXT, "a buffer that is none");
    // A list for a component the condition does not name.
    const XrSpatialComponentTypeEXT only_marker = XR_SPATIAL_COMPONENT_TYPE_MARKER_EXT;
    const XrSpatialComponentDataQueryConditionEXT cond = { .type = XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_CONDITION_EXT,
        .componentTypeCount = 1, .componentTypes = &only_marker };
    XrSpatialBounded2DDataEXT b[8];
    XrSpatialComponentBounded2DListEXT bl = { .type = XR_TYPE_SPATIAL_COMPONENT_BOUNDED_2D_LIST_EXT, .boundCount = 8, .bounds = b };
    XrSpatialEntityIdEXT ids[8];
    XrSpatialComponentDataQueryResultEXT res = { .type = XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_RESULT_EXT, .next = &bl, .entityIdCapacityInput = 8, .entityIds = ids };
    check(sp_xrQuerySpatialComponentDataEXT(snap, &cond, &res) == XR_ERROR_VALIDATION_FAILURE, "a list the condition does not name is refused");
    bl.boundCount = 1;
    res.next = &bl;
    const XrSpatialComponentDataQueryConditionEXT all = { .type = XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_CONDITION_EXT };
    check(sp_xrQuerySpatialComponentDataEXT(snap, &all, &res) == XR_ERROR_SIZE_INSUFFICIENT, "a list shorter than the entities is refused");
    SPX(sp_xrDestroySpatialSnapshotEXT(snap));

    st->qr = q >= 0 ? f.id[q] : 0;
    st->micro = mq >= 0 ? f.id[mq] : 0;
    st->aruco = a >= 0 ? f.id[a] : 0;
    XrSpatialEntityFromIdCreateInfoEXT eci = { .type = XR_TYPE_SPATIAL_ENTITY_FROM_ID_CREATE_INFO_EXT, .entityId = st->qr };
    check(sp_xrCreateSpatialEntityFromIdEXT(st->ctx, &eci, &st->qr_e) == XR_SUCCESS, "the QR code's entity");
    eci.entityId = st->micro;
    check(sp_xrCreateSpatialEntityFromIdEXT(st->ctx, &eci, &st->micro_e) == XR_SUCCESS, "the Micro QR code's entity");
    eci.entityId = st->aruco;
    check(sp_xrCreateSpatialEntityFromIdEXT(st->ctx, &eci, &st->aruco_e) == XR_SUCCESS, "the ArUco marker's entity");
    eci.entityId = 0x123456789;
    XrSpatialEntityEXT none;
    check(sp_xrCreateSpatialEntityFromIdEXT(st->ctx, &eci, &none) == XR_ERROR_SPATIAL_ENTITY_ID_INVALID_EXT, "an id never discovered is refused");

    XrSpatialAnchorCreateInfoEXT aci = { .type = XR_TYPE_SPATIAL_ANCHOR_CREATE_INFO_EXT, .baseSpace = stage, .time = t,
        .pose = { { 0, 0, 0, 1 }, { 1, 1, -1 } } };
    check(sp_xrCreateSpatialAnchorEXT(st->ctx, &aci, &st->anchor, &st->anchor_e) == XR_SUCCESS && st->anchor != 0, "an anchor made");
    aci.pose.orientation.w = 0;
    XrSpatialEntityIdEXT bad_id;
    check(sp_xrCreateSpatialAnchorEXT(st->ctx, &aci, &bad_id, &none) == XR_ERROR_POSE_INVALID, "an anchor of no orientation is refused");
    eci.entityId = st->anchor;
    check(sp_xrCreateSpatialEntityFromIdEXT(st->ctx, &eci, &st->anchor_again) == XR_SUCCESS, "a second handle to the anchor");
    spatial_turned(st, session, stage, t);

    // A discovery cancelled.
    const XrSpatialDiscoverySnapshotCreateInfoEXT dci = { .type = XR_TYPE_SPATIAL_DISCOVERY_SNAPSHOT_CREATE_INFO_EXT };
    XrFutureEXT fut;
    SPX(sp_xrCreateSpatialDiscoverySnapshotAsyncEXT(st->ctx, &dci, &fut));
    const XrFutureCancelInfoEXT cancel = { .type = XR_TYPE_FUTURE_CANCEL_INFO_EXT, .future = fut };
    SPX(sp_xrCancelFutureEXT(in, &cancel));
    const XrCreateSpatialDiscoverySnapshotCompletionInfoEXT cci = { .type = XR_TYPE_CREATE_SPATIAL_DISCOVERY_SNAPSHOT_COMPLETION_INFO_EXT,
        .baseSpace = stage, .time = t, .future = fut };
    XrCreateSpatialDiscoverySnapshotCompletionEXT cc = { .type = XR_TYPE_CREATE_SPATIAL_DISCOVERY_SNAPSHOT_COMPLETION_EXT };
    check(sp_xrCreateSpatialDiscoverySnapshotCompleteEXT(st->ctx, &cci, &cc) == XR_ERROR_FUTURE_INVALID_EXT, "a cancelled discovery cannot be completed");
    const XrSpatialComponentTypeEXT plane = XR_SPATIAL_COMPONENT_TYPE_PLANE_ALIGNMENT_EXT;
    const XrSpatialDiscoverySnapshotCreateInfoEXT bad = { .type = XR_TYPE_SPATIAL_DISCOVERY_SNAPSHOT_CREATE_INFO_EXT, .componentTypeCount = 1, .componentTypes = &plane };
    check(sp_xrCreateSpatialDiscoverySnapshotAsyncEXT(st->ctx, &bad, &fut) == XR_ERROR_SPATIAL_COMPONENT_NOT_ENABLED_EXT, "a component not enabled is refused");
}

// AFTER THE TURN: the AprilTag in view and discovered, the anchor with it; the QR code moved, the same
// entity; the Micro QR code out of view on the right, PAUSED; the ArUco marker taken away, STOPPED.
static void spatial_second(struct spatial_state *st, XrSpace stage, XrTime t)
{
    st->second_done = true;
    check(st->recommended_after_first, "a discovery recommended once the AprilTag came into view");
    XrSpatialSnapshotEXT snap = discover(st->ctx, stage, t);
    check(snap != XR_NULL_HANDLE, "the second discovery");
    if (snap == XR_NULL_HANDLE) return;
    struct found f, an;
    query(snap, 1, &f);
    const int tag = index_of(&f, XR_SPATIAL_CAPABILITY_MARKER_TRACKING_APRIL_TAG_EXT);
    printf("  after the turn: %u markers seen\n", f.n);
    check(tag >= 0 && f.marker[tag].markerId == 3, "after the turn: the AprilTag, number 3");
    check(index_of(&f, XR_SPATIAL_CAPABILITY_MARKER_TRACKING_MICRO_QR_CODE_EXT) < 0, "not the Micro QR code, out of view");
    check(index_of(&f, XR_SPATIAL_CAPABILITY_MARKER_TRACKING_ARUCO_MARKER_EXT) < 0, "nor the ArUco marker, taken away");
    check(index_of_id(&f, st->qr) >= 0, "the QR code again, by the same id");
    query(snap, 4, &an);
    const int ai = index_of_id(&an, st->anchor);
    check(an.ok && an.n == 1 && ai == 0 && near3(an.anchor[0].position, 1, 1, -1), "the anchor discovered where it was made");
    SPX(sp_xrDestroySpatialSnapshotEXT(snap));

    const XrSpatialEntityEXT ents[] = { st->qr_e, st->micro_e, st->aruco_e, st->anchor_again };
    const XrSpatialUpdateSnapshotCreateInfoEXT uci = { .type = XR_TYPE_SPATIAL_UPDATE_SNAPSHOT_CREATE_INFO_EXT, .entityCount = 4,
        .entities = ents, .baseSpace = stage, .time = t };
    XrSpatialSnapshotEXT up = XR_NULL_HANDLE;
    check(sp_xrCreateSpatialUpdateSnapshotEXT(st->ctx, &uci, &up) == XR_SUCCESS, "an update of the QR code, the Micro QR code, the ArUco marker and the anchor");
    struct found u, ua;
    query(up, 3, &u);
    const int q = index_of_id(&u, st->qr), m = index_of_id(&u, st->micro), r = index_of_id(&u, st->aruco);
    check(u.ok && u.n == 3 && q >= 0 && m >= 0 && r >= 0, "the three markers in it");
    check(q >= 0 && u.state[q] == XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT && near3(u.bounds[q].center.position, -0.3f, 1.5f, -2),
          "the QR code TRACKING, moved 30 cm left by the simulator");
    check(m >= 0 && u.state[m] == XR_SPATIAL_ENTITY_TRACKING_STATE_PAUSED_EXT, "the Micro QR code PAUSED, out of view");
    check(m >= 0 && u.bounds[m].extents.width == -1, "and its bounds left alone");
    check(r >= 0 && u.state[r] == XR_SPATIAL_ENTITY_TRACKING_STATE_STOPPED_EXT, "the ArUco marker STOPPED, taken away");
    query(up, 4, &ua);
    check(ua.ok && ua.n == 1 && ua.id[0] == st->anchor && ua.state[0] == XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT &&
          near3(ua.anchor[0].position, 1, 1, -1), "the anchor TRACKING, where it was made");
    SPX(sp_xrDestroySpatialSnapshotEXT(up));

    SPX(sp_xrDestroySpatialEntityEXT(st->anchor_e));   // the anchor goes with the handle it was made with
    const XrSpatialUpdateSnapshotCreateInfoEXT uci2 = { .type = XR_TYPE_SPATIAL_UPDATE_SNAPSHOT_CREATE_INFO_EXT, .entityCount = 1,
        .entities = &st->anchor_again, .baseSpace = stage, .time = t };
    up = XR_NULL_HANDLE;
    check(sp_xrCreateSpatialUpdateSnapshotEXT(st->ctx, &uci2, &up) == XR_SUCCESS, "an update of the anchor by its other handle");
    query(up, 0, &ua);
    check(ua.ok && ua.n == 1 && ua.state[0] == XR_SPATIAL_ENTITY_TRACKING_STATE_STOPPED_EXT, "the anchor STOPPED, its handle destroyed");
    SPX(sp_xrDestroySpatialSnapshotEXT(up));
    SPX(sp_xrDestroySpatialContextEXT(st->ctx));
    check(sp_xrDestroySpatialEntityEXT(st->qr_e) == XR_ERROR_HANDLE_INVALID, "the context's entities go with it");
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        win32 |= strcmp(argv[i], "--gl-win32") == 0;
        gl |= strcmp(argv[i], "--gl") == 0;
        layers |= strcmp(argv[i], "--layers") == 0;
        hands |= strcmp(argv[i], "--hands") == 0;
        hand_interaction |= strcmp(argv[i], "--hand-interaction") == 0;
        spatial |= strcmp(argv[i], "--spatial") == 0;
        if (strcmp(argv[i], "--d3d11") == 0) d3d = 11;
        if (strcmp(argv[i], "--d3d12") == 0) d3d = 12;
    }
#ifndef OXS_DIRECT
    if (d3d != 0) { printf("  Direct3D is Windows'\n"); return 2; }
#endif
    gl |= win32;
    const char *exts[12] = { d3d == 11 ? "XR_KHR_D3D11_enable" : d3d == 12 ? "XR_KHR_D3D12_enable"
                            : gl ? XR_KHR_OPENGL_ENABLE_EXTENSION_NAME : XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
    uint32_t nexts = 1;
    if (gl) exts[nexts++] = XR_MNDX_EGL_ENABLE_EXTENSION_NAME;
    if (layers) {
        exts[nexts++] = XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME;
        exts[nexts++] = XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME;
    }
    if (hands) exts[nexts++] = XR_EXT_HAND_TRACKING_EXTENSION_NAME;
    if (hand_interaction) exts[nexts++] = XR_EXT_HAND_INTERACTION_EXTENSION_NAME;
    if (spatial) {
        exts[nexts++] = XR_EXT_FUTURE_EXTENSION_NAME;
        exts[nexts++] = XR_EXT_SPATIAL_ENTITY_EXTENSION_NAME;
        exts[nexts++] = XR_EXT_SPATIAL_MARKER_TRACKING_EXTENSION_NAME;
        exts[nexts++] = XR_EXT_SPATIAL_ANCHOR_EXTENSION_NAME;
    }
    XrInstanceCreateInfo ci = { .type = XR_TYPE_INSTANCE_CREATE_INFO, .enabledExtensionCount = nexts, .enabledExtensionNames = exts };
    snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "%s%s%s%s%s",
             d3d == 11 ? "sim-test-d3d11" : d3d == 12 ? "sim-test-d3d12" : win32 ? "sim-test-win32" : gl ? "sim-test-gl" : "sim-test", layers ? "-layers" : "", hands ? "-hands" : "",
             hand_interaction ? "-interaction" : "", spatial ? "-spatial" : "");
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

    // SPATIAL ENTITIES are there only when enabled, and need what they build on.
    {
        PFN_xrVoidFunction f = NULL;
        r = xrGetInstanceProcAddr(in, "xrCreateSpatialContextAsyncEXT", &f);
        if (!spatial) check(r == XR_ERROR_FUNCTION_UNSUPPORTED, "no spatial entities unless they are enabled");
        else if (spatial_functions() != 0) return 1;
    }

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
#ifdef OXS_DIRECT
    XrGraphicsBindingD3D11KHR d11_binding = { 0 };
    XrGraphicsBindingD3D12KHR d12_binding = { 0 };
#endif
    if (d3d != 0) {
#ifdef OXS_DIRECT
        if (setup_d3d(sys, &d11_binding, &d12_binding) != 0) return 1;
#endif
    } else if (gl) {
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
#ifdef OXS_DIRECT
    if (d3d == 11) sci.next = &d11_binding;
    if (d3d == 12) sci.next = &d12_binding;
#endif
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
    struct spatial_state sst = { 0 };
    if (spatial) {
        spatial_capabilities(sys, session);
        sst.ctx = spatial_context(session);
        if (sst.ctx == XR_NULL_HANDLE) return 1;
    }
    // The joints and grips as last located once the scripted move had arrived.
    XrHandJointLocationEXT joint[2][XR_HAND_JOINT_COUNT_EXT];
    XrHandJointVelocityEXT joint_velocity[2][XR_HAND_JOINT_COUNT_EXT];
    XrHandJointLocationsEXT joints[2];
    XrSpaceLocation grip_at[2], pinch_at[2], poke_at[2];
    bool located = false, short_refused = false;

    XrSwapchain sc[2];
    static struct images eye_images[2];
    uint32_t nimages[2];
    for (int e = 0; e < 2; e++) {
        XrSwapchainCreateInfo swci = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
            .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
            .format = swapchain_format(), .sampleCount = 1, .width = EYE, .height = EYE, .faceCount = 1, .arraySize = 1, .mipCount = 1 };
        XR(xrCreateSwapchain(session, &swci, &sc[e]));
        XR(xrEnumerateSwapchainImages(sc[e], 4, &nimages[e], images_for_enumerate(&eye_images[e])));
    }

    // THE LAYERS' IMAGES (--layers): each its colour, alpha, and size; the blue square at its top left.
    enum { QUAD_A, CYLINDER, EQUIRECT, QUAD_B, PANELS };
    static const struct { int w, h; float r, g, b, a; } panel[PANELS] = {
        [QUAD_A] = { 128, 128, 1, 1, 0, 1 }, [CYLINDER] = { 256, 64, 1, 0, 1, 1 },
        [EQUIRECT] = { 256, 128, 0, 1, 1, 1 }, [QUAD_B] = { 128, 64, 1, 1, 1, 0.5f },
    };
    XrSwapchain psc[PANELS];
    static struct images panel_images[PANELS];
    XrSpace view = XR_NULL_HANDLE;
    if (layers) {
        rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        XR(xrCreateReferenceSpace(session, &rsi, &view));
        for (int p = 0; p < PANELS; p++) {
            XrSwapchainCreateInfo swci = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
                .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
                .format = swapchain_format(), .sampleCount = 1, .width = panel[p].w,
                .height = panel[p].h, .faceCount = 1, .arraySize = 1, .mipCount = 1 };
            XR(xrCreateSwapchain(session, &swci, &psc[p]));
            uint32_t n = 0;
            XR(xrEnumerateSwapchainImages(psc[p], 4, &n, images_for_enumerate(&panel_images[p])));
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
            } else if (ev.type == XR_TYPE_EVENT_DATA_SPATIAL_DISCOVERY_RECOMMENDED_EXT) {
                const XrEventDataSpatialDiscoveryRecommendedEXT *d = (const XrEventDataSpatialDiscoveryRecommendedEXT *)&ev;
                if (d->spatialContext == sst.ctx && sst.first_done) sst.recommended_after_first = true;
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

        // Markers before the head turn (the first frame, before the simulator has made it) and after it, once the
        // discovery it recommends has been polled -- or five seconds on, when it is overdue.
        if (spatial && !sst.first_done && !saw_turn) spatial_first(&sst, session, stage, fs.predictedDisplayTime);
        if (spatial && sst.first_done && !sst.second_done && saw_turn && (sst.recommended_after_first || frames > 360))
            spatial_second(&sst, stage, fs.predictedDisplayTime);

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
            if (draw_image(&eye_images[e], idx, EYE, e == 0 ? 1 : 0, e == 0 ? 0 : 1, 0, 1) != 0) return 1;
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
                if (draw_image(&panel_images[p], idx, panel[p].h, panel[p].r, panel[p].g, panel[p].b, panel[p].a) != 0) return 1;
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
        if (!exiting && ((saw_trigger && saw_a && saw_turn && frames > 30 && (!spatial || sst.second_done)) || frames > 720)) {
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
    if (spatial) check(sst.first_done && sst.second_done, "markers looked for before and after the turn");

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
#ifdef OXS_DIRECT
    if (d11c != NULL) ID3D11DeviceContext_Release(d11c);
    if (d11 != NULL) ID3D11Device_Release(d11);
    if (d12 != NULL) {
        ID3D12DescriptorHeap_Release(d12h);
        ID3D12Fence_Release(d12f);
        ID3D12GraphicsCommandList_Release(d12l);
        ID3D12CommandAllocator_Release(d12a);
        ID3D12CommandQueue_Release(d12q);
        ID3D12Device_Release(d12);
        CloseHandle(d12e);
    }
#endif
    if (gl || d3d != 0) return failures == 0 ? 0 : 1;
    vkDeviceWaitIdle(dev);
    vkDestroyBuffer(dev, square, NULL);
    vkFreeMemory(dev, mem, NULL);
    vkDestroyFence(dev, fence, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(vk, NULL);
    return failures == 0 ? 0 : 1;
}
