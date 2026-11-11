// #318: your own songs in Customize EA Sports Trax (see trax.h).
#include "trax.h"

#include <windows.h>
#include <shobjidl.h>   // before propkey.h: brings PROPERTYKEY under WIN32_LEAN_AND_MEAN
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>

#include "ini.h"
#include "installer.h"
#include "miniz.h"

#pragma comment(lib, "propsys.lib")

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kSlotTable = 0xFA6, kSlots = 239;
constexpr int kFirstRecord = 41;   // MBX records 41..47: MenuTunes without song details
constexpr int kFirstIndex = 10;    // after the game's 10 Trax songs

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

bool read_file(const fs::path& p, std::vector<uint8_t>& out)
{
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    const long long n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    out.resize((size_t)std::max(0LL, n));
    const bool ok = n >= 0 && fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

bool write_file(const fs::path& p, const void* d, size_t n)
{
    const fs::path tmp = p.wstring() + L".part";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    const bool ok = fwrite(d, 1, n, f) == n;
    if (fclose(f) != 0 || !ok) { _wremove(tmp.c_str()); return false; }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    return !ec;
}

uint64_t fnv(uint64_t h, const void* d, size_t n)
{
    const uint8_t* b = (const uint8_t*)d;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}
uint64_t fnv(uint64_t h, const std::string& s) { return fnv(h, s.data(), s.size() + 1); }

std::string file_key(const fs::path& p)
{
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec).time_since_epoch().count();
    return narrow(p.wstring()) + "|" + std::to_string(fs::file_size(p, ec)) + "|" + std::to_string((long long)t);
}

bool is_song(const fs::path& p)
{
    std::wstring e = p.extension().wstring();
    for (auto& c : e) c = (wchar_t)towlower(c);
    return e == L".mp3" || e == L".m4a" || e == L".aac" || e == L".mp4" || e == L".wav" || e == L".wma" ||
           e == L".flac" || e == L".ogg";
}

// chunkzip v2: 'chunkzip' u32 ver, u32 raw size, u32 chunk, u32 chunks, u32 align;
// chunks from 0x28, each {u32 csize, u32 type (1 = raw deflate)} + data, the next
// header 8 bytes before an aligned data start.
bool unchunkzip(const std::vector<uint8_t>& in, std::vector<uint8_t>& out)
{
    if (in.size() < 0x28 || memcmp(in.data(), "chunkzip", 8) != 0) { out = in; return true; }
    const uint32_t usize = be32(&in[12]), n = be32(&in[20]), align = std::max(1u, be32(&in[24]));
    out.assign(usize, 0);
    size_t pos = 0x28, o = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (pos + 8 > in.size()) return false;
        const uint32_t cs = be32(&in[pos]), type = be32(&in[pos + 4]);
        pos += 8;
        if (pos + cs > in.size()) return false;
        if (type == 1) {
            const size_t got = tinfl_decompress_mem_to_mem(out.data() + o, usize - o, &in[pos], cs, 0);
            if (got == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) return false;
            o += got;
        } else {
            if (o + cs > usize) return false;
            memcpy(out.data() + o, &in[pos], cs);
            o += cs;
        }
        pos += cs;
        pos = (pos + 8 + align - 1) / align * align - 8;
    }
    return o == usize;
}

// EB v3 archive entry by file name (installer.cpp eb_vp6_entries has the layout).
// Names are {u16 dir index, name}; the dir table (count u16 @0x16, entry length u8
// @0x15) follows the names, 16-aligned. "dir/name" matches the full path.
bool eb_find(const fs::path& big, const char* name, uint64_t& off, uint32_t& size)
{
    FILE* f = _wfopen(big.c_str(), L"rb");
    if (!f) return false;
    std::unique_ptr<FILE, int (*)(FILE*)> guard(f, fclose);
    uint8_t hdr[0x30];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || hdr[0] != 'E' || hdr[1] != 'B') return false;
    const uint32_t cnt = be32(hdr + 4), tbl = be32(hdr + 12), nlen = hdr[20], dlen = hdr[21], ndir = be16(hdr + 22);
    if (cnt > 1000000 || tbl > (256u << 20) || nlen < 2) return false;
    const char* slash = strrchr(name, '/');
    const size_t dirs_at = ((size_t)tbl + (size_t)cnt * nlen + 15) & ~(size_t)15;
    std::vector<uint8_t> meta(slash ? dirs_at + (size_t)ndir * dlen : (size_t)tbl + (size_t)cnt * nlen);
    _fseeki64(f, 0, SEEK_SET);
    if (fread(meta.data(), 1, meta.size(), f) != meta.size()) return false;
    for (uint32_t i = 0; i < cnt; i++) {
        const char* raw = (const char*)meta.data() + tbl + (size_t)i * nlen;
        if (slash) {
            const uint32_t di = be16((const uint8_t*)raw);
            if (di >= ndir || _stricmp(std::string(raw + 2, strnlen(raw + 2, nlen - 2)).c_str(), slash + 1) != 0) continue;
            const char* d = (const char*)meta.data() + dirs_at + (size_t)di * dlen;
            if (_stricmp(std::string(d, strnlen(d, dlen)).c_str(), std::string(name, slash).c_str()) != 0) continue;
        } else {
            size_t k = 1;
            while (k < nlen && (unsigned char)raw[k] < 32 && raw[k]) k++;
            while (k < nlen && (unsigned char)raw[k] < 32) k++;   // prefix bytes
            std::string n(raw + k, strnlen(raw + k, nlen - k));
            if (_stricmp(n.c_str(), name) != 0) continue;
        }
        const uint8_t* e = meta.data() + 0x30 + (size_t)i * 16;
        off = (uint64_t)be32(e) * 16;
        size = be32(e + 8);
        return true;
    }
    return false;
}

bool eb_read(const fs::path& big, const char* name, std::vector<uint8_t>& out)
{
    uint64_t off;
    uint32_t size;
    if (!eb_find(big, name, off, size)) return false;
    FILE* f = _wfopen(big.c_str(), L"rb");
    if (!f) return false;
    std::vector<uint8_t> raw(size);
    _fseeki64(f, (long long)off, SEEK_SET);
    const bool ok = fread(raw.data(), 1, size, f) == size;
    fclose(f);
    return ok && unchunkzip(raw, out);
}

