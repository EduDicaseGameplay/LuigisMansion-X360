// Helpers for HLE code: symbol lookup by name and calls into guest functions from the host.
#include "../runtime/runtime.h"
#include "../runtime/log.h"

u32 sym_addr(const char* name) {
    for (u32 i = 0; i < g_symCount; i++)
        if (strcmp(g_symTable[i].name, name) == 0) return g_symTable[i].addr;
    lm_log_once(0x7000 + (u32)strlen(name), "[hle] symbol %s not found", name);
    return 0;
}

// Calls a guest function (callbacks, interrupt handlers) on the current guest stack, keeping the caller's
// registers. The callee gets a red zone below the caller's r1, as a CodeWarrior frame expects.
void guest_call_args(Ctx& c, u32 fn, u32 a0, u32 a1) {
    if (!fn) return;
    Ctx saved = c;
    c.r[1] = (c.r[1] - 0x100) & ~15u;
    c.r[3] = a0;
    c.r[4] = a1;
    c.lr = 0;
    guest_call(c, fn);
    // Every register comes back as it was: this runs interrupt work (callbacks, alarm handlers) in the middle
    // of guest code. Returning the callee's r3 here corrupted the interrupted code's r3 (a jump-table base in
    // JAudio's init parser, fn_8018EA8C).
    c = saved;
}

// LM-17: crash at 801C42A4 (null TEV block in J3DMaterialFactory::create, about half the runs on Xenia).
// JKRHeap::sCurrentHeap (r13+5572) is one global for every thread, as on the GameCube. The loader thread
// (LMDvdThread2, prio 17) makes a temporary 512 KB room heap current while it builds the room's models; the DVD
// thread (prio 10) runs the model-load callback fn_8002F4DC (USA name, PAL 800307E4) when its read completes, and
// that callback carves a JKRSolidHeap of half the *current* heap's free space. When the callback lands inside the
// loader's window, it takes the room heap's memory and the loader's next operator new returns 0. On the console the
// callback runs before or after that window (drive and CPU timing); on Xenia it often lands inside it.
// Fix: the room build is fn_801713CC (USA name, PAL 80174B3C): it creates a JKRSolidHeap(0x80000) in the current heap,
// makes it current, builds the room's objects and restores the previous heap before returning, so the risky window is
// exactly that call. While it runs, the DVD thread's model-load callback uses the heap that was current when the room
// build started (what it gets in the runs that do not crash: hq_2 shows 80BA5BB0), and the room heap is current again
// when the callback returns. A first version tracked "borrowed" heaps through becomeCurrentHeap and never triggered
// (fix17_5/6): the loader thread itself had made the game heap current at boot.
u32 sched_current_thread();
static u32 s_roomThread = 0, s_roomOuterHeap = 0;
static int s_roomDepth = 0;

HLE_FUNC(fn_801713CC) {
    const u32 thread = sched_current_thread(), outer = rd32(c.r[13] + 5572);
    const u32 savedThread = s_roomThread, savedOuter = s_roomOuterHeap;
    s_roomThread = thread; s_roomOuterHeap = outer; s_roomDepth++;
    hook_call_original(c, "fn_801713CC");
    s_roomDepth--; s_roomThread = savedThread; s_roomOuterHeap = savedOuter;
}

HLE_FUNC(fn_8002F4DC) {
    const u32 cur = rd32(c.r[13] + 5572);
    if (s_roomDepth <= 0 || sched_current_thread() == s_roomThread || !s_roomOuterHeap || cur == s_roomOuterHeap) {
        hook_call_original(c, "fn_8002F4DC");
        return;
    }
    lm_log("[heap] model-load callback during the room build (heap %08X): using %08X (LM-17)", cur, s_roomOuterHeap);
    wr32(c.r[13] + 5572, s_roomOuterHeap);
    hook_call_original(c, "fn_8002F4DC");
    wr32(c.r[13] + 5572, cur);  // the room heap again, as the room build left it
}
