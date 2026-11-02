// NHLLauncher: first-run setup (the player's own disc + the 1.01 update -> an
// install NHLLegacy.exe runs from) and the front door afterwards (Play,
// graphics, mods, game files, help). Dear ImGui on D3D11 with the custom look in
// ui.cpp; mouse, keyboard and gamepad navigable (LB/RB switch pages).
//
// Args (dev): --install <dir> (default: the launcher's folder),
//             --eboot-elf <file> (a decrypted 1.01 ELF, until SELF decryption lands),
//             --page <0-4> (open on a home page: play, graphics, mods, game files, help).
#define IMGUI_DEFINE_MATH_OPERATORS
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "bug_report.h"
#include "ini.h"
#include "installer.h"
#include "iso9660.h"
#include "pkg.h"
#include "ui.h"

namespace fs = std::filesystem;
using ui::px;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

ID3D11Device* g_dev;
ID3D11DeviceContext* g_ctx;
IDXGISwapChain* g_swap;
ID3D11RenderTargetView* g_rtv;
UINT g_resize_w, g_resize_h;
HWND g_hwnd;
constexpr UINT WM_GAME_EXITED = WM_APP + 1;
std::vector<std::wstring> g_drops;   // files / folders dropped on the window
bool g_activated = false;             // window got focus back: re-read mods\ (edited in Explorer meanwhile)

void create_rtv()
{
    ID3D11Texture2D* bb = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
}

bool create_device(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (hr != S_OK) return false;
    create_rtv();
    return true;
}

LRESULT WINAPI wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) { g_resize_w = LOWORD(lp); g_resize_h = HIWORD(lp); }
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)lp)->ptMinTrackSize = POINT{ 800, 520 };
        return 0;
    case WM_DROPFILES: {
        HDROP d = (HDROP)wp;
        const UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            wchar_t p[MAX_PATH * 4];
            if (DragQueryFileW(d, i, p, (UINT)(sizeof p / sizeof p[0]))) g_drops.push_back(p);
        }
        DragFinish(d);
        return 0;
    }
    case WM_ACTIVATEAPP:
        if (wp) g_activated = true;
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Native open dialog; folders = pick a folder. Empty on cancel.
std::wstring pick(bool folders, const wchar_t* title, const wchar_t* filter_name, const wchar_t* filter)
{
    std::wstring out;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return out;
    DWORD opt = 0;
    d->GetOptions(&opt);
    d->SetOptions(opt | FOS_FORCEFILESYSTEM | (folders ? FOS_PICKFOLDERS : 0));
    d->SetTitle(title);
    if (!folders && filter) {
        COMDLG_FILTERSPEC spec[] = { { filter_name, filter }, { L"All files", L"*.*" } };
        d->SetFileTypes(2, spec);
    }
    IShellItem* it = nullptr;
    if (SUCCEEDED(d->Show(g_hwnd)) && SUCCEEDED(d->GetResult(&it))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = p; CoTaskMemFree(p); }
        it->Release();
    }
    d->Release();
    return out;
}

