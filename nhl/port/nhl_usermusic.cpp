/*
 * NHL Legacy Edition -- user music (PS3 "custom soundtrack") services. [#316]
 *
 * The game's Custom Audio screens and its user goal songs ("UserGoalHorn")
 * read the console's music library through cellSearch and decode tracks to
 * PCM through cellMusicDecode2 (UserMusicFramework::DecodeControlThread pulls
 * Read2, the spuusermusicarbiter mixes it). Before this file only the two
 * initialisers were answered (with an error) and the other 15 imports fell
 * into the catch-all "CELL_OK" fake: opening the Custom Music tab started a
 * list search that "succeeded" but never delivered its result callback, so
 * the screen waited forever (func_014B3E9C sets +0x1064 = 2 and waits).
 *
 * Library = the "music" folder beside the exe (NHL_MUSIC_DIR overrides).
 *   music\<playlist>\song.mp3   -> one list per sub-folder (album/playlist/
 *                                  artist/genre list searches all see the
 *                                  folders, like RPCS3's /dev_hdd0/music)
 *   music\song.mp3              -> loose tracks, in "all music" only
 * Formats: whatever Media Foundation decodes (mp3, m4a/aac, wav, wma, flac).
 *
 * Semantics follow RPCS3's cellSearch / cellMusicDecode (sysutil-delivered
 * callbacks; Read2 = 48 kHz stereo f32 big-endian interleaved; positions
 * START/MID/END/END_LIST_END). Unlike RPCS3 a finished track keeps its place
 * in the selection, so START replays it (the "song plays only once" bug).
 *
 * Content ids and selection contexts are keyed by a hash of the file's path
 * relative to the library, so a context the game saves in its profile still
 * resolves on the next boot as long as the file stays where it was.
 *
 * NHL_USERMUSIC=0 restores the old behaviour (both initialisers fail).
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")

extern "C" void ps3_hle_register(uint32_t nid, const char* name, void* handler);
extern "C" int32_t cellSysutilQueueGuestCallbackArgs(uint32_t opd, const uint64_t args[8]);
extern "C" uint8_t* vm_base;
extern "C" uint32_t ppu_hle_inject_base;

namespace {

/* ---- constants (RPCS3 cellSearch.h / cellMusicDecode.h) ---------------- */
constexpr uint32_t SEARCH_ERROR_PARAM                 = 0x8002C801u;
constexpr uint32_t SEARCH_ERROR_BUSY                  = 0x8002C802u;
constexpr uint32_t SEARCH_ERROR_ALREADY_INITIALIZED   = 0x8002C805u;
constexpr uint32_t SEARCH_ERROR_NOT_INITIALIZED       = 0x8002C806u;
constexpr uint32_t SEARCH_ERROR_NOT_SUPPORTED_SEARCH  = 0x8002C808u;
constexpr uint32_t SEARCH_ERROR_CONTENT_NOT_FOUND     = 0x8002C80Au;
constexpr uint32_t SEARCH_ERROR_OUT_OF_RANGE          = 0x8002C80Cu;
constexpr uint32_t SEARCH_ERROR_INVALID_SEARCHID      = 0x8002C80Du;
constexpr uint32_t SEARCH_ERROR_NOT_SUPPORTED_CONTEXT = 0x8002C80Fu;
constexpr uint32_t SEARCH_ERROR_GENERIC               = 0x8002C8FFu;

enum { SEV_INITIALIZE = 1, SEV_FINALIZE = 2, SEV_LIST = 3, SEV_INLIST = 4, SEV_CONTENT = 5 };
enum { CT_NONE = 0, CT_MUSIC = 1, CT_MUSICLIST = 2 };
enum { LST_MUSIC_ALBUM = 1, LST_MUSIC_GENRE = 2, LST_MUSIC_ARTIST = 3, LST_MUSIC_PLAYLIST = 9 };
enum { CST_MUSIC_ALL = 1 };
enum { REPEAT_NONE = 0, REPEAT_1 = 1, REPEAT_ALL = 2, REPEAT_NOREPEAT1 = 3 };
enum { OPT_NONE = 0, OPT_SHUFFLE = 1 };
enum { CODEC_UNKNOWN = 0, CODEC_MP3 = 8, CODEC_AAC = 9, CODEC_WAV = 11, CODEC_WMA = 12 };

constexpr uint32_t MD_ERROR_PARAM             = 0x8002C102u;
constexpr uint32_t MD_ERROR_NO_ACTIVE_CONTENT = 0x8002C104u;
constexpr uint32_t MD_ERROR_INVALID_CONTEXT   = 0x8002C106u;
constexpr uint32_t MD_ERROR_NO_MORE_CONTENT   = 0x8002C108u;
constexpr uint32_t MD_ERROR_NO_LPCM_DATA      = 0x8002C10Bu;
constexpr uint32_t MD_ERROR_GENERIC           = 0x8002C1FFu;
enum { MEV_INITIALIZE = 1, MEV_FINALIZE = 2, MEV_SET_DECODE_COMMAND = 4, MEV_SET_SELECTION_CONTEXT = 5 };
enum { CMD_STOP = 0, CMD_START = 1, CMD_NEXT = 2, CMD_PREV = 3 };
enum { ST_DORMANT = 0, ST_DECODING = 1 };
enum { POS_NONE = 0, POS_START = 1, POS_MID = 2, POS_END = 3, POS_END_LIST_END = 4 };

constexpr uint32_t TITLE_FIELD = 385;           /* CELL_SEARCH_TITLE_LEN_MAX + 1 */
constexpr uint32_t MUSICINFO_SIZE = 1628;
constexpr uint32_t LISTINFO_SIZE  = 792;
constexpr uint32_t CONTEXT_SIZE   = 2048;
constexpr uint32_t CTX_MAX_TRACKS = (CONTEXT_SIZE - 32) / 8;
constexpr uint32_t OUT_RATE = 48000;

/* ---- guest memory --------------------------------------------------------- */
inline uint8_t* G(uint32_t a) { return vm_base + a; }
inline void w32(uint32_t a, uint32_t v) { v = _byteswap_ulong(v); std::memcpy(G(a), &v, 4); }
inline void w64(uint32_t a, uint64_t v) { v = _byteswap_uint64(v); std::memcpy(G(a), &v, 8); }
inline uint32_t r32(uint32_t a) { uint32_t v; std::memcpy(&v, G(a), 4); return _byteswap_ulong(v); }
inline uint64_t r64(uint32_t a) { uint64_t v; std::memcpy(&v, G(a), 8); return _byteswap_uint64(v); }
inline void wstr(uint32_t a, const std::string& s, uint32_t field)
{
    size_t n = std::min<size_t>(s.size(), field - 1);
    while (n > 0 && n < s.size() && ((unsigned char)s[n] & 0xC0) == 0x80) --n;   /* keep UTF-8 whole */
    std::memcpy(G(a), s.data(), n);
    G(a)[n] = 0;
}

