// HLE of the Dolphin OS: boot, arena, time, interrupts mask, caches, reports. (Threads, messages, alarms
// and mutexes: hle_thread.cpp, still to be written; see Documentation/PORTING_PROGRESS.md.)
// Own implementation. Semantics from the Dolphin SDK headers in the lm-decomp tree (CC0) and the
// decompiled SDK sources there; nothing copied from the PC port.
#include "../runtime/runtime.h"
#include "../runtime/log.h"
#include <stdio.h>

void lm_gx_invalidate(void* p, u32 n);  // Source/gx/lm_gx_glue.cpp
void lm_gx_written(void* p, u32 n);

u32 sym_addr(const char* name);  // hle_util.cpp
void guest_call_args(Ctx& c, u32 fn, u32 a0, u32 a1);
u32 hle_reserve_top(u32 arenaHi);  // hle_thread.cpp

static u32 s_arenaLo = 0, s_arenaHi = 0;

// --- boot -------------------------------------------------------------------
HLE_FUNC(__init_hardware) { (void)c; }  // HID2 paired singles, FPRs and caches: nothing to set up
HLE_FUNC(__flush_cache) { (void)c; }
HLE_FUNC(__TRK_reset) { (void)c; }

HLE_FUNC(OSInit) {
    static bool done = false;  // the game calls OSInit again from main: keep the first arena
    if (done) { lm_log("[os] OSInit again (lr %08X), ignored", c.lr); return; }
    done = true;
    // Arena: from the end of the game's static data (the linker's __ArenaLo, which OSInit reads from the
    // boot block when it is non-zero) to the top of MEM1 below the FST.
    // __ArenaLo is the linker's stack top (_stack_addr), which __init_registers loads into r1 with
    // lis r1,hi / ori r1,r1,lo: read it from that code.
    const u32 bootLo = rd32(0x80000030);
    u32 lo = bootLo;
    const u32 ir = sym_addr("__init_registers");
    for (u32 a = ir; ir && !lo && a < ir + 0x40; a += 4) {
        const u32 w0 = rd32(a), w1 = rd32(a + 4);
        if ((w0 & 0xFFFF0000u) == 0x3C200000u && (w1 & 0xFFFF0000u) == 0x60210000u)
            lo = (w0 << 16) | (w1 & 0xFFFF);
    }
    if (!lo) lo = 0x80500000;  // fallback; logged below
    s_arenaLo = (lo + 31) & ~31u;
    s_arenaHi = hle_reserve_top(rd32(0x80000034) & ~31u);
    // OSArena.c is recompiled (OSAllocFromArenaLo/Hi read its globals): set them through the game's code.
    guest_call_args(c, sym_addr("OSSetArenaLo"), s_arenaLo, 0);
    guest_call_args(c, sym_addr("OSSetArenaHi"), s_arenaHi, 0);
    lm_log("[os] OSInit: arena %08X-%08X (%u KiB), boot lo %08X", s_arenaLo, s_arenaHi, (s_arenaHi - s_arenaLo) / 1024, bootLo);
}


HLE_FUNC(OSGetConsoleType) { c.r[3] = 0x00000003; }  // retail
HLE_FUNC(OSGetPhysicalMemSize) { c.r[3] = kMem1Size; }
HLE_FUNC(OSGetConsoleSimulatedMemSize) { c.r[3] = kMem1Size; }
HLE_FUNC(OSGetResetCode) { c.r[3] = 0; }
HLE_FUNC(OSGetProgressiveMode) { c.r[3] = 0; }
HLE_FUNC(OSSetProgressiveMode) { (void)c; }
HLE_FUNC(OSGetSoundMode) { c.r[3] = 1; }  // stereo
HLE_FUNC(OSSetSoundMode) { (void)c; }

// Language from the system settings (SRAM). 0 English, 1 German, 2 French, 3 Spanish, 4 Italian, 5 Dutch.
// Set by the language menu (Source/xbox360/language.cpp) before boot.
u32 g_osLanguage = 0;
HLE_FUNC(OSGetLanguage) { c.r[3] = g_osLanguage; }

// --- time ---------------------------------------------------------------------
HLE_FUNC(OSGetTime) {
    const u64 t = time_base();
    c.r[3] = (u32)(t >> 32);
    c.r[4] = (u32)t;
}
HLE_FUNC(OSGetTick) { c.r[3] = (u32)time_base(); }

