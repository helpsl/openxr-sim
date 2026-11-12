// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#pragma once

// THE WINDOW'S CONTENTS AS CLAY LAYS THEM OUT (third_party/clay): a frame is built from nested boxes every
// time it is drawn, laid out by Clay, and its render commands drawn on a Canvas (sim/canvas.hpp). Around
// Clay, what a window needs of it:
//
//   - TOKENS: one theme of colours, six type roles, a spacing scale. Nothing in the app chooses a pixel
//     size or a colour of its own.
//   - ELEMENTS as RAII boxes (Box) and text, and the few controls the window has (button, push button,
//     segmented, toggle, badge, card), each a function.
//   - HIT TESTING and HOVER from the last layout: an element given a tag and index is found under the
//     pointer by that pair, cut to whatever clips it.
//   - LAYOUT FAULTS (check): Clay's own errors, and any element drawn outside its parent on an axis the
//     parent does not clip -- text too long for its card, a row wider than the panel. What the headless
//     test asks to be zero.
//
// Everything is in logical pixels; render() multiplies by the window's scale, text included, so the layout
// is the same at every scale and only sharper.

#include "canvas.hpp"
#include "font.hpp"

#include "clay.h"
#include "os.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oxs::ui {

// ---- tokens ------------------------------------------------------------------------------------------

// THE COLOURS, by role. The desktop's scheme and accent decide them (set_theme, from sim/os.hpp's
// desktop_appearance); the neutrals are LOS's palette's, so on LOS the window and the frame the compositor
// draws round it are one surface -- and elsewhere a quiet grey that suits any desktop.
struct Theme {
    bool dark = true;
    Rgba window, card, well, line, text, dim, faint, accent, success, warning, control, control_hover;
};
const Theme& theme();
void set_theme(const Appearance& desktop);
// Text and marks on a filled `background` (an accent, a status colour): near-black on a light one, white
// on a dark one.
Rgba on(Rgba background);

enum class Role { Title, Heading, Body, Control, Secondary, Caption };
struct Type { Face face; uint16_t size; };
Type type(Role);

enum class Space : uint16_t { XS = 4, S = 8, M = 12, L = 16, XL = 24 };
constexpr uint16_t px(Space s) { return static_cast<uint16_t>(s); }
constexpr float kRadius = 8, kRadiusSmall = 6;

enum class Tone { Neutral, Accent, Success, Warning };
Rgba tone_colour(Tone);

// ---- elements ----------------------------------------------------------------------------------------

struct Size {
    enum Kind { Fit, Grow, Fixed } kind = Fit;
    float value = 0;
    static Size fit() { return { Fit, 0 }; }
    static Size grow() { return { Grow, 0 }; }
    static Size fixed(float v) { return { Fixed, v }; }
};

struct Style {
    const char* tag = nullptr;   // a tag (a string literal) and index name the element for hit() and bounds()
    uint32_t index = 0;
    Size w, h;
    bool row = false;            // children left to right; else top to bottom
    uint16_t pad_x = 0, pad_y = 0, gap = 0;
    Clay_LayoutAlignmentX align_x = CLAY_ALIGN_X_LEFT;
    Clay_LayoutAlignmentY align_y = CLAY_ALIGN_Y_TOP;
    std::optional<Rgba> background;
    float radius = 0;
    std::optional<Rgba> border;
    bool clip_y = false, clip_x = false;   // children outside it are cut off (and may be scrolled)
    float scroll_y = 0;                    // with clip_y: how far its children are moved up
    const ImageView* image = nullptr;      // drawn filling the box
};

// An element, open from construction until destruction; anything built in between is its child.
class Box {
public:
    explicit Box(const Style& s);
    ~Box();
    Box(const Box&) = delete;
    Box& operator=(const Box&) = delete;
};

struct TextOpts { bool wrap = false; bool mono = false; float elide_at = 0; bool caps = false; };
void text(std::string_view s, Role role, Rgba ink, TextOpts opts = {});
void filler();

// ---- controls ----------------------------------------------------------------------------------------

// Each takes the tag and index it is hit by; `hovered` comes from the last layout.
enum class ButtonKind { Primary, Secondary };
void button(const char* tag, uint32_t index, std::string_view label, ButtonKind kind);
void push_button(const char* tag, uint32_t index, std::string_view label, bool on);
void segmented(const char* tag, const std::vector<std::string>& labels, size_t selected);   // index i: the i'th label
void toggle(const char* tag, uint32_t index, bool on);
void badge(std::string_view label, Tone tone, bool strong = false);
// A value to type into: `label` dim before it, `value` the text; while `editing`, outlined in the accent
// (in the warning colour when `bad`) with a caret after the text.
void field(const char* tag, uint32_t index, std::string_view label, std::string_view value, bool editing, bool bad, Size w);
void caps(std::string_view s);   // a card's small heading

// A card: a column of its own, on the card colour, rounded, inset.
class Card {
public:
    explicit Card(const char* tag = nullptr, uint32_t index = 0);
private:
    Box box_;
};

// ---- the frame ---------------------------------------------------------------------------------------

struct Hit { const char* tag; uint32_t index; };

class Ui {
public:
    explicit Ui(Fonts& fonts);
    ~Ui();
    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    // Building a frame: begin, the elements, end -- which lays it out.
    void begin(float w, float h, float mouse_x, float mouse_y);
    void end();
    // Draws the last layout on `canvas`, which is the window's size times `scale`.
    void render(Canvas& canvas, float scale);

    // From the last layout: the topmost tagged element under x, y, and whether one is under the pointer.
    std::optional<Hit> hit(float x, float y) const;
    bool hovered(const char* tag, uint32_t index) const;
    std::optional<RectF> bounds(const char* tag, uint32_t index = 0) const;
    // The layout's faults, described; empty when there are none.
    const std::vector<std::string>& faults() const { return faults_; }

    Fonts& fonts() { return fonts_; }
    // Text that has to outlive the frame's building, for Clay: kept until the next begin.
    Clay_String keep(std::string s);

    // The element being built (Box's), for its constructor and destructor.
    void open(const Style& s);
    void close();

    static Ui& current();

private:
    struct Record {
        Clay_ElementId id;
        int parent;          // index into records_, -1 for the root
        const char* tag;
        uint32_t index;
        bool clip_x, clip_y;
        RectF box;           // laid out
        RectF visible;       // the box cut to everything that clips it
    };
    static void on_error(Clay_ErrorData e);
    static Clay_Dimensions measure(Clay_StringSlice text, Clay_TextElementConfig* config, void* user);

    Fonts& fonts_;
    void* memory_ = nullptr;
    Clay_Context* clay_ = nullptr;
    Clay_RenderCommandArray commands_{};
    std::deque<std::string> kept_;
    std::vector<Record> records_;   // the frame being built
    std::vector<Record> last_;      // the last one laid out: what hit(), hovered() and bounds() read
    std::vector<int> stack_;
    uint32_t auto_ids_ = 0;
    float w_ = 0, h_ = 0, mouse_x_ = -1, mouse_y_ = -1;
    std::vector<std::string> faults_, clay_errors_;
};

}  // namespace oxs::ui
