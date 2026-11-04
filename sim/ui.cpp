// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#include "ui.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>

namespace oxs::ui {

namespace {

Ui* g_current = nullptr;

constexpr Rgba hex(uint32_t v, float a = 255) {
    return { static_cast<float>(v >> 16 & 255), static_cast<float>(v >> 8 & 255), static_cast<float>(v & 255), a };
}

Clay_Color clay(Rgba c) { return { c.r, c.g, c.b, c.a }; }
Rgba rgba(Clay_Color c) { return { c.r, c.g, c.b, c.a }; }
Rgba with_alpha(Rgba c, float a) { c.a = a; return c; }

Clay_SizingAxis axis(const Size& s) {
    Clay_SizingAxis a{};
    switch (s.kind) {
        case Size::Fit:   a.type = CLAY__SIZING_TYPE_FIT; a.size.minMax = { 0, 1e9f }; break;
        case Size::Grow:  a.type = CLAY__SIZING_TYPE_GROW; a.size.minMax = { 0, 1e9f }; break;
        case Size::Fixed: a.type = CLAY__SIZING_TYPE_FIXED; a.size.minMax = { s.value, s.value }; break;
    }
    return a;
}

Clay_String str(const char* s) { return { true, static_cast<int32_t>(std::strlen(s)), s }; }

RectF rect(Clay_BoundingBox b) { return { b.x, b.y, b.width, b.height }; }

RectF intersect(const RectF& a, const RectF& b) {
    const float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const float x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
    return { x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0) };
}

bool inside(const RectF& r, float x, float y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

}  // namespace

// ---- tokens ------------------------------------------------------------------------------------------

// ONE THEME, dark: the window shows other programs' pictures, and a dark frame round them is what keeps
// their colours honest.
const Theme& theme() {
    static const Theme t{
        .window = hex(0x1d1e22), .card = hex(0x27292e), .well = hex(0x121316), .line = hex(0x34363c),
        .text = hex(0xe8e9ec), .dim = hex(0xa2a5ad), .faint = hex(0x6f727a),
        .accent = hex(0x5b8def), .success = hex(0x3fb27f), .warning = hex(0xd9a03f),
        .control = hex(0x34363c), .control_hover = hex(0x3f4249), .on_strong = hex(0x0f1012),
    };
    return t;
}

Type type(Role r) {
    switch (r) {
        case Role::Title:     return { Face::Bold, 20 };
        case Role::Heading:   return { Face::Bold, 15 };
        case Role::Body:      return { Face::Regular, 13 };
        case Role::Control:   return { Face::Regular, 13 };
        case Role::Secondary: return { Face::Regular, 12 };
        case Role::Caption:   return { Face::Bold, 11 };
    }
    return { Face::Regular, 13 };
}

Rgba tone_colour(Tone t) {
    const Theme& th = theme();
    switch (t) {
        case Tone::Accent:  return th.accent;
        case Tone::Success: return th.success;
        case Tone::Warning: return th.warning;
        case Tone::Neutral: break;
    }
    return th.dim;
}

// ---- the frame ---------------------------------------------------------------------------------------

Ui::Ui(Fonts& fonts) : fonts_(fonts) {
    const uint32_t size = Clay_MinMemorySize();
    memory_ = std::malloc(size);
    clay_ = Clay_Initialize(Clay_CreateArenaWithCapacityAndMemory(size, memory_), { 1280, 760 }, { &Ui::on_error, this });
    Clay_SetMeasureTextFunction(&Ui::measure, this);
    // Culling would leave elements scrolled out of the panel without render commands, which is right, and
    // without bounds, which hit() and the fault check need.
    Clay_SetCullingEnabled(false);
    g_current = this;
}

Ui::~Ui() {
    if (g_current == this) g_current = nullptr;
    std::free(memory_);
}

Ui& Ui::current() { return *g_current; }

void Ui::on_error(Clay_ErrorData e) {
    auto* self = static_cast<Ui*>(e.userData);
    self->clay_errors_.emplace_back(e.errorText.chars, static_cast<size_t>(e.errorText.length));
}

Clay_Dimensions Ui::measure(Clay_StringSlice text, Clay_TextElementConfig* config, void* user) {
    auto* self = static_cast<Ui*>(user);
    const Face face = static_cast<Face>(config->fontId);
    const std::string_view s(text.chars, static_cast<size_t>(text.length));
    return { self->fonts_.measure(s, face, config->fontSize), self->fonts_.line_height(face, config->fontSize) };
}

Clay_String Ui::keep(std::string s) {
    kept_.push_back(std::move(s));
    const std::string& k = kept_.back();
    return { false, static_cast<int32_t>(k.size()), k.c_str() };
}

void Ui::begin(float w, float h, float mouse_x, float mouse_y) {
    g_current = this;
    Clay_SetCurrentContext(clay_);
    kept_.clear();
    records_.clear();
    stack_.clear();
    auto_ids_ = 0;
    clay_errors_.clear();
    w_ = w;
    h_ = h;
    mouse_x_ = mouse_x;
    mouse_y_ = mouse_y;
    Clay_SetLayoutDimensions({ w, h });
    Clay_BeginLayout();
}

void Ui::open(const Style& s) {
    const char* tag = s.tag != nullptr ? s.tag : "_";
    const uint32_t index = s.tag != nullptr ? s.index : auto_ids_++;
    Clay_ElementDeclaration d{};
    d.id = Clay__HashString(str(tag), index, 0);
    d.layout.sizing = { axis(s.w), axis(s.h) };
    d.layout.padding = { s.pad_x, s.pad_x, s.pad_y, s.pad_y };
    d.layout.childGap = s.gap;
    d.layout.childAlignment = { s.align_x, s.align_y };
    d.layout.layoutDirection = s.row ? CLAY_LEFT_TO_RIGHT : CLAY_TOP_TO_BOTTOM;
    if (s.background) d.backgroundColor = clay(*s.background);
    d.cornerRadius = { s.radius, s.radius, s.radius, s.radius };
    if (s.border) d.border = { clay(*s.border), { 1, 1, 1, 1, 0 } };
    if (s.clip_x || s.clip_y) d.clip = { s.clip_x, s.clip_y, { 0, -s.scroll_y } };
    if (s.image != nullptr) d.image.imageData = const_cast<ImageView*>(s.image);
    Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
    records_.push_back({ d.id, stack_.empty() ? -1 : stack_.back(), s.tag, s.index, s.clip_x, s.clip_y, {}, {} });
    stack_.push_back(static_cast<int>(records_.size() - 1));
}

void Ui::close() {
    Clay__CloseElement();
    stack_.pop_back();
}

void Ui::end() {
    commands_ = Clay_EndLayout();
    faults_ = clay_errors_;
    const RectF window{ 0, 0, w_, h_ };
    for (Record& r : records_) {
        const Clay_ElementData data = Clay_GetElementData(r.id);
        r.box = data.found ? rect(data.boundingBox) : RectF{};
        const RectF clip = r.parent < 0 ? window : records_[static_cast<size_t>(r.parent)].visible;
        r.visible = intersect(r.box, clip);
        // THE FAULT CHECK: inside the parent on every axis the parent does not clip, half a pixel allowed for
        // rounding. Clipped axes are scrolled ones, where being outside is the point.
        const RectF parent = r.parent < 0 ? window : records_[static_cast<size_t>(r.parent)].box;
        const bool px = r.parent >= 0 && records_[static_cast<size_t>(r.parent)].clip_x;
        const bool py = r.parent >= 0 && records_[static_cast<size_t>(r.parent)].clip_y;
        const float e = 0.5f;
        const bool out_x = !px && (r.box.x < parent.x - e || r.box.x + r.box.w > parent.x + parent.w + e);
        const bool out_y = !py && (r.box.y < parent.y - e || r.box.y + r.box.h > parent.y + parent.h + e);
        if (data.found && (out_x || out_y))
            faults_.push_back(std::format("{}#{} ({:.0f}x{:.0f} at {:.0f},{:.0f}) is outside its parent ({:.0f}x{:.0f} at {:.0f},{:.0f})",
                                          r.tag != nullptr ? r.tag : "element", r.index, r.box.w, r.box.h, r.box.x, r.box.y,
                                          parent.w, parent.h, parent.x, parent.y));
    }
    last_ = records_;
}

void Ui::render(Canvas& canvas, float scale) {
    std::vector<RectF> clips;
    const auto sc = [scale](Clay_BoundingBox b) { return RectF{ b.x * scale, b.y * scale, b.width * scale, b.height * scale }; };
    const auto radii = [scale](Clay_CornerRadius r) { return Radii{ r.topLeft * scale, r.topRight * scale, r.bottomLeft * scale, r.bottomRight * scale }; };
    for (int32_t i = 0; i < commands_.length; i++) {
        const Clay_RenderCommand* c = Clay_RenderCommandArray_Get(&commands_, i);
        const RectF box = sc(c->boundingBox);
        switch (c->commandType) {
            case CLAY_RENDER_COMMAND_TYPE_RECTANGLE:
                canvas.fill(box, radii(c->renderData.rectangle.cornerRadius), rgba(c->renderData.rectangle.backgroundColor));
                break;
            case CLAY_RENDER_COMMAND_TYPE_BORDER: {
                const Clay_BorderWidth& bw = c->renderData.border.width;
                const float w[4] = { bw.left * scale, bw.right * scale, bw.top * scale, bw.bottom * scale };
                canvas.border(box, radii(c->renderData.border.cornerRadius), w, rgba(c->renderData.border.color));
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_TEXT: {
                const Clay_TextRenderData& t = c->renderData.text;
                fonts_.draw(canvas, std::string_view(t.stringContents.chars, static_cast<size_t>(t.stringContents.length)),
                            static_cast<Face>(t.fontId), t.fontSize * scale, box.x, box.y, rgba(t.textColor));
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_IMAGE:
                canvas.image(box, *static_cast<const ImageView*>(c->renderData.image.imageData));
                break;
            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START: {
                const RectF r = clips.empty() ? box : intersect(box, clips.back());
                clips.push_back(r);
                canvas.set_clip(r);
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
                if (!clips.empty()) clips.pop_back();
                if (clips.empty()) canvas.reset_clip();
                else canvas.set_clip(clips.back());
                break;
            default: break;
        }
    }
    canvas.reset_clip();
}

std::optional<Hit> Ui::hit(float x, float y) const {
    // The last match is the innermost (records are in building order, a child after its parent).
    for (auto it = last_.rbegin(); it != last_.rend(); ++it)
        if (it->tag != nullptr && inside(it->visible, x, y)) return Hit{ it->tag, it->index };
    return std::nullopt;
}

bool Ui::hovered(const char* tag, uint32_t index) const {
    for (const Record& r : last_)
        if (r.tag != nullptr && std::strcmp(r.tag, tag) == 0 && r.index == index) return inside(r.visible, mouse_x_, mouse_y_);
    return false;
}

std::optional<RectF> Ui::bounds(const char* tag, uint32_t index) const {
    for (const Record& r : last_)
        if (r.tag != nullptr && std::strcmp(r.tag, tag) == 0 && r.index == index) return r.box;
    return std::nullopt;
}

// ---- elements ----------------------------------------------------------------------------------------

Box::Box(const Style& s) { Ui::current().open(s); }
Box::~Box() { Ui::current().close(); }

void text(std::string_view s0, Role role, Rgba ink, TextOpts opts) {
    Ui& ui = Ui::current();
    Type t = type(role);
    if (opts.mono) t.face = Face::Mono;
    std::string s(s0);
    if (opts.caps) std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (opts.elide_at > 0) s = ui.fonts().elide(s, t.face, t.size, opts.elide_at);
    Style st;
    st.w = opts.wrap ? Size::grow() : Size::fit();
    Box b(st);
    Clay_TextElementConfig cfg{};
    cfg.textColor = clay(ink);
    cfg.fontId = static_cast<uint16_t>(t.face);
    cfg.fontSize = t.size;
    cfg.wrapMode = opts.wrap ? CLAY_TEXT_WRAP_WORDS : CLAY_TEXT_WRAP_NONE;
    Clay__OpenTextElement(ui.keep(std::move(s)), Clay__StoreTextElementConfig(cfg));
}

void filler() {
    Style st;
    st.w = Size::grow();
    Box b(st);
}

void caps(std::string_view s) { text(s, Role::Caption, theme().dim, { .caps = true }); }

Card::Card(const char* tag, uint32_t index)
    : box_([&] {
          Style s;
          s.tag = tag;
          s.index = index;
          s.w = Size::grow();
          s.pad_x = s.pad_y = px(Space::M) + 2;
          s.gap = px(Space::S);
          s.background = theme().card;
          s.border = theme().line;
          s.radius = kRadius;
          return s;
      }()) {}

namespace {

void control_box(const char* tag, uint32_t index, std::string_view label, Rgba bg, Rgba ink, Size w = Size::fit()) {
    Style s;
    s.tag = tag;
    s.index = index;
    s.w = w;
    s.row = true;
    s.pad_x = px(Space::M);
    s.pad_y = 6;
    s.align_x = CLAY_ALIGN_X_CENTER;
    s.align_y = CLAY_ALIGN_Y_CENTER;
    s.background = bg;
    s.radius = kRadiusSmall;
    Box b(s);
    text(label, Role::Control, ink);
}

}  // namespace

void button(const char* tag, uint32_t index, std::string_view label, ButtonKind kind) {
    const Theme& t = theme();
    const bool hov = Ui::current().hovered(tag, index);
    if (kind == ButtonKind::Primary) control_box(tag, index, label, hov ? with_alpha(t.accent, 225) : t.accent, t.on_strong);
    else control_box(tag, index, label, hov ? t.control_hover : t.control, t.text);
}

void push_button(const char* tag, uint32_t index, std::string_view label, bool on) {
    const Theme& t = theme();
    const bool hov = Ui::current().hovered(tag, index);
    control_box(tag, index, label, on ? t.success : hov ? t.control_hover : t.control, on ? t.on_strong : t.text, Size::grow());
}

void segmented(const char* tag, const std::vector<std::string>& labels, size_t selected) {
    const Theme& t = theme();
    Style row;
    row.w = Size::grow();
    row.row = true;
    row.pad_x = row.pad_y = 2;
    row.gap = 2;
    row.background = t.well;
    row.radius = kRadiusSmall + 2;
    Box b(row);
    for (size_t i = 0; i < labels.size(); i++) {
        const auto idx = static_cast<uint32_t>(i);
        const bool hov = Ui::current().hovered(tag, idx);
        control_box(tag, idx, labels[i], i == selected ? t.control_hover : hov ? t.control : with_alpha(t.control, 0),
                    i == selected ? t.text : t.dim, Size::grow());
    }
}

void toggle(const char* tag, uint32_t index, bool on) {
    const Theme& t = theme();
    const bool hov = Ui::current().hovered(tag, index);
    Style track;
    track.tag = tag;
    track.index = index;
    track.w = Size::fixed(38);
    track.h = Size::fixed(22);
    track.row = true;
    track.pad_x = 3;
    track.align_x = on ? CLAY_ALIGN_X_RIGHT : CLAY_ALIGN_X_LEFT;
    track.align_y = CLAY_ALIGN_Y_CENTER;
    track.background = on ? t.accent : hov ? t.control_hover : t.control;
    track.radius = 11;
    Box b(track);
    Style knob;
    knob.w = knob.h = Size::fixed(16);
    knob.background = on ? t.on_strong : t.dim;
    knob.radius = 8;
    Box k(knob);
}

void badge(std::string_view label, Tone tone, bool strong) {
    const Rgba c = tone_colour(tone);
    Style s;
    s.row = true;
    s.pad_x = px(Space::S);
    s.pad_y = 2;
    s.align_y = CLAY_ALIGN_Y_CENTER;
    s.background = strong ? c : with_alpha(c, 45);
    s.radius = 9;
    Box b(s);
    text(label, Role::Secondary, strong ? theme().on_strong : c);
}

}  // namespace oxs::ui
