#include "installer.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

#include "ini.h"
#include "iso9660.h"
#include "pkg.h"
#include "self_decrypt.h"
#include "sfo.h"
#include "../../ps3recomp/ps3recomp-master/libs/system/ps3_psf.h"   // the port's own PARAM.SFO writer (#257)

namespace fs = std::filesystem;

namespace {

// SHA-1 of the 1.01 update's EBOOT.BIN (the encrypted SELF) and of the
// decrypted ELF that NHLLegacy.exe was recompiled from. Any other EBOOT would
// not match the compiled code.
constexpr const char* kUpdateEbootSha1 = "5827C87590BC26408B46026D26B7F65179F3C87E";
constexpr const char* kEbootElfSha1 = "C16C614C130FDE0D755BF69BDBA88E01EAA74C90";

// What the game's FileCopier puts in /dev_hdd1/cache/BLUS31540 on first boot.
// cache.big and cacherender.big are copied too when the cache has room (native
// boots did; RPCS3's smaller cache skipped them).
const char* const kCacheFiles[] = {
    "cacheaudio.big", "cacheboot.big", "cachemovie.big", "cacheoverlay.big", "data0.big",
    "secure0.big.sdat", "gamedata/nis.big", "gamedata/anim/faceposer.big", "cache.big", "cacherender.big",
};
// The game writes its own cache path into this marker once the copy is complete.
constexpr const char* kCacheVer = "881545.ver";
constexpr const char* kCacheVerText = "/dev_hdd1/cache/BLUS31540/881545.ver";

// Archives holding the VP6 movies the ffmpeg bridge replaces: carved by chunk
// walk (kMovieArchives), plus every raw MVhd entry of the archive file tables
// (kMovieTables). The bridge picks the most recently read movie of the published
// size, so a movie with no file of its own got another one's frames (the replay
// transitions showed the arena's on-ice projection loop).
const char* const kMovieArchives[] = { "cacheboot.big", "cachemovie.big", "nocache.big" };
const char* const kMovieTables[] = { "cacheboot.big", "cachemovie.big", "nocache.big", "cache.big" };

struct Cancelled {};

void check_cancel(InstallProgress& p)
{
    if (p.cancel) throw Cancelled{};
}

std::string hex(const uint8_t* d, size_t n)
{
    static const char* x = "0123456789ABCDEF";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += x[d[i] >> 4]; s += x[d[i] & 15]; }
    return s;
}

// SHA-1 of a file ("" on error).
std::string sha1_file(const fs::path& file, InstallProgress* p = nullptr)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return "";
    std::string out;
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (f && BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<uint8_t> buf(1 << 20);
        size_t n;
        while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) {
            BCryptHashData(h, buf.data(), (ULONG)n, 0);
            if (p) check_cancel(*p);
        }
        uint8_t d[20];
        if (BCryptFinishHash(h, d, 20, 0) == 0) out = hex(d, 20);
        BCryptDestroyHash(h);
    }
    if (f) fclose(f);
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

