// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// openxr-sim -- an OpenXR runtime whose headset is a window (LOS's runtime 0.1.0 2026-10-01; the simulated
// headset 0.2.0, 2026-10-21; its own repository, for Linux and Windows, 0.7.0 2026-11-04).
//
// WHY. An OpenXR application asks the loader for the active runtime; with none, or with a manifest
// whose library is missing, xrCreateInstance fails with XR_ERROR_RUNTIME_UNAVAILABLE and most
// applications treat that as fatal -- a game with an optional VR mode refuses to start, an engine
// editor shows an error dialog at launch. With this runtime the instance is created normally, and the
// question "is there a headset?" is the device's to answer (device.h): the null device says OpenXR's
// "not plugged in", XR_ERROR_FORM_FACTOR_UNAVAILABLE, which applications are written to handle; the
// simulated one says yes, and the application runs into a window on the desktop (DESIGN.md).
//
// SHAPE. This file is the loader, the instance, its extensions, systems, paths, strings, the Vulkan
// helpers of XR_KHR_vulkan_enable(2), and the dispatch table. session.c, swapchain*.c and input.c are
// the rest (internal.h). Every core and enabled-extension function exists -- the loader builds its
// dispatch table from xrGetInstanceProcAddr at instance creation, and a function left out is a NULL the
// application's next call jumps through -- and the few without a body return the error OpenXR gives for
// a handle that cannot exist.
//
// THE STUBS share two bodies, cast to each function's type: one returns XR_ERROR_SYSTEM_INVALID,
// the other XR_ERROR_HANDLE_INVALID. Calling a function through a pointer of another type is
// undefined in ISO C; on the System V x86-64 and AArch64 ABIs a callee that reads none of its
// arguments returns correctly whatever was passed, which is the guarantee relied on here.
#include "internal.h"

#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_reflection.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void say(const char *fmt, ...)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("OPENXR_SIM_LOG"); on = e != NULL && e[0] != '\0' && e[0] != '0'; }
    if (!on) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("openxr-sim: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

XrTime now_ns(void) { return plat_now_ns(); }

// THE SETTING (DESIGN.md, "Choosing the device"): `device = sim` in the configuration file
// (plat_config_path: $XDG_CONFIG_HOME/openxr-sim/config, %APPDATA%\openxr-sim\config). One key today.
// Until 2027-05-01 the file LOS's runtime read before the split is read when this one does not exist.
static void configured_device(char *out, size_t n)
{
    out[0] = '\0';
    char path[4096];
    FILE *f = NULL;
    for (int which = 0; which < 2 && f == NULL; which++)
        if (plat_config_path(which, path, sizeof path)) f = plat_fopen_read(path);
    if (f == NULL) return;
    char line[256];
    while (fgets(line, sizeof line, f) != NULL) {
        char key[64], value[64];
        if (line[0] == '#' || sscanf(line, " %63[a-z_] = %63s", key, value) != 2) continue;
        if (strcmp(key, "device") == 0) snprintf(out, n, "%s", value);
    }
    fclose(f);
}

const struct oxs_device *device(void)
{
    static const struct oxs_device *chosen;
    if (chosen != NULL) return chosen;
    static const struct oxs_device *const all[] = { &oxs_device_null, &oxs_device_sim };
    char want[64];
    const char *e = getenv("OPENXR_SIM_DEVICE");
    if (e == NULL || e[0] == '\0') e = getenv("LOS_OPENXR_DEVICE");   // LOS's name for it, until 2027-05-01
    if (e != NULL && e[0] != '\0') snprintf(want, sizeof want, "%s", e);
    else configured_device(want, sizeof want);
    chosen = &oxs_device_null;
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++)
        if (strcmp(want, all[i]->name) == 0) chosen = all[i];
    if (want[0] != '\0' && strcmp(want, chosen->name) != 0) say("no device called \"%s\"; using \"%s\"", want, chosen->name);
    return chosen;
}

// ---- handles -------------------------------------------------------------------------------------

static struct object *live;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void *make(size_t size, uint32_t magic, struct object *instance, struct object *parent)
{
    struct object *o = calloc(1, size);
    if (o == NULL) return NULL;
    o->magic = magic;
    o->instance = instance != NULL ? instance : o;
    o->parent = parent;
    pthread_mutex_lock(&lock);
    o->next = live;
    live = o;
    pthread_mutex_unlock(&lock);
    return o;
}

struct object *find(uint64_t handle, uint32_t magic)
{
    struct object *found = NULL;
    pthread_mutex_lock(&lock);
    for (struct object *o = live; o != NULL; o = o->next)
        if ((uint64_t)(uintptr_t)o == handle && o->magic == magic) { found = o; break; }
    pthread_mutex_unlock(&lock);
    return found;
}

// Owned by `gone`: made from it as an instance, or as a session.
static bool owned(const struct object *o, const struct object *gone)
{
    if (o == gone) return false;
    if (gone->magic == MAGIC_INSTANCE) return o->instance == gone;
    return o->parent == gone;
}

