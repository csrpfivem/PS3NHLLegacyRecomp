// #321 Audio packs (see audio_pack.h).
//
// Profile save layout (SYS-DATA of savedata\BLUS315400000, "PS3UserProfileF"):
//   +0x00 magic "PS3UserProfileF\0", +0x10 u32 BE CRC-32 (zlib) of everything from 0x1C,
//   then sections from 0x1C to the end: { u32 id, u32 ?, u32 size, u32 crc } + size bytes,
//   crc = CRC-32/BZIP2 (MSB-first 04C11DB7, init/xorout ~0) of the section's data.
// Section 5 is the Custom Audio table alone (11-11 diff of a Custom Audio change: only
// its bytes and the two CRCs moved). Each assigned slot holds a content id
// "NHL" + type + u64 path hash + 0, then 00 02.
#include "audio_pack.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <vector>

#include "ini.h"
#include "installer.h"
#include "miniz.h"

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kAudioSection = 5;
constexpr uint32_t kAudioSize = 0x10428;
const char kMagic[] = "PS3UserProfileF";

fs::path user_dir(const fs::path& inst)
{
    Ini ini;
    ini.load((inst / "nhl_settings.ini").wstring());
    fs::path u = widen(ini.get("paths", "user", "user"));
    return u.is_relative() ? inst / u : u;
}

fs::path profile_path(const fs::path& inst)
{
    return user_dir(inst) / "gamedata" / "dev_hdd0" / "home" / "00000001" / "savedata" / "BLUS315400000" / "SYS-DATA";
}

fs::path pending_path(const fs::path& inst) { return user_dir(inst) / "audio_pending.bin"; }

