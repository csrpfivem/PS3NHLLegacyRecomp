/*
 * ps3recomp - cellPad HLE implementation
 *
 * Reads real gamepad input from the host and translates to PS3 pad format.
 *
 * Backend selection:
 *   - Windows default: XInput (no extra dependencies)
 *   - Everywhere else / if PS3RECOMP_PAD_USE_SDL2 is defined: SDL2 GameController
 *
 * Define PS3RECOMP_PAD_USE_SDL2 to force SDL2 backend on Windows.
 */

#include <time.h>
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>
#include "cellPad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../runtime/platform/win32_compat.h"   /* GetTickCount64 on POSIX */
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: guest EA -> host */

/* ---------------------------------------------------------------------------
 * Backend selection
 * -----------------------------------------------------------------------*/

#if defined(PS3RECOMP_PAD_USE_SDL2)
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#elif defined(_WIN32)
  #define PAD_BACKEND_SDL2  0
  #define PAD_BACKEND_XINPUT 1
#else
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#endif

#if PAD_BACKEND_XINPUT
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <xinput.h>
  #pragma comment(lib, "xinput.lib")
#endif

#if PAD_BACKEND_SDL2
  #include <SDL2/SDL.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

#define PAD_MAX_HOST_PORTS  4  /* XInput supports max 4; SDL may support more */

typedef struct {
    int  connected;
    u16  buttons;           /* CELL_PAD_CTRL_* bitmask */
    u8   analog_lx;         /* 0-255, center=128 */
    u8   analog_ly;
    u8   analog_rx;
    u8   analog_ry;
    u8   trigger_l2;        /* 0-255 */
    u8   trigger_r2;        /* 0-255 */
    /* Pressure-sensitive face buttons (0-255) */
    u8   press_right;
    u8   press_left;
    u8   press_up;
    u8   press_down;
    u8   press_triangle;
    u8   press_circle;
    u8   press_cross;
    u8   press_square;
    u8   press_l1;
    u8   press_r1;
} PadHostState;

static int           s_pad_initialized = 0;
static u32           s_max_connect = 0;
static u32           s_port_setting[CELL_PAD_MAX_PORT_NUM];
static PadHostState  s_host_state[PAD_MAX_HOST_PORTS];
/* Per-port "a report has arrived since your last read" flag; see cellPadGetData. */
static int s_data_fresh[PAD_MAX_HOST_PORTS];

#if PAD_BACKEND_SDL2
static SDL_GameController* s_sdl_controllers[PAD_MAX_HOST_PORTS];
static int s_sdl_inited = 0;
#endif

/* ---------------------------------------------------------------------------
 * XInput backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_XINPUT

/* Deadzone for analog sticks (same as XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) */
#define PAD_STICK_DEADZONE  7849
#define PAD_TRIGGER_THRESHOLD 30

static u8 pad_xinput_stick_to_u8(short raw, short deadzone)
{
    float normalized;
    if (raw > deadzone)
        normalized = (float)(raw - deadzone) / (float)(32767 - deadzone);
    else if (raw < -deadzone)
        normalized = (float)(raw + deadzone) / (float)(32767 - deadzone);
    else
        normalized = 0.0f;

    /* Map -1.0..1.0 to 0..255 with center at 128 */
    int val = (int)(normalized * 127.0f) + 128;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

/* Keyboard fallback for port 0.
 *
 * The only backend here is XInput, so on a machine with no controller plugged
 * in a title gets a pad that is reported present and never presses anything --
 * it can be watched but not played. That is what The Simpsons Arcade Game hit:
 * it reached its attract loop and no input existed to start a game.
 *
 * Only fills in for port 0, only when XInput found nothing there, so a real
 * controller always wins and nothing changes for a port that has one. Keys are
 * read only while a window of THIS process is in the foreground, so typing in
 * another application does not drive the game. Set PAD_NO_KEYBOARD=1 to
 * disable it entirely.
 *
 * Arrows = d-pad, Z/X/A/S = cross/circle/square/triangle, Q/W = L1/R1,
 * 1/2 = L2/R2, Enter = START, Tab = SELECT. The left stick mirrors the d-pad
 * so a title that reads the stick instead is playable too. */
#ifdef _WIN32
static int pad_host_window_focused(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static void pad_poll_keyboard(void)
{
    static int off = -1;
    if (off < 0) off = getenv("PAD_NO_KEYBOARD") ? 1 : 0;
    if (off) return;

    PadHostState* hs = &s_host_state[0];
    if (!pad_host_window_focused()) {
        /* Release everything on focus loss, or a key held while alt-tabbing
         * would stay down forever. */
        hs->buttons = 0;
        hs->analog_lx = hs->analog_ly = 128;
        hs->analog_rx = hs->analog_ry = 128;
        hs->connected = 1;
        return;
    }

    static const struct { int vk; u16 btn; } map[] = {
        { VK_UP,     CELL_PAD_CTRL_UP },      { VK_DOWN,  CELL_PAD_CTRL_DOWN },
        { VK_LEFT,   CELL_PAD_CTRL_LEFT },    { VK_RIGHT, CELL_PAD_CTRL_RIGHT },
        { 'Z',       CELL_PAD_CTRL_CROSS },   { 'X',      CELL_PAD_CTRL_CIRCLE },
        { 'A',       CELL_PAD_CTRL_SQUARE },  { 'S',      CELL_PAD_CTRL_TRIANGLE },
        { 'Q',       CELL_PAD_CTRL_L1 },      { 'W',      CELL_PAD_CTRL_R1 },
        { '1',       CELL_PAD_CTRL_L2 },      { '2',      CELL_PAD_CTRL_R2 },
        { VK_RETURN, CELL_PAD_CTRL_START },   { VK_TAB,   CELL_PAD_CTRL_SELECT },
    };

    u16 btns = 0;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (GetAsyncKeyState(map[i].vk) & 0x8000) btns |= map[i].btn;

    hs->buttons   = btns;
    hs->connected = 1;
    hs->analog_lx = (u8)((btns & CELL_PAD_CTRL_LEFT) ? 0 :
                         (btns & CELL_PAD_CTRL_RIGHT) ? 255 : 128);
    hs->analog_ly = (u8)((btns & CELL_PAD_CTRL_UP) ? 0 :
                         (btns & CELL_PAD_CTRL_DOWN) ? 255 : 128);
    hs->analog_rx = hs->analog_ry = 128;
    hs->trigger_l2 = (u8)((btns & CELL_PAD_CTRL_L2) ? 255 : 0);
    hs->trigger_r2 = (u8)((btns & CELL_PAD_CTRL_R2) ? 255 : 0);

    { static int said = 0;
      if (!said && btns) { said = 1;
          printf("[cellPad] keyboard fallback active on port 0 (no XInput device)\n");
          fflush(stdout); } }
}
#endif

static void pad_poll_xinput(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        XINPUT_STATE state;
        memset(&state, 0, sizeof(state));

        DWORD result = XInputGetState((DWORD)i, &state);
        if (result != ERROR_SUCCESS) {
            s_host_state[i].connected = 0;
            continue;
        }

        s_host_state[i].connected = 1;
        XINPUT_GAMEPAD* gp = &state.Gamepad;

        /* Map XInput buttons to PS3 CELL_PAD_CTRL_* */
        u16 btns = 0;
        if (gp->wButtons & XINPUT_GAMEPAD_BACK)           btns |= CELL_PAD_CTRL_SELECT;
        if (gp->wButtons & XINPUT_GAMEPAD_LEFT_THUMB)     btns |= CELL_PAD_CTRL_L3;
        if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)    btns |= CELL_PAD_CTRL_R3;
        if (gp->wButtons & XINPUT_GAMEPAD_START)          btns |= CELL_PAD_CTRL_START;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_UP)        btns |= CELL_PAD_CTRL_UP;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT)     btns |= CELL_PAD_CTRL_RIGHT;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_DOWN)      btns |= CELL_PAD_CTRL_DOWN;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_LEFT)      btns |= CELL_PAD_CTRL_LEFT;
        if (gp->bLeftTrigger > PAD_TRIGGER_THRESHOLD)     btns |= CELL_PAD_CTRL_L2;
        if (gp->bRightTrigger > PAD_TRIGGER_THRESHOLD)    btns |= CELL_PAD_CTRL_R2;
        if (gp->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  btns |= CELL_PAD_CTRL_L1;
        if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) btns |= CELL_PAD_CTRL_R1;
        if (gp->wButtons & XINPUT_GAMEPAD_Y)              btns |= CELL_PAD_CTRL_TRIANGLE;
        if (gp->wButtons & XINPUT_GAMEPAD_B)              btns |= CELL_PAD_CTRL_CIRCLE;
        if (gp->wButtons & XINPUT_GAMEPAD_A)              btns |= CELL_PAD_CTRL_CROSS;
        if (gp->wButtons & XINPUT_GAMEPAD_X)              btns |= CELL_PAD_CTRL_SQUARE;

        s_host_state[i].buttons = btns;

        /* Analog sticks. PS3 Y axis is inverted vs XInput (up = 0). Reflect about
         * 128 (256 - x), NOT 255 - x: the latter turns a centered stick into 127,
         * whose bits (0x7F) alias SELECT+START and can make the guest self-exit.
         * (via sagemono, PR #42) */
        s_host_state[i].analog_lx = pad_xinput_stick_to_u8(gp->sThumbLX, PAD_STICK_DEADZONE);
        s_host_state[i].analog_ly = (u8)(256 - pad_xinput_stick_to_u8(gp->sThumbLY, PAD_STICK_DEADZONE));
        s_host_state[i].analog_rx = pad_xinput_stick_to_u8(gp->sThumbRX, PAD_STICK_DEADZONE);
        s_host_state[i].analog_ry = (u8)(256 - pad_xinput_stick_to_u8(gp->sThumbRY, PAD_STICK_DEADZONE));

        /* Triggers */
        s_host_state[i].trigger_l2 = gp->bLeftTrigger;
        s_host_state[i].trigger_r2 = gp->bRightTrigger;

        /* Pressure-sensitive buttons: XInput has digital only, so 0 or 255 */
        s_host_state[i].press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
        s_host_state[i].press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
        s_host_state[i].press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
        s_host_state[i].press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
        s_host_state[i].press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
        s_host_state[i].press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
        s_host_state[i].press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
        s_host_state[i].press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
        s_host_state[i].press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
        s_host_state[i].press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
    }
}

