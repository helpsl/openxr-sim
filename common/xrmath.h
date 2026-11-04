// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// POSES: a unit quaternion (x y z w) and a position. compose(a, b) is b expressed in a's frame taken to
// a's parent -- "a then b", as matrices A * B. Also used by the simulator.
#ifndef OPENXR_SIM_XRMATH_H
#define OPENXR_SIM_XRMATH_H

#include "device.h"

#include <math.h>
#include <string.h>

static inline struct oxs_pose oxs_pose_identity(void)
{
    struct oxs_pose p = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
    return p;
}

static inline void oxs_quat_mul(const float a[4], const float b[4], float out[4])
{
    const float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    const float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    const float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    const float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = x; out[1] = y; out[2] = z; out[3] = w;
}

static inline void oxs_quat_rotate(const float q[4], const float v[3], float out[3])
{
    // v + 2w(u x v) + 2u x (u x v), u the vector part
    const float ux = q[0], uy = q[1], uz = q[2], w = q[3];
    const float tx = 2 * (uy * v[2] - uz * v[1]);
    const float ty = 2 * (uz * v[0] - ux * v[2]);
    const float tz = 2 * (ux * v[1] - uy * v[0]);
    out[0] = v[0] + w * tx + (uy * tz - uz * ty);
    out[1] = v[1] + w * ty + (uz * tx - ux * tz);
    out[2] = v[2] + w * tz + (ux * ty - uy * tx);
}

static inline void oxs_quat_normalize(float q[4])
{
    const float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n < 1e-6f) { q[0] = q[1] = q[2] = 0; q[3] = 1; return; }
    for (int i = 0; i < 4; i++) q[i] /= n;
}

static inline struct oxs_pose oxs_pose_compose(const struct oxs_pose *a, const struct oxs_pose *b)
{
    struct oxs_pose r;
    oxs_quat_mul(a->q, b->q, r.q);
    oxs_quat_normalize(r.q);
    float moved[3];
    oxs_quat_rotate(a->q, b->p, moved);
    for (int i = 0; i < 3; i++) r.p[i] = a->p[i] + moved[i];
    return r;
}

static inline struct oxs_pose oxs_pose_invert(const struct oxs_pose *a)
{
    struct oxs_pose r;
    r.q[0] = -a->q[0]; r.q[1] = -a->q[1]; r.q[2] = -a->q[2]; r.q[3] = a->q[3];
    const float neg[3] = { -a->p[0], -a->p[1], -a->p[2] };
    oxs_quat_rotate(r.q, neg, r.p);
    return r;
}

// `b` as seen from `a`, both in the same parent: inverse(a) * b.
static inline struct oxs_pose oxs_pose_relative(const struct oxs_pose *a, const struct oxs_pose *b)
{
    const struct oxs_pose ia = oxs_pose_invert(a);
    return oxs_pose_compose(&ia, b);
}

// Turned `yaw` about +Y (positive turns left), then `pitch` about the turned +X (positive looks up), then
// `roll` about the turned -Z.
static inline void oxs_quat_from_euler(float yaw, float pitch, float roll, float q[4])
{
    const float qy[4] = { 0, sinf(yaw / 2), 0, cosf(yaw / 2) };
    const float qp[4] = { sinf(pitch / 2), 0, 0, cosf(pitch / 2) };
    const float qr[4] = { 0, 0, sinf(roll / 2), cosf(roll / 2) };
    float t[4];
    oxs_quat_mul(qy, qp, t);
    oxs_quat_mul(t, qr, q);
    oxs_quat_normalize(q);
}

// A valid unit quaternion, within the tolerance OpenXR allows an application (1%).
static inline bool oxs_quat_valid(const float q[4])
{
    const float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    return n > 0.98f && n < 1.02f && n == n;
}

// A CONTROLLER'S TWO POSES from where the hand points. The aim pose is that pointing, 5 cm ahead. The
// grip pose is OpenXR's: its -Z along the handle through the fist, from the little finger to the thumb,
// which in a hand held out and pointing ahead rises 60 degrees above the pointing -- the handle's top
// forward, its bottom under the wrist. Hand tracking (hand.c) closes the fingers round that handle.
#define OXS_GRIP_PITCH 1.0471976f
static inline void oxs_hand_poses(const struct oxs_pose *pointing, struct oxs_hand *hand)
{
    const struct oxs_pose up = { { sinf(OXS_GRIP_PITCH / 2), 0, 0, cosf(OXS_GRIP_PITCH / 2) }, { 0, 0, 0 } };
    const struct oxs_pose ahead = { { 0, 0, 0, 1 }, { 0, 0, -0.05f } };
    hand->grip = oxs_pose_compose(pointing, &up);
    hand->aim = oxs_pose_compose(pointing, &ahead);
}

// THE STARTING POSE (DESIGN.md, "What the simulated headset is"): standing at the stage's origin, eyes
// 1.6 m up, facing -Z; the hands a little forward and down, either side, pointing ahead. Shared so the
// runtime has the same poses before the simulator has written any, and R in the simulator returns to them.
static inline void oxs_tracking_home(struct oxs_tracking *t)
{
    t->head = oxs_pose_identity();
    t->head.p[1] = 1.6f;
    for (int h = 0; h < 2; h++) {
        struct oxs_hand *hand = &t->hand[h];
        memset(hand, 0, sizeof *hand);
        struct oxs_pose pointing = oxs_pose_identity();
        pointing.p[0] = h == 0 ? -0.2f : 0.2f;
        pointing.p[1] = 1.25f;
        pointing.p[2] = -0.35f;
        oxs_hand_poses(&pointing, hand);
    }
}

#endif
