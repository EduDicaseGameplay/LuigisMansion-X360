// LM360 glue between the recompiled game and the GX core of the Sunshine 360 port (Source/thirdparty/sms_gx).
// The game's own (translated) GX SDK writes BP/CP/XF commands through the write-gather pipe at 0xCC008000;
// mmio.cpp hands every store to lm_gx_pipe(), which feeds the core's FIFO parser. Physical addresses in the
// stream (textures, arrays, display lists, copy targets) resolve into the guest MEM1 buffer.
#include <stdint.h>
#include <sms_gx/gx_pc.h>
#include <dolphin/types.h>

// The core's own display-list recording (platform/gx/src/gx_api.cpp): the pipe is redirected into the list.
extern "C" void GXBeginDisplayList(void* list, u32 size);
extern "C" u32 GXEndDisplayList(void);

namespace gx { void GXPC_SetMemBase(void* base, uint32_t size); }
extern void (*g_wgpipeSink)(uint32_t value, int size);

static void lm_gx_pipe(uint32_t v, int size) {
    if (size == 4) GXPC_Put32(v);
    else if (size == 2) GXPC_Put16((uint16_t)v);
    else GXPC_Put8((uint8_t)v);
}

extern "C++" bool lm_gx_init(void* mem, uint32_t memSize) {
    gx::GXPC_SetMemBase(mem, memSize);
    if (!GXPC_InitAuto(1.0f)) return false;
    g_wgpipeSink = lm_gx_pipe;
    return true;
}

uint32_t lm_gx_frame_count() { return GXPC_FrameCount(); }

// The game's GXBeginDisplayList/GXEndDisplayList (HLE, Source/hle/hle_gx.cpp) record through the core: on the
// GameCube the CPU FIFO is pointed at the list buffer, so the write-gather pipe bytes land in memory instead of
// being drawn. Feeding them to the parser drew the lists at build time and left the buffers empty
// ("unknown FIFO opcode" when the list was called).
void lm_gx_begin_dl(void* list, uint32_t size) { GXBeginDisplayList(list, size); }
uint32_t lm_gx_end_dl() { return GXEndDisplayList(); }

// Cache hooks: the game's DCFlushRange/DCStoreRange/DCInvalidateRange and host writes into guest memory (disc
// reads, ARAM transfers) tell the core, which caches display lists and decoded textures by address.
// Every range goes to the core, whatever its size, as the Sunshine port's os_core.cpp does: the display-list
// cache and the texture cache trust these write stamps for several frames. Dropping ranges under 256 bytes (a
// workaround from before LM-10's real cause was found) left J3D's patched lists and CPU-written vertex arrays
// stale in the list cache: wrong colours and geometry on screen (KNOWN_ISSUES LM-16).
// Cost (console log 0.2.1: the game thread went from ~60 to ~80 ms a frame in the foyer): J3D flushes thousands of
// small ranges a frame. Those under 256 bytes now only stamp the write clock, here on the game thread (atomic page
// stamps, gx_fifo.cpp's memoryWritten: the list and texture caches see them), without a queue entry each; larger
// ones still go through the queue, in order with the commands, and also drop EFB-copy textures they overlap.
namespace gx { void memoryWritten(const void* p, size_t n); }
void lm_gx_invalidate(void* p, uint32_t n) {
    if (!p || !n) return;
    if (n < 256) gx::memoryWritten(p, n);
    else GXPC_InvalidateRange(p, n);
}
void lm_gx_written(void* p, uint32_t n) { if (p && n) GXPC_MemoryWritten(p, n); }

namespace gx { void xeGameDisplayCopy(); }
// After the game's GXCopyDisp: the threaded core's frame bookkeeping (xe_gx_queue.cpp, LM360 addition).
void lm_gx_display_copy() { gx::xeGameDisplayCopy(); }
