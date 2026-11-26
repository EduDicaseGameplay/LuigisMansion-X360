// HLE of the video interface: the retrace interrupt (50 Hz PAL, 60 Hz for EURGB60/NTSC render modes),
// the pre/post retrace callbacks, VIWaitForRetrace and the frame-buffer handoff to the presenter.
#include "hle_thread.h"
#include "../runtime/log.h"

void guest_call_args(Ctx& c, u32 fn, u32 a0, u32 a1);
void present_frame(u32 xfb);  // Source/gx (stub until the renderer lands)

static u32 s_retrace = 0;
static u32 s_preCb = 0, s_postCb = 0;
static u32 s_nextFb = 0, s_curFb = 0;
static u32 s_tvFormat = 1;  // VI_PAL
static u64 s_period = kTimerClock / 50;
static u64 s_next = 0;
static bool s_black = true;
static const u32 kViQueue = 0xF0000010;  // host-only wait queue id (never a guest address)

static void retrace(Ctx& c, u32, u32) {
    s_retrace++;
    if (s_preCb) guest_call_args(c, s_preCb, s_retrace, 0);
    if (s_nextFb != s_curFb) { s_curFb = s_nextFb; }
    present_frame(s_black ? 0 : s_curFb);
    if (s_postCb) guest_call_args(c, s_postCb, s_retrace, 0);
    sched_wakeup(c, kViQueue);
    s_next += s_period;
    const u64 now = time_base();
    if (s_next + s_period < now) s_next = now;  // fell behind (slow frame): do not burst
    irq_post(retrace, 0, 0, s_next);
}

HLE_FUNC(VIInit) {
    (void)c;
    if (s_next) return;
    s_next = time_base() + s_period;
    irq_post(retrace, 0, 0, s_next);
    lm_log("[vi] init, %u Hz", (u32)(kTimerClock / s_period));
}

// GXRenderModeObj: viTVmode @0 (format << 2 | mode)
HLE_FUNC(VIConfigure) {
    const u32 tv = rd32(c.r[3]);
    lm_log("[vi] VIConfigure(%08X) tv mode %u, lr %08X (%s)", c.r[3], tv, c.lr, sym_name(c.lr));
    const u32 fmt = tv >> 2;
    const u32 hz = fmt == 1 ? 50 : 60;  // VI_PAL 50 Hz; NTSC, MPAL, EURGB60 60 Hz
    s_tvFormat = fmt;
    const u64 p = kTimerClock / hz;
    if (p != s_period) { s_period = p; lm_log("[vi] configure: tv mode %u, %u Hz", tv, hz); }
}
HLE_FUNC(VIGetTvFormat) { c.r[3] = s_tvFormat == 5 ? 1 : s_tvFormat; }
HLE_FUNC(VIGetDTVStatus) { c.r[3] = 0; }
HLE_FUNC(VIGetRetraceCount) { c.r[3] = s_retrace; }
HLE_FUNC(VIWaitForRetrace) {
    const u32 start = s_retrace;
    while (s_retrace == start) sched_sleep_on(c, kViQueue);
}
HLE_FUNC(VISetPreRetraceCallback) { const u32 old = s_preCb; s_preCb = c.r[3]; c.r[3] = old; }
HLE_FUNC(VISetPostRetraceCallback) { const u32 old = s_postCb; s_postCb = c.r[3]; c.r[3] = old; }
HLE_FUNC(VISetNextFrameBuffer) { s_nextFb = c.r[3]; }
HLE_FUNC(VIGetNextFrameBuffer) { c.r[3] = s_nextFb; }
HLE_FUNC(VIGetCurrentFrameBuffer) { c.r[3] = s_curFb; }
HLE_FUNC(VISetBlack) { s_black = c.r[3] != 0; }
HLE_FUNC(VIFlush) { (void)c; }
HLE_FUNC(VIGetNextField) { c.r[3] = s_retrace & 1; }
HLE_FUNC(VIGetCurrentLine) { c.r[3] = 0; }
HLE_FUNC(VISetVSyncTimingTest) { (void)c; }

u32 vi_retrace_count() { return s_retrace; }
