/*
 * NHL Legacy Edition -- host-decode bridge for the VP6 movies.
 *
 * The game's own VP6 decoder (part PPU job, part SPU job) produces block
 * corruption that accumulates across inter frames. The Xbox 360 recomp of the
 * same game hit the same decoder bug and shipped this workaround (puckhead73/
 * nhl-legacy-recomp PR #7): decode the movie on the host with ffmpeg and
 * overwrite the decoded planes at the movie player's publish seam, one frame per
 * guest publish so the game keeps the pacing and the audio is untouched.
 *
 * Seam: the lifted func_00D79E90(srcY r3, srcU r4, srcV r5, w r6, h r7,
 * srcPitch r8, desc r9) copies a decoded frame into the display ring described
 * by desc: Y/U/V EAs at +0x10/+0x14/+0x18, pitches at +0x30/+0x34/+0x38, plane
 * format at +0x50 (3/4 = planar, the only case it copies), layout at +0x58
 * (1 = one packed buffer: U at Y+w*h, V at Y+w*h*5/4, chroma pitch = Y pitch/2).
 * A hand-added call at its exit lands in nhl_vp6_publish(). The ring is stored
 * bottom-up, so ffmpeg's top-down rows are written flipped.
 *
 * Which movie: the movies live inside .big archives. tools/vp6carve.py carves
 * each one to <movie dir>/<archive>_<OFFSET>.vp6. A cellFsRead hook (ppu_fs.cpp,
 * g_fs_read_hook) notes which carved byte range the player is reading; the
 * publish picks the most recently read movie with the frame's dimensions. A
 * read back near a movie's start after it had streamed on means the game
 * restarted it, so the decoder restarts too.
 *
 * Env: NHL_VP6_BRIDGE=0 disables it. NHL_FFMPEG = ffmpeg.exe path,
 * NHL_MOVIE_DIR = carved movie directory (both set by tools/run_nhl.ps1).
 * NHL_VP6_FLIP=0 writes rows unflipped.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ppu_context.h"

extern "C" uint8_t* vm_base;
extern "C" void (*g_fs_read_hook)(const char* path, uint64_t pos, uint64_t n);
extern "C" int (*g_vp6_frame_of_hook)(uint32_t ea, uint32_t* wh);   /* rsx_d3d12_backend.c, #178 */
extern "C" int nhl_vp6_frame_of(uint32_t ea, uint32_t* wh);
extern "C" int (*g_vp6_plane_hook)(uint32_t ea, uint32_t* wh, int* idx);   /* rsx_d3d12_backend.c, #179 */
extern "C" int nhl_vp6_plane_of(uint32_t ea, uint32_t* wh, int* idx);
extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern "C" void ppu_dump_guest_stack(ppu_context*, const char*);

/* Last file this host thread read (path@offset), for diagnostics that run
 * right after a guest read completes (the async file worker's callback). */
static thread_local char t_last_read[200];
extern "C" const char* nhl_last_read_path(void) { return t_last_read; }