struct Bank {
    fs::path sbs;              // file holding the sbs ...
    uint64_t sbs_off = 0, sbs_size = 0;   // ... at this offset (inside nocache.big for the game's own)
    std::vector<uint8_t> sbr, mbx;
    std::vector<uint8_t> grf;  // music_graffitidata.bin (the music decision lists)
    std::string from, key;
};

fs::path ini_path(const fs::path& inst, const Ini& ini, const char* key, const char* def)
{
    fs::path p = fs::path(widen(ini.get("paths", key, def)));
    return p.is_absolute() ? p : inst / p;
}

std::string load_bank(const fs::path& inst, Bank& b)
{
    std::error_code ec;
    Ini ini;
    ini.load((inst / "nhl_settings.ini").wstring());
    const fs::path usr = ini_path(inst, ini, "disc", "game") / "PS3_GAME" / "USRDIR";
    // The decision lists: a mod's own copy beside its bank, else the game's.
    auto load_grf = [&](const fs::path& d) {
        if (!d.empty() && read_file(d / "music_graffitidata.bin", b.grf)) {
            std::vector<uint8_t> raw = std::move(b.grf);
            if (unchunkzip(raw, b.grf)) return std::string("|") + file_key(d / "music_graffitidata.bin");
        }
        b.grf.clear();
        eb_read(usr / "audioboot.big", "music_graffitidata.bin", b.grf);
        return std::string("|grf");
    };
    for (const ModInfo& m : list_mods(inst.wstring())) {   // load order: the top one wins
        if (!m.enabled) continue;
        const fs::path d = inst / "mods" / fs::path(widen(m.name)) / "USRDIR" / "audio" / "music";
        const fs::path sbs = d / "music_samples.sbs", sbr = d / "music_samples.sbr", mbx = d / "music_musicboxdata.bin";
        if (!fs::exists(sbs, ec) || !fs::exists(sbr, ec) || !fs::exists(mbx, ec)) continue;
        if (!read_file(sbr, b.sbr) || !read_file(mbx, b.mbx)) return "cannot read the music files of mod '" + m.name + "'";
        b.sbs = sbs;
        b.sbs_size = fs::file_size(sbs, ec);
        b.from = "mod '" + m.name + "'";
        b.key = file_key(sbs) + file_key(sbr) + file_key(mbx) + load_grf(d);
        return "";
    }
    b.key = load_grf({});
    uint32_t size = 0;
    if (!eb_find(usr / "nocache.big", "music_samples.sbs", b.sbs_off, size))
        return "the game's music bank was not found in " + narrow((usr / "nocache.big").wstring());
    b.sbs = usr / "nocache.big";
    b.sbs_size = size;
    if (!eb_read(usr / "audioboot.big", "music_samples.sbr", b.sbr) ||
        !eb_read(usr / "audioboot.big", "music_musicboxdata.bin", b.mbx))
        return "the game's music index was not found in audioboot.big";
    b.from = "the game's own soundtrack";
    b.key += file_key(usr / "nocache.big") + file_key(usr / "audioboot.big");
    return "";
}

std::string song_meta(const fs::path& p, std::string& artist, std::string& title)
{
    // Tags first (Windows' property handlers cover mp3/m4a/flac/wma/wav), then
    // "Artist - Title" in the file name, then the plain file name.
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(SHGetPropertyStoreFromParsingName(p.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&ps)))) {
        auto get = [&](const PROPERTYKEY& k) {
            PROPVARIANT v;
            PropVariantInit(&v);
            std::string s;
            wchar_t buf[512];
            if (SUCCEEDED(ps->GetValue(k, &v)) && v.vt != VT_EMPTY && SUCCEEDED(PropVariantToString(v, buf, 512)))
                s = narrow(buf);
            PropVariantClear(&v);
            return s;
        };
        artist = get(PKEY_Music_Artist);
        title = get(PKEY_Title);
        ps->Release();
    }
    if (artist.empty() || title.empty()) {
        const std::string stem = narrow(p.stem().wstring());
        const size_t d = stem.find(" - ");
        if (d != std::string::npos) {
            if (artist.empty()) artist = stem.substr(0, d);
            if (title.empty()) title = stem.substr(d + 3);
        } else if (title.empty()) {
            title = stem;
        }
    }
    auto cap = [](std::string& s) {
        // Curly quotes / dashes / ellipsis -> ASCII: the game's menu font may not have them.
        static const std::pair<const char*, const char*> kAscii[] = {
            {"\xE2\x80\x98", "'"}, {"\xE2\x80\x99", "'"}, {"\xE2\x80\x9C", "\""}, {"\xE2\x80\x9D", "\""},
            {"\xE2\x80\x93", "-"}, {"\xE2\x80\x94", "-"}, {"\xE2\x80\xA6", "..."}};
        for (auto& [from, to] : kAscii)
            for (size_t p; (p = s.find(from)) != std::string::npos;) s.replace(p, strlen(from), to);
        for (char& c : s) if (c == '\r' || c == '\n') c = ' ';
        if (s.size() > 120) {   // cut on a UTF-8 character boundary
            size_t n = 120;
            while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) --n;
            s.resize(n);
        }
    };
    cap(artist);
    cap(title);
    return "";
}