bool read_file(const fs::path& file, std::vector<uint8_t>& out)
{
    std::error_code ec;
    const uintmax_t n = fs::file_size(file, ec);
    if (ec || n > (64u << 20)) return false;
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (!f) return false;
    out.resize((size_t)n);
    const bool ok = fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

bool same_size(const fs::path& file, uint64_t size)
{
    std::error_code ec;
    return fs::is_regular_file(file, ec) && fs::file_size(file, ec) == size;
}

bool is_iso(const std::wstring& src)
{
    std::error_code ec;
    return fs::is_regular_file(src, ec);
}

// A folder source may be the disc root (holding PS3_GAME) or PS3_GAME itself.
fs::path disc_root_of(const fs::path& folder)
{
    std::error_code ec;
    if (fs::exists(folder / "PS3_GAME" / "PARAM.SFO", ec)) return folder;
    if (fs::exists(folder / "PARAM.SFO", ec) && _wcsicmp(folder.filename().c_str(), L"PS3_GAME") == 0)
        return folder.parent_path();
    return {};
}

std::string check_sfo(const std::vector<uint8_t>& sfo, std::string* info)
{
    auto kv = sfo_parse(sfo);
    if (kv.empty()) return "PARAM.SFO is unreadable. Is this a decrypted dump?";
    const std::string id = kv["TITLE_ID"];
    if (id != kTitleId)
        return "This is " + (kv["TITLE"].empty() ? id : kv["TITLE"] + " (" + id + ")") +
               ". The port needs NHL Legacy Edition, US disc " + kTitleId + ".";
    if (info) *info = kv["TITLE"] + " (" + id + "), disc version " + kv["VERSION"];
    return "";
}

// Copies one file with progress inside [f0, f1] of the current step.
struct CopyCtx { InstallProgress* p; double f0, f1; };
DWORD CALLBACK copy_progress(LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD,
                             HANDLE, HANDLE, LPVOID ctx)
{
    auto* c = (CopyCtx*)ctx;
    if (total.QuadPart > 0) c->p->frac = c->f0 + (c->f1 - c->f0) * (double)done.QuadPart / (double)total.QuadPart;
    return c->p->cancel ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

void copy_file(const fs::path& from, const fs::path& to, InstallProgress& p, double f0, double f1)
{
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    CopyCtx c{ &p, f0, f1 };
    BOOL cancel = FALSE;
    if (!CopyFileExW(from.c_str(), to.c_str(), copy_progress, &c, &cancel, 0)) {
        check_cancel(p);
        throw std::runtime_error("could not copy " + narrow(from.wstring()) + " to " + narrow(to.wstring()));
    }
}

// ---- steps ------------------------------------------------------------------

void step_extract_iso(const std::wstring& iso_path, const fs::path& dest, InstallProgress& p)
{
    IsoImage iso;
    if (!iso.open(iso_path)) throw std::runtime_error("ISO: " + iso.error());
    // PS3_UPDATE holds the console firmware update (PS3UPDAT.PUP); the game never reads it.
    auto wanted = [](const IsoFile& f) { return _strnicmp(f.path.c_str(), "PS3_UPDATE/", 11) != 0; };
    uint64_t total = 0, done = 0;
    for (const IsoFile& f : iso.files()) if (wanted(f)) total += f.size;
    std::vector<uint8_t> buf(8u << 20);
    int skipped = 0, count = 0;
    for (const IsoFile& f : iso.files()) {
        if (!wanted(f)) continue;
        count++;
        const fs::path out = dest / fs::path(widen(f.path));
        if (same_size(out, f.size)) { done += f.size; skipped++; continue; }   // resume
        p.set_label("Extracting " + f.path);
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        const fs::path tmp = out.wstring() + L".part";
        FILE* o = _wfopen(tmp.c_str(), L"wb");
        if (!o) throw std::runtime_error("cannot write " + narrow(out.wstring()));
        for (uint64_t off = 0; off < f.size;) {
            const size_t n = iso.read(f, off, buf.data(), (size_t)std::min<uint64_t>(buf.size(), f.size - off));
            if (n == 0 || fwrite(buf.data(), 1, n, o) != n) {
                fclose(o);
                throw std::runtime_error("read/write error while extracting " + f.path);
            }
            off += n; done += n;
            p.frac = total ? (double)done / (double)total : 1.0;
            if (p.cancel) { fclose(o); throw Cancelled{}; }
        }
        fclose(o);
        fs::rename(tmp, out, ec);
        if (ec) throw std::runtime_error("cannot finish " + narrow(out.wstring()));
    }
    p.add_log("Disc extracted: " + std::to_string(count) + " files" +
              (skipped ? " (" + std::to_string(skipped) + " already there)" : ""));
}

void step_cache(const fs::path& usrdir, const fs::path& cache, InstallProgress& p)
{
    uint64_t total = 0, done = 0;
    std::error_code ec;
    for (const char* f : kCacheFiles) total += fs::file_size(usrdir / f, ec);
    for (const char* f : kCacheFiles) {
        const fs::path from = usrdir / f, to = cache / f;
        const uint64_t n = fs::file_size(from, ec);
        if (ec) throw std::runtime_error(std::string("the disc is missing PS3_GAME/USRDIR/") + f);
        if (!same_size(to, n)) {
            p.set_label(std::string("Building the game cache: ") + f);
            copy_file(from, to, p, (double)done / total, (double)(done + n) / total);
        }
        done += n;
        p.frac = (double)done / total;
    }
    FILE* v = _wfopen((cache / kCacheVer).c_str(), L"wb");
    if (!v || fwrite(kCacheVerText, 1, strlen(kCacheVerText), v) != strlen(kCacheVerText))
        throw std::runtime_error("cannot write the cache marker");
    fclose(v);
    p.add_log("Game cache built (" + std::to_string(total >> 20) + " MB)");
}

// EA movie chunks: <tag><u32 size incl. header>; a movie is a run of chunks from
// an MVhd. Same output as tools\vp6carve.py: <archive>_<OFFSET>.vp6.
bool movie_tag(const uint8_t* t)
{
    // The carve stops at alpha-plane chunks (ALhd/AL0K) on purpose: the movies with
    // an alpha stream (the arena environment loops) come from the archive's file
    // table instead (eb_vp6_entries), which gives their exact extent.
    static const char* const tags[] = { "MVhd", "MV0K", "MV0F", "MVIh", "MVIf", "SCHl", "SCCl", "SCDl",
                                        "SCEl", "MPCh", "pQGT", "pIQT" };
    for (const char* s : tags) if (memcmp(t, s, 4) == 0) return true;
    return false;
}

int carve_archive(const fs::path& archive, const fs::path& out_dir, InstallProgress& p, double f0, double f1)
{
    FILE* f = _wfopen(archive.c_str(), L"rb");
    if (!f) throw std::runtime_error("cannot open " + narrow(archive.wstring()));
    std::unique_ptr<FILE, int (*)(FILE*)> guard(f, fclose);
    _fseeki64(f, 0, SEEK_END);
    const uint64_t size = (uint64_t)_ftelli64(f);
    // Pass 1: every MVhd offset.
    std::vector<uint64_t> found;
    std::vector<uint8_t> buf((64u << 20) + 32);
    size_t tail = 0;
    uint64_t off = 0;
    _fseeki64(f, 0, SEEK_SET);
    for (;;) {
        const size_t n = fread(buf.data() + tail, 1, buf.size() - tail, f);
        if (!n) break;
        const size_t len = tail + n;
        const uint64_t start = off - tail;
        for (size_t i = 0; i + 4 <= len; i++)
            if (buf[i] == 'M' && memcmp(&buf[i], "MVhd", 4) == 0 && i + 32 <= len &&
                (found.empty() || found.back() != start + i))
                found.push_back(start + i);
        off += n;
        tail = std::min<size_t>(32, len);
        memmove(buf.data(), buf.data() + len - tail, tail);
        p.frac = f0 + (f1 - f0) * 0.8 * (double)off / (double)size;
        check_cancel(p);
    }
    // Pass 2: walk each candidate's chunks and write the movie.
    const std::string base = narrow(archive.stem().wstring());
    int movies = 0;
    for (size_t k = 0; k < found.size(); k++) {
        uint8_t hdr[32];
        _fseeki64(f, (long long)found[k], SEEK_SET);
        if (fread(hdr, 1, 32, f) != 32) continue;
        const uint32_t le = hdr[4] | hdr[5] << 8 | hdr[6] << 16 | (uint32_t)hdr[7] << 24;
        const uint32_t be = (uint32_t)hdr[4] << 24 | hdr[5] << 16 | hdr[6] << 8 | hdr[7];
        const bool little = le < be;
        uint64_t pos = found[k];
        int frames = 0;
        for (;;) {
            uint8_t c[8];
            _fseeki64(f, (long long)pos, SEEK_SET);
            if (fread(c, 1, 8, f) != 8 || !movie_tag(c)) break;
            const uint32_t sz = little ? (c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24)
                                       : ((uint32_t)c[4] << 24 | c[5] << 16 | c[6] << 8 | c[7]);
            if (sz < 8 || sz > (64u << 20)) break;
            if (!memcmp(c, "MV0K", 4) || !memcmp(c, "MV0F", 4)) frames++;
            pos += sz;
        }
        if (!frames) continue;
        char name[96];
        snprintf(name, sizeof name, "%s_%08llX.vp6", base.c_str(), (unsigned long long)found[k]);
        const fs::path out = out_dir / name;
        const uint64_t len = pos - found[k];
        movies++;
        if (same_size(out, len)) continue;
        FILE* o = _wfopen((out.wstring() + L".part").c_str(), L"wb");
        if (!o) throw std::runtime_error("cannot write " + narrow(out.wstring()));
        _fseeki64(f, (long long)found[k], SEEK_SET);
        for (uint64_t left = len; left;) {
            const size_t n = fread(buf.data(), 1, (size_t)std::min<uint64_t>(left, buf.size()), f);
            if (!n || fwrite(buf.data(), 1, n, o) != n) { fclose(o); throw std::runtime_error("movie write error"); }
            left -= n;
        }
        fclose(o);
        std::error_code ec;
        fs::rename(out.wstring() + L".part", out, ec);
        p.frac = f0 + (f1 - f0) * (0.8 + 0.2 * (double)(k + 1) / (double)found.size());
        check_cancel(p);
    }
    return movies;
}

// EB v3 archive table (tools\eb_extract.py): big-endian header 'EB' u16 ver,
// u32 count @4, u32 align @8, u32 name-table offset @12, u32 names bytes @16,
// u8 name length @20; entries @0x30 x16 {u32 offset/16, u32 0, u32 size, u32 hash};
// names = fixed-length records with one prefix byte.
// Copies every raw movie entry (data starting with MVhd, named or not).
int eb_vp6_entries(const fs::path& archive, const fs::path& out_dir, InstallProgress& p)
{
    FILE* f = _wfopen(archive.c_str(), L"rb");
    if (!f) throw std::runtime_error("cannot open " + narrow(archive.wstring()));
    std::unique_ptr<FILE, int (*)(FILE*)> guard(f, fclose);
    auto be32 = [](const uint8_t* b) { return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; };
    uint8_t hdr[0x30];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || hdr[0] != 'E' || hdr[1] != 'B')
        throw std::runtime_error(narrow(archive.filename().wstring()) + " is not an EB archive");
    const uint32_t cnt = be32(hdr + 4), tbl = be32(hdr + 12), nlen = hdr[20];
    if (cnt > 1000000 || tbl > (256u << 20) || !nlen) throw std::runtime_error("bad EB header");
    std::vector<uint8_t> meta((size_t)tbl + (size_t)cnt * nlen);
    _fseeki64(f, 0, SEEK_SET);
    if (fread(meta.data(), 1, meta.size(), f) != meta.size()) throw std::runtime_error("EB table read error");
    const std::string base = narrow(archive.stem().wstring());
    std::vector<uint8_t> buf(8u << 20);
    int n = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t* e = meta.data() + 0x30 + (size_t)i * 16;
        const uint64_t off = (uint64_t)be32(e) * 16;
        const uint32_t size = be32(e + 8);
        if (size < 64) continue;
        uint8_t head[4];
        _fseeki64(f, (long long)off, SEEK_SET);
        if (fread(head, 1, 4, f) != 4 || memcmp(head, "MVhd", 4) != 0) continue;   // compressed entry
        char out_name[96];
        snprintf(out_name, sizeof out_name, "%s_%08llX.vp6", base.c_str(), (unsigned long long)off);
        const fs::path out = out_dir / out_name;
        n++;
        if (same_size(out, size)) continue;
        FILE* o = _wfopen((out.wstring() + L".part").c_str(), L"wb");
        if (!o) throw std::runtime_error("cannot write " + narrow(out.wstring()));
        _fseeki64(f, (long long)off, SEEK_SET);
        for (uint64_t left = size; left;) {
            const size_t got = fread(buf.data(), 1, (size_t)std::min<uint64_t>(left, buf.size()), f);
            if (!got || fwrite(buf.data(), 1, got, o) != got) { fclose(o); throw std::runtime_error("movie write error"); }
            left -= got;
        }
        fclose(o);
        std::error_code ec;
        fs::rename(out.wstring() + L".part", out, ec);
        check_cancel(p);
    }
    return n;
}