namespace {

struct Movie {
    std::string archive;      /* lower-case archive base name, e.g. "nocache" */
    std::string file;         /* host path of the carved stream */
    uint64_t off = 0, size = 0;
    uint32_t w = 0, h = 0;
    std::atomic<uint64_t> last_read_ms{0};
    std::atomic<uint64_t> high{0};       /* furthest offset read this generation */
    std::atomic<uint32_t> gen{0};        /* bumped when the game restarts the stream */
};

std::vector<Movie*> g_movies;
std::string g_ffmpeg;
bool g_flip = true;

uint64_t now_ms() { return GetTickCount64(); }

uint32_t rd32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

std::string lower(std::string s)
{
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

/* ---- one ffmpeg process feeding raw yuv420p frames ---- */
struct Decoder {
    Movie* mv = nullptr;
    uint32_t gen = 0;
    size_t frame_bytes = 0;
    HANDLE proc = nullptr, out_rd = nullptr;
    std::thread reader;
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> q;
    bool eof = false, stop = false;
    uint64_t popped = 0, underruns = 0;
    uint64_t last_pub_ms = 0;
    /* [NHL local patch #180] guest frame index (desc +0x54) -> decoded frame: a NEW index
     * takes the next decoded frame (publish order), a repeated one reuses its frame.
     * cache = decoded frames by sequence number (last 8). */
    uint32_t seq_next = 0, max_idx = 0; bool any_idx = false;
    std::map<uint32_t, uint32_t> idx2seq;
    std::map<uint32_t, std::vector<uint8_t>> cache;
    std::mutex cm;   /* guards the maps: tid 4 and tid 5 publish concurrently */

    bool start(Movie* movie)
    {
        mv = movie;
        gen = movie->gen.load();
        frame_bytes = (size_t)movie->w * movie->h * 3 / 2;
        SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
        HANDLE wr = nullptr;
        if (!CreatePipe(&out_rd, &wr, &sa, 1 << 20)) return false;
        SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);
        std::string cmd = "\"" + g_ffmpeg + "\" -nostdin -loglevel error -an -i \"" + movie->file +
                          "\" -f rawvideo -pix_fmt yuv420p -threads 2 pipe:1";
        STARTUPINFOA si = {};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = wr;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput = nullptr;
        PROCESS_INFORMATION pi = {};
        std::vector<char> cl(cmd.begin(), cmd.end());
        cl.push_back(0);
        BOOL ok = CreateProcessA(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                 nullptr, nullptr, &si, &pi);
        CloseHandle(wr);
        if (!ok) {
            std::fprintf(stderr, "[vp6bridge] CreateProcess failed (%lu) for %s\n", GetLastError(), g_ffmpeg.c_str());
            CloseHandle(out_rd); out_rd = nullptr;
            return false;
        }
        CloseHandle(pi.hThread);
        proc = pi.hProcess;
        reader = std::thread([this] { run(); });
        return true;
    }

    void run()
    {
        for (;;) {
            std::vector<uint8_t> f(frame_bytes);
            size_t got = 0;
            while (got < frame_bytes) {
                DWORD n = 0;
                if (!ReadFile(out_rd, f.data() + got, (DWORD)(frame_bytes - got), &n, nullptr) || n == 0) break;
                got += n;
            }
            std::unique_lock<std::mutex> lk(m);
            if (got < frame_bytes || stop) { eof = true; cv.notify_all(); return; }
            cv.wait(lk, [this] { return stop || q.size() < 24; });   /* backpressure */
            if (stop) { eof = true; return; }
            q.push_back(std::move(f));
            cv.notify_all();
        }
    }

    /* Next frame, waiting up to wait_ms. Empty when the stream is out. */
    bool pop(std::vector<uint8_t>& out, DWORD wait_ms)
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::milliseconds(wait_ms), [this] { return !q.empty() || eof; });
        if (q.empty()) return false;
        out = std::move(q.front());
        q.pop_front();
        cv.notify_all();
        popped++;
        return true;
    }

    bool finished()
    {
        std::lock_guard<std::mutex> lk(m);
        return eof && q.empty();
    }

    ~Decoder()
    {
        { std::lock_guard<std::mutex> lk(m); stop = true; }
        cv.notify_all();
        if (proc) { TerminateProcess(proc, 0); }
        if (out_rd) { CloseHandle(out_rd); out_rd = nullptr; }
        if (reader.joinable()) reader.join();
        if (proc) { CloseHandle(proc); proc = nullptr; }
    }
};

std::mutex g_lock;
/* [#178] display meter table: slot Y-plane address -> guest frame index */
struct ShowEnt { uint32_t y, u, v, idx, wh; };
ShowEnt g_show[16]; int g_show_next = 0; std::mutex g_show_lock;
/* Per MOVIE, not per descriptor: the player publishes round-robin into a
 * 4-slot display ring, one descriptor per slot, so consecutive frames of one
 * movie arrive through four different descriptors. */
std::map<Movie*, Decoder*> g_dec;

