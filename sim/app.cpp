// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#include "app.hpp"

#include "titlebar.hpp"

#include <poll.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <fstream>
#include <print>
#include <sstream>

namespace oxs {

using namespace los::ui;
namespace kit = los::ui::kit;
using los::display::Key;

namespace {

constexpr uint32_t kLeftButton = 0x110;   // BTN_LEFT
constexpr double kPanelW = 320;
constexpr int kHeadlessEye = 512;         // the headless test's eyes, converted at most this big

long now_ns() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000L + t.tv_nsec;
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

kit::Tone state_tone(int s) {
    return s == 5 ? kit::Tone::Success : s == 4 || s == 3 ? kit::Tone::Accent : s >= 6 ? kit::Tone::Warning : kit::Tone::Neutral;
}

// "/interaction_profiles/oculus/touch_controller" -> "oculus/touch_controller".
std::string short_profile(const char* p) {
    std::string s = p;
    constexpr std::string_view prefix = "/interaction_profiles/";
    if (s.starts_with(prefix)) s.erase(0, prefix.size());
    return s;
}

// A KEY AS THE HELD SET KEEPS IT: a letter by itself in lower case -- its release may come with Shift
// already let go, or still down -- anything else by what it is.
int key_code(const los::display::KeyEvent& e) {
    if (e.key != Key::None) return 256 + static_cast<int>(e.key);
    if (e.ascii != 0) return std::tolower(static_cast<unsigned char>(e.ascii));
    return -1;
}

constexpr int kKeySpace = 256 + static_cast<int>(Key::Space);

// The keys that hold a hand's controls (DESIGN.md, "The simulator window").
struct KeyControl { int key; uint32_t control; };
constexpr KeyControl kKeyControls[] = {
    { kKeySpace, kTrigger }, { 'g', kGrip }, { 'z', kPrimary }, { 'x', kSecondary }, { 'm', kMenu },
    { 'c', kStickClick },
    { 256 + static_cast<int>(Key::Up), kStickUp }, { 256 + static_cast<int>(Key::Down), kStickDown },
    { 256 + static_cast<int>(Key::Left), kStickLeft }, { 256 + static_cast<int>(Key::Right), kStickRight },
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

kit::State hov(const Id& id, bool active = false) {
    kit::State s;
    s.hovered = hovered(id);
    s.active = active;
    return s;
}

}  // namespace

// ---- the configuration ------------------------------------------------------------------------

namespace {
std::string config_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] == '/') return std::string(xdg) + "/los/openxr";
    const char* home = std::getenv("HOME");
    return home != nullptr ? std::string(home) + "/.config/los/openxr" : std::string();
}
}  // namespace

// The runtime's reading of the same file (src/runtime.c, configured_device): `device = <word>`.
std::string configured_device() {
    std::ifstream f(config_path());
    std::string line, device;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            s.erase(0, s.find_first_not_of(" \t"));
            s.erase(s.find_last_not_of(" \t\r") + 1);
            return s;
        };
        if (trim(line.substr(0, eq)) == "device") device = trim(line.substr(eq + 1));
    }
    return device;
}

// Rewrites `device =`, keeping every other line as it was.
bool set_configured_device(const std::string& device, std::string* error) {
    const std::string path = config_path();
    if (path.empty()) { *error = "no home folder"; return false; }
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            const size_t eq = line.find('=');
            std::string key = eq == std::string::npos ? std::string() : line.substr(0, eq);
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            if (line.empty() || line[0] == '#' || key != "device") lines.push_back(line);
        }
    }
    if (lines.empty()) lines.push_back("# Which OpenXR device the LOS runtime offers: null (none connected) or sim (openxr-sim).");
    lines.push_back("device = " + device);
    const std::string dir = path.substr(0, path.rfind('/'));
    const std::string parent = dir.substr(0, dir.rfind('/'));
    ::mkdir(parent.c_str(), 0700);
    ::mkdir(dir.c_str(), 0700);
    std::ofstream out(path, std::ios::trunc);
    for (const auto& l : lines) out << l << "\n";
    out.close();
    if (!out) { *error = std::format("could not write {}: {}", path, std::strerror(errno)); return false; }
    return true;
}

// ---- the window -------------------------------------------------------------------------------

