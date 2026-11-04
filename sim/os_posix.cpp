// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#include "os.hpp"

#include <dbus/dbus.h>
#include <fontconfig/fontconfig.h>
#include <sys/stat.h>
#include <wayland-client.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace oxs {

FontFile find_font(Face face) {
    static FcConfig* config = FcInitLoadConfigAndFonts();
    if (config == nullptr) return {};
    const char* pattern = face == Face::Bold ? "sans-serif:weight=bold" : face == Face::Mono ? "monospace" : "sans-serif";
    FcPattern* pat = FcNameParse(reinterpret_cast<const FcChar8*>(pattern));
    if (pat == nullptr) return {};
    // TrueType outlines only: stb_truetype reads glyf and CFF, not bitmap fonts.
    FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
    FcConfigSubstitute(config, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult result = FcResultNoMatch;
    FcPattern* match = FcFontMatch(config, pat, &result);
    FontFile out;
    FcChar8* file = nullptr;
    int index = 0;
    if (match != nullptr && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) out.path = reinterpret_cast<const char*>(file);
    if (match != nullptr) FcPatternGetInteger(match, FC_INDEX, 0, &index);
    // A named instance has its number in the index's high 16 bits (fontconfig's FC_INDEX).
    out.embolden = face == Face::Bold && (index >> 16) != 0;
    if (match != nullptr) FcPatternDestroy(match);
    FcPatternDestroy(pat);
    return out;
}

std::string font_search_description() { return "fontconfig matched no scalable sans-serif font"; }

std::string config_path(int which) {
    const char* rel = which == 0 ? "openxr-sim/config" : "los/openxr";
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] == '/') return std::string(xdg) + "/" + rel;
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') return std::string(home) + "/.config/" + rel;
    return {};
}

bool make_parent_dirs(const std::string& path) {
    for (size_t at = path.find('/', 1); at != std::string::npos; at = path.find('/', at + 1)) {
        const std::string dir = path.substr(0, at);
        if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return false;
    }
    return true;
}

namespace {

// THE VALUE INSIDE ANY NUMBER OF VARIANTS: ReadOne answers v(value), the older Read v(v(value)).
void into_value(DBusMessageIter* it) {
    while (dbus_message_iter_get_arg_type(it) == DBUS_TYPE_VARIANT) {
        DBusMessageIter inner;
        dbus_message_iter_recurse(it, &inner);
        *it = inner;
    }
}

// One org.freedesktop.portal.Settings value, with `read` given the iterator at it. The bus STARTS the
// portal if it is installed and not running yet, as it does for GTK and Qt: on a LOS session nothing else
// may have needed it, and with auto-start off the first window of the day was told "no preference" by a
// portal that would have said dark (found in the VM, 2026-11-04). Not installed is an immediate error;
// starting takes a moment, so 1 s for the first answer, and the second is from a running portal.
template <typename Read>
bool ask_portal(DBusConnection* bus, const char* key, Read read) {
    for (const char* method : { "ReadOne", "Read" }) {
        DBusMessage* m = dbus_message_new_method_call("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                                      "org.freedesktop.portal.Settings", method);
        if (m == nullptr) return false;
        const char* ns = "org.freedesktop.appearance";
        dbus_message_append_args(m, DBUS_TYPE_STRING, &ns, DBUS_TYPE_STRING, &key, DBUS_TYPE_INVALID);
        DBusError err;
        dbus_error_init(&err);
        DBusMessage* reply = dbus_connection_send_with_reply_and_block(bus, m, 1000, &err);
        dbus_message_unref(m);
        // An older portal has no ReadOne; any other failure is the answer.
        const bool unknown_method = reply == nullptr && dbus_error_has_name(&err, DBUS_ERROR_UNKNOWN_METHOD);
        dbus_error_free(&err);
        if (reply == nullptr) {
            if (unknown_method) continue;
            return false;
        }
        DBusMessageIter it;
        bool ok = false;
        if (dbus_message_iter_init(reply, &it)) {
            into_value(&it);
            ok = read(&it);
        }
        dbus_message_unref(reply);
        return ok;
    }
    return false;
}

void on_global(void* data, wl_registry*, uint32_t, const char* interface, uint32_t) {
    if (std::strcmp(interface, "zxdg_decoration_manager_v1") == 0) *static_cast<bool*>(data) = true;
}
void on_global_remove(void*, wl_registry*, uint32_t) {}

}  // namespace

Appearance desktop_appearance(bool ask_desktop) {
    Appearance a;
    if (const char* forced = std::getenv("OPENXR_SIM_THEME"); forced != nullptr && forced[0] != '\0') {
        if (std::strcmp(forced, "dark") == 0) { a.scheme = Appearance::Scheme::Dark; return a; }
        if (std::strcmp(forced, "light") == 0) { a.scheme = Appearance::Scheme::Light; return a; }
    }
    if (!ask_desktop) return a;
    DBusError err;
    dbus_error_init(&err);
    // A PRIVATE connection, closed again: the shared one would stay open for the life of the window, and
    // libdbus would exit the process when the bus went away.
    DBusConnection* bus = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    dbus_error_free(&err);
    if (bus == nullptr) return a;
    dbus_connection_set_exit_on_disconnect(bus, FALSE);
    // color-scheme: uint32, 0 no preference, 1 dark, 2 light.
    ask_portal(bus, "color-scheme", [&](DBusMessageIter* it) {
        if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_UINT32) return false;
        dbus_uint32_t v = 0;
        dbus_message_iter_get_basic(it, &v);
        a.scheme = v == 1 ? Appearance::Scheme::Dark : v == 2 ? Appearance::Scheme::Light : Appearance::Scheme::Unknown;
        return true;
    });
    // accent-color: (ddd), sRGB 0..1; a component outside that range means "none chosen".
    ask_portal(bus, "accent-color", [&](DBusMessageIter* it) {
        if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_STRUCT) return false;
        DBusMessageIter f;
        dbus_message_iter_recurse(it, &f);
        double rgb[3];
        for (double& c : rgb) {
            if (dbus_message_iter_get_arg_type(&f) != DBUS_TYPE_DOUBLE) return false;
            dbus_message_iter_get_basic(&f, &c);
            if (!(c >= 0 && c <= 1)) return false;
            dbus_message_iter_next(&f);
        }
        a.has_accent = true;
        a.accent_r = static_cast<float>(rgb[0]);
        a.accent_g = static_cast<float>(rgb[1]);
        a.accent_b = static_cast<float>(rgb[2]);
        return true;
    });
    dbus_connection_close(bus);
    dbus_connection_unref(bus);
    return a;
}

// GLFW USES WAYLAND WHEN WAYLAND_DISPLAY IS SET, and then this asks the same compositor: one registry
// round trip on a connection of its own, closed before GLFW opens its own.
bool compositor_draws_frames() {
    const char* wd = std::getenv("WAYLAND_DISPLAY");
    if (wd == nullptr || wd[0] == '\0') return false;
    wl_display* d = wl_display_connect(nullptr);
    if (d == nullptr) return false;
    bool offered = false;
    static const wl_registry_listener listener = { on_global, on_global_remove };
    wl_registry* r = wl_display_get_registry(d);
    wl_registry_add_listener(r, &listener, &offered);
    wl_display_roundtrip(d);
    wl_registry_destroy(r);
    wl_display_disconnect(d);
    return offered;
}

}  // namespace oxs