void open_folder(const fs::path& p)
{
    std::error_code ec;
    fs::create_directories(p, ec);
    ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::vector<uint8_t> read_file(const fs::path& p)
{
    std::vector<uint8_t> out;
    if (FILE* f = _wfopen(p.c_str(), L"rb")) {
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < (64 << 20)) {
            out.resize((size_t)n);
            if (fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
        }
        fclose(f);
    }
    return out;
}

// The backdrop: PIC1.PNG (the Stanley Cup) from the player's own game files --
// the install first, else the update / disc they picked. Nothing is shipped.
std::vector<uint8_t> find_photo(const fs::path& install, const std::string& game, const std::string& update)
{
    std::error_code ec;
    for (const fs::path& p : { install / "data" / "update" / "PIC1.PNG", install / "game" / "PS3_GAME" / "PIC1.PNG" })
        if (fs::is_regular_file(p, ec)) return read_file(p);
    std::vector<uint8_t> out;
    if (!update.empty()) {
        const fs::path u = widen(update);
        if (fs::is_directory(u, ec)) out = read_file(u / "PIC1.PNG");
        else if (fs::is_regular_file(u, ec)) {
            PkgFile pk;
            if (pk.open(u.wstring()))
                if (const PkgEntry* e = pk.find("PIC1.PNG")) pk.read_all(*e, out);
        }
        if (!out.empty()) return out;
    }
    if (!game.empty()) {
        const fs::path g = widen(game);
        if (fs::is_directory(g, ec)) out = read_file(g / "PS3_GAME" / "PIC1.PNG");
        else if (fs::is_regular_file(g, ec)) {
            IsoImage iso;
            if (iso.open(g.wstring()))
                if (const IsoFile* f = iso.find("PS3_GAME/PIC1.PNG")) iso.read_all(*f, out);
        }
    }
    return out;
}

// ---- app state ---------------------------------------------------------------

enum class Screen { Setup, Installing, Home };
enum Page { kPlay, kGraphics, kMods, kFiles, kHelp, kPages };
const char* const kPageNames[kPages] = { "PLAY", "GRAPHICS", "MODS", "GAME FILES", "HELP" };

// [video] msaa choices (#249): the game's antialiased surfaces get this many samples
struct MsaaChoice { const char* label; const char* value; };
const MsaaChoice kMsaa[] = { { "Off", "0" }, { "2x (PS3)", "2" }, { "4x", "4" }, { "8x", "8" } };
// [video] aniso choices (#252): anisotropic filtering on every texture unit; "1" = off (trilinear, as the PS3)
const MsaaChoice kAniso[] = { { "Off (PS3)", "1" }, { "2x", "2" }, { "4x", "4" }, { "8x", "8" }, { "16x", "16" } };
// [video] render_scale choices (#253): the game's 1280x720 rendered at this scale
const MsaaChoice kScale[] = { { "Native (your screen)", "native" }, { "720p (PS3)", "1" }, { "1.5x (1080p)", "1.5" },
                              { "2x (1440p)", "2" }, { "3x (4K)", "3" },
                              { "480p (weak GPU)", "0.6667" }, { "360p (very weak GPU)", "0.5" } };   // #267
const int kScaleN = (int)(sizeof kScale / sizeof kScale[0]);
// [video] shadow_scale / fx_scale choices (#254)
const MsaaChoice kShadow[] = { { "PS3 (1x)", "1" }, { "High (2x)", "2" }, { "Ultra (4x)", "4" } };
const MsaaChoice kFx[] = { { "PS3 (half / quarter res)", "1" }, { "High (2x)", "2" }, { "Full resolution", "4" } };

template <size_t N>
std::vector<const char*> labels_of(const MsaaChoice (&c)[N])
{
    std::vector<const char*> v;
    for (const MsaaChoice& x : c) v.push_back(x.label);
    return v;
}

struct App {
    fs::path install;
    std::wstring eboot_elf;   // dev override
    Screen screen = Screen::Setup;
    char game[1024] = "", update[1024] = "";
    std::string game_err, game_info, update_err, update_info;
    std::string checked_game = "\x01", checked_update = "\x01";
    InstallProgress* prog = nullptr;
    std::thread worker;
    bool fullscreen = true, vrr = true, hz60 = false;   // hz60: #256
    bool cap30 = false;       // [video] frame_cap = 30 (#267)
    bool edge_native = false; // [video] edge_native = 1 (#293)
    bool no_crowd = false;    // [video] no_crowd = 2 (#295)
    bool ice_bounce = true;   // [video] ice_bounce = 0 turns it off (#298)
    bool skip_intro = false;  // [game] skip_intro = 1 (#297)
    int msaa = 2;            // index into kMsaa (#249)
    int aniso = 4;            // index into kAniso (#252)
    int rscale = 0;           // index into kScale (#253)
    int shadow = 1, fx = 2;   // indices into kShadow / kFx (#254)
    std::string status;       // home-screen message
    bool crashed = false;     // status is about a game crash: offer the report
    std::string last_log;     // err log of the latest run
    bool game_running = false;
    std::vector<ModInfo> mods;
    std::string roster;   // active roster source (#257), "" = your own
    bool mods_dirty = true;
    // "Help" page: the bug-report zip is built on a worker (captures can be large)
    char report_note[4096] = "";
    std::thread report_worker;
    std::atomic<bool> report_busy{ false };
    std::mutex report_m;
    std::string report_msg;
    std::wstring report_zip;
    // Mod installs (drops, Install mod...): one batch at a time on a worker
    std::thread mod_worker;
    std::atomic<bool> mod_busy{ false }, mod_done{ false }, mod_cancel{ false };
    std::atomic<double> mod_frac{ 0 };
    std::mutex mod_m;
    std::string mod_label, mod_msg;   // guarded by mod_m
    bool mod_failed = false;          // guarded by mod_m
    std::string grabbed;              // mod picked up with Y, moved with the d-pad
    // UI
    int page = kPlay;
    float page_t = 1.0f;      // 0 -> 1 after a page switch (fade / slide in)
    bool focus_req = false;   // put the gamepad cursor on the page's first control
    bool show_log = false;    // installing: the raw log
    bool install_seen_done = false;
    ui::Photo photo;
    std::string photo_key = "\x01";

    fs::path ini_path() const { return install / "nhl_settings.ini"; }

    void make_report()
    {
        if (report_busy) return;
        if (report_worker.joinable()) report_worker.join();
        report_busy = true;
        { std::lock_guard<std::mutex> l(report_m); report_msg = "Collecting logs and captures..."; report_zip.clear(); }
        report_worker = std::thread([this, dir = install.wstring(), note = std::string(report_note)] {
            std::wstring zip;
            const std::string err = make_bug_report(dir, note, &zip);
            std::lock_guard<std::mutex> l(report_m);
            report_msg = err.empty() ? "Saved " + narrow(zip) : "Could not make the report: " + err;
            report_zip = err.empty() ? zip : L"";
            report_busy = false;
        });
    }

    void load()
    {
        Ini ini;
        ini.load(ini_path().wstring());
        strncpy(game, ini.get("launcher", "game_source").c_str(), sizeof game - 1);
        strncpy(update, ini.get("launcher", "update_source").c_str(), sizeof update - 1);
        fullscreen = ini.get("video", "fullscreen", "1") != "0";
        vrr = ini.get("video", "vrr", "1") != "0";
        hz60 = ini.get("video", "hz60", "0") == "1";
        cap30 = ini.get("video", "frame_cap", "0") == "30";
        edge_native = ini.get("video", "edge_native", "0") == "1";
        no_crowd = ini.get("video", "no_crowd", "0") != "0";
        ice_bounce = ini.get("video", "ice_bounce", "1") != "0";
        skip_intro = ini.get("game", "skip_intro", "0") == "1";
        { const int s = atoi(ini.get("video", "msaa", "4").c_str());
          msaa = s >= 8 ? 3 : s >= 4 ? 2 : s >= 2 ? 1 : 0; }
        { const int a = atoi(ini.get("video", "aniso", "16").c_str());
          aniso = a >= 16 ? 4 : a >= 8 ? 3 : a >= 4 ? 2 : a >= 2 ? 1 : 0; }
        { const std::string s = ini.get("video", "render_scale", "native");
          rscale = 0;
          for (int i = 0; i < kScaleN; i++) if (s == kScale[i].value) rscale = i; }
        { const int s = atoi(ini.get("video", "shadow_scale", "2").c_str()); shadow = s >= 4 ? 2 : s >= 2 ? 1 : 0; }
        { const int f = atoi(ini.get("video", "fx_scale", "4").c_str()); fx = f >= 4 ? 2 : f >= 2 ? 1 : 0; }
        screen = install_complete(install.wstring()) ? Screen::Home : Screen::Setup;
        mods_dirty = true;
    }

    void save_video()
    {
        Ini ini;
        /* An unreadable file (locked for a moment, or mid-write by another launcher
         * window) used to load as EMPTY and be saved back as just [video] -- losing
         * [paths]/[launcher]/[mods]/[env], so the next start asked for a reinstall
         * (install_test, 2026-10-28). Never overwrite what could not be read. */
        if (!ini.load(ini_path().wstring()) && std::filesystem::exists(ini_path())) {
            status = "Could not read nhl_settings.ini -- video setting not saved, try again.";
            crashed = false;
            return;
        }
        ini.set("video", "fullscreen", fullscreen ? "1" : "0");
        ini.set("video", "vrr", vrr ? "1" : "0");
        ini.set("video", "hz60", hz60 ? "1" : "0");
        ini.set("video", "frame_cap", cap30 ? "30" : "0");
        ini.set("video", "edge_native", edge_native ? "1" : "0");
        ini.set("video", "no_crowd", no_crowd ? "2" : "0");
        ini.set("video", "ice_bounce", ice_bounce ? "1" : "0");
        ini.set("game", "skip_intro", skip_intro ? "1" : "0");
        ini.set("video", "msaa", kMsaa[msaa].value);
        ini.set("video", "aniso", kAniso[aniso].value);
        ini.set("video", "render_scale", kScale[rscale].value);
        ini.set("video", "shadow_scale", kShadow[shadow].value);
        ini.set("video", "fx_scale", kFx[fx].value);
        ini.save(ini_path().wstring(), "; NHL Legacy native settings. Written by NHLLauncher; hand edits are kept.");
    }

    // #267 presets. Low-end = the lightest load on both CPU and GPU.
    void preset_low()
    {
        cap30 = true; hz60 = false; rscale = 1; shadow = 0; fx = 0;
        msaa = 0; aniso = 0; edge_native = true;
    }
    void preset_default()
    {
        cap30 = false; hz60 = false; rscale = 0; shadow = 1; fx = 2;
        msaa = 2; aniso = 4; edge_native = false; no_crowd = false;
    }
    const char* preset_name() const
    {
        if (!cap30 && !hz60 && rscale == 0 && shadow == 1 && fx == 2 && msaa == 2 && aniso == 4 && !edge_native && !no_crowd)
            return "Default quality";
        if (cap30 && !hz60 && rscale == 1 && shadow == 0 && fx == 0 && msaa == 0 && aniso == 0 && edge_native)
            return "Low-end PC";
        return "Custom";
    }

    void import_mods(const std::vector<std::wstring>& srcs)
    {
        if (srcs.empty()) return;
        if (mod_busy) {
            std::lock_guard<std::mutex> l(mod_m);
            mod_msg = "Wait for the mod being installed to finish, then drop the next one.";
            mod_failed = true;
            return;
        }
        if (mod_worker.joinable()) mod_worker.join();
        mod_busy = true;
        mod_frac = 0;
        { std::lock_guard<std::mutex> l(mod_m); mod_msg.clear(); mod_failed = false; }
        mod_worker = std::thread([this, srcs, dir = install.wstring()] {
            std::string msg;
            bool failed = false;
            for (const std::wstring& s : srcs) {
                const std::string name = narrow(fs::path(s).filename().wstring());
                { std::lock_guard<std::mutex> l(mod_m); mod_label = "Installing " + name; }
                mod_frac = 0;
                std::string sum;
                const std::string err = import_mod(dir, s, &mod_frac, &mod_cancel, &sum);
                if (!err.empty()) failed = true;
                msg += (msg.empty() ? "" : "\n") + (err.empty() ? sum : "Could not install " + name + ": " + err);
            }
            { std::lock_guard<std::mutex> l(mod_m); mod_msg = msg; mod_failed = failed; }
            mod_busy = false;
            mod_done = true;
        });
    }

    // Load order: move mod i one place up (-1) or down (+1).
    void move_mod(int i, int d)
    {
        const int j = i + d;
        if (i < 0 || j < 0 || i >= (int)mods.size() || j >= (int)mods.size()) return;
        std::swap(mods[i], mods[j]);
        std::vector<std::string> names;
        for (const ModInfo& m : mods) names.push_back(m.name);
        set_mod_order(install.wstring(), names);
    }

    void refresh_mods()
    {
        if (!mods_dirty) return;
        mods = list_mods(install.wstring());
        roster = active_roster(install.wstring());
        // the roster's mod was deleted: back to your own (the game already falls back to it)
        bool found = roster.empty();
        for (const ModInfo& m : mods) found |= m.roster && _stricmp(m.name.c_str(), roster.c_str()) == 0;
        if (!found) {
            roster.clear();
            set_active_roster(install.wstring(), roster);
        }
        mods_dirty = false;
    }

    void try_photo()
    {
        if (photo.ok()) return;
        const std::string key = std::string(game) + "|" + update + "|" + (game_err.empty() ? "g" : "") +
                                (update_err.empty() ? "u" : "");
        if (key == photo_key) return;
        photo_key = key;
        const std::vector<uint8_t> png = find_photo(install, game_err.empty() ? game : "", update_err.empty() ? update : "");
        ui::make_photo(g_dev, png, &photo);
    }

    void recheck()
    {
        if (checked_game != game) { checked_game = game; game_err = check_game_source(widen(game), &game_info); }
        if (checked_update != update) { checked_update = update; update_err = check_update_source(widen(update), &update_info); }
        try_photo();
    }

    // A file or folder dropped on the setup screen: an ISO / disc folder is the
    // game, a .pkg / update folder the update.
    void take_drop(const std::wstring& w)
    {
        const fs::path p = w;
        std::error_code ec;
        std::wstring ext = p.extension().wstring();
        for (wchar_t& c : ext) c = (wchar_t)towlower(c);
        bool is_game;
        if (ext == L".iso") is_game = true;
        else if (ext == L".pkg") is_game = false;
        else if (fs::is_directory(p / "PS3_GAME", ec)) is_game = true;
        else if (fs::exists(p / "USRDIR" / "EBOOT.BIN", ec) || fs::exists(p / "PARAM.SFO", ec)) is_game = false;
        else is_game = game[0] == 0;
        char* buf = is_game ? game : update;
        const size_t cap = is_game ? sizeof game : sizeof update;
        strncpy(buf, narrow(w).c_str(), cap - 1);
        buf[cap - 1] = 0;
    }

    void start_install()
    {
        InstallConfig cfg;
        cfg.install_dir = install.wstring();
        cfg.game_src = widen(game);
        cfg.update_src = widen(update);
        cfg.eboot_elf = eboot_elf;
        delete prog;
        prog = new InstallProgress();
        if (worker.joinable()) worker.join();
        worker = std::thread([cfg, p = prog] { run_install(cfg, *p); });
        screen = Screen::Installing;
        show_log = false;
        install_seen_done = false;
    }

    void play()
    {
        const fs::path exe = install / "NHLLegacy.exe", logs = install / "logs";
        std::error_code ec;
        crashed = false;
        if (!fs::exists(exe, ec)) { status = "NHLLegacy.exe is missing from " + narrow(install.wstring()); return; }
        if (std::string err = compose_hdd0(install.wstring(), nullptr); !err.empty()) {
            status = "Could not apply mods: " + err;
            return;
        }
        if (std::string err = apply_roster(install.wstring(), nullptr); !err.empty()) {   // #257
            status = "Could not switch rosters: " + err;
            return;
        }
        fs::create_directories(logs, ec);
        // One log pair per run (game_<date>_<time>), so an earlier session's crash
        // is not overwritten by the next PLAY.
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t stamp[32];
        swprintf(stamp, 32, L"game_%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        last_log = narrow((logs / (std::wstring(stamp) + L".err.log")).wstring());
        SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
        HANDLE out = CreateFileW((logs / (std::wstring(stamp) + L".out.log")).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
        HANDLE err = CreateFileW((logs / (std::wstring(stamp) + L".err.log")).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
        STARTUPINFOW si = { sizeof si };
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = out;
        si.hStdError = err;
        PROCESS_INFORMATION pi = {};
        std::wstring cmd = L"\"" + exe.wstring() + L"\"";
        const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                       install.c_str(), &si, &pi);
        CloseHandle(out);
        CloseHandle(err);
        if (!ok) { status = "Could not start the game (error " + std::to_string(GetLastError()) + ")"; return; }
        CloseHandle(pi.hThread);
        game_running = true;
        status.clear();
        ShowWindow(g_hwnd, SW_HIDE);
        std::thread([h = pi.hProcess] {
            WaitForSingleObject(h, INFINITE);
            DWORD code = 0;
            GetExitCodeProcess(h, &code);
            CloseHandle(h);
            PostMessageW(g_hwnd, WM_GAME_EXITED, code, 0);
        }).detach();
    }

    void set_page(int p, bool by_pad)
    {
        p = (p + kPages) % kPages;
        if (p == page) return;
        page = p;
        page_t = 0.0f;
        focus_req = by_pad;
    }

    // The first control of a page takes the gamepad cursor after a page switch.
    void focus_here()
    {
        if (!focus_req) return;
        ImGui::SetKeyboardFocusHere();
        ImGui::SetNavCursorVisible(true);
        focus_req = false;
    }
};

// ---- drawing helpers ---------------------------------------------------------------

ImDrawList* dl() { return ImGui::GetWindowDrawList(); }

void heading(const char* s)
{
    ui::para(ui::F.head_x, 54, ui::kFg, s);
    ui::gap(6);
}

// One line: a check (ok) or warning icon, then wrapped text.
void status_line(const std::string& err, const std::string& info, float w)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool ok = err.empty();
    if (ok) ui::icon_check(dl(), p + px(9, 11), px(9), ui::kGood);
    else ui::icon_warn(dl(), p + px(9, 11), px(9), ui::kWarn);
    ImGui::SetCursorScreenPos(p + px(28, 0));
    ui::para(ui::F.body, 16, ok ? ui::kFg : ui::kWarn, ok ? info.c_str() : err.c_str(), w - px(28));
}

// The info panel right of a settings list: what the focused control does.
void info_panel(ImVec2 a, float w, const char* def_title, const char* def_text)
{
    const bool has = ui::info_title()[0] != 0;
    const char* t = has ? ui::info_title() : def_title;
    const char* s = has ? ui::info_text() : def_text;
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImGui::PushFont(ui::F.head, 0.0f);
    ui::text(d, ui::F.head, px(30), a, ui::kFg, t, px(0.8f));
    ImGui::PopFont();
    d->AddText(ui::F.body, px(17), a + px(0, 44), ui::C(ui::kDim), s, nullptr, w);
}

// Controller hints, right-aligned at the bottom.
void hints(ImVec2 right, std::initializer_list<std::pair<const char*, const char*>> items)
{
    if (!ui::gamepad()) return;
    std::vector<std::pair<const char*, const char*>> v(items);
    float x = right.x;
    for (auto it = v.rbegin(); it != v.rend(); ++it) {
        if (it != v.rbegin() && it->second[0]) x -= px(26);   // a glyph-only item hugs the one after it
        x -= ui::hint(ImGui::GetWindowDrawList(), ImVec2(x, right.y), it->first, it->second);
    }
}

void source_row(App& app, const char* id, char* buf, size_t cap, bool is_game, float w)
{
    ImGui::PushID(id);
    const float bw = px(132), gapw = px(10);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, px(12, 10.5f));
    ImGui::SetNextItemWidth(w - 2 * (bw + gapw));
    ImGui::InputTextWithHint("##path", is_game ? "Path to your ISO or game folder" : "Path to the .pkg or update folder",
                             buf, cap);
    ImGui::PopStyleVar();
    const float h = ImGui::GetItemRectSize().y;
    ImGui::SameLine(0, gapw);
    if (is_game) app.focus_here();
    if (ui::button(is_game ? "ISO file..." : ".pkg file...", ImVec2(bw, h))) {
        std::wstring p = is_game ? pick(false, L"Choose your NHL Legacy Edition ISO", L"Disc images", L"*.iso")
                                 : pick(false, L"Choose the 1.01 update package", L"PS3 packages", L"*.pkg");
        if (!p.empty()) strncpy(buf, narrow(p).c_str(), cap - 1);
    }
    ImGui::SameLine(0, gapw);
    if (ui::button("Folder...", ImVec2(bw, h))) {
        std::wstring p = pick(true, is_game ? L"Choose the folder that contains PS3_GAME"
                                            : L"Choose the BLUS31540 update folder", nullptr, nullptr);
        if (!p.empty()) strncpy(buf, narrow(p).c_str(), cap - 1);
    }
    ImGui::PopID();
}

// Card heading with the step number (a check once the step is done).
void step_title(int n, const char* title, bool done)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 c = p + px(14, 15);
    if (done) ui::icon_check(dl(), c, px(14), ui::kAccent);
    else {
        dl()->AddCircle(c, px(13), ui::C(ui::kDim), 0, px(1.5f));
        char s[4];
        snprintf(s, sizeof s, "%d", n);
        const ImVec2 ts = ui::text_size(ui::F.body_b, px(15), s);
        ui::text(dl(), ui::F.body_b, px(15), ImFloor(c - ts * 0.5f), ui::kFg, s);
    }
    ui::text(dl(), ui::F.head, px(26), p + px(40, 1), ui::kFg, title, px(1.2f));
    ImGui::Dummy(ImVec2(1, px(34)));
}

