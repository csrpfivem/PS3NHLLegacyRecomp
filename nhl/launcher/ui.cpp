#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui.h"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "imgui_internal.h"
#include "resource.h"

namespace ui {

Fonts F;
float S = 1.0f;

namespace {

ImU32 lerp_col(ImU32 a, ImU32 b, float t)
{
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImLerp(x, y, ImClamp(t, 0.0f, 1.0f)));
}

ImU32 with_alpha(ImU32 c, float a)
{
    const ImU32 al = (ImU32)((float)((c >> IM_COL32_A_SHIFT) & 0xff) * ImClamp(a, 0.0f, 1.0f));
    return (c & ~IM_COL32_A_MASK) | (al << IM_COL32_A_SHIFT);
}

ImFont* font_res(int id, float size)
{
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return nullptr;
    void* p = LockResource(LoadResource(nullptr, r));
    const DWORD n = SizeofResource(nullptr, r);
    if (!p || !n) return nullptr;
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;   // the resource stays mapped for the life of the process
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(p, (int)n, size, &cfg);
}

ImFont* font_sys(const wchar_t* file, float size)
{
    wchar_t win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    const std::wstring w = std::wstring(win) + L"\\Fonts\\" + file;
    if (GetFileAttributesW(w.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
    char path[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, path, sizeof path, nullptr, nullptr);
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(path, size);
}

ID3D11ShaderResourceView* upload(ID3D11Device* dev, const void* rgba, int w, int h)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = { rgba, (UINT)w * 4, 0 };
    ID3D11Texture2D* t = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, &sd, &t))) return nullptr;
    ID3D11ShaderResourceView* v = nullptr;
    dev->CreateShaderResourceView(t, nullptr, &v);
    t->Release();
    return v;
}

// Box blur, one axis, RGBA8 in place (radius r).
void box_pass(std::vector<uint8_t>& img, int w, int h, int r, bool horizontal)
{
    const int n = horizontal ? w : h, lines = horizontal ? h : w;
    const int step = horizontal ? 4 : w * 4;
    std::vector<uint8_t> line((size_t)n * 4);
    for (int l = 0; l < lines; l++) {
        uint8_t* base = img.data() + (horizontal ? (size_t)l * w * 4 : (size_t)l * 4);
        for (int i = 0; i < n; i++) memcpy(&line[(size_t)i * 4], base + (size_t)i * step, 4);
        for (int c = 0; c < 4; c++) {
            int sum = 0;
            for (int i = -r; i <= r; i++) sum += line[(size_t)std::clamp(i, 0, n - 1) * 4 + c];
            for (int i = 0; i < n; i++) {
                base[(size_t)i * step + c] = (uint8_t)(sum / (2 * r + 1));
                sum += line[(size_t)std::min(i + r + 1, n - 1) * 4 + c] - line[(size_t)std::max(i - r, 0) * 4 + c];
            }
        }
    }
}

std::string g_info_title, g_info_text;
int g_info_prio = -1;

void info(const char* title, const char* text, int prio)
{
    if (prio < g_info_prio) return;
    g_info_prio = prio;
    g_info_title = title ? title : "";
    g_info_text = text ? text : "";
}

struct It {
    bool pressed = false, hov = false, foc = false, held = false, dis = false;
    ImVec2 a, b;
    ImGuiID id = 0;
};

It item(const char* id, ImVec2 size)
{
    It it;
    it.a = ImGui::GetCursorScreenPos();
    it.b = it.a + size;
    it.dis = (GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
    it.pressed = ImGui::InvisibleButton(id, ImMax(size, ImVec2(1, 1)), ImGuiButtonFlags_EnableNav) && !it.dis;
    it.id = ImGui::GetItemID();
    it.hov = !it.dis && ImGui::IsItemHovered();
    it.foc = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    it.held = ImGui::IsItemActive();
    return it;
}

const char* label_end(const char* s) { return ImGui::FindRenderedTextEnd(s); }

void row_bg(const It& it)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float hot = anim(it.id, (it.hov || it.foc) ? 1.0f : 0.0f);
    const float foc = anim(it.id + 1, it.foc ? 1.0f : 0.0f);
    if (hot > 0.003f) dl->AddRectFilled(it.a, it.b, C(kCardHi, hot), px(4));
    if (foc > 0.003f)
        dl->AddRectFilled(it.a, ImVec2(it.a.x + px(4), it.b.y), C(kAccent, foc), px(4), ImDrawFlags_RoundCornersLeft);
}