/* ---------------------------------------------------------------------------
 * [NHL local patch #324] Native PS5 DualSense / PS4 DualShock 4 (raw HID).
 *
 * Sony pads are not XInput devices, so without Steam Input or DS4Windows the
 * game never saw them. This reads them straight from hid.dll: USB and
 * Bluetooth, DualSense (+Edge) and DS4 v1/v2 (+wireless adapter). Rumble and a
 * per-player lightbar go out as output reports.
 *
 *  - A hot-plug thread enumerates HID every 2 s (paths are pre-filtered by
 *    VID/PID text, so no other device is ever opened). Each pad gets a reader
 *    thread; the game's poll only copies the latest parsed state (no hitch).
 *  - Ports: XInput slot i stays port i exactly as before. A Sony pad takes the
 *    lowest port XInput leaves empty and keeps it while connected. Keyboard
 *    fallback still fills port 0 only when nothing is there.
 *  - Duplicates: pads Steam Input already virtualises are listed by Steam in
 *    SDL_GAMECONTROLLER_IGNORE_DEVICES; those are skipped (SDL does the same).
 *    An exclusive owner (DS4Windows/HidHide) makes our open fail -> skipped.
 *  - Off under Wine/Proton (wine_xinput_hid already works on the Deck);
 *    PAD_SONY_HID=1 forces it on. PAD_NO_SONY_HID=1 turns it off anywhere.
 * -----------------------------------------------------------------------*/
#include <setupapi.h>
#include <hidsdi.h>
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

#define SONY_MAX_PADS 4

typedef struct {
    volatile LONG  used;          /* slot owned by a live device */
    volatile LONG  dead;          /* reader saw the device go away */
    volatile LONG  stop;
    HANDLE         h, thread, wthread, wake;
    volatile unsigned st_writes, st_wfail, st_wmax;   /* #324e stats */
    volatile unsigned long long last_rpt_ms;          /* #324g stale guard */
    WCHAR          path[512];
    int            is_ds5, is_bt;
    USHORT         in_len, out_len;
    int            port;          /* host port, -1 until assigned */
    SRWLOCK        lock;
    PadHostState   st;            /* guarded by lock */
    int            have_data;
    volatile LONG  motor_large, motor_small, led_dirty;
    int            sent_large, sent_small;
    u8             out_seq;
} SonyPad;

static SonyPad        s_sony[SONY_MAX_PADS];
static HANDLE         s_sony_hotplug = NULL;
static volatile LONG  s_sony_stop = 0;

static u32 sony_crc32(const u8* p, size_t n, u32 crc)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static u8 sony_stick(u8 raw)
{
    /* same deadzone + curve as the XInput path, so both feel identical */
    int s = ((int)raw - 128) * 258;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    return pad_xinput_stick_to_u8((short)s, PAD_STICK_DEADZONE);
}

/* d: LX LY RX RY, then the two button bytes (hat+face, shoulders/sticks/menu),
 * and the analog triggers. Both pads share this button bit layout. */
static void sony_fill(PadHostState* o, const u8* d, u8 b0, u8 b1, u8 l2, u8 r2)
{
    static const u16 hat[9] = {
        CELL_PAD_CTRL_UP, CELL_PAD_CTRL_UP | CELL_PAD_CTRL_RIGHT, CELL_PAD_CTRL_RIGHT,
        CELL_PAD_CTRL_DOWN | CELL_PAD_CTRL_RIGHT, CELL_PAD_CTRL_DOWN,
        CELL_PAD_CTRL_DOWN | CELL_PAD_CTRL_LEFT, CELL_PAD_CTRL_LEFT,
        CELL_PAD_CTRL_UP | CELL_PAD_CTRL_LEFT, 0 };
    u16 btns = hat[(b0 & 0x0F) > 8 ? 8 : (b0 & 0x0F)];
    if (b0 & 0x10) btns |= CELL_PAD_CTRL_SQUARE;
    if (b0 & 0x20) btns |= CELL_PAD_CTRL_CROSS;
    if (b0 & 0x40) btns |= CELL_PAD_CTRL_CIRCLE;
    if (b0 & 0x80) btns |= CELL_PAD_CTRL_TRIANGLE;
    if (b1 & 0x01) btns |= CELL_PAD_CTRL_L1;
    if (b1 & 0x02) btns |= CELL_PAD_CTRL_R1;
    if (b1 & 0x10) btns |= CELL_PAD_CTRL_SELECT;   /* Share / Create */
    if (b1 & 0x20) btns |= CELL_PAD_CTRL_START;    /* Options */
    if (b1 & 0x40) btns |= CELL_PAD_CTRL_L3;
    if (b1 & 0x80) btns |= CELL_PAD_CTRL_R3;
    if (l2 > PAD_TRIGGER_THRESHOLD) btns |= CELL_PAD_CTRL_L2;
    if (r2 > PAD_TRIGGER_THRESHOLD) btns |= CELL_PAD_CTRL_R2;
    /* PS button / touchpad / mute: ignored */

    o->connected  = 1;
    o->buttons    = btns;
    o->analog_lx  = sony_stick(d[0]);
    o->analog_ly  = sony_stick(d[1]);   /* Sony Y is already up = 0, like PS3 */
    o->analog_rx  = sony_stick(d[2]);
    o->analog_ry  = sony_stick(d[3]);
    o->trigger_l2 = l2;
    o->trigger_r2 = r2;
    o->press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
    o->press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
    o->press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
    o->press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
    o->press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
    o->press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
    o->press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
    o->press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
    o->press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
    o->press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
}

static int sony_parse(SonyPad* p, const u8* b, DWORD n, PadHostState* o)
{
    if (n < 10) return 0;
    if (p->is_ds5) {
        if (b[0] == 0x31 && n >= 13) {                 /* BT full report */
            const u8* d = b + 2;
            sony_fill(o, d, d[7], d[8], d[4], d[5]);
            return 1;
        }
        if (b[0] == 0x01) {
            const u8* d = b + 1;
            if (!p->is_bt && n >= 11) sony_fill(o, d, d[7], d[8], d[4], d[5]);  /* USB */
            else                      sony_fill(o, d, d[4], d[5], d[7], d[8]);  /* BT simple */
            return 1;
        }
        return 0;
    }
    if (b[0] == 0x11 && n >= 12) {                     /* DS4 BT full report */
        const u8* d = b + 3;
        sony_fill(o, d, d[4], d[5], d[7], d[8]);
        return 1;
    }
    if (b[0] == 0x01) {                                /* DS4 USB / BT simple */
        const u8* d = b + 1;
        sony_fill(o, d, d[4], d[5], d[7], d[8]);
        return 1;
    }
    return 0;
}