// ---- screens ---------------------------------------------------------------------------

void draw_setup(App& app, float W)
{
    app.recheck();
    const float cw = std::min(px(860), W - px(144)), inner = cw - px(44);
    heading("SET UP YOUR GAME");
    ui::para(ui::F.body, 17, ui::kDim,
             "Runs from your own copy of the game. Your original files are only read, never changed. "
             "You can also drop an ISO, a .pkg or a folder onto this window.", cw);
    ui::gap(16);

    ui::begin_card(cw);
    step_title(1, "YOUR GAME", app.game_err.empty());
    ui::para(ui::F.body, 16, ui::kDim, "Your own NHL Legacy Edition disc (US, BLUS31540): a decrypted ISO or an extracted folder.", inner);
    ui::gap(4);
    source_row(app, "game", app.game, sizeof app.game, true, inner);
    ui::gap(2);
    status_line(app.game_err, app.game_info, inner);
    ui::end_card();
    ui::gap(14);

    ui::begin_card(cw);
    step_title(2, "UPDATE 1.01", app.update_err.empty());
    ui::para(ui::F.body, 16, ui::kDim,
             "The update package (UP0006-BLUS31540_00-NHL16NTSCPATCH01...pkg), or an installed update folder such as "
             "RPCS3's dev_hdd0\\game\\BLUS31540 (mod files in it move to the Mods page).", inner);
    ui::gap(4);
    source_row(app, "update", app.update, sizeof app.update, false, inner);
    ui::gap(2);
    status_line(app.update_err, app.update_info, inner);
    ui::end_card();
    ui::gap(14);

    const bool ready = app.game_err.empty() && app.update_err.empty();
    ui::begin_card(cw);
    step_title(3, "INSTALL", false);
    const std::string where = "Installs into " + narrow(app.install.wstring());
    ui::para(ui::F.body, 16, ui::kDim, where.c_str(), inner);
    ui::para(ui::F.body, 16, ui::kDim,
             fs::is_regular_file(widen(app.game)) ? "Needs about 10 GB of free space (the ISO is extracted)."
                                                  : "Needs about 3 GB of free space.", inner);
    ui::gap(10);
    ImGui::BeginDisabled(!ready);
    if (ui::button("INSTALL", px(220, 50), ui::Btn::Primary)) app.start_install();
    ImGui::EndDisabled();
    if (install_complete(app.install.wstring())) {
        ImGui::SameLine(0, px(14));
        if (ui::button("Back", px(140, 50))) app.screen = Screen::Home;
    }
    ui::end_card();
    ui::gap(24);
}

