// Mod install from a drop on the launcher (or its Install mod button): see
// import_mod() in installer.h.
//
// A mod pack is usually laid out like RPCS3's hdd0, possibly several variants side by
// side plus a roster, e.g.
//   26-27Extras\Main Release\BLUS31540\USRDIR\...
//   26-27Extras\Bright Ice (Upscaled Faceoff Circles)\BLUS31540\USRDIR\...
//   26-27Extras\Roster\SYS-DATA
// which installs as mods\26-27Extras (USRDIR + roster\SYS-DATA) and
// mods\26-27Extras - Bright Ice (Upscaled Faceoff Circles).
#include "installer.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <vector>

#include "ini.h"
#include "miniz.h"

namespace fs = std::filesystem;

namespace {

bool ieq(const std::wstring& a, const wchar_t* b) { return _wcsicmp(a.c_str(), b) == 0; }

// Explorer / macOS leftovers are not game files.
bool junk(const fs::path& p)
{
    const std::wstring n = p.filename().wstring();
    return ieq(n, L"desktop.ini") || ieq(n, L"Thumbs.db") || ieq(n, L".DS_Store") || n.rfind(L"._", 0) == 0;
}

// A mod name becomes a folder name.
std::string clean_name(std::string s)
{
    for (char& c : s)
        if (strchr("<>:\"/\\|?*", c) || (unsigned char)c < 32) c = '_';
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '.')) s.erase(s.begin());
    return s.empty() ? "Mod" : s;
}

struct Progress {
    std::atomic<double>* frac;
    const std::atomic<bool>* cancel;
    uint64_t total = 0, done = 0;
    void add(uint64_t n)
    {
        done += n;
        if (frac && total) *frac = std::min(1.0, (double)done / (double)total);
        if (cancel && *cancel) throw std::runtime_error("cancelled");
    }
};

void copy_one(const fs::path& from, const fs::path& to)
{
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    if (!CopyFileW(from.c_str(), to.c_str(), FALSE))
        throw std::runtime_error("cannot copy " + narrow(from.filename().wstring()));
}

// .zip -> dir. Entries whose paths would leave dir are skipped.
void unzip(const fs::path& zip, const fs::path& dir, Progress& pr)
{
    FILE* f = _wfopen(zip.c_str(), L"rb");
    if (!f) throw std::runtime_error("cannot open the zip");
    _fseeki64(f, 0, SEEK_END);
    const uint64_t size = (uint64_t)_ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    mz_zip_archive z = {};
    if (!mz_zip_reader_init_cfile(&z, f, size, 0)) {
        fclose(f);
        throw std::runtime_error("this zip cannot be read (damaged, or not a .zip)");
    }
    std::string err;
    const mz_uint n = mz_zip_reader_get_num_files(&z);
    mz_zip_archive_file_stat st;
    for (mz_uint i = 0; i < n; i++)
        if (mz_zip_reader_file_stat(&z, i, &st) && !st.m_is_directory) pr.total += st.m_uncomp_size;
    try {
        for (mz_uint i = 0; i < n && err.empty(); i++) {
            if (!mz_zip_reader_file_stat(&z, i, &st) || st.m_is_directory) continue;
            if (st.m_is_encrypted) { err = "the zip is password-protected; extract it first, then drop the folder"; break; }
            std::string name = st.m_filename;
            std::replace(name.begin(), name.end(), '\\', '/');
            if (!name.empty() && name.back() == '/') continue;   // a folder entry written with '\' (Windows zippers)
            bool safe = name.find(':') == std::string::npos && !name.empty() && name[0] != '/';
            for (size_t a = 0; safe && a <= name.size();) {
                size_t b = name.find('/', a);
                if (b == std::string::npos) b = name.size();
                const std::string c = name.substr(a, b - a);
                safe = c != "..";
                a = b + 1;
            }
            if (!safe) continue;
            const fs::path out = dir / fs::path(widen(name));
            std::error_code ec;
            fs::create_directories(out.parent_path(), ec);
            FILE* o = _wfopen(out.c_str(), L"wb");
            if (!o) { err = "cannot write " + name; break; }
            const bool ok = mz_zip_reader_extract_to_cfile(&z, i, o, 0);
            if (fclose(o) != 0 || !ok) { err = "cannot extract " + name + " (unsupported compression? try extracting it first)"; break; }
            pr.add(st.m_uncomp_size);
        }
    } catch (...) {
        mz_zip_reader_end(&z);
        fclose(f);
        throw;
    }
    mz_zip_reader_end(&z);
    fclose(f);
    if (!err.empty()) throw std::runtime_error(err);
}

// Every USRDIR (not looking inside them) and every roster SYS-DATA outside them.
void scan(const fs::path& root, std::vector<fs::path>& usrdirs, std::vector<fs::path>& rosters)
{
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const std::wstring n = it->path().filename().wstring();
        if (it->is_directory(ec)) {
            if (ieq(n, L"__MACOSX")) it.disable_recursion_pending();
            else if (ieq(n, L"USRDIR")) { usrdirs.push_back(it->path()); it.disable_recursion_pending(); }
        } else if (ieq(n, L"SYS-DATA")) {
            rosters.push_back(it->path());
        }
    }
}

