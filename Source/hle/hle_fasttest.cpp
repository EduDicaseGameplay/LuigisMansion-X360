// Self-test of the register-localized functions (Tools/recomp/localize.txt, ppc_recomp.py localize_body): with
// game:\lm360_fasttest.txt next to default.xex, at the first PADRead each localized function and its plain
// translation (f_X_ref) run on the same random registers and memory, many times; registers (the whole Ctx) and the
// memory they touched must come out bit for bit equal. Tests only: never leave the file next to the game.
// LuigisMansion-X360, own code.
#include "../runtime/runtime.h"
#include "../runtime/log.h"
#include <stdio.h>
#include <string.h>
#include <vector>

static u32 s_seed = 0x12345678u;
static u32 rnd() { s_seed ^= s_seed << 13; s_seed ^= s_seed >> 17; s_seed ^= s_seed << 5; return s_seed; }
static u32 rnd_float_bits() {  // finite floats from ~1e-8 to ~1e8, either sign, now and then 0 or 1.0
    const u32 r = rnd();
    if ((r & 63) == 0) return 0;
    if ((r & 63) == 1) return 0x3F800000u;
    const u32 exp = 100 + (rnd() % 54);
    return (r & 0x80000000u) | (exp << 23) | (rnd() & 0x7FFFFFu);
}
static double rnd_double() {
    const u32 b = rnd_float_bits();
    float f;
    memcpy(&f, &b, 4);
    return (double)f;
}

void lm_fasttest(Ctx& c) {
    FILE* flag = fopen("game:\\lm360_fasttest.txt", "rb");
    if (!flag) return;
    fclose(flag);
    const u32 size = 0x6000;
    const u32 base = (c.r[1] - 0x1000 - size) & ~31u;  // below the caller's stack frames, restored afterwards
    std::vector<u8> saved(size), m0(size), ma(size);
    memcpy(&saved[0], hostp(base), size);
    u32 totalBad = 0;
    for (u32 k = 0; k < g_localizedCount; k++) {
        const LocalizedEntry& e = g_localized[k];
        // Only the matrix/vector functions take nothing but pointers in r3-r6: the others write the GX FIFO and
        // game globals, which random inputs would wreck (localize.txt's second group).
        if (!(e.addr >= 0x801E6000u && e.addr < 0x801E8000u) && e.addr != 0x801FE804u && e.addr != 0x801FE838u) continue;
        u32 bad = 0, firstBad = 0xFFFFFFFFu;
        const u32 trials = 4000;
        for (u32 t = 0; t < trials; t++) {
            for (u32 o = 0; o < size; o += 4) wr32(base + o, rnd_float_bits());
            Ctx a = c;
            a.r[1] = base + size - 0x100;
            a.r[3] = base + 0x1000;
            a.r[4] = base + 0x1800 + (rnd() % 8) * 4;
            a.r[5] = base + 0x2000;
            a.r[6] = base + 0x2800;
            a.r[7] = base + 0x3000;
            for (int i = 0; i < 32; i++) { a.f[i] = rnd_double(); a.ps1[i] = rnd_double(); }
            Ctx b = a;
            memcpy(&m0[0], hostp(base), size);
            e.ref(a);
            memcpy(&ma[0], hostp(base), size);
            memcpy(hostp(base), &m0[0], size);
            e.fn(b);
            const bool regs = memcmp(&a, &b, sizeof(Ctx)) == 0;
            const bool mem = memcmp(&ma[0], hostp(base), size) == 0;
            if (!regs || !mem) {
                if (!bad) firstBad = t;
                bad++;
            }
        }
        totalBad += bad;
        // speed: the same call 20000 times each way (inputs left from the last trial)
        Ctx a = c;
        a.r[1] = base + size - 0x100;
        a.r[3] = base + 0x1000;
        a.r[4] = base + 0x1800;
        a.r[5] = base + 0x2000;
        a.r[6] = base + 0x2800;
        a.r[7] = base + 0x3000;
        Ctx b = a;
        const u64 t0 = time_base();
        for (int i = 0; i < 20000; i++) { Ctx x = a; e.ref(x); }
        const u64 t1 = time_base();
        for (int i = 0; i < 20000; i++) { Ctx x = b; e.fn(x); }
        const u64 t2 = time_base();
        const double us = 1e6 / (double)kTimerClock / 20000.0;
        lm_log("[fasttest] %08X %s: %u trials, %u different%s; %.3f us -> %.3f us a call", e.addr, sym_name(e.addr),
               trials, bad, bad ? " (FAILED)" : "", (double)(t1 - t0) * us, (double)(t2 - t1) * us);
        (void)firstBad;
    }
    memcpy(hostp(base), &saved[0], size);
    lm_log("[fasttest] %u functions: %s", g_localizedCount, totalBad ? "FAILED" : "PASSED (bit for bit)");
}
