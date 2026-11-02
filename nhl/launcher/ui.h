// The launcher's look ("Silver night"): theme colours, the embedded fonts, the
// game's own Stanley Cup photo as the backdrop, and custom-drawn widgets. Every
// widget is a navigable ImGui item, so mouse, keyboard and gamepad all work.
//
// Layout is designed at 1280x800 (the Steam Deck) and scaled by ui::S.
#pragma once
#include <d3d11.h>

#include <cstdint>
#include <string>
#include <vector>

#include "imgui.h"

namespace ui {

constexpr ImU32 kInk = IM_COL32(11, 15, 20, 255);
constexpr ImU32 kFg = IM_COL32(238, 242, 245, 255);
constexpr ImU32 kDim = IM_COL32(152, 164, 176, 255);
constexpr ImU32 kAccent = IM_COL32(232, 238, 243, 255);
constexpr ImU32 kCard = IM_COL32(255, 255, 255, 18);
constexpr ImU32 kCardHi = IM_COL32(255, 255, 255, 30);
constexpr ImU32 kOff = IM_COL32(57, 66, 76, 255);
constexpr ImU32 kGood = IM_COL32(143, 214, 164, 255);
constexpr ImU32 kWarn = IM_COL32(240, 179, 90, 255);

struct Fonts {
    ImFont* body = nullptr;     // Archivo Regular
    ImFont* body_b = nullptr;   // Archivo SemiBold
    ImFont* head = nullptr;     // Big Shoulders Display Bold
    ImFont* head_x = nullptr;   // Big Shoulders Display ExtraBold
};
extern Fonts F;
extern float S;   // design px -> screen px
inline float px(float v) { return v * S; }
inline ImVec2 px(float x, float y) { return ImVec2(x * S, y * S); }
// A colour with the current style alpha (page fades) applied.
inline ImU32 C(ImU32 c, float a = 1.0f) { return ImGui::GetColorU32(c, a); }

// Fonts from the exe's resources; falls back to Segoe UI if they are missing.
void load_fonts();
// Per-frame: S from the window size, style sizes scaled to match.
void begin_frame(float w, float h);

// ---- the backdrop photo (PIC1.PNG from the player's own game files) ----------
struct Photo {
    ID3D11ShaderResourceView* sharp = nullptr;
    ID3D11ShaderResourceView* blur = nullptr;
    int w = 0, h = 0;
    bool ok() const { return sharp != nullptr; }
};
// Decodes a PNG and uploads it cropped to the top 75% (the bottom of PIC1 holds
// the Dolby / DTS notices), plus a blurred copy for the settings pages.
bool make_photo(ID3D11Device* dev, const std::vector<uint8_t>& png, Photo* out);
void free_photo(Photo* p);
// blur: 0 = the sharp hero with a left-side shade, 1 = blurred and darkened.
void draw_backdrop(ImDrawList* dl, const Photo& p, ImVec2 a, ImVec2 b, float blur);

// ---- text ---------------------------------------------------------------------
ImVec2 text_size(ImFont* f, float size, const char* s, float tracking = 0);
void text(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, ImU32 col, const char* s, float tracking = 0);
// In-flow text (advances the cursor). wrap = width in screen px, 0 = no wrap.
void para(ImFont* f, float size, ImU32 col, const char* s, float wrap = 0);
void section(const char* label);   // small spaced caps heading above a group of rows
void gap(float design_px);

// ---- animation ----------------------------------------------------------------
// Eases a per-id value toward target; returns the current value.
float anim(ImGuiID id, float target, float speed = 14.0f);

// ---- widgets (all navigable) ------------------------------------------------------
enum class Btn { Primary, Secondary, Hero };
bool button(const char* label, ImVec2 size, Btn kind = Btn::Secondary);
// Rows fill the given width (screen px). help goes to the info panel while the
// row is hovered or focused.
// note: optional dim text after the label.
bool toggle_row(const char* label, bool* v, const char* help, float w, const char* note = nullptr);
// A toggle row with a place in a load order: its number on the left, up / down arrows
// for the mouse; Y picks it up so the d-pad moves it (A / B / Y put it down);
// Alt+Up / Alt+Down on the keyboard. dragging = held and dragged with the mouse (the
// caller moves it to where the mouse is).
enum class RowAct { None, Toggle, Up, Down };
RowAct order_row(const char* label, bool* v, const char* help, float w, const char* note, int number, bool can_up,
                 bool can_down, bool* grabbed, bool* dragging = nullptr);
bool choice_row(const char* label, int* idx, const char* const* labels, int n, const char* help, float w);
// A selectable card (presets).
bool tile(const char* title, const char* sub, bool selected, ImVec2 size, const char* help);

// Text for the info panel (what the focused / hovered control does).
void set_info(const char* title, const char* text);
const char* info_title();
const char* info_text();
void clear_info();

// Cards: a translucent panel behind whatever is drawn between begin and end.
void begin_card(float w, float pad = 22);
void end_card();

// Icons drawn from primitives.
void icon_check(ImDrawList* dl, ImVec2 c, float r, ImU32 col);
void icon_warn(ImDrawList* dl, ImVec2 c, float r, ImU32 col);
// One controller hint: glyph is "A", "B", "Y", "LB", "RB", "START", "LR" (d-pad left/right) or "UD" (up/down).
float hint(ImDrawList* dl, ImVec2 right_edge, const char* glyph, const char* label);   // returns its width

bool gamepad();   // a controller is connected (hints are shown)

}  // namespace ui
