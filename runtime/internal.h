// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// WHAT THE RUNTIME'S FILES SHARE: the handle objects, the session, the logger. runtime.c is the
// loader, the instance and the dispatch table; session.c the session's states, its frames and its
// spaces; swapchain.c with swapchain_vk.c and swapchain_gl.c the images; input.c the actions. DESIGN.md
// says what the simulated headset is.
#ifndef OPENXR_SIM_INTERNAL_H
#define OPENXR_SIM_INTERNAL_H

// The graphics APIs, without their platforms: the GL bindings' structs (Xlib, XCB, Wayland) need those
// systems' headers and are never read -- only their type says "OpenGL" -- and EGL's is mirrored in
// swapchain_gl.c.
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_OPENGL
#define XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_USE_TIMESPEC
// OpenGL ES's guard declares one structure (XR_FB_swapchain_update_state_opengl_es) in EGL's enum type,
// which is EGL's `unsigned int`.
typedef unsigned int EGLenum;
#include <time.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "plat.h"
#include "threads.h"
#include <stdbool.h>
#include <stdint.h>

#include "device.h"
#include "xrmath.h"

#define RUNTIME_NAME "openxr-sim"
#define RUNTIME_VERSION XR_MAKE_VERSION(0, 7, 0)

void say(const char *fmt, ...) OXS_PRINTF(1, 2);
const struct oxs_device *device(void);

// XrTime is CLOCK_MONOTONIC in nanoseconds, which is also what XR_KHR_convert_timespec_time converts.
XrTime now_ns(void);

// ---- handles -------------------------------------------------------------------------------------
//
// Every handle is a pointer to an object that starts with a `struct object`, and is in a list of live
// objects: a stale or foreign handle is XR_ERROR_HANDLE_INVALID, never a dereference. Destroying an
// instance destroys everything made from it, and a session everything made from the session -- each
// object's `fini` first, so a swapchain lets go of its images before the session lets go of the device.

enum {
    MAGIC_INSTANCE = 0x4c584931, MAGIC_ACTION_SET, MAGIC_ACTION, MAGIC_MESSENGER,
    MAGIC_SESSION, MAGIC_SPACE, MAGIC_SWAPCHAIN, MAGIC_HAND_TRACKER,
};

struct object {
    uint32_t magic;
    struct object *instance;   // itself, for an instance
    struct object *parent;     // the session, for a space or a swapchain; else NULL
    struct object *next;
    void (*fini)(struct object *);
};

extern pthread_mutex_t lock;   // the live list, the path table, the event queue

void *make(size_t size, uint32_t magic, struct object *instance, struct object *parent);
struct object *find(uint64_t handle, uint32_t magic);
void destroy(struct object *gone);
#define H(o) ((uint64_t)(uintptr_t)(o))

// The two-call idiom every enumeration uses.
#define TWO_CALL(cap, count_out, out, items, n)                                                       \
    do {                                                                                              \
        if ((count_out) == NULL) return XR_ERROR_VALIDATION_FAILURE;                                  \
        *(count_out) = (uint32_t)(n);                                                                 \
        if ((cap) == 0) return XR_SUCCESS;                                                            \
        if ((cap) < (uint32_t)(n)) return XR_ERROR_SIZE_INSUFFICIENT;                                 \
        for (uint32_t i_ = 0; i_ < (uint32_t)(n); i_++) (out)[i_] = (items)[i_];                       \
    } while (0)

// Each file's functions for the dispatch table, ended by { NULL, NULL }.
struct entry { const char *name; PFN_xrVoidFunction fn; };
extern const struct entry session_functions[], swapchain_functions[], input_functions[], hand_functions[];
#define ENTRY(n) { "xr" #n, (PFN_xrVoidFunction)oxs_##n },

// ---- the instance --------------------------------------------------------------------------------

