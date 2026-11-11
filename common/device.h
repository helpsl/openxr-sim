// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// THE DEVICE BEHIND THE RUNTIME. The runtime is OpenXR -- handles, extensions, paths, sessions, the
// loader -- and asks a device only what a headset would answer. Two devices, chosen with
// OPENXR_SIM_DEVICE or `device =` in the configuration file (runtime/plat.h) (DESIGN.md, "Choosing the device"):
//   null  a headset that is not connected (device_null.c), the default;
//   sim   the simulated headset (device_sim.c): a window showing both eyes, the poses and buttons
//         driven by the mouse and keyboard (openxr-sim).
// Also included by the simulator, which is C++: plain structs, nothing of OpenXR's.
#ifndef OPENXR_SIM_DEVICE_H
#define OPENXR_SIM_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A pose: an orientation (a unit quaternion, x y z w) and a position in metres. OpenXR's axes: +X right,
// +Y up, -Z forward.
struct oxs_pose { float q[4]; float p[3]; };

enum {
    OXS_BUTTON_PRIMARY = 1,     // A on the right hand, X on the left
    OXS_BUTTON_SECONDARY = 2,   // B, Y
    OXS_BUTTON_MENU = 4,
    OXS_BUTTON_STICK = 8,       // the thumbstick pressed in
    OXS_BUTTON_SYSTEM = 16,
};

struct oxs_hand {
    struct oxs_pose grip, aim;   // in STAGE
    float trigger, squeeze;         // 0..1
    float stick[2];                 // -1..1, +Y forward
    uint32_t buttons;               // OXS_BUTTON_*
};

// What the simulated devices are doing, in STAGE: the floor at y = 0, the headset's start above the origin.
struct oxs_tracking {
    struct oxs_pose head;        // between the eyes
    struct oxs_hand hand[2];     // 0 left, 1 right
};

// SPATIAL MARKERS (DESIGN.md, "Spatial entities"): printed codes in the room, which the simulator owns and the
// runtime reports through XR_EXT_spatial_marker_tracking when the headset can see them (oxs_marker_seen,
// common/xrmath.h). A marker's pose is its centre, +X to the right of the printed face, +Y to its top, +Z
// out of the face -- the extension's own convention.
enum oxs_marker_kind { OXS_MARKER_QR = 1, OXS_MARKER_MICRO_QR, OXS_MARKER_ARUCO, OXS_MARKER_APRIL_TAG };
#define OXS_MARKERS_MAX 16
#define OXS_MARKER_TEXT 256   // a QR code's text, NUL included

struct oxs_marker {
    uint32_t serial;          // never reused: a marker removed and another added is a new entity
    int32_t kind;             // enum oxs_marker_kind
    int32_t dict;             // ArUco, AprilTag: the dictionary, by XrSpatialMarkerArucoDictEXT's or
                              // XrSpatialMarkerAprilTagDictEXT's numbers; else 0
    uint32_t id;              // ArUco, AprilTag: the code's number in its dictionary; else 0
    struct oxs_pose pose;     // in STAGE
    float size;               // the side of the printed square, metres
    char text[OXS_MARKER_TEXT];   // QR, Micro QR: what it encodes
};

struct oxs_room {
    uint32_t count;
    struct oxs_marker marker[OXS_MARKERS_MAX];
};

// What the application drew for one eye: the rectangle of the image it named, in its own byte order.
struct oxs_eye {
    const uint8_t *pixels;
    uint32_t width, height, stride;
    bool bgra;      // B G R A in memory, else R G B A
    bool linear;    // the values are linear light, not sRGB
    bool flipped;   // the rows run bottom to top (OpenGL)
};

// COMPOSITION LAYERS (DESIGN.md, "Composition layers"): a frame of anything but one plain projection
// layer is composited by the simulator (src/composite.c). The runtime resolves each layer for each eye:
// its pose in STAGE at the frame's display time, and its sub-image.
#define OXS_LAYERS_MAX 16    // XR_MIN_COMPOSITION_LAYERS_SUPPORTED, which the runtime reports

enum oxs_layer_kind { OXS_PROJECTION, OXS_QUAD, OXS_CYLINDER, OXS_EQUIRECT };

struct oxs_layer {
    int kind;                    // enum oxs_layer_kind
    bool source_alpha, unpremultiplied;    // XR_COMPOSITION_LAYER_*_BIT
    bool scale_bias;                       // XR_KHR_composition_layer_color_scale_bias
    bool own_view;               // projection: the one the eye is seen through (its image is the frame's eye)
    float scale[4], bias[4];
    struct oxs_pose pose;     // projection: the view's eye; quad: its centre; cylinder, equirect: the centre
    float tan_left, tan_right, tan_up, tan_down;   // projection: the view's field, as tangents
    float size[2];               // quad: metres
    float radius;                // cylinder, equirect: 0 or infinity for infinitely far
    float angle;                 // cylinder: centralAngle; equirect: centralHorizontalAngle
    float aspect;                // cylinder
    float upper, lower;          // equirect: the vertical angles
    struct oxs_eye image;     // the sub-image
};

// An eye as the layers are composited into it: where it is in STAGE, its field as tangents, and the size
// the application drew it at (the first projection layer's), which the simulator shows it no larger than.
struct oxs_camera {
    struct oxs_pose pose;
    float tan_left, tan_right, tan_up, tan_down;
    uint32_t width, height;
};

struct oxs_frame {
    struct oxs_eye eye[2];    // the first projection layer's eyes; none in a frame without one
    // Composited frames only (nlayers > 0): every layer, in the order submitted, for each eye.
    uint32_t nlayers[2];
    struct oxs_layer layer[2][OXS_LAYERS_MAX];
    struct oxs_camera camera[2];
    int64_t display_time;
    uint64_t number;
};

// What the simulator shows about the application.
struct oxs_status {
    const char *app;
    const char *profile;    // the interaction profile the controllers claim to be, or ""
    int state;              // XrSessionState
    float fps;
    float haptic[2];        // the vibration the application asked of each hand, 0..1
};

struct oxs_device {
    const char *name;            // OPENXR_SIM_DEVICE=<name>
    const char *system_name;     // XrSystemProperties.systemName
    // Whether a system of this form factor exists now: 0 (XR_SUCCESS) with *id set, or the error
    // OpenXR defines -- XR_ERROR_FORM_FACTOR_UNAVAILABLE ("supported, not connected": the application
    // keeps polling) or XR_ERROR_FORM_FACTOR_UNSUPPORTED ("never"). An XrResult, as an int here.
    int (*get_system)(int form_factor, uint64_t *id);
    // With a system; the null device leaves these NULL. open() shows the headset (the session was
    // created) and says why not into `why`; close() takes it away. track() and present() answer false
    // once the headset has gone -- the window was closed -- which ends the session.
    bool (*open)(const char *app, char *why, size_t why_size);
    void (*close)(void);
    bool (*track)(struct oxs_tracking *out);
    bool (*present)(const struct oxs_frame *frame);
    void (*status)(const struct oxs_status *status);
    // The markers in the room now; false once the headset has gone.
    bool (*room)(struct oxs_room *out);
};

extern const struct oxs_device oxs_device_null;
extern const struct oxs_device oxs_device_sim;

#ifdef __cplusplus
}
#endif

#endif