/* Callback params live in a small ring next to the save-data scratch window
 * (inject base + 0x130000; save data uses +0x100000, game data +0x120000). */
uint32_t param_slot()
{
    static long s_next = 0;
    const long k = InterlockedIncrement(&s_next) & 63;
    const uint32_t a = ppu_hle_inject_base + 0x00130000u + (uint32_t)k * 16u;
    std::memset(G(a), 0, 16);
    return a;
}

int g_log_left = 400;
std::mutex g_log_mx;
double um_uptime()
{
    static const ULONGLONG t0 = GetTickCount64();
    return (double)(GetTickCount64() - t0) / 1000.0;
}
#define UM_LOG(...) do { std::lock_guard<std::mutex> _l(g_log_mx); if (g_log_left > 0) { --g_log_left; \
    std::fprintf(stderr, "[usermusic %8.2f] ", um_uptime()); std::fprintf(stderr, __VA_ARGS__); } } while (0)
}
extern "C" void ppu_guest_callstack(const char* tag);   /* ppu_loader.cpp: guest chain of this call */
static void um_stack(const char* tag) { static const char* e = std::getenv("NHL_USERMUSIC_TRACE"); if (e && *e == '1') ppu_guest_callstack(tag); }
namespace {

/* ---- library --------------------------------------------------------------- */
struct Track {
    std::wstring path;
    std::string  title, album;
    uint64_t     hash = 0;
    int          list = -1;
    int64_t      size = 0;
    int          codec = CODEC_UNKNOWN;
    int64_t      duration_ms = -1;   /* filled lazily */
};
struct List {
    std::wstring path;
    std::string  name;
    uint64_t     hash = 0;
    std::vector<int> tracks;
};
struct Search {
    int type = CT_NONE;              /* entry type: CT_MUSIC or CT_MUSICLIST */
    int list_type = 0;
    std::vector<int> items;
};

std::mutex g_mx;                     /* library + search state */
std::vector<Track> g_tracks;
std::vector<List>  g_lists;
std::map<int, Search> g_searches;
int  g_next_search = 1;
bool g_search_init = false;
uint32_t g_search_cb = 0, g_search_ud = 0;

std::string utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
uint64_t fnv(const std::wstring& rel)
{
    std::wstring l = rel;
    for (auto& c : l) c = (wchar_t)towlower(c);
    const std::string u = utf8(l);
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : u) { h ^= c; h *= 1099511628211ull; }
    return h ? h : 1;
}
std::wstring library_root()
{
    if (const wchar_t* e = _wgetenv(L"NHL_MUSIC_DIR"); e && *e) return e;
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring d = exe;
    const size_t p = d.find_last_of(L"\\/");
    d = (p == std::wstring::npos) ? L"." : d.substr(0, p);
    return d + L"\\music";
}
int codec_of(const std::wstring& ext)
{
    if (ext == L".mp3") return CODEC_MP3;
    if (ext == L".m4a" || ext == L".aac" || ext == L".mp4") return CODEC_AAC;
    if (ext == L".wav") return CODEC_WAV;
    if (ext == L".wma") return CODEC_WMA;
    return CODEC_UNKNOWN;
}
bool is_audio(const std::wstring& name, std::wstring* ext_out)
{
    const size_t p = name.find_last_of(L'.');
    if (p == std::wstring::npos) return false;
    std::wstring ext = name.substr(p);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    static const wchar_t* ok[] = { L".mp3", L".m4a", L".aac", L".mp4", L".wav", L".wma", L".flac" };
    for (auto* o : ok) if (ext == o) { if (ext_out) *ext_out = ext; return true; }
    return false;
}
std::vector<std::pair<std::wstring, bool>> list_dir(const std::wstring& dir)   /* name, is_dir; sorted */
{
    std::vector<std::pair<std::wstring, bool>> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;
        out.emplace_back(n, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0; });
    return out;
}

void add_track(const std::wstring& dir, const std::wstring& rel_dir, const std::wstring& name, int list)
{
    std::wstring ext;
    if (!is_audio(name, &ext)) return;
    Track t;
    t.path  = dir + L"\\" + name;
    t.title = utf8(name.substr(0, name.find_last_of(L'.')));
    t.album = list >= 0 ? g_lists[(size_t)list].name : std::string();
    t.hash  = fnv(rel_dir.empty() ? name : rel_dir + L"\\" + name);
    t.list  = list;
    t.codec = codec_of(ext);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExW(t.path.c_str(), GetFileExInfoStandard, &a))
        t.size = ((int64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    if (list >= 0) g_lists[(size_t)list].tracks.push_back((int)g_tracks.size());
    g_tracks.push_back(std::move(t));
}

/* Rebuilds the library. Hashes are path-based, so ids stay valid across
 * rescans; durations already probed are carried over. Caller holds g_mx. */
void scan_library()
{
    std::vector<Track> keep;
    keep.swap(g_tracks);
    g_lists.clear();
    const std::wstring root = library_root();
    CreateDirectoryW(root.c_str(), nullptr);   /* so players can find where music goes */
    auto entries = list_dir(root);
    for (auto& [name, is_dir] : entries) {
        if (!is_dir) continue;
        List l;
        l.path = root + L"\\" + name;
        l.name = utf8(name);
        l.hash = fnv(L"<list>\\" + name);
        g_lists.push_back(std::move(l));
    }
    for (size_t i = 0; i < g_lists.size(); ++i) {
        const std::wstring rel = g_lists[i].path.substr(root.size() + 1);
        for (auto& [name, is_dir] : list_dir(g_lists[i].path))
            if (!is_dir) add_track(g_lists[i].path, rel, name, (int)i);
    }
    for (auto& [name, is_dir] : entries)
        if (!is_dir) add_track(root, L"", name, -1);
    for (Track& t : g_tracks)
        if (t.duration_ms < 0)
            for (const Track& o : keep) if (o.hash == t.hash) { t.duration_ms = o.duration_ms; break; }
}

int find_track(uint64_t h) { for (size_t i = 0; i < g_tracks.size(); ++i) if (g_tracks[i].hash == h) return (int)i; return -1; }
int find_list(uint64_t h)  { for (size_t i = 0; i < g_lists.size();  ++i) if (g_lists[i].hash  == h) return (int)i; return -1; }

/* CellSearchContentId (16 bytes): "NHL" + type, hash (BE), 0. */
void write_content_id(uint32_t a, int type, uint64_t hash)
{
    G(a)[0] = 'N'; G(a)[1] = 'H'; G(a)[2] = 'L'; G(a)[3] = (uint8_t)type;
    w64(a + 4, hash);
    w32(a + 12, 0);
}
bool read_content_id(uint32_t a, int* type, uint64_t* hash)
{
    if (!a || G(a)[0] != 'N' || G(a)[1] != 'H' || G(a)[2] != 'L') return false;
    *type = G(a)[3];
    *hash = r64(a + 4);
    return true;
}

/* ---- Media Foundation decoding ------------------------------------------- */
void mf_thread_init()
{
    static thread_local bool s_done = false;
    if (s_done) return;
    s_done = true;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);   /* RPC_E_CHANGED_MODE is fine */
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        UM_LOG("MFStartup -> 0x%08lX\n", (unsigned long)hr);
    });
}