struct instance {
    struct object o;
    char app_name[XR_MAX_APPLICATION_NAME_SIZE];
    bool headless;                  // XR_MND_headless enabled
    // Composition layers beyond the core's, usable only when enabled (session.c, oxs_EndFrame).
    bool cylinder, equirect2, color_scale_bias;
    bool hand_tracking;             // XR_EXT_hand_tracking enabled: its functions are there (hand.c)
    bool hand_interaction;          // XR_EXT_hand_interaction enabled: its profile and the pinch and poke poses (input.c)
    bool asked_vulkan, asked_gl;    // the graphics requirements were asked, as OpenXR insists
    PFN_vkGetInstanceProcAddr vk_gipa;   // from xrCreateVulkanInstanceKHR, when the application used it
    VkInstance vk_instance;
    XrEventDataBuffer events[32];   // a ring; the oldest is dropped when it is full
    unsigned event_head, event_count;
    struct session *session;        // at most one at a time
    struct action_set **sets;       // every action set, for bindings and xrSyncActions
    size_t nsets;
    // Suggested bindings, per interaction profile: what xrSuggestInteractionProfileBindings said last.
    struct suggestion { XrPath profile; XrActionSuggestedBinding *b; uint32_t n; } *suggested;
    size_t nsuggested;
};

void post_event(struct instance *in, const void *event, size_t size);

// Each eye's recommended size (xrEnumerateViewConfigurationViews), and the size a frame of no projection
// layer is composited at.
#define EYE_RECOMMENDED 1024

// The one system the simulated device has. Any other id is XR_ERROR_SYSTEM_INVALID.
#define OXS_SYSTEM_ID ((XrSystemId)0x105)
bool system_ok(struct instance *in, XrSystemId id);

// Paths, interned for the life of the process.
const char *path_string(XrPath p);   // NULL for an unknown path; the string lives forever
XrPath path_of(const char *s);       // XR_NULL_PATH for a malformed one

// ---- the session ---------------------------------------------------------------------------------

enum graphics { GFX_NONE, GFX_VULKAN, GFX_GL };

struct vk {
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkPhysicalDeviceMemoryProperties memory;
    int64_t formats[8];          // what this device can draw into and copy from
    uint32_t nformats;
#define VK_FN(name) PFN_##name name;
#include "vk_functions.h"
#undef VK_FN
};

struct gl;   // swapchain_gl.c

struct session {
    struct object o;
    struct instance *in;
    enum graphics gfx;
    struct vk vk;
    struct gl *gl;
    pthread_mutex_t frame_lock;
    pthread_cond_t frame_cond;
    XrSessionState state;
    bool running;          // between xrBeginSession and xrEndSession
    bool exit_requested;   // xrRequestExitSession, or the window was closed
    bool device_open;
    uint64_t waited, begun, ended;   // frames through xrWaitFrame, xrBeginFrame, xrEndFrame
    XrTime last_display;             // the predicted display time xrWaitFrame last gave
    XrTime frame_period;
    struct oxs_pose local_origin; // where LOCAL is, in STAGE: the head where the session began
    pthread_mutex_t track_lock;      // `tracking`: read from the render thread and the input thread alike
    struct oxs_tracking tracking; // the devices, as last read
    // Actions
    struct action_set **attached;
    size_t nattached;
    XrPath profile;                  // the interaction profile the controllers claim to be
    float haptic[2];                 // the vibration asked of each hand, shown in the simulator
    XrTime haptic_until[2];
    // Frame-rate measurement, shown in the simulator.
    XrTime fps_since;
    unsigned fps_frames;
    float fps;
};

// The devices now, read from the simulator: there is no prediction, as nothing it reports moves on its own.
struct oxs_tracking session_tracking(struct session *s);
void session_status(struct session *s);   // tells the simulator the state, the frame rate, the profile
void session_poll(struct session *s);   // notices a closed window: the session is asked to stop

// vkGetInstanceProcAddr: the application's when it handed one over (vulkan_enable2), else the loader's.
PFN_vkGetInstanceProcAddr vk_loader(struct instance *in);

// ---- spaces --------------------------------------------------------------------------------------

