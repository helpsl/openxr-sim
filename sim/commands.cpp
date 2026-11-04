// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// WHAT THE CONTROL SOCKET ANSWERS (src/sim/control.hpp; DESIGN.md, "The control socket"). One command a
// line, words separated by spaces; one JSON object back, always with "ok". The devices move exactly as the
// mouse and keys move them, and a button held from here is held as the panel's buttons are -- alongside
// whatever the person holds, not instead of it.

#include "app.hpp"

#include "los/parse.hpp"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <sstream>
#include <vector>

namespace oxs {

namespace {

std::string json_str(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
                else out += c;
        }
    }
    return out + "\"";
}

std::string fail(std::string_view why) { return std::format(R"({{"ok":false,"error":{}}})", json_str(why)); }

std::optional<Part> part_of(std::string_view w) {
    if (w == "headset" || w == "head") return Part::Headset;
    if (w == "right") return Part::Right;
    if (w == "left") return Part::Left;
    return std::nullopt;
}

const char* part_name(Part p) { return p == Part::Headset ? "headset" : p == Part::Right ? "right" : "left"; }

// The names a control goes by, the buttons' own (A, X) as well as what they do.
struct Named { const char* name; uint32_t control; };
constexpr Named kControls[] = {
    { "trigger", kTrigger }, { "grip", kGrip }, { "squeeze", kGrip },
    { "primary", kPrimary }, { "a", kPrimary }, { "x", kPrimary },
    { "secondary", kSecondary }, { "b", kSecondary }, { "y", kSecondary },
    { "menu", kMenu }, { "stick", kStickClick },
    { "stick-up", kStickUp }, { "stick-down", kStickDown }, { "stick-left", kStickLeft }, { "stick-right", kStickRight },
};

std::string controls_json(uint32_t held) {
    static constexpr Named kOut[] = {
        { "trigger", kTrigger }, { "grip", kGrip }, { "primary", kPrimary }, { "secondary", kSecondary },
        { "menu", kMenu }, { "stick", kStickClick }, { "stick-up", kStickUp }, { "stick-down", kStickDown },
        { "stick-left", kStickLeft }, { "stick-right", kStickRight },
    };
    std::string out = "[";
    for (const Named& n : kOut)
        if ((held & n.control) != 0) out += std::string(out.size() > 1 ? "," : "") + json_str(n.name);
    return out + "]";
}

double number(const std::string& w, bool* ok) {
    const auto v = los::parse<double>(w);
    if (!v || !std::isfinite(*v)) { *ok = false; return 0; }
    return *v;
}

// The eyes side by side, each at most `max` pixels on its longer side, as a PNG.
std::string eyes_png(const EyePixels eyes[2], int max, int* out_w, int* out_h) {
    double scale[2] = { 1, 1 };
    int w[2] = {}, h[2] = {};
    for (int e = 0; e < 2; e++) {
        if (eyes[e].width <= 0 || eyes[e].height <= 0) continue;
        scale[e] = std::min(1.0, static_cast<double>(max) / std::max(eyes[e].width, eyes[e].height));
        w[e] = std::max(1, static_cast<int>(std::lround(eyes[e].width * scale[e])));
        h[e] = std::max(1, static_cast<int>(std::lround(eyes[e].height * scale[e])));
    }
    *out_w = w[0] + w[1];
    *out_h = std::max(h[0], h[1]);
    if (*out_w <= 0 || *out_h <= 0) return {};
    cairo_surface_t* out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, *out_w, *out_h);
    cairo_t* cr = cairo_create(out);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    double x = 0;
    for (int e = 0; e < 2; e++) {
        if (w[e] == 0) continue;
        cairo_surface_t* src = cairo_image_surface_create_for_data(const_cast<uint8_t*>(eyes[e].argb.data()), CAIRO_FORMAT_ARGB32,
                                                                   eyes[e].width, eyes[e].height, eyes[e].width * 4);
        cairo_save(cr);
        cairo_translate(cr, x, 0);
        cairo_scale(cr, scale[e], scale[e]);
        cairo_set_source_surface(cr, src, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
        cairo_surface_destroy(src);
        x += w[e];
    }
    cairo_destroy(cr);
    std::string png;
    cairo_surface_write_to_png_stream(out, [](void* p, const unsigned char* d, unsigned int n) {
        static_cast<std::string*>(p)->append(reinterpret_cast<const char*>(d), n);
        return CAIRO_STATUS_SUCCESS;
    }, &png);
    cairo_surface_destroy(out);
    return png;
}

