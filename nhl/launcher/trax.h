// #318: your own songs in Customize EA Sports Trax.
//
// Songs in <install>\music\EA Trax\ become extra EA Trax entries (11..17). At PLAY,
// build_trax() rebuilds the game's music bank into data\trax\USRDIR\audio\music\,
// which compose_hdd0 lays on top of the update and mods:
//   base bank   = the highest enabled mod's audio\music bank, else the game's own
//                 (nocache.big music_samples.sbs + audioboot.big sbr/musicboxdata, chunkzip)
//   music box   = MBX0: records at 0xAC (count u32 @0xA4), 0x24 bytes
//                 {sound id, 1,1,1,1,1, type, pairs, props ptr}; props = pairs*2 string ptrs.
//                 Records 41..47 are MenuTunes with no song details; giving them
//                 Index/Artist/Title/AssetID 10..16 adds them to the Trax list (trax4).
//   index (SBR) = sounds @0xB6 (count u16 @0x30) {id u32, n u16, rec u32}; rec = {u16,
//                 u32 -> slot table 0xFA6 + 4*slot}; slot table = 239 u32 sbs offsets.
//   audio       = EALayer3 SPS: 0x48 header block (SNR 0x15 04 AC 44 ...), 0x44 data
//                 blocks, 0x45 end. Encoded with ffmpeg mp3_mf (Windows' MP3 encoder) +
//                 ealayer3 -E --two-files (BSD), cached in data\trax_cache\.
#pragma once
#include <string>
#include <vector>

// 7 go into the bank's empty MenuTunes records, the rest into new sounds appended to it.
// How many the game itself lists is being measured ([launcher] trax_fill = N repeats
// the songs up to N entries for that test).
constexpr int kTraxMax = 200;

struct TraxSong {
    std::wstring path;
    std::string artist, title;   // UTF-8
};

// Songs in <install>\music\EA Trax, sorted by file name (all of them; only the
// first kTraxMax are used).
std::vector<TraxSong> trax_songs(const std::wstring& install_dir);

// Builds (or removes, with no songs) data\trax. "" or an error; summary = one line.
std::string build_trax(const std::wstring& install_dir, std::string* summary);
