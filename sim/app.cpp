// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#include "app.hpp"

#include "os.hpp"
#include "xrmath.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <print>
#include <vector>

namespace oxs {

using namespace ui;

namespace {

constexpr float kPanelW = 320;
constexpr float kStatusH = 28;
constexpr int kHeadlessEye = 512;   // the headless test's eyes, converted at most this big

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// XrSessionState, as the person reads it.
const char* state_name(int s) {
    switch (s) {
        case 1: return "Idle";
        case 2: return "Ready";
        case 3: return "Synchronized";
        case 4: return "Visible";
        case 5: return "Focused";
        case 6: return "Stopping";
        case 7: return "Loss pending";
        case 8: return "Exiting";
        default: return "Starting";
    }
}

Tone state_tone(int s) { return s == 5 ? Tone::Success : s == 4 || s == 3 ? Tone::Accent : s >= 6 ? Tone::Warning : Tone::Neutral; }

// "/interaction_profiles/oculus/touch_controller" -> "oculus/touch_controller".
std::string short_profile(const char* p) {
    std::string s = p;
    constexpr std::string_view prefix = "/interaction_profiles/";
    if (s.starts_with(prefix)) s.erase(0, prefix.size());
    return s;
}

// A KEY AS THE HELD SET KEEPS IT: a letter as itself in lower case, anything else by its GLFW code.
int key_code(int key) {
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) return 'a' + (key - GLFW_KEY_A);
    return 256 + key;
}

// The keys that hold a hand's controls (DESIGN.md, "The simulator window").
struct KeyControl { int key; uint32_t control; };
const KeyControl kKeyControls[] = {
    { 256 + GLFW_KEY_SPACE, kTrigger }, { 'g', kGrip }, { 'z', kPrimary }, { 'x', kSecondary }, { 'm', kMenu },
    { 'c', kStickClick },
    { 256 + GLFW_KEY_UP, kStickUp }, { 256 + GLFW_KEY_DOWN, kStickDown },
    { 256 + GLFW_KEY_LEFT, kStickLeft }, { 256 + GLFW_KEY_RIGHT, kStickRight },
};

// The panel's buttons, which hold the same controls with the mouse. A and B are X and Y on the left hand.
struct PanelButton { uint32_t control; const char* right; const char* left; };
constexpr PanelButton kPanelButtons[] = {
    { kTrigger, "Trigger", "Trigger" }, { kGrip, "Grip", "Grip" }, { kPrimary, "A", "X" },
    { kSecondary, "B", "Y" }, { kMenu, "Menu", "Menu" }, { kStickClick, "Stick", "Stick" },
};

bool write_ppm(const std::string& path, const EyePixels& e) {
    if (e.width <= 0) return false;
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << e.width << " " << e.height << "\n255\n";
    std::string row(static_cast<size_t>(e.width) * 3, '\0');
    for (int y = 0; y < e.height; y++) {
        const uint8_t* s = e.argb.data() + static_cast<size_t>(y) * e.width * 4;
        for (int x = 0; x < e.width; x++) {
            row[static_cast<size_t>(x) * 3 + 0] = static_cast<char>(s[x * 4 + 2]);
            row[static_cast<size_t>(x) * 3 + 1] = static_cast<char>(s[x * 4 + 1]);
            row[static_cast<size_t>(x) * 3 + 2] = static_cast<char>(s[x * 4 + 0]);
        }
        f.write(row.data(), static_cast<std::streamsize>(row.size()));
    }
    return static_cast<bool>(f);
}

std::string trim(std::string s) {
    s.erase(0, s.find_first_not_of(" \t"));
    s.erase(s.find_last_not_of(" \t\r") + 1);
    return s;
}

}  // namespace

// ---- the configuration ------------------------------------------------------------------------

// The runtime's reading of the same file (runtime/runtime.c, configured_device): `device = <word>`, in this
// project's file, or else -- until 2027-05-01 -- in the one LOS's runtime read.
std::string configured_device() {
    for (int which = 0; which < 2; which++) {
        std::ifstream f(config_path(which));
        if (!f) continue;
        std::string line, device;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            if (trim(line.substr(0, eq)) == "device") device = trim(line.substr(eq + 1));
        }
        return device;
    }
    return {};
}

