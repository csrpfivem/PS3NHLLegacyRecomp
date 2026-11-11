// #321 Audio packs: one .nhlaudio file (a zip) with a whole Custom Audio setup, so a
// player can share up-to-date goal horns / goal songs / menu music in one drop.
//
//   nhlaudio.txt      what is inside (for people; the import only checks it exists)
//   customaudio.bin   the Custom Audio table: section 5 of the profile save
//                     (savedata\BLUS315400000\SYS-DATA), 0x10428 bytes -- which song
//                     each slot plays, by content id
//   music\...         the files of <install>\music (playlist folders, covers)
//
// Content ids are hashes of the path inside music\ (nhl_usermusic.cpp), so an import
// that puts the files at the same paths makes the same assignments work anywhere.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// What could go in a pack: each playlist folder of music\ (and the loose songs).
struct AudioPackItem {
    std::wstring folder;    // name under music\, L"" = loose songs in music\ itself
    std::string label;      // UTF-8 for the UI
    int songs = 0;
    uint64_t bytes = 0;     // all its files (songs, covers)
    int slots = 0;          // Custom Audio slots that play from it
};
struct AudioPackScan {
    std::vector<AudioPackItem> items;
    bool has_table = false; // the profile's Custom Audio table could be read
    int slots = 0;          // assigned slots in it
    int slots_elsewhere = 0;// assigned to songs no longer in music\ (dropped from a pack)
    std::string table_note; // why not, when !has_table
};
AudioPackScan scan_audio_pack(const std::wstring& install_dir);

// <install>\audio packs\<name>.nhlaudio with the given playlist folders (L"" = loose
// songs) and, with_table, the Custom Audio choices -- only the slots whose songs are in
// the pack (the rest are left unassigned). "" or an error; *out = the file written.
std::string export_audio_pack(const std::wstring& install_dir, const std::vector<std::wstring>& folders, bool with_table,
                              std::wstring* out, std::string* summary);
// True for a .nhlaudio file, or a .zip that holds nhlaudio.txt / customaudio.bin.
bool is_audio_pack(const std::wstring& path);
// Copies the pack's music into <install>\music (same names replaced, other songs
// kept) and writes its Custom Audio table into the profile (backup in
// user\audio backups). Without a profile yet, the table waits in user\ and
// apply_pending_audio puts it in at the next PLAY. Game must not be running.
std::string import_audio_pack(const std::wstring& install_dir, const std::wstring& pack, std::string* summary);
// At PLAY: a waiting table goes into the profile once there is one. "" or an error.
std::string apply_pending_audio(const std::wstring& install_dir, std::string* summary);