void chevron(ImDrawList* dl, ImVec2 c, float s, bool right, ImU32 col)
{
    const float d = right ? 1.0f : -1.0f;
    dl->AddLine(ImVec2(c.x - d * s * 0.5f, c.y - s), ImVec2(c.x + d * s * 0.5f, c.y), col, px(2));
    dl->AddLine(ImVec2(c.x + d * s * 0.5f, c.y), ImVec2(c.x - d * s * 0.5f, c.y + s), col, px(2));
}

struct Card { ImVec2 start; float w = 0, pad = 0; ImDrawList* dl = nullptr; } g_card;

}  // namespace

void load_fonts()
{
    F.body = font_res(IDR_FONT_BODY, 18.0f);
    F.body_b = font_res(IDR_FONT_BODY_B, 18.0f);
    F.head = font_res(IDR_FONT_HEAD, 18.0f);
    F.head_x = font_res(IDR_FONT_HEAD_X, 18.0f);
    if (!F.body) F.body = font_sys(L"segoeui.ttf", 18.0f);
    if (!F.body_b) F.body_b = font_sys(L"segoeuib.ttf", 18.0f);
    if (!F.head) F.head = font_sys(L"bahnschrift.ttf", 18.0f);
    if (!F.body) F.body = ImGui::GetIO().Fonts->AddFontDefault();
    if (!F.body_b) F.body_b = F.body;
    if (!F.head) F.head = F.body_b;
    if (!F.head_x) F.head_x = F.head;
}

void begin_frame(float w, float h)
{
    static ImGuiStyle base;
    static bool have_base = false;
    static float last = -1.0f;
    float s = std::min(w / 1280.0f, h / 800.0f);
    s = std::clamp(std::round(s * 40.0f) / 40.0f, 0.5f, 4.0f);
    S = s;
    if (s != last) {
        ImGuiStyle& st = ImGui::GetStyle();
        if (!have_base) { base = st; have_base = true; }
        st = base;
        st.ScaleAllSizes(s);
        st.FontScaleMain = s;
        st.FontScaleDpi = 1.0f;
        last = s;
    }
    clear_info();
}

// ---- photo ----------------------------------------------------------------------

bool make_photo(ID3D11Device* dev, const std::vector<uint8_t>& png, Photo* out)
{
    if (png.empty()) return false;
    IWICImagingFactory* fac = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac)))) return false;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* cv = nullptr;
    std::vector<uint8_t> rgba;
    UINT w = 0, h = 0, ch = 0;
    bool ok = SUCCEEDED(fac->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromMemory((BYTE*)png.data(), (DWORD)png.size())) &&
              SUCCEEDED(fac->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(fac->CreateFormatConverter(&cv)) &&
              SUCCEEDED(cv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(cv->GetSize(&w, &h)) && w >= 64 && h >= 64;
    if (ok) {
        ch = h * 3 / 4;   // drop the bottom quarter (audio format notices)
        rgba.resize((size_t)w * ch * 4);
        const WICRect rc = { 0, 0, (INT)w, (INT)ch };
        ok = SUCCEEDED(cv->CopyPixels(&rc, w * 4, (UINT)rgba.size(), rgba.data()));
    }
    if (cv) cv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (stream) stream->Release();
    fac->Release();
    if (!ok) return false;

    // Blurred copy: a quarter-size average, then three box passes per axis.
    const int bw = (int)w / 4, bh = (int)ch / 4;
    std::vector<uint8_t> thumb((size_t)bw * bh * 4);
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++)
            for (int c = 0; c < 4; c++) {
                int sum = 0;
                for (int j = 0; j < 4; j++)
                    for (int i = 0; i < 4; i++) sum += rgba[(((size_t)(y * 4 + j) * w) + (size_t)(x * 4 + i)) * 4 + c];
                thumb[((size_t)y * bw + x) * 4 + c] = (uint8_t)(sum / 16);
            }
    for (int pass = 0; pass < 3; pass++) {
        box_pass(thumb, bw, bh, 6, true);
        box_pass(thumb, bw, bh, 6, false);
    }

    free_photo(out);
    out->sharp = upload(dev, rgba.data(), (int)w, (int)ch);
    out->blur = upload(dev, thumb.data(), bw, bh);
    out->w = (int)w;
    out->h = (int)ch;
    if (!out->sharp || !out->blur) { free_photo(out); return false; }
    return true;
}

