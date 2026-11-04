// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#include "rig.hpp"

#include "xrmath.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace oxs {

namespace {

constexpr double kTurnPerPixel = 0.005;            // radians: a 600-pixel drag is most of a half turn
constexpr double kPitchLimit = 1.45;               // just short of straight up or down
constexpr double kHeadSpeed = 1.0, kHandSpeed = 0.5;   // metres a second; a quarter with Shift

oxs_pose pose_of(const double p[3], double yaw, double pitch) {
    oxs_pose r{};
    oxs_quat_from_euler(static_cast<float>(yaw), static_cast<float>(pitch), 0, r.q);
    for (int i = 0; i < 3; i++) r.p[i] = static_cast<float>(p[i]);
    return r;
}

}  // namespace

// WHERE EVERYTHING STARTS: oxs_tracking_home, the runtime's poses before the simulator has written any,
// so R puts the devices exactly where an application first saw them.
void Rig::reset() {
    oxs_tracking home{};
    oxs_tracking_home(&home);
    head_ = { { home.head.p[0], home.head.p[1], home.head.p[2] }, 0, 0 };
    for (size_t h = 0; h < 2; h++) {
        const oxs_pose& g = home.hand[h].grip;
        hand_[h] = { { g.p[0] - home.head.p[0], g.p[1] - home.head.p[1], g.p[2] - home.head.p[2] }, 0, 0 };
    }
}

void Rig::turn(double dx, double dy) {
    // Dragging right turns right, which is a negative yaw (about +Y); dragging up looks up.
    turn_by(selected_, -dx * kTurnPerPixel * 180 / std::numbers::pi, -dy * kTurnPerPixel * 180 / std::numbers::pi);
}

void Rig::turn_by(Part part, double dyaw_deg, double dpitch_deg) {
    Pose& p = part == Part::Headset ? head_ : hand_[part == Part::Left ? 0 : 1];
    p.yaw = std::remainder(p.yaw + dyaw_deg * std::numbers::pi / 180, 2 * std::numbers::pi);
    p.pitch = std::clamp(p.pitch + dpitch_deg * std::numbers::pi / 180, -kPitchLimit, kPitchLimit);
}

void Rig::step(const Motion& m, double dt, bool slow) {
    if (m.forward == 0 && m.right == 0 && m.up == 0) return;
    const double d = (selected_ == Part::Headset ? kHeadSpeed : kHandSpeed) * dt * (slow ? 0.25 : 1);
    move_by(selected_, d * m.forward, d * m.right, d * m.up);
}

void Rig::move_by(Part part, double forward, double right, double up) {
    const bool head = part == Part::Headset;
    // Forward is -Z. The head walks the way it faces, level; a hand moves in the body's frame, which is
    // already turned with the head, so its forward is the body's.
    const double yaw = head ? head_.yaw : 0;
    const double fx = -std::sin(yaw), fz = -std::cos(yaw);   // forward
    const double rx = std::cos(yaw), rz = -std::sin(yaw);    // right
    double* p = head ? head_.p : hand_[part == Part::Left ? 0 : 1].p;
    p[0] += forward * fx + right * rx;
    p[2] += forward * fz + right * rz;
    p[1] += up;
    if (head) p[1] = std::max(0.1, p[1]);   // not through the floor
}

void Rig::set_pose(Part part, const double pos[3], double yaw_deg, double pitch_deg) {
    const double yaw = yaw_deg * std::numbers::pi / 180;
    const double pitch = std::clamp(pitch_deg * std::numbers::pi / 180, -kPitchLimit, kPitchLimit);
    if (part == Part::Headset) {
        head_ = { { pos[0], std::max(0.1, pos[1]), pos[2] }, std::remainder(yaw, 2 * std::numbers::pi), pitch };
        return;
    }
    // INTO THE BODY'S FRAME: the offset from the head, turned back by the head's yaw; the heading less the
    // body's. tracking() turns it forward again.
    const double dx = pos[0] - head_.p[0], dz = pos[2] - head_.p[2];
    const double c = std::cos(head_.yaw), s = std::sin(head_.yaw);
    Pose& h = hand_[part == Part::Left ? 0 : 1];
    h.p[0] = c * dx - s * dz;
    h.p[1] = pos[1] - head_.p[1];
    h.p[2] = s * dx + c * dz;
    h.yaw = std::remainder(yaw - head_.yaw, 2 * std::numbers::pi);
    h.pitch = pitch;
}

oxs_tracking Rig::tracking() const {
    oxs_tracking t{};
    t.head = pose_of(head_.p, head_.yaw, head_.pitch);
    const double zero[3] = { 0, 0, 0 };
    oxs_pose body = pose_of(zero, head_.yaw, 0);
    for (int i = 0; i < 3; i++) body.p[i] = static_cast<float>(head_.p[i]);
    for (size_t h = 0; h < 2; h++) {
        const oxs_pose local = pose_of(hand_[h].p, hand_[h].yaw, hand_[h].pitch);
        oxs_hand& hand = t.hand[h];
        const oxs_pose pointing = oxs_pose_compose(&body, &local);
        oxs_hand_poses(&pointing, &hand);   // as oxs_tracking_home has them
        const uint32_t c = held_[h];
        hand.trigger = (c & kTrigger) != 0 ? 1.0f : 0.0f;
        hand.squeeze = (c & kGrip) != 0 ? 1.0f : 0.0f;
        hand.stick[0] = static_cast<float>(((c & kStickRight) != 0) - ((c & kStickLeft) != 0));
        hand.stick[1] = static_cast<float>(((c & kStickUp) != 0) - ((c & kStickDown) != 0));
        hand.buttons = ((c & kPrimary) != 0 ? OXS_BUTTON_PRIMARY : 0) | ((c & kSecondary) != 0 ? OXS_BUTTON_SECONDARY : 0) |
                       ((c & kMenu) != 0 ? OXS_BUTTON_MENU : 0) | ((c & kStickClick) != 0 ? OXS_BUTTON_STICK : 0);
    }
    return t;
}

void Rig::readout(Part part, double pos[3], double& yaw_deg, double& pitch_deg) const {
    const oxs_tracking t = tracking();
    const Pose& p = part == Part::Headset ? head_ : hand_[part == Part::Left ? 0 : 1];
    const oxs_pose& world = part == Part::Headset ? t.head : t.hand[part == Part::Left ? 0 : 1].grip;
    for (int i = 0; i < 3; i++) pos[i] = world.p[i];
    // A hand's heading is the body's plus its own: the way it points in the room.
    yaw_deg = std::remainder(p.yaw + (part == Part::Headset ? 0 : head_.yaw), 2 * std::numbers::pi) * 180 / std::numbers::pi;
    pitch_deg = p.pitch * 180 / std::numbers::pi;
}

}  // namespace oxs