static void sony_write(SonyPad* p, OVERLAPPED* ov)
{
    static const u8 led[4][3] = { {0,0,64}, {64,0,0}, {0,64,0}, {64,0,40} };  /* PS player colours */
    static const u8 ds5_player[4] = { 0x04, 0x0A, 0x15, 0x1B };
    const int pi = (p->port >= 0 && p->port < 4) ? p->port : 0;
    const u8 lg = (u8)InterlockedCompareExchange(&p->motor_large, 0, 0);
    const u8 sm = (u8)InterlockedCompareExchange(&p->motor_small, 0, 0);
    const int led_now = InterlockedExchange(&p->led_dirty, 0);
    /* Windows reports 547 for BT output (the 78-byte 0x31/0x11 report + zero
     * padding, like hidapi); a 128-byte buffer silently skipped every BT write
     * (play342: writes 0, no rumble). */
    u8 r[1024];
    DWORD len = p->out_len;
    if (len == 0 || len > sizeof r) return;
    memset(r, 0, sizeof r);

    if (p->is_ds5) {
        u8* c;
        if (p->is_bt) {
            if (len < 78) return;
            r[0] = 0x31; r[1] = (u8)((p->out_seq++ & 0x0F) << 4); r[2] = 0x10;
            c = r + 3;
        } else { r[0] = 0x02; c = r + 1; }
        c[0] = 0x03;                       /* compatible vibration + haptics select */
        c[2] = sm; c[3] = lg;              /* right = weak, left = strong */
        c[38] = 0x04;                      /* compatible vibration v2 (newer firmware) */
        if (led_now) {
            c[1]  = 0x04 | 0x10;           /* lightbar + player LEDs */
            c[38] |= 0x02; c[41] = 0x02;   /* end the blue boot animation */
            c[43] = ds5_player[pi];
            c[44] = led[pi][0]; c[45] = led[pi][1]; c[46] = led[pi][2];
        }
        if (p->is_bt) {
            u8 seed = 0xA2;
            u32 crc = sony_crc32(&seed, 1, 0);
            crc = sony_crc32(r, 74, crc);
            r[74] = (u8)crc; r[75] = (u8)(crc >> 8); r[76] = (u8)(crc >> 16); r[77] = (u8)(crc >> 24);
        }
    } else {
        u8* c;
        if (p->is_bt) {
            if (len < 78) return;
            r[0] = 0x11; r[1] = 0xC0 | 0x04; r[3] = 0x07; c = r + 6;
        } else { r[0] = 0x05; r[1] = 0x07; c = r + 4; }
        c[0] = sm; c[1] = lg;
        c[2] = led[pi][0]; c[3] = led[pi][1]; c[4] = led[pi][2];
        if (p->is_bt) {
            u8 seed = 0xA2;
            u32 crc = sony_crc32(&seed, 1, 0);
            crc = sony_crc32(r, 74, crc);
            r[74] = (u8)crc; r[75] = (u8)(crc >> 8); r[76] = (u8)(crc >> 16); r[77] = (u8)(crc >> 24);
        }
    }

    ResetEvent(ov->hEvent);
    DWORD wr = 0;
    const unsigned long long t0 = GetTickCount64();
    int ok = 1;
    if (!WriteFile(p->h, r, len, &wr, ov)) {
        if (GetLastError() == ERROR_IO_PENDING) {
            /* CancelIoEx on THIS write only: plain CancelIo would also kill
             * the reader's pending ReadFile on the same handle. */
            if (WaitForSingleObject(ov->hEvent, 100) != WAIT_OBJECT_0) CancelIoEx(p->h, ov);
            ok = GetOverlappedResult(p->h, ov, &wr, TRUE);
        } else ok = 0;
    }
    const unsigned dt = (unsigned)(GetTickCount64() - t0);
    p->st_writes++;
    if (!ok) p->st_wfail++;
    if (dt > p->st_wmax) p->st_wmax = dt;
    p->sent_large = lg; p->sent_small = sm;
}

/* Output reports get their own thread: a Bluetooth write can take tens of ms,
 * and with rumble changing every frame in a match it starved the reader
 * (play341: BT DualSense drove menus, nothing in a match). */
static DWORD WINAPI sony_writer(LPVOID arg)
{
    SonyPad* p = (SonyPad*)arg;
    OVERLAPPED wov;
    memset(&wov, 0, sizeof wov);
    wov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    sony_write(p, &wov);   /* lightbar on connect */
    /* [#324g] Bluetooth: only the connect-time lightbar write. Rumble writes
     * over BT stalled the link (play343: reports 400/s -> 11-76/s once rumble
     * began; play344: a failed write, then a 14 s gap and no reports after,
     * last state CROSS held). PAD_SONY_BT_RUMBLE=1 re-enables them. */
    static int bt_rumble = -1;
    if (bt_rumble < 0) bt_rumble = getenv("PAD_SONY_BT_RUMBLE") ? 1 : 0;
    if (p->is_bt && !bt_rumble) {
        while (!p->stop) WaitForSingleObject(p->wake, 500);
        CloseHandle(wov.hEvent);
        return 0;
    }
    while (!p->stop) {
        WaitForSingleObject(p->wake, 500);
        if (p->stop) break;
        if (p->led_dirty || p->motor_large != p->sent_large || p->motor_small != p->sent_small)
            sony_write(p, &wov);
    }
    CloseHandle(wov.hEvent);
    return 0;
}

static DWORD WINAPI sony_reader(LPVOID arg)
{
    SonyPad* p = (SonyPad*)arg;
    OVERLAPPED rov;
    memset(&rov, 0, sizeof rov);
    rov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    u8 buf[1024];
    DWORD want = p->in_len ? p->in_len : 64;
    if (want > sizeof buf) want = sizeof buf;
    unsigned long long last_log = GetTickCount64();
    unsigned n_full = 0, n_simple = 0, n_other = 0, max_gap = 0;
    unsigned long long last_rpt = 0;
    DWORD gle = 0;

    while (!p->stop) {
        ResetEvent(rov.hEvent);
        DWORD n = 0;
        if (!ReadFile(p->h, buf, want, &n, &rov) && (gle = GetLastError()) != ERROR_IO_PENDING) break;
        while (!p->stop && WaitForSingleObject(rov.hEvent, 500) == WAIT_TIMEOUT) {}
        if (p->stop) break;
        if (!GetOverlappedResult(p->h, &rov, &n, FALSE)) { gle = GetLastError(); break; }   /* unplugged */
        PadHostState s;
        memset(&s, 0, sizeof s);
        { const unsigned long long t = GetTickCount64();
          if (last_rpt && t - last_rpt > max_gap) max_gap = (unsigned)(t - last_rpt);
          last_rpt = t;
          p->last_rpt_ms = t; }
        if (n && (buf[0] == 0x31 || buf[0] == 0x11)) n_full++;
        else if (n && buf[0] == 0x01) n_simple++;
        else n_other++;
        if (sony_parse(p, buf, n, &s)) {
            AcquireSRWLockExclusive(&p->lock);
            p->st = s; p->have_data = 1;
            ReleaseSRWLockExclusive(&p->lock);
        }
        /* [#324e] stats every 10 s (diagnostic for BT; cheap) */
        const unsigned long long now = GetTickCount64();
        if (now - last_log >= 10000) {
            printf("[cellPad] %s %s port %d: %u reports/10s (full %u, simple %u, other %u) "
                   "max gap %u ms, writes %u fail %u max %u ms, btns 0x%04x\n",
                   p->is_ds5 ? "DualSense" : "DualShock 4", p->is_bt ? "BT" : "USB", p->port,
                   n_full + n_simple + n_other, n_full, n_simple, n_other, max_gap,
                   p->st_writes, p->st_wfail, p->st_wmax, s.buttons);
            fflush(stdout);
            n_full = n_simple = n_other = 0; max_gap = 0;
            p->st_writes = p->st_wfail = p->st_wmax = 0;
            last_log = now;
        }
    }
    if (!p->stop) { printf("[cellPad] reader stopped, error %lu\n", (unsigned long)gle); fflush(stdout); }
    CancelIoEx(p->h, &rov);
    { DWORD n; GetOverlappedResult(p->h, &rov, &n, TRUE); }
    CloseHandle(rov.hEvent);
    InterlockedExchange(&p->dead, 1);
    return 0;
}

static int sony_ignored_by_steam(USHORT vid, USHORT pid)
{
    const char* e = getenv("SDL_GAMECONTROLLER_IGNORE_DEVICES");
    if (!e || !*e) return 0;
    char want[16];
    snprintf(want, sizeof want, "0x%04x/0x%04x", vid, pid);
    char low[1024]; size_t i = 0;
    for (; e[i] && i < sizeof low - 1; i++) low[i] = (char)tolower((unsigned char)e[i]);
    low[i] = 0;
    return strstr(low, want) != NULL;
}

static int sony_path_is_candidate(const WCHAR* path)
{
    WCHAR low[512]; size_t i = 0;
    for (; path[i] && i < 511; i++) low[i] = towlower(path[i]);
    low[i] = 0;
    if (!wcsstr(low, L"054c")) return 0;
    static const WCHAR* pids[] = { L"05c4", L"09cc", L"0ba0", L"0ce6", L"0df2" };
    for (int k = 0; k < 5; k++) {
        WCHAR a[16], b[16];
        swprintf(a, 16, L"pid_%ls", pids[k]);
        swprintf(b, 16, L"pid&%ls", pids[k]);
        if (wcsstr(low, a) || wcsstr(low, b)) return 1;
    }
    return 0;
}