void step_movies(const fs::path& usrdir, const fs::path& out_dir, InstallProgress& p)
{
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    uint64_t total = 0, done = 0;
    for (const char* a : kMovieArchives) total += fs::file_size(usrdir / a, ec);
    int movies = 0;
    for (const char* a : kMovieArchives) {
        const uint64_t n = fs::file_size(usrdir / a, ec);
        p.set_label(std::string("Extracting movies from ") + a);
        movies += carve_archive(usrdir / a, out_dir, p, 0.95 * done / total, 0.95 * (done + n) / total);
        done += n;
    }
    // Movies with an alpha stream (the arena loops, the replay transitions), which
    // the carve stops at, come from the archives' file tables at their exact extent.
    int table = 0;
    for (const char* a : kMovieTables) {
        p.set_label(std::string("Extracting movies listed in ") + a);
        table += eb_vp6_entries(usrdir / a, out_dir, p);
    }
    p.frac = 1.0;
    p.add_log("Movies extracted: " + std::to_string(movies) + " carved + " + std::to_string(table) + " from archive tables");
}

// The official 1.01 update package (UP0006-BLUS31540_00-NHL16NTSCPATCH01): its
// files, sizes and SHA-1s, so an update FOLDER (e.g. RPCS3's, often with mods
// mixed in) can be split into the official update and the player's mod files.
struct OfficialFile { const char* path; uint64_t size; const char* sha1; };
const OfficialFile kOfficialUpdate[] = {
    { "PS3LOGO.DAT", 5120, "033DFC4A2D5646DC02BE118B95E398C3260ACA89" },
    { "PARAM.SFO", 460, "C3069B6807F0BE5B72B4F02A8C2B66F0E9E46AC4" },
    { "ICON0.PNG", 32617, "4CB34A2B1F3D8C58B6A46F7DF7098739682BECAD" },
    { "PIC1.PNG", 2360573, "F42D01523CC8419DCF1388AEB5568C8F7A73A97E" },
    { "USRDIR/EBOOT.BIN", 36212528, kUpdateEbootSha1 },
    { "USRDIR/options.viv", 284, "B2FD5927AD73536B9751A1BD22B778D130DE4E91" },
    { "USRDIR/patch.big", 60596976, "C1CC558DADD7D305F5022C88166C3652F894D58F" },
};
constexpr const char* kImportedMod = "Imported from update folder";