// Rewrites `device =` in this project's file, keeping every other line as it was.
bool set_configured_device(const std::string& device, std::string* error) {
    const std::string path = config_path(0);
    if (path.empty()) { *error = "no home folder"; return false; }
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            const size_t eq = line.find('=');
            const std::string key = eq == std::string::npos ? std::string() : trim(line.substr(0, eq));
            if (line.empty() || line[0] == '#' || key != "device") lines.push_back(line);
        }
    }
    if (lines.empty()) lines.push_back("# Which OpenXR device openxr-sim's runtime offers: null (none connected) or sim (the simulator's window).");
    lines.push_back("device = " + device);
    if (!make_parent_dirs(path)) { *error = std::format("could not make the folder for {}", path); return false; }
    std::ofstream out(path, std::ios::trunc);
    for (const auto& l : lines) out << l << "\n";
    out.close();
    if (!out) { *error = std::format("could not write {}: {}", path, std::strerror(errno)); return false; }
    return true;
}

// ---- the window -------------------------------------------------------------------------------

App::App(std::string app_name) : app_name_(std::move(app_name)) {
    use_sim_ = configured_device() == "sim";
    runtime_ = runtime_state();
    oxs_room_home(&room_);
}

App::~App() {
    {
        std::lock_guard lock(mutex_);
        closing_ = true;
    }
    answered_.notify_all();
    control_.stop();
    link_.interrupt();
    if (link_thread_.joinable()) link_thread_.join();
    // A permission prompt still open: its answer is waited for, as the change it makes is the person's.
    if (runtime_thread_.joinable()) runtime_thread_.join();
}

bool App::fonts_ready(std::string* error) {
    if (!fonts_loaded_) {
        if (!fonts_.load(error)) return false;
        fonts_loaded_ = true;
        ui_ = std::make_unique<Ui>(fonts_);
    }
    return true;
}

void App::take_frame(int max_w, int max_h) {
    if (!link_.take_frame(max_w, max_h, eyes_)) return;
    for (int e = 0; e < 2; e++)
        eye_view_[e] = eyes_[e].width > 0 ? ImageView{ eyes_[e].argb.data(), eyes_[e].width, eyes_[e].height, eyes_[e].width * 4 } : ImageView{};
}

void App::layout(float w, float h, float mouse_x, float mouse_y) {
    ui_->begin(w, h, mouse_x, mouse_y);
    build(w, h);
    ui_->end();
}

void App::draw() {
    dirty_ = false;
    const float w = win_.width(), h = win_.height(), s = win_.scale();
    layout(w, h, win_.mouse_x(), win_.mouse_y());
    canvas_.resize(static_cast<int>(std::lround(w * s)), static_cast<int>(std::lround(h * s)));
    canvas_.clear(theme().window);
    ui_->render(canvas_, s);
    win_.present(canvas_);
}

void App::build(float w, float h) {
    const Theme& t = theme();
    const float body_h = std::max(1.0f, h - kStatusH);
    Style root;
    root.w = Size::fixed(w);
    root.h = Size::fixed(h);
    root.background = t.window;
    Box root_box(root);
    {
        Style body;
        body.w = Size::grow();
        body.h = Size::fixed(body_h);
        body.row = true;
        Box b(body);
        if (link_.attached()) {
            build_view(std::max(100.0f, w - kPanelW), body_h);
            build_panel(kPanelW, body_h);
        } else {
            Style page;
            page.tag = "page";
            page.w = Size::fixed(w);
            page.h = Size::fixed(body_h);
            page.clip_y = true;
            page.scroll_y = scroll_;
            Box p(page);
            Style col;
            col.tag = "page-col";
            col.w = Size::fixed(w);
            col.pad_x = px(Space::XL);
            col.pad_y = px(Space::XL);
            col.gap = px(Space::L);
            Box c(col);
            build_idle(w - 2 * px(Space::XL));
        }
    }
    {
        Style bar;
        bar.w = Size::grow();
        bar.h = Size::fixed(kStatusH);
        bar.row = true;
        bar.pad_x = px(Space::M);
        bar.align_y = CLAY_ALIGN_Y_CENTER;
        bar.background = t.card;
        Box b(bar);
        std::string s;
        if (!link_.attached()) s = use_sim_ ? "OpenXR applications will use the simulated headset" : "OpenXR applications see no headset";
        else if (gone_) s = "The application has ended its session";
        else s = std::format("Frame {}  \xC2\xB7  drag to turn  \xC2\xB7  W A S D Q E to move  \xC2\xB7  Tab: next device", link_.frames());
        text(s, Role::Secondary, t.dim, { .elide_at = w - 2 * px(Space::M) });
    }
}

