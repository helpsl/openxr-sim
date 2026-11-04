// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#pragma once

// THE SIMULATED DEVICES: a headset and two hands, moved by the mouse and keyboard (DESIGN.md, "The
// simulator window"). No window in here -- what is held and how far it has moved, and the poses that
// makes, so the arithmetic can be tested without one.
//
// THE HANDS GO WHERE THE HEAD GOES. Each is kept relative to the BODY -- the head's position, turned by
// the head's yaw alone -- as a person's hands are: walk forward and they come too; look down and they
// stay where they were.

#include "device.h"

#include <array>
#include <cstdint>

namespace oxs {

enum class Part { Headset = 0, Right = 1, Left = 2 };

// What a hand's buttons can be, held by a key or by the mouse on the panel's button.
enum Control : uint32_t {
    kTrigger = 1, kGrip = 2, kPrimary = 4, kSecondary = 8, kMenu = 16, kStickClick = 32,
    kStickUp = 64, kStickDown = 128, kStickLeft = 256, kStickRight = 512,
};

// Which way something is being moved, from the keys: W A S D, Q E.
struct Motion { int forward = 0, right = 0, up = 0; };

class Rig {
public:
    Rig() { reset(); }

    void reset();
    Part selected() const { return selected_; }
    void select(Part p) { selected_ = p; }
    void select_next() { selected_ = static_cast<Part>((static_cast<int>(selected_) + 1) % 3); }
    // The hand the buttons act on: the selected one, or the right while the headset is selected.
    int button_hand() const { return selected_ == Part::Left ? 0 : 1; }

    // A drag of dx, dy pixels on the view: turns the selected part.
    void turn(double dx, double dy);
    // `dt` seconds of moving as `m` says. `slow`: Shift, a quarter of the speed.
    void step(const Motion& m, double dt, bool slow);

    // THE SAME, BY THE NUMBER (the control socket, src/sim/control.hpp): any part, not only the selected.
    // Turned by degrees (yaw positive to the left, as OpenXR's); moved by metres the way the keys move it --
    // the head along the way it faces, level; a hand in the body's frame.
    void turn_by(Part, double dyaw_deg, double dpitch_deg);
    void move_by(Part, double forward, double right, double up);
    // Put a part where readout() would then say it is: STAGE metres and degrees. A hand stays relative to
    // the body, so this is readout() inverted -- read a pose, write it back, and nothing moves.
    void set_pose(Part, const double pos[3], double yaw_deg, double pitch_deg);

    // What is held on each hand (Control bits), from the keys and the mouse together.
    void hold(int hand, uint32_t controls) { held_[static_cast<size_t>(hand)] = controls; }
    uint32_t held(int hand) const { return held_[static_cast<size_t>(hand)]; }

    oxs_tracking tracking() const;

    // For the panel: the selected part's position (STAGE, metres) and heading and pitch (degrees).
    void readout(Part, double pos[3], double& yaw_deg, double& pitch_deg) const;

private:
    struct Pose { double p[3]; double yaw, pitch; };
    Pose head_{};
    std::array<Pose, 2> hand_{};   // relative to the body; [0] left, [1] right, as oxs_tracking
    std::array<uint32_t, 2> held_{};
    Part selected_ = Part::Headset;
};

}  // namespace oxs