struct Decoder {
    IMFSourceReader* rd = nullptr;
    uint32_t src_rate = OUT_RATE, src_ch = 2;
    bool eof = false;
    std::vector<float> src;           /* stereo frames at src_rate, not yet resampled */
    double src_pos = 0.0;             /* fractional read position in src */
    std::vector<float> out;           /* stereo frames at 48 kHz, ready */
    size_t out_pos = 0;               /* frames already handed out from 'out' */

    ~Decoder() { if (rd) rd->Release(); }

    bool open(const std::wstring& path)
    {
        mf_thread_init();
        if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &rd)) || !rd) return false;
        rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
        /* Ask for 48 kHz stereo float; if the reader won't convert, take float
         * at the source rate and resample here. */
        auto try_type = [&](bool full) {
            IMFMediaType* t = nullptr;
            if (FAILED(MFCreateMediaType(&t))) return false;
            t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
            if (full) {
                t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, OUT_RATE);
                t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
                t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 8);
                t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, OUT_RATE * 8);
            }
            const HRESULT hr = rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, t);
            t->Release();
            return SUCCEEDED(hr);
        };
        if (!try_type(true) && !try_type(false)) return false;
        IMFMediaType* cur = nullptr;
        if (FAILED(rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur))) return false;
        GUID sub = {};
        cur->GetGUID(MF_MT_SUBTYPE, &sub);
        src_rate = MFGetAttributeUINT32(cur, MF_MT_AUDIO_SAMPLES_PER_SECOND, OUT_RATE);
        src_ch   = MFGetAttributeUINT32(cur, MF_MT_AUDIO_NUM_CHANNELS, 2);
        const uint32_t bits = MFGetAttributeUINT32(cur, MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
        cur->Release();
        if (sub != MFAudioFormat_Float || bits != 32 || !src_rate || !src_ch) return false;
        return true;
    }

    static int64_t probe_duration_ms(const std::wstring& path)
    {
        mf_thread_init();
        IMFSourceReader* r = nullptr;
        if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &r)) || !r) return 0;
        PROPVARIANT v;
        PropVariantInit(&v);
        int64_t ms = 0;
        if (SUCCEEDED(r->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &v)) && v.vt == VT_UI8)
            ms = (int64_t)(v.uhVal.QuadPart / 10000);
        PropVariantClear(&v);
        r->Release();
        return ms;
    }

    /* Decodes one more sample into 'src'. false once the stream has ended. */
    bool pull()
    {
        if (eof || !rd) { eof = true; return false; }
        DWORD flags = 0;
        IMFSample* s = nullptr;
        const HRESULT hr = rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &s);
        if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))) {
            if (s) s->Release();
            eof = true;
            return false;
        }
        if (!s) return true;          /* gap / stream tick */
        IMFMediaBuffer* b = nullptr;
        if (SUCCEEDED(s->ConvertToContiguousBuffer(&b)) && b) {
            BYTE* p = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
                const float* f = reinterpret_cast<const float*>(p);
                const size_t frames = len / (4 * src_ch);
                for (size_t i = 0; i < frames; ++i) {
                    const float* fr = f + i * src_ch;
                    float l = fr[0], r = src_ch > 1 ? fr[1] : fr[0];
                    if (src_ch > 2) {             /* fold centre/surrounds in at -3 dB */
                        float c = 0;
                        for (uint32_t k = 2; k < src_ch; ++k) c += fr[k];
                        c *= 0.7071f / (float)(src_ch - 2);
                        l += c; r += c;
                    }
                    src.push_back(l);
                    src.push_back(r);
                }
                b->Unlock();
            }
            b->Release();
        }
        s->Release();
        resample(false);
        return true;
    }

    void resample(bool flush)
    {
        const size_t n = src.size() / 2;
        if (src_rate == OUT_RATE) {
            out.insert(out.end(), src.begin(), src.end());
            src.clear();
            return;
        }
        const double step = (double)src_rate / OUT_RATE;
        while (src_pos + 1.0 < (double)n || (flush && src_pos < (double)n)) {
            const size_t i = (size_t)src_pos;
            const double fr = src_pos - (double)i;
            const size_t j = (i + 1 < n) ? i + 1 : i;
            out.push_back((float)(src[2 * i]     + (src[2 * j]     - src[2 * i])     * fr));
            out.push_back((float)(src[2 * i + 1] + (src[2 * j + 1] - src[2 * i + 1]) * fr));
            src_pos += step;
        }
        const size_t drop = std::min<size_t>((size_t)src_pos, n);
        src.erase(src.begin(), src.begin() + 2 * drop);
        src_pos -= (double)drop;
    }

    size_t ready() const { return out.size() / 2 - out_pos; }

    /* Makes at least 'want' frames ready, or as many as remain. */
    void fill(size_t want)
    {
        while (ready() < want && pull()) {}
        if (eof && !src.empty()) { resample(true); src.clear(); }
        if (out_pos > 1u << 16) {     /* compact */
            out.erase(out.begin(), out.begin() + 2 * out_pos);
            out_pos = 0;
        }
    }
};

/* ---- music decode state ------------------------------------------------------ */
struct Selection {
    bool valid = false;
    uint32_t repeat = REPEAT_NONE, option = OPT_NONE;
    uint32_t first = 0;
    std::vector<uint64_t> hashes;
};