static void sony_try_open(const WCHAR* path)
{
    for (int i = 0; i < SONY_MAX_PADS; i++)
        if (s_sony[i].used && !wcscmp(s_sony[i].path, path)) return;   /* already ours */
    int slot = -1;
    for (int i = 0; i < SONY_MAX_PADS; i++) if (!s_sony[i].used) { slot = i; break; }
    if (slot < 0) return;

    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) return;   /* exclusively owned (DS4Windows) or gone */

    HIDD_ATTRIBUTES at; memset(&at, 0, sizeof at); at.Size = sizeof at;
    PHIDP_PREPARSED_DATA pp = NULL; HIDP_CAPS caps; memset(&caps, 0, sizeof caps);
    int ok = HidD_GetAttributes(h, &at) && at.VendorID == 0x054C &&
             HidD_GetPreparsedData(h, &pp) && HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS &&
             caps.UsagePage == 0x01 && caps.Usage == 0x05;
    if (pp) HidD_FreePreparsedData(pp);
    int ds5 = at.ProductID == 0x0CE6 || at.ProductID == 0x0DF2;
    int ds4 = at.ProductID == 0x05C4 || at.ProductID == 0x09CC || at.ProductID == 0x0BA0;
    if (!ok || (!ds5 && !ds4) || sony_ignored_by_steam(at.VendorID, at.ProductID)) {
        static int said_steam = 0;
        if (ok && !said_steam && sony_ignored_by_steam(at.VendorID, at.ProductID)) {
            said_steam = 1;
            printf("[cellPad] Sony pad %04x:%04x left to Steam Input\n", at.VendorID, at.ProductID);
            fflush(stdout);
        }
        CloseHandle(h);
        return;
    }

    SonyPad* p = &s_sony[slot];
    memset(p, 0, sizeof *p);
    InitializeSRWLock(&p->lock);
    p->h = h;
    wcsncpy(p->path, path, 511);
    p->is_ds5  = ds5;
    p->in_len  = caps.InputReportByteLength;
    p->out_len = caps.OutputReportByteLength;
    p->is_bt   = caps.InputReportByteLength > 64 || wcsstr(p->path, L"00001124") != NULL;
    p->port    = -1;
    p->sent_large = p->sent_small = -1;
    p->led_dirty = 1;

    if (p->is_bt) {
        /* Reading the calibration feature report switches the pad from the
         * short BT report to the full one (0x11 DS4 / 0x31 DualSense). */
        u8 f[1024]; memset(f, 0, sizeof f);
        f[0] = ds5 ? 0x05 : 0x02;
        ULONG fl = caps.FeatureReportByteLength ? caps.FeatureReportByteLength : 64;
        if (fl > sizeof f) fl = sizeof f;
        HidD_GetFeature(h, f, fl);
    }

    p->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    InterlockedExchange(&p->used, 1);
    /* [#324f] Latency: keep only the 2 newest reports in the driver queue, and
     * run the reader time-critical (it sleeps on I/O almost all the time).
     * play342: under game load the reader got 15-100 of ~250 reports/s and the
     * default 32-report queue served input up to ~2 s old ("kept skating after
     * I let go", missed line changes). */
    HidD_SetNumInputBuffers(h, 2);
    p->thread = CreateThread(NULL, 0, sony_reader, p, 0, NULL);
    if (p->thread) SetThreadPriority(p->thread, THREAD_PRIORITY_TIME_CRITICAL);
    p->wthread = CreateThread(NULL, 0, sony_writer, p, 0, NULL);
    printf("[cellPad] %s (%s) connected (in %u out %u feat %u)\n", ds5 ? "DualSense" : "DualShock 4",
           p->is_bt ? "Bluetooth" : "USB", caps.InputReportByteLength,
           caps.OutputReportByteLength, caps.FeatureReportByteLength);
    fflush(stdout);
}

static void sony_reap(SonyPad* p)
{
    InterlockedExchange(&p->stop, 1);
    if (p->wake) SetEvent(p->wake);
    if (p->thread) { WaitForSingleObject(p->thread, 2000); CloseHandle(p->thread); }
    if (p->wthread) { WaitForSingleObject(p->wthread, 2000); CloseHandle(p->wthread); }
    if (p->h && p->h != INVALID_HANDLE_VALUE) CloseHandle(p->h);
    if (p->wake) CloseHandle(p->wake);
    printf("[cellPad] %s disconnected\n", p->is_ds5 ? "DualSense" : "DualShock 4");
    fflush(stdout);
    p->h = NULL; p->thread = NULL; p->wake = NULL;
    InterlockedExchange(&p->used, 0);
}

static DWORD WINAPI sony_hotplug(LPVOID arg)
{
    (void)arg;
    GUID g; HidD_GetHidGuid(&g);
    while (!s_sony_stop) {
        for (int i = 0; i < SONY_MAX_PADS; i++)
            if (s_sony[i].used && s_sony[i].dead) sony_reap(&s_sony[i]);

        HDEVINFO di = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (di != INVALID_HANDLE_VALUE) {
            SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize = sizeof ifd;
            for (DWORD k = 0; SetupDiEnumDeviceInterfaces(di, NULL, &g, k, &ifd); k++) {
                union { SP_DEVICE_INTERFACE_DETAIL_DATA_W d; u8 raw[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + 1024]; } u;
                u.d.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, &u.d, sizeof u, NULL, NULL)) continue;
                if (sony_path_is_candidate(u.d.DevicePath)) sony_try_open(u.d.DevicePath);
            }
            SetupDiDestroyDeviceInfoList(di);
        }
        for (int t = 0; t < 20 && !s_sony_stop; t++) Sleep(100);
    }
    return 0;
}

static int sony_enabled(void)
{
    if (getenv("PAD_NO_SONY_HID")) return 0;
    if (getenv("PAD_SONY_HID")) return 1;
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    if (nt && GetProcAddress(nt, "wine_get_version")) return 0;   /* Proton: leave to wine */
    return 1;
}

/* After XInput: give each Sony pad a port XInput left empty. */
static void pad_merge_sony(void)
{
    if (!s_sony_hotplug) return;
    int taken[PAD_MAX_HOST_PORTS];
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) taken[i] = s_host_state[i].connected;

    /* Pads already on a port keep it; only new ones (or ones whose port
     * XInput took) pick a free port. Without this pass a pad re-plugged into
     * a lower s_sony slot stole port 0 from the pad playing on it (play338). */
    for (int i = 0; i < SONY_MAX_PADS; i++) {
        SonyPad* p = &s_sony[i];
        if (p->used && !p->dead && p->port >= 0) {
            if (taken[p->port]) p->port = -1;   /* XInput claimed it */
            else taken[p->port] = 2;            /* held by this Sony pad */
        }
    }

    for (int i = 0; i < SONY_MAX_PADS; i++) {
        SonyPad* p = &s_sony[i];
        if (!p->used || p->dead) { if (!p->used) p->port = -1; continue; }
        /* Move down to a lower free port (e.g. player 1 unplugged): the game
         * drives a match from port 0, so a lone pad stuck on port 1 played
         * menus only (play337, DS4 red light). */
        int low = -1;
        for (int k = 0; k < PAD_MAX_HOST_PORTS; k++) if (!taken[k]) { low = k; break; }
        if (low >= 0 && (p->port < 0 || low < p->port)) {
            if (p->port >= 0) {
                memset(&s_host_state[p->port], 0, sizeof(PadHostState));
                taken[p->port] = 0;
            }
            p->port = low;
            InterlockedExchange(&p->led_dirty, 1);
            SetEvent(p->wake);
            printf("[cellPad] %s on port %d\n", p->is_ds5 ? "DualSense" : "DualShock 4", p->port);
            fflush(stdout);
        }
        if (p->port < 0) continue;
        taken[p->port] = 1;
        /* [#324g] No report for 500 ms (BT dropout): neutral, not the last
         * state -- a stale held stick/button is what made players drift. */
        const int stale = p->last_rpt_ms && GetTickCount64() - p->last_rpt_ms > 500;
        AcquireSRWLockShared(&p->lock);
        if (p->have_data && !stale) s_host_state[p->port] = p->st;
        else { memset(&s_host_state[p->port], 0, sizeof(PadHostState));
               s_host_state[p->port].connected = 1;
               s_host_state[p->port].analog_lx = s_host_state[p->port].analog_ly = 128;
               s_host_state[p->port].analog_rx = s_host_state[p->port].analog_ry = 128; }
        ReleaseSRWLockShared(&p->lock);
    }
}

/* Returns 1 if a Sony pad owns host port `port` (rumble routed there). */
static int sony_rumble(u32 port, u8 large, u8 small)
{
    for (int i = 0; i < SONY_MAX_PADS; i++) {
        SonyPad* p = &s_sony[i];
        if (p->used && !p->dead && p->port == (int)port) {
            InterlockedExchange(&p->motor_large, large);
            InterlockedExchange(&p->motor_small, small);
            if (large != p->sent_large || small != p->sent_small) SetEvent(p->wake);
            return 1;
        }
    }
    return 0;
}

static void pad_init_backend(void)
{
    /* XInput needs no explicit init */
    if (!s_sony_hotplug && sony_enabled()) {
        s_sony_stop = 0;
        s_sony_hotplug = CreateThread(NULL, 0, sony_hotplug, NULL, 0, NULL);
    }
}