// A progress bar with a slow light sweep while it runs.
void bar(float frac, float w, bool running)
{
    const ImVec2 a = ImGui::GetCursorScreenPos(), b = a + ImVec2(w, px(8));
    ImDrawList* d = dl();
    d->AddRectFilled(a, b, ui::C(ui::kOff), px(4));
    const float fx = a.x + w * std::clamp(frac, 0.0f, 1.0f);
    if (fx > a.x + 1) d->AddRectFilled(a, ImVec2(fx, b.y), ui::C(ui::kAccent), px(4));
    if (running && fx > a.x + px(20)) {
        const float t = std::fmod((float)ImGui::GetTime() * 0.6f, 1.0f);
        const float cx = a.x + (fx - a.x) * t, hw = px(60);
        ImGui::PushClipRect(a, ImVec2(fx, b.y), true);
        const ImU32 c0 = IM_COL32(255, 255, 255, 0), c1 = ui::C(IM_COL32(255, 255, 255, 140));
        d->AddRectFilledMultiColor(ImVec2(cx - hw, a.y), ImVec2(cx, b.y), c0, c1, c1, c0);
        d->AddRectFilledMultiColor(ImVec2(cx, a.y), ImVec2(cx + hw, b.y), c1, c0, c0, c1);
        ImGui::PopClipRect();
    }
    ImGui::Dummy(ImVec2(w, px(8)));
}

void draw_installing(App& app, float W)
{
    InstallProgress& p = *app.prog;
    std::string label, log;
    { std::lock_guard<std::mutex> l(p.m); label = p.label; log = p.log; }
    const float cw = std::min(px(860), W - px(144));
    const bool done = p.done, failed = p.failed;
    if (done && !app.install_seen_done) { app.install_seen_done = true; app.focus_req = ui::gamepad(); }
    if (failed) app.show_log = true;
    heading(!done ? "INSTALLING" : failed ? "INSTALL STOPPED" : "INSTALLED");
    char step[64];
    snprintf(step, sizeof step, "STEP %d OF %d", (int)p.step, (int)p.steps);
    const ImVec2 sp = ImGui::GetCursorScreenPos();
    ui::text(dl(), ui::F.head, px(19), sp, ui::kDim, step, px(1.6f));
    ImGui::Dummy(ImVec2(1, px(24)));
    ui::para(ui::F.body_b, 21, ui::kFg, label.c_str(), cw);
    ui::gap(10);
    bar((float)p.frac, cw, !done);
    ui::gap(16);
    ui::para(ui::F.body, 15, ui::kDim, "Overall");
    const float overall = p.steps ? ((float)std::max(0, p.step - 1) + (float)p.frac) / (float)p.steps : 0;
    bar(done && !failed ? 1.0f : overall, cw, !done);
    ui::gap(22);
    app.focus_here();
    if (!done) {
        if (ui::button(p.cancel ? "Cancelling..." : "Cancel", px(170, 48))) p.cancel = true;
    } else if (failed) {
        if (ui::button("Back", px(170, 48), ui::Btn::Primary)) app.screen = Screen::Setup;
    } else {
        if (ui::button("CONTINUE", px(200, 48), ui::Btn::Primary)) { app.load(); app.screen = Screen::Home; app.page = kPlay; }
    }
    ImGui::SameLine(0, px(14));
    if (ui::button(app.show_log ? "Hide details" : "Show details", px(170, 48))) app.show_log = !app.show_log;
    if (app.show_log) {
        ui::gap(14);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 90));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, px(14, 12));
        ImGui::BeginChild("log", ImVec2(cw, std::max(px(160), ImGui::GetContentRegionAvail().y - px(10))),
                          ImGuiChildFlags_AlwaysUseWindowPadding);
        ui::para(ui::F.body, 14, ui::kDim, log.c_str());
        if (!done) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
}