// --- interrupts mask --------------------------------------------------------------
u32 g_eeOffLr = 0, g_eeOffFn = 0;  // diagnostics (hle_thread.cpp [late]): who last turned interrupts off
u32 g_eeOffLrT[64], g_eeOffFnT[64];  // the same per guest thread (index g_curThreadIdx)
extern u8 g_curThreadIdx;
HLE_FUNC(OSDisableInterrupts) {
    c.r[3] = (c.msr >> 15) & 1;
    if (c.msr & 0x8000u) { g_eeOffLr = g_eeOffLrT[g_curThreadIdx & 63] = c.lr; g_eeOffFn = g_eeOffFnT[g_curThreadIdx & 63] = 0x801DFC38u; }
    c.msr &= ~0x8000u;
}
HLE_FUNC(OSEnableInterrupts) {
    c.r[3] = (c.msr >> 15) & 1;
    c.mtmsr(c.msr | 0x8000u);
}
HLE_FUNC(OSRestoreInterrupts) {
    const u32 was = (c.msr >> 15) & 1;
    if (was && !c.r[3]) { g_eeOffLr = g_eeOffLrT[g_curThreadIdx & 63] = c.lr; g_eeOffFn = g_eeOffFnT[g_curThreadIdx & 63] = 0x801DFC60u; }
    c.mtmsr(c.r[3] ? (c.msr | 0x8000u) : (c.msr & ~0x8000u));
    c.r[3] = was;
}

// --- caches: guest memory is ordinary cached host memory -------------------------------
HLE_FUNC(DCFlushRange) {
    if (c.r[4] >= 0x10000) lm_log_once(0x7D000000u ^ c.lr ^ c.r[4], "[dc] DCFlushRange(%08X, %X) from %08X %s", c.r[3], c.r[4], c.lr, sym_name(c.lr));
    lm_gx_invalidate(hostp(c.r[3]), c.r[4]);
}
HLE_FUNC(DCFlushRangeNoSync) {
    if (c.r[4] >= 0x10000) lm_log_once(0x7D000000u ^ c.lr ^ c.r[4], "[dc] DCFlushRangeNoSync(%08X, %X) from %08X %s", c.r[3], c.r[4], c.lr, sym_name(c.lr));
    lm_gx_invalidate(hostp(c.r[3]), c.r[4]);
}
HLE_FUNC(DCStoreRange) {
    if (c.r[4] >= 0x10000) lm_log_once(0x7D000000u ^ c.lr ^ c.r[4], "[dc] DCStoreRange(%08X, %X) from %08X %s", c.r[3], c.r[4], c.lr, sym_name(c.lr));
    lm_gx_invalidate(hostp(c.r[3]), c.r[4]);
}
HLE_FUNC(DCStoreRangeNoSync) {
    if (c.r[4] >= 0x10000) lm_log_once(0x7D000000u ^ c.lr ^ c.r[4], "[dc] DCStoreRangeNoSync(%08X, %X) from %08X %s", c.r[3], c.r[4], c.lr, sym_name(c.lr));
    lm_gx_invalidate(hostp(c.r[3]), c.r[4]);
}
// Not a write: the Sunshine port's os_core.cpp leaves it empty. LM calls it right after its EFB copies (cache hygiene,
// LM PC port docs/RENDERING.md); telling the core would drop the copy textures it has just made.
HLE_FUNC(DCInvalidateRange) { (void)c; }
HLE_FUNC(DCZeroRange) {
    for (u32 a = c.r[3] & ~31u, e = c.r[3] + c.r[4]; a < e; a += 32) dcbz(a);
    lm_gx_written(hostp(c.r[3]), c.r[4]);
}
HLE_FUNC(ICInvalidateRange) { (void)c; }
HLE_FUNC(ICFlashInvalidate) { (void)c; }
HLE_FUNC(LCEnable) { (void)c; }
HLE_FUNC(LCDisable) { (void)c; }
HLE_FUNC(LCQueueWait) { (void)c; }
HLE_FUNC(PPCSync) { (void)c; }

// Locked-cache DMA (LCLoadBlocks / LCStoreBlocks): copy between the locked cache and MEM1.
HLE_FUNC(LCLoadBlocks) {
    const u32 dst = c.r[3], src = c.r[4], blocks = c.r[5] ? c.r[5] : 128;
    for (u32 i = 0; i < blocks * 32; i += 4) wr32(dst + i, rd32(src + i));
}
HLE_FUNC(LCStoreBlocks) {
    const u32 dst = c.r[3], src = c.r[4], blocks = c.r[5] ? c.r[5] : 128;
    for (u32 i = 0; i < blocks * 32; i += 4) wr32(dst + i, rd32(src + i));
    // DMA into main memory bypasses the CPU cache, so the game sends no flush: tell the GX core (its list and
    // texture caches) that these bytes changed.
    lm_gx_written(hostp(dst), blocks * 32);
}