static void pad_shutdown_backend(void)
{
    if (s_sony_hotplug) {
        InterlockedExchange(&s_sony_stop, 1);
        WaitForSingleObject(s_sony_hotplug, 3000);
        CloseHandle(s_sony_hotplug);
        s_sony_hotplug = NULL;
        for (int i = 0; i < SONY_MAX_PADS; i++) if (s_sony[i].used) sony_reap(&s_sony[i]);
    }
}

#endif /* PAD_BACKEND_XINPUT */

/* ---------------------------------------------------------------------------
 * SDL2 backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_SDL2

static u8 pad_sdl_axis_to_u8(int raw)
{
    /* SDL axis: -32768..32767 -> 0..255 with center at 128 */
    int val = ((raw + 32768) * 255) / 65535;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

static u8 pad_sdl_trigger_to_u8(int raw)
{
    /* SDL trigger: 0..32767 -> 0..255 */
    int val = (raw * 255) / 32767;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

static void pad_poll_sdl2(void)
{
    SDL_GameControllerUpdate();

    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (!s_sdl_controllers[i]) {
            /* Try to open newly connected controllers */
            if (SDL_IsGameController(i)) {
                s_sdl_controllers[i] = SDL_GameControllerOpen(i);
            }
        }

        SDL_GameController* gc = s_sdl_controllers[i];
        if (!gc || !SDL_GameControllerGetAttached(gc)) {
            s_host_state[i].connected = 0;
            if (gc) {
                SDL_GameControllerClose(gc);
                s_sdl_controllers[i] = NULL;
            }
            continue;
        }

        s_host_state[i].connected = 1;

        u16 btns = 0;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK))          btns |= CELL_PAD_CTRL_SELECT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK))     btns |= CELL_PAD_CTRL_L3;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK))    btns |= CELL_PAD_CTRL_R3;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START))         btns |= CELL_PAD_CTRL_START;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP))       btns |= CELL_PAD_CTRL_UP;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))    btns |= CELL_PAD_CTRL_RIGHT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))     btns |= CELL_PAD_CTRL_DOWN;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))     btns |= CELL_PAD_CTRL_LEFT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  btns |= CELL_PAD_CTRL_L1;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) btns |= CELL_PAD_CTRL_R1;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y))             btns |= CELL_PAD_CTRL_TRIANGLE;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B))             btns |= CELL_PAD_CTRL_CIRCLE;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A))             btns |= CELL_PAD_CTRL_CROSS;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X))             btns |= CELL_PAD_CTRL_SQUARE;

        /* Triggers via axis */
        int lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        if (lt > 3000) btns |= CELL_PAD_CTRL_L2;
        if (rt > 3000) btns |= CELL_PAD_CTRL_R2;

        s_host_state[i].buttons = btns;

        /* Analog sticks */
        s_host_state[i].analog_lx = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX));
        s_host_state[i].analog_ly = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY));
        s_host_state[i].analog_rx = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX));
        s_host_state[i].analog_ry = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY));

        /* Triggers */
        s_host_state[i].trigger_l2 = pad_sdl_trigger_to_u8(lt);
        s_host_state[i].trigger_r2 = pad_sdl_trigger_to_u8(rt);

        /* Pressure: SDL has digital buttons, so 0 or 255 */
        s_host_state[i].press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
        s_host_state[i].press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
        s_host_state[i].press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
        s_host_state[i].press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
        s_host_state[i].press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
        s_host_state[i].press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
        s_host_state[i].press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
        s_host_state[i].press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
        s_host_state[i].press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
        s_host_state[i].press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
    }
}

static void pad_init_backend(void)
{
    if (!s_sdl_inited) {
        if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
            SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
        }
        s_sdl_inited = 1;
    }
    memset(s_sdl_controllers, 0, sizeof(s_sdl_controllers));

    /* Open any controllers already connected */
    int num = SDL_NumJoysticks();
    for (int i = 0; i < num && i < PAD_MAX_HOST_PORTS; i++) {
        if (SDL_IsGameController(i)) {
            s_sdl_controllers[i] = SDL_GameControllerOpen(i);
        }
    }
}

static void pad_shutdown_backend(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (s_sdl_controllers[i]) {
            SDL_GameControllerClose(s_sdl_controllers[i]);
            s_sdl_controllers[i] = NULL;
        }
    }
}

#endif /* PAD_BACKEND_SDL2 */

/* ---------------------------------------------------------------------------
 * Poll dispatcher
 * -----------------------------------------------------------------------*/

/* Re-poll the host at most every PAD_POLL_INTERVAL_MS; serve cached state in
 * between.
 *
 * XInputGetState is a driver call, and on a slot with nothing plugged in it is
 * an expensive one: measured at 97.6 us for a sweep of the 7 host ports on a
 * machine with no controller attached. cellPadGetData ran that sweep on EVERY
 * call, and a guest that polls the pad from a spin loop -- The Simpsons Arcade
 * Game does, in its input stage -- therefore spends its whole main thread
 * inside the input driver.
 *
 * 4 ms is 250 Hz, well above the 60 Hz a title can actually observe (and above
 * a real DualShock 3's own report rate), so no game can tell the difference,
 * including one sampling for button-press edges. */
#define PAD_POLL_INTERVAL_MS 4

/* #296: menu keys for the save list, which has no guest pad handle. Any port.
 * 1 up, 2 down, 4 confirm, 8 back. */
static void pad_poll_backend(void);
u32 cellPad_menu_buttons(void)
{
    pad_poll_backend();
    u32 out = 0;
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (!s_host_state[i].connected) continue;
        const u16 b = s_host_state[i].buttons;
        if ((b & CELL_PAD_CTRL_UP)   || s_host_state[i].analog_ly < 64)  out |= 1;
        if ((b & CELL_PAD_CTRL_DOWN) || s_host_state[i].analog_ly > 192) out |= 2;
        if (b & (CELL_PAD_CTRL_CROSS | CELL_PAD_CTRL_START)) out |= 4;
        if (b & CELL_PAD_CTRL_CIRCLE) out |= 8;
    }
    return out;
}

static unsigned long long pad_now_ms(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)ts.tv_nsec / 1000000ull;
#endif
}