std::string base64(const std::string& in) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = static_cast<uint8_t>(in[i]) << 16 | static_cast<uint8_t>(in[i + 1]) << 8 | static_cast<uint8_t>(in[i + 2]);
        out += k[v >> 18]; out += k[(v >> 12) & 63]; out += k[(v >> 6) & 63]; out += k[v & 63];
    }
    if (i < in.size()) {
        const uint32_t v = static_cast<uint8_t>(in[i]) << 16 | (i + 1 < in.size() ? static_cast<uint8_t>(in[i + 1]) << 8 : 0);
        out += k[v >> 18]; out += k[(v >> 12) & 63];
        out += i + 1 < in.size() ? k[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

}  // namespace

std::string App::status_json() const {
    return std::format(R"({{"ok":true,"app":{},"profile":{},"state":{},"state_name":{},"fps":{:.1f},"frames":{},)"
                       R"("eyes":[[{},{}],[{},{}]],"haptic":[{:.2f},{:.2f}],"selected":"{}","control":{}}})",
                       json_str(status_.app[0] ? status_.app : app_name_.c_str()), json_str(status_.profile), status_.state,
                       json_str(session_state_name(status_.state)), status_.fps, link_.frames(),
                       eyes_[0].source_w, eyes_[0].source_h, eyes_[1].source_w, eyes_[1].source_h,
                       status_.haptic[0], status_.haptic[1], part_name(rig_.selected()), json_str(control_.name()));
}

std::string App::poses_json() const {
    std::string out = R"({"ok":true)";
    for (const Part p : { Part::Headset, Part::Left, Part::Right }) {
        double pos[3], yaw, pitch;
        rig_.readout(p, pos, yaw, pitch);
        out += std::format(R"(,"{}":{{"position":[{:.4f},{:.4f},{:.4f}],"yaw":{:.2f},"pitch":{:.2f})", part_name(p),
                           pos[0], pos[1], pos[2], yaw, pitch);
        if (p != Part::Headset) out += std::format(R"(,"held":{})", controls_json(rig_.held(p == Part::Left ? 0 : 1)));
        out += "}";
    }
    return out + "}";
}

std::string App::command(const std::string& line) {
    std::istringstream in(line);
    std::vector<std::string> w;
    for (std::string s; in >> s;) w.push_back(s);
    if (w.empty()) return fail("an empty command");
    const std::string& verb = w[0];

    if (verb == "status") return status_json();
    if (verb == "poses") return poses_json();
    if (verb == "reset") {
        rig_.reset();
        publish();
        return poses_json();
    }
    if (verb == "set" || verb == "move" || verb == "turn") {
        const size_t want = verb == "set" ? 7 : verb == "move" ? 5 : 4;
        const auto part = w.size() > 1 ? part_of(w[1]) : std::nullopt;
        if (!part || w.size() != want)
            return fail(verb == "set" ? "set PART X Y Z YAW PITCH (PART: headset, left, right; metres, degrees)"
                      : verb == "move" ? "move PART FORWARD RIGHT UP (metres)" : "turn PART YAW PITCH (degrees, yaw left)");
        bool ok = true;
        double v[5] = {};
        for (size_t i = 2; i < w.size(); i++) v[i - 2] = number(w[i], &ok);
        if (!ok) return fail("not a number: " + line);
        if (verb == "set") rig_.set_pose(*part, v, v[3], v[4]);
        else if (verb == "move") rig_.move_by(*part, v[0], v[1], v[2]);
        else rig_.turn_by(*part, v[0], v[1]);
        publish();
        return poses_json();
    }
    if (verb == "hold" || verb == "release") {
        const auto part = w.size() > 1 ? part_of(w[1]) : std::nullopt;
        if (!part || *part == Part::Headset || (verb == "hold" && w.size() < 3))
            return fail(verb + " left|right CONTROL... (trigger grip a b x y menu stick stick-up stick-down stick-left stick-right)" +
                        (verb == "release" ? std::string(", or none for all") : std::string()));
        const int hand = *part == Part::Left ? 0 : 1;
        uint32_t controls = 0;
        for (size_t i = 2; i < w.size(); i++) {
            std::string name = w[i];
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
            const auto it = std::find_if(std::begin(kControls), std::end(kControls), [&](const Named& n) { return name == n.name; });
            if (it == std::end(kControls)) return fail("no control called " + w[i]);
            controls |= it->control;
        }
        if (verb == "hold") remote_held_[hand] |= controls;
        else remote_held_[hand] &= w.size() > 2 ? ~controls : 0u;
        publish();
        return poses_json();
    }
    if (verb == "shot") {
        int max = 512;
        if (w.size() > 1) {
            const auto v = los::parse<int>(w[1]);
            if (!v || *v < 16 || *v > 2048) return fail("shot [MAX]: each eye at most MAX pixels, 16 to 2048");
            max = *v;
        }
        if (eyes_[0].width <= 0 && eyes_[1].width <= 0) return fail("no frame yet: the application has drawn nothing");
        int pw = 0, ph = 0;
        const std::string png = eyes_png(eyes_, max, &pw, &ph);
        if (png.empty()) return fail("the picture could not be encoded");
        return std::format(R"({{"ok":true,"frame":{},"width":{},"height":{},"png":"{}"}})", link_.frames(), pw, ph, base64(png));
    }
    return fail("unknown command '" + verb + "': status poses set move turn hold release reset shot");
}

}  // namespace oxs
