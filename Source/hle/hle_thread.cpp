// HLE of the Dolphin OS threads, messages, mutexes, conditions and alarms, and the delivery of host events
// as guest interrupts. LuigisMansion-X360, own code.
//
// Model (the same idea as the Sunshine 360 port's fibers, xe_fiber.cpp, and the PC recompilation's
// scheduler, written anew here): every guest thread is an Xbox 360 fiber with its own Ctx. Only one
// guest thread runs at a time, chosen by priority as on the GameCube (0 highest). A thread switch
// happens only where the Gekko would switch: a blocking call, a wakeup of a higher-priority thread,
// or the end of an interrupt. Host events (retrace, disc reads, alarms) are queued with a due time; a
// small host timer thread raises g_irqPending, and the game's own fiber runs the work at its next
// IRQ_CHECK with interrupts enabled.
//
// Guest structures (offsets from the Dolphin SDK headers, os/OSThread.h etc.):
//   OSThread: state u16 @2C8, attr u16 @2CA, suspend s32 @2CC, priority s32 @2D0, base s32 @2D4, val @2D8,
//             stackBase @304, stackEnd @308
//   OSThreadQueue: head @0, tail @4      OSMutex: queue @0, thread @8, count @C, link @10
//   OSMessageQueue: queueSend @0, queueReceive @8, msgArray @10, msgCount @14, firstIndex @18, usedCount @1C
//   OSAlarm: handler @0, tag @4, fire u64 @8, prev @10, next @14, period u64 @18, start u64 @20
#include "hle_thread.h"
#include "../runtime/log.h"

void guest_call_args(Ctx& c, u32 fn, u32 a0, u32 a1);

enum { TS_READY = 1, TS_RUNNING = 2, TS_WAITING = 4, TS_MORIBUND = 8 };

struct GuestThread {
    u32 guest;
    void* fiber;
    Ctx ctx;
    u32 entry, param;
    s32 prio;
    int state;
    s32 suspend;
    u32 waitQueue;
    u32 exitValue;
    bool detached;
    u64 order;  // round robin among equal priorities
};

static GuestThread* s_threads[64];
static int s_threadCount = 0;
static GuestThread* s_cur = 0;
static int s_schedDisabled = 0;
static u64 s_order = 0;
static u32 s_hleArea = 0;  // guest memory reserved for HLE structures (default thread, ...)

// ---------------------------------------------------------------------------
// Interrupt work queue
// ---------------------------------------------------------------------------
struct IrqItem { IrqWork fn; u32 a, b; u64 due; };
static IrqItem s_irq[256];
static int s_irqCount = 0;
static volatile u64 s_nextDue = ~0ull;
static bool s_inIrq = false;

static void update_next_due() {
    u64 n = ~0ull;
    for (int i = 0; i < s_irqCount; i++)
        if (s_irq[i].due < n) n = s_irq[i].due;
    s_nextDue = n;
    if (n <= time_base()) g_irqPending = 1;
}

void irq_post(IrqWork fn, u32 a, u32 b, u64 due) {
    if (s_irqCount >= (int)(sizeof(s_irq) / sizeof(s_irq[0]))) lm_fatal("interrupt queue full");
    IrqItem& it = s_irq[s_irqCount++];
    it.fn = fn; it.a = a; it.b = b; it.due = due;
    update_next_due();
}

// Stall watchdog (bring-up): when the retrace count stops for 3 s, report where the guest is, once per stall.
u32 vi_retrace_count();  // hle_vi.cpp
extern u32 g_traceRing[256];
extern u32 g_tracePos;
extern u8 g_traceThread[256];
extern u8 g_curThreadIdx;
static void stall_report() {
    lm_log("[stall] retrace stopped at %u; current thread index %u, irq queue %d, inIrq %d", vi_retrace_count(),
           g_curThreadIdx, s_irqCount, s_inIrq ? 1 : 0);
    for (int i = 24; i >= 1; i--) {
        const u32 k = (g_tracePos - i) & 255;
        lm_log("[stall] -%2d t%u %08X %s", i, g_traceThread[k], g_traceRing[k], sym_name(g_traceRing[k]));
    }
    lm_log_flush();
}