// Which variant a USRDIR is: the folder above it, minus BLUS31540 ("" = the pack root).
std::string variant_of(const fs::path& usrdir, const fs::path& root)
{
    std::error_code ec;
    fs::path rel = fs::relative(usrdir.parent_path(), root, ec);
    while (!rel.empty() && rel != "." && _wcsnicmp(rel.filename().c_str(), L"BLUS31540", 9) == 0) rel = rel.parent_path();
    if (rel.empty() || rel == ".") return "";
    return narrow(rel.filename().wstring());
}

// A loose file goes where the game has a file of that name (unique), else "".
fs::path game_path_of(const fs::path& inst, const std::wstring& file_name, std::string* err)
{
    std::vector<fs::path> roots = { inst / "data" / "update" / "USRDIR", inst / "game" / "PS3_GAME" / "USRDIR" };
    Ini ini;
    ini.load((inst / "nhl_settings.ini").wstring());
    const fs::path src = widen(ini.get("launcher", "game_source"));
    std::error_code ec;
    if (!src.empty() && fs::is_directory(src / "PS3_GAME" / "USRDIR", ec)) roots.push_back(src / "PS3_GAME" / "USRDIR");
    std::set<std::wstring> keys;
    fs::path found;
    for (const fs::path& r : roots) {
        if (!fs::is_directory(r, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(r, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec) || !ieq(it->path().filename().wstring(), file_name.c_str())) continue;
            const fs::path rel = fs::relative(it->path(), r, ec);
            std::wstring key = rel.generic_wstring();
            CharLowerBuffW(key.data(), (DWORD)key.size());
            if (keys.insert(key).second) found = rel;
        }
    }
    if (keys.size() == 1) return found;
    const std::string n = narrow(file_name);
    *err = keys.empty() ? "no game file is called " + n + ", so it is unclear where it goes. Put it in a folder laid "
                          "out like the game (USRDIR\\...) and drop that folder."
                        : n + " matches " + std::to_string(keys.size()) + " game files. Put it in a folder laid out "
                          "like the game (USRDIR\\...) and drop that folder.";
    return {};
}

struct NewMod {
    std::string name;
    fs::path usrdir;     // source USRDIR ("" = roster only / loose file)
    fs::path loose, loose_rel;   // a single file and where it goes under USRDIR
    fs::path roster;     // a SYS-DATA, or ""
    uint64_t bytes = 0;
    int files = 0;
};

// Files of a USRDIR to install: everything but the official update's own files.
std::vector<fs::path> mod_files(const fs::path& usrdir, uint64_t* bytes)
{
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(usrdir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec)) {
            if (ieq(it->path().filename().wstring(), L"__MACOSX")) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec) || junk(it->path())) continue;
        const uint64_t n = it->file_size(ec);
        const std::string rel = "USRDIR/" + narrow(fs::relative(it->path(), usrdir, ec).generic_wstring());
        if (is_official_update_file(rel, n)) continue;
        out.push_back(it->path());
        if (bytes) *bytes += n;
    }
    return out;
}

