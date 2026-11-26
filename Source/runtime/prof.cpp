// Sampling profiler: where the game thread's time goes on the console. LuigisMansion-X360, own code; the technique is
// the knowledge base's (MC3-27 / JSRF "prof:": sample a cheap marker from another hardware thread, log the top).
// Each translated function stores its entry address in g_lmCurFn (LM_ENTER, ppc_ctx.h). This thread reads it once a
// millisecond and every 10 s logs the 30 most frequent with their share:
//   [prof] 10000 samples: idle 12.3%, render wait 4.1%; top: 801C3F90 create__18J3DMaterialFactory... 3.2% ...
// The address is the last function entered: time after a call returns counts for the callee until the next call,
// so read it as "around which code". Not in LM_TRACE builds (LM_ENTER is the trace there).
#include "runtime.h"
#include "log.h"
#include <stdlib.h>

volatile u32 g_lmCurFn = 0, g_lmCurLr = 0;

#if !defined(LM_TRACE)
namespace {
const int kSlots = 8192;
struct Slot { u32 fn, n; };
Slot s_tab[kSlots];
Slot s_callers[kSlots];  // by the calling function (from lr at entry)
u32 s_samples = 0;

void count(Slot* tab, u32 fn) {
    u32 h = (fn * 2654435761u) >> 19;
    for (int i = 0; i < kSlots; i++, h = (h + 1) & (kSlots - 1)) {
        if (tab[h].fn == fn) { tab[h].n++; return; }
        if (!tab[h].fn) { tab[h].fn = fn; tab[h].n = 1; return; }
    }
}

int byCount(const void* a, const void* b) {
    const Slot* x = (const Slot*)a;
    const Slot* y = (const Slot*)b;
    return x->n < y->n ? 1 : x->n > y->n ? -1 : 0;
}

void report() {
    static Slot top[kSlots];
    int m = 0;
    u32 idle = 0, wait = 0;
    for (int i = 0; i < kSlots; i++) {
        if (!s_tab[i].fn) continue;
        if (s_tab[i].fn == 1) idle = s_tab[i].n;
        else if (s_tab[i].fn == 2) wait = s_tab[i].n;
        else top[m++] = s_tab[i];
    }
    qsort(top, m, sizeof(Slot), byCount);
    const float k = 100.0f / float(s_samples);
    lm_log("[prof] %u samples: idle %.1f%%, render wait %.1f%%, game code %.1f%%", s_samples, idle * k, wait * k,
           (s_samples - idle - wait) * k);
    for (int i = 0; i < m && i < 30; i++)
        lm_log("[prof]   %5.1f%% %08X %s", top[i].n * k, top[i].fn, sym_name(top[i].fn));
    m = 0;
    for (int i = 0; i < kSlots; i++)
        if (s_callers[i].fn) top[m++] = s_callers[i];
    qsort(top, m, sizeof(Slot), byCount);
    lm_log("[prof] by caller (the function holding the return address):");
    for (int i = 0; i < m && i < 25; i++)
        lm_log("[prof]   %5.1f%% %08X %s", top[i].n * k, top[i].fn, sym_name(top[i].fn));
    memset(s_tab, 0, sizeof(s_tab));
    memset(s_callers, 0, sizeof(s_callers));
    s_samples = 0;
}

DWORD WINAPI sampler(void*) {
    for (;;) {
        Sleep(1);
        const u32 fn = g_lmCurFn;
        if (!fn) continue;
        count(s_tab, fn);
        const u32 lr = g_lmCurLr;
        if (fn > 2 && lr >= 0x80003000u && lr < 0x80400000u) count(s_callers, lr);  // the call site
        if (++s_samples >= 10000) report();
    }
}
}  // namespace

void prof_start() {
    HANDLE h = CreateThread(0, 64 * 1024, sampler, 0, CREATE_SUSPENDED, 0);
    if (!h) return;
    XSetThreadProcessor(h, 5);
    ResumeThread(h);
    lm_log("[prof] sampling profiler on hardware thread 5 (1 ms, report every 10 s)");
}
#else
void prof_start() {}
#endif