// THE EYES: the one chosen, or both side by side, as large as the room allows, their own proportions kept.
void App::build_view(float w, float h) {
    const Theme& t = theme();
    const float gap = px(Space::S);
    Style st;
    st.tag = "view";
    st.w = Size::fixed(w);
    st.h = Size::fixed(h);
    st.row = true;
    st.align_x = CLAY_ALIGN_X_CENTER;
    st.align_y = CLAY_ALIGN_Y_CENTER;
    st.gap = static_cast<uint16_t>(gap);
    st.background = t.well;
    st.clip_x = st.clip_y = true;
    Box view(st);

    const int shown = shown_ == 2 ? 2 : 1;
    const float room_w = std::max(1.0f, (w - (shown + 1) * gap) / shown), room_h = std::max(1.0f, h - 2 * gap);
    const float scale = win_.opened() ? win_.scale() : 1.0f;
    view_px_w_ = static_cast<int>(room_w * scale);
    view_px_h_ = static_cast<int>(room_h * scale);
    if (eye_view_[0].bgra == nullptr && eye_view_[1].bgra == nullptr) {
        Style s;
        s.align_x = CLAY_ALIGN_X_CENTER;
        s.gap = px(Space::XS);
        Box b(s);
        text(gone_ ? "The application has gone"
                   : std::format("Waiting for {}'s first frame", app_name_.empty() ? "the application" : app_name_),
             Role::Heading, t.text, { .elide_at = w - 2 * gap });
        text(gone_ ? "Close this window." : "It draws once its session is running.", Role::Secondary, t.dim);
        return;
    }
    for (int e = 0; e < 2; e++) {
        if (eye_view_[e].bgra == nullptr || (shown_ != 2 && e != shown_)) continue;
        const float sw = static_cast<float>(eyes_[e].source_w), sh = static_cast<float>(eyes_[e].source_h);
        const float fit = std::min(room_w / sw, room_h / sh);
        Style img;
        img.w = Size::fixed(std::floor(sw * fit));
        img.h = Size::fixed(std::floor(sh * fit));
        img.image = &eye_view_[e];
        Box b(img);
    }
}

void App::build_panel(float w, float h) {
    const Theme& t = theme();
    Style vp;
    vp.tag = "panel";
    vp.w = Size::fixed(w);
    vp.h = Size::fixed(h);
    vp.clip_y = true;
    vp.scroll_y = scroll_;
    vp.background = t.window;
    Box viewport(vp);
    Style col;
    col.tag = "panel-col";
    col.w = Size::fixed(w);
    col.pad_x = col.pad_y = px(Space::M);
    col.gap = px(Space::M);
    Box c(col);
    const float inner = w - 2 * px(Space::M) - 2 * (px(Space::M) + 2);   // a card's text room
    {
        Card card;
        caps("Application");
        const std::string name = status_.app[0] != '\0' ? status_.app : app_name_;
        text(name.empty() ? "Unnamed" : name, Role::Heading, t.text, { .elide_at = inner });
        {
            Style line;
            line.row = true;
            line.gap = px(Space::S);
            line.align_y = CLAY_ALIGN_Y_CENTER;
            Box l(line);
            badge(gone_ ? "Ended" : state_name(status_.state), gone_ ? Tone::Neutral : state_tone(status_.state), true);
            if (!gone_ && status_.fps > 0) text(std::format("{:.0f} frames a second", status_.fps), Role::Secondary, t.dim);
        }
        text(status_.profile[0] != '\0' ? "Controllers: " + short_profile(status_.profile) : std::string("No controller bindings yet"),
             Role::Secondary, t.dim, { .elide_at = inner });
    }
    {
        Card card;
        caps("The view shows");
        segmented("eyes", { "Left eye", "Right eye", "Both eyes" }, static_cast<size_t>(shown_));
    }
    {
        Card card;
        caps("The mouse and keys move");
        segmented("part", { "Headset", "Right hand", "Left hand" }, static_cast<size_t>(rig_.selected()));
        double p[3], yaw = 0, pitch = 0;
        rig_.readout(rig_.selected(), p, yaw, pitch);
        text(std::format("x {:+.2f}  y {:+.2f}  z {:+.2f} m", p[0], p[1], p[2]), Role::Secondary, t.dim, { .mono = true });
        text(std::format("heading {:+.0f}\xC2\xB0  pitch {:+.0f}\xC2\xB0", yaw, pitch), Role::Secondary, t.dim, { .mono = true });
        button("reset", 0, "Put everything back", ButtonKind::Secondary);
    }
    build_hand(1);
    build_hand(0);
    build_markers(inner);
    {
        Card card;
        caps("Keys");
        for (const char* line : {
                 "Drag the view to turn. W A S D move, Q E down and up; Shift is slower.",
                 "Tab: the next device. R: everything back where it started. V: the left eye, the right, or both.",
                 "Space trigger, G grip, Z A/X, X B/Y, M menu, arrows the stick, C its click -- on the selected hand, "
                 "or the right while the headset is selected." })
            text(line, Role::Secondary, t.dim, { .wrap = true });
    }
}