// Explorer's folder files (desktop.ini, Thumbs.db) are not game files.
bool shell_junk(const fs::path& p)
{
    const std::wstring n = p.filename().wstring();
    return _wcsicmp(n.c_str(), L"desktop.ini") == 0 || _wcsicmp(n.c_str(), L"Thumbs.db") == 0;
}

bool is_pkg(const std::wstring& src)
{
    std::error_code ec;
    return fs::is_regular_file(src, ec);
}

void write_all(const fs::path& out, const std::vector<uint8_t>& d)
{
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    const fs::path tmp = out.wstring() + L".part";
    FILE* o = _wfopen(tmp.c_str(), L"wb");
    if (!o || fwrite(d.data(), 1, d.size(), o) != d.size()) {
        if (o) fclose(o);
        throw std::runtime_error("cannot write " + narrow(out.wstring()));
    }
    fclose(o);
    fs::remove(out, ec);
    fs::rename(tmp, out, ec);
    if (ec) throw std::runtime_error("cannot write " + narrow(out.wstring()));
}

// .pkg -> data\update (all its files, verified by the package digest).
void update_from_pkg(const std::wstring& src, const fs::path& update, InstallProgress& p)
{
    PkgFile pkg;
    if (!pkg.open(src)) throw std::runtime_error("update package: " + pkg.error());
    p.set_label("Verifying the update package");
    if (!pkg.verify_digest([&p](double f) { p.frac = 0.3 * f; return !p.cancel; })) {
        check_cancel(p);
        throw std::runtime_error("the update package is damaged (checksum mismatch)");
    }
    int n = 0;
    for (const PkgEntry& e : pkg.entries()) {
        if (e.folder()) continue;
        const fs::path out = update / fs::path(widen(e.name));
        p.set_label("Installing the update: " + e.name);
        std::vector<uint8_t> d;
        if (!pkg.read_all(e, d)) throw std::runtime_error("update package read error: " + e.name);
        write_all(out, d);
        n++;
        check_cancel(p);
    }
    p.frac = 0.5;
    p.add_log("Update installed from the package: " + std::to_string(n) + " files");
}

// Folder -> the official files to data\update, everything else to mods\<imported>.
void update_from_folder(const fs::path& src, const fs::path& update, const fs::path& mods, InstallProgress& p)
{
    std::error_code ec;
    for (const OfficialFile& f : kOfficialUpdate) {
        const fs::path from = src / fs::path(widen(f.path));
        p.set_label(std::string("Checking the update: ") + f.path);
        if (!same_size(from, f.size) || sha1_file(from, &p) != f.sha1)
            throw std::runtime_error(std::string("the update folder's ") + f.path +
                                     " is not the official 1.01 file (pick the .pkg instead?)");
        const fs::path to = update / fs::path(widen(f.path));
        if (!same_size(to, f.size)) copy_file(from, to, p, p.frac, p.frac);
    }
    std::vector<fs::path> extra;
    uint64_t total = 0, done = 0;
    for (auto it = fs::recursive_directory_iterator(src, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || shell_junk(it->path())) continue;
        const std::string rel = narrow(fs::relative(it->path(), src, ec).generic_wstring());
        bool official = false;
        for (const OfficialFile& f : kOfficialUpdate) official |= _stricmp(rel.c_str(), f.path) == 0;
        if (!official) { extra.push_back(it->path()); total += it->file_size(ec); }
    }
    const fs::path mod = mods / kImportedMod;
    for (const fs::path& f : extra) {
        const fs::path to = mod / fs::relative(f, src, ec);
        const uint64_t n = fs::file_size(f, ec);
        if (!same_size(to, n)) {
            p.set_label("Importing mod files: " + narrow(f.filename().wstring()));
            copy_file(f, to, p, 0.5 * done / total, 0.5 * (done + n) / total);
        }
        done += n;
    }
    p.frac = 0.5;
    p.add_log("Update installed from the folder: " + std::to_string(std::size(kOfficialUpdate)) + " official files" +
              (extra.empty() ? std::string() : ", " + std::to_string(extra.size()) + " mod files imported into mods\\" + kImportedMod));
}