std::vector<uint8_t> read_all(const fs::path& p)
{
    std::vector<uint8_t> out;
    if (FILE* f = _wfopen(p.c_str(), L"rb")) {
        _fseeki64(f, 0, SEEK_END);
        const long long n = _ftelli64(f);
        _fseeki64(f, 0, SEEK_SET);
        if (n > 0 && n < (256ll << 20)) {
            out.resize((size_t)n);
            if (fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
        }
        fclose(f);
    }
    return out;
}

// Written beside and swapped in: the profile is never left half-written.
bool write_all(const fs::path& p, const void* d, size_t n)
{
    const fs::path tmp = p.wstring() + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    const bool ok = fwrite(d, 1, n, f) == n;
    if (fclose(f) != 0 || !ok) { _wremove(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) { _wremove(tmp.c_str()); return false; }
    return true;
}

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
void put_be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

uint32_t crc_bzip2(const uint8_t* d, size_t n)
{
    static uint32_t tbl[256];
    if (!tbl[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t r = i << 24;
            for (int k = 0; k < 8; k++) r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : r << 1;
            tbl[i] = r;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = (c << 8) ^ tbl[((c >> 24) ^ d[i]) & 0xFF];
    return ~c;
}

// Offset of the audio section's 16-byte header, or an error. Checks the whole chain.
std::string find_audio_section(const std::vector<uint8_t>& b, size_t* hdr)
{
    if (b.size() < 0x1C + 16 || memcmp(b.data(), kMagic, sizeof kMagic) != 0) return "the profile save is not a PS3UserProfile";
    *hdr = 0;
    for (size_t off = 0x1C; off < b.size();) {
        if (off + 16 > b.size()) return "the profile save is damaged (section chain)";
        const uint32_t id = be32(&b[off]), size = be32(&b[off + 8]);
        if (size > b.size() - off - 16) return "the profile save is damaged (section size)";
        if (id == kAudioSection) {
            if (size != kAudioSize) return "the profile save has an unknown Custom Audio layout";
            *hdr = off;
        }
        off += 16 + size;
    }
    return *hdr ? "" : "the profile save has no Custom Audio table";
}

int assigned_slots(const uint8_t* t)
{
    int n = 0;
    for (size_t i = 0; i + 4 <= kAudioSize; i++)
        if (t[i] == 'N' && t[i + 1] == 'H' && t[i + 2] == 'L' && t[i + 3] <= 4) n++;
    return n;
}

// Puts table into the profile at p (backup first). "" or an error.
std::string write_table(const fs::path& inst, const fs::path& p, const uint8_t* table)
{
    std::vector<uint8_t> b = read_all(p);
    if (b.empty()) return "cannot read the profile save";
    size_t hdr = 0;
    if (std::string err = find_audio_section(b, &hdr); !err.empty()) return err;
    if (memcmp(&b[hdr + 16], table, kAudioSize) == 0) return "";   // already this setup

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[48];
    swprintf(stamp, 48, L"SYS-DATA_%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const fs::path bak_dir = user_dir(inst) / "audio backups";
    std::error_code ec;
    fs::create_directories(bak_dir, ec);
    if (!CopyFileW(p.c_str(), (bak_dir / stamp).c_str(), FALSE)) return "cannot back up the profile save";

    memcpy(&b[hdr + 16], table, kAudioSize);
    put_be32(&b[hdr + 12], crc_bzip2(&b[hdr + 16], kAudioSize));
    put_be32(&b[0x10], (uint32_t)mz_crc32(MZ_CRC32_INIT, &b[0x1C], b.size() - 0x1C));
    if (!write_all(p, b.data(), b.size())) return "cannot write the profile save";
    return "";
}

// The game's content ids (nhl_usermusic.cpp scan_library): FNV-1a 64 of the lower-cased
// UTF-8 path inside music\ -- "List\song.mp3" for a playlist's songs (its own folder
// only), "song.mp3" for loose ones, "<list>\List" for the playlist itself.
uint64_t content_hash(const std::wstring& rel)
{
    std::wstring l = rel;
    for (auto& c : l) c = (wchar_t)towlower(c);
    const std::string u = narrow(l);
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : u) { h ^= c; h *= 1099511628211ull; }
    return h ? h : 1;
}

bool is_song_name(const fs::path& p)
{
    std::wstring e = p.extension().wstring();
    for (auto& c : e) c = (wchar_t)towlower(c);
    for (const wchar_t* ok : { L".mp3", L".m4a", L".aac", L".mp4", L".wav", L".wma", L".flac" })
        if (e == ok) return true;
    return false;
}

// Assigned slots in the table: offset of each content id and its hash.
std::vector<std::pair<size_t, uint64_t>> table_slots(const uint8_t* t)
{
    std::vector<std::pair<size_t, uint64_t>> out;
    for (size_t i = 0; i + 16 <= kAudioSize; i++)
        if (t[i] == 'N' && t[i + 1] == 'H' && t[i + 2] == 'L' && t[i + 3] >= 1 && t[i + 3] <= 4 && !t[i + 12] && !t[i + 13] &&
            !t[i + 14] && !t[i + 15]) {
            uint64_t h = 0;
            for (int k = 0; k < 8; k++) h = h << 8 | t[i + 4 + k];
            out.push_back({ i, h });
            i += 15;
        }
    return out;
}

// Content ids a folder of music\ answers to (L"" = loose songs).
std::vector<uint64_t> folder_hashes(const fs::path& music, const std::wstring& folder)
{
    std::vector<uint64_t> out;
    std::error_code ec;
    const fs::path dir = folder.empty() ? music : music / folder;
    if (!folder.empty()) out.push_back(content_hash(L"<list>\\" + folder));
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && is_song_name(it->path()))
            out.push_back(content_hash(folder.empty() ? it->path().filename().wstring()
                                                      : folder + L"\\" + it->path().filename().wstring()));
    return out;
}

bool read_table(const fs::path& inst, std::vector<uint8_t>* table, std::string* note)
{
    const std::vector<uint8_t> b = read_all(profile_path(inst));
    size_t hdr = 0;
    if (b.empty()) { *note = "no profile save yet"; return false; }
    if (std::string err = find_audio_section(b, &hdr); !err.empty()) { *note = err; return false; }
    table->assign(b.begin() + hdr + 16, b.begin() + hdr + 16 + kAudioSize);
    return true;
}

bool zip_name_safe(const std::string& name)
{
    if (name.empty() || name[0] == '/' || name.find(':') != std::string::npos) return false;
    for (size_t a = 0; a <= name.size();) {
        size_t e = name.find('/', a);
        if (e == std::string::npos) e = name.size();
        if (name.compare(a, e - a, "..") == 0 && e - a == 2) return false;
        a = e + 1;
    }
    return true;
}

}  // namespace

AudioPackScan scan_audio_pack(const std::wstring& install_dir)
{
    const fs::path inst(install_dir), music = inst / "music";
    AudioPackScan s;
    std::vector<uint8_t> table;
    s.has_table = read_table(inst, &table, &s.table_note);
    const auto slots = s.has_table ? table_slots(table.data()) : std::vector<std::pair<size_t, uint64_t>>{};
    s.slots = (int)slots.size();
    std::vector<bool> found(slots.size(), false);

    std::error_code ec;
    AudioPackItem loose;
    loose.label = "Loose songs in the music folder";
    for (fs::directory_iterator it(music, ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring n = it->path().filename().wstring();
        if (n.rfind(L".", 0) == 0) continue;
        if (it->is_directory(ec)) {
            AudioPackItem i;
            i.folder = n;
            i.label = narrow(n);
            std::error_code e2;
            for (fs::recursive_directory_iterator r(it->path(), e2), e3; !e2 && r != e3; r.increment(e2))
                if (r->is_regular_file(e2)) { i.bytes += r->file_size(e2); i.songs += is_song_name(r->path()); }
            s.items.push_back(std::move(i));
        } else if (it->is_regular_file(ec)) {
            loose.bytes += it->file_size(ec);
            loose.songs += is_song_name(it->path());
        }
    }
    std::sort(s.items.begin(), s.items.end(), [](const AudioPackItem& a, const AudioPackItem& b) {
        return _wcsicmp(a.folder.c_str(), b.folder.c_str()) < 0;
    });
    if (loose.songs) s.items.push_back(std::move(loose));
    for (AudioPackItem& i : s.items) {
        const std::vector<uint64_t> hs = folder_hashes(music, i.folder);
        for (size_t k = 0; k < slots.size(); k++)
            if (std::find(hs.begin(), hs.end(), slots[k].second) != hs.end()) { i.slots++; found[k] = true; }
    }
    s.slots_elsewhere = (int)std::count(found.begin(), found.end(), false);
    return s;
}

std::string export_audio_pack(const std::wstring& install_dir, const std::vector<std::wstring>& folders, bool with_table,
                              std::wstring* out, std::string* summary)
{
    const fs::path inst(install_dir), music = inst / "music";
    std::error_code ec;
    std::vector<std::pair<std::string, fs::path>> files;   // zip name, source
    std::vector<uint64_t> keep;                             // content ids the pack carries
    uint64_t bytes = 0;
    for (const std::wstring& folder : folders) {
        const fs::path dir = folder.empty() ? music : music / folder;
        if (!fs::is_directory(dir, ec)) continue;
        auto add = [&](const fs::path& p) {
            if (p.filename().wstring().rfind(L".", 0) == 0) return;
            std::string rel = narrow(fs::relative(p, music, ec).wstring());
            std::replace(rel.begin(), rel.end(), '\\', '/');
            files.push_back({ "music/" + rel, p });
            bytes += fs::file_size(p, ec);
        };
        if (folder.empty()) {
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_regular_file(ec)) add(it->path());
        } else {
            for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_regular_file(ec)) add(it->path());
        }
        const std::vector<uint64_t> hs = folder_hashes(music, folder);
        keep.insert(keep.end(), hs.begin(), hs.end());
    }
    std::sort(files.begin(), files.end());

    std::vector<uint8_t> table;
    std::string table_note = "left out";
    int dropped = 0;
    if (with_table && read_table(inst, &table, &table_note)) {
        // a slot whose song is not in the pack would point at nothing on the other PC:
        // unassign it (all zero, as the game leaves a slot nobody set)
        for (const auto& [off, h] : table_slots(table.data()))
            if (std::find(keep.begin(), keep.end(), h) == keep.end()) {
                memset(&table[off], 0, std::min<size_t>(18, kAudioSize - off));
                dropped++;
            }
        if (assigned_slots(table.data()) == 0) { table.clear(); table_note = "none of its songs are in the pack"; }
    } else {
        table.clear();
    }
    if (files.empty() && table.empty()) return "nothing chosen to put in the pack";

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf(name, 64, L"Audio pack %04u-%02u-%02u %02u%02u%02u.nhlaudio", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond);
    const fs::path dir = inst / "audio packs", path = dir / name;
    fs::create_directories(dir, ec);

    std::string info = "NHL Legacy audio pack 1\r\n";
    char line[160];
    snprintf(line, sizeof line, "made %04u-%02u-%02u %02u:%02u\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    info += line;
    if (!table.empty()) { snprintf(line, sizeof line, "Custom Audio: %d slots assigned\r\n", assigned_slots(table.data())); info += line; }
    else info += "Custom Audio: not included (" + table_note + ")\r\n";
    info += "\r\nDrop this file on NHLLauncher to install it.\r\n\r\nFiles:\r\n";
    for (const auto& f : files) info += "  " + f.first.substr(6) + "\r\n";

    FILE* fp = _wfopen(path.c_str(), L"wb");
    if (!fp) return "cannot create " + narrow(path.wstring());
    mz_zip_archive z = {};
    std::string err;
    if (!mz_zip_writer_init_cfile(&z, fp, MZ_ZIP_FLAG_WRITE_ZIP64)) err = "zip init failed";
    if (err.empty() && !mz_zip_writer_add_mem(&z, "nhlaudio.txt", info.data(), info.size(), MZ_DEFAULT_LEVEL)) err = "zip: nhlaudio.txt";
    if (err.empty() && !table.empty() && !mz_zip_writer_add_mem(&z, "customaudio.bin", table.data(), table.size(), MZ_DEFAULT_LEVEL))
        err = "zip: customaudio.bin";
    for (const auto& f : files) {
        if (!err.empty()) break;
        FILE* in = _wfopen(f.second.c_str(), L"rb");
        if (!in) { err = "cannot read " + f.first; break; }
        _fseeki64(in, 0, SEEK_END);
        const long long n = _ftelli64(in);
        _fseeki64(in, 0, SEEK_SET);
        // songs and covers are compressed already: stored, so export is disk speed
        if (!mz_zip_writer_add_cfile(&z, f.first.c_str(), in, (mz_uint64)n, nullptr, nullptr, 0, MZ_NO_COMPRESSION, nullptr, 0,
                                     nullptr, 0))
            err = "zip: cannot add " + f.first;
        fclose(in);
    }
    const bool ok = err.empty() && mz_zip_writer_finalize_archive(&z);
    mz_zip_writer_end(&z);
    fclose(fp);
    if (!ok) {
        fs::remove(path, ec);
        return err.empty() ? "zip finalize failed" : err;
    }
    if (out) *out = path.wstring();
    if (summary) {
        snprintf(line, sizeof line, "%zu files (%.0f MB)", files.size(), bytes / 1048576.0);
        *summary = "Saved " + narrow(path.filename().wstring()) + ": " + line;
        if (table.empty()) *summary += ", no Custom Audio choices (" + table_note + ")";
        else {
            snprintf(line, sizeof line, ", Custom Audio choices for %d slot%s", assigned_slots(table.data()),
                     assigned_slots(table.data()) == 1 ? "" : "s");
            *summary += line;
            if (dropped) { snprintf(line, sizeof line, " (%d left out: their songs are not in the pack)", dropped); *summary += line; }
        }
        *summary += ".";
    }
    return "";
}

bool is_audio_pack(const std::wstring& path)
{
    fs::path p(path);
    std::wstring e = p.extension().wstring();
    for (auto& c : e) c = (wchar_t)towlower(c);
    if (e == L".nhlaudio") return true;
    if (e != L".zip") return false;
    mz_zip_archive z = {};
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    const uint64_t size = (uint64_t)_ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    bool yes = false;
    if (mz_zip_reader_init_cfile(&z, f, size, 0)) {
        yes = mz_zip_reader_locate_file(&z, "nhlaudio.txt", nullptr, 0) >= 0 ||
              mz_zip_reader_locate_file(&z, "customaudio.bin", nullptr, 0) >= 0;
        mz_zip_reader_end(&z);
    }
    fclose(f);
    return yes;
}

std::string import_audio_pack(const std::wstring& install_dir, const std::wstring& pack, std::string* summary)
{
    const fs::path inst(install_dir), music = inst / "music";
    FILE* f = _wfopen(pack.c_str(), L"rb");
    if (!f) return "cannot open the pack";
    _fseeki64(f, 0, SEEK_END);
    const uint64_t size = (uint64_t)_ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    mz_zip_archive z = {};
    if (!mz_zip_reader_init_cfile(&z, f, size, 0)) { fclose(f); return "the pack cannot be read (damaged?)"; }

    std::string err;
    std::vector<uint8_t> table;
    int songs = 0;
    const mz_uint n = mz_zip_reader_get_num_files(&z);
    mz_zip_archive_file_stat st;
    for (mz_uint i = 0; i < n && err.empty(); i++) {
        if (!mz_zip_reader_file_stat(&z, i, &st) || st.m_is_directory) continue;
        if (st.m_is_encrypted) { err = "the pack is password-protected"; break; }
        std::string name = st.m_filename;
        std::replace(name.begin(), name.end(), '\\', '/');
        if (name == "customaudio.bin") {
            if (st.m_uncomp_size != kAudioSize) { err = "the pack's Custom Audio table has the wrong size"; break; }
            table.resize(kAudioSize);
            if (!mz_zip_reader_extract_to_mem(&z, i, table.data(), table.size(), 0)) err = "cannot read customaudio.bin";
            continue;
        }
        if (name.rfind("music/", 0) != 0 || name.back() == '/' || !zip_name_safe(name)) continue;
        const fs::path dst = music / fs::path(widen(name.substr(6)));
        std::error_code ec;
        fs::create_directories(dst.parent_path(), ec);
        const fs::path tmp = dst.wstring() + L".part";
        FILE* o = _wfopen(tmp.c_str(), L"wb");
        if (!o) { err = "cannot write " + name; break; }
        const bool ok = mz_zip_reader_extract_to_cfile(&z, i, o, 0);
        if (fclose(o) != 0 || !ok || !MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            _wremove(tmp.c_str());
            err = "cannot extract " + name;
            break;
        }
        songs++;
    }
    mz_zip_reader_end(&z);
    fclose(f);
    if (!err.empty()) return err;
    if (!songs && table.empty()) return "this is not an audio pack (no music and no Custom Audio setup)";

    std::string what;
    char line[96];
    snprintf(line, sizeof line, "%d file%s added to your music folder", songs, songs == 1 ? "" : "s");
    what = line;
    if (!table.empty()) {
        const fs::path prof = profile_path(inst);
        std::error_code ec;
        if (fs::exists(prof, ec)) {
            if (std::string e = write_table(inst, prof, table.data()); !e.empty()) return what + ", but Custom Audio was not set: " + e;
            fs::remove(pending_path(inst), ec);
            snprintf(line, sizeof line, "; Custom Audio set (%d slots)", assigned_slots(table.data()));
            what += line;
        } else {
            fs::create_directories(pending_path(inst).parent_path(), ec);
            if (!write_all(pending_path(inst), table.data(), table.size())) return what + ", but the Custom Audio setup could not be saved";
            what += "; Custom Audio is set when your profile exists (after the first game start)";
        }
    }
    if (summary) *summary = "Audio pack installed: " + what + ".";
    return "";
}

std::string apply_pending_audio(const std::wstring& install_dir, std::string* summary)
{
    const fs::path inst(install_dir), pend = pending_path(inst), prof = profile_path(inst);
    std::error_code ec;
    if (!fs::exists(pend, ec) || !fs::exists(prof, ec)) return "";
    const std::vector<uint8_t> t = read_all(pend);
    if (t.size() != kAudioSize) { fs::remove(pend, ec); return "the waiting Custom Audio setup was damaged"; }
    if (std::string err = write_table(inst, prof, t.data()); !err.empty()) return err;
    fs::remove(pend, ec);
    if (summary) *summary = "Custom Audio setup from the audio pack applied.";
    return "";
}