std::mutex g_md;                     /* decode state; taken before g_mx when both */
bool g_md_init = false;
uint32_t g_md_cb = 0, g_md_ud = 0;
Selection g_sel;
std::vector<uint32_t> g_order;       /* play order (indices into g_sel.hashes) */
int  g_cur = -1;                     /* position in g_order */
int  g_status = ST_DORMANT;
Decoder* g_dec = nullptr;
uint64_t g_track_frames = 0;         /* frames handed out for the current track */
bool g_end_pending = false;          /* short track: START sent, END owed */

void queue_search_cb(int ev, uint32_t result, uint32_t param)
{
    const uint64_t a[8] = { (uint64_t)ev, result, param, g_search_ud, 0, 0, 0, 0 };
    cellSysutilQueueGuestCallbackArgs(g_search_cb, a);
}
void queue_md_cb(int ev, uint32_t result)
{
    const uint64_t a[8] = { (uint64_t)ev, result, g_md_ud, 0, 0, 0, 0, 0 };
    cellSysutilQueueGuestCallbackArgs(g_md_cb, a);
}

/* STATUS_NOTIFICATION (event 0). The game's decode callback (func_014B476C)
 * keys its play state on these, which RPCS3 never sends:
 *   0 (DORMANT)   clears 0x024E50C4 "decoder active"; the play state machine
 *                 (func_014B48xx, state +0x1058 == 1) will not start another
 *                 song until it is clear -- the "plays only once" bug;
 *   1 (DECODING)  sets 0x024E50C5, which state 5 waits for after START;
 *   0x8002C10C    NEXT_CONTENTS_READY: the decoder moved on to the next track
 *                 (the game stops a single-song play on it). */
constexpr uint32_t MD_NEXT_CONTENTS_READY = 0x8002C10Cu;
void notify_status(uint32_t v) { queue_md_cb(0, v); }

void close_decoder() { delete g_dec; g_dec = nullptr; g_track_frames = 0; g_end_pending = false; }

void build_order()
{
    g_order.clear();
    for (uint32_t i = 0; i < g_sel.hashes.size(); ++i) g_order.push_back(i);
    if (g_sel.option == OPT_SHUFFLE && g_order.size() > 1) {
        static std::mt19937 rng{ std::random_device{}() };
        std::shuffle(g_order.begin(), g_order.end(), rng);
        auto it = std::find(g_order.begin(), g_order.end(), g_sel.first);
        if (it != g_order.end()) std::iter_swap(g_order.begin(), it);
        g_cur = 0;
    } else {
        g_cur = (int)std::min<uint32_t>(g_sel.first, (uint32_t)g_order.size() - 1);
    }
}

int next_index(int cur, bool forward)
{
    const int n = (int)g_order.size();
    if (n == 0) return -1;
    if (forward && g_sel.repeat == REPEAT_1) return cur;
    if (forward && g_sel.repeat == REPEAT_NOREPEAT1) return -1;
    const int nx = cur + (forward ? 1 : -1);
    if (nx >= 0 && nx < n) return nx;
    if (g_sel.repeat == REPEAT_ALL) return (nx + n) % n;
    return -1;
}

/* Opens the decoder for g_order[g_cur], skipping tracks that are missing or
 * fail to open. false when none of the remaining tracks can play. */
bool open_current()
{
    for (size_t tries = 0; tries <= g_order.size() && g_cur >= 0; ++tries) {
        close_decoder();
        std::wstring path;
        {
            std::lock_guard<std::mutex> l(g_mx);
            if (g_tracks.empty()) scan_library();
            const int t = find_track(g_sel.hashes[g_order[(size_t)g_cur]]);
            if (t >= 0) path = g_tracks[(size_t)t].path;
        }
        if (!path.empty()) {
            auto* d = new Decoder;
            if (d->open(path)) {
                g_dec = d;
                UM_LOG("playing %s\n", utf8(path).c_str());
                return true;
            }
            UM_LOG("cannot decode %s\n", utf8(path).c_str());
            delete d;
        }
        const int nx = next_index(g_cur, true);
        if (nx == g_cur) break;
        g_cur = nx;
    }
    return false;
}

/* =========================== cellSearch ==================================== */
uint64_t um_cellSearchInitialize(uint64_t mode, uint64_t, uint64_t func, uint64_t ud,
                                 uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> l(g_mx);
    UM_LOG("cellSearchInitialize(mode=%d, func=0x%X)\n", (int)mode, (uint32_t)func);
    if ((uint32_t)func == 0) return SEARCH_ERROR_PARAM;
    if (g_search_init) return SEARCH_ERROR_ALREADY_INITIALIZED;
    g_search_init = true;
    g_search_cb = (uint32_t)func;
    g_search_ud = (uint32_t)ud;
    scan_library();
    UM_LOG("library %s: %zu playlists, %zu tracks\n", utf8(library_root()).c_str(), g_lists.size(), g_tracks.size());
    if (const char* st = std::getenv("NHL_USERMUSIC_SELFTEST"); st && *st == '1') {
        /* decode every track on a side thread and report length / peak */
        std::vector<std::wstring> paths;
        for (const Track& t : g_tracks) paths.push_back(t.path);
        CreateThread(nullptr, 0, [](void* p) -> DWORD {
            auto* v = static_cast<std::vector<std::wstring>*>(p);
            for (const auto& path : *v) {
                Decoder d;
                const DWORD t0 = GetTickCount();
                if (!d.open(path)) { UM_LOG("selftest %s: OPEN FAILED\n", utf8(path).c_str()); continue; }
                const uint32_t rate = d.src_rate, ch = d.src_ch;
                uint64_t frames = 0; float peak = 0;
                for (;;) {
                    d.fill(48000);
                    const size_t n = d.ready();
                    if (!n) break;
                    const float* s = d.out.data() + 2 * d.out_pos;
                    for (size_t i = 0; i < 2 * n; ++i) peak = std::max(peak, std::fabs(s[i]));
                    d.out_pos += n; frames += n;
                }
                UM_LOG("selftest %s: src %u Hz %u ch -> %.2f s at 48 kHz, peak %.3f, %lu ms\n",
                       utf8(path).c_str(), rate, ch, frames / 48000.0, peak, GetTickCount() - t0);
            }
            delete v;
            return 0;
        }, new std::vector<std::wstring>(std::move(paths)), 0, nullptr);
    }
    queue_search_cb(SEV_INITIALIZE, 0, 0);
    return 0;
}

uint64_t um_cellSearchFinalize(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> l(g_mx);
    UM_LOG("cellSearchFinalize\n");
    um_stack("usermusic SearchFinalize");
    if (!g_search_init) return SEARCH_ERROR_NOT_INITIALIZED;
    g_searches.clear();
    g_search_init = false;
    queue_search_cb(SEV_FINALIZE, 0, 0);
    return 0;
}