void draw_play(App& app, ImVec2 o, float W, float H)
{
    ImDrawList* d = dl();
    auto at = [&](float x, float y) { return o + px(x, y); };
    ui::text(d, ui::F.head, px(30), at(72, 96), ui::kDim, "NHL", px(3.6f));
    ui::text(d, ui::F.head_x, px(112), at(68, 126), ui::kFg, "LEGACY");
    ui::text(d, ui::F.head_x, px(112), at(68, 222), ui::kFg, "EDITION");

    // Installed - Update 1.01 - mods - roster
    int on = 0;
    for (const ModInfo& m : app.mods) on += m.enabled;
    std::string meta = "  \xC2\xB7  Update 1.01  \xC2\xB7  ";
    meta += on == 0 ? "No mods" : on == 1 ? "1 mod on" : std::to_string(on) + " mods on";
    meta += "  \xC2\xB7  Roster: " + (app.roster.empty() ? std::string("your own") : app.roster);
    ImVec2 mp = at(72, 352);
    ui::text(d, ui::F.body_b, px(17), mp, ui::kFg, "Installed");
    mp.x += ui::text_size(ui::F.body_b, px(17), "Installed").x;
    ui::text(d, ui::F.body, px(17), mp, ui::kDim, meta.c_str());

    ImGui::SetCursorScreenPos(at(72, 404));
    app.focus_here();
    ImGui::BeginDisabled(app.game_running || app.mod_busy);   // a mod install would race PLAY's mod compose
    if (ui::button(app.game_running ? "RUNNING###play" : "PLAY###play", px(310, 80), ui::Btn::Hero)) app.play();
    ImGui::EndDisabled();
    if (app.mod_busy)
        ui::text(d, ui::F.body, px(15), at(400, 434), ui::kDim, "Installing a mod...");
    ImGui::SetItemDefaultFocus();

    std::string sub = std::string(kScale[app.rscale].label) + ", " + kMsaa[app.msaa].label + " MSAA";
    if (app.cap30) sub += ", 30 fps cap";
    else if (app.hz60) sub += ", 60 fps menus";
    ImVec2 gp = at(72, 514);
    ui::text(d, ui::F.body, px(16), gp, ui::kDim, "Graphics:  ");
    gp.x += ui::text_size(ui::F.body, px(16), "Graphics:  ").x;
    ui::text(d, ui::F.body_b, px(16), gp, ui::kFg, app.preset_name());
    gp.x += ui::text_size(ui::F.body_b, px(16), app.preset_name()).x;
    ui::text(d, ui::F.body, px(16), gp, ui::kDim, ("  \xC2\xB7  " + sub).c_str());

    if (!app.status.empty()) {
        ImGui::SetCursorScreenPos(at(72, 560));
        status_line(app.status, "", std::min(px(620), W - px(144)));
        if (app.crashed) {
            ImGui::SetCursorScreenPos(ImVec2(o.x + px(100), ImGui::GetCursorScreenPos().y + px(8)));
            if (ui::button("Report a problem", px(200, 42))) app.set_page(kHelp, ImGui::GetIO().NavVisible);
        }
    }
    ui::text(d, ui::F.body, px(14), ImVec2(o.x + px(72), o.y + H - px(28)), ui::kDim,
             "Native PC port. Runs from your own copy of the game.");
    (void)W;
}

void draw_graphics(App& app, float cw)
{
    heading("GRAPHICS");
    bool changed = false;
    const float tw = (cw - px(14)) * 0.5f;
    app.focus_here();
    if (ui::tile("Default quality", "Your screen's resolution, sharper shadows, 4x MSAA",
                 strcmp(app.preset_name(), "Default quality") == 0, ImVec2(tw, px(84)),
                 "Native resolution, sharper shadows, full-resolution lighting and effects, 4x MSAA and 16x texture "
                 "filtering.")) { app.preset_default(); changed = true; }
    ImGui::SameLine(0, px(14));
    if (ui::tile("Low-end PC", "30 fps, 720p, the lightest load", strcmp(app.preset_name(), "Low-end PC") == 0,
                 ImVec2(tw, px(84)),
                 "30 fps cap, 720p, PS3 shadows and effects, no MSAA or anisotropic filtering, fast player geometry. "
                 "If a weak GPU still struggles, try 480p or 360p under Render resolution.")) { app.preset_low(); changed = true; }

    ui::section("DISPLAY");
    changed |= ui::toggle_row("Fullscreen", &app.fullscreen,
                              "Plays fullscreen. F11 switches between fullscreen and a window in the game.", cw);
    changed |= ui::toggle_row("Variable refresh rate", &app.vrr,
                              "FreeSync / G-SYNC. Turn VRR off on a fixed-refresh screen.", cw);
    changed |= ui::toggle_row("60 fps menus and cutscenes", &app.hz60,
                              "Menus, intros and cutscenes run at 60 instead of 30, and matches feel more responsive. "
                              "Gameplay itself stays at the game's 30 Hz. Best with VRR or a 120 Hz+ screen.", cw);

    ui::section("PERFORMANCE");
    changed |= ui::toggle_row("Cap the frame rate at 30", &app.cap30,
                              "For slow CPUs. The game logic runs at 30 anyway; in matches it draws extra frames in "
                              "between. Capping frees CPU time on 4-core / older CPUs (Steam Deck). Applies when you "
                              "press PLAY.", cw);
    changed |= ui::toggle_row("Fast player geometry", &app.edge_native,
                              "For slow CPUs. Prepares the player models with much faster code, for more speed in "
                              "matches on 4-core / older CPUs (Steam Deck). Applies when you press PLAY.", cw);
    changed |= ui::toggle_row("No crowd", &app.no_crowd,
                              "For slow CPUs. Removes the fans and their seats from the arena for more speed in "
                              "matches, and most of all in the crowd shots before a game. The stands look empty. "
                              "Applies when you press PLAY.", cw);

    ui::section("QUALITY");
    static const std::vector<const char*> scale = labels_of(kScale), shadow = labels_of(kShadow), fx = labels_of(kFx),
                                          msaa = labels_of(kMsaa), aniso = labels_of(kAniso);
    changed |= ui::choice_row("Render resolution", &app.rscale, scale.data(), (int)scale.size(),
                              "Native renders the game at your screen's resolution (the PS3 used 720p). Above native "
                              "looks even smoother but costs more GPU. If a weak GPU struggles, try 480p or 360p. "
                              "Applies when you press PLAY.", cw);
    changed |= ui::choice_row("Shadow quality", &app.shadow, shadow.data(), (int)shadow.size(),
                              "Sharper player and stick shadows (bigger shadow maps).", cw);
    changed |= ui::choice_row("Lighting and effects", &app.fx, fx.data(), (int)fx.size(),
                              "The game computes player lighting, glow and depth of field at half or quarter "
                              "resolution; higher settings render them sharper. Costs GPU time.", cw);
    changed |= ui::choice_row("Anti-aliasing (MSAA)", &app.msaa, msaa.data(), (int)msaa.size(),
                              "Smooths edges in the 3D scene, including the glass and net. The PS3 used 2x. Higher "
                              "costs GPU time; drop it if the game slows down. Applies when you press PLAY.", cw);
    changed |= ui::choice_row("Texture filtering", &app.aniso, aniso.data(), (int)aniso.size(),
                              "Anisotropic filtering keeps the ice, boards and jerseys sharp at a distance and at "
                              "angles. Very cheap on most GPUs; lower it on weak ones (Steam Deck) if needed. Applies "
                              "when you press PLAY.", cw);
    changed |= ui::toggle_row("Ice light on players", &app.ice_bounce,
                              "Light bouncing up from the ice onto the players' jerseys and gear, as on the PS3. Off "
                              "gives darker, flatter jerseys (a matter of taste). Applies when you press PLAY.", cw);

    ui::section("GAME");
    changed |= ui::toggle_row("Skip the intro", &app.skip_intro,
                              "Goes straight to the main menu: no intro video, legal screen, Press Start or autosave "
                              "notice (autosave stays on). From Puck Peak's RPCS3 patch. Applies when you press PLAY.", cw);
    if (changed) app.save_video();
    ui::gap(20);
}