App::App(std::string app_name) : app_name_(std::move(app_name)) {
    pal_ = losmenu::load_palette();
    use_sim_ = configured_device() == "sim";
}

App::~App() {
    release_eyes();
    if (timer_fd_ >= 0) ::close(timer_fd_);
}

void App::release_eyes() {
    for (auto& img : eye_img_)
        if (img.valid()) { release_image(img); img = {}; }
}

void App::take_frame(int max_w, int max_h) {
    // The old images point into the buffers take_frame rewrites; nothing draws between here and their
    // replacement.
    if (!link_.take_frame(max_w, max_h, eyes_)) return;
    release_eyes();
    for (int e = 0; e < 2; e++)
        if (eyes_[e].width > 0)
            eye_img_[e] = image_from_pixels(eyes_[e].argb.data(), eyes_[e].width, eyes_[e].height, eyes_[e].width * 4);
}

void App::build(int w, int h) {
    acts_.clear();
    const bool own = win_.opened() && !win_.server_decorated() && !win_.maximized();
    const double frame = own ? 2 : 0;
    const double body_h = std::max(1.0, h - frame - losmenu::kTitlebarHeight - losmenu::kStatusBarHeight);
    const kit::Palette kp = kit::from(pal_);

    BoxStyle root;
    root.shape(Length::fixed(Px::surface(w)), Length::fixed(Px::surface(h)));
    root.direction = Direction::Column;
    root.background = kit::level(kit::Level::Window, kp);   // STYLE-EXCEPTION: the window's ground, by its level
    losmenu::window_frame(root, pal_, own);
    Box root_box(Id("root"), root);
    {
        losmenu::TitlebarSpec spec;
        spec.maximized = win_.opened() && win_.maximized();
        spec.title = link_.attached() ? std::format("OpenXR Simulator — {}", app_name_.empty() ? "an application" : app_name_)
                                      : std::string("OpenXR Simulator");
        losmenu::titlebar(pal_, spec, {});
    }
    {
        BoxStyle body;
        body.shape(Length::grow(), Length::fixed(Px::surface(body_h)));
        body.direction = Direction::Row;
        Box b(Id("body"), body);
        if (link_.attached()) {
            build_view(std::max(100.0, w - frame - kPanelW), body_h);
            build_panel(kPanelW, body_h);
        } else {
            kit::Viewport vp(Id("page"), w - frame, body_h, scroll_);
            BoxStyle content;
            content.along = Length::fit();
            content.across = Length::fixed(Px::surface(w - frame));
            content.direction = Direction::Column;
            Box col(Id("page-col"), content);
            build_idle(kp);
        }
    }
    {
        losmenu::StatusBar bar(Id("status"), pal_);
        std::string s;
        if (!link_.attached()) s = use_sim_ ? "OpenXR applications will use the simulated headset" : "OpenXR applications see no headset";
        else if (gone_) s = "The application has ended its session";
        else s = std::format("Frame {}  ·  drag to turn  ·  W A S D Q E to move  ·  Tab: next device", link_.frames());
        text(s, losmenu::status_text(pal_));
    }
}

// THE TWO EYES, side by side and as large as the room allows, their own proportions kept.
void App::build_view(double w, double h) {
    const kit::Palette kp = kit::from(pal_);
    const double gap = kit::space(kit::Space::S);
    BoxStyle st;
    st.shape(Length::fixed(Px::surface(w)), Length::fixed(Px::surface(h)));
    st.children(Direction::Row, Align::Center, Align::Center);
    st.gap = static_cast<uint16_t>(gap);
    st.background = kit::level(kit::Level::Well, kp);   // STYLE-EXCEPTION: the headset's view, a recess by its level
    st.clip_along = true;
    st.cursor = dragging_ ? Cursor::Grabbing : Cursor::Grab;
    Box view(Id("view"), st);

    const double room_w = std::max(1.0, (w - 3 * gap) / 2), room_h = std::max(1.0, h - 2 * gap);
    const int scale = win_.opened() ? std::max(1, win_.scale()) : 1;
    view_px_w_ = static_cast<int>(room_w * scale);
    view_px_h_ = static_cast<int>(room_h * scale);
    if (!eye_img_[0].valid() && !eye_img_[1].valid()) {
        kit::Stack s(Id("waiting"), kit::Space::XS, Align::Center);
        kit::text(gone_ ? "The application has gone" : std::format("Waiting for {}'s first frame", app_name_.empty() ? "the application" : app_name_),
                  los::Type::Heading, kit::Ink::Text, kp);
        kit::text(gone_ ? "Close this window." : "It draws once its session is running.", los::Type::Secondary, kit::Ink::Dim, kp);
        return;
    }
    for (int e = 0; e < 2; e++) {
        if (!eye_img_[e].valid()) continue;
        const double sw = eyes_[e].source_w, sh = eyes_[e].source_h;
        const double fit = std::min(room_w / sw, room_h / sh);
        image(Id("eye", static_cast<uint32_t>(e)), eye_img_[e], Size{ sw * fit, sh * fit });
    }
}