// Late interrupts (audio diagnostics, LM-30): when a due interrupt waits more than 30 ms, the timer thread notes
// the guest function running (g_lmCurFn/g_lmCurLr, the profiler's) at the longest point; run_due_irqs logs it.
static volatile u32 s_lateFn = 0, s_lateLr = 0, s_lateMs = 0;
static u32 s_lateInIrq = 0, s_lateEEoff = 0;
extern u32 g_eeOffLr, g_eeOffFn, g_eeOffLrT[64], g_eeOffFnT[64];  // hle_os.cpp
static u32 s_lateThread = 0, s_lateOffLr = 0, s_lateOffFn = 0;  // deferrals seen while late: inside an interrupt handler / EE off
static DWORD WINAPI timer_thread(LPVOID) {
    u32 lastRetrace = 0, lastTrace = 0;
    DWORD since = GetTickCount();
    bool reported = false;
    for (;;) {
        const u64 tnow = time_base(), due = s_nextDue;
        if (tnow >= due) {
            g_irqPending = 1;
            const u32 ms = (u32)((tnow - due) / (kTimerClock / 1000));
            if (ms >= 30 && ms > s_lateMs) { s_lateFn = g_lmCurFn; s_lateLr = g_lmCurLr; s_lateMs = ms; }
        }
        Sleep(1);
        const u32 r = vi_retrace_count();
        if (r != lastRetrace) { lastRetrace = r; since = GetTickCount(); reported = false; continue; }
        if (!reported && r > 0 && GetTickCount() - since > 3000) {
            reported = true;
            stall_report();
            lastTrace = g_tracePos;
        }
        (void)lastTrace;
    }
}

static GuestThread* best_ready();
static void switch_to(GuestThread* t);

// Runs every due work item on the current guest thread, as an interrupt would.
static bool run_due_irqs(Ctx& c) {
    if (s_inIrq) return false;
    s_inIrq = true;
    if (s_lateMs) {
        static u32 s_lateLogs = 0;
        if (s_lateLogs++ < 200)
            lm_log("[late] interrupt %u ms late; guest in %08X %s (lr %08X %s); deferred %u in a handler, %u with EE off; thread %u turned EE off in %08X %s (lr %08X %s)",
                   s_lateMs, s_lateFn, sym_name(s_lateFn), s_lateLr, sym_name(s_lateLr), s_lateInIrq, s_lateEEoff,
                   s_lateThread, s_lateOffFn, sym_name(s_lateOffFn), s_lateOffLr, sym_name(s_lateOffLr));
        s_lateMs = 0;
        s_lateInIrq = s_lateEEoff = 0;
    }
    bool any = false;
    for (;;) {
        const u64 now = time_base();
        int pick = -1;
        for (int i = 0; i < s_irqCount; i++)
            if (s_irq[i].due <= now && (pick < 0 || s_irq[i].due < s_irq[pick].due)) pick = i;
        if (pick < 0) break;
        IrqItem it = s_irq[pick];
        s_irq[pick] = s_irq[--s_irqCount];
        const u32 msr = c.msr;
        c.msr &= ~0x8000u;  // handlers run with interrupts off
        it.fn(c, it.a, it.b);
        c.msr = msr;
        any = true;
    }
    update_next_due();
    s_inIrq = false;
    return any;
}

// Bring-up memory watch: reports the first change of a guest word range, with the recent functions.
static u32 s_watchAddr = 0x8021D260, s_watchLen = 0x40, s_watchSum = 0xFFFFFFFF;
extern u32 g_traceRing[256];
extern u32 g_tracePos;
void watch_check(const char* where) {
    u32 sum = 0;
    for (u32 i = 0; i < s_watchLen; i += 4) sum = sum * 31 + rd32(s_watchAddr + i);
    if (s_watchSum == 0xFFFFFFFF) { s_watchSum = sum; return; }
    if (sum == s_watchSum) return;
    s_watchSum = sum;
    lm_log("[watch] %08X changed (%s): %08X %08X %08X %08X", s_watchAddr, where, rd32(s_watchAddr), rd32(s_watchAddr + 4),
           rd32(s_watchAddr + 8), rd32(s_watchAddr + 12));
    for (int i = 48; i >= 1; i--) {
        const u32 a = g_traceRing[(g_tracePos - i) & 255];
        lm_log("[watch] -%2d %08X %s", i, a, sym_name(a));
    }
}

