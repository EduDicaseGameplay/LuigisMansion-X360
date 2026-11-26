// HLE of the controller (PADRead from an Xbox 360 pad through XInput). The memory card is hle_card.cpp.
// PADStatus (pad.h, 12 bytes):
//
// button u16 @0, stickX s8 @2, stickY @3, substickX @4, substickY @5, triggerL u8 @6, triggerR @7,
// analogA @8, analogB @9, err s8 @10 (0 ok, -1 no controller).
#include "../runtime/runtime.h"
#include "../runtime/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>


// --- controller -------------------------------------------------------------------
enum {
    PAD_LEFT = 0x0001, PAD_RIGHT = 0x0002, PAD_DOWN = 0x0004, PAD_UP = 0x0008, PAD_Z = 0x0010, PAD_R = 0x0020,
    PAD_L = 0x0040, PAD_A = 0x0100, PAD_B = 0x0200, PAD_X = 0x0400, PAD_Y = 0x0800, PAD_START = 0x1000
};

// Radial dead zone with the XInput recommended sizes (XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE 7849, RIGHT 8689): inside it
// the stick reads centred (a worn Xbox 360 stick rests well past the old 1800, so Luigi walked or turned by
// himself); outside, the rest of the travel is rescaled so full tilt still reaches the GameCube's ~100. The game's
// own PADClamp then applies the GameCube dead zone (15/127), as on the console. Same scheme as the LM PC port
// (controls.cpp: radial dead zone, rescaled outside it).
static void stick2(s16 vx, s16 vy, int dead, s8* ox, s8* oy) {
    const float x = vx, y = vy;
    const float mag = sqrtf(x * x + y * y);
    if (mag <= (float)dead) { *ox = 0; *oy = 0; return; }
    const float m = mag > 32767.0f ? 32767.0f : mag;
    const float scale = (m - dead) / (32767.0f - dead) * 104.0f / mag;  // 104: a GameCube stick's full tilt
    int ix = (int)(x * scale), iy = (int)(y * scale);
    if (ix > 127) ix = 127; if (ix < -127) ix = -127;
    if (iy > 127) iy = 127; if (iy < -127) iy = -127;
    *ox = (s8)ix; *oy = (s8)iy;
}

// Scripted buttons for tests (JSRF technique): game:\pad_script.txt, e.g. "20 START;25 A;30 A@2;40 LY+" =
// seconds since boot, button (A B X Y Z L R START UP DOWN LEFT RIGHT, LX+/LX-/LY+/LY- for the stick), optional
// @duration in seconds (default 0.2). Never leave the file next to the game for normal play.
struct PadEvent { float t, dur; u32 buttons; int lx, ly; };
static PadEvent s_script[64];
static int s_scriptCount = -1;
static DWORD s_bootTick = 0;

static void load_script() {
    s_scriptCount = 0;
    s_bootTick = GetTickCount();
    FILE* f = fopen("game:\\pad_script.txt", "rb");
    if (!f) return;
    char buf[2048];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    for (char* tok = strtok(buf, ";\r\n"); tok && s_scriptCount < 64; tok = strtok(0, ";\r\n")) {
        PadEvent e = {0, 0.2f, 0, 0, 0};
        char name[16] = {0};
        if (sscanf(tok, "%f %15s", &e.t, name) < 2) continue;
        char* at = strchr(name, '@');
        if (at) { e.dur = (float)atof(at + 1); *at = 0; }
        static const struct { const char* n; u32 b; } map[] = {
            {"A", 0x0100}, {"B", 0x0200}, {"X", 0x0400}, {"Y", 0x0800}, {"Z", 0x0010}, {"L", 0x0040}, {"R", 0x0020},
            {"START", 0x1000}, {"UP", 0x0008}, {"DOWN", 0x0004}, {"LEFT", 0x0001}, {"RIGHT", 0x0002}};
        for (int i = 0; i < 12; i++) if (!strcmp(name, map[i].n)) e.buttons = map[i].b;
        if (!strcmp(name, "LX+")) e.lx = 100;
        if (!strcmp(name, "LX-")) e.lx = -100;
        if (!strcmp(name, "LY+")) e.ly = 100;
        if (!strcmp(name, "LY-")) e.ly = -100;
        s_script[s_scriptCount++] = e;
    }
    lm_log("[pad] script: %d events", s_scriptCount);
}

static u32 s_lastPoll[4] = {0, 0, 0, 0};
static bool s_connected[4] = {true, false, false, false};