void free_photo(Photo* p)
{
    if (p->sharp) p->sharp->Release();
    if (p->blur) p->blur->Release();
    *p = Photo();
}

void draw_backdrop(ImDrawList* dl, const Photo& p, ImVec2 a, ImVec2 b, float blur)
{
    const float W = b.x - a.x, H = b.y - a.y;
    dl->AddRectFilledMultiColor(a, b, IM_COL32(20, 27, 36, 255), IM_COL32(20, 27, 36, 255), kInk, kInk);
    if (p.ok()) {
        // "cover", anchored to the top right where the Cup stands
        const float sc = std::max(W / (float)p.w, H / (float)p.h);
        const float dw = (float)p.w * sc, dh = (float)p.h * sc;
        const ImVec2 uv0((dw - W) / dw, 0), uv1(1, H / dh);
        if (blur < 0.999f)
            dl->AddImage((ImTextureID)(intptr_t)p.sharp, a, b, uv0, uv1, IM_COL32(255, 255, 255, (int)(255 * (1 - blur))));
        if (blur > 0.001f)
            dl->AddImage((ImTextureID)(intptr_t)p.blur, a, b, uv0, uv1, IM_COL32(255, 255, 255, (int)(255 * blur)));
    }
    // Shade: strong on the left for the text, clear over the Cup; flat when blurred.
    const float xs[] = { 0.0f, 0.38f, 0.70f, 1.0f }, al[] = { 0.97f, 0.88f, 0.25f, 0.10f };
    for (int i = 0; i < 3; i++) {
        const float a0 = ImLerp(al[i], 0.84f, blur), a1 = ImLerp(al[i + 1], 0.84f, blur);
        const ImU32 c0 = with_alpha(kInk, a0), c1 = with_alpha(kInk, a1);
        dl->AddRectFilledMultiColor(ImVec2(a.x + W * xs[i], a.y), ImVec2(a.x + W * xs[i + 1], b.y), c0, c1, c1, c0);
    }
    const float bot = 0.9f * (1 - blur);
    if (bot > 0.01f)
        dl->AddRectFilledMultiColor(ImVec2(a.x, a.y + H * 0.7f), b, with_alpha(kInk, 0), with_alpha(kInk, 0),
                                    with_alpha(kInk, bot), with_alpha(kInk, bot));
}

// ---- text -------------------------------------------------------------------------

ImVec2 text_size(ImFont* f, float size, const char* s, float tracking)
{
    ImVec2 sz = f->CalcTextSizeA(size, FLT_MAX, 0.0f, s);
    if (tracking != 0.0f) {
        int n = 0;
        for (const char* p = s; *p;) {
            unsigned int c;
            p += ImTextCharFromUtf8(&c, p, nullptr);
            n++;
        }
        if (n > 1) sz.x += tracking * (float)(n - 1);
    }
    return sz;
}

void text(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, ImU32 col, const char* s, float tracking)
{
    if (tracking == 0.0f) { dl->AddText(f, size, pos, C(col), s); return; }
    for (const char* p = s; *p;) {
        unsigned int c;
        const char* e = p + ImTextCharFromUtf8(&c, p, nullptr);
        dl->AddText(f, size, pos, C(col), p, e);
        pos.x += f->CalcTextSizeA(size, FLT_MAX, 0.0f, p, e).x + tracking;
        p = e;
    }
}