void service_irq(Ctx& c) {
    watch_check("irq");
    g_irqPending = 0;
    if (!(c.msr & 0x8000) || s_inIrq) {  // retried at OSRestoreInterrupts/mtmsr
        g_irqPending = 1;
        if (s_lateMs) {
            if (s_inIrq) s_lateInIrq++;
            else { s_lateEEoff++; s_lateThread = g_curThreadIdx; s_lateOffLr = g_eeOffLrT[g_curThreadIdx & 63]; s_lateOffFn = g_eeOffFnT[g_curThreadIdx & 63]; }
        }
        return;
    }
    if (run_due_irqs(c)) sched_yield_if_needed(c);
}

// ---------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------
static GuestThread* find_thread(u32 guest) {
    for (int i = 0; i < s_threadCount; i++)
        if (s_threads[i]->guest == guest) return s_threads[i];
    return 0;
}

static void publish(GuestThread* t) {
    wr16(t->guest + 0x2C8, (u32)t->state);
    wr32(t->guest + 0x2CC, (u32)t->suspend);
    wr32(t->guest + 0x2D0, (u32)t->prio);
}

static GuestThread* best_ready() {
    GuestThread* best = 0;
    for (int i = 0; i < s_threadCount; i++) {
        GuestThread* t = s_threads[i];
        if (t->suspend > 0) continue;
        if (t->state != TS_READY && t->state != TS_RUNNING) continue;
        if (!best || t->prio < best->prio || (t->prio == best->prio && t->order < best->order)) best = t;
    }
    return best;
}

static void switch_to(GuestThread* t) {
    if (t == s_cur) return;
    GuestThread* prev = s_cur;
    if (prev->state == TS_RUNNING) prev->state = TS_READY;
    prev->order = ++s_order;
    publish(prev);
    s_cur = t;
    t->state = TS_RUNNING;
    publish(t);
    extern u8 g_curThreadIdx;
    for (int i = 0; i < s_threadCount; i++) if (s_threads[i] == t) g_curThreadIdx = (u8)i;
    SwitchToFiber(t->fiber);
    for (int i = 0; i < s_threadCount; i++) if (s_threads[i] == prev) g_curThreadIdx = (u8)i;
    // Back on prev's fiber.
}

// Blocks the current thread until some thread can run; delivers interrupts while every thread waits.
static void reschedule(Ctx& c) {
    for (;;) {
        GuestThread* t = best_ready();
        if (t) {
            if (t != s_cur) switch_to(t);
            return;
        }
        // Idle: nothing can run until an interrupt wakes someone.
#if !defined(LM_TRACE)
        g_lmCurFn = 1;  // the profiler's "idle" (runtime/prof.cpp)
#endif
        const u64 due = s_nextDue;
        const u64 now = time_base();
        if (due > now) {
            const u64 ms = (due - now) / (kTimerClock / 1000);
            Sleep(ms > 20 ? 20 : (DWORD)ms);
        }
        const u32 msr = c.msr;
        c.msr |= 0x8000u;  // the idle loop takes interrupts
        g_irqPending = 0;
        run_due_irqs(c);
        c.msr = msr;
        lm_log_kick();
    }
}

void sched_yield_if_needed(Ctx& c) {
    if (s_schedDisabled) return;
    GuestThread* t = best_ready();
    if (t && t != s_cur && t->prio < s_cur->prio) switch_to(t);
    (void)c;
}

u32 sched_current_thread() { return s_cur ? s_cur->guest : 0; }

void sched_sleep_on(Ctx& c, u32 queue) {
    s_cur->state = TS_WAITING;
    s_cur->waitQueue = queue;
    publish(s_cur);
    reschedule(c);
}

void sched_wakeup(Ctx& c, u32 queue) {
    bool any = false;
    for (int i = 0; i < s_threadCount; i++) {
        GuestThread* t = s_threads[i];
        if (t->state == TS_WAITING && t->waitQueue == queue) {
            t->state = TS_READY;
            t->waitQueue = 0;
            t->order = ++s_order;
            publish(t);
            any = true;
        }
    }
    if (any && !s_inIrq) sched_yield_if_needed(c);
}

