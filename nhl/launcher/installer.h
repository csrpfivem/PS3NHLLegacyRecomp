// The install job: turns the player's own disc (ISO or extracted folder) and the
// 1.01 update into the folder layout NHLLegacy.exe runs from.
//
// <install>\                 the launcher's folder
//   NHLLauncher.exe NHLLegacy.exe nhl_settings.ini ffmpeg\ffmpeg.exe
//   game\PS3_GAME\...         extracted ISO (only for an ISO source)
//   data\eboot.elf            decrypted 1.01 EBOOT
//   data\update\              the official 1.01 update files (from the .pkg or a folder)
//   mods\<name>\              one folder per mod, laid out like the update (USRDIR\...)
//   data\hdd0\game\BLUS31540  what the game sees as /dev_hdd0/game/BLUS31540: the
//                             update + enabled mods, hard-linked (compose_hdd0)
//   data\hdd1\cache\BLUS31540 the game's first-boot cache, built at disk speed (/dev_hdd1)
//   data\movies               VP6 movies carved for the ffmpeg bridge
//   data\edat_cache           decrypted secure0.big.sdat
//   user\                     working dir: saves (gamedata\), shader cache,
//                             rosters\ (roster saves not in use, per source)
//   logs\
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

constexpr const char* kTitleId = "BLUS31540";

struct InstallConfig {
    std::wstring install_dir;
    std::wstring game_src;      // .iso file, or a folder holding PS3_GAME
    std::wstring update_src;    // the 1.01 update: a .pkg file, or a folder holding USRDIR\EBOOT.BIN
    std::wstring eboot_elf;     // dev: an already decrypted 1.01 ELF instead of decrypting
};

struct ModInfo {
    std::string name;   // folder name under mods\ (UTF-8)
    int files = 0;
    bool enabled = true;
    bool roster = false;   // has roster\SYS-DATA (#257)
};
// Mods in <install>\mods with their on/off state from nhl_settings.ini [mods], in
// load order: [launcher] mod_order = names joined by '|' (no file name can hold one),
// then mods not in that list, alphabetically. Higher in the order (earlier) wins.
std::vector<ModInfo> list_mods(const std::wstring& install_dir);
void set_mod_enabled(const std::wstring& install_dir, const std::string& name, bool on);
void set_mod_order(const std::wstring& install_dir, const std::vector<std::string>& names);
// A file of the official 1.01 update: path relative to the update root ('/'), and size.
bool is_official_update_file(const std::string& rel, uint64_t size);
// Installs a mod from a drop (mod_import.cpp): a folder, a .zip or one game file is
// copied into mods\ (the source is only read). Every USRDIR inside is one mod; a
// roster's SYS-DATA goes with the biggest. A mod of the same name is replaced and keeps
// its place and on/off state; new mods are on, at the top of the load order (a pack's
// variants above its main mod, so they win). Blocking;
// frac gets 0..1, cancel stops it. "" or an error; summary says what was installed.
std::string import_mod(const std::wstring& install_dir, const std::wstring& src, std::atomic<double>* frac,
                       const std::atomic<bool>* cancel, std::string* summary);
// Rebuilds data\hdd0\game\BLUS31540 from data\update + the enabled mods (higher in the
// load order wins). "" or an error; summary gets a one-line result.
std::string compose_hdd0(const std::wstring& install_dir, std::string* summary);
// Rosters (#257): a mod may carry roster\SYS-DATA (a roster save's data, as RPCS3 /
// save tools export it). One roster source is active: [launcher] roster = mod name,
// "" = the player's own saved rosters / the game's built-in one. apply_roster puts the
// active source in the save slot the game auto-loads at boot; "" or an error.
std::string active_roster(const std::wstring& install_dir);
void set_active_roster(const std::wstring& install_dir, const std::string& mod);
std::string apply_roster(const std::wstring& install_dir, std::string* summary);

struct InstallProgress {
    std::atomic<int> step{ 0 }, steps{ 1 };
    std::atomic<double> frac{ 0 };       // of the current step
    std::atomic<bool> done{ false }, failed{ false }, cancel{ false };
    std::mutex m;
    std::string label, log;              // guarded by m
    void set_label(const std::string& s) { std::lock_guard<std::mutex> l(m); label = s; }
    void add_log(const std::string& s) { std::lock_guard<std::mutex> l(m); log += s; log += '\n'; }
};

// Quick checks for the wizard (no copying). "" = OK, else a message for the player.
// info gets a one-line description of what was found.
std::string check_game_source(const std::wstring& src, std::string* info);
std::string check_update_source(const std::wstring& src, std::string* info);

// Blocking; run it on a worker thread. Re-running resumes (finished files are skipped).
void run_install(const InstallConfig& cfg, InstallProgress& p);

// True when <install> has everything NHLLegacy.exe needs.
bool install_complete(const std::wstring& install_dir);

std::string narrow(const std::wstring& w);
std::wstring widen(const std::string& s);
