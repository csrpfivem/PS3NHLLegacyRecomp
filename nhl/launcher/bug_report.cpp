// Bug-report zip (see bug_report.h). Layout inside the zip:
//   report.txt                  the player's note, date, PC (CPU, GPU, Windows), enabled mods, exe info
//   nhl_settings.ini
//   logs\game_<ts>.err/out.log  the last two sessions (the newest is the one that went wrong)
//   captures\frame_NN\...       F9 captures from the newest session: manifest + backbuffer for
//                               all of them, every render target only for the newest few;
//                               images as JPEG (a raw-BMP report was 54 MB, too big to send)
#include "bug_report.h"

#include <windows.h>
#include <dxgi.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <vector>

#include "installer.h"
#include "miniz.h"

namespace fs = std::filesystem;

namespace {

constexpr int kLogSessions = 2;      // newest session + the one before it
constexpr int kFullCaptures = 2;     // newest frames that keep every rt_*.bmp (F9 saves 4 near-identical frames)
constexpr int kMaxCaptures = 64;

// BMP file -> JPEG bytes through the Windows Imaging Component. Empty on failure.
std::string bmp_to_jpeg(const fs::path& src)
{
    std::string out;
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return out;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    IStream* mem = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fe = nullptr;
    IPropertyBag2* props = nullptr;
    UINT w = 0, h = 0;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    bool ok = SUCCEEDED(f->CreateDecoderFromFilename(src.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) &&
              SUCCEEDED(f->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &mem)) &&
              SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(mem, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&fe, &props));
    if (ok) {
        PROPBAG2 opt = {};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.92f;
        props->Write(1, &opt, &v);
        ok = SUCCEEDED(fe->Initialize(props)) && SUCCEEDED(fe->SetSize(w, h)) && SUCCEEDED(fe->SetPixelFormat(&fmt)) &&
             IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR) && SUCCEEDED(fe->WriteSource(conv, nullptr)) &&
             SUCCEEDED(fe->Commit()) && SUCCEEDED(enc->Commit());
    }
    if (ok) {
        STATSTG s;
        LARGE_INTEGER zero = {};
        ULONG got = 0;
        if (SUCCEEDED(mem->Stat(&s, STATFLAG_NONAME)) && SUCCEEDED(mem->Seek(zero, STREAM_SEEK_SET, nullptr))) {
            out.resize((size_t)s.cbSize.QuadPart);
            if (FAILED(mem->Read(out.data(), (ULONG)out.size(), &got)) || got != out.size()) out.clear();
        }
    }
    for (IUnknown* u : std::initializer_list<IUnknown*>{ props, fe, enc, mem, conv, frame, dec, f })
        if (u) u->Release();
    return out;
}

ULONGLONG file_time(const fs::path& p, bool creation)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return 0;
    const FILETIME& t = creation ? a.ftCreationTime : a.ftLastWriteTime;
    return ((ULONGLONG)t.dwHighDateTime << 32) | t.dwLowDateTime;
}

std::string reg_string(HKEY root, const wchar_t* key, const wchar_t* value)
{
    wchar_t buf[256];
    DWORD n = sizeof buf;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &n) != ERROR_SUCCESS) return "?";
    return narrow(buf);
}

std::string gpu_names()
{
    std::string out;
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return "?";
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            char line[320];
            snprintf(line, sizeof line, "%s%s (%llu MB)", out.empty() ? "" : "; ", narrow(d.Description).c_str(),
                     (unsigned long long)(d.DedicatedVideoMemory >> 20));
            out += line;
        }
        a->Release();
    }
    f->Release();
    return out.empty() ? "?" : out;
}

std::string windows_version()
{
    std::string name = reg_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName");
    const std::string build = reg_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuild");
    // Windows 11 still says "Windows 10" in ProductName; build 22000+ is 11
    if (atoi(build.c_str()) >= 22000 && name.rfind("Windows 10", 0) == 0) name.replace(8, 2, "11");
    return name + " (build " + build + ")";
}

std::string exe_info(const fs::path& p)
{
    std::error_code ec;
    if (!fs::exists(p, ec)) return "missing";
    SYSTEMTIME st;
    const ULONGLONG t = file_time(p, false);
    FILETIME ft = { (DWORD)t, (DWORD)(t >> 32) }, lt;
    FileTimeToLocalFileTime(&ft, &lt);
    FileTimeToSystemTime(&lt, &st);
    char b[96];
    snprintf(b, sizeof b, "%llu bytes, built %04u-%02u-%02u %02u:%02u", (unsigned long long)fs::file_size(p, ec),
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return b;
}

struct Zip {
    mz_zip_archive z = {};
    FILE* f = nullptr;
    std::string err;

    bool open(const fs::path& p)
    {
        f = _wfopen(p.c_str(), L"wb");
        if (!f) { err = "cannot create " + narrow(p.wstring()); return false; }
        if (!mz_zip_writer_init_cfile(&z, f, MZ_ZIP_FLAG_WRITE_ZIP64)) { err = "zip init failed"; return false; }
        return true;
    }
    void add_text(const std::string& name, const std::string& s, mz_uint level = MZ_DEFAULT_LEVEL)
    {
        if (err.empty() && !mz_zip_writer_add_mem(&z, name.c_str(), s.data(), s.size(), level)) err = "zip: cannot add " + name;
    }
    // Missing or locked files are skipped, not fatal: a report with what could be read beats none.
    void add_file(const std::string& name, const fs::path& src, mz_uint level)
    {
        if (!err.empty()) return;
        FILE* in = _wfopen(src.c_str(), L"rb");
        if (!in) return;
        _fseeki64(in, 0, SEEK_END);
        const long long n = _ftelli64(in);
        _fseeki64(in, 0, SEEK_SET);
        if (n > 0 && !mz_zip_writer_add_cfile(&z, name.c_str(), in, (mz_uint64)n, nullptr, nullptr, 0, level, nullptr, 0, nullptr, 0))
            err = "zip: cannot add " + name;
        fclose(in);
    }
    bool close()
    {
        const bool ok = err.empty() && mz_zip_writer_finalize_archive(&z);
        mz_zip_writer_end(&z);
        if (f) fclose(f);
        f = nullptr;
        if (!ok && err.empty()) err = "zip finalize failed";
        return ok;
    }
};

}  // namespace