void App::build_hand(int hand) {
    const uint32_t k = static_cast<uint32_t>(hand);
    Card card;
    {
        Style head;
        head.w = Size::grow();
        head.row = true;
        head.align_y = CLAY_ALIGN_Y_CENTER;
        Box h(head);
        caps(hand == 1 ? "Right hand" : "Left hand");
        filler();
        if (status_.haptic[hand] > 0) badge("Vibrating", Tone::Warning, true);
        else if (rig_.button_hand() == hand) badge("Keys", Tone::Accent);
    }
    const uint32_t held = rig_.held(hand);
    for (size_t row = 0; row < 2; row++) {
        Style line;
        line.w = Size::grow();
        line.row = true;
        line.gap = px(Space::XS);
        Box l(line);
        for (size_t i = row * 3; i < row * 3 + 3; i++) {
            const PanelButton& b = kPanelButtons[i];
            push_button("hold", k * 16 + static_cast<uint32_t>(i), hand == 1 ? b.right : b.left, (held & b.control) != 0);
        }
    }
}

// THE ROOM'S MARKERS, and which of them the headset sees now -- what an application tracking them finds.
void App::build_markers(float inner) {
    const Theme& t = theme();
    Card card;
    caps("Markers in the room");
    if (room_.count == 0) text("None: the control socket's add-marker puts one up.", Role::Secondary, t.dim, { .wrap = true });
    const oxs_pose head = rig_.tracking().head;
    for (uint32_t i = 0; i < room_.count; i++) {
        const oxs_marker& m = room_.marker[i];
        Style line;
        line.w = Size::grow();
        line.row = true;
        line.gap = px(Space::S);
        line.align_y = CLAY_ALIGN_Y_CENTER;
        Box l(line);
        text(marker_name(m), Role::Secondary, t.text, { .elide_at = inner - 96 });
        filler();
        const bool seen = oxs_marker_seen(&head, &m);
        badge(seen ? "In view" : "Out of view", seen ? Tone::Success : Tone::Neutral, seen);
    }
}

// STARTED BY ITSELF: nothing to show but the choice, and what it means.
void App::build_idle(float w) {
    const Theme& t = theme();
    {
        Style head;
        head.w = Size::grow();
        head.gap = px(Space::S);
        Box h(head);
        text("OpenXR Simulator", Role::Title, t.text);
        text("A headset and two controllers in a window, for trying OpenXR applications without the hardware. "
             "When an application starts a VR session, its two eyes appear here and the mouse and keys move the headset "
             "and the controllers.", Role::Body, t.dim, { .wrap = true });
    }
    {
        Card card;
        Style row;
        row.w = Size::grow();
        row.row = true;
        row.gap = px(Space::L);
        row.align_y = CLAY_ALIGN_Y_CENTER;
        Box r(row);
        {
            Style words;
            words.w = Size::grow();
            words.gap = px(Space::XS);
            Box wd(words);
            text("Use the simulated headset", Role::Body, t.text);
            text(use_sim_ ? "OpenXR applications find a headset, and this window opens when one starts a session."
                          : "Off: OpenXR applications find no headset connected.",
                 Role::Secondary, t.dim, { .wrap = true });
        }
        (void)w;
        toggle("use", 0, use_sim_);
    }
    build_runtime_card();
}

