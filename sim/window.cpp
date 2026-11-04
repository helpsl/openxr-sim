// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#include "window.hpp"

#include "os.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include <GL/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#ifndef GL_BGRA
#define GL_BGRA 0x80E1   // OpenGL 1.2, and EXT_bgra before it; Windows' gl.h is 1.1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace oxs {

namespace {

constexpr double kDoubleClick = 0.5;   // seconds, Windows' default (GTK's is 0.4)
constexpr float kDoubleClickSlop = 4;  // logical pixels

void on_glfw_error(int code, const char* what) { std::fprintf(stderr, "openxr-sim: GLFW (%d): %s\n", code, what); }

bool glfw_up() {
    static const bool ok = [] {
        glfwSetErrorCallback(on_glfw_error);
        // THE DESKTOP'S OWN TITLE BAR WHEN IT OFFERS ONE. GLFW on Wayland draws its frame with libdecor
        // whenever it can load it, and libdecor never asks the compositor to: it takes the compositor's
        // default, which is "draw your own" on LOS (whose compositor draws a frame only for a window that
        // asks) -- so the window wore libdecor's bar instead of the desktop's. Without libdecor GLFW asks
        // for a server-side frame through xdg-decoration. Only where it is offered: GNOME's compositor
        // offers none, and libdecor's bar is the right one there.
        if (compositor_draws_frames()) glfwInitHint(GLFW_WAYLAND_LIBDECOR, GLFW_WAYLAND_DISABLE_LIBDECOR);
        return glfwInit() == GLFW_TRUE;
    }();
    return ok;
}

Mods mods_of(int m) { return { (m & GLFW_MOD_SHIFT) != 0, (m & GLFW_MOD_CONTROL) != 0, (m & GLFW_MOD_ALT) != 0 }; }

}  // namespace

Window* Window::self(GLFWwindow* w) { return static_cast<Window*>(glfwGetWindowUserPointer(w)); }

Window::~Window() {
    if (win_ == nullptr) return;
    glfwMakeContextCurrent(win_);
    if (texture_ != 0) glDeleteTextures(1, &texture_);
    for (GLFWcursor* c : cursors_)
        if (c != nullptr) glfwDestroyCursor(c);
    glfwDestroyWindow(win_);
}

bool Window::open(const std::string& title, int w, int h, std::string* error) {
    if (!glfw_up()) { *error = "no display: GLFW could not start (is there a Wayland, X11 or Windows desktop?)"; return false; }
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 1);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, "openxr-sim");
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "openxr-sim");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "openxr-sim");
    win_ = glfwCreateWindow(w, h, title.c_str(), nullptr, nullptr);
    if (win_ == nullptr) { *error = "GLFW could not make a window with an OpenGL context"; return false; }
    glfwSetWindowUserPointer(win_, this);
    glfwMakeContextCurrent(win_);
    // NEVER WAIT FOR THE DISPLAY: a frame is drawn only when something changed, and a minimised window on
    // Wayland would otherwise hold the swap -- and with it the link to the application -- until it is shown.
    glfwSwapInterval(0);
    cursors_[0] = glfwCreateStandardCursor(GLFW_ARROW_CURSOR);
    cursors_[1] = glfwCreateStandardCursor(GLFW_POINTING_HAND_CURSOR);
    cursors_[2] = glfwCreateStandardCursor(GLFW_RESIZE_ALL_CURSOR);

    glfwSetKeyCallback(win_, [](GLFWwindow* g, int key, int, int action, int m) {
        Window* s = self(g);
        if (s->on_key && key != GLFW_KEY_UNKNOWN) s->on_key(key, action != GLFW_RELEASE, action == GLFW_REPEAT, mods_of(m));
    });
    glfwSetMouseButtonCallback(win_, [](GLFWwindow* g, int button, int action, int) {
        Window* s = self(g);
        int clicks = 1;
        if (action == GLFW_PRESS) {
            const double now = glfwGetTime();
            if (button == s->last_click_button_ && now - s->last_click_ < kDoubleClick &&
                std::abs(s->mx_ - s->last_click_x_) <= kDoubleClickSlop && std::abs(s->my_ - s->last_click_y_) <= kDoubleClickSlop) {
                clicks = 2;
                s->last_click_ = -1;   // a third press starts again
            } else {
                s->last_click_ = now;
            }
            s->last_click_button_ = button;
            s->last_click_x_ = s->mx_;
            s->last_click_y_ = s->my_;
        }
        const int b = button == GLFW_MOUSE_BUTTON_LEFT ? 0 : button == GLFW_MOUSE_BUTTON_RIGHT ? 1 : button == GLFW_MOUSE_BUTTON_MIDDLE ? 2 : -1;
        if (s->on_button && b >= 0) s->on_button(s->mx_, s->my_, b, action == GLFW_PRESS, clicks);
    });
    glfwSetCursorPosCallback(win_, [](GLFWwindow* g, double x, double y) {
        Window* s = self(g);
        s->to_logical(x, y, s->mx_, s->my_);
        if (s->on_motion) s->on_motion(s->mx_, s->my_);
    });
    glfwSetCursorEnterCallback(win_, [](GLFWwindow* g, int entered) {
        Window* s = self(g);
        if (!entered) {
            s->mx_ = s->my_ = -1;
            if (s->on_motion) s->on_motion(-1, -1);
        }
    });
    glfwSetScrollCallback(win_, [](GLFWwindow* g, double, double dy) {
        Window* s = self(g);
        if (s->on_scroll) s->on_scroll(static_cast<float>(dy));
    });
    glfwSetWindowFocusCallback(win_, [](GLFWwindow* g, int focused) {
        Window* s = self(g);
        if (!focused && s->on_focus_lost) s->on_focus_lost();
    });
    const auto resized = [](GLFWwindow* g, int, int) {
        Window* s = self(g);
        s->update_size();
        if (s->on_resize) s->on_resize();
    };
    glfwSetFramebufferSizeCallback(win_, resized);
    glfwSetWindowSizeCallback(win_, resized);
    glfwSetWindowContentScaleCallback(win_, [](GLFWwindow* g, float, float) {
        Window* s = self(g);
        s->update_size();
        if (s->on_resize) s->on_resize();
    });
    glfwSetWindowRefreshCallback(win_, [](GLFWwindow* g) {
        Window* s = self(g);
        if (s->on_resize) s->on_resize();
    });
    update_size();
    return true;
}