void on_read(const char* path, uint64_t pos, uint64_t n)
{
    if (!path || !*path) return;
    snprintf(t_last_read, sizeof t_last_read, "%s@0x%llX", path, (unsigned long long)pos);
    /* NHL_READ_TRACE=<substring>: log every read of a matching path with its
     * file offset (to carve exactly what the game loaded out of an archive). */
    { static const char* rt = (const char*)1;
      if (rt == (const char*)1) rt = std::getenv("NHL_READ_TRACE");
      if (rt && *rt && strstr(path, rt))
          std::fprintf(stderr, "[READTRACE] %s pos=0x%llX n=%llu\n", path,
                       (unsigned long long)pos, (unsigned long long)n); }
    /* NHL_READ_STACK=<substring>: the guest call stack of the first 6 reads of
     * a matching path -- which loader asked for it. */
    { static const char* rs = (const char*)1; static int ns = 0;
      if (rs == (const char*)1) rs = std::getenv("NHL_READ_STACK");
      if (rs && *rs && strstr(path, rs) && ns < 6 && g_active_ctx) {
          ns++;
          std::fprintf(stderr, "[READSTACK] %s pos=0x%llX n=%llu tid=%llu\n", path,
                       (unsigned long long)pos, (unsigned long long)n,
                       (unsigned long long)g_active_ctx->thread_id);
          ppu_dump_guest_stack(g_active_ctx, "readstack");
      } }
    const char* b = strrchr(path, '/');
    std::string base = lower(b ? b + 1 : path);
    size_t dot = base.rfind('.');
    if (dot == std::string::npos || base.compare(dot, 4, ".big") != 0) return;
    base.resize(dot);
    for (Movie* mv : g_movies) {
        if (mv->archive != base) continue;
        if (pos + n <= mv->off || pos >= mv->off + mv->size) continue;
        uint64_t rel = pos > mv->off ? pos - mv->off : 0;
        if (rel < 0x20000 && mv->high.load() > 0x100000) {   /* back to the start: restarted */
            mv->gen++;
            mv->high = 0;
        }
        if (rel + n > mv->high.load()) mv->high = rel + n;
        if (!mv->last_read_ms.load())
            std::fprintf(stderr, "[vp6bridge] first read of %s@0x%llX (%ux%u) via %s\n", mv->archive.c_str(),
                         (unsigned long long)mv->off, mv->w, mv->h, path);
        mv->last_read_ms = now_ms();
    }
}

Movie* pick(uint32_t w, uint32_t h)
{
    Movie* best = nullptr;
    uint64_t t = now_ms();
    for (Movie* mv : g_movies) {
        if (mv->w != w || mv->h != h) continue;
        uint64_t lr = mv->last_read_ms.load();
        /* No short window: a small movie (the 7 MB transition) is read in one
         * go, long before its last frame is shown. */
        if (!lr || t - lr > 600000) continue;
        if (!best || lr > best->last_read_ms.load()) best = mv;
    }
    return best;
}

void load_movies(const std::string& dir)
{
    WIN32_FIND_DATAA fd;
    HANDLE hf = FindFirstFileA((dir + "\\*.vp6").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return;
    do {
        std::string name = fd.cFileName;
        size_t us = name.rfind('_');
        if (us == std::string::npos) continue;
        Movie* mv = new Movie;
        mv->archive = lower(name.substr(0, us));
        mv->off = strtoull(name.c_str() + us + 1, nullptr, 16);
        mv->size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        mv->file = dir + "\\" + name;
        uint8_t hdr[32] = {};
        if (FILE* f = fopen(mv->file.c_str(), "rb")) { fread(hdr, 1, sizeof hdr, f); fclose(f); }
        if (memcmp(hdr, "MVhd", 4) != 0) { delete mv; continue; }
        uint32_t le = hdr[4] | hdr[5] << 8 | hdr[6] << 16 | (uint32_t)hdr[7] << 24;
        uint32_t be = (uint32_t)hdr[4] << 24 | hdr[5] << 16 | hdr[6] << 8 | hdr[7];
        bool little = le < be;
        mv->w = little ? (hdr[12] | hdr[13] << 8) : (hdr[12] << 8 | hdr[13]);
        mv->h = little ? (hdr[14] | hdr[15] << 8) : (hdr[14] << 8 | hdr[15]);
        g_movies.push_back(mv);
        std::fprintf(stderr, "[vp6bridge] movie %s @0x%llX size=%llu %ux%u\n", mv->archive.c_str(),
                     (unsigned long long)mv->off, (unsigned long long)mv->size, mv->w, mv->h);
    } while (FindNextFileA(hf, &fd));
    FindClose(hf);
}

void write_plane(uint32_t dst, uint32_t pitch, const uint8_t* src, uint32_t w, uint32_t h)
{
    if (!dst || !pitch || pitch < w) return;
    for (uint32_t r = 0; r < h; r++) {
        const uint8_t* s = src + (size_t)(g_flip ? h - 1 - r : r) * w;
        memcpy(vm_base + dst + (size_t)r * pitch, s, w);
    }
}

} // namespace