void App::build_panel(double w, double h) {
    const kit::Palette kp = kit::from(pal_);
    kit::Viewport vp(Id("panel"), w, h, scroll_);
    BoxStyle content;
    content.along = Length::fit();
    content.across = Length::fixed(Px::surface(w));
    content.direction = Direction::Column;
    Box col(Id("panel-col"), content);
    {
        kit::Block wrap(Id("b-app"), kit::Where::Page);
        kit::Card card(Id("app"), kp);
        kit::caps("Application", kp);
        const std::string name = status_.app[0] != '\0' ? status_.app : app_name_;
        kit::text(name.empty() ? "Unnamed" : name, los::Type::Heading, kit::Ink::Text, kp, { .elide_at = w - 60 });
        {
            kit::Inline line(Id("app-state"));
            kit::badge(Id("state"), gone_ ? "Ended" : state_name(status_.state), gone_ ? kit::Tone::Neutral : state_tone(status_.state), kp, true);
            if (!gone_ && status_.fps > 0)
                kit::text(std::format("{:.0f} frames a second", status_.fps), los::Type::Secondary, kit::Ink::Dim, kp);
        }
        kit::text(status_.profile[0] != '\0' ? "Controllers: " + short_profile(status_.profile) : std::string("No controller bindings yet"),
                  los::Type::Secondary, kit::Ink::Dim, kp, { .elide_at = w - 60 });
    }
    {
        kit::Block wrap(Id("b-move"), kit::Where::Page);
        kit::Card card(Id("move"), kp);
        kit::caps("The mouse and keys move", kp);
        const Part parts[] = { Part::Headset, Part::Right, Part::Left };
        std::vector<Id> ids;
        for (const Part p : parts) ids.push_back(act([this, p] { rig_.select(p); }));
        kit::segmented(ids, { "Headset", "Right hand", "Left hand" }, static_cast<size_t>(rig_.selected()),
                       { kit::Tone::Neutral, kit::Tone::Neutral, kit::Tone::Neutral }, kp);
        double p[3], yaw = 0, pitch = 0;
        rig_.readout(rig_.selected(), p, yaw, pitch);
        kit::text(std::format("x {:+.2f}  y {:+.2f}  z {:+.2f} m", p[0], p[1], p[2]), los::Type::Secondary, kit::Ink::Dim, kp, { .mono = true });
        kit::text(std::format("heading {:+.0f}°  pitch {:+.0f}°", yaw, pitch), los::Type::Secondary, kit::Ink::Dim, kp, { .mono = true });
        const Id reset = act([this] { rig_.reset(); publish(); });
        kit::button(reset, "Put everything back", kit::ButtonKind::Secondary, hov(reset), kp);
    }
    build_hand(1, kp);
    build_hand(0, kp);
    {
        kit::Block wrap(Id("b-keys"), kit::Where::Page);
        kit::Card card(Id("keys"), kp);
        kit::caps("Keys", kp);
        for (const char* line : {
                 "Drag the view to turn. W A S D move, Q E down and up; Shift is slower.",
                 "Tab: the next device. R: everything back where it started.",
                 "Space trigger, G grip, Z A/X, X B/Y, M menu, arrows the stick, C its click -- on the selected hand, "
                 "or the right while the headset is selected." })
            kit::text(line, los::Type::Secondary, kit::Ink::Dim, kp, { .wrap = true });
    }
}