struct space {
    struct object o;
    struct session *session;
    XrReferenceSpaceType ref;   // 0 for an action space
    struct action *action;
    XrPath subaction;
    struct oxs_pose offset;  // poseInReferenceSpace / poseInActionSpace
};

// Where a space is in STAGE at `t`, and whether it is tracked (an action space whose action is bound to
// nothing, or not active, is not).
bool space_in_stage(struct space *sp, XrTime t, struct oxs_pose *out);

// ---- swapchains ----------------------------------------------------------------------------------

struct swapchain {
    struct object o;
    struct session *session;
    XrSwapchainCreateInfo info;
    uint32_t count;              // images
    bool color;                  // read back at release; depth images never are
    bool bgra, linear;           // byte order and whether the values are linear (not sRGB)
    uint32_t next_acquire;       // round robin
    uint32_t acquired[4], nacquired;   // in acquisition order
    uint32_t nwaited;            // how many of those, oldest first, have been waited for
    int last_released;           // -1 before the first release
    uint8_t *pixels[4];          // per image: every array layer, tightly packed, after its release
    bool gl_flipped;             // GL's rows run bottom to top
    // The API's own objects
    VkImage vk_image[4];
    VkDeviceMemory vk_memory[4];
    VkBuffer vk_buffer[4];
    VkDeviceMemory vk_buffer_memory[4];
    VkImageLayout vk_layout;
    bool vk_coherent;
    uint32_t gl_texture[4];
};

XrResult vk_session_init(struct session *s, const XrGraphicsBindingVulkanKHR *b);
void vk_session_fini(struct session *s);
const int64_t *vk_formats(struct session *s, uint32_t *n);
XrResult vk_swapchain_init(struct swapchain *sc);
void vk_swapchain_fini(struct swapchain *sc);
XrResult vk_swapchain_read(struct swapchain *sc, uint32_t image);
XrResult vk_enumerate_images(struct swapchain *sc, XrSwapchainImageBaseHeader *images);   // sc->count of them

XrResult gl_session_init(struct session *s, const void *binding);
void gl_session_fini(struct session *s);
const int64_t *gl_formats(uint32_t *n);
XrResult gl_swapchain_init(struct swapchain *sc);
void gl_swapchain_fini(struct swapchain *sc);
XrResult gl_swapchain_read(struct swapchain *sc, uint32_t image);
XrResult gl_enumerate_images(struct swapchain *sc, XrSwapchainImageBaseHeader *images);

// ---- actions -------------------------------------------------------------------------------------

struct action_set {
    struct object o;
    char name[XR_MAX_ACTION_SET_NAME_SIZE];
    uint32_t priority;
    bool attached;
    struct action **actions;
    size_t nactions;
};

struct action_state {        // per subaction path (or the whole action): what the last sync saw
    bool b; float f; XrVector2f v;
    bool active, changed;
    XrTime changed_at;
};

struct action {
    struct object o;             // its parent is its set
    struct action_set *set;
    char name[XR_MAX_ACTION_NAME_SIZE];
    XrActionType type;
    XrPath sub[8];           // the subaction paths it was created with
    uint32_t nsub;
    XrPath bound[16];        // what it is bound to, in the session's chosen profile
    uint32_t nbound;
    struct action_state state[9];   // [0] the action as a whole, [1 + i] for sub[i]
};

void input_choose_profile(struct session *s);
// Where the hand an action space follows is, in STAGE; false when it is bound to no pose.
bool input_pose(struct session *s, struct action *a, XrPath subaction, struct oxs_pose *out);

// XR_EXT_hand_interaction's two poses for a hand (0 left, 1 right) holding a controller at `grip`, in the
// same space as `grip` (hand.c): the pinch, between the index fingertip and the thumb's, and the poke, on
// the surface of the index fingertip -- of the hand hand tracking draws, its index straight.
void hand_pinch_poke(int hand, const struct oxs_pose *grip, struct oxs_pose *pinch, struct oxs_pose *poke);

#endif
