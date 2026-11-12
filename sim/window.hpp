// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#pragma once

// THE WINDOW: GLFW's, on Wayland, X11 or Windows, with the system's own decorations. What is in it is a
// Canvas (sim/canvas.hpp) drawn in software and shown as one texture: no shaders, no extension loader,
// OpenGL 1.1 -- which every driver, Mesa's software ones and Windows' own opengl32 included, has.
//
// SIZES ARE LOGICAL: the layout is done at the window's size divided by its content scale and drawn at
// the framebuffer's, so a 1280-wide window at 150% is laid out at 1280 and drawn with 1920 pixels. Pointer
// positions come back in the same logical units.

#include "canvas.hpp"

#include <functional>
#include <string>

struct GLFWwindow;
struct GLFWcursor;

namespace oxs {

struct Mods { bool shift = false, ctrl = false, alt = false; };

enum class Cursor { Arrow, Hand, Move };

class Window {
public:
    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool open(const std::string& title, int w, int h, std::string* error);
    bool opened() const { return win_ != nullptr; }
    void set_title(const std::string& title);
    void set_min_size(int w, int h);

    // Logical size, and the scale from it to the framebuffer.
    float width() const { return lw_; }
    float height() const { return lh_; }
    float scale() const { return scale_; }

    // THE LOOP: waits for input, a wake() from another thread, or `timeout` seconds -- whichever is first --
    // and dispatches what came. False once the window is asked to close (or quit() was called).
    bool wait(double timeout);
    void quit();
    // From any thread: makes wait() return.
    static void wake();

    void present(const Canvas& canvas);
    void set_cursor(Cursor c);

    // Keys by GLFW key code (layout-independent: W is where W is on a US keyboard).
    std::function<void(int key, bool down, bool repeat, const Mods&)> on_key;
    // Text typed, a Unicode code point at a time, as the keyboard layout makes it.
    std::function<void(unsigned codepoint)> on_char;
    // Left = 0, right = 1, middle = 2; `clicks` counts a double-click as 2.
    std::function<void(float x, float y, int button, bool pressed, int clicks)> on_button;
    std::function<void(float x, float y)> on_motion;
    std::function<void(float dy)> on_scroll;
    std::function<void()> on_focus_lost;
    std::function<void()> on_resize;

    float mouse_x() const { return mx_; }
    float mouse_y() const { return my_; }

private:
    void update_size();
    void to_logical(double x, double y, float& lx, float& ly) const;
    static Window* self(GLFWwindow* w);

    GLFWwindow* win_ = nullptr;
    GLFWcursor* cursors_[3] {};
    Cursor cursor_ = Cursor::Arrow;
    unsigned int texture_ = 0;
    int tex_w_ = 0, tex_h_ = 0;
    int fb_w_ = 0, fb_h_ = 0, win_w_ = 0, win_h_ = 0;
    float lw_ = 0, lh_ = 0, scale_ = 1;
    float mx_ = -1, my_ = -1;
    double last_click_ = -1;
    float last_click_x_ = 0, last_click_y_ = 0;
    int last_click_button_ = -1;
    bool quit_ = false;
};

}  // namespace oxs