void draw_mods(App& app, float cw)
{
    heading("MODS");
    app.refresh_mods();
    ui::para(ui::F.body, 16, ui::kDim,
             "Drop a mod's .zip, folder or file anywhere on this window to install it. Higher in the list wins when two "
             "mods change the same file. Changes apply when you press PLAY.", cw);
    ui::gap(12);

    ImGui::BeginDisabled(app.mod_busy);
    if (app.mods.empty()) app.focus_here();
    if (ui::button("Install mod...", px(170, 46), ui::Btn::Primary)) {
        const std::wstring p = pick(false, L"Choose a mod: its .zip, or a single game file", L"Zip files", L"*.zip");
        if (!p.empty()) app.import_mods({ p });
    }
    ImGui::SameLine(0, px(12));
    if (ui::button("From a folder...", px(170, 46))) {
        const std::wstring p = pick(true, L"Choose the mod's folder", nullptr, nullptr);
        if (!p.empty()) app.import_mods({ p });
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, px(12));
    if (ui::button("Open mods folder", px(180, 46))) open_folder(app.install / "mods");
    ImGui::SameLine(0, px(12));
    if (ui::button("Refresh", px(110, 46))) app.mods_dirty = true;
    ui::gap(10);

    std::string label, msg;
    bool failed;
    { std::lock_guard<std::mutex> l(app.mod_m); label = app.mod_label; msg = app.mod_msg; failed = app.mod_failed; }
    if (app.mod_busy) {
        ui::begin_card(cw, 18);
        ui::para(ui::F.body_b, 17, ui::kFg, (label + "...").c_str(), cw - px(36));
        ui::gap(8);
        bar((float)app.mod_frac, cw - px(36), true);
        ui::end_card();
        ui::gap(8);
    } else if (!msg.empty()) {
        status_line(failed ? msg : "", msg, cw);
        ui::gap(8);
    }

    if (app.mods.empty()) {
        ui::begin_card(cw);
        ui::para(ui::F.body_b, 19, ui::kFg, "No mods installed");
        ui::gap(4);
        ui::para(ui::F.body, 16, ui::kDim,
                 "Drop a mod here, or use Install mod. A mod is laid out like the update (USRDIR\\..., often inside "
                 "BLUS31540\\) and can bring a roster's SYS-DATA.", cw - px(44));
        ui::end_card();
    }
    int move_i = -1, move_d = 0, drag_i = -1;
    std::vector<float> block_top, block_bottom;   // each mod's row + its roster row (mouse drag targets)
    ImGui::BeginDisabled(app.mod_busy);
    for (int i = 0; i < (int)app.mods.size(); i++) {
        ModInfo& m = app.mods[i];
        char note[32];
        snprintf(note, sizeof note, m.roster ? "%d files  \xC2\xB7  roster" : "%d files", m.files);
        if (i == 0) app.focus_here();
        bool grab = app.grabbed == m.name, dragging = false;
        block_top.push_back(ImGui::GetCursorScreenPos().y);
        const ui::RowAct act = ui::order_row(
            (m.name + "##mod").c_str(), &m.enabled,
            "Turns this mod on or off. Higher in the list wins when two mods change the same file; drag it, use the "
            "arrows, Y and the d-pad, or Alt+Up / Alt+Down. Applies when you press PLAY.",
            cw, note, i + 1, i > 0, i + 1 < (int)app.mods.size(), &grab, &dragging);
        if (dragging) drag_i = i;
        if (grab) app.grabbed = m.name;
        else if (app.grabbed == m.name) app.grabbed.clear();
        if (act == ui::RowAct::Toggle) set_mod_enabled(app.install.wstring(), m.name, m.enabled);
        if (act == ui::RowAct::Up || act == ui::RowAct::Down) { move_i = i; move_d = act == ui::RowAct::Up ? -1 : 1; }
        if (m.roster) {   // #257: one roster at a time; off goes back to your own
            bool use = app.roster == m.name;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(54));
            if (ui::toggle_row(("Use its roster##" + m.name).c_str(), &use,
                               "Play with this mod's roster. Turn it off to go back to yours. Your own saved rosters "
                               "and in-game edits to each roster are kept when you switch.", cw - px(54))) {
                app.roster = use ? m.name : "";
                set_active_roster(app.install.wstring(), app.roster);
            }
        }
        block_bottom.push_back(ImGui::GetCursorScreenPos().y);
    }
    ImGui::EndDisabled();
    // A dragged mod steps one place toward the mouse, but only when the mouse lands
    // inside its block at the new place (blocks differ in height: roster rows), so it
    // never bounces back.
    if (drag_i >= 0 && move_i < 0) {
        const float y = ImGui::GetIO().MousePos.y, top = block_top[drag_i], bot = block_bottom[drag_i];
        const int d = drag_i;
        if (d > 0 && y < top && y < block_top[d - 1] + (bot - top)) { move_i = d; move_d = -1; }
        else if (d + 1 < (int)app.mods.size() && y >= bot && y >= top + (block_bottom[d + 1] - block_top[d + 1])) {
            move_i = d;
            move_d = 1;
        }
    }
    if (move_i >= 0) app.move_mod(move_i, move_d);
    ui::gap(14);
    const std::string r = "Roster in use: " + (app.roster.empty() ? std::string("your own / the game's") : app.roster);
    ui::para(ui::F.body_b, 17, ui::kFg, r.c_str(), cw);
    ui::para(ui::F.body, 15, ui::kDim, "Your own saved rosters and in-game edits to each roster are kept when you switch.", cw);
    ui::gap(20);
}

void kv_row(const char* k, const std::string& v, float w)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ui::text(dl(), ui::F.body, px(15), p + px(0, 2), ui::kDim, k);
    ImGui::SetCursorScreenPos(p + px(130, 0));
    ui::para(ui::F.body_b, 16, ui::kFg, v.empty() ? "-" : v.c_str(), w - px(130));
    ui::gap(6);
}

void draw_files(App& app, float cw)
{
    heading("GAME FILES");
    Ini ini;
    ini.load(app.ini_path().wstring());
    ui::begin_card(cw);
    kv_row("Game", ini.get("launcher", "game_source"), cw - px(44));
    kv_row("Update", ini.get("launcher", "update_source"), cw - px(44));
    kv_row("Installed in", narrow(app.install.wstring()), cw - px(44));
    ui::end_card();
    ui::gap(18);
    app.focus_here();
    if (ui::button("Repair or change files", px(250, 48), ui::Btn::Primary)) app.screen = Screen::Setup;
    ImGui::SameLine(0, px(14));
    if (ui::button("Open install folder", px(210, 48))) open_folder(app.install);
    ImGui::SameLine(0, px(14));
    if (ui::button("Open logs folder", px(190, 48))) open_folder(app.install / "logs");
    ui::gap(16);
    ui::para(ui::F.body, 16, ui::kDim,
             "Repair runs the install again with your files; anything already in place is checked and skipped. "
             "Your original files are only read, never changed.", cw);
}

void draw_help(App& app, float cw)
{
    heading("GET HELP");
    static const char* const steps[] = {
        "In the game, press F9 while the problem is on screen. It saves a capture.",
        "Close the game.",
        "Say what happened below and create the report. It collects the last game logs, your F9 captures, your "
        "settings and basic PC info into one zip to send.",
    };
    for (int i = 0; i < 3; i++) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 c = p + px(14, 12);
        dl()->AddCircle(c, px(13), ui::C(ui::kDim), 0, px(1.5f));
        char s[4];
        snprintf(s, sizeof s, "%d", i + 1);
        const ImVec2 ts = ui::text_size(ui::F.body_b, px(15), s);
        ui::text(dl(), ui::F.body_b, px(15), ImFloor(c - ts * 0.5f), ui::kFg, s);
        ImGui::SetCursorScreenPos(p + px(42, 0));
        ui::para(ui::F.body, 17, ui::kFg, steps[i], cw - px(42));
        ui::gap(8);
    }
    ui::gap(6);
    ui::para(ui::F.body_b, 16, ui::kDim, "What happened? (which mode or screen, what you did just before)", cw);
    ui::gap(2);
    ImGui::PushFont(ui::F.body, 17);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, px(12, 10));
    ImGui::InputTextMultiline("##note", app.report_note, sizeof app.report_note, ImVec2(cw, px(120)));
    ImGui::PopStyleVar();
    ImGui::PopFont();
    ui::gap(12);
    app.focus_here();
    ImGui::BeginDisabled(app.game_running || app.report_busy);
    if (ui::button(app.report_busy ? "Working...###rep" : "Create bug report###rep", px(220, 48), ui::Btn::Primary))
        app.make_report();
    ImGui::EndDisabled();
    std::string msg;
    std::wstring zip;
    { std::lock_guard<std::mutex> l(app.report_m); msg = app.report_msg; zip = app.report_zip; }
    if (!zip.empty()) {
        ImGui::SameLine(0, px(14));
        if (ui::button("Show the file", px(170, 48))) {
            const std::wstring args = L"/select,\"" + zip + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
    }
    ImGui::SameLine(0, px(14));
    if (ui::button("Open logs folder", px(190, 48))) open_folder(app.install / "logs");
    if (!msg.empty()) {
        ui::gap(10);
        ui::para(ui::F.body, 16, ui::kFg, msg.c_str(), cw);
    }
    ui::gap(20);
}