uint32_t start_search(Search&& s, int ev, uint32_t out_id)
{
    const int id = g_next_search++;
    const uint32_t n = (uint32_t)s.items.size();
    g_searches[id] = std::move(s);
    w32(out_id, (uint32_t)id);
    const uint32_t p = param_slot();
    w32(p, (uint32_t)id);
    w32(p + 4, n);
    queue_search_cb(ev, 0, p);
    return (uint32_t)id;
}

uint64_t um_cellSearchStartListSearch(uint64_t type, uint64_t order, uint64_t out_id,
                                      uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> l(g_mx);
    if (!g_search_init) return SEARCH_ERROR_NOT_INITIALIZED;
    if (!(uint32_t)out_id) return SEARCH_ERROR_PARAM;
    const int t = (int)(int32_t)type;
    if (t != LST_MUSIC_ALBUM && t != LST_MUSIC_GENRE && t != LST_MUSIC_ARTIST && t != LST_MUSIC_PLAYLIST) {
        UM_LOG("cellSearchStartListSearch(type=%d) not a music list\n", t);
        return SEARCH_ERROR_NOT_SUPPORTED_SEARCH;
    }
    scan_library();
    Search s;
    s.type = CT_MUSICLIST;
    s.list_type = t;
    for (size_t i = 0; i < g_lists.size(); ++i) s.items.push_back((int)i);
    const uint32_t id = start_search(std::move(s), SEV_LIST, (uint32_t)out_id);
    UM_LOG("cellSearchStartListSearch(type=%d, order=%d) -> id %u, %zu lists\n", t, (int)order, id, g_lists.size());
    return 0;
}

uint64_t um_cellSearchStartContentSearchInList(uint64_t list_id, uint64_t key, uint64_t order, uint64_t out_id,
                                               uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> l(g_mx);
    if (!g_search_init) return SEARCH_ERROR_NOT_INITIALIZED;
    if (!(uint32_t)list_id || !(uint32_t)out_id) return SEARCH_ERROR_PARAM;
    int type; uint64_t h;
    if (!read_content_id((uint32_t)list_id, &type, &h) || type != CT_MUSICLIST) return SEARCH_ERROR_CONTENT_NOT_FOUND;
    const int li = find_list(h);
    if (li < 0) return SEARCH_ERROR_CONTENT_NOT_FOUND;
    Search s;
    s.type = CT_MUSIC;
    s.items = g_lists[(size_t)li].tracks;
    const uint32_t id = start_search(std::move(s), SEV_INLIST, (uint32_t)out_id);
    UM_LOG("cellSearchStartContentSearchInList(%s, key=%d, order=%d) -> id %u, %zu tracks\n",
           g_lists[(size_t)li].name.c_str(), (int)key, (int)order, id, g_lists[(size_t)li].tracks.size());
    return 0;
}

uint64_t um_cellSearchStartContentSearch(uint64_t type, uint64_t key, uint64_t order, uint64_t out_id,
                                         uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> l(g_mx);
    if (!g_search_init) return SEARCH_ERROR_NOT_INITIALIZED;
    if (!(uint32_t)out_id) return SEARCH_ERROR_PARAM;
    if ((int)(int32_t)type != CST_MUSIC_ALL) {
        UM_LOG("cellSearchStartContentSearch(type=%d) not music\n", (int)(int32_t)type);
        return SEARCH_ERROR_NOT_SUPPORTED_SEARCH;
    }
    scan_library();
    Search s;
    s.type = CT_MUSIC;
    for (size_t i = 0; i < g_tracks.size(); ++i) s.items.push_back((int)i);
    const uint32_t id = start_search(std::move(s), SEV_CONTENT, (uint32_t)out_id);
    UM_LOG("cellSearchStartContentSearch(all music, key=%d, order=%d) -> id %u, %zu tracks\n", (int)key, (int)order, id, g_tracks.size());
    return 0;
}

void write_track_info(uint32_t buf, Track& t)
{
    if (t.duration_ms < 0) t.duration_ms = Decoder::probe_duration_ms(t.path);
    std::memset(G(buf), 0, MUSICINFO_SIZE);
    w64(buf + 0, (uint64_t)t.duration_ms);
    w64(buf + 8, (uint64_t)t.size);
    w32(buf + 44, 48000);            /* samplingRate */
    w32(buf + 48, 16);               /* quantizationBitrate */
    w32(buf + 60, (uint32_t)t.codec);
    w32(buf + 64, 1);                /* CONTENTSTATUS_AVAILABLE */
    wstr(buf + 76, t.title, TITLE_FIELD);
    wstr(buf + 76 + 388, t.album, TITLE_FIELD);
    wstr(buf + 76 + 388 * 2, "", TITLE_FIELD);
    wstr(buf + 76 + 388 * 3, "", TITLE_FIELD);
}
void write_list_info(uint32_t buf, const List& li, int list_type)
{
    std::memset(G(buf), 0, LISTINFO_SIZE);
    int64_t dur = 0;
    for (int t : li.tracks) if (g_tracks[(size_t)t].duration_ms > 0) dur += g_tracks[(size_t)t].duration_ms;
    w32(buf + 0, (uint32_t)list_type);
    w32(buf + 4, (uint32_t)li.tracks.size());
    w64(buf + 8, (uint64_t)dur);
    wstr(buf + 16, li.name, TITLE_FIELD);
    wstr(buf + 16 + 388, "", TITLE_FIELD);
}

uint64_t um_cellSearchGetContentInfoByOffset(uint64_t sid, uint64_t off, uint64_t buf, uint64_t out_type, uint64_t out_id,
                                             uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> l(g_mx);
    if (!g_search_init) return SEARCH_ERROR_NOT_INITIALIZED;
    if ((uint32_t)out_type) w32((uint32_t)out_type, CT_NONE);
    auto it = g_searches.find((int)(int32_t)sid);
    if (it == g_searches.end()) return SEARCH_ERROR_INVALID_SEARCHID;
    const int o = (int)(int32_t)off;
    if (o < 0 || o >= (int)it->second.items.size()) return SEARCH_ERROR_OUT_OF_RANGE;
    const int idx = it->second.items[(size_t)o];
    if (it->second.type == CT_MUSIC) {
        Track& t = g_tracks[(size_t)idx];
        if ((uint32_t)buf) write_track_info((uint32_t)buf, t);
        if ((uint32_t)out_id) write_content_id((uint32_t)out_id, CT_MUSIC, t.hash);
    } else {
        const List& li = g_lists[(size_t)idx];
        if ((uint32_t)buf) write_list_info((uint32_t)buf, li, it->second.list_type);
        if ((uint32_t)out_id) write_content_id((uint32_t)out_id, CT_MUSICLIST, li.hash);
    }
    if ((uint32_t)out_type) w32((uint32_t)out_type, (uint32_t)it->second.type);
    return 0;
}