std::string make_bug_report(const std::wstring& install_dir, const std::string& note, std::wstring* zip_out)
{
    // WIC (bmp_to_jpeg) needs COM on this thread; the launcher calls this from a worker
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComExit { HRESULT hr; ~ComExit() { if (SUCCEEDED(hr)) CoUninitialize(); } } com_exit{ com };
    const fs::path install = install_dir, logs = install / "logs", caps = logs / "captures";
    std::error_code ec;
    fs::create_directories(logs, ec);

    // game sessions, newest first
    std::vector<std::pair<ULONGLONG, fs::path>> sessions;
    for (const auto& e : fs::directory_iterator(logs, ec)) {
        const std::wstring n = e.path().filename().wstring();
        if (n.rfind(L"game_", 0) == 0 && n.size() > 8 && n.substr(n.size() - 8) == L".err.log")
            sessions.push_back({ file_time(e.path(), true), e.path() });
    }
    std::sort(sessions.begin(), sessions.end(), [](auto& a, auto& b) { return a.first > b.first; });
    if (sessions.empty()) return "No game logs yet -- play once, then make the report.";

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[40];
    swprintf(stamp, 40, L"bug_report_%04u%02u%02u_%02u%02u%02u.zip", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const fs::path zip_path = logs / stamp;

    Zip zip;
    if (!zip.open(zip_path)) return zip.err;

    // report.txt
    {
        std::string r = "NHL Legacy native port -- bug report\n";
        char b[64];
        snprintf(b, sizeof b, "%04u-%02u-%02u %02u:%02u\n\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
        r += b;
        r += "What happened (player's words):\n" + (note.empty() ? std::string("(nothing written)") : note) + "\n\n";
        r += "Session: " + narrow(sessions[0].second.filename().wstring()) + "\n";
        r += "CPU: " + reg_string(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString") + "\n";
        r += "GPU: " + gpu_names() + "\n";
        r += "Windows: " + windows_version() + "\n";
        MEMORYSTATUSEX ms = { sizeof ms };
        if (GlobalMemoryStatusEx(&ms)) { snprintf(b, sizeof b, "RAM: %llu MB\n", (unsigned long long)(ms.ullTotalPhys >> 20)); r += b; }
        r += "NHLLegacy.exe: " + exe_info(install / "NHLLegacy.exe") + "\n";
        r += "Mods:";
        int nm = 0;
        for (const ModInfo& m : list_mods(install_dir))
            if (m.enabled) { r += (nm++ ? ", " : " ") + m.name; }
        r += nm ? "\n" : " none\n";
        zip.add_text("report.txt", r);
    }
    zip.add_file("nhl_settings.ini", install / "nhl_settings.ini", MZ_DEFAULT_LEVEL);

    for (int i = 0; i < (int)sessions.size() && i < kLogSessions; i++) {
        const fs::path err = sessions[i].second;
        std::wstring base = err.filename().wstring();
        base = base.substr(0, base.size() - 8);   // drop ".err.log"
        zip.add_file("logs/" + narrow(base) + ".err.log", err, MZ_DEFAULT_LEVEL);
        zip.add_file("logs/" + narrow(base) + ".out.log", logs / (base + L".out.log"), MZ_DEFAULT_LEVEL);
    }

    // F9 captures written during the newest session (frame numbers restart each session,
    // so the folder's time tells which session it belongs to)
    std::vector<std::pair<ULONGLONG, fs::path>> frames;
    const ULONGLONG since = sessions[0].first;
    for (const auto& e : fs::directory_iterator(caps, ec))
        if (e.is_directory(ec) && e.path().filename().wstring().rfind(L"frame_", 0) == 0) {
            const ULONGLONG t = file_time(e.path() / L"manifest.txt", false);
            if (t >= since) frames.push_back({ t, e.path() });
        }
    std::sort(frames.begin(), frames.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (int i = 0; i < (int)frames.size() && i < kMaxCaptures; i++) {
        const std::string dir = "captures/" + narrow(frames[i].second.filename().wstring()) + "/";
        for (const auto& f : fs::directory_iterator(frames[i].second, ec)) {
            const std::wstring n = f.path().filename().wstring();
            const bool core = n == L"manifest.txt" || n == L"backbuffer.bmp";
            if (!core && i >= kFullCaptures) continue;
            const bool bmp = n.size() > 4 && n.substr(n.size() - 4) == L".bmp";
            const std::string jpg = bmp ? bmp_to_jpeg(f.path()) : std::string();
            if (!jpg.empty())   // already compressed: store
                zip.add_text(dir + narrow(n.substr(0, n.size() - 4)) + ".jpg", jpg, MZ_NO_COMPRESSION);
            else
                zip.add_file(dir + narrow(n), f.path(), MZ_DEFAULT_LEVEL);
        }
    }

    if (!zip.close()) {
        fs::remove(zip_path, ec);
        return zip.err;
    }
    if (zip_out) *zip_out = zip_path.wstring();
    return "";
}