// Destroys `gone` and everything it owns: children first (a session's swapchains before the session,
// the sessions before the instance), each with its `fini`.
void destroy(struct object *gone)
{
    for (;;) {
        struct object *child = NULL;
        pthread_mutex_lock(&lock);
        // A session before the rest of an instance's objects, since its own children go with it.
        for (struct object *o = live; o != NULL; o = o->next)
            if (owned(o, gone) && (child == NULL || o->magic == MAGIC_SESSION)) child = o;
        pthread_mutex_unlock(&lock);
        if (child == NULL) break;
        destroy(child);
    }
    if (gone->fini != NULL) gone->fini(gone);
    pthread_mutex_lock(&lock);
    for (struct object **p = &live; *p != NULL; p = &(*p)->next)
        if (*p == gone) { *p = gone->next; break; }
    pthread_mutex_unlock(&lock);
    free(gone);
}

// ---- extensions ----------------------------------------------------------------------------------
//
// THE ONES APPLICATIONS REQUIRE AT xrCreateInstance. An application that asks for an extension the
// runtime lacks gets XR_ERROR_EXTENSION_NOT_PRESENT -- the same fatal error as no runtime at all --
// so every graphics binding an application on Linux might name is offered, OpenGL ES with them
// (its only binding is Android's, but an engine may still require the extension).

static const struct { const char *name; uint32_t version; } EXTENSIONS[] = {
    { XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_KHR_vulkan_enable_SPEC_VERSION },
    { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_KHR_vulkan_enable2_SPEC_VERSION },
    { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME, XR_KHR_opengl_enable_SPEC_VERSION },
    { XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME, XR_KHR_opengl_es_enable_SPEC_VERSION },
    { "XR_MNDX_egl_enable", 2 },   // its macros sit behind XR_USE_PLATFORM_EGL (swapchain_gl.c)
    { XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME, XR_KHR_composition_layer_depth_SPEC_VERSION },
    // The layer types the simulator draws besides the core's projection and quad (composite.c). Not the
    // cube: a cube map is a swapchain of six faces, which swapchain.c does not make.
    { XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME, XR_KHR_composition_layer_cylinder_SPEC_VERSION },
    { XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME, XR_KHR_composition_layer_equirect2_SPEC_VERSION },
    { XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME, XR_KHR_composition_layer_color_scale_bias_SPEC_VERSION },
    // Joints made from the controllers (hand.c).
    { XR_EXT_HAND_TRACKING_EXTENSION_NAME, XR_EXT_hand_tracking_SPEC_VERSION },
    { XR_EXT_HAND_INTERACTION_EXTENSION_NAME, XR_EXT_hand_interaction_SPEC_VERSION },
    { XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME, XR_KHR_convert_timespec_time_SPEC_VERSION },
    { XR_EXT_DEBUG_UTILS_EXTENSION_NAME, XR_EXT_debug_utils_SPEC_VERSION },
    { XR_EXT_LOCAL_FLOOR_EXTENSION_NAME, XR_EXT_local_floor_SPEC_VERSION },
    { XR_MND_HEADLESS_EXTENSION_NAME, XR_MND_headless_SPEC_VERSION },
};
#define NEXTENSIONS (sizeof EXTENSIONS / sizeof EXTENSIONS[0])

static XRAPI_ATTR XrResult XRAPI_CALL oxs_EnumerateInstanceExtensionProperties(
    const char *layer, uint32_t cap, uint32_t *count, XrExtensionProperties *props)
{
    if (layer != NULL && layer[0] != '\0') return XR_ERROR_API_LAYER_NOT_PRESENT;
    if (count == NULL) return XR_ERROR_VALIDATION_FAILURE;
    *count = NEXTENSIONS;
    if (cap == 0) return XR_SUCCESS;
    if (cap < NEXTENSIONS) return XR_ERROR_SIZE_INSUFFICIENT;
    for (uint32_t i = 0; i < NEXTENSIONS; i++) {
        if (props[i].type != XR_TYPE_EXTENSION_PROPERTIES) return XR_ERROR_VALIDATION_FAILURE;
        snprintf(props[i].extensionName, sizeof props[i].extensionName, "%s", EXTENSIONS[i].name);
        props[i].extensionVersion = EXTENSIONS[i].version;
    }
    return XR_SUCCESS;
}

static bool offered(const char *name)
{
    for (size_t i = 0; i < NEXTENSIONS; i++)
        if (strcmp(EXTENSIONS[i].name, name) == 0) return true;
    return false;
}

// ---- the instance --------------------------------------------------------------------------------