static void WINAPI thread_fiber(void* p) {
    GuestThread* t = (GuestThread*)p;
    Ctx& c = t->ctx;
    guest_call(c, t->entry);
    // Returned from the thread function: OSExitThread(r3).
    t->exitValue = c.r[3];
    t->state = TS_MORIBUND;
    publish(t);
    sched_wakeup(c, t->guest + 0x2DC);  // queueJoin
    reschedule(c);
    lm_fatal("exited thread %08X was scheduled again", t->guest);
}

void sched_init(Ctx& boot) {
    ConvertThreadToFiber(0);
    CreateThread(0, 0, timer_thread, 0, 0, 0);
    // The default thread: an OSThread in guest memory reserved at the top of the arena by OSInit.
    GuestThread* t = new GuestThread();
    memset(t, 0, sizeof(*t));
    t->fiber = GetCurrentFiber();
    t->prio = 16;
    t->state = TS_RUNNING;
    t->guest = s_hleArea;
    s_threads[s_threadCount++] = t;
    s_cur = t;
    (void)boot;
}

// Called by OSInit before anything else: reserves guest memory just below the arena top.
u32 hle_reserve_top(u32 arenaHi) {
    s_hleArea = (arenaHi - 0x400) & ~31u;
    memset(hostp(s_hleArea), 0, 0x400);
    if (s_cur) s_cur->guest = s_hleArea;
    return s_hleArea;
}

// The boot Ctx belongs to the default thread: the recompiled code keeps using it on the main fiber.
Ctx* g_bootCtx = 0;

// ---------------------------------------------------------------------------
// OSThread API
// ---------------------------------------------------------------------------
HLE_FUNC(OSInitThreadQueue) { wr32(c.r[3], 0); wr32(c.r[3] + 4, 0); }
HLE_FUNC(OSGetCurrentThread) { c.r[3] = sched_current_thread(); }

HLE_FUNC(OSCreateThread) {
    const u32 th = c.r[3], fn = c.r[4], param = c.r[5], stack = c.r[6], size = c.r[7], prio = c.r[8], attr = c.r[9];
    GuestThread* t = find_thread(th);
    if (!t) {
        if (s_threadCount >= 64) lm_fatal("too many guest threads");
        t = new GuestThread();
        s_threads[s_threadCount++] = t;
    } else if (t->fiber) {
        DeleteFiber(t->fiber);
    }
    memset(t, 0, sizeof(*t));
    t->guest = th;
    t->entry = fn;
    t->param = param;
    t->prio = (s32)prio;
    t->state = TS_READY;
    t->suspend = 1;
    t->detached = (attr & 1) != 0;
    t->order = ++s_order;
    t->ctx = c;  // r2, r13, GQRs and FPSCR as the creator's
    t->ctx.r[1] = (stack - 8) & ~7u;
    wr32(t->ctx.r[1], 0);
    t->ctx.r[3] = param;
    t->ctx.lr = 0;
    t->ctx.msr = 0x8000;
    t->fiber = CreateFiber(512 * 1024, (LPFIBER_START_ROUTINE)thread_fiber, t);
    if (!t->fiber) lm_fatal("CreateFiber failed for thread %08X", th);
    host_write_check(th + 0x2C8, 0x40, "OSCreateThread");
    memset(hostp(th + 0x2C8), 0, 0x40);
    wr16(th + 0x2CA, attr);
    wr32(th + 0x2D4, prio);
    wr32(th + 0x304, stack);
    wr32(th + 0x308, stack - size);
    publish(t);
    lm_log("[os] thread %08X created: entry %08X (%s) prio %d stack %08X+%X", th, fn, sym_name(fn), prio, stack - size, size);
    c.r[3] = 1;
}

HLE_FUNC(OSResumeThread) {
    GuestThread* t = find_thread(c.r[3]);
    if (!t) { c.r[3] = 0; return; }
    const s32 prev = t->suspend;
    if (t->suspend > 0) t->suspend--;
    t->order = ++s_order;
    publish(t);
    c.r[3] = (u32)prev;
    if (prev == 1) sched_yield_if_needed(c);
}