static void pad_poll_backend(void)
{
    /* Benign race: two threads may poll in the same tick. That costs one extra
     * sweep, never a missed or stale-forever read. */
    static unsigned long long s_last_ms = 0;
    unsigned long long now = pad_now_ms();
    if (s_last_ms && now - s_last_ms < PAD_POLL_INTERVAL_MS) return;
    s_last_ms = now;
    /* A real pad streams reports at roughly this rate whether or not anything
     * changed, and libpad buffers them; a fresh sweep is a fresh report for
     * every port. cellPadGetData hands each one out exactly once. */
    for (int _i = 0; _i < PAD_MAX_HOST_PORTS; _i++) s_data_fresh[_i] = 1;

#if PAD_BACKEND_XINPUT
    pad_poll_xinput();
    pad_merge_sony();
#elif PAD_BACKEND_SDL2
    pad_poll_sdl2();
#endif
#ifdef _WIN32
    if (!s_host_state[0].connected) pad_poll_keyboard();
#endif
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellPadInit(u32 max_connect)
{
    printf("[cellPad] Init(max_connect=%u)\n", max_connect);

    if (s_pad_initialized)
        return CELL_PAD_ERROR_ALREADY_OPENED;

    /* The bound is the 127-slot pad table, not the 7 ports: larger requests
     * are accepted and clamped. Resistance: Fall of Man asks for 8; rejected,
     * the library stayed closed, every GetInfo failed and the title never
     * polled the pad -- stuck on "Press the START button". */
    if (max_connect == 0 || max_connect > 127)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_pad_initialized = 1;
    s_max_connect = max_connect > CELL_PAD_MAX_PORT_NUM ? CELL_PAD_MAX_PORT_NUM : max_connect;
    memset(s_port_setting, 0, sizeof(s_port_setting));
    memset(s_host_state, 0, sizeof(s_host_state));
    pad_init_backend();

    /* Do an initial poll to detect connected controllers */
    pad_poll_backend();

    return CELL_OK;
}

s32 cellPadEnd(void)
{
    printf("[cellPad] End()\n");

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    pad_shutdown_backend();

    s_pad_initialized = 0;
    s_max_connect = 0;
    return CELL_OK;
}

void cellPad_poll(void)
{
    if (s_pad_initialized) {
        pad_poll_backend();
    }
}

/* HLE args are GUEST effective addresses; guest structs are BIG-ENDIAN. The
 * vm_write helpers from ppu_memory.h translate + byte-swap. (cellPad was
 * written for host pointers and was never actually invoked until its NIDs
 * were fixed.) These used to be redeclared here with a 64-bit address
 * parameter, which conflicts with the header now that it is included. */

s32 cellPadGetData(u32 port_no, CellPadData* data_guest)
{
    { static int _once = 0;
      if (!_once++) printf("[cellPad] GetData polling begins (port %u)\n", port_no); }
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= s_max_connect || !data_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadData _d; CellPadData* data = &_d;
    memset(data, 0, sizeof(CellPadData));

    /* Poll fresh state */
    pad_poll_backend();

    /* YDKJ_INJECT_PAD: the game's wait loop polls the pad (cellPadSetActDirect /
     * GetData) -- if it's parked on a "press button" prompt after loading, no
     * host input means it waits forever. Inject a real button PULSE (CROSS+START+
     * CIRCLE) so it advances. Legit input simulation, not forged pixels. */
    /* NOTE: cache getenv() -- this runs on every poll from multiple threads, and the
     * uncached version crashed deterministically (varying fault address = race). Every
     * other probe in this tree uses the cached `static int x=-1` pattern; match it. */
    static int  s_inj   = -1;
    static u16  s_btn   = 0x0008u;   /* default START */
    static long s_delay = 3000;
    if (s_inj < 0) {
        s_inj = getenv("YDKJ_INJECT_PAD") ? 1 : 0;
        const char* be = getenv("YDKJ_PAD_BTN");   if (be) s_btn   = (u16)strtoul(be,0,0);
        const char* de = getenv("YDKJ_PAD_DELAY"); if (de) s_delay = strtol(de,0,0);
    }
    /* Gate on WALL-CLOCK, not poll count: the game polls the pad hundreds of thousands
     * of times inside an early wait loop, so any count-based delay fires during init
     * (crash: guest ctr=0x0005F8A0 r3=0 -- button handler on an unconstructed object).
     * YDKJ_PAD_DELAY is now SECONDS to wait before the first injected press. */
    if (s_inj && port_no < PAD_MAX_HOST_PORTS) {
        static long _pc = 0; _pc++;
        /* Cheap counter gate FIRST: clock() is a slow call and this poll loop is hammered
         * by the guest from multiple threads; sampling it every poll perturbs timing. Only
         * consult the clock once every 256 polls, and never after arming. */
        static int     s_armed = 0;
        static clock_t s_t0    = 0;
        if (!s_armed) {
            if ((_pc & 0xFF) == 0) {
                if (!s_t0) s_t0 = clock();
                else if ((double)(clock() - s_t0) / (double)CLOCKS_PER_SEC >= (double)s_delay) s_armed = 1;
            }
            goto skip_inject;
        }
        u16 btn = s_btn; long delay = 0;
        /* Delay past init, then a single clean pulse every ~180 calls (press 15, release 165). */
        if (_pc > delay) {
            long ph = (_pc - delay) % 180;
            if (ph < 15) s_host_state[port_no].buttons |= btn;
            else         s_host_state[port_no].buttons &= ~btn;
            s_host_state[port_no].connected = 1;
        }
    }
skip_inject: ;

    if (port_no >= PAD_MAX_HOST_PORTS || (!s_host_state[port_no].connected && port_no != 0)) {
        goto emit;   /* data stays zeroed -> len=0 */
    }
    /* port 0 is always a valid (virtual) pad; s_host_state[0] is neutral if no
     * physical device, which yields no-buttons-pressed data below. */

    {
    PadHostState* hs = &s_host_state[port_no];
    u32 setting = s_port_setting[port_no];

    /* Determine data length based on port settings */
    s32 len = CELL_PAD_LEN_CHANGE_DEFAULT;
    if (setting & CELL_PAD_SETTING_SENSOR_ON)
        len = CELL_PAD_LEN_CHANGE_SENSOR_ON;
    else if (setting & CELL_PAD_SETTING_PRESS_ON)
        len = CELL_PAD_LEN_CHANGE_PRESS_ON;

    data->len = len;
    /* button[0] is reserved and 0 on hardware; button[1] carries the pad mode
     * in its high nibble (0x7) and len/2 in its low one. GH3 drops any packet
     * whose button[0] is non-zero -- with len (8) there, every press was
     * received and thrown away, and "Press any button" never advanced. */
    data->button[0] = 0;
    data->button[1] = (u16)(0x70 | ((len / 2) & 0xF));

    /* Digital buttons. hs->buttons packs both halves into one 16-bit mask
     * (SELECT=bit0..LEFT=bit7 = DIGITAL1; L2=bit8..SQUARE=bit15 = DIGITAL2).
     * Split correctly — writing the whole value into DIGITAL1 with DIGITAL2=0
     * left every face button (cross/circle/triangle/square, L1/L2/R1/R2) dead.
     * (via sagemono, PR #42) */
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] = (u16)(hs->buttons & 0xFF);
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] = (u16)((hs->buttons >> 8) & 0xFF);

    /* PAD_AUTOPRESS: headless bring-up input -- pulse CROSS then START every few
     * seconds of polling so boot screens that wait for input advance without a
     * human at the pad. Env-gated test scaffolding, off by default. */
    { static int s_ap = -1; static unsigned s_apn = 0;
      if (s_ap < 0) s_ap = getenv("PAD_AUTOPRESS") ? 1 : 0;
      if (s_ap && port_no == 0) {
          unsigned ph = s_apn++ % 240;
          if (ph < 12)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= 0x40;   /* CROSS */
          else if (ph >= 120 && ph < 132)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= 0x08;   /* START */
      } }

    /* PAD_SCRIPT="<sec>:<mask>,<sec>:<mask>,..." -- press a NAMED button at a
     * given wall-clock second, each held ~250 ms. PAD_AUTOPRESS only pulses
     * CROSS and START, which is enough to clear a "press start" screen but not
     * to navigate a menu: You Don't Know Jack needs DOWN to move off its name
     * field before CROSS means anything. Masks are CELL_PAD_CTRL_* packed as
     * (DIGITAL2 << 8) | DIGITAL1: START 0x0008, UP 0x0010, DOWN 0x0040,
     * LEFT 0x0080, RIGHT 0x0020, CROSS 0x4000, CIRCLE 0x2000, TRIANGLE 0x1000.
     * Legit input simulation on the same footing as PAD_AUTOPRESS/PAD_STICK. */
    { static int s_sc = -1;
      static struct { double t; unsigned mask; } ev[32]; static int n_ev = 0;
      static unsigned long long t0 = 0;   /* not ULONGLONG: this builds on POSIX too */
      if (s_sc < 0) {
          s_sc = 0;
          const char* e = getenv("PAD_SCRIPT");
          if (e && *e) {
              s_sc = 1; t0 = GetTickCount64();
              const char* p = e;
              while (*p && n_ev < 32) {
                  double t = strtod(p, (char**)&p);
                  if (*p == ':') p++;
                  unsigned m = (unsigned)strtoul(p, (char**)&p, 0);
                  ev[n_ev].t = t; ev[n_ev].mask = m; n_ev++;
                  while (*p == ',' || *p == ' ') p++;
              }
              printf("[cellPad] PAD_SCRIPT: %d event(s)\n", n_ev);
          }
      }
      if (s_sc && port_no == 0) {
          double now = (double)(GetTickCount64() - t0) / 1000.0;
          /* Latch, do not window. A fixed time window is missed whenever the
           * title stops polling the pad for longer than the window (it does
           * exactly that on a loading screen), so the press silently never
           * happens. Fire on the first poll at or after the event time and
           * hold it for a fixed number of polls, which is what a real press
           * looks like regardless of poll rate. */
          static int fired[32], held[32];
          for (int i = 0; i < n_ev; i++) {
              if (!fired[i] && now >= ev[i].t) { fired[i] = 1; held[i] = 40; }
              if (held[i] > 0) {
                  held[i]--;
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(ev[i].mask & 0xFF);
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((ev[i].mask >> 8) & 0xFF);
                  static int said[32];
                  if (!said[i]) { said[i] = 1;
                      printf("[cellPad] PAD_SCRIPT t=%.1fs press 0x%04X\n", ev[i].t, ev[i].mask); }
              }
          }
      } }

    /* PAD_FILE=<path> -- closed-loop input. Each poll, if the file holds a
     * button mask (same encoding as PAD_SCRIPT), press it for a fixed number
     * of polls and empty the file. A driver outside the process can then look
     * at the log or a frame dump, decide which screen the title is on, and
     * write the next button -- which a fixed time schedule cannot do, because
     * a menu system with several screens wanders when driven blind.
     *
     *   echo 0x0008 > pad.txt    # START
     *   echo 0x4000 > pad.txt    # CROSS
     *   echo 0x0040 4 > pad.txt  # DOWN for 4 polls (menus auto-repeat a
     *                            # long hold; the default is 40)
     */
    { static int s_pf = -1; static char pf_path[512];
      static unsigned held_mask = 0; static int held_n = 0;
      if (s_pf < 0) { const char* e = getenv("PAD_FILE");
                      s_pf = (e && *e) ? 1 : 0;
                      if (s_pf) { strncpy(pf_path, e, sizeof pf_path - 1); pf_path[sizeof pf_path - 1] = 0; } }
      if (s_pf && port_no == 0) {
          if (held_n <= 0) {
              FILE* f = fopen(pf_path, "rb");
              if (f) {
                  char buf[64]; buf[0] = 0;
                  if (fgets(buf, sizeof buf, f)) {
                      char* rest;
                      unsigned m = (unsigned)strtoul(buf, &rest, 0);
                      int n = (int)strtol(rest, 0, 0);
                      if (m) { held_mask = m; held_n = n > 0 ? n : 40;
                               printf("[cellPad] PAD_FILE press 0x%04X\n", m); fflush(stdout); }
                  }
                  fclose(f);
                  if (held_n > 0) { FILE* w = fopen(pf_path, "wb"); if (w) fclose(w); }
              }
          }
          if (held_n > 0) { held_n--;
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(held_mask & 0xFF);
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((held_mask >> 8) & 0xFF); }
      } }

    /* Analog sticks */
    /* PAD_STICK="lx,ly,rx,ry" (0-255, 128 = centred): hold the analog sticks at
     * fixed positions. Rubber Ducky drives its camera from the sticks and never
     * moves on its own, so with no host pad the view is frozen wherever it
     * started -- which can leave the subject of the demo off-screen or too far
     * away to make out. Legit input simulation, same footing as YDKJ_INJECT_PAD. */
    { static int s_st = -1;
      static unsigned char s_v[4] = {128,128,128,128};
      /* Optional 5th/6th fields: the seconds the hold starts and ends, so the menus
       * before gameplay are not driven by it (PAD_STICK="128,0,128,128,300,320"). */
      static double s_from = 0, s_until = 1e30; static unsigned long long s_t0 = 0;
      if (!s_t0) s_t0 = GetTickCount64();
      if (s_st < 0) { const char* e = getenv("PAD_STICK");
        s_st = e ? 1 : 0;
        if (e) { int a=128,b=128,c=128,d=128;
                 sscanf(e, "%d,%d,%d,%d,%lf,%lf", &a,&b,&c,&d,&s_from,&s_until);
                 s_v[0]=(unsigned char)a; s_v[1]=(unsigned char)b;
                 s_v[2]=(unsigned char)c; s_v[3]=(unsigned char)d; } }
      if (s_st && (double)(GetTickCount64() - s_t0) / 1000.0 >= s_from && (double)(GetTickCount64() - s_t0) / 1000.0 < s_until) { hs->analog_lx = s_v[0]; hs->analog_ly = s_v[1];
                  hs->analog_rx = s_v[2]; hs->analog_ry = s_v[3];
                  hs->connected = 1; }
      /* PAD_SWEEP=<seconds per step>: walk the sticks through a fixed set of
       * deflections instead of holding one. Guessing a single stick position and
       * re-running costs ~5 minutes a try; a sweep answers "can input move the
       * camera anywhere useful" in one run, and the frame dumps say which step
       * paid off. Steps: centre, each left-stick direction, then each right. */
      { static int sw = -1; static clock_t t0 = 0;
        if (sw < 0) { const char* e = getenv("PAD_SWEEP");
                      sw = e ? atoi(e) : 0; if (sw < 0) sw = 0; }
        if (sw) {
            static const unsigned char steps[9][4] = {
                {128,128,128,128},
                {128,  0,128,128}, {128,255,128,128},
                {  0,128,128,128}, {255,128,128,128},
                {128,128,128,  0}, {128,128,128,255},
                {128,128,  0,128}, {128,128,255,128},
            };
            if (!t0) t0 = clock();
            long el = (long)((double)(clock() - t0) / (double)CLOCKS_PER_SEC);
            int k = (int)((el / (sw > 0 ? sw : 1)) % 9);
            hs->analog_lx = steps[k][0]; hs->analog_ly = steps[k][1];
            hs->analog_rx = steps[k][2]; hs->analog_ry = steps[k][3];
            hs->connected = 1;
            { static int lastk = -1;
              if (k != lastk) { lastk = k;
                fprintf(stderr, "[PADSWEEP] step %d: l=(%u,%u) r=(%u,%u)%c", k,
                        steps[k][0], steps[k][1], steps[k][2], steps[k][3], 10); } }
        } } }
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = hs->analog_rx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = hs->analog_ry;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X]  = hs->analog_lx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y]  = hs->analog_ly;

    /* Pressure-sensitive buttons (only meaningful if PRESS_ON) */
    if (setting & CELL_PAD_SETTING_PRESS_ON) {
        data->button[CELL_PAD_BTN_OFFSET_PRESS_RIGHT]    = hs->press_right;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_LEFT]     = hs->press_left;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_UP]       = hs->press_up;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_DOWN]     = hs->press_down;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE] = hs->press_triangle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CIRCLE]   = hs->press_circle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CROSS]    = hs->press_cross;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_SQUARE]   = hs->press_square;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L1]       = hs->press_l1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R1]       = hs->press_r1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L2]       = hs->trigger_l2;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R2]       = hs->trigger_r2;
        /* A button whose digital bit is set reads full pressure. Injected
         * input (PAD_FILE/PAD_SCRIPT/PAD_AUTOPRESS, keyboard) only sets the
         * digital bits, and the host values above zeroed any pressure it had
         * written -- so a title keying its menus off the pressure words (GH3,
         * Tornado Outbreak's title screen) never saw the press. */
        static const struct { u8 word; u8 bit; u8 off; } pm[] = {
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x20, CELL_PAD_BTN_OFFSET_PRESS_RIGHT },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x80, CELL_PAD_BTN_OFFSET_PRESS_LEFT },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x10, CELL_PAD_BTN_OFFSET_PRESS_UP },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x40, CELL_PAD_BTN_OFFSET_PRESS_DOWN },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x10, CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x20, CELL_PAD_BTN_OFFSET_PRESS_CIRCLE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x40, CELL_PAD_BTN_OFFSET_PRESS_CROSS },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x80, CELL_PAD_BTN_OFFSET_PRESS_SQUARE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x04, CELL_PAD_BTN_OFFSET_PRESS_L1 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x08, CELL_PAD_BTN_OFFSET_PRESS_R1 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x01, CELL_PAD_BTN_OFFSET_PRESS_L2 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x02, CELL_PAD_BTN_OFFSET_PRESS_R2 } };
        for (unsigned k = 0; k < sizeof pm / sizeof pm[0]; k++)
            if ((data->button[pm[k].word] & pm[k].bit) && data->button[pm[k].off] < 255)
                data->button[pm[k].off] = 255;
    }

    /* Sensor data (only meaningful if SENSOR_ON) */
    if (setting & CELL_PAD_SETTING_SENSOR_ON) {
        /* Default sensor values: accelerometer at rest (512 = 1g center) */
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_X] = 512;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Y] = 399; /* gravity */
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Z] = 512;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_G] = 512;
    }
    }  /* end connected block */