std::string run(const fs::path& inst, const fs::path& src, Progress& pr, std::string* summary)
{
    std::error_code ec;
    const fs::path mods = inst / "mods", stage = mods / ".incoming";
    fs::remove_all(stage, ec);
    fs::create_directories(stage, ec);
    if (!fs::exists(src, ec)) throw std::runtime_error("it is not there any more");

    std::wstring ext = src.extension().wstring();
    for (wchar_t& c : ext) c = (wchar_t)towlower(c);
    const bool dir = fs::is_directory(src, ec);
    const std::string pack = clean_name(narrow((dir ? src.filename() : src.stem()).wstring()));
    std::vector<NewMod> found;
    fs::path root;

    if (!dir && (ext == L".rar" || ext == L".7z"))
        throw std::runtime_error("extract " + narrow(ext) + " files first (7-Zip or WinRAR), then drop the folder");
    if (dir || ext == L".zip") {
        if (dir) root = src;
        else {
            root = stage / "unzipped";
            pr.total = 0;
            unzip(src, root, pr);   // the copy below is a quick move for a zip, so this is most of the work
        }
        std::vector<fs::path> usrdirs, rosters;
        scan(root, usrdirs, rosters);
        for (const fs::path& u : usrdirs) {
            NewMod m;
            m.usrdir = u;
            m.name = variant_of(u, root);
            mod_files(u, &m.bytes);
            if (m.bytes) found.push_back(m);
        }
        // naming: one mod = the pack's name; several = the biggest (or the one at the
        // pack root) gets it, the others "<pack> - <variant>"
        size_t main = 0;
        for (size_t i = 0; i < found.size(); i++) {
            if (found[i].name.empty()) { main = i; break; }
            if (found[i].bytes > found[main].bytes) main = i;
        }
        std::set<std::string> used;
        for (size_t i = 0; i < found.size(); i++) {
            std::string n = i == main ? pack : pack + " - " + (found[i].name.empty() ? "Part" : found[i].name);
            n = clean_name(n);
            for (int k = 2; used.count(n); k++) n = clean_name(pack + " - " + std::to_string(k));
            used.insert(n);
            found[i].name = n;
        }
        if (!rosters.empty()) {
            if (found.empty()) { NewMod m; m.name = pack; found.push_back(m); main = 0; }
            found[main].roster = rosters[0];
        }
        // the main mod goes below its variants, so they (e.g. a different ice) win over it
        if (!found.empty() && main + 1 != found.size())
            std::rotate(found.begin() + (ptrdiff_t)main, found.begin() + (ptrdiff_t)main + 1, found.end());
        if (found.empty())
            throw std::runtime_error("no game files found in it. A mod has a USRDIR folder (often BLUS31540\\USRDIR) "
                                     "or a roster's SYS-DATA.");
    } else if (ieq(src.filename().wstring(), L"SYS-DATA")) {
        NewMod m;
        m.name = clean_name(narrow(src.parent_path().filename().wstring()) + " roster");
        m.roster = src;
        found.push_back(m);
    } else {
        std::string err;
        const fs::path rel = game_path_of(inst, src.filename().wstring(), &err);
        if (rel.empty()) throw std::runtime_error(err);
        NewMod m;
        m.name = pack;
        m.loose = src;
        m.loose_rel = rel;
        m.bytes = fs::file_size(src, ec);
        found.push_back(m);
    }

    // Build each mod under .incoming\new\<name>: a zip's files are moved, a folder's copied.
    pr.done = 0;
    pr.total = 0;
    for (const NewMod& m : found) pr.total += m.bytes;
    for (NewMod& m : found) {
        const fs::path out = stage / "new" / fs::path(widen(m.name));
        fs::create_directories(out, ec);
        if (!m.usrdir.empty()) {
            for (const fs::path& f : mod_files(m.usrdir, nullptr)) {
                const fs::path to = out / "USRDIR" / fs::relative(f, m.usrdir, ec);
                const uint64_t n = fs::file_size(f, ec);
                if (!dir) {   // unzipped into .incoming: same disk, just move it
                    fs::create_directories(to.parent_path(), ec);
                    fs::rename(f, to, ec);
                    if (ec) copy_one(f, to);
                } else {
                    copy_one(f, to);
                }
                m.files++;
                pr.add(n);
            }
        }
        if (!m.loose.empty()) {
            copy_one(m.loose, out / "USRDIR" / m.loose_rel);
            m.files++;
            pr.add(m.bytes);
        }
        if (!m.roster.empty()) {
            copy_one(m.roster, out / "roster" / "SYS-DATA");
            const fs::path icon = m.roster.parent_path() / "ICON0.PNG";
            if (fs::is_regular_file(icon, ec)) copy_one(icon, out / "roster" / "ICON0.PNG");
        }
    }

    // Swap them in. A mod of the same name is replaced; it keeps its place and state.
    // New ones go on top (in pack order), so the newest install wins.
    std::vector<std::string> order, added;
    for (const ModInfo& m : list_mods(inst.wstring())) order.push_back(m.name);
    const fs::path ini_path = inst / "nhl_settings.ini";
    std::string text;
    int k = 0;
    for (const NewMod& m : found) {
        const fs::path dst = mods / fs::path(widen(m.name)), built = stage / "new" / fs::path(widen(m.name));
        const bool existed = fs::exists(dst, ec);
        const fs::path old = stage / ("old" + std::to_string(k++));
        if (existed) {
            fs::rename(dst, old, ec);
            if (ec) throw std::runtime_error("cannot replace mods\\" + m.name + " (is a file in it open?)");
        }
        fs::rename(built, dst, ec);
        if (ec) {
            std::error_code ec2;
            if (existed) fs::rename(old, dst, ec2);   // put the old version back
            throw std::runtime_error("cannot move the mod into mods\\" + m.name);
        }
        bool listed = false;
        for (const std::string& o : order) listed |= _stricmp(o.c_str(), m.name.c_str()) == 0;
        if (!listed) added.push_back(m.name);
        if (!existed) set_mod_enabled(inst.wstring(), m.name, true);
        text += (text.empty() ? "" : "; ") + std::string(existed ? "Updated " : "Installed ") + m.name + " (" +
                std::to_string(m.files) + (m.files == 1 ? " file" : " files") + (m.roster.empty() ? "" : ", with a roster") + ")";
    }
    order.insert(order.begin(), added.begin(), added.end());
    set_mod_order(inst.wstring(), order);
    fs::remove_all(stage, ec);
    if (summary) *summary = text;
    return "";
}

}  // namespace

std::string import_mod(const std::wstring& install_dir, const std::wstring& src, std::atomic<double>* frac,
                       const std::atomic<bool>* cancel, std::string* summary)
{
    Progress pr{ frac, cancel };
    const fs::path inst = install_dir;
    try {
        return run(inst, src, pr, summary);
    } catch (const std::exception& e) {
        std::error_code ec;
        fs::remove_all(inst / "mods" / ".incoming", ec);
        return e.what();
    }
}