void step_update(const InstallConfig& cfg, const fs::path& data, const fs::path& mods, InstallProgress& p)
{
    const fs::path update = data / "update", elf_out = data / "eboot.elf";
    std::error_code ec;
    if (is_pkg(cfg.update_src)) update_from_pkg(cfg.update_src, update, p);
    else update_from_folder(cfg.update_src, update, mods, p);
    const fs::path eboot = update / "USRDIR" / "EBOOT.BIN";
    if (sha1_file(eboot, &p) != kUpdateEbootSha1)
        throw std::runtime_error("the update's EBOOT.BIN is not the 1.01 update this port was built from");

    // The decrypted EBOOT (NPDRM SELF -> ELF). --eboot-elf supplies one instead (dev).
    if (sha1_file(elf_out) == kEbootElfSha1) { p.frac = 1.0; return; }
    if (!cfg.eboot_elf.empty()) {
        p.set_label("Checking the decrypted EBOOT");
        if (sha1_file(cfg.eboot_elf, &p) != kEbootElfSha1)
            throw std::runtime_error("the decrypted EBOOT is not the 1.01 ELF this port was built from");
        copy_file(cfg.eboot_elf, elf_out, p, 0.5, 1.0);
        return;
    }
    p.set_label("Decrypting the EBOOT");
    std::vector<uint8_t> self, elf;
    if (!read_file(eboot, self)) throw std::runtime_error("cannot read the update's EBOOT.BIN");
    if (std::string err = self_to_elf(self, elf); !err.empty())
        throw std::runtime_error("EBOOT decryption failed: " + err);
    p.frac = 0.8;
    const fs::path tmp = elf_out.wstring() + L".part";
    fs::create_directories(elf_out.parent_path(), ec);
    FILE* o = _wfopen(tmp.c_str(), L"wb");
    if (!o || fwrite(elf.data(), 1, elf.size(), o) != elf.size()) {
        if (o) fclose(o);
        throw std::runtime_error("cannot write data\\eboot.elf");
    }
    fclose(o);
    if (sha1_file(tmp) != kEbootElfSha1)
        throw std::runtime_error("the decrypted EBOOT does not match the 1.01 ELF this port was built from");
    fs::rename(tmp, elf_out, ec);
    p.add_log("EBOOT decrypted (" + std::to_string(elf.size() >> 20) + " MB, verified)");
}

void step_settings(const InstallConfig& cfg, const fs::path& disc_root, InstallProgress& p)
{
    const fs::path inst = cfg.install_dir;
    Ini ini;
    ini.load((inst / "nhl_settings.ini").wstring());   // keep the player's [video] etc.
    std::error_code ec;
    fs::path disc = fs::relative(disc_root, inst, ec);
    if (ec || disc.empty() || narrow(disc.wstring()).rfind("..", 0) == 0) disc = disc_root;   // outside: absolute
    ini.set("paths", "eboot", "data/eboot.elf");
    ini.set("paths", "disc", narrow(disc.generic_wstring()));
    ini.set("paths", "hdd0", "data/hdd0");
    ini.set("paths", "hdd1", "data/hdd1");
    ini.set("paths", "user", "user");
    ini.set("paths", "movies", "data/movies");
    ini.set("paths", "ffmpeg", ini.get("paths", "ffmpeg", "ffmpeg/ffmpeg.exe"));
    ini.set("paths", "edat_cache", "data/edat_cache");
    if (ini.get("video", "fullscreen").empty()) ini.set("video", "fullscreen", "1");
    if (ini.get("video", "vrr").empty()) ini.set("video", "vrr", "1");
    ini.set("launcher", "game_source", narrow(cfg.game_src));
    ini.set("launcher", "update_source", narrow(cfg.update_src));
    if (!ini.save((inst / "nhl_settings.ini").wstring(),
                  "; NHL Legacy native settings. Written by NHLLauncher; hand edits are kept."))
        throw std::runtime_error("cannot write nhl_settings.ini");
    p.add_log("Settings written");
}

}  // namespace

