// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// XR_EXT_hand_tracking: the 26 joints of each hand, made from the simulated controllers. The simulator
// has no hands of its own, so a hand is a fixed skeleton held round the controller's grip pose, its
// fingers curled by what is held on it -- the trigger curls the index, the grip the other three, a face
// button or the stick puts the thumb down. DESIGN.md, "Hand tracking", says how it is placed.
#include "internal.h"

#include <string.h>

struct hand_tracker {
    struct object o;
    struct session *session;
    int hand;   // 0 left, 1 right, as oxs_tracking keeps them
};

static XRAPI_ATTR XrResult XRAPI_CALL oxs_CreateHandTrackerEXT(XrSession session, const XrHandTrackerCreateInfoEXT *info,
                                                              XrHandTrackerEXT *out)
{
    struct session *s = (struct session *)find(H(session), MAGIC_SESSION);
    if (s == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || out == NULL || info->type != XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT) return XR_ERROR_VALIDATION_FAILURE;
    if (!s->in->hand_tracking) return XR_ERROR_FEATURE_UNSUPPORTED;
    if (info->hand != XR_HAND_LEFT_EXT && info->hand != XR_HAND_RIGHT_EXT) return XR_ERROR_VALIDATION_FAILURE;
    if (info->handJointSet != XR_HAND_JOINT_SET_DEFAULT_EXT) return XR_ERROR_VALIDATION_FAILURE;
    struct hand_tracker *t = make(sizeof *t, MAGIC_HAND_TRACKER, &s->in->o, &s->o);
    if (t == NULL) return XR_ERROR_OUT_OF_MEMORY;
    t->session = s;
    t->hand = info->hand == XR_HAND_LEFT_EXT ? 0 : 1;
    *out = (XrHandTrackerEXT)(uintptr_t)t;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_DestroyHandTrackerEXT(XrHandTrackerEXT tracker)
{
    struct object *o = find(H(tracker), MAGIC_HAND_TRACKER);
    if (o == NULL) return XR_ERROR_HANDLE_INVALID;
    destroy(o);
    return XR_SUCCESS;
}

// ---- the skeleton --------------------------------------------------------------------------------
//
// The right hand, in metres, in the frame of its palm as the extension has every joint: -Z towards the
// fingers, +Y out of the back of the hand, so the thumb is on -X. Each joint's -Z runs along the bone
// from it to the next towards the tip; the left hand is this one mirrored.

#define DEG (3.14159265f / 180)

struct finger {
    float base[3];      // the METACARPAL joint, at the wrist end of the metacarpal
    float knuckle[3];   // the PROXIMAL joint, where the metacarpal ends
    float length[3];    // proximal, intermediate, distal phalanges
    float radius[5];    // metacarpal .. tip
    float fist[3];      // how far its knuckle, middle and last joints bend round the handle (palm_on_grip)
};

static const struct finger FINGERS[4] = {   // index, middle, ring, little
    { { -0.012f, 0, 0.030f }, { -0.022f, 0, -0.035f }, { 0.040f, 0.024f, 0.020f }, { 0.011f, 0.010f, 0.009f, 0.008f, 0.007f },
      { 25 * DEG, 60 * DEG, 50 * DEG } },
    { { 0, 0, 0.030f }, { 0, 0, -0.037f }, { 0.044f, 0.028f, 0.022f }, { 0.011f, 0.010f, 0.009f, 0.008f, 0.007f },
      { 30 * DEG, 75 * DEG, 55 * DEG } },
    { { 0.010f, 0, 0.030f }, { 0.020f, 0, -0.033f }, { 0.040f, 0.026f, 0.021f }, { 0.010f, 0.009f, 0.008f, 0.007f, 0.007f },
      { 20 * DEG, 60 * DEG, 55 * DEG } },
    { { 0.018f, 0, 0.028f }, { 0.038f, 0, -0.026f }, { 0.032f, 0.019f, 0.018f }, { 0.009f, 0.008f, 0.007f, 0.006f, 0.006f },
      { 10 * DEG, 30 * DEG, 40 * DEG } },
};

// The thumb has no intermediate phalanx: its METACARPAL joint is the one the thumb swings from.
static const float THUMB_BASE[3] = { -0.020f, -0.010f, 0.025f };
static const float THUMB_POINTS[3] = { -0.60f, -0.25f, -0.75f };   // out from the palm towards the fingers
static const float THUMB_NAIL[3] = { -0.7f, 0.7f, 0 };             // roughly where its nail faces
static const float THUMB_LENGTH[3] = { 0.045f, 0.032f, 0.026f };    // metacarpal, proximal, distal
static const float THUMB_RADIUS[4] = { 0.013f, 0.011f, 0.010f, 0.009f };

// How far each joint of a finger bends when it is not held: the knuckle, the middle joint, the last.
static const float RELAXED[3] = { 10 * DEG, 15 * DEG, 10 * DEG };
// The thumb rests on the controller's face, beyond the fist's end; down is that much pressed into it.
static const float THUMB_DOWN[3] = { 5 * DEG, 20 * DEG, 25 * DEG };    // metacarpal, proximal, distal

static void normalize3(float v[3])
{
    const float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (int i = 0; i < 3; i++) v[i] /= n;
}

static void cross(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

// The orientation whose -Z is `ahead` and whose +Y is as near `up` as that allows.
static void look(const float ahead[3], const float up[3], float q[4])
{
    float z[3] = { -ahead[0], -ahead[1], -ahead[2] }, x[3], y[3];
    normalize3(z);
    cross(up, z, x);
    normalize3(x);
    cross(z, x, y);
    // The rotation whose columns are x, y, z, as a quaternion.
    const float t = x[0] + y[1] + z[2];
    if (t > 0) {
        const float r = sqrtf(1 + t) * 2;
        q[3] = r / 4; q[0] = (y[2] - z[1]) / r; q[1] = (z[0] - x[2]) / r; q[2] = (x[1] - y[0]) / r;
    } else if (x[0] > y[1] && x[0] > z[2]) {
        const float r = sqrtf(1 + x[0] - y[1] - z[2]) * 2;
        q[3] = (y[2] - z[1]) / r; q[0] = r / 4; q[1] = (y[0] + x[1]) / r; q[2] = (z[0] + x[2]) / r;
    } else if (y[1] > z[2]) {
        const float r = sqrtf(1 + y[1] - x[0] - z[2]) * 2;
        q[3] = (z[0] - x[2]) / r; q[0] = (y[0] + x[1]) / r; q[1] = r / 4; q[2] = (z[1] + y[2]) / r;
    } else {
        const float r = sqrtf(1 + z[2] - x[0] - y[1]) * 2;
        q[3] = (x[1] - y[0]) / r; q[0] = (z[0] + x[2]) / r; q[1] = (z[1] + y[2]) / r; q[2] = r / 4;
    }
    oxs_quat_normalize(q);
}

// `q` bent by `angle` about its own X, towards the palm (-Z turns to -Y).
static void bend(float q[4], float angle)
{
    const float r[4] = { sinf(-angle / 2), 0, 0, cosf(-angle / 2) };
    float out[4];
    oxs_quat_mul(q, r, out);
    memcpy(q, out, sizeof out);
    oxs_quat_normalize(q);
}

// The next joint along: `length` down the -Z of `from`.
static void along(const struct oxs_pose *from, float length, float out[3])
{
    const float d[3] = { 0, 0, -length };
    oxs_quat_rotate(from->q, d, out);
    for (int i = 0; i < 3; i++) out[i] += from->p[i];
}

// A chain of n bones from `start`, the first oriented `q`: joints[0] is the start, joints[n] the tip. Each
// joint between is bent by its angle[] (n - 1 of them); the tip points as the last bone does.
static void chain(const float start[3], const float q[4], const float *length, const float *angle, int n,
                  struct oxs_pose *joints)
{
    memcpy(joints[0].p, start, sizeof joints[0].p);
    memcpy(joints[0].q, q, sizeof joints[0].q);
    for (int i = 0; i < n; i++) {
        along(&joints[i], length[i], joints[i + 1].p);
        memcpy(joints[i + 1].q, joints[i].q, sizeof joints[i].q);
        if (i < n - 1) bend(joints[i + 1].q, angle[i]);
    }
}

// The right hand in its palm's frame, its fingers curled by curl[0..3] (index..little) and its thumb by
// thumb, each 0..1.
static void skeleton(const float curl[4], float thumb, struct oxs_pose *j, float *radius)
{
    j[XR_HAND_JOINT_PALM_EXT] = oxs_pose_identity();
    radius[XR_HAND_JOINT_PALM_EXT] = 0.020f;
    j[XR_HAND_JOINT_WRIST_EXT] = oxs_pose_identity();
    j[XR_HAND_JOINT_WRIST_EXT].p[2] = 0.040f;
    radius[XR_HAND_JOINT_WRIST_EXT] = 0.025f;

    const float up[3] = { 0, 1, 0 };
    for (int f = 0; f < 4; f++) {
        const struct finger *F = &FINGERS[f];
        const int first = XR_HAND_JOINT_INDEX_METACARPAL_EXT + 5 * f;
        float ahead[3] = { F->knuckle[0] - F->base[0], F->knuckle[1] - F->base[1], F->knuckle[2] - F->base[2] };
        const float metacarpal = sqrtf(ahead[0] * ahead[0] + ahead[1] * ahead[1] + ahead[2] * ahead[2]);
        float q[4];
        look(ahead, up, q);
        const float length[4] = { metacarpal, F->length[0], F->length[1], F->length[2] };
        float angle[3];
        for (int k = 0; k < 3; k++) angle[k] = RELAXED[k] + (F->fist[k] - RELAXED[k]) * curl[f];
        chain(F->base, q, length, angle, 4, &j[first]);
        for (int k = 0; k < 5; k++) radius[first + k] = F->radius[k];
    }

    float q[4];
    look(THUMB_POINTS, THUMB_NAIL, q);
    bend(q, THUMB_DOWN[0] * thumb);
    const float angle[2] = { THUMB_DOWN[1] * thumb, THUMB_DOWN[2] * thumb };
    chain(THUMB_BASE, q, THUMB_LENGTH, angle, 3, &j[XR_HAND_JOINT_THUMB_METACARPAL_EXT]);
    for (int k = 0; k < 4; k++) radius[XR_HAND_JOINT_THUMB_METACARPAL_EXT + k] = THUMB_RADIUS[k];
}

// WHERE THE PALM IS ON THE CONTROLLER, for the right hand, in its grip pose as OpenXR defines one: the
// handle along Z through the closed fist, the thumb's end at -Z; +X out of the back of the hand; so +Y is
// towards the wrist. The palm lies against the handle's side, the handle across it below the knuckles,
// where the curled fingers close round it -- 3 cm under the palm and 5 cm towards the fingers, which with
// each finger's `fist` lays every joint of a full grip 2.7 cm from the axis, round a handle 3.6 cm across.
static struct oxs_pose palm_on_grip(void)
{
    const float ahead[3] = { 0, -1, 0 }, back[3] = { 1, 0, 0 };
    struct oxs_pose p;
    look(ahead, back, p.q);
    p.p[0] = 0.030f; p.p[1] = 0.049f; p.p[2] = 0;
    return p;
}

// The other hand: the same pose reflected in the grip's YZ plane, which is still a rotation.
static void mirror(struct oxs_pose *p)
{
    p->p[0] = -p->p[0];
    p->q[1] = -p->q[1];
    p->q[2] = -p->q[2];
}

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// XR_EXT_HAND_INTERACTION'S PINCH AND POKE, for a controller: "a fixed offset from the grip pose in front of
// the controller", which the extension leaves to the runtime. Taken from the skeleton above with the index
// straight and the thumb up -- the hand as it is when nothing is pressed -- so the poke is where hand
// tracking draws the index fingertip, and neither moves inside the hand when the trigger curls it: the
// specification asks that they rotate with the grip, and a pinch point that ran away into the handle as
// the trigger was pulled would make the pinch the trigger is paired with impossible to aim.
//   * The poke is on the surface of the fingertip, a radius beyond its joint. +Z back along the last bone,
//     +Y out of the back of the hand: the tip joint's own orientation, which is what the extension defines.
//   * The pinch is midway between the index and thumb tips. +Z back towards the middle of their proximal
//     joints; +Y the thumb's side, which is up with the palms facing each other and the fingers ahead.
void hand_pinch_poke(int hand, const struct oxs_pose *grip, struct oxs_pose *pinch, struct oxs_pose *poke)
{
    static const float open[4] = { 0, 0, 0, 0 };
    struct oxs_pose j[XR_HAND_JOINT_COUNT_EXT];
    float radius[XR_HAND_JOINT_COUNT_EXT];
    skeleton(open, 0, j, radius);

    const struct oxs_pose *it = &j[XR_HAND_JOINT_INDEX_TIP_EXT], *tt = &j[XR_HAND_JOINT_THUMB_TIP_EXT];
    struct oxs_pose tip = *it;
    along(it, radius[XR_HAND_JOINT_INDEX_TIP_EXT], tip.p);

    struct oxs_pose mid;
    float knuckles[3], ahead[3];
    const float thumb_side[3] = { -1, 0, 0 };
    for (int i = 0; i < 3; i++) {
        mid.p[i] = (it->p[i] + tt->p[i]) / 2;
        knuckles[i] = (j[XR_HAND_JOINT_INDEX_PROXIMAL_EXT].p[i] + j[XR_HAND_JOINT_THUMB_PROXIMAL_EXT].p[i]) / 2;
        ahead[i] = mid.p[i] - knuckles[i];
    }
    look(ahead, thumb_side, mid.q);

    const struct oxs_pose palm = palm_on_grip();
    struct oxs_pose on_grip[2] = { oxs_pose_compose(&palm, &mid), oxs_pose_compose(&palm, &tip) };
    for (int k = 0; k < 2; k++)
        if (hand == 0) mirror(&on_grip[k]);
    *pinch = oxs_pose_compose(grip, &on_grip[0]);
    *poke = oxs_pose_compose(grip, &on_grip[1]);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_LocateHandJointsEXT(XrHandTrackerEXT tracker, const XrHandJointsLocateInfoEXT *info,
                                                             XrHandJointLocationsEXT *out)
{
    struct hand_tracker *t = (struct hand_tracker *)find(H(tracker), MAGIC_HAND_TRACKER);
    if (t == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || out == NULL || info->type != XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT ||
        out->type != XR_TYPE_HAND_JOINT_LOCATIONS_EXT)
        return XR_ERROR_VALIDATION_FAILURE;
    struct space *base = (struct space *)find(H(info->baseSpace), MAGIC_SPACE);
    if (base == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info->time <= 0) return XR_ERROR_TIME_INVALID;
    if (out->jointCount != XR_HAND_JOINT_COUNT_EXT || out->jointLocations == NULL) return XR_ERROR_VALIDATION_FAILURE;
    XrHandJointVelocitiesEXT *v = NULL;
    for (XrBaseOutStructure *n = out->next; n != NULL; n = n->next)
        if (n->type == XR_TYPE_HAND_JOINT_VELOCITIES_EXT) v = (XrHandJointVelocitiesEXT *)n;
    if (v != NULL && (v->jointCount != XR_HAND_JOINT_COUNT_EXT || v->jointVelocities == NULL)) return XR_ERROR_VALIDATION_FAILURE;

    // Nothing moves on its own, so no velocity is reported, as for spaces.
    if (v != NULL)
        for (uint32_t i = 0; i < XR_HAND_JOINT_COUNT_EXT; i++)
            v->jointVelocities[i] = (XrHandJointVelocityEXT){ 0, { 0, 0, 0 }, { 0, 0, 0 } };

    // A hand is tracked while the session has the input, as actions are.
    struct session *s = t->session;
    struct oxs_pose b;
    out->isActive = s->state == XR_SESSION_STATE_FOCUSED && space_in_stage(base, info->time, &b);
    if (!out->isActive) {
        for (uint32_t i = 0; i < XR_HAND_JOINT_COUNT_EXT; i++)
            out->jointLocations[i] = (XrHandJointLocationEXT){ 0, { { 0, 0, 0, 1 }, { 0, 0, 0 } }, 0 };
        return XR_SUCCESS;
    }

    const struct oxs_hand hand = session_tracking(s).hand[t->hand];
    const bool thumb_down = (hand.buttons & (OXS_BUTTON_PRIMARY | OXS_BUTTON_SECONDARY | OXS_BUTTON_STICK)) != 0 ||
                            fabsf(hand.stick[0]) > 0.1f || fabsf(hand.stick[1]) > 0.1f;
    const float curl[4] = { clamp01(hand.trigger), clamp01(hand.squeeze), clamp01(hand.squeeze), clamp01(hand.squeeze) };
    struct oxs_pose j[XR_HAND_JOINT_COUNT_EXT];
    float radius[XR_HAND_JOINT_COUNT_EXT];
    skeleton(curl, thumb_down ? 1 : 0, j, radius);

    const struct oxs_pose palm = palm_on_grip();
    for (uint32_t i = 0; i < XR_HAND_JOINT_COUNT_EXT; i++) {
        struct oxs_pose on_grip = oxs_pose_compose(&palm, &j[i]);
        if (t->hand == 0) mirror(&on_grip);
        const struct oxs_pose in_stage = oxs_pose_compose(&hand.grip, &on_grip);
        const struct oxs_pose r = oxs_pose_relative(&b, &in_stage);
        out->jointLocations[i] = (XrHandJointLocationEXT){
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT,
            { { r.q[0], r.q[1], r.q[2], r.q[3] }, { r.p[0], r.p[1], r.p[2] } }, radius[i] };
    }
    return XR_SUCCESS;
}

const struct entry hand_functions[] = {
    ENTRY(CreateHandTrackerEXT) ENTRY(DestroyHandTrackerEXT) ENTRY(LocateHandJointsEXT)
    { NULL, NULL }
};