int run(const std::wstring& exe, const std::wstring& args, const fs::path& cwd)
{
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi))
        return -1;
    DWORD code = (DWORD)-2;
    if (WaitForSingleObject(pi.hProcess, 300000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

// run() with stderr into a file (ffmpeg prints its analysis there).
int run_log(const std::wstring& exe, const std::wstring& args, const fs::path& cwd, const fs::path& log)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
    HANDLE h = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = h;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi)) {
        CloseHandle(h);
        return -1;
    }
    DWORD code = (DWORD)-2;
    if (WaitForSingleObject(pi.hProcess, 300000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(h);
    return (int)code;
}

// Quiet songs are raised to this loudness (the game's own Trax measure about -10..-12
// LUFS; the user found Surf Curse - Freaks at -13.6 too quiet, 11-11). Louder songs
// are left alone.
constexpr double kMinLufs = -9.0;

// Integrated loudness (LUFS) of the song, or 0 when it cannot be measured.
double song_lufs(const fs::path& ffmpeg, const fs::path& song, const fs::path& tmp)
{
    const fs::path log = tmp / "loudness.txt";
    if (run_log(ffmpeg.wstring(), L"-hide_banner -nostats -i \"" + song.wstring() + L"\" -vn -af ebur128 -f null -", tmp, log) != 0)
        return 0;
    std::vector<uint8_t> t;
    if (!read_file(log, t)) return 0;
    const std::string s(t.begin(), t.end());
    const size_t sum = s.rfind("Summary:");
    if (sum == std::string::npos) return 0;
    const size_t i = s.find("I:", sum);
    return i == std::string::npos ? 0 : atof(s.c_str() + i + 2);
}

// song -> MP3 (Windows' encoder via ffmpeg mp3_mf) -> EALayer3 (ealayer3 -E) -> SPS.
std::string encode(const fs::path& inst, const fs::path& song, const fs::path& out)
{
    Ini ini;
    ini.load((inst / "nhl_settings.ini").wstring());
    const fs::path ffmpeg = ini_path(inst, ini, "ffmpeg", "ffmpeg/ffmpeg.exe");
    const fs::path eal = inst / "tools" / "ealayer3" / "ealayer3.exe";
    std::error_code ec;
    if (!fs::exists(ffmpeg, ec)) return "ffmpeg is missing (" + narrow(ffmpeg.wstring()) + ")";
    if (!fs::exists(eal, ec)) return "tools\\ealayer3\\ealayer3.exe is missing";
    const fs::path tmp = inst / "data" / "trax_cache" / ".work";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    const std::wstring mp3 = (tmp / "in.mp3").wstring();
    std::wstring af;
    if (const double lufs = song_lufs(ffmpeg, song, tmp); lufs < kMinLufs - 0.5 && lufs > -40) {
        wchar_t f[96];   // gain up, limiter at -1 dBFS so the boost cannot clip
        swprintf(f, 96, L" -af volume=%.1fdB,alimiter=limit=0.891:level=disabled", kMinLufs - lufs);
        af = f;
    }
    if (run(ffmpeg.wstring(), L"-hide_banner -loglevel error -y -i \"" + song.wstring() + L"\" -vn" + af +
                                  L" -ar 44100 -ac 2 -c:a mp3_mf -b:a 256k -map_metadata -1 \"" + mp3 + L"\"",
            tmp) != 0 || !fs::exists(mp3, ec))
        return "could not convert " + narrow(song.filename().wstring()) + " to MP3";
    if (run(eal.wstring(), L"-E in.mp3 --two-files -b-", tmp) != 0)
        return "ealayer3 failed on " + narrow(song.filename().wstring());
    std::vector<uint8_t> h, s;
    if (!read_file(tmp / "in.ealayer3.header", h) || !read_file(tmp / "in.ealayer3", s) || h.size() < 8 || s.empty() ||
        (h[0] & 0x0F) != 5)
        return "ealayer3 gave no audio for " + narrow(song.filename().wstring());
    std::vector<uint8_t> sps = { 0x48, 0, 0, 0x0C };
    h[0] = 0x15;   // SNR v1 (what the game's bank uses), codec 5 = EALayer3
    sps.insert(sps.end(), h.begin(), h.begin() + 8);
    for (size_t p = 0; p < s.size();) {
        const uint32_t bs = be32(&s[p]) & 0xFFFFFF;
        if (bs < 8 || p + bs > s.size()) return "bad EALayer3 block in " + narrow(song.filename().wstring());
        const size_t at = sps.size();
        sps.insert(sps.end(), s.begin() + p, s.begin() + p + bs);
        sps[at] = 0x44;
        p += bs;
    }
    const uint8_t pad = (uint8_t)((4 - (sps.size() + 4) % 4) % 4);
    sps.insert(sps.end(), { 0x45, 0, 0, (uint8_t)(4 + pad) });
    sps.insert(sps.end(), pad, 0);
    fs::remove_all(tmp, ec);
    return write_file(out, sps.data(), sps.size()) ? "" : "cannot write " + narrow(out.wstring());
}

// ---- Auto-play (#318d) ------------------------------------------------------------
// music_graffitidata.bin (GRF0) holds the lists the music box picks songs from: offsets
// are from 0x1C; header +0x14 group count, +0x18 group table {u32 nodes, i32 ptr (<0 =
// none)}; nodes 0x10 bytes, +0xC -> list {u32 n, u32 sound id[n]}; +0xC of the header
// is the "relocated" flag (0 in the file). The Trax menu list holds the game's 10 Trax
// ids, so songs not in it only ever played when picked by hand (user, 11-11). Every
// list holding a Trax id gets a copy at the end of the file with the new ids added.
bool patch_grf(std::vector<uint8_t>& g, const std::vector<uint32_t>& trax, const std::vector<uint32_t>& add, int& lists)
{
    lists = 0;
    constexpr size_t B = 0x1C;
    if (g.size() < B || memcmp(g.data(), "GRF0", 4) != 0 || be32(&g[0xC]) != 0) return false;
    const size_t size0 = g.size();
    auto ok = [&](size_t at, size_t n) { return at <= size0 && n <= size0 - at; };
    const uint32_t groups = be32(&g[0x14]);
    const size_t gt = B + be32(&g[0x18]);
    if (!ok(gt, (size_t)groups * 8)) return false;
    std::vector<size_t> nodes;   // file offsets of node list pointers
    for (uint32_t j = 0; j < groups; j++) {
        const uint32_t n = be32(&g[gt + 8 * j]);
        const int32_t p = (int32_t)be32(&g[gt + 8 * j + 4]);
        if (p < 0 || !n) continue;
        if (!ok(B + (size_t)p, (size_t)n * 16)) return false;
        for (uint32_t k = 0; k < n; k++) nodes.push_back(B + (size_t)p + 16 * k + 0xC);
    }
    std::map<uint32_t, uint32_t> moved;   // old list ptr -> new list ptr
    for (size_t at : nodes) {
        const uint32_t lp = be32(&g[at]);
        if (!moved.count(lp)) {
            if (!ok(B + lp, 4) || !ok(B + lp + 4, (size_t)be32(&g[B + lp]) * 4)) return false;
            const uint32_t n = be32(&g[B + lp]);
            std::vector<uint32_t> ids(n);
            bool trax_list = false;
            for (uint32_t i = 0; i < n; i++) {
                ids[i] = be32(&g[B + lp + 4 + 4 * i]);
                trax_list |= std::find(trax.begin(), trax.end(), ids[i]) != trax.end();
            }
            if (!trax_list) { moved[lp] = lp; continue; }
            for (uint32_t id : add)
                if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
            while (g.size() % 4) g.push_back(0);
            moved[lp] = (uint32_t)(g.size() - B);
            g.resize(g.size() + 4 + ids.size() * 4);
            put32(&g[B + moved[lp]], (uint32_t)ids.size());
            for (size_t i = 0; i < ids.size(); i++) put32(&g[B + moved[lp] + 4 + 4 * i], ids[i]);
            lists++;
        }
        put32(&g[at], moved[lp]);
    }
    return lists > 0;
}

// ---- Album art (#318c) ------------------------------------------------------------
// The Trax screen shows AssetID N as fe/ion/artassets/eatrax/eN.big (+ eatrax_thumbs/
// eN_thumb.big) from cache.big: a BIGF {Apt movie, shape, refpack'd DDS DXT5, Apt
// constants}. e0..e21 are all the EA SPORTS logo. The movie shows the texture's
// top-left 275x271 (thumb: 64x64 texture, 61x60 shown). Loose files under USRDIR\fe
// override cache.big (mods ship them), so each song with a cover gets its own eN.big,
// a copy of e0.big with the texture swapped. Numbers from kArtBase up are free.
constexpr int kArtBase = 23;
struct ArtKind { const char* tmpl; int tex, w, h; };
constexpr ArtKind kArt[2] = { { "fe/ion/artassets/eatrax/e0.big", 512, 275, 271 },
                              { "fe/ion/artassets/eatrax_thumbs/e0_thumb.big", 64, 61, 60 } };

// DXT5 (opaque): colour endpoints from the block's principal axis.
void dxt5_block(const uint8_t* px /*16 rgba*/, uint8_t* out)
{
    out[0] = out[1] = 255;   // alpha 255, all alpha indices 0
    memset(out + 2, 0, 6);
    float mean[3] = {}, cov[6] = {};
    for (int i = 0; i < 16; i++) for (int c = 0; c < 3; c++) mean[c] += px[i * 4 + c] / 16.f;
    for (int i = 0; i < 16; i++) {
        const float r = px[i * 4] - mean[0], g = px[i * 4 + 1] - mean[1], b = px[i * 4 + 2] - mean[2];
        cov[0] += r * r; cov[1] += r * g; cov[2] += r * b; cov[3] += g * g; cov[4] += g * b; cov[5] += b * b;
    }
    float ax[3] = { 1, 1, 1 };
    for (int it = 0; it < 6; it++) {
        const float x = cov[0] * ax[0] + cov[1] * ax[1] + cov[2] * ax[2], y = cov[1] * ax[0] + cov[3] * ax[1] + cov[4] * ax[2],
                    z = cov[2] * ax[0] + cov[4] * ax[1] + cov[5] * ax[2];
        const float m = std::max({ fabsf(x), fabsf(y), fabsf(z) });
        if (m < 1e-6f) break;
        ax[0] = x / m; ax[1] = y / m; ax[2] = z / m;
    }
    float lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < 16; i++) {
        const float t = (px[i * 4] - mean[0]) * ax[0] + (px[i * 4 + 1] - mean[1]) * ax[1] + (px[i * 4 + 2] - mean[2]) * ax[2];
        lo = std::min(lo, t); hi = std::max(hi, t);
    }
    const float n2 = ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2];
    auto to565 = [&](float t) {
        int v[3];
        for (int c = 0; c < 3; c++) v[c] = std::clamp((int)lroundf(mean[c] + ax[c] * t / std::max(n2, 1e-6f)), 0, 255);
        return (uint16_t)((v[0] * 31 + 127) / 255 << 11 | (v[1] * 63 + 127) / 255 << 5 | (v[2] * 31 + 127) / 255);
    };
    uint16_t c0 = to565(hi), c1 = to565(lo);
    if (c0 < c1) std::swap(c0, c1);
    uint8_t pal[4][3];
    auto expand = [](uint16_t c, uint8_t* o) { o[0] = (uint8_t)((c >> 11) * 255 / 31); o[1] = (uint8_t)((c >> 5 & 63) * 255 / 63); o[2] = (uint8_t)((c & 31) * 255 / 31); };
    expand(c0, pal[0]);
    expand(c1, pal[1]);
    for (int c = 0; c < 3; c++) {
        pal[2][c] = (uint8_t)((2 * pal[0][c] + pal[1][c]) / 3);
        pal[3][c] = (uint8_t)((pal[0][c] + 2 * pal[1][c]) / 3);
    }
    uint32_t idx = 0;
    if (c0 != c1)
        for (int i = 0; i < 16; i++) {
            int best = 0, bd = INT_MAX;
            for (int k = 0; k < 4; k++) {
                int d = 0;
                for (int c = 0; c < 3; c++) d += (px[i * 4 + c] - pal[k][c]) * (px[i * 4 + c] - pal[k][c]);
                if (d < bd) bd = d, best = k;
            }
            idx |= (uint32_t)best << (2 * i);
        }
    out[8] = (uint8_t)c0; out[9] = (uint8_t)(c0 >> 8); out[10] = (uint8_t)c1; out[11] = (uint8_t)(c1 >> 8);
    for (int b = 0; b < 4; b++) out[12 + b] = (uint8_t)(idx >> (8 * b));
}