std::string narrow(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string check_game_source(const std::wstring& src, std::string* info)
{
    if (src.empty()) return "Pick your NHL Legacy Edition ISO or game folder.";
    std::vector<uint8_t> sfo, eboot;
    if (is_iso(src)) {
        IsoImage iso;
        if (!iso.open(src)) return "ISO: " + iso.error();
        const IsoFile* s = iso.find("PS3_GAME/PARAM.SFO");
        const IsoFile* e = iso.find("PS3_GAME/USRDIR/EBOOT.BIN");
        if (!s || !e) return "This ISO has no PS3_GAME folder. Is it a PS3 game disc?";
        if (!iso.read_all(*s, sfo)) return "ISO read error";
        eboot.resize(4);
        iso.read(*e, 0, eboot.data(), 4);
        if (sfo.size() < 4 || memcmp(sfo.data(), "\0PSF", 4) != 0 || memcmp(eboot.data(), "SCE\0", 4) != 0)
            return "This ISO is still encrypted (a raw Redump image). Decrypt it first, or extract it to a folder.";
        std::string r = check_sfo(sfo, info);
        if (r.empty() && info) *info += ", ISO, " + std::to_string(iso.total_bytes() >> 20) + " MB";
        return r;
    }
    const fs::path root = disc_root_of(src);
    if (root.empty()) return "This folder has no PS3_GAME\\PARAM.SFO. Pick the folder that contains PS3_GAME.";
    if (!read_file(root / "PS3_GAME" / "PARAM.SFO", sfo)) return "PARAM.SFO is unreadable";
    std::string r = check_sfo(sfo, info);
    if (r.empty() && info) *info += ", folder";
    return r;
}

std::string check_update_source(const std::wstring& src, std::string* info)
{
    if (src.empty()) return "Pick the 1.01 update: the .pkg file or an update folder.";
    std::vector<uint8_t> sfo;
    std::string kind;
    if (is_pkg(src)) {
        PkgFile pkg;
        if (!pkg.open(src)) return "Update package: " + pkg.error();
        const PkgEntry* s = pkg.find("PARAM.SFO");
        if (!s || !pkg.read_all(*s, sfo)) return "The package has no PARAM.SFO.";
        if (!pkg.find("USRDIR/EBOOT.BIN")) return "The package has no USRDIR/EBOOT.BIN.";
        kind = "package " + pkg.content_id();
    } else {
        const fs::path p = src;
        if (!read_file(p / "PARAM.SFO", sfo)) return "This folder has no PARAM.SFO. Pick the BLUS31540 update folder.";
        std::error_code ec;
        if (!fs::exists(p / "USRDIR" / "EBOOT.BIN", ec)) return "The update folder has no USRDIR\\EBOOT.BIN.";
        int extra = 0;
        for (auto it = fs::recursive_directory_iterator(p, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec)) extra++;
        extra -= (int)std::size(kOfficialUpdate);
        kind = "folder" + (extra > 0 ? ", plus " + std::to_string(extra) + " mod files (they go to the Mods tab)" : std::string());
    }
    auto kv = sfo_parse(sfo);
    if (kv["TITLE_ID"] != kTitleId) return "This is not an NHL Legacy Edition (BLUS31540) update.";
    if (kv["APP_VER"] != "01.01") return "This is update version " + kv["APP_VER"] + "; the port needs 01.01.";
    if (info) *info = "Update " + kv["APP_VER"] + " (" + kv["TITLE_ID"] + "), " + kind;
    return "";
}

bool install_complete(const std::wstring& install_dir)
{
    const fs::path inst = install_dir;
    Ini ini;
    if (!ini.load((inst / "nhl_settings.ini").wstring()) || ini.get("paths", "eboot").empty()) return false;
    std::error_code ec;
    return fs::exists(inst / "data" / "eboot.elf", ec) &&
           fs::exists(inst / "data" / "hdd1" / "cache" / kTitleId / kCacheVer, ec) &&
           fs::exists(inst / "data" / "update" / "USRDIR" / "EBOOT.BIN", ec);
}

std::vector<ModInfo> list_mods(const std::wstring& install_dir)
{
    const fs::path inst = install_dir;
    Ini ini;
    ini.load((inst / "nhl_settings.ini").wstring());
    std::vector<ModInfo> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(inst / "mods", ec); it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        if (it->path().filename().wstring().rfind(L".", 0) == 0) continue;   // mods\.incoming: an install in progress
        ModInfo m;
        m.name = narrow(it->path().filename().wstring());
        for (auto f = fs::recursive_directory_iterator(it->path(), ec); f != fs::recursive_directory_iterator(); f.increment(ec))
            if (f->is_regular_file(ec)) m.files++;
        m.enabled = ini.get("mods", m.name, "1") != "0";
        m.roster = fs::is_regular_file(it->path() / "roster" / "SYS-DATA", ec);
        out.push_back(std::move(m));
    }
    // Load order: [launcher] mod_order first, then the rest alphabetically.
    std::vector<std::string> order;
    const std::string list = ini.get("launcher", "mod_order");
    for (size_t a = 0; a < list.size();) {
        size_t b = list.find('|', a);
        if (b == std::string::npos) b = list.size();
        if (b > a) order.push_back(list.substr(a, b - a));
        a = b + 1;
    }
    auto rank = [&](const std::string& n) {
        for (size_t i = 0; i < order.size(); i++)
            if (_stricmp(order[i].c_str(), n.c_str()) == 0) return i;
        return order.size();
    };
    std::sort(out.begin(), out.end(), [&](const ModInfo& a, const ModInfo& b) {
        const size_t ra = rank(a.name), rb = rank(b.name);
        return ra != rb ? ra < rb : _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

void set_mod_order(const std::wstring& install_dir, const std::vector<std::string>& names)
{
    const fs::path ini_path = fs::path(install_dir) / "nhl_settings.ini";
    Ini ini;
    if (!ini.load(ini_path.wstring()) && fs::exists(ini_path)) return;   // never overwrite what could not be read
    std::string list;
    for (const std::string& n : names) list += (list.empty() ? "" : "|") + n;
    ini.set("launcher", "mod_order", list);
    ini.save(ini_path.wstring(), "; NHL Legacy native settings. Written by NHLLauncher; hand edits are kept.");
}

bool is_official_update_file(const std::string& rel, uint64_t size)
{
    for (const OfficialFile& f : kOfficialUpdate)
        if (_stricmp(rel.c_str(), f.path) == 0 && f.size == size) return true;
    return false;
}

void set_mod_enabled(const std::wstring& install_dir, const std::string& name, bool on)
{
    const fs::path ini_path = fs::path(install_dir) / "nhl_settings.ini";
    Ini ini;
    ini.load(ini_path.wstring());
    ini.set("mods", name, on ? "1" : "0");
    ini.save(ini_path.wstring(), "; NHL Legacy native settings. Written by NHLLauncher; hand edits are kept.");
}

std::string compose_hdd0(const std::wstring& install_dir, std::string* summary)
{
    const fs::path inst = install_dir, update = inst / "data" / "update";
    const fs::path hdd0 = inst / "data" / "hdd0", game = hdd0 / "game" / kTitleId, manifest = hdd0 / "composed.txt";
    std::error_code ec;
    if (!fs::exists(update / "USRDIR" / "EBOOT.BIN", ec)) return "the update is not installed";

    // Wanted files: relative path (case-folded key) -> source. Mods override the
    // update; among mods the one higher in the load order wins.
    std::map<std::wstring, std::pair<fs::path, fs::path>> want;
    int from_mods = 0;
    auto add_tree = [&](const fs::path& root, bool mod) {
        for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec) || shell_junk(it->path())) continue;
            const fs::path rel = fs::relative(it->path(), root, ec);
            std::wstring key = rel.generic_wstring();
            CharLowerBuffW(key.data(), (DWORD)key.size());
            if (mod && key.rfind(L"roster/", 0) == 0) continue;   // a mod's roster is a save, not a game file (apply_roster)
            want[key] = { rel, it->path() };
            from_mods += mod;
        }
    };
    add_tree(update, false);
    const std::vector<ModInfo> mods = list_mods(install_dir);
    for (auto m = mods.rbegin(); m != mods.rend(); ++m)   // bottom of the list first: the top one wins
        if (m->enabled) add_tree(inst / "mods" / fs::path(widen(m->name)), true);

    // Drop what the previous compose placed. No manifest = an older install that
    // copied files in: that folder is launcher-owned, so clear it.
    if (FILE* f = _wfopen(manifest.c_str(), L"rb")) {
        char line[2048];
        while (fgets(line, sizeof line, f)) {
            std::string l = line;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            if (!l.empty()) fs::remove(game / fs::path(widen(l)), ec);
        }
        fclose(f);
    } else {
        fs::remove_all(game, ec);
    }

    fs::create_directories(game, ec);
    FILE* mf = _wfopen(manifest.c_str(), L"wb");
    if (!mf) return "cannot write " + narrow(manifest.wstring());
    int linked = 0, copied = 0;
    for (const auto& [key, rs] : want) {
        const fs::path dst = game / rs.first;
        fs::create_directories(dst.parent_path(), ec);
        fs::remove(dst, ec);
        if (CreateHardLinkW(dst.c_str(), rs.second.c_str(), nullptr)) linked++;
        else if (CopyFileW(rs.second.c_str(), dst.c_str(), FALSE)) copied++;
        else { fclose(mf); return "cannot place " + narrow(rs.first.wstring()); }
        fprintf(mf, "%s\n", narrow(rs.first.generic_wstring()).c_str());
    }
    fclose(mf);
    if (summary)
        *summary = std::to_string(want.size()) + " update files (" + std::to_string(from_mods) + " from mods), " +
                   std::to_string(linked) + " linked, " + std::to_string(copied) + " copied";
    return "";
}

