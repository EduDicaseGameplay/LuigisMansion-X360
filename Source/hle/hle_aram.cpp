// HLE of the auxiliary RAM (16 MiB ARAM) and its DMA queue (AR, ARQ). The ARAM is host memory;
// transfers are copies done at once, completions are delivered through the interrupt queue.
// ARQRequest (arq.h): next @0, owner @4, type @8, priority @C, source @10, dest @14, length @18, callback @1C.
// Type 0 = main memory -> ARAM, 1 = ARAM -> main memory.
#include "hle_thread.h"
#include "../runtime/log.h"

void guest_call_args(Ctx& c, u32 fn, u32 a0, u32 a1);
void lm_gx_written(void* p, u32 n);  // Source/gx/lm_gx_glue.cpp

enum { kAramSize = 0x01000000, kAramBase = 0x4000 };  // the SDK reserves the first 16 KiB
static u8* s_aram = 0;
static u32 s_allocTop = kAramBase;
static u32 s_dmaCb = 0;

static void aram_init() {
    if (s_aram) return;
    s_aram = (u8*)malloc(kAramSize);
    memset(s_aram, 0, kAramSize);
}

static void copy(u32 type, u32 mainAddr, u32 aramAddr, u32 len) {
    aram_init();
    aramAddr &= kAramSize - 1;
    if (aramAddr + len > kAramSize) len = kAramSize - aramAddr;
    if (type == 0) memcpy(s_aram + aramAddr, hostp(mainAddr), len);
    else { host_write_check(mainAddr, len, "aram->main"); memcpy(hostp(mainAddr), s_aram + aramAddr, len); lm_gx_written(hostp(mainAddr), len); }
}

static const u64 kDmaDelay = kTimerClock / 10000;  // 0.1 ms

static void dma_done(Ctx& c, u32 cb, u32) { if (cb) guest_call_args(c, cb, 0, 0); }
static void arq_done(Ctx& c, u32 req, u32) {
    const u32 cb = rd32(req + 0x1C);
    if (cb) guest_call_args(c, cb, req, 0);
}

HLE_FUNC(ARInit) { aram_init(); c.r[3] = kAramBase; }
HLE_FUNC(ARCheckInit) { c.r[3] = 1; }
HLE_FUNC(ARReset) { s_allocTop = kAramBase; (void)c; }
HLE_FUNC(ARGetSize) { c.r[3] = kAramSize; }
HLE_FUNC(ARGetBaseAddress) { c.r[3] = kAramBase; }
HLE_FUNC(ARGetInternalSize) { c.r[3] = kAramSize; }
HLE_FUNC(ARAlloc) {
    const u32 len = (c.r[3] + 31) & ~31u;
    c.r[3] = s_allocTop;
    s_allocTop += len;
    if (s_allocTop > kAramSize) lm_log("[aram] out of ARAM (%u bytes asked)", len);
}
HLE_FUNC(ARFree) {
    // The SDK frees the last block only; return its address and size through the pointer.
    if (c.r[3]) wr32(c.r[3], 0);
    c.r[3] = s_allocTop;
}
HLE_FUNC(ARRegisterDMACallback) { const u32 old = s_dmaCb; s_dmaCb = c.r[3]; c.r[3] = old; }
HLE_FUNC(ARGetDMAStatus) { c.r[3] = 0; }
// ARStartDMA(type, mainmem, aram, length)
HLE_FUNC(ARStartDMA) {
    copy(c.r[3], c.r[4], c.r[5], c.r[6]);
    irq_post(dma_done, s_dmaCb, 0, time_base() + kDmaDelay);
}
HLE_FUNC(ARSetSize) { (void)c; }

HLE_FUNC(ARQInit) { aram_init(); (void)c; }
HLE_FUNC(ARQReset) { (void)c; }
HLE_FUNC(ARQSetChunkSize) { (void)c; }
HLE_FUNC(ARQGetChunkSize) { c.r[3] = 0x1000; }
HLE_FUNC(ARQFlushQueue) { (void)c; }
HLE_FUNC(ARQCheckBusy) { c.r[3] = 0; }
// ARQPostRequest(request, owner, type, priority, source, dest, length, callback)
HLE_FUNC(ARQPostRequest) {
    const u32 req = c.r[3], type = c.r[5], src = c.r[7], dst = c.r[8], len = c.r[9], cb = c.r[10];
    wr32(req + 0x00, 0);
    wr32(req + 0x04, c.r[4]);
    wr32(req + 0x08, type);
    wr32(req + 0x0C, c.r[6]);
    wr32(req + 0x10, src);
    wr32(req + 0x14, dst);
    wr32(req + 0x18, len);
    wr32(req + 0x1C, cb);
    if (type == 0) copy(0, src, dst, len);
    else copy(1, dst, src, len);
    irq_post(arq_done, req, 0, time_base() + kDmaDelay);
}
HLE_FUNC(ARQRemoveRequest) { c.r[3] = 0; }
HLE_FUNC(ARQRemoveOwnerRequest) { c.r[3] = 0; }

// The DSP mixer's view of ARAM (hle_audio.cpp: samples are read in place, as the microcode's accelerator does).
const u8* lm_aram_ptr(u32 addr) {
    aram_init();
    addr &= kAramSize - 1;
    return addr + 16 <= kAramSize ? s_aram + addr : 0;
}