emit:
    /* A non-zero len means "here is a CHANGE packet" -- the constant is literally
     * named CELL_PAD_LEN_CHANGE_DEFAULT. Hardware reports len = 0 once the port
     * has nothing new, and titles DRAIN on that: The Simpsons Arcade Game's
     * input stage is
     *
     *     while (cellPadGetData(port, &d) == CELL_OK && d.len > 0) { ... }
     *
     * (func_0013CFA8, loop 0x13D648 -> 0x13D040). Reporting a change on every
     * call made that loop infinite: the main thread pegged one core inside
     * libpad and the title never got past its first rendered screen.
     *
     * So hand out one packet per host report and report len = 0 for any read
     * after that until the next one arrives (pad_poll_backend sets the flag when
     * it actually re-polls, every PAD_POLL_INTERVAL_MS). That is what the
     * hardware does: a pad streams reports at a fixed rate whether or not
     * anything changed, and libpad buffers them.
     *
     * Reporting only on CHANGE instead is wrong and was tried: a HELD button
     * produces exactly one packet, so a title that reads button[] only when
     * len > 0 sees the press for a single frame and then sees nothing. In this
     * game that came out as jump being the only control that seemed to work and
     * attack firing once.
     *
     * PAD_ALWAYS_CHANGE=1 restores the old always-a-packet behaviour. */
    {
        static int always = -1;
        if (always < 0) always = getenv("PAD_ALWAYS_CHANGE") ? 1 : 0;
        if (!always && port_no < PAD_MAX_HOST_PORTS && data->len) {
            if (!s_data_fresh[port_no]) data->len = 0;   /* nothing new: end the drain */
            else                        s_data_fresh[port_no] = 0;
        }
    }
    {
        unsigned int ea = (unsigned int)(uintptr_t)data_guest;
        vm_write32((unsigned long long)ea + 0, (unsigned int)data->len);   /* len (s32) */
        for (int _i = 0; _i < CELL_PAD_MAX_CODES; _i++)                    /* button[] u16 */
            vm_write16((unsigned long long)ea + 4 + (unsigned int)_i * 2, data->button[_i]);
    }
    return CELL_OK;
}

/* cellPadGetInfo (v1 API, NID 0x3AAAD464) — used by PSL1GHT's ioPadGetInfo.
 * Layout (RPCS3 cellPad.h CellPadInfo, all big-endian):
 *   +0x00 u32 max_connect        +0x04 u32 now_connect   +0x08 u32 system_info
 *   +0x0C u16 vendor_id[127]     +0x10A u16 product_id[127] +0x208 u8 status[127]
 *   (CELL_MAX_PADS / PSL1GHT MAX_PADS = 127 -- NOT 7; see below)
 * Leaving this unimplemented let the guest read stack garbage as pad state and
 * run off into the weeds before ever reaching gcm init. */