void App::build_hand(int hand, const kit::Palette& kp) {
    const uint32_t k = static_cast<uint32_t>(hand);
    kit::Block wrap(Id("b-hand", k), kit::Where::Page);
    kit::Card card(Id("hand", k), kp);
    {
        kit::Inline head(Id("hand-head", k));
        kit::caps(hand == 1 ? "Right hand" : "Left hand", kp);
        kit::filler(Id("hand-fill", k));
        if (status_.haptic[hand] > 0) kit::badge(Id("buzz", k), "Vibrating", kit::Tone::Warning, kp, true);
        else if (rig_.button_hand() == hand) kit::badge(Id("keys-here", k), "Keys", kit::Tone::Accent, kp);
    }
    const uint32_t held = rig_.held(hand);
    for (size_t row = 0; row < 2; row++) {
        kit::Inline line(Id("hand-row", k * 2 + static_cast<uint32_t>(row)), kit::Space::XS);
        for (size_t i = row * 3; i < row * 3 + 3; i++) {
            const PanelButton& b = kPanelButtons[i];
            const Id id("hold", k * 16 + static_cast<uint32_t>(i));
            const bool on = (held & b.control) != 0;
            kit::push_button(id, hand == 1 ? b.right : b.left, on ? kit::Tone::Success : kit::Tone::Neutral, hov(id, on), kp);
        }
    }
}

// STARTED FROM THE MENU: nothing to show but the choice, and what it means.
void App::build_idle(const kit::Palette& kp) {
    {
        kit::Block wrap(Id("b-intro"), kit::Where::Page);
        kit::PageHeader(Id("ph"), "OpenXR Simulator",
                        "A headset and two controllers in a window, for trying OpenXR applications without the hardware. "
                        "When an application starts a VR session, its two eyes appear here and the mouse and keys move the headset "
                        "and the controllers.", kp);
    }
    kit::Block wrap(Id("b-use"), kit::Where::Page);
    kit::Card card(Id("use"), kp, {}, kit::Inset::Rows);
    kit::Setting row(Id("use-row"), "Use the simulated headset",
                     use_sim_ ? "OpenXR applications find a headset, and this window opens when one starts a session."
                              : "Off: OpenXR applications find no headset connected.", {}, kp);
    const Id t = act([this] {
        std::string error;
        if (set_configured_device(use_sim_ ? "null" : "sim", &error)) use_sim_ = !use_sim_;
        else std::println(stderr, "openxr-sim: {}", error);
    });
    kit::toggle(t, use_sim_, hov(t), kp);
}

// ---- input ------------------------------------------------------------------------------------

// What the held keys and the panel's held buttons come to: the selected part's motion, and each hand's
// controls.
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
    win_.request_redraw();
}

void App::on_key(const los::display::KeyEvent& e, bool down) {
    shift_ = e.mods.shift;
    if (down && (losmenu::is_close_shortcut(e.mods.ctrl, e.ascii) || e.key == Key::Escape)) { win_.quit(); return; }
    const int code = key_code(e);
    if (code < 0 || !link_.attached()) return;
    if (down) {
        if (e.repeat) return;
        if (e.key == Key::Tab) { rig_.select_next(); publish(); return; }
        if (code == 'r') { rig_.reset(); publish(); return; }
        keys_.insert(code);
    } else {
        keys_.erase(code);
    }
    last_tick_ns_ = now_ns();
    arm_timer();
    publish();
}

void App::on_button(double x, double y, uint32_t button, bool pressed, int clicks) {
    if (button != kLeftButton) return;
    const Point p{ x, y };
    if (!pressed) {
        const bool any = dragging_ || mouse_held_[0] != 0 || mouse_held_[1] != 0;
        dragging_ = false;
        mouse_held_[0] = mouse_held_[1] = 0;
        if (any) publish();
        return;
    }
    if (win_.begin_edge_resize(x, y)) return;
    switch (losmenu::titlebar_at(win_.ui(), x, y)) {
        case losmenu::TitlebarPart::Close:    win_.quit(); return;
        case losmenu::TitlebarPart::Maximise: win_.toggle_maximized(); return;
        case losmenu::TitlebarPart::Minimise: win_.minimize(); return;
        case losmenu::TitlebarPart::Drag:     if (clicks >= 2) win_.toggle_maximized(); else win_.start_move(); return;
        case losmenu::TitlebarPart::None:     break;
    }
    if (const auto h = win_.ui().hit_named("hold", p)) {
        const uint32_t hand = h->index / 16, i = h->index % 16;
        if (hand < 2 && i < std::size(kPanelButtons)) { mouse_held_[hand] |= kPanelButtons[i].control; publish(); }
        return;
    }
    if (const auto h = win_.ui().hit_named("act", p); h && h->index < acts_.size()) {
        auto f = acts_[h->index];   // a copy: the action may rebuild the table
        f();
        win_.request_redraw();
        return;
    }
    if (link_.attached() && win_.ui().hit_named("view", p)) {
        dragging_ = true;
        last_x_ = x;
        last_y_ = y;
        win_.request_redraw();
    }
}

