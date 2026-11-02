/*
 * NHL: standalone settings (launcher phase 1, #221).
 *
 * Every NHL boot used to depend on tools\run_nhl.ps1: ~20 environment switches
 * the game cannot run without (SPU interpreter mode, GCM semaphore/ring fixes,
 * VP_NOFIXPROJ...) and the paths of the sandbox (disc, update, cache, movies,
 * ffmpeg). A player has no script, so the exe now carries those switches as
 * built-in defaults and reads its paths from nhl_settings.ini next to the exe
 * (or --config <file>), which NHLLauncher writes.
 *
 * Precedence, highest first: the real environment > ini [env] > ini [paths] /
 * [video] > built-in defaults. So run_nhl.ps1 (which sets everything itself and
 * passes NHL_NO_DEFAULTS=1) boots exactly as before, and any switch can still be
 * flipped from the environment for a test.
 *
 * ini format: [section] lines, key = value, full-line ; or # comments. Relative
 * paths resolve against [paths] install (default: the exe's folder).
 *   [paths]  install eboot disc hdd0 hdd1 user movies ffmpeg edat_cache
 *            shader_cache dev_flash
 *   [video]  fullscreen vrr msaa aniso render_scale shadow_scale fx_scale hz60
 *            (#251: enhance is gone; an old key is ignored)
 *   [env]    ANY_VAR = value
 * user = the working directory: saves (gamedata\dev_hdd0\home) and the shader
 * cache live there.
 */
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

extern "C" void nhl_env_cache_reset(void);   /* boot_main.cpp */

namespace {

/* run_nhl.ps1's defaults (see the comments there for what each one fixes). */
const char* const kDefaults[][2] = {
    { "PS3_VERBOSE", "0" },
    { "RD_SPU_INTERP", "1" }, { "RD_SPU_INTERP_ASYNC", "1" }, { "SPU_INTERP_UNLIFTED", "1" },
    { "GCM_SEMA_ACQUIRE", "1" }, { "GCM_NO_AUTOMAP", "1" }, { "GCM_TITLE_RECYCLE", "0" },
    { "SPU_MICROSTEP", "0" }, { "SPU_RESIDENT_REJOIN", "1" }, { "SPU_POLL_LSA", "3EE00" },
    { "GCM_REF_IMMEDIATE", "1" }, { "GCM_PARK_GRACE_MS", "10000" },
    { "PS3_TICK_LR", "0077363C" }, { "PS3_TICK_TID", "1" },
    { "VP_NOFIXPROJ", "1" },
    { "NHL_SPU_PRIO", "auto" }, { "NHL_SPU_CORES", "auto" },   // #258: on for >= 8 cores + SMT
    { "NHL_FULLSCREEN", "1" }, { "NHL_VRR", "1" },
    { "PS3_TITLE", "NHL Legacy" },
};

struct Kv { std::string sec, key, val; };

std::string s_eboot;

bool env_set(const char* k)
{
    SetLastError(0);
    char c;
    return GetEnvironmentVariableA(k, &c, 1) != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
}

void set_default(const char* k, const std::string& v)
{
    if (!env_set(k)) SetEnvironmentVariableA(k, v.c_str());
}

std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string lower(std::string s)
{
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}

/* Absolute, forward slashes (the runtime's path code splices '/' everywhere). */
std::string abs_path(const std::string& base, const std::string& p)
{
    std::filesystem::path q = std::filesystem::path(p);
    if (q.is_relative()) q = std::filesystem::path(base) / q;
    std::string s = q.lexically_normal().string();
    for (char& c : s) if (c == '\\') c = '/';
    while (s.size() > 3 && s.back() == '/') s.pop_back();
    return s;
}

std::vector<Kv> read_ini(const std::string& file)
{
    std::vector<Kv> out;
    FILE* f = fopen(file.c_str(), "rb");
    if (!f) return out;
    std::string sec;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        std::string l = trim(line);
        if (l.size() >= 3 && (unsigned char)l[0] == 0xEF) l = l.substr(3);   /* UTF-8 BOM */
        if (l.empty() || l[0] == ';' || l[0] == '#') continue;
        if (l[0] == '[') { sec = lower(trim(l.substr(1, l.find(']') - 1))); continue; }
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        out.push_back({ sec, trim(l.substr(0, eq)), trim(l.substr(eq + 1)) });
    }
    fclose(f);
    return out;
}

}  // namespace

/* Path of the update EBOOT (decrypted ELF) from [paths] eboot, or NULL. */
const char* nhl_config_eboot(void) { return s_eboot.empty() ? nullptr : s_eboot.c_str(); }

/* Called first thing in main(), before anything reads the environment.
 * config: --config <file>, or NULL for <exe dir>\nhl_settings.ini. */