HLE_FUNC(OSSuspendThread) {
    GuestThread* t = find_thread(c.r[3]);
    if (!t) { c.r[3] = 0; return; }
    const s32 prev = t->suspend++;
    publish(t);
    c.r[3] = (u32)prev;
    if (t == s_cur) reschedule(c);
}

HLE_FUNC(OSSleepThread) { sched_sleep_on(c, c.r[3]); }
HLE_FUNC(OSWakeupThread) { sched_wakeup(c, c.r[3]); }

HLE_FUNC(OSYieldThread) {
    s_cur->order = ++s_order;
    s_cur->state = TS_READY;
    reschedule(c);
}

HLE_FUNC(OSExitThread) {
    s_cur->exitValue = c.r[3];
    s_cur->state = TS_MORIBUND;
    publish(s_cur);
    sched_wakeup(c, s_cur->guest + 0x2DC);
    reschedule(c);
    lm_fatal("exited thread resumed");
}

HLE_FUNC(OSCancelThread) {
    GuestThread* t = find_thread(c.r[3]);
    if (!t) return;
    t->state = TS_MORIBUND;
    publish(t);
    if (t == s_cur) reschedule(c);
}

HLE_FUNC(OSJoinThread) {
    GuestThread* t = find_thread(c.r[3]);
    const u32 valp = c.r[4];
    if (!t) { c.r[3] = 0; return; }
    while (t->state != TS_MORIBUND) sched_sleep_on(c, t->guest + 0x2DC);
    if (valp) wr32(valp, t->exitValue);
    c.r[3] = 1;
}

HLE_FUNC(OSDetachThread) {
    GuestThread* t = find_thread(c.r[3]);
    if (t) t->detached = true;
}
HLE_FUNC(OSIsThreadTerminated) {
    GuestThread* t = find_thread(c.r[3]);
    c.r[3] = !t || t->state == TS_MORIBUND;
}
HLE_FUNC(OSIsThreadSuspended) {
    GuestThread* t = find_thread(c.r[3]);
    c.r[3] = t && t->suspend > 0;
}
HLE_FUNC(OSSetThreadPriority) {
    GuestThread* t = find_thread(c.r[3]);
    if (!t) { c.r[3] = 0; return; }
    t->prio = (s32)c.r[4];
    publish(t);
    c.r[3] = 1;
    sched_yield_if_needed(c);
}
HLE_FUNC(OSGetThreadPriority) {
    GuestThread* t = find_thread(c.r[3]);
    c.r[3] = t ? (u32)t->prio : 31;
}
HLE_FUNC(OSDisableScheduler) { c.r[3] = (u32)s_schedDisabled++; }
HLE_FUNC(OSEnableScheduler) { c.r[3] = (u32)s_schedDisabled; if (s_schedDisabled) s_schedDisabled--; }
HLE_FUNC(OSCheckActiveThreads) {
    int n = 0;
    for (int i = 0; i < s_threadCount; i++) n += s_threads[i]->state != TS_MORIBUND;
    c.r[3] = (u32)n;
}
HLE_FUNC(OSSetSwitchThreadCallback) { c.r[3] = 0; }
HLE_FUNC(OSClearStack) { (void)c; }

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------
HLE_FUNC(OSInitMessageQueue) {
    const u32 mq = c.r[3];
    host_write_check(mq, 0x20, "OSInitMessageQueue");
    memset(hostp(mq), 0, 0x20);
    wr32(mq + 0x10, c.r[4]);
    wr32(mq + 0x14, c.r[5]);
}

HLE_FUNC(OSSendMessage) {
    const u32 mq = c.r[3], msg = c.r[4], flags = c.r[5];
    lm_log_once(c.lr ^ (msg * 2654435761u), "[os] send mq %08X msg %08X flags %u from %08X (%s)", mq, msg, flags, c.lr, sym_name(c.lr));
    while (rd32(mq + 0x1C) >= rd32(mq + 0x14)) {
        if (!(flags & 1)) { c.r[3] = 0; return; }
        sched_sleep_on(c, mq + 0x0);
    }
    const u32 count = rd32(mq + 0x14), first = rd32(mq + 0x18), used = rd32(mq + 0x1C);
    wr32(rd32(mq + 0x10) + ((first + used) % count) * 4, msg);
    wr32(mq + 0x1C, used + 1);
    sched_wakeup(c, mq + 0x8);
    c.r[3] = 1;
}