/* Called at the exit of the lifted func_00D79E90 (the plane publish). */
extern "C" unsigned long long ps3_qpc_us(void);   /* cellGcmSys.c */
extern "C" void nhl_vp6_publish(uint32_t desc, uint32_t w, uint32_t h)
{
    if (g_movies.empty() || !vm_base || !desc || !w || !h) return;
    /* NHL_VP6_DIAG=1: who publishes (guest stack on the first call) and how often
     * (publishes per second, every 2 s) -- the movie's speed is this rate. */
    { static int on = -1; if (on < 0) on = std::getenv("NHL_VP6_DIAG") ? 1 : 0;
      if (on) {
          static std::atomic<long> n{0}; static std::atomic<long long> t0{0};
          long k = ++n; long long now = (long long)now_ms();
          if (k == 1) { t0 = now;
              if (g_active_ctx) { std::fprintf(stderr, "[vp6diag] first publish by guest tid %u\n",
                                               (unsigned)g_active_ctx->thread_id);
                                  ppu_dump_guest_stack(g_active_ctx, "vp6pub"); } }
          else if (now - t0 >= 2000) {
              std::fprintf(stderr, "[vp6diag] %.1f publishes/s (tid %u)\n", (k - 1) * 1000.0 / (now - t0),
                           g_active_ctx ? (unsigned)g_active_ctx->thread_id : 0u);
              n = 1; t0 = now; }
          /* [NHL local patch #178] gaps between consecutive publishes of the same
           * stream (keyed by size), QPC microseconds: an average of 30/s can still
           * be bursty (two in one 33 ms display interval, none in the next = a
           * skipped then a repeated movie frame = jitter). Every 2 s per size. */
          { struct G { uint32_t wh; unsigned long long last, t0; uint32_t b[6], n; unsigned long long mx; };
            static G g[4]; static std::mutex gm;
            std::lock_guard<std::mutex> lk(gm);
            const uint32_t wh = (w << 16) | h; G* s = nullptr;
            for (G& x : g) if (x.wh == wh) { s = &x; break; }
            if (!s) for (G& x : g) if (!x.wh) { x = G{}; x.wh = wh; s = &x; break; }
            if (s) {
                const unsigned long long t = ps3_qpc_us();
                if (s->last) {
                    const unsigned long long d = t - s->last;
                    s->b[d < 10000 ? 0 : d < 25000 ? 1 : d < 30000 ? 2 : d < 37000 ? 3 : d < 50000 ? 4 : 5]++;
                    s->n++; if (d > s->mx) s->mx = d;
                }
                s->last = t; if (!s->t0) s->t0 = t;
                if (t - s->t0 >= 2000000 && s->n) {
                    std::fprintf(stderr, "[vp6gap] %ux%u %u gaps: <10ms %u, 10-25 %u, 25-30 %u, 30-37 %u, 37-50 %u, 50+ %u | max %.1f ms\n",
                                 w, h, s->n, s->b[0], s->b[1], s->b[2], s->b[3], s->b[4], s->b[5], s->mx / 1000.0);
                    memset(s->b, 0, sizeof s->b); s->n = 0; s->mx = 0; s->t0 = t;
                }
            } } } }
    uint32_t fmt = rd32(desc + 0x50), mode = rd32(desc + 0x58);
    { /* One line per distinct (w, h, fmt, mode) the seam sees. */
        static uint32_t seen[16]; static int ns = 0;
        uint32_t k = (w << 20) ^ (h << 8) ^ (fmt << 4) ^ mode; bool f = false;
        for (int i = 0; i < ns; i++) if (seen[i] == k) f = true;
        if (!f && ns < 16) { seen[ns++] = k;
            std::fprintf(stderr, "[vp6bridge] publish seen: desc=0x%08X %ux%u fmt=%u mode=%u\n", desc, w, h, fmt, mode); }
    }
    if (fmt != 3 && fmt != 4) return;              /* the seam copied nothing */
    /* [#178] display meter: remember which guest frame index (desc +0x54) each slot's
     * Y plane holds, so the renderer can report which frame each present showed. */
    { std::lock_guard<std::mutex> lk(g_show_lock);
      const uint32_t y = rd32(desc + 0x10), idx = rd32(desc + 0x54);
      uint32_t u = rd32(desc + 0x14), v = rd32(desc + 0x18);
      if (mode == 1) { u = y + w * h; v = y + w * h * 5 / 4; }   /* same layout as the plane write below */
      int k = -1;
      for (int i = 0; i < 16; i++) if (g_show[i].y == y) { k = i; break; }
      if (k < 0) { k = g_show_next++ & 15; }
      g_show[k].y = y; g_show[k].u = u; g_show[k].v = v; g_show[k].idx = idx; g_show[k].wh = (w << 16) | h; }
    /* [#178] NHL_VP6_DESCDUMP=<n>: the first n publishes' descriptor words 0x00..0x7C
     * with the publishing tid -- to find the guest's own frame number / timestamp. */
    { static int dn = -1; static std::atomic<int> dk{0}; static uint32_t dw = 0;
      if (dn < 0) { const char* e = std::getenv("NHL_VP6_DESCDUMP"); dn = e ? atoi(e) : 0;
                    const char* ew = std::getenv("NHL_VP6_DESCDUMP_W"); dw = ew ? (uint32_t)atoi(ew) : 0; }
      if (dn > 0 && (!dw || w == dw)) { const int k = dk++; if (k < dn) {
          char line[640]; int o = snprintf(line, sizeof line, "[vp6desc] #%d tid=%u t=%.3f desc=0x%08X:", k,
                                           g_active_ctx ? (unsigned)g_active_ctx->thread_id : 0u,
                                           (double)ps3_qpc_us() / 1e6, desc);
          for (uint32_t i = 0; i < 0x80 && o < (int)sizeof line - 12; i += 4) o += snprintf(line + o, sizeof line - o, " %08X", rd32(desc + i));
          std::fprintf(stderr, "%s\n", line); } } }
    Movie* mv = pick(w, h);
    if (!mv) {
        static int s_nm = 0;
        if (s_nm++ < 4) std::fprintf(stderr, "[vp6bridge] no carved movie read for a %ux%u publish\n", w, h);
        /* [NHL #89] guest-decoded movies: publish count per descriptor with an
         * uptime stamp (first, every 30th) -- how long the in-game environment
         * movie really runs (RPCS3: ~8 s per play, re-created repeatedly). */
        { static uint32_t descs[8]; static uint32_t cnt[8]; static int nd = 0;
          int k = -1; for (int i = 0; i < nd; i++) if (descs[i] == desc) k = i;
          if (k < 0 && nd < 8) { k = nd++; descs[k] = desc; cnt[k] = 0; }
          if (k >= 0) { cnt[k]++;
              if (cnt[k] == 1 || cnt[k] % 30 == 0)
                  std::fprintf(stderr, "[vp6guest] desc=0x%08X %ux%u publish #%u t=%.3f\n", desc, w, h, cnt[k],
                               (double)ps3_qpc_us() / 1e6); } }
        return;
    }

    /* [#180] new guest frame index -> next decoded frame; repeated index -> the same frame.
     * The player primes its 4-slot ring with frame 0 (index 0 published 5x): one pop per
     * publish ran the picture 4 frames (133 ms) ahead for the whole movie. Serving strictly BY
     * index was wrong too: the arena boards publish out of order (7,5,6,10,8,9 ...) and the
     * game shows the most recently published slot, so by-index content stepped backwards on
     * screen (play246). Publish-order content keeps the shown picture monotonic either way.
     * NHL_VP6_BYINDEX=0 = old one-pop-per-publish. */
    static int byidx_on = -1;
    if (byidx_on < 0) { const char* e = std::getenv("NHL_VP6_BYINDEX"); byidx_on = (e && *e == '0') ? 0 : 1; }
    const uint32_t gidx = rd32(desc + 0x54);
    const bool byidx = byidx_on && gidx < 0x100000u;

    Decoder* d;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        /* Drop decoders for movies nothing has published for a while. */
        for (auto it = g_dec.begin(); it != g_dec.end();) {
            if (it->first != mv && now_ms() - it->second->last_pub_ms > 10000) {
                delete it->second;
                it = g_dec.erase(it);
            } else ++it;
        }
        Decoder*& slot = g_dec[mv];
        bool restart = !slot || slot->gen != mv->gen.load();
        if (!restart && byidx) {
            std::lock_guard<std::mutex> clk(slot->cm);
            if (slot->any_idx && gidx + 8 < slot->max_idx) restart = true;           /* rewound: replay */
            else if (slot->finished() && !slot->idx2seq.count(gidx)) restart = true;  /* past the end: loop */
        } else if (!restart && slot->finished()) restart = true;
        if (restart) {
            if (slot) {
                std::fprintf(stderr, "[vp6bridge] restart %s@0x%llX after %llu frames (%llu underruns)%s\n",
                             mv->archive.c_str(), (unsigned long long)mv->off,
                             (unsigned long long)slot->popped, (unsigned long long)slot->underruns,
                             slot->gen != mv->gen.load() ? " -- game restarted the stream" : " -- end of stream");
                delete slot;
                slot = nullptr;
            }
            Decoder* nd = new Decoder;
            if (!nd->start(mv)) { delete nd; return; }
            slot = nd;
            std::fprintf(stderr, "[vp6bridge] desc=0x%08X start %s@0x%llX %ux%u gen=%u\n", desc,
                         mv->archive.c_str(), (unsigned long long)mv->off, w, h, mv->gen.load());
        }
        d = slot;
        d->last_pub_ms = now_ms();
    }

    std::vector<uint8_t> f;
    if (!byidx) {
        if (!d->pop(f, d->popped ? 100 : 1500)) { d->underruns++; return; }   /* keep the guest frame */
    } else {
        /* [#180] a new index takes the next decoded frame; a repeat reuses its frame.
         * Decoder late or at its end: hold the newest frame. */
        std::lock_guard<std::mutex> clk(d->cm);
        auto m = d->idx2seq.find(gidx);
        uint32_t seq;
        if (m != d->idx2seq.end()) seq = m->second;
        else {
            std::vector<uint8_t> nf;
            if (d->pop(nf, d->popped ? 100 : 1500)) {
                seq = d->seq_next++;
                d->cache[seq] = std::move(nf);
                while (d->cache.size() > 8) d->cache.erase(d->cache.begin());
            } else if (!d->cache.empty()) {
                seq = std::prev(d->cache.end())->first; d->underruns++;
            } else { d->underruns++; return; }
            d->idx2seq[gidx] = seq;
            while (d->idx2seq.size() > 32) d->idx2seq.erase(d->idx2seq.begin());
        }
        if (!d->any_idx || gidx > d->max_idx) d->max_idx = gidx;
        d->any_idx = true;
        auto it = d->cache.find(seq);
        if (it == d->cache.end()) {
            if (d->cache.empty()) { d->underruns++; return; }
            it = std::prev(d->cache.end());
        }
        f = it->second;
        /* the slot now shows decoded frame `seq`: the display meter and the #179 snapshot
         * de-dup key on what is in the planes, not on the guest's index */
        { std::lock_guard<std::mutex> lk(g_show_lock); const uint32_t y = rd32(desc + 0x10);
          for (int i = 0; i < 16; i++) if (g_show[i].y == y) { g_show[i].idx = it->first; break; } }
    }

    uint32_t Y = rd32(desc + 0x10), U = rd32(desc + 0x14), V = rd32(desc + 0x18);
    uint32_t pY = rd32(desc + 0x30), pU = rd32(desc + 0x34), pV = rd32(desc + 0x38);
    if (mode == 1) { U = Y + w * h; V = Y + w * h * 5 / 4; pU = pV = pY / 2; }
    const uint8_t* fy = f.data();
    const uint8_t* fu = fy + (size_t)w * h;
    const uint8_t* fv = fu + (size_t)(w / 2) * (h / 2);
    write_plane(Y, pY, fy, w, h);
    write_plane(U, pU, fu, w / 2, h / 2);
    write_plane(V, pV, fv, w / 2, h / 2);
    /* Progress marks: every 300th frame, plus 100/200/250/255+ so a short movie (the
     * 259-frame EA SPORTS transition) shows how far the game actually played it. */
    if ((d->popped % 300) == 1 || d->popped == 100 || d->popped == 200 || d->popped == 250 ||
        (d->popped >= 255 && d->popped <= 262))
        std::fprintf(stderr, "[vp6bridge] desc=0x%08X frame %llu (queue ok, %llu underruns)%s\n", desc,
                     (unsigned long long)d->popped, (unsigned long long)d->underruns,
                     d->finished() ? " [decoder at EOF]" : "");
}