// ---- rosters (#257) ------------------------------------------------------------
// NHL keeps rosters as saves BLUS3154002xx (DETAIL "Rosters", SUB_TITLE = the roster's
// name, one SYS-DATA) and auto-loads one at boot, right after the profile (RPCS3 log:
// ListAutoLoad prefix "BLUS3154002"). A mod's roster is that SYS-DATA, as RPCS3 / save
// tools export it, in mods\<name>\roster\. Switching moves the roster saves now in the
// save folder to user\rosters\<source>\ and brings the chosen source's back -- or, the
// first time, builds the save from the mod -- so the player's own rosters and any
// in-game edits to a mod roster are kept per source. user\rosters\active.txt names the
// source the save folder holds ("" = the player's own / the game's built-in roster).

std::string active_roster(const std::wstring& install_dir)
{
    Ini ini;
    ini.load((fs::path(install_dir) / "nhl_settings.ini").wstring());
    return ini.get("launcher", "roster");
}

void set_active_roster(const std::wstring& install_dir, const std::string& mod)
{
    const fs::path ini_path = fs::path(install_dir) / "nhl_settings.ini";
    Ini ini;
    if (!ini.load(ini_path.wstring()) && fs::exists(ini_path)) return;   // never overwrite what could not be read
    ini.set("launcher", "roster", mod);
    ini.save(ini_path.wstring(), "; NHL Legacy native settings. Written by NHLLauncher; hand edits are kept.");
}