static void instance_fini(struct object *o)
{
    struct instance *in = (struct instance *)o;
    for (size_t i = 0; i < in->nsuggested; i++) free(in->suggested[i].b);
    free(in->suggested);
    free(in->sets);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_CreateInstance(const XrInstanceCreateInfo *info, XrInstance *out)
{
    if (info == NULL || out == NULL || info->type != XR_TYPE_INSTANCE_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    // 1.0 and 1.1 applications alike; a 2.x application is told this runtime is too old.
    if (XR_VERSION_MAJOR(info->applicationInfo.apiVersion) != 1) return XR_ERROR_API_VERSION_UNSUPPORTED;
    bool headless = false, cylinder = false, equirect2 = false, color_scale_bias = false, hand_tracking = false,
         hand_interaction = false;
    for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
        if (!offered(info->enabledExtensionNames[i])) {
            say("%s asked for %s, which is not offered", info->applicationInfo.applicationName,
                info->enabledExtensionNames[i]);
            return XR_ERROR_EXTENSION_NOT_PRESENT;
        }
        const char *e = info->enabledExtensionNames[i];
        if (strcmp(e, XR_MND_HEADLESS_EXTENSION_NAME) == 0) headless = true;
        if (strcmp(e, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME) == 0) cylinder = true;
        if (strcmp(e, XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME) == 0) equirect2 = true;
        if (strcmp(e, XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME) == 0) color_scale_bias = true;
        if (strcmp(e, XR_EXT_HAND_TRACKING_EXTENSION_NAME) == 0) hand_tracking = true;
        if (strcmp(e, XR_EXT_HAND_INTERACTION_EXTENSION_NAME) == 0) hand_interaction = true;
    }
    if (info->enabledApiLayerCount > 0) return XR_ERROR_API_LAYER_NOT_PRESENT;   // the loader's job
    struct instance *in = make(sizeof *in, MAGIC_INSTANCE, NULL, NULL);
    if (in == NULL) return XR_ERROR_OUT_OF_MEMORY;
    in->o.fini = instance_fini;
    in->headless = headless;
    in->cylinder = cylinder;
    in->equirect2 = equirect2;
    in->color_scale_bias = color_scale_bias;
    in->hand_tracking = hand_tracking;
    in->hand_interaction = hand_interaction;
    snprintf(in->app_name, sizeof in->app_name, "%s", info->applicationInfo.applicationName);
    *out = (XrInstance)(uintptr_t)in;
    say("instance for %s (device \"%s\")", in->app_name, device()->name);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_DestroyInstance(XrInstance instance)
{
    struct object *o = find(H(instance), MAGIC_INSTANCE);
    if (o == NULL) return XR_ERROR_HANDLE_INVALID;
    destroy(o);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_GetInstanceProperties(XrInstance instance, XrInstanceProperties *p)
{
    if (find(H(instance), MAGIC_INSTANCE) == NULL) return XR_ERROR_HANDLE_INVALID;
    if (p == NULL || p->type != XR_TYPE_INSTANCE_PROPERTIES) return XR_ERROR_VALIDATION_FAILURE;
    p->runtimeVersion = RUNTIME_VERSION;
    snprintf(p->runtimeName, sizeof p->runtimeName, "%s", RUNTIME_NAME);
    return XR_SUCCESS;
}

// ---- events --------------------------------------------------------------------------------------

void post_event(struct instance *in, const void *event, size_t size)
{
    pthread_mutex_lock(&lock);
    const unsigned cap = sizeof in->events / sizeof in->events[0];
    if (in->event_count == cap) {   // full: the oldest goes, and the application is told it lost some
        in->event_head = (in->event_head + 1) % cap;
        in->event_count--;
    }
    XrEventDataBuffer *e = &in->events[(in->event_head + in->event_count) % cap];
    memset(e, 0, sizeof *e);
    memcpy(e, event, size < sizeof *e ? size : sizeof *e);
    in->event_count++;
    pthread_mutex_unlock(&lock);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_PollEvent(XrInstance instance, XrEventDataBuffer *event)
{
    struct instance *in = (struct instance *)find(H(instance), MAGIC_INSTANCE);
    if (in == NULL) return XR_ERROR_HANDLE_INVALID;
    if (event == NULL) return XR_ERROR_VALIDATION_FAILURE;
    if (in->session != NULL) session_poll(in->session);
    pthread_mutex_lock(&lock);
    if (in->event_count == 0) { pthread_mutex_unlock(&lock); return XR_EVENT_UNAVAILABLE; }
    *event = in->events[in->event_head];
    in->event_head = (in->event_head + 1) % (sizeof in->events / sizeof in->events[0]);
    in->event_count--;
    pthread_mutex_unlock(&lock);
    return XR_SUCCESS;
}

// ---- the system ----------------------------------------------------------------------------------
//
// THE SIMULATED HEADSET'S NUMBERS (DESIGN.md, "What the simulated headset is"). A null device has no
// system, so every function here answers XR_ERROR_SYSTEM_INVALID for it.

#define EYE_MAX 4096

bool system_ok(struct instance *in, XrSystemId id)
{
    (void)in;
    if (id != OXS_SYSTEM_ID) return false;
    uint64_t sys = 0;
    return device()->get_system(XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY, &sys) == XR_SUCCESS && sys == id;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_GetSystem(XrInstance instance, const XrSystemGetInfo *info, XrSystemId *id)
{
    if (find(H(instance), MAGIC_INSTANCE) == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || id == NULL || info->type != XR_TYPE_SYSTEM_GET_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY && info->formFactor != XR_FORM_FACTOR_HANDHELD_DISPLAY)
        return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    uint64_t sys = XR_NULL_SYSTEM_ID;
    const XrResult r = (XrResult)device()->get_system(info->formFactor, &sys);
    if (r == XR_SUCCESS) *id = sys;
    say("xrGetSystem: %d", (int)r);
    return r;
}

#define SYSTEM_CHECK(instance, id)                                                                    \
    struct instance *in = (struct instance *)find(H(instance), MAGIC_INSTANCE);                       \
    if (in == NULL) return XR_ERROR_HANDLE_INVALID;                                                   \
    if (!system_ok(in, (id))) return XR_ERROR_SYSTEM_INVALID

static XRAPI_ATTR XrResult XRAPI_CALL oxs_GetSystemProperties(XrInstance instance, XrSystemId id, XrSystemProperties *p)
{
    SYSTEM_CHECK(instance, id);
    if (p == NULL || p->type != XR_TYPE_SYSTEM_PROPERTIES) return XR_ERROR_VALIDATION_FAILURE;
    p->systemId = id;
    p->vendorId = 0x105;
    snprintf(p->systemName, sizeof p->systemName, "%s", device()->system_name);
    p->graphicsProperties.maxSwapchainImageWidth = EYE_MAX;
    p->graphicsProperties.maxSwapchainImageHeight = EYE_MAX;
    p->graphicsProperties.maxLayerCount = XR_MIN_COMPOSITION_LAYERS_SUPPORTED;
    p->trackingProperties.orientationTracking = XR_TRUE;
    p->trackingProperties.positionTracking = XR_TRUE;
    for (XrBaseOutStructure *n = p->next; n != NULL; n = n->next)
        if (n->type == XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT)
            ((XrSystemHandTrackingPropertiesEXT *)n)->supportsHandTracking = in->hand_tracking ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_EnumerateViewConfigurations(XrInstance instance, XrSystemId id, uint32_t cap,
                                                                     uint32_t *count, XrViewConfigurationType *out)
{
    SYSTEM_CHECK(instance, id);
    static const XrViewConfigurationType types[] = { XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO };
    TWO_CALL(cap, count, out, types, 1);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_GetViewConfigurationProperties(XrInstance instance, XrSystemId id,
                                                                        XrViewConfigurationType type,
                                                                        XrViewConfigurationProperties *p)
{
    SYSTEM_CHECK(instance, id);
    if (p == NULL || p->type != XR_TYPE_VIEW_CONFIGURATION_PROPERTIES) return XR_ERROR_VALIDATION_FAILURE;
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    p->viewConfigurationType = type;
    p->fovMutable = XR_FALSE;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_EnumerateViewConfigurationViews(XrInstance instance, XrSystemId id,
                                                                         XrViewConfigurationType type, uint32_t cap,
                                                                         uint32_t *count, XrViewConfigurationView *out)
{
    SYSTEM_CHECK(instance, id);
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    if (count == NULL) return XR_ERROR_VALIDATION_FAILURE;
    *count = 2;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    for (int i = 0; i < 2; i++) {
        if (out[i].type != XR_TYPE_VIEW_CONFIGURATION_VIEW) return XR_ERROR_VALIDATION_FAILURE;
        out[i].recommendedImageRectWidth = out[i].recommendedImageRectHeight = EYE_RECOMMENDED;
        out[i].maxImageRectWidth = out[i].maxImageRectHeight = EYE_MAX;
        out[i].recommendedSwapchainSampleCount = out[i].maxSwapchainSampleCount = 1;
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_EnumerateEnvironmentBlendModes(XrInstance instance, XrSystemId id,
                                                                        XrViewConfigurationType type, uint32_t cap,
                                                                        uint32_t *count, XrEnvironmentBlendMode *out)
{
    SYSTEM_CHECK(instance, id);
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    static const XrEnvironmentBlendMode modes[] = { XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
    TWO_CALL(cap, count, out, modes, 1);
    return XR_SUCCESS;
}

// ---- graphics requirements -----------------------------------------------------------------------
//
// OpenXR makes asking these a precondition of xrCreateSession (XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING).
// What the runtime does with an application's device -- create images, copy them to memory (DESIGN.md,
// "Frames: copied, not shared") -- is Vulkan 1.0 and OpenGL 3.0 (OpenGL ES 3.0).

static XRAPI_ATTR XrResult XRAPI_CALL oxs_GetVulkanGraphicsRequirementsKHR(XrInstance instance, XrSystemId id,
                                                                          XrGraphicsRequirementsVulkanKHR *r)
{
    SYSTEM_CHECK(instance, id);
    if (r == NULL || (r->type != XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR)) return XR_ERROR_VALIDATION_FAILURE;
    r->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
    r->maxApiVersionSupported = XR_MAKE_VERSION(1, 4, 0);
    in->asked_vulkan = true;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_GetOpenGLGraphicsRequirementsKHR(XrInstance instance, XrSystemId id,
                                                                          XrGraphicsRequirementsOpenGLKHR *r)
{
    SYSTEM_CHECK(instance, id);
    if (r == NULL || r->type != XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR) return XR_ERROR_VALIDATION_FAILURE;
    r->minApiVersionSupported = XR_MAKE_VERSION(3, 0, 0);
    r->maxApiVersionSupported = XR_MAKE_VERSION(4, 6, 0);
    in->asked_gl = true;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_GetOpenGLESGraphicsRequirementsKHR(XrInstance instance, XrSystemId id,
                                                                            XrGraphicsRequirementsOpenGLESKHR *r)
{
    SYSTEM_CHECK(instance, id);
    if (r == NULL || r->type != XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR) return XR_ERROR_VALIDATION_FAILURE;
    r->minApiVersionSupported = XR_MAKE_VERSION(3, 0, 0);
    r->maxApiVersionSupported = XR_MAKE_VERSION(3, 2, 0);
    in->asked_gl = true;
    return XR_SUCCESS;
}

// ---- Vulkan's helpers ----------------------------------------------------------------------------
//
// NO EXTENSIONS ARE NEEDED: the runtime only creates images and copies them on the application's own
// device. The lists are empty strings, which wineopenxr reads as none (its parse_extensions()).

static XrResult empty_list(uint32_t cap, uint32_t *count, char *buffer)
{
    if (count == NULL) return XR_ERROR_VALIDATION_FAILURE;
    *count = 1;
    if (cap == 0) return XR_SUCCESS;
    if (buffer == NULL) return XR_ERROR_VALIDATION_FAILURE;
    buffer[0] = '\0';
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_GetVulkanInstanceExtensionsKHR(XrInstance instance, XrSystemId id, uint32_t cap,
                                                                        uint32_t *count, char *buffer)
{
    SYSTEM_CHECK(instance, id);
    return empty_list(cap, count, buffer);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_GetVulkanDeviceExtensionsKHR(XrInstance instance, XrSystemId id, uint32_t cap,
                                                                      uint32_t *count, char *buffer)
{
    SYSTEM_CHECK(instance, id);
    return empty_list(cap, count, buffer);
}

// vkGetInstanceProcAddr: the application's, when it handed one over (vulkan_enable2), else the loader's.
PFN_vkGetInstanceProcAddr vk_loader(struct instance *in)
{
    if (in != NULL && in->vk_gipa != NULL) return in->vk_gipa;
    static PFN_vkGetInstanceProcAddr gipa;
#ifdef _WIN32
    static const char *const libs[] = { "vulkan-1.dll", NULL };
#else
    static const char *const libs[] = { "libvulkan.so.1", NULL };
#endif
    if (gipa == NULL) gipa = (PFN_vkGetInstanceProcAddr)plat_symbol(libs, "vkGetInstanceProcAddr");
    return gipa;
}

// THE GPU: the first discrete one, else the first integrated, else the first. Every Vulkan device can
// do what the runtime asks of it, so this is only about which one an application would want.
static XrResult pick_device(struct instance *in, VkInstance vki, VkPhysicalDevice *out)
{
    PFN_vkGetInstanceProcAddr gipa = vk_loader(in);
    if (gipa == NULL || vki == VK_NULL_HANDLE) return XR_ERROR_RUNTIME_FAILURE;
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(vki, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties props = (PFN_vkGetPhysicalDeviceProperties)gipa(vki, "vkGetPhysicalDeviceProperties");
    if (enumerate == NULL || props == NULL) return XR_ERROR_RUNTIME_FAILURE;
    VkPhysicalDevice devs[16];
    uint32_t n = 16;
    if (enumerate(vki, &n, devs) < 0 || n == 0) return XR_ERROR_RUNTIME_FAILURE;
    int best = -1, best_rank = 99;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        props(devs[i], &p);
        const int rank = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 0
                       : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 2;
        if (rank < best_rank) { best = (int)i; best_rank = rank; }
    }
    VkPhysicalDeviceProperties p;
    props(devs[best], &p);
    say("Vulkan device: %s", p.deviceName);
    *out = devs[best];
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_GetVulkanGraphicsDeviceKHR(XrInstance instance, XrSystemId id, VkInstance vki,
                                                                    VkPhysicalDevice *out)
{
    SYSTEM_CHECK(instance, id);
    if (out == NULL) return XR_ERROR_VALIDATION_FAILURE;
    return pick_device(in, vki, out);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_GetVulkanGraphicsDevice2KHR(XrInstance instance, const XrVulkanGraphicsDeviceGetInfoKHR *info,
                                                                     VkPhysicalDevice *out)
{
    if (info == NULL || out == NULL || info->type != XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR) return XR_ERROR_VALIDATION_FAILURE;
    SYSTEM_CHECK(instance, info->systemId);
    return pick_device(in, info->vulkanInstance, out);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_CreateVulkanInstanceKHR(XrInstance instance, const XrVulkanInstanceCreateInfoKHR *info,
                                                                 VkInstance *out, VkResult *vk_result)
{
    if (info == NULL || out == NULL || vk_result == NULL || info->type != XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR)
        return XR_ERROR_VALIDATION_FAILURE;
    SYSTEM_CHECK(instance, info->systemId);
    if (info->pfnGetInstanceProcAddr == NULL || info->vulkanCreateInfo == NULL) return XR_ERROR_VALIDATION_FAILURE;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)info->pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (create == NULL) return XR_ERROR_RUNTIME_FAILURE;
    *vk_result = create(info->vulkanCreateInfo, info->vulkanAllocator, out);
    if (*vk_result == VK_SUCCESS) {
        in->vk_gipa = info->pfnGetInstanceProcAddr;
        in->vk_instance = *out;
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_CreateVulkanDeviceKHR(XrInstance instance, const XrVulkanDeviceCreateInfoKHR *info,
                                                               VkDevice *out, VkResult *vk_result)
{
    if (info == NULL || out == NULL || vk_result == NULL || info->type != XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR)
        return XR_ERROR_VALIDATION_FAILURE;
    SYSTEM_CHECK(instance, info->systemId);
    if (info->pfnGetInstanceProcAddr == NULL || info->vulkanCreateInfo == NULL) return XR_ERROR_VALIDATION_FAILURE;
    // vkCreateDevice is an instance function: the instance the application made through us, else any
    // instance the loader resolves it for.
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)info->pfnGetInstanceProcAddr(in->vk_instance, "vkCreateDevice");
    if (create == NULL) return XR_ERROR_RUNTIME_FAILURE;
    *vk_result = create(info->vulkanPhysicalDevice, info->vulkanCreateInfo, info->vulkanAllocator, out);
    return XR_SUCCESS;
}

// ---- strings -------------------------------------------------------------------------------------

static XRAPI_ATTR XrResult XRAPI_CALL oxs_ResultToString(XrInstance instance, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE])
{
    if (find(H(instance), MAGIC_INSTANCE) == NULL) return XR_ERROR_HANDLE_INVALID;
#define CASE(name, val) case name: snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "%s", #name); return XR_SUCCESS;
    switch (value) {
        XR_LIST_ENUM_XrResult(CASE)
        default: break;
    }
#undef CASE
    snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "%s_%d", value >= 0 ? "XR_UNKNOWN_SUCCESS" : "XR_UNKNOWN_FAILURE", (int)value);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_StructureTypeToString(XrInstance instance, XrStructureType value,
                                                                char buffer[XR_MAX_STRUCTURE_NAME_SIZE])
{
    if (find(H(instance), MAGIC_INSTANCE) == NULL) return XR_ERROR_HANDLE_INVALID;
// A handful of names are longer than the 64 bytes OpenXR gives this buffer; they are cut, as the
// specification's own buffer size cuts them.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#define CASE(name, val) case name: snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "%s", #name); return XR_SUCCESS;
    switch (value) {
        XR_LIST_ENUM_XrStructureType(CASE)
        default: break;
    }
#undef CASE
#pragma GCC diagnostic pop
    snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "XR_UNKNOWN_STRUCTURE_TYPE_%d", (int)value);
    return XR_SUCCESS;
}

// ---- paths ---------------------------------------------------------------------------------------
//
// Interned for the life of the process, as the specification allows. Applications build their
// input bindings from paths, often before asking for a system.

static char **paths;
static size_t npaths, cap_paths;

static bool path_ok(const char *s)
{
    if (s == NULL || s[0] != '/' || strlen(s) >= XR_MAX_PATH_LENGTH) return false;
    for (const char *p = s; *p; p++) {
        const char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '/')) return false;
        if (c == '/' && (p[1] == '/' || p[1] == '\0') && p != s) return false;
    }
    return strcmp(s, "/") != 0;
}

XrPath path_of(const char *s)
{
    if (!path_ok(s)) return XR_NULL_PATH;
    pthread_mutex_lock(&lock);
    for (size_t i = 0; i < npaths; i++)
        if (strcmp(paths[i], s) == 0) { pthread_mutex_unlock(&lock); return i + 1; }
    XrPath out = XR_NULL_PATH;
    if (npaths == cap_paths) {
        const size_t want = cap_paths ? cap_paths * 2 : 64;
        char **g = realloc(paths, want * sizeof *g);
        if (g != NULL) { paths = g; cap_paths = want; }
    }
    if (npaths < cap_paths && (paths[npaths] = strdup(s)) != NULL) out = ++npaths;
    pthread_mutex_unlock(&lock);
    return out;
}

const char *path_string(XrPath p)
{
    pthread_mutex_lock(&lock);
    const char *s = p >= 1 && p <= npaths ? paths[p - 1] : NULL;
    pthread_mutex_unlock(&lock);
    return s;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_StringToPath(XrInstance instance, const char *s, XrPath *out)
{
    if (find(H(instance), MAGIC_INSTANCE) == NULL) return XR_ERROR_HANDLE_INVALID;
    if (out == NULL) return XR_ERROR_VALIDATION_FAILURE;
    if (!path_ok(s)) return XR_ERROR_PATH_FORMAT_INVALID;
    *out = path_of(s);
    return *out != XR_NULL_PATH ? XR_SUCCESS : XR_ERROR_PATH_COUNT_EXCEEDED;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_PathToString(XrInstance instance, XrPath path, uint32_t cap, uint32_t *count, char *buffer)
{
    if (find(H(instance), MAGIC_INSTANCE) == NULL) return XR_ERROR_HANDLE_INVALID;
    if (count == NULL) return XR_ERROR_VALIDATION_FAILURE;
    const char *s = path_string(path);
    if (s == NULL) return XR_ERROR_PATH_INVALID;
    const uint32_t n = (uint32_t)strlen(s) + 1;
    *count = n;
    if (cap == 0) return XR_SUCCESS;
    if (cap < n) return XR_ERROR_SIZE_INSUFFICIENT;
    memcpy(buffer, s, n);
    return XR_SUCCESS;
}

// ---- XR_EXT_debug_utils, XR_KHR_convert_timespec_time ---------------------------------------------

static XRAPI_ATTR XrResult XRAPI_CALL oxs_CreateDebugUtilsMessengerEXT(XrInstance instance,
    const XrDebugUtilsMessengerCreateInfoEXT *info, XrDebugUtilsMessengerEXT *out)
{
    struct object *in = find(H(instance), MAGIC_INSTANCE);
    if (in == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || out == NULL) return XR_ERROR_VALIDATION_FAILURE;
    struct object *o = make(sizeof *o, MAGIC_MESSENGER, in, NULL);
    if (o == NULL) return XR_ERROR_OUT_OF_MEMORY;
    *out = (XrDebugUtilsMessengerEXT)(uintptr_t)o;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_DestroyDebugUtilsMessengerEXT(XrDebugUtilsMessengerEXT m)
{
    struct object *o = find(H(m), MAGIC_MESSENGER);
    if (o == NULL) return XR_ERROR_HANDLE_INVALID;
    destroy(o);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_SubmitDebugUtilsMessageEXT(XrInstance instance, XrDebugUtilsMessageSeverityFlagsEXT sev,
    XrDebugUtilsMessageTypeFlagsEXT types, const XrDebugUtilsMessengerCallbackDataEXT *data)
{
    (void)sev; (void)types; (void)data;
    return find(H(instance), MAGIC_INSTANCE) != NULL ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_SetDebugUtilsObjectNameEXT(XrInstance instance, const XrDebugUtilsObjectNameInfoEXT *info)
{
    (void)info;
    return find(H(instance), MAGIC_INSTANCE) != NULL ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_SessionBeginDebugUtilsLabelRegionEXT(XrSession session, const XrDebugUtilsLabelEXT *label)
{
    (void)label;
    return find(H(session), MAGIC_SESSION) != NULL ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_SessionEndDebugUtilsLabelRegionEXT(XrSession session)
{
    return find(H(session), MAGIC_SESSION) != NULL ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_SessionInsertDebugUtilsLabelEXT(XrSession session, const XrDebugUtilsLabelEXT *label)
{
    (void)label;
    return find(H(session), MAGIC_SESSION) != NULL ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_ConvertTimespecTimeToTimeKHR(XrInstance instance, const struct timespec *ts, XrTime *t)
{
    if (find(H(instance), MAGIC_INSTANCE) == NULL) return XR_ERROR_HANDLE_INVALID;
    if (ts == NULL || t == NULL) return XR_ERROR_VALIDATION_FAILURE;
    *t = (XrTime)ts->tv_sec * 1000000000 + ts->tv_nsec;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_ConvertTimeToTimespecTimeKHR(XrInstance instance, XrTime t, struct timespec *ts)
{
    if (find(H(instance), MAGIC_INSTANCE) == NULL) return XR_ERROR_HANDLE_INVALID;
    if (ts == NULL || t <= 0) return XR_ERROR_TIME_INVALID;
    ts->tv_sec = t / 1000000000;
    ts->tv_nsec = t % 1000000000;
    return XR_SUCCESS;
}

// ---- the dispatch table --------------------------------------------------------------------------

static XRAPI_ATTR XrResult XRAPI_CALL no_system(void) { return XR_ERROR_SYSTEM_INVALID; }
static XRAPI_ATTR XrResult XRAPI_CALL no_handle(void) { return XR_ERROR_HANDLE_INVALID; }

static const struct entry INSTANCE_FUNCTIONS[] = {
    ENTRY(EnumerateInstanceExtensionProperties) ENTRY(CreateInstance) ENTRY(DestroyInstance)
    ENTRY(GetInstanceProperties) ENTRY(PollEvent) ENTRY(GetSystem) ENTRY(GetSystemProperties)
    ENTRY(EnumerateViewConfigurations) ENTRY(GetViewConfigurationProperties) ENTRY(EnumerateViewConfigurationViews)
    ENTRY(EnumerateEnvironmentBlendModes) ENTRY(GetVulkanGraphicsRequirementsKHR)
    ENTRY(GetOpenGLGraphicsRequirementsKHR) ENTRY(GetOpenGLESGraphicsRequirementsKHR)
    ENTRY(GetVulkanInstanceExtensionsKHR) ENTRY(GetVulkanDeviceExtensionsKHR) ENTRY(GetVulkanGraphicsDeviceKHR)
    ENTRY(GetVulkanGraphicsDevice2KHR) ENTRY(CreateVulkanInstanceKHR) ENTRY(CreateVulkanDeviceKHR)
    ENTRY(ResultToString) ENTRY(StructureTypeToString) ENTRY(StringToPath) ENTRY(PathToString)
    ENTRY(CreateDebugUtilsMessengerEXT) ENTRY(DestroyDebugUtilsMessengerEXT) ENTRY(SubmitDebugUtilsMessageEXT)
    ENTRY(SetDebugUtilsObjectNameEXT) ENTRY(SessionBeginDebugUtilsLabelRegionEXT)
    ENTRY(SessionEndDebugUtilsLabelRegionEXT) ENTRY(SessionInsertDebugUtilsLabelEXT)
    ENTRY(ConvertTimespecTimeToTimeKHR) ENTRY(ConvertTimeToTimespecTimeKHR)
    { NULL, NULL }
};

// XR_KHR_vulkan_enable2's requirements function is enable's under a second name, with the same structure.
static const struct entry ALIASES[] = {
    { "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction)oxs_GetVulkanGraphicsRequirementsKHR },
    { NULL, NULL }
};

// Every function this runtime answers for: core 1.0 and 1.1, and the offered extensions'.
#define NAME(n, ext) "xr" #n,
static const char *const ALL_FUNCTIONS[] = {
    XR_LIST_FUNCTIONS_XR_VERSION_1_0(NAME)
    XR_LIST_FUNCTIONS_XR_VERSION_1_1(NAME)
    XR_LIST_FUNCTIONS_XR_KHR_vulkan_enable(NAME)
    XR_LIST_FUNCTIONS_XR_KHR_vulkan_enable2(NAME)
    XR_LIST_FUNCTIONS_XR_KHR_opengl_enable(NAME)
    XR_LIST_FUNCTIONS_XR_KHR_opengl_es_enable(NAME)
    XR_LIST_FUNCTIONS_XR_KHR_convert_timespec_time(NAME)
    XR_LIST_FUNCTIONS_XR_EXT_debug_utils(NAME)
    XR_LIST_FUNCTIONS_XR_EXT_hand_tracking(NAME)
};
#undef NAME

// The ones without a body, should any be left: those taking a system answer for a system that cannot
// exist, the rest for a handle that cannot.
static bool takes_system(const char *fn)
{
    static const char *const sys[] = { "xrCreateSession", NULL };
    for (int i = 0; sys[i] != NULL; i++)
        if (strcmp(fn, sys[i]) == 0) return true;
    return false;
}

static PFN_xrVoidFunction lookup(const char *name)
{
    const struct entry *tables[] = { INSTANCE_FUNCTIONS, ALIASES, session_functions, swapchain_functions, input_functions,
                                     hand_functions };
    for (size_t t = 0; t < sizeof tables / sizeof tables[0]; t++)
        for (const struct entry *e = tables[t]; e->name != NULL; e++)
            if (strcmp(name, e->name) == 0) return e->fn;
    return NULL;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_GetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *fn)
{
    if (name == NULL || fn == NULL) return XR_ERROR_VALIDATION_FAILURE;
    *fn = NULL;
    if (strcmp(name, "xrGetInstanceProcAddr") == 0) { *fn = (PFN_xrVoidFunction)oxs_GetInstanceProcAddr; return XR_SUCCESS; }
    // WITHOUT AN INSTANCE, only the three functions that may be called before one.
    if (instance == XR_NULL_HANDLE) {
        if (strcmp(name, "xrEnumerateInstanceExtensionProperties") == 0) *fn = (PFN_xrVoidFunction)oxs_EnumerateInstanceExtensionProperties;
        else if (strcmp(name, "xrCreateInstance") == 0) *fn = (PFN_xrVoidFunction)oxs_CreateInstance;
        else if (strcmp(name, "xrEnumerateApiLayerProperties") == 0) *fn = (PFN_xrVoidFunction)no_handle;
        return *fn != NULL ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
    }
    struct instance *in = (struct instance *)find(H(instance), MAGIC_INSTANCE);
    if (in == NULL) return XR_ERROR_HANDLE_INVALID;
    // An extension's functions are there only when it is enabled.
    if (!in->hand_tracking)
        for (const struct entry *e = hand_functions; e->name != NULL; e++)
            if (strcmp(name, e->name) == 0) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if ((*fn = lookup(name)) != NULL) return XR_SUCCESS;
    for (size_t i = 0; i < sizeof ALL_FUNCTIONS / sizeof ALL_FUNCTIONS[0]; i++)
        if (strcmp(name, ALL_FUNCTIONS[i]) == 0) {
            say("%s has no body here", name);
            *fn = takes_system(name) ? (PFN_xrVoidFunction)no_system : (PFN_xrVoidFunction)no_handle;
            return XR_SUCCESS;
        }
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

// ---- the loader ----------------------------------------------------------------------------------

OXS_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo *loader,
                                                                              XrNegotiateRuntimeRequest *request)
{
    if (loader == NULL || request == NULL || loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loader->structVersion != XR_LOADER_INFO_STRUCT_VERSION || loader->structSize != sizeof *loader ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
        request->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION || request->structSize != sizeof *request)
        return XR_ERROR_INITIALIZATION_FAILED;
    if (loader->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION ||
        loader->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;
    if (XR_VERSION_MAJOR(loader->minApiVersion) > 1 || XR_VERSION_MAJOR(loader->maxApiVersion) < 1)
        return XR_ERROR_INITIALIZATION_FAILED;
    request->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    request->runtimeApiVersion = XR_CURRENT_API_VERSION;
    request->getInstanceProcAddr = oxs_GetInstanceProcAddr;
    say("negotiated with the loader");
    return XR_SUCCESS;
}