// WHICH RUNTIME THE LOADER GIVES APPLICATIONS, and the one thing to do about it: make it this one, update
// the registered copy to this build, or give back the one it replaced.
void App::build_runtime_card() {
    const Theme& t = theme();
    const RuntimeState& st = runtime_;
    std::string what, action;
    bool make_active = true;
    if (!st.can_register) {
        what = "This simulator is not installed, so it cannot be made the active runtime: " + st.why_not + ".";
    } else if (st.active_is_ours && st.system_choice) {
        what = "This simulator, chosen for every user of this computer (" + st.active_path + ").";
    } else if (st.active_is_ours && st.ours_is_this_build) {
        what = "This simulator: OpenXR applications use it.";
        make_active = false;
        action = st.previous_name.empty() ? "Stop using it" : "Go back to " + st.previous_name;
    } else if (st.active_is_ours) {
        what = "openxr-sim " + st.ours_version + " is registered; this is " OPENXR_SIM_VERSION ".";
        action = "Update it";
    } else {
        what = st.active_name.empty() ? std::string("None: OpenXR applications find no runtime.")
                                      : st.active_name + ": OpenXR applications use it, not this simulator.";
        action = "Make this the active runtime";
    }
#ifdef _WIN32
    // Said before it happens: the prompt is Windows', and the copy is for every user.
    if (make_active && st.can_register)
        what += st.active_is_ours ? " Updating installs this version in Program Files, which Windows asks an administrator to allow."
                                  : " Making it active installs it in Program Files for every user, which Windows asks an administrator to allow.";
#endif
    Card card;
    Style row;
    row.w = Size::grow();
    row.row = true;
    row.gap = px(Space::L);
    row.align_y = CLAY_ALIGN_Y_CENTER;
    Box r(row);
    {
        Style words;
        words.w = Size::grow();
        words.gap = px(Space::XS);
        Box wd(words);
        text("The active OpenXR runtime", Role::Body, t.text);
        text(what, Role::Secondary, t.dim, { .wrap = true });
        if (runtime_busy_) text("Waiting for permission\xE2\x80\xA6", Role::Secondary, t.dim);
        else if (!runtime_message_.empty()) text(runtime_message_, Role::Secondary, t.warning, { .wrap = true });
    }
    if (!action.empty() && !runtime_busy_) button(make_active ? "runtime-on" : "runtime-off", 0, action, make_active ? ButtonKind::Primary : ButtonKind::Secondary);
}

void App::change_runtime(bool make_active) {
    if (runtime_busy_.exchange(true)) return;
    if (runtime_thread_.joinable()) runtime_thread_.join();
    runtime_message_.clear();
    dirty_ = true;
    runtime_thread_ = std::thread([this, make_active] {
        std::string error;
        const RegisterResult r = make_active ? make_runtime_active(&error) : restore_previous_runtime(&error);
        runtime_message_ = r == RegisterResult::Cancelled ? "Cancelled: nothing was changed." : r == RegisterResult::Failed ? error : "";
        runtime_done_ = true;
        Window::wake();
    });
}

// ---- input ------------------------------------------------------------------------------------

// What the held keys and the panel's held buttons come to: each hand's controls.
void App::apply_input() {
    uint32_t keys = 0;
    for (const auto& kc : kKeyControls)
        if (keys_.contains(kc.key)) keys |= kc.control;
    const int target = rig_.button_hand();
    for (int h = 0; h < 2; h++) rig_.hold(h, mouse_held_[h] | remote_held_[h] | (h == target ? keys : 0));
}

void App::publish() {
    apply_input();
    link_.write_tracking(rig_.tracking());
    link_.write_room(room_);
    dirty_ = true;
}

