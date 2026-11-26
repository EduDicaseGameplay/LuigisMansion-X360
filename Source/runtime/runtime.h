// Runtime services shared by the CPU runtime, the HLE SDK and the Xbox 360 platform layer.
#pragma once
#include "ppc_ctx.h"

#if defined(_XBOX)
#include <xtl.h>
#else
#include <stdlib.h>
#endif

bool mem_init();
void dispatch_init();
void hooks_init();
GuestFn dispatch_lookup(u32 addr);
const char* sym_name(u32 addr);
const char* hook_name(u32 i);
u32 hook_addr(u32 i);

// Hardware registers at 0xCC000000 (mmio.cpp).
u32 mmio_read(u32 a, int size);
void mmio_write(u32 a, u32 v, int size);

// Guest time base: 40.5 MHz (bus 162 MHz / 4), as OSGetTime sees it.
u64 time_base();
enum { kBusClock = 162000000, kCoreClock = 486000000, kTimerClock = kBusClock / 4 };

// HLE registration: HLE_FUNC(OSInit) { ... } defines and registers the host body of a hooked function.
struct HleRegistrar { HleRegistrar(const char* name, GuestFn fn); };
#define HLE_FUNC(name) \
    static void hle_##name(Ctx& c); \
    static HleRegistrar s_reg_##name(#name, hle_##name); \
    static void hle_##name(Ctx& c)

// Guest pointer helpers for HLE code.
LM_INLINE void* gptr(u32 a) { return a ? (void*)hostp(a) : 0; }
LM_INLINE u32 garg(Ctx& c, int n) { return c.r[3 + n]; }

// Boot (boot.cpp).
bool boot_load_dol(const char* path, u32* entry);
void boot_setup_lowmem(const char* gameId, u32 fstAddr, u32 fstSize);

void hook_call_original(Ctx& c, const char* name);