HLE_FUNC(PADRead) {
    const u32 out = c.r[3];
    static bool s_fastTested = false;  // Source/hle/hle_fasttest.cpp (game:\lm360_fasttest.txt only)
    if (!s_fastTested) { s_fastTested = true; void lm_fasttest(Ctx&); lm_fasttest(c); }
    if (s_scriptCount < 0) load_script();
    u32 sb = 0; int slx = 0, sly = 0;
    const float now = (GetTickCount() - s_bootTick) / 1000.0f;
    for (int i = 0; i < s_scriptCount; i++)
        if (now >= s_script[i].t && now < s_script[i].t + s_script[i].dur) { sb |= s_script[i].buttons; slx += s_script[i].lx; sly += s_script[i].ly; }
    for (int port = 0; port < 4; port++) {
        const u32 p = out + port * 12;
        memset(hostp(p), 0, 12);
        // Empty ports are polled every 2 s only (KI lesson: XInputGetState on an empty port is slow).
        const DWORD tick = GetTickCount();
        if (port == 0 && s_scriptCount > 0) {
            wr16(p + 0, sb | 0x0080); wr8(p + 2, (u8)(s8)slx); wr8(p + 3, (u8)(s8)sly);
            wr8(p + 8, (sb & 0x0100) ? 0xFF : 0); wr8(p + 9, (sb & 0x0200) ? 0xFF : 0); wr8(p + 10, 0);
            continue;
        }
        if (!s_connected[port] && tick - s_lastPoll[port] < 2000) { wr8(p + 10, 0xFF); continue; }
        s_lastPoll[port] = tick;
        XINPUT_STATE st;
        const DWORD err = XInputGetState(port, &st);
        if (err != ERROR_SUCCESS) {
            lm_log_once(0x60000000u + port * 0x100 + (err & 0xFF), "[pad] port %d: XInputGetState error %u", port, err);
            s_connected[port] = false; wr8(p + 10, 0xFF); continue;
        }
        if (!s_connected[port] || port == 0) lm_log_once(0x61000000u + port, "[pad] port %d connected", port);
        s_connected[port] = true;
        const XINPUT_GAMEPAD& g = st.Gamepad;
        if (g.wButtons) lm_log_once(0x62000000u + g.wButtons, "[pad] port %d buttons %04X", port, g.wButtons);
        // Back + Start held for 2 s: back to the dashboard / launcher (KI and BRE convention).
        static DWORD s_exitSince = 0;
        if ((g.wButtons & XINPUT_GAMEPAD_BACK) && (g.wButtons & XINPUT_GAMEPAD_START)) {
            if (!s_exitSince) s_exitSince = tick;
            else if (tick - s_exitSince > 2000) { lm_log("[pad] Back+Start: exit"); lm_log_flush(); XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0); }
        } else s_exitSince = 0;
        u32 b = 0;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) b |= PAD_LEFT;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) b |= PAD_RIGHT;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) b |= PAD_DOWN;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_UP) b |= PAD_UP;
        if (g.wButtons & (XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_LEFT_SHOULDER)) b |= PAD_Z;  // LB too (LM PC port)
        if (g.wButtons & XINPUT_GAMEPAD_A) b |= PAD_A;
        if (g.wButtons & XINPUT_GAMEPAD_B) b |= PAD_B;
        if (g.wButtons & XINPUT_GAMEPAD_X) b |= PAD_X;
        if (g.wButtons & XINPUT_GAMEPAD_Y) b |= PAD_Y;
        if (g.wButtons & XINPUT_GAMEPAD_START) b |= PAD_START;
        if ((g.wButtons & XINPUT_GAMEPAD_BACK) && !(g.wButtons & XINPUT_GAMEPAD_START)) b |= PAD_START;  // Back too (LM PC port)
        if (g.bLeftTrigger > 200) b |= PAD_L;
        if (g.bRightTrigger > 200) b |= PAD_R;
        wr16(p + 0, b | 0x0080);  // 0x0080: origin valid
        s8 lx, ly, rx, ry;
        stick2(g.sThumbLX, g.sThumbLY, 7849, &lx, &ly);
        stick2(g.sThumbRX, g.sThumbRY, 8689, &rx, &ry);
        wr8(p + 2, (u8)lx);
        wr8(p + 3, (u8)ly);
        wr8(p + 4, (u8)rx);
        wr8(p + 5, (u8)ry);
        // Diagnostics for console videos: what reaches the game, at most every 2 s while anything is held.
        static DWORD s_padLog = 0;
        static u32 s_padLogs = 0;
        if ((b || lx || ly || rx || ry || g.bLeftTrigger > 20 || g.bRightTrigger > 20) && tick - s_padLog >= 2000 && s_padLogs < 300) {
            s_padLog = tick;
            s_padLogs++;
            lm_log("[pad] raw %04X L(%d,%d) R(%d,%d) LT %u RT %u -> GC %04X stick(%d,%d) C(%d,%d)", g.wButtons, g.sThumbLX,
                   g.sThumbLY, g.sThumbRX, g.sThumbRY, g.bLeftTrigger, g.bRightTrigger, b, lx, ly, rx, ry);
        }
        wr8(p + 6, g.bLeftTrigger);
        wr8(p + 7, g.bRightTrigger);
        wr8(p + 8, (b & PAD_A) ? 0xFF : 0);
        wr8(p + 9, (b & PAD_B) ? 0xFF : 0);
        wr8(p + 10, 0);
    }
    c.r[3] = 0;
}
HLE_FUNC(PADInit) { c.r[3] = 1; }
HLE_FUNC(PADReset) { c.r[3] = 1; }
HLE_FUNC(PADRecalibrate) { c.r[3] = 1; }
HLE_FUNC(PADSetSpec) { (void)c; }
HLE_FUNC(PADSetAnalogMode) { (void)c; }
HLE_FUNC(PADOriginUpdateCallback) { (void)c; }
HLE_FUNC(PADReceiveCheckCallback) { (void)c; }
HLE_FUNC(PADSetSamplingCallback) { c.r[3] = 0; }
HLE_FUNC(PADControlMotor) {
    const int port = (int)c.r[3];
    if (port < 0 || port > 3 || !s_connected[port]) return;
    XINPUT_VIBRATION v;
    v.wLeftMotorSpeed = v.wRightMotorSpeed = c.r[4] == 1 ? 40000 : 0;  // PAD_MOTOR_RUMBLE = 1
    XInputSetState(port, &v);
}
HLE_FUNC(SIInit) { (void)c; }
HLE_FUNC(SIBusy) { c.r[3] = 0; }
HLE_FUNC(SIDisablePolling) { c.r[3] = 0; }
HLE_FUNC(SIEnablePolling) { c.r[3] = 0; }
HLE_FUNC(SISetXY) { c.r[3] = 0; }