void App::on_motion(double x, double y) {
    if (dragging_) {
        rig_.turn(x - last_x_, y - last_y_);
        last_x_ = x;
        last_y_ = y;
        publish();
        return;
    }
    win_.request_redraw();
}

// ---- the link -----------------------------------------------------------------------------------

void App::on_frame_ready() {
    if (!link_.drain()) {
        gone_ = true;
        // THE APPLICATION HAS GONE, so the window has nothing more to show. In headset mode it goes too:
        // it was the application's, opened by its session.
        win_.quit();
        return;
    }
    take_frame(view_px_w_, view_px_h_);
    link_.status(status_);
    win_.request_redraw();
}

// SIXTY TIMES A SECOND while something is held, so a held W walks smoothly; four otherwise, for the
// application's state while it draws nothing (paused, or between sessions).
void App::arm_timer() {
    if (timer_fd_ < 0) return;
    const bool moving = !keys_.empty();
    const long ns = moving ? 16666667L : 250000000L;
    itimerspec its{ { 0, ns }, { 0, ns } };
    timerfd_settime(timer_fd_, 0, &its, nullptr);
}

void App::tick() {
    const long now = now_ns();
    const double dt = last_tick_ns_ > 0 ? std::min(0.1, (now - last_tick_ns_) / 1e9) : 0;
    last_tick_ns_ = now;
    Motion m;
    m.forward = keys_.contains('w') - keys_.contains('s');
    m.right = keys_.contains('d') - keys_.contains('a');
    m.up = keys_.contains('e') - keys_.contains('q');
    rig_.step(m, dt, shift_);
    oxs_shm_status st{};
    const bool changed_status = link_.status(st) && std::memcmp(&st, &status_, sizeof st) != 0;
    if (changed_status) status_ = st;
    if (!keys_.empty()) publish();
    else if (changed_status) win_.request_redraw();
}

int App::run(std::string* error) {
    const std::string title = link_.attached() ? "OpenXR Simulator — " + app_name_ : std::string("OpenXR Simulator");
    if (!win_.open(title, "openxr-sim", 1280, 760, error)) return 1;
    win_.on_build([this](int w, int h) { build(w, h); });
    win_.apply_content_min_size({ 720, 520 });
    win_.on_button([this](double x, double y, uint32_t b, bool p, int clicks, const los::display::Modifiers&) {
        on_button(x, y, b, p, clicks);
    });
    win_.on_key([this](const los::display::KeyEvent& e) { on_key(e, true); });
    win_.on_key_release([this](const los::display::KeyEvent& e) { on_key(e, false); });
    // NOTHING STAYS HELD when the keyboard goes elsewhere: the release goes to the other window.
    win_.on_focus_lost([this] {
        keys_.clear();
        mouse_held_[0] = mouse_held_[1] = 0;
        dragging_ = false;
        arm_timer();
        if (link_.attached()) publish();
    });
    win_.on_motion([this](double x, double y) { on_motion(x, y); });
    win_.on_scroll([this](double, double dy, const los::display::Modifiers&) {
        const char* view = link_.attached() ? "panel" : "page";
        const char* col = link_.attached() ? "panel-col" : "page-col";
        const double room = win_.ui().bounds(Id(col)).h - win_.ui().bounds(Id(view)).h;
        scroll_ = std::clamp(scroll_ + kit::wheel_pixels(dy), 0.0, std::max(0.0, room));
        win_.request_redraw();
    });
    win_.on_close([this] { win_.quit(); });
    if (link_.attached()) {
        // The devices where the runtime already says they are, before the application asks.
        publish();
        win_.watch_fd(link_.socket(), [this] { on_frame_ready(); });
        timer_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        arm_timer();
        win_.watch_fd(timer_fd_, [this] { uint64_t n; while (::read(timer_fd_, &n, sizeof n) > 0) {} tick(); });
        start_control();
    }
    win_.run();
    return 0;
}