void nhl_config_load(const char* config)
{
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    const std::string exe_dir = std::filesystem::path(exe).parent_path().string();
    const std::string file = config ? abs_path(exe_dir, config) : abs_path(exe_dir, "nhl_settings.ini");
    const std::vector<Kv> ini = read_ini(file);
    if (!ini.empty()) printf("[config] %s (%zu entries)\n", file.c_str(), ini.size());
    else if (config) printf("[config] WARNING: %s missing or empty\n", file.c_str());

    std::string install = exe_dir;
    for (const Kv& e : ini)
        if (e.sec == "paths" && lower(e.key) == "install" && !e.val.empty()) install = abs_path(exe_dir, e.val);

    for (const Kv& e : ini)
        if (e.sec == "env" && !e.key.empty()) set_default(e.key.c_str(), e.val);

    static const char* const kPathVars[][2] = {
        { "disc", "PS3_VFS_ROOT" }, { "hdd0", "PS3_HDD0_ROOT" }, { "hdd1", "PS3_HDD1_ROOT" },
        { "movies", "NHL_MOVIE_DIR" }, { "ffmpeg", "NHL_FFMPEG" }, { "edat_cache", "PS3_EDAT_CACHE_DIR" },
        { "shader_cache", "SHADER_CACHE_DIR" }, { "dev_flash", "PS3_DEV_FLASH" },
    };
    std::string user;
    bool frame_cap30 = false;   /* #267 */
    for (const Kv& e : ini) {
        if (e.val.empty()) continue;
        const std::string k = lower(e.key);
        if (e.sec == "paths") {
            if (k == "eboot") s_eboot = abs_path(install, e.val);
            else if (k == "user") user = abs_path(install, e.val);
            for (auto& pv : kPathVars)
                if (k == pv[0]) set_default(pv[1], abs_path(install, e.val));
        } else if (e.sec == "video") {
            if (k == "fullscreen") set_default("NHL_FULLSCREEN", e.val);
            else if (k == "vrr") set_default("NHL_VRR", e.val);
            else if (k == "msaa") set_default("NHL_MSAA", e.val);         /* #249: 0 2 4 8 */
            else if (k == "aniso") set_default("NHL_ANISO", e.val);       /* #252: 1 (off) 2 4 8 16 */
            else if (k == "render_scale") set_default("NHL_RENDER_SCALE", e.val);   /* #253: 1 native 1.5 2 3 */
            else if (k == "shadow_scale") set_default("NHL_SHADOW_SCALE", e.val);   /* #254: 1 2 4 */
            else if (k == "fx_scale") set_default("NHL_FX_SCALE", e.val);           /* #254: 1 2 4 */
            /* #256: 60 fps menus & cutscenes = the vblank clock doubled. The game paces its front
             * end / cinematics on two vblanks a frame (and times them by measured dt), so they run at
             * 60 with correct speed; matches keep their fixed 30 Hz tick. */
            else if (k == "hz60") set_default("RSX_VBLANK_HZ", e.val == "1" ? "119.88" : "59.94");
            else if (k == "frame_cap") frame_cap30 = e.val == "30";
            else if (k == "edge_native") set_default("NHL_EDGE_NATIVE", e.val);     /* #293 */
            else if (k == "no_crowd") set_default("NHL_NO_CROWD", e.val);           /* #295 */
            else if (k == "ice_bounce") set_default("NHL_ICE_BOUNCE", e.val);       /* #298: 0 = no ice light on players */
        } else if (e.sec == "game") {
            if (k == "skip_intro") set_default("NHL_SKIP_INTRO", e.val);            /* #297 */
        }
    }
    /* [NHL local patch #267] frame_cap = 30: one guest flip per 33 ms (#262's GCM_FLIP_EVERY),
     * so a match draws one frame per 30 Hz game update instead of 2-4 -- the setting that helps
     * a slow CPU (deck5: +12% game speed at 4 cores / 2.3 GHz). With hz60 the vblank clock is
     * doubled, so that is every 4th vblank. */
    if (frame_cap30) {
        char hz[32] = "";
        const double v = GetEnvironmentVariableA("RSX_VBLANK_HZ", hz, sizeof hz) ? atof(hz) : 59.94;
        set_default("GCM_FLIP_EVERY", v > 90.0 ? "4" : "2");
    }

    if (!env_set("NHL_NO_DEFAULTS"))
        for (auto& d : kDefaults) set_default(d[0], d[1]);

    /* [NHL local patch #244] Bug-report defaults for launcher installs (an ini was read):
     * F9 writes a frame capture to <install>\logs\captures, and the cheap per-second
     * counters run (HITCH=1 without the thread sampler), which also arms the #241 freeze
     * dump. The launcher's "Report a problem" zips these with the session's logs. */
    if (!env_set("NHL_NO_DEFAULTS") && !ini.empty()) {
        std::string cap = install + "/logs/captures";
        for (char& c : cap) if (c == '\\') c = '/';
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(cap), ec);
        set_default("RSX_CAP_DIR", cap);
        set_default("HITCH", "1");
        set_default("HITCH_NOSAMPLE", "1");
        /* [#254b] the launcher's displayed defaults for [video] keys an older ini lacks: without
         * these an ini written before the setting existed ran 1x while the launcher showed
         * High / Full (game_20261028_225412: shadow maps held 1024x1024). MSAA (4) and
         * anisotropic (16) already default in the backend. */
        set_default("NHL_RENDER_SCALE", "native");
        set_default("NHL_SHADOW_SCALE", "2");
        set_default("NHL_FX_SCALE", "4");
    }

    /* Folders the game writes into must exist (the guest's mkdir is not recursive). */
    for (const char* v : { "PS3_HDD1_ROOT", "PS3_EDAT_CACHE_DIR" }) {
        char p[1024];
        if (GetEnvironmentVariableA(v, p, sizeof p) - 1u < sizeof p - 1) {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(p), ec);
        }
    }
    if (!user.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(user), ec);
        if (SetCurrentDirectoryA(user.c_str())) printf("[config] user folder %s\n", user.c_str());
        else printf("[config] WARNING: cannot enter user folder %s\n", user.c_str());
    }
    nhl_env_cache_reset();   /* anything looked up before this saw the old environment */
}