uint64_t um_cellSearchGetContentInfoByContentId(uint64_t cid, uint64_t buf, uint64_t out_type,
                                                uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> l(g_mx);
    if (!g_search_init) return SEARCH_ERROR_NOT_INITIALIZED;
    if (!(uint32_t)cid) return SEARCH_ERROR_PARAM;
    if ((uint32_t)out_type) w32((uint32_t)out_type, CT_NONE);
    int type; uint64_t h;
    if (!read_content_id((uint32_t)cid, &type, &h)) return SEARCH_ERROR_CONTENT_NOT_FOUND;
    if (type == CT_MUSIC) {
        const int t = find_track(h);
        if (t < 0) return SEARCH_ERROR_CONTENT_NOT_FOUND;
        if ((uint32_t)buf) write_track_info((uint32_t)buf, g_tracks[(size_t)t]);
    } else if (type == CT_MUSICLIST) {
        const int li = find_list(h);
        if (li < 0) return SEARCH_ERROR_CONTENT_NOT_FOUND;
        if ((uint32_t)buf) write_list_info((uint32_t)buf, g_lists[(size_t)li], LST_MUSIC_PLAYLIST);
    } else return SEARCH_ERROR_CONTENT_NOT_FOUND;
    if ((uint32_t)out_type) w32((uint32_t)out_type, (uint32_t)type);
    return 0;
}

uint64_t um_cellSearchGetOffsetByContentId(uint64_t sid, uint64_t cid, uint64_t out_off,
                                           uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> l(g_mx);
    if (!g_search_init) return SEARCH_ERROR_NOT_INITIALIZED;
    if (!(uint32_t)cid || !(uint32_t)out_off) return SEARCH_ERROR_PARAM;
    w32((uint32_t)out_off, 0xFFFFFFFFu);
    auto it = g_searches.find((int)(int32_t)sid);
    if (it == g_searches.end()) return SEARCH_ERROR_INVALID_SEARCHID;
    int type; uint64_t h;
    if (!read_content_id((uint32_t)cid, &type, &h) || type != it->second.type) return SEARCH_ERROR_CONTENT_NOT_FOUND;
    for (size_t i = 0; i < it->second.items.size(); ++i) {
        const int idx = it->second.items[i];
        const uint64_t ih = type == CT_MUSIC ? g_tracks[(size_t)idx].hash : g_lists[(size_t)idx].hash;
        if (ih == h) { w32((uint32_t)out_off, (uint32_t)i); return 0; }
    }
    return SEARCH_ERROR_CONTENT_NOT_FOUND;
}

/* CellMusicSelectionContext (2048 bytes, opaque to the game):
 *   +0 "NHLC"  +4 version 1  +8 repeat  +12 option  +16 first  +20 count
 *   +32 u64 track hashes[count]  (all big-endian) */
void write_context(uint32_t a, const Selection& s)
{
    std::memset(G(a), 0, CONTEXT_SIZE);
    std::memcpy(G(a), "NHLC", 4);
    w32(a + 4, 1);
    w32(a + 8, s.repeat);
    w32(a + 12, s.option);
    w32(a + 16, s.first);
    const uint32_t n = (uint32_t)std::min<size_t>(s.hashes.size(), CTX_MAX_TRACKS);
    w32(a + 20, n);
    for (uint32_t i = 0; i < n; ++i) w64(a + 32 + i * 8, s.hashes[i]);
}
bool read_context(uint32_t a, Selection* s)
{
    if (std::memcmp(G(a), "NHLC", 4) != 0 || r32(a + 4) != 1) return false;
    const uint32_t n = r32(a + 20);
    if (n == 0 || n > CTX_MAX_TRACKS) return false;
    s->repeat = r32(a + 8);
    s->option = r32(a + 12);
    s->first  = r32(a + 16);
    if (s->first >= n) s->first = 0;
    s->hashes.clear();
    for (uint32_t i = 0; i < n; ++i) s->hashes.push_back(r64(a + 32 + i * 8));
    s->valid = true;
    return true;
}

uint64_t um_cellSearchGetMusicSelectionContext(uint64_t sid, uint64_t cid, uint64_t repeat, uint64_t option, uint64_t out_ctx,
                                               uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> l(g_mx);
    if (!g_search_init) return SEARCH_ERROR_NOT_INITIALIZED;
    if (!(uint32_t)out_ctx) return SEARCH_ERROR_PARAM;
    auto it = g_searches.find((int)(int32_t)sid);
    if (it == g_searches.end()) return SEARCH_ERROR_INVALID_SEARCHID;
    Selection s;
    s.repeat = (uint32_t)repeat;
    s.option = (uint32_t)option;
    const Search& sr = it->second;
    if (!(uint32_t)cid) {
        if (sr.type != CT_MUSIC) return SEARCH_ERROR_NOT_SUPPORTED_CONTEXT;
        for (int t : sr.items) s.hashes.push_back(g_tracks[(size_t)t].hash);
        if (s.option == OPT_SHUFFLE && !s.hashes.empty()) s.first = (uint32_t)(std::random_device{}() % s.hashes.size());
    } else {
        int type; uint64_t h;
        if (!read_content_id((uint32_t)cid, &type, &h)) return SEARCH_ERROR_CONTENT_NOT_FOUND;
        if (type == CT_MUSICLIST) {
            const int li = find_list(h);
            if (li < 0) return SEARCH_ERROR_CONTENT_NOT_FOUND;
            for (int t : g_lists[(size_t)li].tracks) s.hashes.push_back(g_tracks[(size_t)t].hash);
        } else if (type == CT_MUSIC) {
            if (sr.type != CT_MUSIC) return SEARCH_ERROR_NOT_SUPPORTED_CONTEXT;
            for (size_t i = 0; i < sr.items.size(); ++i) {
                const uint64_t th = g_tracks[(size_t)sr.items[i]].hash;
                if (th == h) s.first = (uint32_t)s.hashes.size();
                s.hashes.push_back(th);
            }
            if (find_track(h) < 0) return SEARCH_ERROR_CONTENT_NOT_FOUND;
        } else return SEARCH_ERROR_CONTENT_NOT_FOUND;
    }
    if (s.hashes.empty()) return SEARCH_ERROR_CONTENT_NOT_FOUND;
    if (s.hashes.size() > CTX_MAX_TRACKS) {
        /* keep the window that contains the first track */
        const size_t start = s.first >= CTX_MAX_TRACKS ? s.first - CTX_MAX_TRACKS / 2 : 0;
        const size_t end = std::min(s.hashes.size(), start + CTX_MAX_TRACKS);
        s.hashes = std::vector<uint64_t>(s.hashes.begin() + start, s.hashes.begin() + end);
        s.first -= (uint32_t)start;
    }
    write_context((uint32_t)out_ctx, s);
    UM_LOG("cellSearchGetMusicSelectionContext(search %d, %s, repeat=%u, option=%u) -> %zu tracks, first %u\n",
           (int)(int32_t)sid, (uint32_t)cid ? "id" : "all", s.repeat, s.option, s.hashes.size(), s.first);
    return 0;
}