HLE_FUNC(OSJamMessage) {
    const u32 mq = c.r[3], msg = c.r[4], flags = c.r[5];
    while (rd32(mq + 0x1C) >= rd32(mq + 0x14)) {
        if (!(flags & 1)) { c.r[3] = 0; return; }
        sched_sleep_on(c, mq + 0x0);
    }
    const u32 count = rd32(mq + 0x14), used = rd32(mq + 0x1C);
    const u32 first = (rd32(mq + 0x18) + count - 1) % count;
    wr32(mq + 0x18, first);
    wr32(rd32(mq + 0x10) + first * 4, msg);
    wr32(mq + 0x1C, used + 1);
    sched_wakeup(c, mq + 0x8);
    c.r[3] = 1;
}

HLE_FUNC(OSReceiveMessage) {
    const u32 mq = c.r[3], msgp = c.r[4], flags = c.r[5];
    while (rd32(mq + 0x1C) == 0) {
        if (!(flags & 1)) { c.r[3] = 0; return; }
        sched_sleep_on(c, mq + 0x8);
    }
    const u32 count = rd32(mq + 0x14), first = rd32(mq + 0x18), used = rd32(mq + 0x1C);
    if (msgp) wr32(msgp, rd32(rd32(mq + 0x10) + first * 4));
    lm_log_once(0x40000000u ^ c.lr ^ (rd32(rd32(mq + 0x10) + first * 4) * 2654435761u), "[os] recv mq %08X msg %08X at %08X (%s)",
                mq, rd32(rd32(mq + 0x10) + first * 4), c.lr, sym_name(c.lr));
    wr32(mq + 0x18, (first + 1) % count);
    wr32(mq + 0x1C, used - 1);
    sched_wakeup(c, mq + 0x0);
    c.r[3] = 1;
}

// ---------------------------------------------------------------------------
// Mutexes and conditions
// ---------------------------------------------------------------------------
HLE_FUNC(OSInitMutex) { host_write_check(c.r[3], 0x18, "OSInitMutex"); memset(hostp(c.r[3]), 0, 0x18); }

static void lock_mutex(Ctx& c, u32 m) {
    const u32 me = sched_current_thread();
    for (;;) {
        const u32 owner = rd32(m + 8);
        if (owner == 0) { wr32(m + 8, me); wr32(m + 0xC, 1); return; }
        if (owner == me) { wr32(m + 0xC, rd32(m + 0xC) + 1); return; }
        sched_sleep_on(c, m);
    }
}
static void unlock_mutex(Ctx& c, u32 m) {
    if (rd32(m + 8) != sched_current_thread()) return;
    const u32 n = rd32(m + 0xC) - 1;
    wr32(m + 0xC, n);
    if (n == 0) { wr32(m + 8, 0); sched_wakeup(c, m); }
}

HLE_FUNC(OSLockMutex) { lock_mutex(c, c.r[3]); }
HLE_FUNC(OSUnlockMutex) { unlock_mutex(c, c.r[3]); }
HLE_FUNC(OSTryLockMutex) {
    const u32 m = c.r[3], me = sched_current_thread(), owner = rd32(m + 8);
    if (owner == 0) { wr32(m + 8, me); wr32(m + 0xC, 1); c.r[3] = 1; }
    else if (owner == me) { wr32(m + 0xC, rd32(m + 0xC) + 1); c.r[3] = 1; }
    else c.r[3] = 0;
}
HLE_FUNC(OSInitCond) { wr32(c.r[3], 0); wr32(c.r[3] + 4, 0); }
HLE_FUNC(OSWaitCond) {
    const u32 cond = c.r[3], m = c.r[4];
    const u32 count = rd32(m + 0xC);
    if (rd32(m + 8) == sched_current_thread()) { wr32(m + 0xC, 0); wr32(m + 8, 0); sched_wakeup(c, m); }
    sched_sleep_on(c, cond);
    lock_mutex(c, m);
    wr32(m + 0xC, count ? count : 1);
}
HLE_FUNC(OSSignalCond) { sched_wakeup(c, c.r[3]); }