// rgba w x h -> refpack(DDS DXT5 size x size), the picture top-left, edges extended.
std::vector<uint8_t> art_texture(const std::vector<uint8_t>& rgba, int w, int h, int size)
{
    std::vector<uint8_t> dds(128 + (size_t)size * size, 0);
    uint8_t* d = dds.data();
    auto le = [&](size_t at, uint32_t v) { memcpy(d + at, &v, 4); };
    memcpy(d, "DDS ", 4);
    le(4, 124); le(8, 0x81007); le(12, size); le(16, size); le(20, (uint32_t)size * size);
    le(76, 32); le(80, 4); memcpy(d + 84, "DXT5", 4); le(108, 0x1000);
    uint8_t blk[64];
    for (int by = 0; by < size; by += 4)
        for (int bx = 0; bx < size; bx += 4) {
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++)
                    memcpy(blk + (y * 4 + x) * 4, &rgba[((size_t)std::min(by + y, h - 1) * w + std::min(bx + x, w - 1)) * 4], 4);
            dxt5_block(blk, d + 128 + ((size_t)(by / 4) * (size / 4) + bx / 4) * 16);
        }
    // refpack, literals only: 10 FB, u24 size, runs of 0xE0+ (4..112 bytes), 0xFC+ end.
    std::vector<uint8_t> r = { 0x10, 0xFB, (uint8_t)(dds.size() >> 16), (uint8_t)(dds.size() >> 8), (uint8_t)dds.size() };
    size_t p = 0;
    for (; dds.size() - p >= 4;) {
        const size_t n = std::min<size_t>(112, (dds.size() - p) & ~(size_t)3);
        r.push_back((uint8_t)(0xE0 + (n - 4) / 4));
        r.insert(r.end(), dds.begin() + p, dds.begin() + p + n);
        p += n;
    }
    r.push_back((uint8_t)(0xFC + (dds.size() - p)));
    r.insert(r.end(), dds.begin() + p, dds.end());
    return r;
}