void para(ImFont* f, float size, ImU32 col, const char* s, float wrap)
{
    ImGui::PushFont(f, size);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    if (wrap > 0) ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    ImGui::TextUnformatted(s);
    if (wrap > 0) ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void section(const char* label)
{
    gap(10);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    text(ImGui::GetWindowDrawList(), F.head, px(17), ImVec2(p.x + px(18), p.y), kDim, label, px(1.8f));
    ImGui::Dummy(ImVec2(1, px(22)));
}

void gap(float design_px) { ImGui::Dummy(ImVec2(1, px(design_px))); }

float anim(ImGuiID id, float target, float speed)
{
    float* v = ImGui::GetStateStorage()->GetFloatRef(id, target);
    *v += (target - *v) * (1.0f - std::exp(-speed * ImGui::GetIO().DeltaTime));
    if (std::fabs(target - *v) < 0.002f) *v = target;
    return *v;
}

// ---- widgets ------------------------------------------------------------------------

bool button(const char* label, ImVec2 size, Btn kind)
{
    It it = item(label, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float hot = anim(it.id, (it.hov || it.foc) ? 1.0f : 0.0f);
    const float foc = anim(it.id + 1, it.foc ? 1.0f : 0.0f);
    const float a = it.dis ? 0.35f : 1.0f;
    const float r = px(4);
    ImVec2 A = it.a, B = it.b;
    if (it.held && !it.dis) { A += px(1.5f, 1.5f); B -= px(1.5f, 1.5f); }
    ImU32 tc;
    if (kind == Btn::Secondary) {
        dl->AddRectFilled(A, B, C(kCardHi, hot * a), r);
        dl->AddRect(A, B, C(lerp_col(with_alpha(kDim, 0.6f), kFg, hot), a), r, 0, px(1.5f));
        tc = with_alpha(kFg, a);
    } else {
        if (kind == Btn::Hero && !it.dis) {   // slow breathing glow
            const float t = (float)ImGui::GetTime();
            for (int i = 1; i <= 3; i++)
                dl->AddRect(A - px(3.0f * i, 3.0f * i), B + px(3.0f * i, 3.0f * i),
                            C(kAccent, (0.07f + 0.05f * std::sin(t * 2.2f)) / (float)i * (1 - foc)), r + px(3.0f * i), 0,
                            px(2));
        }
        dl->AddRectFilled(A, B, C(lerp_col(kAccent, IM_COL32_WHITE, hot), a), r);
        tc = with_alpha(kInk, a);
    }
    if (foc > 0.003f) dl->AddRect(it.a - px(5, 5), it.b + px(5, 5), C(kAccent, foc * a), r + px(4), 0, px(2.5f));
    ImFont* f = kind == Btn::Hero ? F.head_x : F.body_b;
    const float fs = kind == Btn::Hero ? px(40) : px(17), tr = kind == Btn::Hero ? px(5.5f) : 0.0f;
    std::string shown(label, label_end(label));
    const ImVec2 ts = text_size(f, fs, shown.c_str(), tr);
    text(dl, f, fs, ImFloor((A + B - ts) * 0.5f), tc, shown.c_str(), tr);
    return it.pressed;
}

bool toggle_row(const char* label, bool* v, const char* help, float w, const char* note)
{
    It it = item(label, ImVec2(w, px(50)));
    bool changed = false;
    if (it.pressed) { *v = !*v; changed = true; }
    row_bg(it);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = (it.a.y + it.b.y) * 0.5f, a = it.dis ? 0.4f : 1.0f;
    std::string shown(label, label_end(label));
    const float sw = px(46), sh = px(26);
    dl->PushClipRect(it.a, ImVec2(it.b.x - px(30) - sw, it.b.y), true);   // long mod names stop before the switch
    text(dl, F.body, px(18), ImVec2(it.a.x + px(18), cy - px(11)), with_alpha(kFg, a), shown.c_str());
    if (note) {
        const float x = it.a.x + px(18) + text_size(F.body, px(18), shown.c_str()).x + px(12);
        text(dl, F.body, px(15), ImVec2(x, cy - px(9)), with_alpha(kDim, a), note);
    }
    dl->PopClipRect();
    const ImVec2 sa(it.b.x - px(18) - sw, cy - sh * 0.5f);
    const float k = anim(it.id + 2, *v ? 1.0f : 0.0f, 18.0f);
    dl->AddRectFilled(sa, sa + ImVec2(sw, sh), C(lerp_col(kOff, kAccent, k), a), sh * 0.5f);
    const float kr = sh * 0.5f - px(3);
    dl->AddCircleFilled(ImVec2(sa.x + sh * 0.5f + k * (sw - sh), cy), kr, C(lerp_col(kFg, kInk, k), a));
    if (it.foc) info(shown.c_str(), help, 2);
    else if (it.hov) info(shown.c_str(), help, 1);
    return changed;
}

RowAct order_row(const char* label, bool* v, const char* help, float w, const char* note, int number, bool can_up,
                 bool can_down, bool* grabbed, bool* dragging)
{
    ImGui::SetNextItemAllowOverlap();   // the arrow buttons sit on top of the row
    It it = item(label, ImVec2(w, px(50)));
    const ImVec2 after = ImGui::GetCursorScreenPos();
    RowAct act = RowAct::None;
    // Mouse drag: a held row that moved past a few pixels is being dragged (the
    // caller moves it); letting go after a drag does not toggle it.
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID drag_key = it.id + 6;
    const bool was_drag = st->GetBool(drag_key);
    if (!it.held) st->SetBool(drag_key, false);
    else if (ImGui::IsMouseDragging(0, px(6))) st->SetBool(drag_key, true);
    const bool drag = it.held && st->GetBool(drag_key);
    if (dragging) *dragging = drag;
    if (drag) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    const bool focused = ImGui::IsItemFocused() && !it.dis;
    if (!focused) *grabbed = false;
    if (focused) {
        ImGui::SetKeyOwner(ImGuiKey_GamepadFaceUp, it.id);
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, 0, it.id)) *grabbed = !*grabbed;
        // keys that move the row belong to it (not to navigation): the d-pad / stick
        // while it is picked up, the arrow keys while Alt is held
        static const ImGuiKey kUp[] = { ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp, ImGuiKey_UpArrow };
        static const ImGuiKey kDown[] = { ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown, ImGuiKey_DownArrow };
        for (int i = 0; i < 3; i++) {
            if (!(*grabbed || (i == 2 && ImGui::GetIO().KeyAlt))) continue;
            ImGui::SetKeyOwner(kUp[i], it.id);
            ImGui::SetKeyOwner(kDown[i], it.id);
            if (ImGui::IsKeyPressed(kUp[i], ImGuiInputFlags_Repeat, it.id) && can_up) act = RowAct::Up;
            if (ImGui::IsKeyPressed(kDown[i], ImGuiInputFlags_Repeat, it.id) && can_down) act = RowAct::Down;
        }
        if (*grabbed)
            for (ImGuiKey k : { ImGuiKey_GamepadFaceRight, ImGuiKey_Escape }) {
                ImGui::SetKeyOwner(k, it.id);
                if (ImGui::IsKeyPressed(k, 0, it.id)) *grabbed = false;
            }
    }
    if (it.pressed && !was_drag) {
        if (*grabbed) *grabbed = false;
        else { *v = !*v; act = RowAct::Toggle; }
    }
    if (*grabbed) ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    row_bg(it);
    const float lift = anim(it.id + 4, (*grabbed || drag) ? 1.0f : 0.0f, 16.0f);
    if (lift > 0.003f) dl->AddRect(it.a, it.b, C(kAccent, lift), px(4), 0, px(2));
    const float cy = (it.a.y + it.b.y) * 0.5f, a = it.dis ? 0.4f : 1.0f;
    char num[8];
    snprintf(num, sizeof num, "%d", number);
    const ImVec2 ns = text_size(F.head, px(20), num);
    text(dl, F.head, px(20), ImVec2(it.a.x + px(30) - ns.x * 0.5f, cy - px(12)), with_alpha(kDim, a), num);
    const float sw = px(46), sh = px(26), ax = it.b.x - px(18) - sw - px(84);
    std::string shown(label, label_end(label));
    dl->PushClipRect(it.a, ImVec2(ax - px(8), it.b.y), true);
    text(dl, F.body, px(18), ImVec2(it.a.x + px(54), cy - px(11)), with_alpha(kFg, a), shown.c_str());
    if (note) {
        const float x = it.a.x + px(54) + text_size(F.body, px(18), shown.c_str()).x + px(12);
        text(dl, F.body, px(15), ImVec2(x, cy - px(9)), with_alpha(kDim, a), note);
    }
    dl->PopClipRect();
    const ImVec2 sa(it.b.x - px(18) - sw, cy - sh * 0.5f);
    const float k = anim(it.id + 2, *v ? 1.0f : 0.0f, 18.0f);
    dl->AddRectFilled(sa, sa + ImVec2(sw, sh), C(lerp_col(kOff, kAccent, k), a), sh * 0.5f);
    dl->AddCircleFilled(ImVec2(sa.x + sh * 0.5f + k * (sw - sh), cy), sh * 0.5f - px(3), C(lerp_col(kFg, kInk, k), a));

    // Up / down arrows (mouse); shown while the row is hovered, focused or picked up.
    const float show = anim(it.id + 5, (it.hov || it.foc || *grabbed || ImGui::IsMouseHoveringRect(it.a, it.b)) ? 1.0f : 0.0f);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    ImGui::PushID((int)it.id);   // arrow ids are per row
    for (int i = 0; i < 2; i++) {
        const bool up = i == 0, can = up ? can_up : can_down;
        const ImVec2 b0(ax + px(40.0f * i), cy - px(17)), b1 = b0 + px(34, 34);
        ImGui::SetCursorScreenPos(b0);
        ImGui::PushID(i);
        const bool pressed = ImGui::InvisibleButton("##move", px(34, 34)) && can && !it.dis;
        const bool hov = ImGui::IsItemHovered() && can;
        ImGui::PopID();
        if (pressed) act = up ? RowAct::Up : RowAct::Down;
        if (show < 0.003f) continue;
        if (hov) dl->AddRectFilled(b0, b1, C(kCardHi, show), px(4));
        const ImVec2 c = (b0 + b1) * 0.5f;
        const ImU32 col = C(can ? kFg : kOff, show * a);
        if (up) dl->AddTriangleFilled(c + px(0, -6), c + px(-7, 5), c + px(7, 5), col);
        else dl->AddTriangleFilled(c + px(0, 6), c + px(7, -5), c + px(-7, -5), col);
    }
    ImGui::PopID();
    ImGui::PopItemFlag();
    ImGui::SetCursorScreenPos(after);

    if (it.foc) info(shown.c_str(), help, 2);
    else if (it.hov) info(shown.c_str(), help, 1);
    return act;
}