// Brand, page tabs (home) and the LB / RB glyphs.
void draw_topbar(App& app, ImVec2 o, float W)
{
    ImDrawList* d = dl();
    const float cy = o.y + px(42);
    ui::text(d, ui::F.head_x, px(25), ImVec2(o.x + px(44), cy - px(15)), ui::kFg, "NHL LEGACY", px(1.0f));
    float x = o.x + px(44) + ui::text_size(ui::F.head_x, px(25), "NHL LEGACY", px(1.0f)).x + px(44);
    if (app.screen != Screen::Home) {
        ui::text(d, ui::F.head, px(21), ImVec2(x, cy - px(12)), ui::kDim, app.screen == Screen::Setup ? "SETUP" : "INSTALL",
                 px(1.3f));
        return;
    }
    const bool pad = ui::gamepad();
    if (pad) {
        ui::hint(d, ImVec2(x + px(40), cy), "LB", "");
        x += px(56);
    }
    static float ux = -1, uw = 0;
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    for (int i = 0; i < kPages; i++) {
        const ImVec2 ts = ui::text_size(ui::F.head, px(21), kPageNames[i], px(1.3f));
        ImGui::SetCursorScreenPos(ImVec2(x, cy - px(18)));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("tab", ImVec2(ts.x, px(36)))) app.set_page(i, false);
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        const ImU32 col = i == app.page ? ui::kFg : hov ? IM_COL32(200, 210, 220, 255) : ui::kDim;
        ui::text(d, ui::F.head, px(21), ImVec2(x, cy - px(12)), col, kPageNames[i], px(1.3f));
        if (i == app.page) {
            const float k = 1.0f - std::exp(-16.0f * ImGui::GetIO().DeltaTime);
            if (ux < 0) { ux = x - o.x; uw = ts.x; }
            ux += (x - o.x - ux) * k;
            uw += (ts.x - uw) * k;
        }
        x += ts.x + px(34);
    }
    ImGui::PopItemFlag();
    d->AddRectFilled(ImVec2(o.x + ux, cy + px(16)), ImVec2(o.x + ux + uw, cy + px(19)), ui::C(ui::kAccent));
    if (pad) ui::hint(d, ImVec2(x - px(34) + px(16) + px(40), cy), "RB", "");
    (void)W;
}

// Gamepad / keyboard shortcuts that work anywhere on the home screen.
void home_keys(App& app)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Tab)))
        app.set_page(app.page + 1, true);
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Tab)))
        app.set_page(app.page - 1, true);
    if (app.page != kPlay && app.grabbed.empty() &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        app.set_page(kPlay, true);
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) && !app.game_running && !app.mod_busy) app.play();
}

void draw_frame(App& app)
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 o = vp->Pos;
    const float W = vp->Size.x, H = vp->Size.y;
    const float dt = ImGui::GetIO().DeltaTime;

    static float blur = 0.0f;
    const float blur_target = (app.screen == Screen::Home && app.page == kPlay) ? 0.0f : 1.0f;
    blur += (blur_target - blur) * (1.0f - std::exp(-9.0f * dt));
    ui::draw_backdrop(ImGui::GetBackgroundDrawList(), app.photo, o, o + ImVec2(W, H), blur);

    static bool first = true;
    if (first) { app.focus_req = ui::gamepad(); first = false; }

    ImGui::SetNextWindowPos(o);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PushFont(ui::F.body, 18);

    // Drops: the game / update on the setup screen, mods to install afterwards.
    if (!g_drops.empty()) {
        if (app.screen == Screen::Setup)
            for (const std::wstring& w : g_drops) app.take_drop(w);
        else if (app.screen == Screen::Home && !app.game_running) {
            app.import_mods(g_drops);
            app.set_page(kMods, false);
        }
        g_drops.clear();
    }
    if (app.mod_done.exchange(false)) app.mods_dirty = true;
    if (g_activated) { g_activated = false; app.mods_dirty = true; }
    if (app.screen == Screen::Home) {
        home_keys(app);
        app.refresh_mods();
    } else if (app.screen == Screen::Setup && install_complete(app.install.wstring()) && !ImGui::GetIO().WantTextInput &&
               (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        app.screen = Screen::Home;
    }
    draw_topbar(app, o, W);

    // Page body: fades and slides in after a switch.
    app.page_t = std::min(1.0f, app.page_t + dt * 5.0f);
    const float t = 1.0f - (1.0f - app.page_t) * (1.0f - app.page_t);
    const float top = px(104), bottom = px(64);
    const bool side_info = app.screen == Screen::Home && (app.page == kGraphics || app.page == kMods);
    const float cw = side_info ? std::clamp(W - px(72) - px(470), px(480), px(700)) : std::min(px(860), W - px(144));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, t);
    ImGui::SetCursorScreenPos(o + ImVec2(0, top + px(14) * (1.0f - t)));
    char id[16];
    snprintf(id, sizeof id, "page%d", app.screen == Screen::Home ? app.page : 10 + (int)app.screen);
    const bool play = app.screen == Screen::Home && app.page == kPlay;
    ImGui::BeginChild(id, ImVec2(W, H - top - bottom), ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoBackground | (play ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : 0));
    if (play) {
        draw_play(app, ImGui::GetCursorScreenPos(), W, H - top - bottom);
    } else {
        ImGui::Indent(px(72));
        ui::gap(8);
        switch (app.screen) {
        case Screen::Setup: draw_setup(app, W); break;
        case Screen::Installing: draw_installing(app, W); break;
        case Screen::Home:
            switch (app.page) {
            case kGraphics: draw_graphics(app, cw); break;
            case kMods: draw_mods(app, cw); break;
            case kFiles: draw_files(app, cw); break;
            case kHelp: draw_help(app, cw); break;
            }
            break;
        }
    }
    ImGui::EndChild();

    if (side_info) {
        const float x = o.x + px(72) + cw + px(64), w = W - (x - o.x) - px(56);
        if (w > px(200)) {
            if (app.page == kGraphics)
                info_panel(ImVec2(x, o.y + top + px(110)), w, "Graphics",
                           "Changes save right away and apply the next time you press PLAY.");
            else
                info_panel(ImVec2(x, o.y + top + px(110)), w, "Mods",
                           "Your own saved rosters and in-game edits to each roster are kept when you switch.");
        }
    }
    ImGui::PopStyleVar();

    // Controller hints
    const ImVec2 hr(o.x + W - px(44), o.y + H - px(32));
    if (app.screen == Screen::Home) {
        if (app.page == kPlay) hints(hr, { { "A", "Select" }, { "LB", "" }, { "RB", "Pages" }, { "START", "Play" } });
        else if (app.page == kGraphics)
            hints(hr, { { "A", "Select" }, { "LR", "Change" }, { "B", "Back" }, { "LB", "" }, { "RB", "Pages" } });
        else if (app.page == kMods && !app.grabbed.empty()) hints(hr, { { "UD", "Move" }, { "A", "Done" } });
        else if (app.page == kMods)
            hints(hr, { { "A", "Select" }, { "Y", "Move" }, { "B", "Back" }, { "LB", "" }, { "RB", "Pages" } });
        else hints(hr, { { "A", "Select" }, { "B", "Back" }, { "LB", "" }, { "RB", "Pages" } });
    } else if (app.screen == Screen::Setup && install_complete(app.install.wstring())) {
        hints(hr, { { "A", "Select" }, { "B", "Back" } });
    } else {
        hints(hr, { { "A", "Select" } });
    }

    ImGui::PopFont();
    ImGui::End();
}

