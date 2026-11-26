// Guest threads, scheduler and interrupt delivery (hle_thread.cpp).
#pragma once
#include "../runtime/runtime.h"

// Host events delivered as guest interrupts, in the game's own thread (like an exception on the Gekko).
typedef void (*IrqWork)(Ctx& c, u32 a, u32 b);
void irq_post(IrqWork fn, u32 a, u32 b, u64 dueTime);  // dueTime in time_base() ticks, 0 = now

// Threads
void sched_init(Ctx& bootCtx);
u32 sched_current_thread();            // guest OSThread address
void sched_sleep_on(Ctx& c, u32 queue);  // OSSleepThread semantics (queue = OSThreadQueue address)
void sched_wakeup(Ctx& c, u32 queue);    // OSWakeupThread semantics (may switch)
void sched_yield_if_needed(Ctx& c);
void sched_idle_wait(u64 untilTicks);

// The guest stack for interrupt work when no guest thread can run.
u32 irq_stack_top();
