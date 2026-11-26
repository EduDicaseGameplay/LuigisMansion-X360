// Frame presentation. Until the GX renderer (sms_gx FIFO core + Xenos back end, see
// Documentation/ARCHITECTURE.md) is integrated, only frames are counted and logged.
#include "../runtime/runtime.h"
#include "../runtime/log.h"

static u32 s_frames = 0;
u64 wgpipe_bytes();
u32 lm_gx_frame_count();
u32 lm_copydisp_count();  // lm_gx_glue.cpp: display copies seen by the GX core

void present_frame(u32 xfb) {
    s_frames++;
    if ((s_frames % 250) == 0)
        lm_log("[vi] %u retraces, xfb %08X, %u KiB through the write-gather pipe, %u display copies (core), %u GXCopyDisp calls", s_frames, xfb,
               (u32)(wgpipe_bytes() >> 10), lm_gx_frame_count(), lm_copydisp_count());
}