void apply_style()
{
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowPadding = ImVec2(0, 0);
    s.FramePadding = ImVec2(12, 10);
    s.ItemSpacing = ImVec2(12, 8);
    s.FrameRounding = 4;
    s.FrameBorderSize = 1;
    s.GrabRounding = 4;
    s.ScrollbarSize = 8;
    s.ScrollbarRounding = 4;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.FontSizeBase = 18;
    ImVec4* c = s.Colors;
    auto v = [](ImU32 x) { return ImGui::ColorConvertU32ToFloat4(x); };
    c[ImGuiCol_Text] = v(ui::kFg);
    c[ImGuiCol_TextDisabled] = v(ui::kDim);
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = v(IM_COL32(20, 26, 34, 245));
    c[ImGuiCol_FrameBg] = v(IM_COL32(0, 0, 0, 90));
    c[ImGuiCol_FrameBgHovered] = v(IM_COL32(0, 0, 0, 120));
    c[ImGuiCol_FrameBgActive] = v(IM_COL32(0, 0, 0, 140));
    c[ImGuiCol_Border] = v(IM_COL32(255, 255, 255, 30));
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = v(IM_COL32(255, 255, 255, 40));
    c[ImGuiCol_ScrollbarGrabHovered] = v(IM_COL32(255, 255, 255, 70));
    c[ImGuiCol_ScrollbarGrabActive] = v(IM_COL32(255, 255, 255, 90));
    c[ImGuiCol_TextSelectedBg] = v(IM_COL32(232, 238, 243, 70));
    c[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);   // focus is drawn by the widgets themselves
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    App app;
    {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        app.install = fs::path(exe).parent_path();
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::wstring h_game, h_update;
        bool headless = false, report = false, roster = false, compose = false;
        for (int i = 1; i < argc; i++) {
            if (!wcscmp(argv[i], L"--headless-install")) headless = true;
            else if (!wcscmp(argv[i], L"--make-report")) report = true;
            else if (!wcscmp(argv[i], L"--apply-roster")) roster = true;
            else if (!wcscmp(argv[i], L"--compose")) compose = true;
            else if (i + 1 >= argc) break;
            else if (!wcscmp(argv[i], L"--install")) app.install = fs::absolute(argv[++i]);
            else if (!wcscmp(argv[i], L"--eboot-elf")) app.eboot_elf = fs::absolute(argv[++i]).wstring();
            else if (!wcscmp(argv[i], L"--game")) h_game = argv[++i];
            else if (!wcscmp(argv[i], L"--update")) h_update = argv[++i];
            else if (!wcscmp(argv[i], L"--import-mod")) {
                // what dropping a mod does, without a window; the result line goes to
                // <install>\logs\mod_import.log, exit code 0 = installed
                std::string sum;
                const std::string err = import_mod(app.install.wstring(), fs::absolute(argv[++i]).wstring(), nullptr, nullptr, &sum);
                std::error_code ec;
                fs::create_directories(app.install / "logs", ec);
                if (FILE* f = _wfopen((app.install / "logs" / "mod_import.log").c_str(), L"wb")) {
                    fputs(err.empty() ? sum.c_str() : err.c_str(), f);
                    fclose(f);
                }
                return err.empty() ? 0 : 1;
            }
            else if (!wcscmp(argv[i], L"--page")) app.page = std::clamp(_wtoi(argv[++i]), 0, kPages - 1);
        }
        LocalFree(argv);
        // --make-report: the "Create bug report" zip without a window; the result line
        // goes to <install>\logs\report.log, exit code 0 = zip written.
        if (report) {
            std::wstring zip;
            const std::string err = make_bug_report(app.install.wstring(), "(made with --make-report)", &zip);
            if (FILE* f = _wfopen((app.install / "logs" / "report.log").c_str(), L"wb")) {
                fputs(err.empty() ? narrow(zip).c_str() : err.c_str(), f);
                fclose(f);
            }
            return err.empty() ? 0 : 1;
        }
        // --apply-roster: what PLAY does to the roster saves (#257), without a window;
        // the result line goes to <install>\logs\roster.log, exit code 0 = done.
        if (roster) {
            std::string summary;
            const std::string err = apply_roster(app.install.wstring(), &summary);
            std::error_code ec;
            fs::create_directories(app.install / "logs", ec);
            if (FILE* f = _wfopen((app.install / "logs" / "roster.log").c_str(), L"wb")) {
                fputs(err.empty() ? (summary.empty() ? "unchanged" : summary.c_str()) : err.c_str(), f);
                fclose(f);
            }
            return err.empty() ? 0 : 1;
        }
        // --compose: what PLAY does to data\hdd0 (update + enabled mods), without a
        // window; the result line goes to <install>\logs\compose.log, exit code 0 = done.
        if (compose) {
            std::string summary;
            const std::string err = compose_hdd0(app.install.wstring(), &summary);
            std::error_code ec;
            fs::create_directories(app.install / "logs", ec);
            if (FILE* f = _wfopen((app.install / "logs" / "compose.log").c_str(), L"wb")) {
                fputs(err.empty() ? summary.c_str() : err.c_str(), f);
                fclose(f);
            }
            return err.empty() ? 0 : 1;
        }
        // --headless-install --game <iso|folder> --update <folder>: no window; the
        // log goes to <install>\logs\install.log, exit code 0 = installed.
        if (headless) {
            InstallConfig cfg{ app.install.wstring(), h_game, h_update, app.eboot_elf };
            InstallProgress p;
            run_install(cfg, p);
            std::error_code ec;
            fs::create_directories(app.install / "logs", ec);
            if (FILE* f = _wfopen((app.install / "logs" / "install.log").c_str(), L"wb")) {
                fputs(p.log.c_str(), f);
                fclose(f);
            }
            return p.failed ? 1 : 0;
        }
    }
    app.load();

    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    WNDCLASSEXW wc = { sizeof wc, CS_CLASSDC, wnd_proc, 0, 0, inst, LoadIconW(nullptr, IDI_APPLICATION),
                       LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"NHLLauncher", nullptr };
    RegisterClassExW(&wc);
    // 1280x800 (the layout's design size) at the screen's DPI, shrunk to fit the work area.
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    float cw = 1280 * scale, ch = 800 * scale;
    const float fit = std::min({ 1.0f, (wa.right - wa.left) * 0.92f / cw, (wa.bottom - wa.top) * 0.88f / ch });
    RECT r = { 0, 0, (LONG)(cw * fit), (LONG)(ch * fit) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    g_hwnd = CreateWindowW(wc.lpszClassName, L"NHL Legacy Edition", WS_OVERLAPPEDWINDOW,
                           wa.left + std::max(0, (int)(wa.right - wa.left - ww) / 2),
                           wa.top + std::max(0, (int)(wa.bottom - wa.top - wh) / 2), ww, wh, nullptr, nullptr, inst, nullptr);
    if (!create_device(g_hwnd)) {
        MessageBoxW(nullptr, L"Direct3D 11 is not available.", L"NHL Legacy Edition", MB_ICONERROR);
        return 1;
    }
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    DragAcceptFiles(g_hwnd, TRUE);
    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigDebugHighlightIdConflicts = false;   // a programmer warning; players must never see it
    ImGui::GetCurrentContext()->ConfigNavWindowingKeyNext = 0;   // Ctrl+Tab switches pages instead
    ImGui::GetCurrentContext()->ConfigNavWindowingKeyPrev = 0;
    apply_style();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    ui::load_fonts();
    app.try_photo();

    for (bool done = false; !done;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_GAME_EXITED) {
                app.game_running = false;
                if (msg.wParam != 0) {
                    app.status = "The game closed with code " + std::to_string((long long)(int)msg.wParam) +
                                 ". Report a problem packs up the logs to send.";
                    app.crashed = true;
                }
                ShowWindow(g_hwnd, SW_SHOW);
                SetForegroundWindow(g_hwnd);
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (!IsWindowVisible(g_hwnd) || IsIconic(g_hwnd)) { Sleep(50); continue; }
        if (g_resize_w && g_resize_h) {
            g_rtv->Release();
            g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_w = g_resize_h = 0;
            create_rtv();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ui::begin_frame(io.DisplaySize.x, io.DisplaySize.y);
        ImGui::NewFrame();
        draw_frame(app);
        ImGui::Render();
        const float clear[4] = { 11 / 255.0f, 15 / 255.0f, 20 / 255.0f, 1 };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    if (app.prog) app.prog->cancel = true;
    if (app.worker.joinable()) app.worker.join();
    if (app.report_worker.joinable()) app.report_worker.join();
    app.mod_cancel = true;
    if (app.mod_worker.joinable()) app.mod_worker.join();
    ui::free_photo(&app.photo);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_rtv->Release();
    g_swap->Release();
    g_ctx->Release();
    g_dev->Release();
    DestroyWindow(g_hwnd);
    CoUninitialize();
    return 0;
}