// Template BIGF with its texture (the refpack entry) swapped; later entries move.
bool art_big(const std::vector<uint8_t>& tmpl, const std::vector<uint8_t>& tex, std::vector<uint8_t>& out)
{
    if (tmpl.size() < 16 || memcmp(tmpl.data(), "BIGF", 4) != 0) return false;
    const uint32_t cnt = be32(&tmpl[8]);
    struct E { size_t field; uint32_t off, size; };
    std::vector<E> es;
    size_t p = 16;
    for (uint32_t i = 0; i < cnt && cnt < 64; i++) {
        if (p + 9 > tmpl.size()) return false;
        es.push_back({ p, be32(&tmpl[p]), be32(&tmpl[p + 4]) });
        const void* z = memchr(&tmpl[p + 8], 0, tmpl.size() - p - 8);
        if (!z) return false;
        p = (const uint8_t*)z - tmpl.data() + 1;
    }
    const E* t = nullptr;
    for (const E& e : es)
        if (e.size > 1000 && (size_t)e.off + e.size <= tmpl.size() && tmpl[e.off] == 0x10 && tmpl[e.off + 1] == 0xFB) t = &e;
    if (!t) return false;
    const uint32_t old_end = (t->off + t->size + 63) & ~63u, new_end = (uint32_t)((t->off + tex.size() + 63) & ~(size_t)63);
    uint32_t end = 0;
    for (const E& e : es) end = std::max(end, e.off + e.size);
    if (end > tmpl.size() || old_end > end) return false;
    out.assign(tmpl.begin(), tmpl.begin() + t->off);
    out.insert(out.end(), tex.begin(), tex.end());
    out.resize(new_end, 0);
    out.insert(out.end(), tmpl.begin() + old_end, tmpl.begin() + end);
    for (const E& e : es) {
        if (&e == t) put32(&out[e.field + 4], (uint32_t)tex.size());
        else if (e.off >= old_end) put32(&out[e.field], e.off - old_end + new_end);
    }
    const uint32_t total = (uint32_t)out.size();
    memcpy(&out[4], &total, 4);   // little-endian, as the game's files have it
    return true;
}

// The song's cover: a same-named .jpg/.png beside it, else the picture in its tags.
fs::path art_source(const fs::path& song)
{
    std::error_code ec;
    for (const wchar_t* ext : { L".jpg", L".jpeg", L".png" }) {
        fs::path p = song;
        p.replace_extension(ext);
        if (fs::exists(p, ec)) return p;
    }
    return song;
}

// Writes big/thumb from the song's cover; false = no cover (or it could not be read).
bool make_art(const fs::path& inst, const fs::path& song, const std::vector<uint8_t> tmpl[2], const fs::path out[2])
{
    Ini ini;
    ini.load((inst / "nhl_settings.ini").wstring());
    const fs::path ffmpeg = ini_path(inst, ini, "ffmpeg", "ffmpeg/ffmpeg.exe");
    const fs::path tmp = inst / "data" / "trax_cache" / ".art";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    std::wstring args = L"-hide_banner -loglevel error -y -i \"" + art_source(song).wstring() + L"\" -an -filter_complex \"[0:v:0]split=2[a][b]";
    for (int k = 0; k < 2; k++)
        args += L";[" + std::wstring(k ? L"b" : L"a") + L"]scale=" + std::to_wstring(kArt[k].w) + L":" + std::to_wstring(kArt[k].h) +
                L":flags=lanczos,format=rgba[o" + std::to_wstring(k) + L"]";
    args += L"\"";
    for (int k = 0; k < 2; k++) args += L" -map [o" + std::to_wstring(k) + L"] -frames:v 1 -f rawvideo o" + std::to_wstring(k) + L".rgba";
    bool ok = run(ffmpeg.wstring(), args, tmp) == 0;
    for (int k = 0; ok && k < 2; k++) {
        std::vector<uint8_t> rgba, big;
        ok = read_file(tmp / (L"o" + std::to_wstring(k) + L".rgba"), rgba) && rgba.size() == (size_t)kArt[k].w * kArt[k].h * 4 &&
             art_big(tmpl[k], art_texture(rgba, kArt[k].w, kArt[k].h, kArt[k].tex), big) && write_file(out[k], big.data(), big.size());
    }
    fs::remove_all(tmp, ec);
    return ok;
}

}  // namespace