s32 cellPadGetInfo(CellPadInfo2* info)
{
    uint32_t gaddr = (uint32_t)(uintptr_t)info;
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!gaddr)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_backend();

    /* Present a virtual pad on port 0 when nothing physical is attached, the
     * same as cellPadGetInfo2 does.
     *
     * The two entry points disagreed: GetInfo2 carried the fallback and GetInfo
     * did not, so which one a title imports decided whether it believed a
     * controller existed. The Orange Box's chooser imports GetInfo (v1) and
     * never GetInfo2, so it saw zero pads, opened "SIXAXIS wireless controller
     * not detected", took the auto-OK, and opened it again -- forever, before
     * it ever finished booting. Both entry points describe the same hardware
     * and have to answer the same way. */
    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++)
        if (s_host_state[i].connected || i == 0) connected++;

    vm_write32(gaddr + 0x00, s_max_connect);
    vm_write32(gaddr + 0x04, connected);
    vm_write32(gaddr + 0x08, 0);                       /* system_info */
    /* CellPadInfo (v1) sizes its arrays by CELL_MAX_PADS = 127, not by the 7
     * ports: vendor_id[127] @0x0C, product_id[127] @0x10A, status[127] @0x208.
     * Packed for 7 ports, status landed at 0x28 and GH3 -- which reads
     * info+0x208+port -- saw port 0 disconnected and never polled the pad. */
    for (u32 i = 0; i < CELL_PAD_MAX_PORT_NUM; i++) {
        int on = (i < s_max_connect && i < PAD_MAX_HOST_PORTS &&
                  (s_host_state[i].connected || i == 0));
        vm_write16(gaddr + 0x00C + i * 2, on ? 0x054C : 0);   /* vendor: Sony  */
        vm_write16(gaddr + 0x10A + i * 2, on ? 0x0268 : 0);   /* product: DS3  */
        vm_write8(gaddr + 0x208 + i, on ? CELL_PAD_STATUS_CONNECTED : 0);
    }
    return CELL_OK;
}

s32 cellPadGetInfo2(CellPadInfo2* info_guest)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Poll to get latest connection state */
    pad_poll_backend();

    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadInfo2 _in; CellPadInfo2* info = &_in;
    memset(info, 0, sizeof(CellPadInfo2));
    info->max_connect = s_max_connect;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++) {
        /* Always present a virtual pad on port 0 (standard emulator behavior) so
         * boot flows that block until a controller is connected can proceed even
         * with no physical device attached. */
        if (s_host_state[i].connected || i == 0) {
            info->port_status[i]       = CELL_PAD_STATUS_CONNECTED;
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                                       | CELL_PAD_CAPABILITY_PRESS_MODE
                                       | CELL_PAD_CAPABILITY_SENSOR_MODE
                                       | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                                       | CELL_PAD_CAPABILITY_ACTUATOR;
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED;
        }
    }
    info->now_connect = connected;

    /* copy to guest big-endian (CellPadInfo2 is all u32 fields) */
    {
        unsigned int ea = (unsigned int)(uintptr_t)info_guest;
        for (unsigned int _o = 0; _o < sizeof(CellPadInfo2); _o += 4)
            vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o));
    }
    return CELL_OK;
}

/* cellPadPeriphGetInfo -- the peripheral-class view of the pad ports (NID
 * 0x4CC9B68D). LBP polls this during boot; leaving it unresolved returned
 * CELL_OK with the caller's CellPadPeriphInfo left as uninitialised stack
 * garbage, so the game read junk port_status/pclass_type for all 7 ports.
 *
 * Layout (SDK cell/pad/pad_codes.h) -- all u32, so the whole struct marshals
 * big-endian in one sweep like cellPadGetInfo2:
 *   max_connect, now_connect, system_info,
 *   port_status[7], port_setting[7], device_capability[7],
 *   device_type[7], pclass_type[7], pclass_profile[7]
 * We report a standard DUALSHOCK-class pad (pclass_type STANDARD = 0, no
 * profile bits) mirroring the ports cellPadGetInfo2 already reports. */
s32 cellPadPeriphGetInfo(CellPadPeriphInfo* info_guest)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_backend();

    CellPadPeriphInfo _in; CellPadPeriphInfo* info = &_in;
    memset(info, 0, sizeof(CellPadPeriphInfo));
    info->max_connect = s_max_connect;
    info->system_info = 0;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < CELL_PAD_MAX_PORT_NUM; i++) {
        /* Same virtual-pad-on-port-0 rule as cellPadGetInfo2, so both views of
         * the ports agree (a game that cross-checks them must see one truth). */
        if ((i < PAD_MAX_HOST_PORTS && s_host_state[i].connected) || i == 0) {
            info->port_status[i]       = CELL_PAD_STATUS_CONNECTED;
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                                       | CELL_PAD_CAPABILITY_PRESS_MODE
                                       | CELL_PAD_CAPABILITY_SENSOR_MODE
                                       | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                                       | CELL_PAD_CAPABILITY_ACTUATOR;
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            info->pclass_type[i]       = CELL_PAD_PCLASS_TYPE_STANDARD;
            info->pclass_profile[i]    = 0;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED;
        }
    }
    info->now_connect = connected;

    { unsigned int ea = (unsigned int)(uintptr_t)info_guest;
      for (unsigned int _o = 0; _o < sizeof(CellPadPeriphInfo); _o += 4)
          vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o)); }

    { static int _n = 0;
      if (_n++ < 2)
          printf("[cellPad] PeriphGetInfo(max=%u now=%u) -> STANDARD class\n",
                 info->max_connect, connected); }
    return CELL_OK;
}

s32 cellPadSetPortSetting(u32 port_no, u32 port_setting)
{
    printf("[cellPad] SetPortSetting(port=%u, setting=0x%X)\n",
           port_no, port_setting);

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_port_setting[port_no] = port_setting;
    return CELL_OK;
}

/* cellPadInfoPressMode (NID 0x0E2DFAAD) / cellPadSetPressMode (NID 0xF83F8182)
 *
 * Press mode makes the digital buttons report an analog 0..255 pressure in the
 * extended part of CellPadData. A DS3 supports it, and the virtual pad reported
 * by cellPadGetInfo/GetInfo2 already advertises CELL_PAD_CAPABILITY_PRESS_MODE,
 * so these have to agree with that claim rather than fall through to the
 * unresolved-NID path.
 *
 * Info returns 1 when the port supports press mode; Set toggles it, recorded in
 * the same s_port_setting word cellPadSetPortSetting uses so a title that reads
 * the setting back sees what it asked for. */
s32 cellPadInfoPressMode(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    return 1;                                  /* supported */
}

s32 cellPadSetPressMode(u32 port_no, u32 mode)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    if (mode)
        s_port_setting[port_no] |= CELL_PAD_SETTING_PRESS_ON;
    else
        s_port_setting[port_no] &= ~(u32)CELL_PAD_SETTING_PRESS_ON;
    return CELL_OK;
}

s32 cellPadGetCapabilityInfo(u32 port_no, CellPadCapabilityInfo* info)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !info)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    u32 info_ea = (u32)(uintptr_t)info;
    for (u32 i = 0; i < CELL_PAD_MAX_CODES; i++)
        vm_write32(info_ea + i * 4, 0);

    /* Report standard DualShock 3 capabilities */
    vm_write32(info_ea, CELL_PAD_CAPABILITY_PS3_CONFORMITY
                        | CELL_PAD_CAPABILITY_PRESS_MODE
                        | CELL_PAD_CAPABILITY_SENSOR_MODE
                        | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                        | CELL_PAD_CAPABILITY_ACTUATOR);

    return CELL_OK;
}

s32 cellPadSetActDirect(u32 port_no, CellPadActParam* param)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !param)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    param = GUEST_PTR(param, CellPadActParam*);

#if PAD_BACKEND_XINPUT
    /* Map to XInput vibration */
    if (port_no < PAD_MAX_HOST_PORTS && s_host_state[port_no].connected &&
        !sony_rumble(port_no, param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE],
                     param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] > 1
                         ? param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL]
                         : (u8)(param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] ? 255 : 0))) {
        XINPUT_VIBRATION vib;
        vib.wLeftMotorSpeed  = (WORD)(param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] * 257);
        vib.wRightMotorSpeed = (WORD)(param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] * 257);
        XInputSetState((DWORD)port_no, &vib);
    }
#endif

#if PAD_BACKEND_SDL2
    if (port_no < PAD_MAX_HOST_PORTS && s_sdl_controllers[port_no]) {
        SDL_GameControllerRumble(
            s_sdl_controllers[port_no],
            (Uint16)(param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] * 257),
            (Uint16)(param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] * 257),
            100 /* duration ms */
        );
    }
#endif

    return CELL_OK;
}

s32 cellPadClearBuf(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Nothing to clear in our implementation -- state is polled fresh */
    return CELL_OK;
}
