// Xbox 360 entry point: loads the user's game files from the XEX folder, boots the recompiled game.
// Folder layout on the console (Documentation/BUILD_GUIDE.md):
//   default.xex, sys\main.dol, sys\fst.bin, files\... (the European disc's files)
#include "../runtime/runtime.h"
#include "../runtime/log.h"
#include "../hle/hle_thread.h"
#include <stdio.h>

#ifndef LM_VERSION
#define LM_VERSION "dev"
#endif


u64 time_base() {
    static LARGE_INTEGER freq = {0};
    static LARGE_INTEGER t0;
    LARGE_INTEGER now;
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0); }
    QueryPerformanceCounter(&now);
    const u64 d = (u64)(now.QuadPart - t0.QuadPart);
    // ticks * 40.5 MHz / freq without overflow for days of uptime
    return (d / (u64)freq.QuadPart) * kTimerClock + ((d % (u64)freq.QuadPart) * kTimerClock) / (u64)freq.QuadPart;
}

extern "C" void GXPC_SetWidescreen(float widthOver43);  // Source/thirdparty/sms_gx (xe_gx_render.cpp)

static Ctx s_mainCtx;
u32 g_fstAddr = 0;  // the FST in guest memory (hle_dvd.cpp)

static bool load_fst(u32* fstAddr, u32* fstSize) {
    FILE* f = fopen("game:\\sys\\fst.bin", "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    *fstSize = (u32)n;
    *fstAddr = (0x81800000 - (u32)n) & ~31u;
    const bool ok = fread(hostp(*fstAddr), 1, n, f) == (size_t)n;
    fclose(f);
    return ok;
}

void __cdecl main() {
    lm_log_open("game:\\lm360.log", LM_VERSION " (" __DATE__ " " __TIME__ ")");
    if (!mem_init()) lm_fatal("cannot allocate %u MiB of guest memory", kHostMemSize >> 20);
    lm_log("[rt] guest memory at %p", g_mem);
    // Widescreen (16:9, Hor+): follows the console's display setting, as the Sunshine 360 port does. The GX core widens
    // the EFB to 854 game pixels; full-screen 3D gets a wider horizontal field of view (not stretched), 2D artwork and
    // the HUD stay 4:3 in the centre, fades and screen passes cover the whole width (xe_gx_render.cpp, "Widescreen").
    // game:\lm360_43.txt next to default.xex forces the original 4:3 picture.
    {
        XVIDEO_MODE vm;
        memset(&vm, 0, sizeof(vm));
        XGetVideoMode(&vm);
        FILE* force43 = fopen("game:\\lm360_43.txt", "rb");
        if (force43) fclose(force43);
        const bool wide = vm.fIsWideScreen && !force43;
        if (wide) GXPC_SetWidescreen((16.0f / 9.0f) / (4.0f / 3.0f));
        lm_log("[gx] display %ux%u%s%s: %s", vm.dwDisplayWidth, vm.dwDisplayHeight, vm.fIsWideScreen ? " widescreen" : "",
               vm.fIsHiDef ? " HD" : "", wide ? "16:9 (Hor+)" : force43 ? "4:3 (lm360_43.txt)" : "4:3");
    }
    extern bool lm_gx_init(void* mem, u32 memSize);
    if (!lm_gx_init(g_mem, kMem1Size)) lm_log("[gx] renderer init failed: continuing without picture");
    else lm_log("[gx] renderer ready (sms_gx core + Xenos back end)");
    extern void prof_start();
    prof_start();
    dispatch_init();
    hooks_init();
    u32 entry = 0;
    if (!boot_load_dol("game:\\sys\\main.dol", &entry))
        lm_fatal("game:\\sys\\main.dol is missing: copy the European disc's sys and files folders next to default.xex");
    u32 fstAddr = 0, fstSize = 0;
    if (!load_fst(&fstAddr, &fstSize)) lm_fatal("game:\\sys\\fst.bin is missing");
    boot_setup_lowmem("GLMP01", fstAddr, fstSize);
    g_fstAddr = fstAddr;
    lm_log("[boot] FST %08X size %X; entering %08X (%s)", fstAddr, fstSize, entry, sym_name(entry));
    lm_log_flush();
    memset(&s_mainCtx, 0, sizeof(s_mainCtx));
    s_mainCtx.msr = 0x8000;
    s_mainCtx.r[1] = 0x81800000 - 0x100;  // __init_registers sets the real stack
    sched_init(s_mainCtx);
    extern bool g_watchArmed;
    g_watchArmed = true;
    guest_call(s_mainCtx, entry);
    lm_log("[boot] the game's entry returned");
    lm_log_flush();
    for (;;) Sleep(1000);
}