void App::on_key(int key, bool down, bool repeat, const Mods& mods) {
    shift_ = mods.shift || ((key == GLFW_KEY_LEFT_SHIFT || key == GLFW_KEY_RIGHT_SHIFT) && down);
    if (down && (key == GLFW_KEY_ESCAPE || (mods.ctrl && (key == GLFW_KEY_W || key == GLFW_KEY_Q)))) { win_.quit(); return; }
    if (!link_.attached()) return;
    const int code = key_code(key);
    if (down) {
        if (repeat) return;
        if (key == GLFW_KEY_TAB) { rig_.select_next(); publish(); return; }
        if (code == 'r') { rig_.reset(); publish(); return; }
        if (code == 'v') { shown_ = (shown_ + 1) % 3; dirty_ = true; return; }
        keys_.insert(code);
    } else {
        keys_.erase(code);
    }
    last_tick_ = now_s();
    publish();
}

void App::on_button(float x, float y, int button, bool pressed, int clicks) {
    (void)clicks;
    if (button != 0) return;
    if (!pressed) {
        const bool any = dragging_ || mouse_held_[0] != 0 || mouse_held_[1] != 0;
        dragging_ = false;
        mouse_held_[0] = mouse_held_[1] = 0;
        if (any) publish();
        return;
    }
    const auto h = ui_->hit(x, y);
    if (!h) return;
    const std::string_view tag = h->tag;
    if (tag == "hold") {
        const uint32_t hand = h->index / 16, i = h->index % 16;
        if (hand < 2 && i < std::size(kPanelButtons)) { mouse_held_[hand] |= kPanelButtons[i].control; publish(); }
    } else if (tag == "part") {
        if (h->index < 3) { rig_.select(static_cast<Part>(h->index)); publish(); }
    } else if (tag == "eyes") {
        if (h->index < 3) { shown_ = static_cast<int>(h->index); dirty_ = true; }
    } else if (tag == "reset") {
        rig_.reset();
        publish();
    } else if (tag == "runtime-on" || tag == "runtime-off") {
        change_runtime(tag == "runtime-on");
    } else if (tag == "use") {
        std::string error;
        if (set_configured_device(use_sim_ ? "null" : "sim", &error)) use_sim_ = !use_sim_;
        else std::println(stderr, "openxr-sim: {}", error);
        dirty_ = true;
    } else if (tag == "view" && link_.attached()) {
        dragging_ = true;
        last_x_ = x;
        last_y_ = y;
        dirty_ = true;
    }
}

void App::on_motion(float x, float y) {
    if (dragging_) {
        rig_.turn(x - last_x_, y - last_y_);
        last_x_ = x;
        last_y_ = y;
        publish();
        return;
    }
    const auto h = ui_->hit(x, y);
    const std::string_view tag = h ? h->tag : "";
    win_.set_cursor(tag == "view" && link_.attached() ? Cursor::Move
                    : tag == "hold" || tag == "part" || tag == "eyes" || tag == "reset" || tag == "use" || tag == "runtime-on" || tag == "runtime-off"
                    ? Cursor::Hand : Cursor::Arrow);
    dirty_ = true;
}

void App::on_scroll(float dy) {
    const char* view = link_.attached() ? "panel" : "page";
    const char* col = link_.attached() ? "panel-col" : "page-col";
    const auto v = ui_->bounds(view), c = ui_->bounds(col);
    const float room = v && c ? c->h - v->h : 0;
    scroll_ = std::clamp(scroll_ - dy * 48, 0.0f, std::max(0.0f, room));
    dirty_ = true;
}

// ---- the link -----------------------------------------------------------------------------------

void App::on_frame_ready() {
    take_frame(view_px_w_, view_px_h_);
    link_.status(status_);
    dirty_ = true;
}

void App::tick() {
    const double now = now_s();
    const double dt = last_tick_ > 0 ? std::min(0.1, now - last_tick_) : 0;
    last_tick_ = now;
    Motion m;
    m.forward = keys_.contains('w') - keys_.contains('s');
    m.right = keys_.contains('d') - keys_.contains('a');
    m.up = keys_.contains('e') - keys_.contains('q');
    rig_.step(m, dt, shift_);
    oxs_shm_status st{};
    if (link_.status(st) && std::memcmp(&st, &status_, sizeof st) != 0) {
        status_ = st;
        dirty_ = true;
    }
    if (!keys_.empty()) publish();
}