namespace {
fs::path user_dir(const fs::path& inst)
{
    Ini ini;
    ini.load((inst / "nhl_settings.ini").wstring());
    fs::path u = widen(ini.get("paths", "user", "user"));
    return u.is_relative() ? inst / u : u;
}

std::string read_text(const fs::path& f)
{
    std::string s;
    if (FILE* fp = _wfopen(f.c_str(), L"rb")) {
        char b[512]; size_t n;
        while ((n = fread(b, 1, sizeof b, fp)) > 0) s.append(b, n);
        fclose(fp);
    }
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

bool write_text(const fs::path& f, const std::string& s)
{
    FILE* fp = _wfopen(f.c_str(), L"wb");
    if (!fp) return false;
    fwrite(s.data(), 1, s.size(), fp);
    return fclose(fp) == 0;
}

std::wstring roster_key(const std::string& src) { return src.empty() ? L"_own" : L"mod-" + widen(src); }

// Builds savedata\BLUS315400200 from mods\<mod>\roster: SYS-DATA, a PARAM.SFO like the
// game writes (ps3_psf_build), the mod's ICON0.PNG or the profile save's.
std::string build_roster_save(const fs::path& mod_roster, const fs::path& save, const std::string& name)
{
    std::error_code ec;
    const fs::path dir = save / L"BLUS315400200";
    fs::create_directories(dir, ec);
    if (!CopyFileW((mod_roster / L"SYS-DATA").c_str(), (dir / L"SYS-DATA").c_str(), FALSE))
        return "cannot copy the roster's SYS-DATA";
    uint8_t sfo[4096];
    const size_t n = ps3_psf_build(sfo, sizeof sfo, "BLUS315400200", "NHL\xC2\xAE Legacy Edition", name.c_str(),
                                   "Rosters", 0, "000");
    FILE* fp = n ? _wfopen((dir / L"PARAM.SFO").c_str(), L"wb") : nullptr;
    if (!fp) return "cannot write the roster's PARAM.SFO";
    fwrite(sfo, 1, n, fp);
    fclose(fp);
    for (const fs::path& icon : { mod_roster / L"ICON0.PNG", save / L"BLUS315400000" / L"ICON0.PNG" })
        if (fs::is_regular_file(icon, ec)) { CopyFileW(icon.c_str(), (dir / L"ICON0.PNG").c_str(), FALSE); break; }
    return "";
}
}  // namespace

std::string apply_roster(const std::wstring& install_dir, std::string* summary)
{
    const fs::path inst = install_dir, user = user_dir(inst);
    const fs::path save = user / "gamedata" / "dev_hdd0" / "home" / "00000001" / "savedata", stash = user / "rosters";
    std::error_code ec;
    fs::create_directories(save, ec);
    fs::create_directories(stash, ec);

    std::string want = active_roster(install_dir);
    const fs::path mod_roster = inst / "mods" / fs::path(widen(want)) / "roster";
    if (!want.empty() && !fs::is_regular_file(mod_roster / "SYS-DATA", ec)) {
        if (summary) *summary = "roster of '" + want + "' not found -- using your own";
        want.clear();
    }
    // the mod's roster file as last placed: a changed file (an updated mod) is rebuilt
    const std::string want_src = want.empty() ? "" : sha1_file(mod_roster / "SYS-DATA");
    const std::string cur = read_text(stash / "active.txt");
    if (cur == want) {
        if (want.empty() || read_text(stash / "active-src.txt") == want_src) return "";
        fs::remove_all(save / L"BLUS315400200", ec);
        if (std::string e = build_roster_save(mod_roster, save, want); !e.empty()) return e;
        write_text(stash / "active-src.txt", want_src);
        if (summary) *summary = "roster '" + want + "' updated from the mod";
        return "";
    }

    // 1. the roster saves now in place -> user\rosters\<current source>
    const fs::path out = stash / roster_key(cur);
    if (fs::exists(out, ec)) fs::rename(out, fs::path(out.wstring() + L".old-" + std::to_wstring(GetTickCount64())), ec);
    fs::create_directories(out, ec);
    for (auto it = fs::directory_iterator(save, ec); it != fs::directory_iterator(); it.increment(ec)) {
        const std::wstring n = it->path().filename().wstring();
        if (!it->is_directory(ec) || n.rfind(L"BLUS3154002", 0) != 0) continue;
        fs::rename(it->path(), out / n, ec);
        if (ec) return "cannot move roster save " + narrow(n) + " (is the game still running?)";
    }
    write_text(out / "source.txt", read_text(stash / "active-src.txt"));

    // 2. the wanted source: its stashed saves (with any in-game edits), else a new save from the mod
    const fs::path in = stash / roster_key(want);
    const bool reuse = fs::is_directory(in, ec) && (want.empty() || read_text(in / "source.txt") == want_src);
    if (reuse) {
        for (auto it = fs::directory_iterator(in, ec); it != fs::directory_iterator(); it.increment(ec))
            if (it->is_directory(ec)) fs::rename(it->path(), save / it->path().filename(), ec);
        fs::remove_all(in, ec);
    } else if (!want.empty()) {
        if (fs::exists(in, ec)) fs::rename(in, fs::path(in.wstring() + L".old-" + std::to_wstring(GetTickCount64())), ec);
        if (std::string e = build_roster_save(mod_roster, save, want); !e.empty()) return e;
    }
    write_text(stash / "active.txt", want);
    write_text(stash / "active-src.txt", want_src);
    if (summary && summary->empty()) *summary = want.empty() ? "using your own roster" : "using the roster of '" + want + "'";
    return "";
}

void run_install(const InstallConfig& cfg, InstallProgress& p)
{
    const fs::path inst = cfg.install_dir, data = inst / "data";
    const bool iso = is_iso(cfg.game_src);
    p.steps = iso ? 5 : 4;
    try {
        std::string info, err = check_game_source(cfg.game_src, &info);
        if (err.empty()) err = check_update_source(cfg.update_src, nullptr);
        if (!err.empty()) throw std::runtime_error(err);
        p.add_log("Game: " + info);
        int step = 0;
        fs::path disc_root;
        if (iso) {
            p.step = ++step; p.frac = 0;
            step_extract_iso(cfg.game_src, inst / "game", p);
            disc_root = inst / "game";
        } else {
            disc_root = disc_root_of(cfg.game_src);
        }
        const fs::path usrdir = disc_root / "PS3_GAME" / "USRDIR";
        p.step = ++step; p.frac = 0;
        step_cache(usrdir, data / "hdd1" / "cache" / kTitleId, p);
        p.step = ++step; p.frac = 0;
        step_movies(usrdir, data / "movies", p);
        p.step = ++step; p.frac = 0;
        step_update(cfg, data, inst / "mods", p);
        p.step = ++step; p.frac = 0;
        p.set_label("Writing settings");
        std::error_code ec;
        for (const char* d : { "edat_cache", "hdd1" }) fs::create_directories(data / d, ec);
        fs::create_directories(inst / "user", ec);
        fs::create_directories(inst / "logs", ec);
        step_settings(cfg, disc_root, p);
        std::string mods_summary;
        if (std::string e = compose_hdd0(cfg.install_dir, &mods_summary); !e.empty()) throw std::runtime_error(e);
        p.add_log("Game update folder assembled: " + mods_summary);
        p.frac = 1;
        p.set_label("Done");
        p.add_log("Install complete.");
    } catch (const Cancelled&) {
        p.add_log("Cancelled. Run the install again to continue where it stopped.");
        p.failed = true;
    } catch (const std::exception& e) {
        p.add_log(std::string("ERROR: ") + e.what());
        p.failed = true;
    }
    p.done = true;
}