uint64_t um_cellSearchEnd(uint64_t sid, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> l(g_mx);
    if (!g_search_init) return SEARCH_ERROR_NOT_INITIALIZED;
    return g_searches.erase((int)(int32_t)sid) ? 0 : SEARCH_ERROR_INVALID_SEARCHID;
}

/* ========================= cellMusicDecode2 ================================= */
uint64_t um_cellMusicDecodeInitialize2(uint64_t mode, uint64_t, uint64_t prio, uint64_t func, uint64_t ud,
                                       uint64_t speed, uint64_t bufsize, uint64_t)
{
    std::lock_guard<std::mutex> l(g_md);
    UM_LOG("cellMusicDecodeInitialize2(mode=%d, prio=%d, func=0x%X, speed=%d, buf=0x%X)\n",
           (int)mode, (int)prio, (uint32_t)func, (int)speed, (uint32_t)bufsize);
    if ((uint32_t)func == 0 || (int)(int32_t)mode != 0) return MD_ERROR_PARAM;
    g_md_init = true;
    g_md_cb = (uint32_t)func;
    g_md_ud = (uint32_t)ud;
    g_status = ST_DORMANT;
    queue_md_cb(MEV_INITIALIZE, 0);
    return 0;
}

uint64_t um_cellMusicDecodeFinalize2(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> l(g_md);
    UM_LOG("cellMusicDecodeFinalize2 (after %llu frames of the last track)\n", (unsigned long long)g_track_frames);
    um_stack("usermusic Finalize2");
    close_decoder();
    const bool was_decoding = g_status == ST_DECODING;
    g_status = ST_DORMANT;
    if (g_md_init && g_md_cb) {
        if (was_decoding) notify_status(ST_DORMANT);
        queue_md_cb(MEV_FINALIZE, 0);
    }
    g_md_init = false;
    return 0;
}

uint64_t um_cellMusicDecodeSetSelectionContext2(uint64_t ctx, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> l(g_md);
    if (!(uint32_t)ctx) return MD_ERROR_PARAM;
    if (!g_md_cb) return MD_ERROR_GENERIC;
    Selection s;
    const bool ok = read_context((uint32_t)ctx, &s);
    UM_LOG("cellMusicDecodeSetSelectionContext2 -> %s (%zu tracks)\n", ok ? "ok" : "INVALID", s.hashes.size());
    if (ok) {
        close_decoder();
        if (g_status == ST_DECODING) notify_status(ST_DORMANT);
        g_status = ST_DORMANT;
        g_sel = std::move(s);
        build_order();
    }
    queue_md_cb(MEV_SET_SELECTION_CONTEXT, ok ? 0 : MD_ERROR_INVALID_CONTEXT);
    return 0;
}

uint64_t um_cellMusicDecodeSetDecodeCommand2(uint64_t cmd, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> l(g_md);
    const int c = (int)(int32_t)cmd;
    if (c < CMD_STOP || c > CMD_PREV) return MD_ERROR_PARAM;
    if (!g_md_cb) return MD_ERROR_GENERIC;
    uint32_t result = 0;
    if (c == CMD_STOP) {
        UM_LOG("STOP after %.2f s of the track\n", g_track_frames / 48000.0);
        um_stack("usermusic STOP");
    }
    switch (c) {
    case CMD_STOP:
        close_decoder();
        g_status = ST_DORMANT;
        break;
    case CMD_START:
        if (!g_sel.valid) { result = MD_ERROR_NO_ACTIVE_CONTENT; break; }
        build_order();               /* every START plays the selection from its first track */
        if (open_current()) g_status = ST_DECODING;
        else { g_status = ST_DORMANT; result = MD_ERROR_NO_ACTIVE_CONTENT; }
        break;
    case CMD_NEXT:
    case CMD_PREV: {
        if (!g_sel.valid) { result = MD_ERROR_NO_ACTIVE_CONTENT; break; }
        const int nx = next_index(g_cur, c == CMD_NEXT);
        close_decoder();
        if (nx < 0) { g_status = ST_DORMANT; result = MD_ERROR_NO_MORE_CONTENT; break; }
        g_cur = nx;
        if (open_current()) g_status = ST_DECODING;
        else { g_status = ST_DORMANT; result = MD_ERROR_NO_MORE_CONTENT; }
        break;
    }
    }
    UM_LOG("cellMusicDecodeSetDecodeCommand2(%d) -> 0x%X, status %d\n", c, result, g_status);
    queue_md_cb(MEV_SET_DECODE_COMMAND, result);
    notify_status((uint32_t)g_status);   /* after the result: event 4 OK re-sets 0x50C4 */
    return 0;
}

uint64_t um_cellMusicDecodeGetDecodeStatus2(uint64_t out, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> l(g_md);
    if (!(uint32_t)out) return MD_ERROR_PARAM;
    w32((uint32_t)out, (uint32_t)g_status);
    return 0;
}

uint64_t um_cellMusicDecodeGetContentsId2(uint64_t out, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> l(g_md);
    if (!(uint32_t)out) return MD_ERROR_PARAM;
    if (!g_sel.valid || g_cur < 0 || g_cur >= (int)g_order.size()) return MD_ERROR_NO_ACTIVE_CONTENT;
    write_content_id((uint32_t)out, CT_MUSIC, g_sel.hashes[g_order[(size_t)g_cur]]);
    return 0;
}

