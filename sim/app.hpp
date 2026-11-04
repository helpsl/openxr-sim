// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#pragma once

// THE SIMULATOR'S WINDOW (DESIGN.md, "The simulator window"): the application's two eyes side by side,
// and a panel -- which device the mouse and keys move, each hand's buttons, the application's state, the
// keys. Started by the runtime with --headset when an application creates a session; started from the
// menu with nothing, it says whether OpenXR applications will use it and lets the person choose.

#include "control.hpp"
#include "link.hpp"
#include "rig.hpp"

#include "los/app_window.hpp"
#include "los/ui.hpp"
#include "los/ui_kit.hpp"
#include "los/ui_theme.hpp"
#include "palette.hpp"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace oxs {

class App {
public:
    explicit App(std::string app_name);
    ~App();

    // Headset mode: the descriptors the runtime handed over.
    bool attach(int shm_fd, int socket_fd, std::string* error) { return link_.attach(shm_fd, socket_fd, error); }

    int  run(std::string* error);
    // THE TEST'S HEADSET (OPENXR_SIM_HEADLESS=DIR): no window. Frames are taken as they come, the
    // devices make one scripted move once frames arrive (headless_script), and when the application has
    // gone DIR gets left.ppm and right.ppm (the last frame's eyes, as converted) and window.png (this
    // window, drawn headless), and stdout one line of what was seen.
    int  headless(const std::string& dir);
    bool render_png(const std::string& path, int w, int h, std::string* error);
    int  layout_check(int w, int h);

private:
    void build(int w, int h);
    void build_view(double w, double h);
    void build_panel(double w, double h);
    void build_hand(int hand, const los::ui::kit::Palette&);
    void build_idle(const los::ui::kit::Palette&);

    void on_key(const los::display::KeyEvent&, bool down);
    void on_button(double x, double y, uint32_t button, bool pressed, int clicks);
    void on_motion(double x, double y);
    void on_frame_ready();
    void tick();
    void apply_input();
    void publish();
    void arm_timer();
    void release_eyes();
    void take_frame(int max_w, int max_h);

    // THE CONTROL SOCKET (src/sim/control.hpp, src/sim/commands.cpp): headset mode only.
    void start_control();
    std::string command(const std::string& line);
    std::string status_json() const;
    std::string poses_json() const;

    std::vector<std::function<void()>> acts_;
    los::ui::Id act(std::function<void()> f) {
        acts_.push_back(std::move(f));
        return los::ui::Id("act", static_cast<uint32_t>(acts_.size() - 1));
    }

    std::string app_name_;
    Link link_;
    Rig rig_;
    EyePixels eyes_[2];
    los::ui::Image eye_img_[2] {};
    oxs_shm_status status_{};
    bool gone_ = false;           // the application's end of the link has closed

    std::set<int> keys_;          // held: a letter as itself (lower case), else 256 + Key
    bool shift_ = false;
    uint32_t mouse_held_[2] {};   // a panel button held down with the mouse, per hand
    uint32_t remote_held_[2] {};  // held through the control socket, until it releases them
    ControlSocket control_;
    bool dragging_ = false;
    double last_x_ = 0, last_y_ = 0;
    int view_px_w_ = 640, view_px_h_ = 640;   // an eye's room in the view, in physical pixels (last frame's)
    long last_tick_ns_ = 0;
    double scroll_ = 0;
    bool use_sim_ = false;        // the idle window's switch: `device = sim` in the configuration

    int timer_fd_ = -1;
    losmenu::Palette pal_;
    los::display::AppWindow win_;
};

// XrSessionState as a person reads it: "Focused".
const char* session_state_name(int state);

// THE CONFIGURATION (DESIGN.md, "Choosing the device"): `device =` in ~/.config/los/openxr.
std::string configured_device();
bool set_configured_device(const std::string& device, std::string* error);

}  // namespace oxs