// ---------------------------------------------------------------------------
// Alarms
// ---------------------------------------------------------------------------
struct AlarmState { u32 alarm; u32 gen; };
static AlarmState s_alarms[256];
static int s_alarmCount = 0;

static AlarmState* alarm_state(u32 a) {
    for (int i = 0; i < s_alarmCount; i++)
        if (s_alarms[i].alarm == a) return &s_alarms[i];
    if (s_alarmCount >= 256) lm_fatal("too many alarms");
    s_alarms[s_alarmCount].alarm = a;
    s_alarms[s_alarmCount].gen = 0;
    return &s_alarms[s_alarmCount++];
}

static u64 rd64(u32 a) { return ((u64)rd32(a) << 32) | rd32(a + 4); }
static void wr64(u32 a, u64 v) { wr32(a, (u32)(v >> 32)); wr32(a + 4, (u32)v); }

static void alarm_fire(Ctx& c, u32 alarm, u32 gen) {
    AlarmState* s = alarm_state(alarm);
    if (s->gen != gen) return;  // cancelled or re-armed
    const u32 handler = rd32(alarm);
    const u64 period = rd64(alarm + 0x18);
    if (period) {
        const u64 next = rd64(alarm + 8) + period;
        wr64(alarm + 8, next);
        irq_post(alarm_fire, alarm, gen, next);
    } else {
        wr32(alarm, 0);
    }
    guest_call_args(c, handler, alarm, 0);
}

HLE_FUNC(OSCreateAlarm) { host_write_check(c.r[3], 0x28, "OSCreateAlarm"); memset(hostp(c.r[3]), 0, 0x28); alarm_state(c.r[3])->gen++; }
HLE_FUNC(OSSetAlarm) {
    const u32 alarm = c.r[3];
    const u64 tick = ((u64)c.r[5] << 32) | c.r[6];
    const u32 handler = c.r[7];
    AlarmState* s = alarm_state(alarm);
    s->gen++;
    const u64 fire = time_base() + tick;
    wr32(alarm, handler);
    wr64(alarm + 8, fire);
    wr64(alarm + 0x18, 0);
    irq_post(alarm_fire, alarm, s->gen, fire);
}
HLE_FUNC(OSSetPeriodicAlarm) {
    const u32 alarm = c.r[3];
    const u64 start = ((u64)c.r[5] << 32) | c.r[6];
    const u64 period = ((u64)c.r[7] << 32) | c.r[8];
    const u32 handler = c.r[9];
    AlarmState* s = alarm_state(alarm);
    s->gen++;
    u64 fire = start;
    const u64 now = time_base();
    if (fire < now && period) fire += ((now - fire) / period + 1) * period;
    wr32(alarm, handler);
    wr64(alarm + 8, fire);
    wr64(alarm + 0x18, period);
    wr64(alarm + 0x20, start);
    irq_post(alarm_fire, alarm, s->gen, fire);
}
HLE_FUNC(OSCancelAlarm) { alarm_state(c.r[3])->gen++; wr32(c.r[3], 0); }
HLE_FUNC(OSSetAlarmTag) { wr32(c.r[3] + 4, c.r[4]); }
HLE_FUNC(OSCancelAlarms) {
    for (int i = 0; i < s_alarmCount; i++)
        if (rd32(s_alarms[i].alarm + 4) == c.r[3]) { s_alarms[i].gen++; wr32(s_alarms[i].alarm, 0); }
}

// ---------------------------------------------------------------------------
// Hardware interrupt plumbing: our HLE modules deliver their events directly.
// ---------------------------------------------------------------------------
HLE_FUNC(__OSSetInterruptHandler) { c.r[3] = 0; }
HLE_FUNC(__OSGetInterruptHandler) { c.r[3] = 0; }
HLE_FUNC(__OSMaskInterrupts) { c.r[3] = 0; }
HLE_FUNC(__OSUnmaskInterrupts) { c.r[3] = 0; }
HLE_FUNC(OSSetErrorHandler) { c.r[3] = 0; }
HLE_FUNC(DBInit) { (void)c; }
HLE_FUNC(OSInitAlarm) { (void)c; }
HLE_FUNC(OSGetResetSwitchState) { c.r[3] = 0; }
