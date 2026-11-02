// Minimal ordered INI file: [section] / key = value / ; comments (comments are
// not preserved on save). Shared by the launcher and NHLLegacy.exe's
// nhl_settings.ini (the game ignores sections it does not know).
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // MoveFileExW (atomic save)
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

class Ini {
public:
    bool load(const std::wstring& file)
    {
        secs_.clear();
        FILE* f = _wfopen(file.c_str(), L"rb");
        if (!f) return false;
        std::string sec;
        char line[2048];
        while (fgets(line, sizeof line, f)) {
            std::string l = trim(line);
            if (l.size() >= 3 && (unsigned char)l[0] == 0xEF) l = l.substr(3);
            if (l.empty() || l[0] == ';' || l[0] == '#') continue;
            if (l[0] == '[') { sec = trim(l.substr(1, l.find(']') - 1)); section(sec); continue; }
            size_t eq = l.find('=');
            if (eq != std::string::npos) set(sec, trim(l.substr(0, eq)), trim(l.substr(eq + 1)));
        }
        fclose(f);
        return true;
    }
    // Written to <file>.tmp and swapped in, so a reader (the game, another launcher
    // window) never sees a truncated or half-written file.
    bool save(const std::wstring& file, const char* header) const
    {
        const std::wstring tmp = file + L".tmp";
        FILE* f = _wfopen(tmp.c_str(), L"wb");
        if (!f) return false;
        if (header) fprintf(f, "%s\n", header);
        for (const auto& [name, kvs] : secs_) {
            fprintf(f, "\n[%s]\n", name.c_str());
            for (const auto& [k, v] : kvs) fprintf(f, "%s = %s\n", k.c_str(), v.c_str());
        }
        if (fclose(f) != 0) { _wremove(tmp.c_str()); return false; }
        if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) { _wremove(tmp.c_str()); return false; }
        return true;
    }
    std::string get(const std::string& sec, const std::string& key, const std::string& def = "") const
    {
        for (const auto& [name, kvs] : secs_)
            if (_stricmp(name.c_str(), sec.c_str()) == 0)
                for (const auto& [k, v] : kvs)
                    if (_stricmp(k.c_str(), key.c_str()) == 0) return v;
        return def;
    }
    void set(const std::string& sec, const std::string& key, const std::string& val)
    {
        auto& kvs = section(sec);
        for (auto& [k, v] : kvs)
            if (_stricmp(k.c_str(), key.c_str()) == 0) { v = val; return; }
        kvs.push_back({ key, val });
    }

private:
    std::vector<std::pair<std::string, std::string>>& section(const std::string& sec)
    {
        for (auto& [name, kvs] : secs_)
            if (_stricmp(name.c_str(), sec.c_str()) == 0) return kvs;
        secs_.push_back({ sec, {} });
        return secs_.back().second;
    }
    static std::string trim(const std::string& s)
    {
        size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> secs_;
};