/* [#178] renderer query: the guest frame index a movie Y plane at `ea` holds,
 * -1 if it is not a movie plane. *wh = (w << 16) | h of that movie. */
extern "C" int nhl_vp6_frame_of(uint32_t ea, uint32_t* wh)
{
    std::lock_guard<std::mutex> lk(g_show_lock);
    for (int i = 0; i < 16; i++) if (g_show[i].y && g_show[i].y == ea) { if (wh) *wh = g_show[i].wh; return (int)g_show[i].idx; }
    return -1;
}

/* [#179] renderer query at draw-record time: 0/1/2 = the Y/U/V plane of a movie ring
 * slot (with that slot's guest frame index and movie size), -1 = not a movie plane. */
extern "C" int nhl_vp6_plane_of(uint32_t ea, uint32_t* wh, int* idx)
{
    if (!ea) return -1;
    std::lock_guard<std::mutex> lk(g_show_lock);
    for (int i = 0; i < 16; i++) {
        const ShowEnt& s = g_show[i];
        const int p = (s.y && s.y == ea) ? 0 : (s.u && s.u == ea) ? 1 : (s.v && s.v == ea) ? 2 : -1;
        if (p >= 0) { if (wh) *wh = s.wh; if (idx) *idx = (int)s.idx; return p; }
    }
    return -1;
}