bool choice_row(const char* label, int* idx, const char* const* labels, int n, const char* help, float w)
{
    It it = item(label, ImVec2(w, px(50)));
    const int old = *idx;
    if (ImGui::IsItemFocused() && !it.dis) {
        // d-pad / stick / arrow keys change the value; they belong to this row, not to navigation
        static const ImGuiKey kL[] = { ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft, ImGuiKey_LeftArrow };
        static const ImGuiKey kR[] = { ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight, ImGuiKey_RightArrow };
        for (ImGuiKey k : kL) {
            ImGui::SetKeyOwner(k, it.id);
            if (ImGui::IsKeyPressed(k, ImGuiInputFlags_Repeat, it.id) && *idx > 0) --*idx;
        }
        for (ImGuiKey k : kR) {
            ImGui::SetKeyOwner(k, it.id);
            if (ImGui::IsKeyPressed(k, ImGuiInputFlags_Repeat, it.id) && *idx < n - 1) ++*idx;
        }
    }
    row_bg(it);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = (it.a.y + it.b.y) * 0.5f, a = it.dis ? 0.4f : 1.0f;
    std::string shown(label, label_end(label));
    text(dl, F.body, px(18), ImVec2(it.a.x + px(18), cy - px(11)), with_alpha(kFg, a), shown.c_str());
    const char* val = labels[*idx];
    const ImVec2 vs = text_size(F.body_b, px(17), val);
    const float rx = it.b.x - px(24), vx = rx - px(20) - vs.x, lx = vx - px(20);
    if (it.pressed) {
        if (ImGui::IsMouseReleased(0) && ImGui::GetIO().MousePos.x < vx - px(4)) *idx = std::max(*idx - 1, 0);
        else if (ImGui::IsMouseReleased(0) && ImGui::GetIO().MousePos.x > rx - px(14)) *idx = std::min(*idx + 1, n - 1);
        else *idx = (*idx + 1) % n;
    }
    const float nudge = anim(it.id + 3, 0.0f, 16.0f);
    text(dl, F.body_b, px(17), ImVec2(vx + nudge, cy - px(10.5f)), with_alpha(kFg, a), val);
    chevron(dl, ImVec2(lx, cy), px(6), false, C(*idx > 0 ? kFg : kOff, a));
    chevron(dl, ImVec2(rx, cy), px(6), true, C(*idx < n - 1 ? kFg : kOff, a));
    if (*idx != old) ImGui::GetStateStorage()->SetFloat(it.id + 3, *idx > old ? px(10) : -px(10));
    if (it.foc) info(shown.c_str(), help, 2);
    else if (it.hov) info(shown.c_str(), help, 1);
    return *idx != old;
}

