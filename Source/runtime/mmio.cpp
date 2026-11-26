// Hardware registers at 0xCC000000. The SDK units that drive hardware are HLE (hooks.txt), so game code
// only reaches the write-gather pipe (0xCC008000, inline GX vertex writes) and the odd register read.
// Every other access is logged once per register so the bring-up log lists what is still missing.
#include "runtime.h"
#include "log.h"

// The GX command stream sink (Source/gx: the sms_gx FIFO parser on the Xbox 360). Until it is
// linked in, bytes are counted only.
static u64 s_wgpipeBytes = 0;
void (*g_wgpipeSink)(u32 value, int size) = 0;

u32 mmio_read(u32 a, int size) {
    const u32 reg = a & 0x00FFFFFF;
    switch (reg & 0xFFFF00) {
    case 0x006400:  // SI
    case 0x006800:  // EXI
    case 0x005000:  // DSP
    case 0x006C00:  // AI
    case 0x002000:  // VI
    default:
        lm_log_once(0x10000000 | reg, "[mmio] read%d %08X", size * 8, a);
        return 0;
    }
}

void mmio_write(u32 a, u32 v, int size) {
    const u32 reg = a & 0x00FFFFFF;
    if (reg >= 0x008000 && reg < 0x008020) {  // write-gather pipe
        s_wgpipeBytes += size;
        if (g_wgpipeSink) g_wgpipeSink(v, size);
        return;
    }
    lm_log_once(0x20000000 | reg, "[mmio] write%d %08X = %08X", size * 8, a, v);
}

u64 wgpipe_bytes() { return s_wgpipeBytes; }