extern "C" void nhl_vp6_bridge_init(void)
{
    const char* on = std::getenv("NHL_VP6_BRIDGE");
    if (on && *on == '0') return;
    const char* ff = std::getenv("NHL_FFMPEG");
    const char* dir = std::getenv("NHL_MOVIE_DIR");
    if (!ff || !*ff || !dir || !*dir) {
        std::fprintf(stderr, "[vp6bridge] off: NHL_FFMPEG / NHL_MOVIE_DIR not set\n");
        return;
    }
    if (GetFileAttributesA(ff) == INVALID_FILE_ATTRIBUTES) {
        std::fprintf(stderr, "[vp6bridge] off: ffmpeg not found at %s\n", ff);
        return;
    }
    const char* fl = std::getenv("NHL_VP6_FLIP");
    g_flip = !(fl && *fl == '0');
    g_ffmpeg = ff;
    load_movies(dir);
    if (g_movies.empty()) { std::fprintf(stderr, "[vp6bridge] off: no carved movies in %s\n", dir); return; }
    g_fs_read_hook = &on_read;
    if (const char* sh = std::getenv("NHL_VP6_SHOW"); sh && *sh != '0') g_vp6_frame_of_hook = &nhl_vp6_frame_of;   /* #178 */
    g_vp6_plane_hook = &nhl_vp6_plane_of;   /* #179: record-time movie plane snapshots */
    std::fprintf(stderr, "[vp6bridge] on: %zu movies, ffmpeg %s\n", g_movies.size(), ff);
}