void Window::update_size() {
    glfwGetFramebufferSize(win_, &fb_w_, &fb_h_);
    glfwGetWindowSize(win_, &win_w_, &win_h_);
    float sx = 1, sy = 1;
    glfwGetWindowContentScale(win_, &sx, &sy);
    scale_ = std::max(0.5f, sx);
    lw_ = std::max(1.0f, fb_w_ / scale_);
    lh_ = std::max(1.0f, fb_h_ / scale_);
}

// Pointer positions are in screen coordinates: on Wayland the window's logical ones, on Windows its pixels.
void Window::to_logical(double x, double y, float& lx, float& ly) const {
    const double kx = win_w_ > 0 ? static_cast<double>(fb_w_) / win_w_ / scale_ : 1;
    const double ky = win_h_ > 0 ? static_cast<double>(fb_h_) / win_h_ / scale_ : 1;
    lx = static_cast<float>(x * kx);
    ly = static_cast<float>(y * ky);
}

void Window::set_title(const std::string& title) {
    if (win_ != nullptr) glfwSetWindowTitle(win_, title.c_str());
}

void Window::set_min_size(int w, int h) {
    if (win_ == nullptr) return;
    const int pw = static_cast<int>(std::lround(w * scale_ * win_w_ / std::max(1, fb_w_)));
    const int ph = static_cast<int>(std::lround(h * scale_ * win_h_ / std::max(1, fb_h_)));
    glfwSetWindowSizeLimits(win_, pw, ph, GLFW_DONT_CARE, GLFW_DONT_CARE);
}

bool Window::wait(double timeout) {
    if (win_ == nullptr) return false;
    if (timeout > 0) glfwWaitEventsTimeout(timeout);
    else glfwPollEvents();
    return !quit_ && !glfwWindowShouldClose(win_);
}

void Window::quit() { quit_ = true; wake(); }

void Window::wake() { glfwPostEmptyEvent(); }

void Window::set_cursor(Cursor c) {
    if (win_ == nullptr || c == cursor_) return;
    cursor_ = c;
    glfwSetCursor(win_, cursors_[static_cast<int>(c)]);
}

void Window::present(const Canvas& canvas) {
    if (win_ == nullptr || canvas.width() <= 0 || canvas.height() <= 0) return;
    glfwMakeContextCurrent(win_);
    if (texture_ == 0) {
        glGenTextures(1, &texture_);
        glBindTexture(GL_TEXTURE_2D, texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (canvas.width() != tex_w_ || canvas.height() != tex_h_) {
        tex_w_ = canvas.width();
        tex_h_ = canvas.height();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tex_w_, tex_h_, 0, GL_BGRA, GL_UNSIGNED_BYTE, canvas.data());
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex_w_, tex_h_, GL_BGRA, GL_UNSIGNED_BYTE, canvas.data());
    }
    glViewport(0, 0, fb_w_, fb_h_);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    // The canvas's first row is the window's top; GL's texture origin is its first row, its viewport's the bottom.
    //
    // VERTEX ARRAYS, NOT glBegin: Mesa 26.2's llvmpipe on Wayland freed a buffer of the window's surface
    // and read it again at the next swap after any glBegin/glEnd -- the window died on its first frame. A
    // twenty-line GLFW program showed it with nothing of ours in it (2026-11-04); arrays are as old (1.1).
    const float u = static_cast<float>(fb_w_) / tex_w_, v = static_cast<float>(fb_h_) / tex_h_;
    const float xy[] = { -1, -1, 1, -1, 1, 1, -1, 1 };
    const float uv[] = { 0, v, u, v, u, 0, 0, 0 };
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, xy);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glfwSwapBuffers(win_);
}

}  // namespace oxs