// --- reports ---------------------------------------------------------------------
// Minimal printf for guest format strings: integer args from r4.., float args from f1...
static void guest_vformat(Ctx& c, u32 fmtAddr, int firstGpr, char* out, int outSize) {
    int gi = firstGpr, fi = 1, o = 0;
    for (u32 p = fmtAddr; o < outSize - 1; p++) {
        char ch = (char)rd8(p);
        if (!ch) break;
        if (ch != '%') { out[o++] = ch; continue; }
        char spec[16];
        int s = 0;
        spec[s++] = '%';
        for (;;) {
            ch = (char)rd8(++p);
            if (!ch || s >= 14) break;
            spec[s++] = ch;
            if (strchr("diouxXcspfFeEgG%", ch)) break;
        }
        spec[s] = 0;
        char tmp[256];
        tmp[0] = 0;
        const u32 arg = gi < 11 ? c.r[gi] : 0;
        switch (ch) {
        case '%': strcpy(tmp, "%"); break;
        case 's': {
            char str[200];
            int n = 0;
            for (u32 q = arg; q && n < 199; q++) { char x = (char)rd8(q); if (!x) break; str[n++] = x; }
            str[n] = 0;
            _snprintf(tmp, sizeof(tmp), spec, str);
            gi++;
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            _snprintf(tmp, sizeof(tmp), spec, fi < 9 ? c.f[fi] : 0.0);
            fi++;
            break;
        default:
            _snprintf(tmp, sizeof(tmp), spec, arg);
            gi++;
        }
        tmp[sizeof(tmp) - 1] = 0;
        for (int i = 0; tmp[i] && o < outSize - 1; i++) out[o++] = tmp[i];
        if (!ch) break;
    }
    while (o > 0 && (out[o - 1] == '\n' || out[o - 1] == '\r')) o--;
    out[o] = 0;
}

HLE_FUNC(OSReport) {
    char buf[512];
    guest_vformat(c, c.r[3], 4, buf, sizeof(buf));
    lm_log("[guest] %s", buf);
}
HLE_FUNC(OSVReport) { lm_log("[guest] (vreport) fmt at %08X", c.r[3]); }
HLE_FUNC(OSPanic) {
    char buf[512];
    guest_vformat(c, c.r[5], 6, buf, sizeof(buf));
    char file[64];
    int n = 0;
    for (u32 q = c.r[3]; q && n < 63; q++) { char x = (char)rd8(q); if (!x) break; file[n++] = x; }
    file[n] = 0;
    lm_fatal("OSPanic %s:%u: %s", file, c.r[4], buf);
}

// UnlockSram(commit, offset) (os/OSRtc.c, PAL 801E18E4): the end of every __OSLockSram/__OSUnlockSram pair. The lock
// turns interrupts off and keeps them off until this call, which restores them (OSRestoreInterrupts(Scb.enabled)).
// It was a missing hook (returned 0 and did nothing), so after the game's first SRAM access (fn_801E1DD8, the
// progressive-mode setter, at boot) the main thread ran with interrupts off: interrupts were only taken when another
// thread ran, 40-145 ms late in loads, and the audio (AI interrupt, DSP frames) came late and choppy (LM-30, [late]
// log: 1.3 million deferrals with EE off, all on thread 0, EE turned off at 801E1DFC). The original body writes the
// SRAM to the RTC over EXI; here the SRAM is only memory, so a commit always succeeds, as the body records it:
// Scb (804B8780): +64 offset (64 = in sync), +68 interrupts state saved by the lock, +72 locked, +76 sync.
HLE_FUNC(UnlockSram) {
    const u32 scb = 0x804B8780u;
    if (c.r[3]) {
        wr32(scb + 76, 1);
        wr32(scb + 64, 64);
    }
    wr32(scb + 72, 0);
    const u32 enabled = rd32(scb + 68);
    c.mtmsr(enabled ? (c.msr | 0x8000u) : (c.msr & ~0x8000u));
    c.r[3] = rd32(scb + 76);
}