// The control socket's commands, answered here, between frames.
void App::run_commands() {
    std::deque<Pending*> todo;
    {
        std::lock_guard lock(mutex_);
        todo.swap(pending_);
    }
    if (todo.empty()) return;
    for (Pending* p : todo) {
        std::string answer = command(p->line);
        std::lock_guard lock(mutex_);
        p->answer = std::move(answer);
        p->done = true;
    }
    answered_.notify_all();
}

int App::run(std::string* error) {
    if (!fonts_ready(error)) return 1;
    const std::string title = link_.attached() ? "OpenXR Simulator \xE2\x80\x94 " + (app_name_.empty() ? std::string("an application") : app_name_)
                                               : std::string("OpenXR Simulator");
    if (!win_.open(title, 1280, 760, error)) return 1;
    win_.set_min_size(720, 520);
    win_.on_key = [this](int k, bool d, bool r, const Mods& m) { on_key(k, d, r, m); };
    win_.on_button = [this](float x, float y, int b, bool p, int c) { on_button(x, y, b, p, c); };
    win_.on_motion = [this](float x, float y) { on_motion(x, y); };
    win_.on_scroll = [this](float dy) { on_scroll(dy); };
    // NOTHING STAYS HELD when the keyboard goes elsewhere: the release goes to the other window.
    win_.on_focus_lost = [this] {
        keys_.clear();
        mouse_held_[0] = mouse_held_[1] = 0;
        dragging_ = false;
        if (link_.attached()) publish();
    };
    // ON WINDOWS drawn at once: its loop is not running while the window is being dragged to a new size.
    // Elsewhere only marked, and drawn by the loop: on Wayland this is called from inside libdecor's
    // configure, and swapping there (eglSwapBuffers re-entering the surface being configured) freed a
    // buffer Mesa then read -- the window died on its first frame (2026-11-04, under valgrind).
#ifdef _WIN32
    win_.on_resize = [this] { draw(); };
#else
    win_.on_resize = [this] { dirty_ = true; };
#endif
    if (link_.attached()) {
        // The devices where the runtime already says they are, before the application asks.
        publish();
        link_thread_ = std::thread([this] {
            for (;;) {
                const int r = link_.wait(-1);
                if (r < 0) { link_gone_ = true; Window::wake(); return; }
                if (r > 0) { frame_ready_ = true; Window::wake(); }
            }
        });
        std::string why;
        const bool ok = control_.start([this](const std::string& line) {
            Pending p{ line, {}, false };
            std::unique_lock lock(mutex_);
            if (closing_) return std::string(R"({"ok":false,"error":"the simulator is closing"})");
            pending_.push_back(&p);
            Window::wake();
            answered_.wait(lock, [&] { return p.done || closing_; });
            if (!p.done) {
                pending_.erase(std::remove(pending_.begin(), pending_.end(), &p), pending_.end());
                return std::string(R"({"ok":false,"error":"the simulator is closing"})");
            }
            return p.answer;
        }, &why);
        if (!ok) std::println(stderr, "openxr-sim: no control socket ({}); the window works without it", why);
    }
    draw();
    // SIXTY TIMES A SECOND while something is held, so a held W walks smoothly; four otherwise, for the
    // application's state while it draws nothing (paused, or between sessions).
    while (win_.wait(keys_.empty() ? 0.25 : 1.0 / 60)) {
        run_commands();
        if (link_gone_) {
            // THE APPLICATION HAS GONE, so the window has nothing more to show. In headset mode it goes too:
            // it was the application's, opened by its session.
            gone_ = true;
            break;
        }
        if (frame_ready_.exchange(false)) on_frame_ready();
        if (runtime_done_.exchange(false)) {
            runtime_busy_ = false;
            runtime_ = runtime_state();
            dirty_ = true;
        }
        if (link_.attached() && (now_s() - last_tick_ >= (keys_.empty() ? 0.24 : 1.0 / 90))) tick();
        if (dirty_) draw();
    }
    {
        std::lock_guard lock(mutex_);
        closing_ = true;
    }
    answered_.notify_all();
    control_.stop();
    return 0;
}