// ---- the control socket ------------------------------------------------------------------------------

const char* session_state_name(int state) { return state_name(state); }

// ON THE WINDOW'S OWN LOOP, as the link is: a command runs between frames, never beside one.
void App::start_control() {
    std::string error;
    if (!control_.open(&error)) {
        std::println(stderr, "openxr-sim: no control socket ({}); the window works without it", error);
        return;
    }
    win_.watch_fd(control_.listen_fd(), [this] {
        for (int fd; (fd = control_.accept_client()) >= 0;)
            win_.watch_fd(fd, [this, fd] {
                if (!control_.serve(fd, [this](const std::string& line) { return command(line); })) win_.unwatch_fd(fd);
            });
    });
}

// ---- headless ------------------------------------------------------------------------------------

// THE SCRIPTED MOVE, once the application is drawing: the head turned 30 degrees to the left, and the
// right hand's trigger and A held. The test client looks for exactly these.
int App::headless(const std::string& dir) {
    if (!link_.attached()) { std::println(stderr, "openxr-sim: headless needs --headset"); return 2; }
    link_.write_tracking(rig_.tracking());
    bool scripted = false, focused = false;
    for (;;) {
        pollfd p{ link_.socket(), POLLIN, 0 };
        const int r = ::poll(&p, 1, 100);
        if (r < 0 && errno != EINTR) break;
        const bool alive = link_.drain();
        link_.take_frame(kHeadlessEye, kHeadlessEye, eyes_);
        oxs_shm_status st{};
        if (link_.status(st)) status_ = st;
        focused |= status_.state == 5;
        if (!scripted && link_.frames() >= 3) {
            rig_.turn(-(30.0 * 3.14159265358979 / 180) / 0.005, 0);
            mouse_held_[1] = kTrigger | kPrimary;
            apply_input();
            link_.write_tracking(rig_.tracking());
            scripted = true;
        }
        if (!alive) break;
    }
    gone_ = true;
    const bool l = write_ppm(dir + "/left.ppm", eyes_[0]), rr = write_ppm(dir + "/right.ppm", eyes_[1]);
    for (int e = 0; e < 2; e++)
        if (eyes_[e].width > 0) eye_img_[e] = image_from_pixels(eyes_[e].argb.data(), eyes_[e].width, eyes_[e].height, eyes_[e].width * 4);
    std::string error;
    const bool png = render_png(dir + "/window.png", 1280, 760, &error);
    const size_t faults = png ? win_.check("openxr-sim", { 1280, 760 }) : 0;
    // Written last, and whole, so a test waiting for it knows everything above is there.
    const std::string summary = std::format("frames {} app \"{}\" profile \"{}\" focused {} eyes {}x{} {}x{} ppm {} png {} layout faults {}{}\n",
        link_.frames(), status_.app, status_.profile, focused ? "yes" : "no", eyes_[0].source_w, eyes_[0].source_h,
        eyes_[1].source_w, eyes_[1].source_h, l && rr ? "yes" : "no", png ? "yes" : "no", faults, png ? "" : " (" + error + ")");
    {
        std::ofstream f(dir + "/summary.tmp");
        f << summary;
    }
    std::rename((dir + "/summary.tmp").c_str(), (dir + "/summary").c_str());
    return link_.frames() > 0 && l && rr && png && faults == 0 ? 0 : 1;
}

bool App::render_png(const std::string& path, int w, int h, std::string* error) {
    if (!win_.opened()) {
        win_.open_headless("openxr-sim", w, h);
        win_.on_build([this](int ww, int hh) { build(ww, hh); });
    }
    return win_.render_png(path, { w, h }, error);
}

int App::layout_check(int w, int h) {
    if (!win_.opened()) {
        win_.open_headless("openxr-sim", w, h);
        win_.on_build([this](int ww, int hh) { build(ww, hh); });
    }
    const size_t faults = win_.check("openxr-sim", { w, h });
    std::println("openxr-sim: {:d} layout fault(s)", faults);
    return faults == 0 ? 0 : 1;
}

}  // namespace oxs