std::vector<TraxSong> trax_songs(const std::wstring& install_dir)
{
    std::vector<TraxSong> out;
    std::error_code ec;
    const fs::path dir = fs::path(install_dir) / "music" / "EA Trax";
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && is_song(it->path())) out.push_back({ it->path().wstring(), "", "" });
    std::sort(out.begin(), out.end(), [](const TraxSong& a, const TraxSong& b) { return _wcsicmp(a.path.c_str(), b.path.c_str()) < 0; });
    static thread_local bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));   // property store
    (void)com;
    for (TraxSong& s : out) song_meta(s.path, s.artist, s.title);
    return out;
}

// The bank, in full (vanilla and the user's mod alike, 239 sounds):
//   SBR: header (sound count u32 @0x1C), sounds @0xB6 x10 {id, u16 1, rec ptr}, sorted by
//        id; recs @0xB6+M*10 x6 {u16 0, u32 -> slot entry}; slot table @0xB6+M*16 x4.
//   MBX: the same sounds in the same order as records; everything after the records is
//        their props (string pointer tables + strings), nothing else points there.
// Entries go first into the 7 empty MenuTunes records (41..47, trax4), then into new
// sounds appended at the end of both files (new ids above the highest, new slots).
std::string build_trax(const std::wstring& install_dir, std::string* summary)
{
    const fs::path inst = install_dir, root = inst / "data" / "trax";
    // data\trax is laid into the game folder as is, so the stamp lives in the cache.
    const fs::path dir = root / "USRDIR" / "audio" / "music", stamp = inst / "data" / "trax_cache" / "built.txt";
    std::error_code ec;
    std::vector<TraxSong> songs = trax_songs(install_dir);
    const size_t skipped = songs.size() > (size_t)kTraxMax ? songs.size() - kTraxMax : 0;
    if (skipped) songs.resize(kTraxMax);
    if (songs.empty()) {
        fs::remove_all(root, ec);
        fs::remove(stamp, ec);
        if (summary) *summary = "no EA Trax songs";
        return "";
    }
    // Entries: each song once; [launcher] trax_fill = N (test) repeats them up to N.
    struct Entry { size_t song; std::string artist, title; };
    std::vector<Entry> entries;
    for (size_t i = 0; i < songs.size(); i++) entries.push_back({ i, songs[i].artist, songs[i].title });
    {
        Ini ini;
        ini.load((inst / "nhl_settings.ini").wstring());
        const int fill = std::clamp(atoi(ini.get("launcher", "trax_fill", "0").c_str()), 0, kTraxMax);
        for (size_t k = entries.size(); k < (size_t)fill; k++)
            entries.push_back({ k % songs.size(), songs[k % songs.size()].artist,
                                songs[k % songs.size()].title + " (" + std::to_string(k + 1) + ")" });
    }

    Bank bank;
    if (std::string e = load_bank(inst, bank); !e.empty()) return e;
    const std::vector<uint8_t>& sbr0 = bank.sbr;
    const std::vector<uint8_t>& mbx0 = bank.mbx;
    const std::string bad = "the music bank from " + bank.from + " has an unexpected layout";
    if (sbr0.size() < 0xB6 || mbx0.size() < 0xAC || memcmp(mbx0.data(), "MBX0", 4) != 0) return bad;
    const uint32_t M0 = be32(&sbr0[0x1C]), T0 = 0xB6 + M0 * 16;
    if (M0 < kFirstRecord + 7 || M0 > 100000 || sbr0.size() < T0 + (size_t)M0 * 4 || be32(&sbr0[T0]) != 0x10 ||
        be32(&mbx0[0xA4]) != M0 || be32(&mbx0[0xA8]) != 0xAC || mbx0.size() < 0xAC + (size_t)M0 * 0x24)
        return bad;
    std::vector<uint32_t> sid(M0), slot(M0), offs(M0 + 1);
    uint32_t max_sid = 0;
    for (uint32_t k = 0; k < M0; k++) {
        sid[k] = be32(&sbr0[0xB6 + k * 10]);
        const uint32_t rec = be32(&sbr0[0xB6 + k * 10 + 6]);
        if (rec + 6 > sbr0.size() || be32(&mbx0[0xAC + k * 0x24]) != sid[k]) return bad;
        const uint32_t p = be32(&sbr0[rec + 2]);
        if (p < T0 || (p - T0) % 4 || (p - T0) / 4 >= M0) return bad;
        slot[k] = (p - T0) / 4;
        max_sid = std::max(max_sid, sid[k]);
        offs[k] = be32(&sbr0[T0 + k * 4]);
    }
    offs[M0] = (uint32_t)bank.sbs_size;
    auto props_str = [&](const std::vector<uint8_t>& m, uint32_t rec, uint32_t i) -> std::string {
        const uint32_t tbl = be32(&m[rec + 0x20]);
        if (tbl + 4 * i + 4 > m.size()) return "";
        const uint32_t p = be32(&m[tbl + 4 * i]);
        return p < m.size() ? std::string((const char*)&m[p], strnlen((const char*)&m[p], m.size() - p)) : "";
    };
    std::vector<uint32_t> targets;   // records that get an entry
    for (uint32_t k = kFirstRecord; k < kFirstRecord + 7 && targets.size() < entries.size(); k++) {
        const uint32_t rec = 0xAC + k * 0x24;
        if (be32(&mbx0[rec + 0x1C]) != 1 || props_str(mbx0, rec, 1) != "MenuTunes") return bad;
        targets.push_back(k);
    }
    const uint32_t added = (uint32_t)(entries.size() - targets.size()), M = M0 + added;
    for (uint32_t j = 0; j < added; j++) targets.push_back(M0 + j);

    // What gets built; skip all work when nothing changed.
    std::string key = bank.key + "|v5";
    std::map<size_t, std::string> song_key, art_key;
    for (size_t i = 0; i < songs.size(); i++) {
        song_key[i] = file_key(songs[i].path);
        const fs::path a = art_source(songs[i].path);
        art_key[i] = song_key[i] + (a != fs::path(songs[i].path) ? "|" + file_key(a) : "") + "|art1";
        key += "|" + art_key[i];
    }
    for (const Entry& e : entries) key += "|" + song_key[e.song] + "|" + e.artist + "|" + e.title;
    {
        std::vector<uint8_t> old;
        if (read_file(stamp, old) && std::string(old.begin(), old.end()) == key && fs::exists(dir / "music_samples.sbs", ec)) {
            if (summary) *summary = std::to_string(entries.size()) + " EA Trax songs (unchanged)";
            return "";
        }
    }

    // Encode each song once (cached per song file).
    const fs::path cache = inst / "data" / "trax_cache";
    fs::create_directories(cache, ec);
    std::vector<fs::path> sps(songs.size());
    for (size_t i = 0; i < songs.size(); i++) {
        char name[32];
        snprintf(name, sizeof name, "%016llx.sps", (unsigned long long)fnv(0xCBF29CE484222325ull, song_key[i] + "|lufs-9"));
        sps[i] = cache / name;
        if (!fs::exists(sps[i], ec))
            if (std::string e = encode(inst, songs[i].path, sps[i]); !e.empty()) return e;
    }
    // Drop encodes of songs that are gone (or of an older encoder setting).
    {
        std::vector<fs::path> stale;
        for (fs::directory_iterator it(cache, ec), end; !ec && it != end; it.increment(ec))
            if (it->path().extension() == L".sps" && std::find(sps.begin(), sps.end(), it->path()) == sps.end())
                stale.push_back(it->path());
        for (const fs::path& p : stale) fs::remove(p, ec);
    }

    // Covers (cached per song: <hash>.e.big + .t.big, or .noart). Without the game's
    // template files or a cover, the entry keeps the EA SPORTS logo (AssetID 0).
    std::vector<int> art_id(songs.size(), 0);
    std::vector<std::array<fs::path, 2>> art_files(songs.size());
    {
        Ini ini;
        ini.load((inst / "nhl_settings.ini").wstring());
        const fs::path cache_big = ini_path(inst, ini, "disc", "game") / "PS3_GAME" / "USRDIR" / "cache.big";
        std::vector<uint8_t> tmpl[2];
        const bool have_tmpl = eb_read(cache_big, kArt[0].tmpl, tmpl[0]) && eb_read(cache_big, kArt[1].tmpl, tmpl[1]);
        for (size_t i = 0; have_tmpl && i < songs.size(); i++) {
            char h[32];
            snprintf(h, sizeof h, "%016llx", (unsigned long long)fnv(0xCBF29CE484222325ull, art_key[i]));
            const fs::path out[2] = { cache / (std::string(h) + ".e.big"), cache / (std::string(h) + ".t.big") },
                           none = cache / (std::string(h) + ".noart");
            bool ok = fs::exists(out[0], ec) && fs::exists(out[1], ec);
            if (!ok && !fs::exists(none, ec)) {
                ok = make_art(inst, songs[i].path, tmpl, out);
                if (!ok) write_file(none, "", 0);
            }
            if (ok) art_id[i] = kArtBase + (int)i, art_files[i] = { out[0], out[1] };
        }
    }

    // MBX: header, M records (old props pointers moved by the added records), the old
    // props, then each entry's props block.
    const uint32_t delta = added * 0x24, old_end = 0xAC + M0 * 0x24;
    std::vector<uint8_t> mbx(mbx0.begin(), mbx0.begin() + old_end);
    put32(&mbx[0xA4], M);
    for (uint32_t k = 0; k < M0; k++) put32(&mbx[0xAC + k * 0x24 + 0x20], be32(&mbx0[0xAC + k * 0x24 + 0x20]) + delta);
    for (uint32_t j = 0; j < added; j++) {   // new records: like record 41, new ids
        const size_t at = mbx.size();
        mbx.insert(mbx.end(), mbx0.begin() + 0xAC + kFirstRecord * 0x24, mbx0.begin() + 0xAC + (kFirstRecord + 1) * 0x24);
        put32(&mbx[at], max_sid + 1 + j);
    }
    mbx.insert(mbx.end(), mbx0.begin() + old_end, mbx0.end());
    for (uint32_t k = 0; k < M0; k++) {
        const uint32_t rec = 0xAC + k * 0x24, n = be32(&mbx0[rec + 0x1C]), tbl = be32(&mbx0[rec + 0x20]) + delta;
        if (tbl + 8 * n > mbx.size()) return bad;
        for (uint32_t i = 0; i < 2 * n; i++) put32(&mbx[tbl + 4 * i], be32(&mbx[tbl + 4 * i]) + delta);
    }
    for (size_t i = 0; i < entries.size(); i++) {
        const size_t rec = 0xAC + (size_t)targets[i] * 0x24;
        const std::string idx = std::to_string(kFirstIndex + i);
        // FirstBoot: the game's own entries 0, 1 and 9 carry it; without it, a list with
        // only these songs ticked played nothing at startup (user, 11-11).
        const std::pair<std::string, std::string> kv[] = { { "VolumeType", "MenuTunes" }, { "Index", idx },
                                                            { "Artist", entries[i].artist }, { "Title", entries[i].title },
                                                            { "AssetID", std::to_string(art_id[entries[i].song]) },
                                                            { "FirstBoot", "Yes" } };
        while (mbx.size() % 4) mbx.push_back(0);
        const size_t tbl = mbx.size();
        mbx.resize(tbl + sizeof kv / sizeof kv[0] * 8);
        size_t pi = 0;
        for (const auto& [k, v] : kv)
            for (const std::string* s : { &k, &v }) {
                while (mbx.size() % 4) mbx.push_back(0);
                put32(&mbx[tbl + 4 * pi++], (uint32_t)mbx.size());
                mbx.insert(mbx.end(), s->begin(), s->end());
                mbx.push_back(0);
            }
        put32(&mbx[rec + 0x1C], (uint32_t)(sizeof kv / sizeof kv[0]));
        put32(&mbx[rec + 0x20], (uint32_t)tbl);
    }
    while (mbx.size() % 4) mbx.push_back(0);

    // SBR: M sounds; slot of sound k = its old slot, new sounds get new slots M0+j.
    const uint32_t T = 0xB6 + M * 16;
    std::vector<uint8_t> sbr(T + (size_t)M * 4, 0);
    memcpy(sbr.data(), sbr0.data(), 0xB6);
    put32(&sbr[0x1C], M);
    for (uint32_t k = 0; k < M; k++) {
        const uint8_t* old = k < M0 ? &sbr0[0xB6 + k * 10] : &sbr0[0xB6 + kFirstRecord * 10];
        memcpy(&sbr[0xB6 + k * 10], old, 10);
        if (k >= M0) put32(&sbr[0xB6 + k * 10], max_sid + 1 + (k - M0));
        put32(&sbr[0xB6 + k * 10 + 6], 0xB6 + M * 10 + k * 6);
        put32(&sbr[0xB6 + M * 10 + k * 6 + 2], T + 4 * (k < M0 ? slot[k] : k));
    }
    // slot -> song audio
    std::map<uint32_t, size_t> audio;
    for (size_t i = 0; i < entries.size(); i++) {
        const uint32_t k = targets[i];
        audio[k < M0 ? slot[k] : k] = entries[i].song;
    }

    // SBS: the base slots (chosen ones swapped), then the new slots.
    fs::create_directories(dir, ec);
    fs::remove(stamp, ec);
    {
        FILE* in = _wfopen(bank.sbs.c_str(), L"rb");
        if (!in) return "cannot open " + narrow(bank.sbs.wstring());
        std::unique_ptr<FILE, int (*)(FILE*)> gi(in, fclose);
        const fs::path out_p = dir / "music_samples.sbs", part = out_p.wstring() + L".part";
        FILE* o = _wfopen(part.c_str(), L"wb");
        if (!o) return "cannot write " + narrow(out_p.wstring());
        std::vector<uint8_t> buf(8u << 20);
        auto copy = [&](FILE* f, uint64_t at, uint64_t n) {
            _fseeki64(f, (long long)at, SEEK_SET);
            while (n) {
                const size_t got = fread(buf.data(), 1, (size_t)std::min<uint64_t>(n, buf.size()), f);
                if (!got || fwrite(buf.data(), 1, got, o) != got) return false;
                n -= got;
            }
            return true;
        };
        bool ok = copy(in, bank.sbs_off, offs[0]);
        uint64_t pos = offs[0];
        for (uint32_t i = 0; ok && i < M; i++) {
            put32(&sbr[T + i * 4], (uint32_t)pos);
            if (auto a = audio.find(i); a != audio.end()) {
                FILE* s = _wfopen(sps[a->second].c_str(), L"rb");
                const uint64_t n = s ? fs::file_size(sps[a->second], ec) : 0;
                ok = s && copy(s, 0, n);
                if (s) fclose(s);
                pos += n;
            } else if (i < M0) {
                ok = copy(in, bank.sbs_off + offs[i], offs[i + 1] - offs[i]);
                pos += offs[i + 1] - offs[i];
            } else {
                ok = false;
            }
            if (pos > 0xFFFFFFFFull) ok = false;
        }
        if (fclose(o) != 0) ok = false;
        if (!ok) { _wremove(part.c_str()); return "could not write the music bank (4 GB at most)"; }
        fs::rename(part, out_p, ec);
        if (ec) return "cannot replace " + narrow(out_p.wstring());
    }
    if (!write_file(dir / "music_samples.sbr", sbr.data(), sbr.size()) ||
        !write_file(dir / "music_musicboxdata.bin", mbx.data(), mbx.size()))
        return "could not write the music index";
    // Auto-play: the new entries' sounds join every list that holds a Trax song.
    std::string autoplay_note;
    {
        std::vector<uint32_t> trax_ids, add;
        for (uint32_t k = 0; k < M0; k++) {
            const uint32_t rec = 0xAC + k * 0x24, n = be32(&mbx0[rec + 0x1C]);
            for (uint32_t i = 0; i < n; i++)
                if (props_str(mbx0, rec, 2 * i) == "Index") { trax_ids.push_back(sid[k]); break; }
        }
        for (uint32_t k : targets) add.push_back(k < M0 ? sid[k] : max_sid + 1 + (k - M0));
        std::vector<uint8_t> grf = bank.grf;
        int lists = 0;
        fs::remove(dir / "music_graffitidata.bin", ec);
        if (patch_grf(grf, trax_ids, add, lists)) {
            if (!write_file(dir / "music_graffitidata.bin", grf.data(), grf.size())) return "could not write the music lists";
        } else {
            autoplay_note = "; songs play only when picked (music lists not found)";
        }
    }
    // Covers:data\trax\USRDIR\fe\ion\artassets\eatrax[_thumbs]\eN[_thumb].big
    const fs::path art = root / "USRDIR" / "fe" / "ion" / "artassets";
    fs::remove_all(root / "USRDIR" / "fe", ec);
    int covers = 0;
    for (size_t i = 0; i < songs.size(); i++) {
        if (!art_id[i]) continue;
        const std::string n = "e" + std::to_string(art_id[i]);
        const fs::path to[2] = { art / "eatrax" / (n + ".big"), art / "eatrax_thumbs" / (n + "_thumb.big") };
        for (int k = 0; k < 2; k++) {
            fs::create_directories(to[k].parent_path(), ec);
            fs::copy_file(art_files[i][k], to[k], fs::copy_options::overwrite_existing, ec);
            if (ec) return "could not write the cover of " + narrow(fs::path(songs[i].path).filename().wstring());
        }
        covers++;
    }
    write_file(stamp, key.data(), key.size());
    if (summary)
        *summary = std::to_string(entries.size()) + " EA Trax songs (" + std::to_string(covers) + " with covers) added to " + bank.from +
                   (skipped ? " (" + std::to_string(skipped) + " more skipped: " + std::to_string(kTraxMax) + " at most)" : "") +
                   autoplay_note;
    return "";
}