// ---- the control socket ------------------------------------------------------------------------------

const char* session_state_name(int state) { return state_name(state); }

// ---- headless ------------------------------------------------------------------------------------

// THE SCRIPTED MOVE, once the application is drawing: the head turned 30 degrees to the left, the right
// hand's trigger and A held, and the ArUco marker taken out of the room. The test client looks for exactly
// these.
int App::headless(const std::string& dir) {
    if (!link_.attached()) { std::println(stderr, "openxr-sim: headless needs --headset"); return 2; }
    link_.write_tracking(rig_.tracking());
    link_.write_room(room_);
    bool scripted = false, focused = false;
    for (;;) {
        const int r = link_.wait(100);
        link_.take_frame(kHeadlessEye, kHeadlessEye, eyes_);
        oxs_shm_status st{};
        if (link_.status(st)) status_ = st;
        focused |= status_.state == 5;
        if (!scripted && link_.frames() >= 3) {
            rig_.turn(-(30.0 * 3.14159265358979 / 180) / 0.005, 0);
            mouse_held_[1] = kTrigger | kPrimary;
            apply_input();
            for (uint32_t i = 0; i < room_.count; i++)
                if (room_.marker[i].kind == OXS_MARKER_ARUCO) {
                    std::copy(room_.marker + i + 1, room_.marker + room_.count, room_.marker + i);
                    room_.count--;
                    break;
                }
            link_.write_room(room_);
            link_.write_tracking(rig_.tracking());
            scripted = true;
        }
        if (r < 0) break;
    }
    gone_ = true;
    const bool l = write_ppm(dir + "/left.ppm", eyes_[0]), rr = write_ppm(dir + "/right.ppm", eyes_[1]);
    for (int e = 0; e < 2; e++)
        if (eyes_[e].width > 0) eye_view_[e] = { eyes_[e].argb.data(), eyes_[e].width, eyes_[e].height, eyes_[e].width * 4 };
    std::string error;
    const bool png = render_png(dir + "/window.png", 1280, 760, &error);
    const size_t faults = png ? ui_->faults().size() : 0;
    if (png)
        for (const std::string& f : ui_->faults()) std::println(stderr, "openxr-sim: layout fault: {}", f);
    // Written last, and whole, so a test waiting for it knows everything above is there.
    const std::string summary = std::format("frames {} app \"{}\" profile \"{}\" focused {} eyes {}x{} {}x{} ppm {} png {} layout faults {}{}\n",
        link_.frames(), status_.app, status_.profile, focused ? "yes" : "no", eyes_[0].source_w, eyes_[0].source_h,
        eyes_[1].source_w, eyes_[1].source_h, l && rr ? "yes" : "no", png ? "yes" : "no", faults, png ? "" : " (" + error + ")");
    {
        std::ofstream f(dir + "/summary.tmp");
        f << summary;
    }
    std::remove((dir + "/summary").c_str());
    std::rename((dir + "/summary.tmp").c_str(), (dir + "/summary").c_str());
    return link_.frames() > 0 && l && rr && png && faults == 0 ? 0 : 1;
}

bool App::render_png(const std::string& path, int w, int h, std::string* error) {
    if (!fonts_ready(error)) return false;
    // Laid out twice: hover and the panel's scroll limits read the layout before.
    layout(static_cast<float>(w), static_cast<float>(h), -1, -1);
    layout(static_cast<float>(w), static_cast<float>(h), -1, -1);
    canvas_.resize(w, h);
    canvas_.clear(theme().window);
    ui_->render(canvas_, 1);
    return canvas_.write_png(path, error);
}

int App::layout_check(int w, int h) {
    std::string error;
    if (!fonts_ready(&error)) { std::println(stderr, "openxr-sim: {}", error); return 1; }
    layout(static_cast<float>(w), static_cast<float>(h), -1, -1);
    layout(static_cast<float>(w), static_cast<float>(h), -1, -1);
    for (const std::string& f : ui_->faults()) std::println("  {}", f);
    std::println("openxr-sim: {:d} layout fault(s)", ui_->faults().size());
    return ui_->faults().empty() ? 0 : 1;
}

}  // namespace oxs