uint64_t um_cellMusicDecodeRead2(uint64_t buf, uint64_t start_time, uint64_t req, uint64_t read_size, uint64_t position,
                                 uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> l(g_md);
    const uint32_t b = (uint32_t)buf, st = (uint32_t)start_time, rs = (uint32_t)read_size, ps = (uint32_t)position;
    if (!b || !st || !rs || !ps || req == 0) return MD_ERROR_PARAM;
    w32(ps, POS_NONE);
    w64(rs, 0);
    w32(st, 0);
    if (g_status != ST_DECODING || !g_dec) {
        static int s_idle = 0;
        if (++s_idle <= 3) UM_LOG("Read2 while dormant -> NO_LPCM_DATA\n");
        return MD_ERROR_NO_LPCM_DATA;
    }

    const uint64_t req_bytes = std::min<uint64_t>(req, 0x7FFFFFFFull);
    const size_t want = (size_t)(req_bytes / 8);
    int pos;
    size_t give;
    if (g_end_pending) {
        pos = POS_END;               /* decided below */
        give = 0;
    } else {
        g_dec->fill(want + 1);
        const size_t have = g_dec->ready();
        const bool last = g_dec->eof && have <= want;
        give = std::min(have, want);
        if (give == 0 && !last) return MD_ERROR_NO_LPCM_DATA;
        if (g_track_frames == 0) { pos = POS_START; if (last) g_end_pending = true; }
        else pos = last ? POS_END : POS_MID;
        if (g_track_frames == 0 && last) give = std::min(have, want);
    }

    int next = -2;
    if (pos == POS_END) {
        next = next_index(g_cur, true);
        if (next < 0) pos = POS_END_LIST_END;
    }

    /* [#320] Gain. The game mixes the user-music voice far below its own horn / music
     * (a -9.6 LUFS, 0 dBFS horn was barely audible, install_test 11-11), and nothing in
     * cellMusicDecode2 sets a level. NHL_USERMUSIC_GAIN = dB (launcher: [game]
     * usermusic_gain), default +12. The arbiter mixes in float, so samples above 1.0 are
     * kept (bounded at +-8 against garbage). */
    static const float s_gain = [] {
        const char* e = std::getenv("NHL_USERMUSIC_GAIN");
        const double db = e && *e ? std::atof(e) : 12.0;
        const float g = (float)std::pow(10.0, std::min(30.0, std::max(-30.0, db)) / 20.0);
        UM_LOG("user music gain %+.1f dB (x%.2f)\n", db, g);
        return g;
    }();
    uint8_t* dst = G(b);
    if (give) {
        const float* src = g_dec->out.data() + 2 * g_dec->out_pos;
        for (size_t i = 0; i < give * 2; ++i) {
            const float f = std::min(8.0f, std::max(-8.0f, src[i] * s_gain));
            uint32_t v;
            std::memcpy(&v, &f, 4);
            v = _byteswap_ulong(v);
            std::memcpy(dst + i * 4, &v, 4);
        }
        g_dec->out_pos += give;
    }
    if (give * 8 < req_bytes) std::memset(dst + give * 8, 0, (size_t)(req_bytes - give * 8));
    w32(st, (uint32_t)(g_track_frames * 1000 / OUT_RATE));
    w64(rs, (uint64_t)give * 8);
    w32(ps, (uint32_t)pos);
    g_track_frames += give;

    if (pos == POS_END) {
        g_cur = next;
        if (open_current()) notify_status(MD_NEXT_CONTENTS_READY);
        else { g_status = ST_DORMANT; notify_status(ST_DORMANT); }
    } else if (pos == POS_END_LIST_END) {
        close_decoder();
        g_status = ST_DORMANT;
        notify_status(ST_DORMANT);
    }
    static int s_reads = 0;
    if (pos != POS_MID || ++s_reads <= 2)
        UM_LOG("Read2(req=0x%llX) -> 0x%llX bytes, position %d\n",
               (unsigned long long)req, (unsigned long long)give * 8, pos);
    return 0;
}

} // namespace

extern "C" void nhl_usermusic_register(void)
{
    const char* e = std::getenv("NHL_USERMUSIC");
    if (e && *e == '0') return;
    ps3_hle_register(0xC81CCF8Au, "cellSearchInitialize",               (void*)&um_cellSearchInitialize);
    ps3_hle_register(0xBFAB7616u, "cellSearchFinalize",                 (void*)&um_cellSearchFinalize);
    ps3_hle_register(0x0A4C8295u, "cellSearchStartListSearch",          (void*)&um_cellSearchStartListSearch);
    ps3_hle_register(0x64FB0B76u, "cellSearchStartContentSearchInList", (void*)&um_cellSearchStartContentSearchInList);
    ps3_hle_register(0x0591826Fu, "cellSearchStartContentSearch",       (void*)&um_cellSearchStartContentSearch);
    ps3_hle_register(0x3B210319u, "cellSearchGetContentInfoByOffset",   (void*)&um_cellSearchGetContentInfoByOffset);
    ps3_hle_register(0x9663A44Bu, "cellSearchGetContentInfoByContentId",(void*)&um_cellSearchGetContentInfoByContentId);
    ps3_hle_register(0x540D9068u, "cellSearchGetOffsetByContentId",     (void*)&um_cellSearchGetOffsetByContentId);
    ps3_hle_register(0x025CE169u, "cellSearchGetMusicSelectionContext", (void*)&um_cellSearchGetMusicSelectionContext);
    ps3_hle_register(0x774033D6u, "cellSearchEnd",                      (void*)&um_cellSearchEnd);
    ps3_hle_register(0xF80E31E5u, "cellMusicDecodeInitialize2",         (void*)&um_cellMusicDecodeInitialize2);
    ps3_hle_register(0x4AEF2877u, "cellMusicDecodeFinalize2",           (void*)&um_cellMusicDecodeFinalize2);
    ps3_hle_register(0x491D6BA5u, "cellMusicDecodeSetSelectionContext2",(void*)&um_cellMusicDecodeSetSelectionContext2);
    ps3_hle_register(0x2EF701ECu, "cellMusicDecodeSetDecodeCommand2",   (void*)&um_cellMusicDecodeSetDecodeCommand2);
    ps3_hle_register(0x1576E4F2u, "cellMusicDecodeGetDecodeStatus2",    (void*)&um_cellMusicDecodeGetDecodeStatus2);
    ps3_hle_register(0x25EA7AC0u, "cellMusicDecodeGetContentsId2",      (void*)&um_cellMusicDecodeGetContentsId2);
    ps3_hle_register(0xB2D054DFu, "cellMusicDecodeRead2",               (void*)&um_cellMusicDecodeRead2);
}
