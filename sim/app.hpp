// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#pragma once

// THE SIMULATOR'S WINDOW (DESIGN.md, "The simulator window"): the application's two eyes side by side,
// and a panel -- which device the mouse and keys move, each hand's buttons, the application's state, the
// keys. Started by the runtime with --headset when an application creates a session; started by itself,
// it says whether OpenXR applications will use it and lets the person choose.
//
// THREE THREADS in headset mode: the window's, which does everything; the link's, which only waits on the
// wake channel and wakes the window's (sim/link.hpp, wait); and the control socket's, which hands each
// command to the window's and waits for the answer (sim/control.hpp).

#include "canvas.hpp"
#include "control.hpp"
#include "font.hpp"
#include "install.hpp"
#include "link.hpp"
#include "rig.hpp"
#include "ui.hpp"
#include "window.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace oxs {

class App {
public:
    explicit App(std::string app_name);
    ~App();

    // Headset mode: what the runtime handed over (sim/link.hpp, attach).
    bool attach(const std::string& link, std::string* error) { return link_.attach(link, error); }

    int  run(std::string* error);
    // THE TEST'S HEADSET (OPENXR_SIM_HEADLESS=DIR): no window. Frames are taken as they come, the devices
    // make one scripted move once frames arrive, and when the application has gone DIR gets left.ppm and
    // right.ppm (the last frame's eyes, as converted) and window.png (this window, drawn headless), and a
    // one-line summary of what was seen.
    int  headless(const std::string& dir);
    bool render_png(const std::string& path, int w, int h, std::string* error);
    int  layout_check(int w, int h);

private:
    bool fonts_ready(std::string* error);
    void layout(float w, float h, float mouse_x, float mouse_y);
    void draw();
    void build(float w, float h);
    void build_view(float w, float h);
    void build_panel(float w, float h);
    void build_hand(int hand);
    void build_idle(float w);

    void on_key(int key, bool down, bool repeat, const Mods& mods);
    void on_button(float x, float y, int button, bool pressed, int clicks);
    void on_motion(float x, float y);
    void on_scroll(float dy);
    void on_frame_ready();
    void tick();
    void apply_input();
    void publish();
    void take_frame(int max_w, int max_h);
    void run_commands();

    // THE CONTROL SOCKET (sim/control.hpp, sim/commands.cpp): headset mode only.
    std::string command(const std::string& line);
    std::string status_json() const;
    std::string poses_json() const;

    std::string app_name_;
    Link link_;
    Rig rig_;
    EyePixels eyes_[2];
    ImageView eye_view_[2];
    oxs_shm_status status_{};
    bool gone_ = false;           // the application's end of the link has closed

    std::set<int> keys_;          // held: a letter as itself (lower case), else 256 + its GLFW key
    bool shift_ = false;
    uint32_t mouse_held_[2] {};   // a panel button held down with the mouse, per hand
    uint32_t remote_held_[2] {};  // held through the control socket, until it releases them
    bool dragging_ = false;
    float last_x_ = 0, last_y_ = 0;
    int view_px_w_ = 640, view_px_h_ = 640;   // an eye's room in the view, in physical pixels (last frame's)
    double last_tick_ = 0;
    float scroll_ = 0;
    bool use_sim_ = false;        // the idle window's switch: `device = sim` in the configuration
    bool dirty_ = true;

    // THE ACTIVE RUNTIME (sim/install.hpp), the idle window's second card. Changing it may wait for
    // Windows' permission prompt, so it runs on a thread of its own, which says when it is done.
    void build_runtime_card();
    void change_runtime(bool make_active);
    RuntimeState runtime_;
    std::thread runtime_thread_;
    std::atomic<bool> runtime_busy_{ false }, runtime_done_{ false };
    std::string runtime_message_;   // the last change's failure, or that it was cancelled; written by that thread
                                    // before runtime_done_, read by the window's after it

    Fonts fonts_;
    bool fonts_loaded_ = false;
    std::unique_ptr<ui::Ui> ui_;
    Canvas canvas_;
    Window win_;

    // The link's thread, and what it tells the window's.
    std::thread link_thread_;
    std::atomic<bool> frame_ready_{ false }, link_gone_{ false };

    // Commands from the control socket's thread, answered on the window's.
    struct Pending { std::string line, answer; bool done = false; };
    std::mutex mutex_;
    std::condition_variable answered_;
    std::deque<Pending*> pending_;
    bool closing_ = false;
    ControlSocket control_;
};

// XrSessionState as a person reads it: "Focused".
const char* session_state_name(int state);

// THE CONFIGURATION (DESIGN.md, "Choosing the device"): `device =` in the configuration file (sim/os.hpp).
std::string configured_device();
bool set_configured_device(const std::string& device, std::string* error);

}  // namespace oxs
