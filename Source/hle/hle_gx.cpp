// HLE of the GX functions that wait on GPU interrupts or read the GPU back. Everything else in GX runs
// translated and reaches the sms_gx core through the write-gather pipe (Source/gx/lm_gx_glue.cpp).
#include "../runtime/runtime.h"
#include "../runtime/log.h"

void guest_call_args(Ctx& c, u32 fn, u32 a0, u32 a1);
static u32 s_drawSyncCb = 0, s_drawDoneCb = 0, s_token = 0;

// Draw done: the core executes commands as they arrive, so the GPU is "done" at once; what matters is the
// PE-finish interrupt, which calls the game's GXSetDrawDoneCallback. LM's European frame end (fn_80007844)
// waits for it: postRetraceCallback only sends 'DONE' once the frame was drawn. Without it no display copy
// ever followed the boot, the core never ended a frame and its textures ran out of memory.
#include "hle_thread.h"
static void draw_done_irq(Ctx& c, u32 cb, u32) { if (cb) guest_call_args(c, cb, 0, 0); }
static void draw_done_now(Ctx& c, u32 cb) {
    if (!cb) return;
    const u32 msr = c.msr;
    c.msr &= ~0x8000u;  // as in an interrupt handler
    guest_call_args(c, cb, 0, 0);
    c.msr = msr;
}
HLE_FUNC(GXDrawDone) { draw_done_now(c, s_drawDoneCb); }
HLE_FUNC(GXSetDrawDone) { irq_post(draw_done_irq, s_drawDoneCb, 0, 0); }
HLE_FUNC(GXWaitDrawDone) { (void)c; }  // the callback was already posted by GXSetDrawDone
HLE_FUNC(GXSetDrawSync) {
    s_token = c.r[3] & 0xFFFF;
    if (s_drawSyncCb) guest_call_args(c, s_drawSyncCb, s_token, 0);
}
HLE_FUNC(GXReadDrawSync) { c.r[3] = s_token; }
HLE_FUNC(GXSetDrawSyncCallback) { const u32 o = s_drawSyncCb; s_drawSyncCb = c.r[3]; c.r[3] = o; }
HLE_FUNC(GXSetDrawDoneCallback) { const u32 o = s_drawDoneCb; s_drawDoneCb = c.r[3]; c.r[3] = o; }
HLE_FUNC(GXPeekZ) { if (c.r[5]) wr32(c.r[5], 0x00FFFFFF); }
HLE_FUNC(GXPeekARGB) { if (c.r[5]) wr32(c.r[5], 0); }
HLE_FUNC(GXPokeZ) { (void)c; }
HLE_FUNC(GXPokeARGB) { (void)c; }
HLE_FUNC(GXGetGPStatus) {
    // overhi, underlow, readIdle, cmdIdle, brkpt: an idle, empty FIFO
    if (c.r[3]) wr8(c.r[3], 0);
    if (c.r[4]) wr8(c.r[4], 1);
    if (c.r[5]) wr8(c.r[5], 1);
    if (c.r[6]) wr8(c.r[6], 1);
    if (c.r[7]) wr8(c.r[7], 0);
}
HLE_FUNC(GXAbortFrame) { (void)c; }
HLE_FUNC(GXClearGPMetric) { (void)c; }
HLE_FUNC(GXSetGPMetric) { (void)c; }
HLE_FUNC(GXReadGPMetric) { if (c.r[3]) wr32(c.r[3], 0); if (c.r[4]) wr32(c.r[4], 0); }
HLE_FUNC(GXEnableBreakPt) { (void)c; }
HLE_FUNC(GXDisableBreakPt) { (void)c; }
HLE_FUNC(GXSetBreakPtCallback) { c.r[3] = 0; }
static u32 s_gxThread = 0;
HLE_FUNC(GXGetCurrentGXThread) { c.r[3] = s_gxThread; }
HLE_FUNC(GXSetCurrentGXThread) { u32 sched_current_thread(); const u32 o = s_gxThread; s_gxThread = sched_current_thread(); c.r[3] = o; }
// FIFO objects: the core has no memory FIFO; keep what the game sets so getters return it.
static u32 s_cpuFifo = 0, s_gpFifo = 0;
HLE_FUNC(GXGetCPUFifo) { c.r[3] = s_cpuFifo; }
HLE_FUNC(GXGetGPFifo) { c.r[3] = s_gpFifo; }
HLE_FUNC(GXSetCPUFifo) { s_cpuFifo = c.r[3]; }
HLE_FUNC(GXSetGPFifo) { s_gpFifo = c.r[3]; }
HLE_FUNC(GXInitFifoBase) { (void)c; }
HLE_FUNC(GXInitFifoPtrs) { (void)c; }
HLE_FUNC(GXGetFifoPtrs) { if (c.r[4]) wr32(c.r[4], 0); if (c.r[5]) wr32(c.r[5], 0); }
HLE_FUNC(GXSaveCPUFifo) { (void)c; }
HLE_FUNC(__GXSaveCPUFifoAux) { (void)c; }
HLE_FUNC(__GXCleanGPFifo) { (void)c; }
HLE_FUNC(__GXFifoInit) { (void)c; }
HLE_FUNC(__GXPEInit) { (void)c; }
HLE_FUNC(GXRedirectWriteGatherPipe) { c.r[3] = 0xCC008000; }
HLE_FUNC(GXRestoreWriteGatherPipe) { (void)c; }

void lm_gx_begin_dl(void* list, u32 size);  // lm_gx_glue.cpp
u32 lm_gx_end_dl();
HLE_FUNC(GXBeginDisplayList) { lm_gx_begin_dl(hostp(c.r[3]), c.r[4]); }
HLE_FUNC(GXEndDisplayList) { c.r[3] = lm_gx_end_dl(); }

static u32 s_copyDisp = 0;
u32 lm_copydisp_count() { return s_copyDisp; }
HLE_FUNC(GXCopyDisp) {
    s_copyDisp++;
    if (s_copyDisp <= 3) lm_log("[gx] GXCopyDisp(dest %08X, clear %u) #%u, lr %08X (%s)", c.r[3], c.r[4], s_copyDisp, c.lr, sym_name(c.lr));
    hook_call_original(c, "GXCopyDisp");
    void lm_gx_display_copy();
    void lm_shadow_pass_frame_end();
    lm_shadow_pass_frame_end();
#if !defined(LM_TRACE)
    const u32 fn = g_lmCurFn;
    g_lmCurFn = 2;  // the profiler's "render wait" (runtime/prof.cpp): the frame handoff to the render thread
    lm_gx_display_copy();
    g_lmCurFn = fn;
#else
    lm_gx_display_copy();
#endif
}