bool tile(const char* title, const char* sub, bool selected, ImVec2 size, const char* help)
{
    It it = item(title, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float hot = anim(it.id, (it.hov || it.foc) ? 1.0f : 0.0f);
    const float sel = anim(it.id + 1, selected ? 1.0f : 0.0f);
    const float foc = anim(it.id + 2, it.foc ? 1.0f : 0.0f);
    dl->AddRectFilled(it.a, it.b, C(lerp_col(kCard, kCardHi, hot)), px(5));
    if (sel > 0.003f) dl->AddRect(it.a, it.b, C(kAccent, sel), px(5), 0, px(2));
    if (foc > 0.003f) dl->AddRect(it.a - px(5, 5), it.b + px(5, 5), C(kAccent, foc), px(9), 0, px(2.5f));
    std::string shown(title, label_end(title));
    text(dl, F.body_b, px(18), it.a + px(18, 13), kFg, shown.c_str());
    ImGui::PushClipRect(it.a, it.b, true);
    dl->AddText(F.body, px(14), it.a + px(18, 39), C(kDim), sub, nullptr, size.x - px(36));
    ImGui::PopClipRect();
    if (selected) {
        const float r = px(9);
        icon_check(dl, ImVec2(it.b.x - px(20), it.a.y + px(22)), r, kAccent);
    }
    if (it.foc) info(shown.c_str(), help, 2);
    else if (it.hov) info(shown.c_str(), help, 1);
    return it.pressed;
}

void set_info(const char* title, const char* text) { info(title, text, 0); }
const char* info_title() { return g_info_title.c_str(); }
const char* info_text() { return g_info_text.c_str(); }
void clear_info()
{
    g_info_prio = -1;
    g_info_title.clear();
    g_info_text.clear();
}

void begin_card(float w, float pad)
{
    g_card.dl = ImGui::GetWindowDrawList();
    g_card.dl->ChannelsSplit(2);
    g_card.dl->ChannelsSetCurrent(1);
    g_card.start = ImGui::GetCursorScreenPos();
    g_card.w = w;
    g_card.pad = px(pad);
    ImGui::SetCursorScreenPos(g_card.start + ImVec2(g_card.pad, g_card.pad));
    ImGui::BeginGroup();
}

void end_card()
{
    ImGui::EndGroup();
    const float bottom = ImGui::GetItemRectMax().y + g_card.pad;
    g_card.dl->ChannelsSetCurrent(0);
    g_card.dl->AddRectFilled(g_card.start, ImVec2(g_card.start.x + g_card.w, bottom), C(kCard), px(6));
    g_card.dl->ChannelsMerge();
    ImGui::SetCursorScreenPos(ImVec2(g_card.start.x, bottom));
    ImGui::Dummy(ImVec2(g_card.w, 0));
}

void icon_check(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
{
    dl->AddCircleFilled(c, r, C(col));
    const ImVec2 pts[] = { c + ImVec2(-r * 0.45f, r * 0.02f), c + ImVec2(-r * 0.12f, r * 0.35f),
                           c + ImVec2(r * 0.48f, -r * 0.33f) };
    dl->AddPolyline(pts, 3, C(kInk), 0, std::max(1.5f, r * 0.22f));
}

void icon_warn(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
{
    dl->AddCircleFilled(c, r, C(col));
    const float t = std::max(1.5f, r * 0.2f);
    dl->AddLine(c + ImVec2(0, -r * 0.5f), c + ImVec2(0, r * 0.12f), C(kInk), t);
    dl->AddCircleFilled(c + ImVec2(0, r * 0.45f), t * 0.6f, C(kInk));
}

float hint(ImDrawList* dl, ImVec2 right, const char* glyph, const char* label)
{
    const ImVec2 ls = text_size(F.body, px(15), label);
    const float gh = px(22);
    const bool round = strlen(glyph) == 1;
    const float gw = round ? gh : (strcmp(glyph, "START") == 0 || strcmp(glyph, "LR") == 0) ? px(30)
                                  : strcmp(glyph, "UD") == 0 ? px(24) : px(32);
    const float total = gw + px(8) + ls.x;
    const ImVec2 g0(right.x - total, right.y - gh * 0.5f), g1 = g0 + ImVec2(gw, gh);
    const ImVec2 gc = (g0 + g1) * 0.5f;
    if (round) {
        dl->AddCircle(gc, gh * 0.5f, C(kDim), 0, px(1.5f));
        const ImVec2 ts = text_size(F.body_b, px(12), glyph);
        text(dl, F.body_b, px(12), ImFloor(gc - ts * 0.5f), kFg, glyph);
    } else {
        dl->AddRect(g0, g1, C(kDim), px(5), 0, px(1.5f));
        if (strcmp(glyph, "START") == 0) {
            for (int i = -1; i <= 1; i++)
                dl->AddLine(gc + ImVec2(-px(6), px(4.0f * i)), gc + ImVec2(px(6), px(4.0f * i)), C(kFg), px(1.5f));
        } else if (strcmp(glyph, "LR") == 0) {
            dl->AddTriangleFilled(gc + px(-11, 0), gc + px(-5, -5), gc + px(-5, 5), C(kFg));
            dl->AddTriangleFilled(gc + px(11, 0), gc + px(5, -5), gc + px(5, 5), C(kFg));
        } else if (strcmp(glyph, "UD") == 0) {
            dl->AddTriangleFilled(gc + px(0, -8), gc + px(-5, -2), gc + px(5, -2), C(kFg));
            dl->AddTriangleFilled(gc + px(0, 8), gc + px(5, 2), gc + px(-5, 2), C(kFg));
        } else {
            const ImVec2 ts = text_size(F.body_b, px(11), glyph);
            text(dl, F.body_b, px(11), ImFloor(gc - ts * 0.5f), kFg, glyph);
        }
    }
    text(dl, F.body, px(15), ImVec2(g1.x + px(8), right.y - ls.y * 0.5f), kDim, label);
    return total;
}

bool gamepad() { return (ImGui::GetIO().BackendFlags & ImGuiBackendFlags_HasGamepad) != 0; }

}  // namespace ui
