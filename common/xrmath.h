// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// POSES: a unit quaternion (x y z w) and a position. compose(a, b) is b expressed in a's frame taken to
// a's parent -- "a then b", as matrices A * B. Also used by the simulator.
#ifndef OPENXR_SIM_XRMATH_H
#define OPENXR_SIM_XRMATH_H

#include "device.h"

#include <math.h>
#include <stdio.h>
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

// THE ROOM'S MARKERS AT THE START (DESIGN.md, "Spatial entities"): one of each kind the runtime tracks, so an
// application sees markers without anything being set up. A QR code on the wall ahead, an ArUco marker
// (4x4_50, number 7) lying on a table below it, a Micro QR code to the right of the QR code -- out of view
// once the head turns 30 degrees to the left -- and an AprilTag (36h11, number 3) on the wall to the left,
// out of view until the head turns towards it. The simulator's
// first serial after these is OXS_ROOM_HOME_SERIALS + 1.
#define OXS_ROOM_HOME_SERIALS 4u

// HOW MANY CODES A DICTIONARY HOLDS, by XrSpatialMarkerArucoDictEXT's numbers (1..16: 4x4 to 7x7, each of
// 50, 100, 250 and 1000) or XrSpatialMarkerAprilTagDictEXT's (1..4: 16h5, 25h9, 36h10, 36h11); 0 for a
// dictionary that is neither, or a kind without one.
static inline uint32_t oxs_marker_dict_size(int kind, int dict)
{
    static const uint32_t aruco[4] = { 50, 100, 250, 1000 }, april[4] = { 30, 35, 2320, 587 };
    if (kind == OXS_MARKER_ARUCO && dict >= 1 && dict <= 16) return aruco[(dict - 1) % 4];
    if (kind == OXS_MARKER_APRIL_TAG && dict >= 1 && dict <= 4) return april[dict - 1];
    return 0;
}

// A marker's orientation from where its printed face looks: `yaw` turns it about +Y (0 faces +Z, back
// towards the start; 90 degrees faces +X), `pitch` tilts the face up (90 degrees: lying face up).
static inline void oxs_marker_facing(float yaw, float pitch, float q[4]) { oxs_quat_from_euler(yaw, -pitch, 0, q); }

static inline void oxs_room_home(struct oxs_room *room)
{
    memset(room, 0, sizeof *room);
    static const struct { int kind, dict; uint32_t id; float p[3], yaw, pitch, size; const char *text; } home[] = {
        { OXS_MARKER_QR, 0, 0, { 0, 1.5f, -2 }, 0, 0, 0.2f, "openxr-sim" },
        { OXS_MARKER_ARUCO, 1, 7, { 0, 0.8f, -1.2f }, 0, 1.5707963f, 0.1f, "" },
        { OXS_MARKER_MICRO_QR, 0, 0, { 0.8f, 1.5f, -2 }, 0, 0, 0.15f, "OXS" },
        { OXS_MARKER_APRIL_TAG, 4, 3, { -2, 1.5f, -1.2f }, 1.5707963f, 0, 0.2f, "" },
    };
    for (uint32_t i = 0; i < sizeof home / sizeof home[0]; i++) {
        struct oxs_marker *m = &room->marker[i];
        m->serial = i + 1;
        m->kind = home[i].kind;
        m->dict = home[i].dict;
        m->id = home[i].id;
        oxs_marker_facing(home[i].yaw, home[i].pitch, m->pose.q);
        memcpy(m->pose.p, home[i].p, sizeof m->pose.p);
        m->size = home[i].size;
        snprintf(m->text, sizeof m->text, "%s", home[i].text);
    }
    room->count = sizeof home / sizeof home[0];
}

// WHETHER THE HEADSET SEES A MARKER: its centre inside the 90-degree field both eyes share (tangents of 1
// each way, from the head), its printed face turned towards the head by less than 70 degrees, and near
// enough to read -- the square at least a twentieth of its distance across (a 20 cm code to 4 m).
static inline bool oxs_marker_seen(const struct oxs_pose *head, const struct oxs_marker *m)
{
    const struct oxs_pose in_head = oxs_pose_relative(head, &m->pose);
    const float *c = in_head.p;
    const float dist = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    if (c[2] >= -0.05f || fabsf(c[0]) > -c[2] || fabsf(c[1]) > -c[2]) return false;
    if (m->size <= 0 || m->size < dist / 20) return false;
    const float z[3] = { 0, 0, 1 };
    float face[3];
    oxs_quat_rotate(in_head.q, z, face);
    // From the marker towards the head (the head's origin) is -c.
    return -(face[0] * c[0] + face[1] * c[1] + face[2] * c[2]) > cosf(1.2217305f) * dist;
}

#endif
