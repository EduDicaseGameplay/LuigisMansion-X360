// Guest CPU state and guest memory access for the code made by Tools/recomp/ppc_recomp.py.
// LuigisMansion-X360. Builds with the XDK's VS2010 compiler (_XBOX, big-endian PowerPC: guest
// memory is read as is) and with clang/MSVC on a little-endian PC (host tests: byte swaps).
//
// Guest memory: MEM1 is 24 MiB at physical 0, seen through the cached (0x80...) and uncached
// (0xC0...) mirrors. Everything at or above 0xCC000000 (hardware registers, the write-gather pipe,
// the locked cache at 0xE0000000) takes the slow path in mem_slow.cpp.
#pragma once

#if defined(_MSC_VER)
typedef unsigned __int8 u8;
typedef unsigned __int16 u16;
typedef unsigned __int32 u32;
typedef unsigned __int64 u64;
typedef signed __int8 s8;
typedef signed __int16 s16;
typedef signed __int32 s32;
typedef signed __int64 s64;
#define LM_INLINE __forceinline
#define LM_NOINLINE __declspec(noinline)
#else
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
#define LM_INLINE inline __attribute__((always_inline))
#define LM_NOINLINE __attribute__((noinline))
#endif

#if defined(_XBOX)
#include <ppcintrinsics.h>
#define LM_BIG_ENDIAN 1
#else
#define LM_BIG_ENDIAN 0
#endif
#include <math.h>
#include <string.h>

enum { kMem1Size = 0x01800000, kHostMemSize = 0x02000000, kLockedCacheSize = 0x4000 };

extern u8* g_mem;              // kHostMemSize bytes, 64 KiB aligned
extern volatile u32 g_irqPending;

u32 mem_rd_slow(u32 a, int size);
void mem_wr_slow(u32 a, u32 v, int size);

LM_INLINE u32 bswap32(u32 v) {
#if defined(_MSC_VER)
    return _byteswap_ulong(v);
#else
    return __builtin_bswap32(v);
#endif
}
LM_INLINE u32 bswap16(u32 v) { return ((v & 0xFF) << 8) | ((v >> 8) & 0xFF); }

LM_INLINE u8* hostp(u32 a) { return g_mem + (a & (kHostMemSize - 1)); }
LM_INLINE bool is_slow(u32 a) { return a >= 0xCC000000u; }

#if LM_BIG_ENDIAN
#define LM_BE32(v) (v)
#define LM_BE16(v) (v)
#else
#define LM_BE32(v) bswap32(v)
#define LM_BE16(v) ((u16)bswap16(v))
#endif

// Bring-up write watch (with LM_TRACE): stores into 8021D240-8021D27F are reported with the trace.
#if defined(LM_TRACE)
void watch_store(u32 a, u32 v, int size);
#define LM_WATCH(a, v, n) do { if (((a) & 0x01FFFFC0u) == 0x0021D240u) watch_store(a, v, n); } while (0)
#else
#define LM_WATCH(a, v, n) ((void)0)
#endif

LM_INLINE u32 rd32(u32 a) {
    if (is_slow(a)) return mem_rd_slow(a, 4);
    return LM_BE32(*(const u32*)hostp(a));
}
LM_INLINE u32 rd16(u32 a) {
    if (is_slow(a)) return mem_rd_slow(a, 2);
    return LM_BE16(*(const u16*)hostp(a));
}
LM_INLINE u32 rd8(u32 a) {
    if (is_slow(a)) return mem_rd_slow(a, 1);
    return *hostp(a);
}
LM_INLINE void wr32(u32 a, u32 v) {
    LM_WATCH(a, v, 4);
    if (is_slow(a)) { mem_wr_slow(a, v, 4); return; }
    *(u32*)hostp(a) = LM_BE32(v);
}
LM_INLINE void wr16(u32 a, u32 v) {
    LM_WATCH(a, v, 2);
    if (is_slow(a)) { mem_wr_slow(a, v & 0xFFFF, 2); return; }
    *(u16*)hostp(a) = LM_BE16((u16)v);
}
LM_INLINE void wr8(u32 a, u32 v) {
    LM_WATCH(a, v, 1);
    if (is_slow(a)) { mem_wr_slow(a, v & 0xFF, 1); return; }
    *hostp(a) = (u8)v;
}

union LmF32 { float f; u32 u; };
union LmF64 { double d; u64 u; };

LM_INLINE float rdf32(u32 a) {
#if LM_BIG_ENDIAN
    if (!is_slow(a)) return *(const float*)hostp(a);
#endif
    LmF32 x; x.u = rd32(a); return x.f;
}
LM_INLINE double rdf64(u32 a) {
#if LM_BIG_ENDIAN
    if (!is_slow(a)) return *(const double*)hostp(a);
#endif
    LmF64 x; x.u = ((u64)rd32(a) << 32) | rd32(a + 4); return x.d;
}
LM_INLINE void wrf32(u32 a, float v) {
    LM_WATCH(a, 0, 4);
#if LM_BIG_ENDIAN
    if (!is_slow(a)) { *(float*)hostp(a) = v; return; }
#endif
    LmF32 x; x.f = v; wr32(a, x.u);
}
LM_INLINE void wrf64(u32 a, double v) {
    LM_WATCH(a, 0, 8);
#if LM_BIG_ENDIAN
    if (!is_slow(a)) { *(double*)hostp(a) = v; return; }
#endif
    LmF64 x; x.d = v; wr32(a, (u32)(x.u >> 32)); wr32(a + 4, (u32)x.u);
}

LM_INLINE u32 fbits_lo(double v) { LmF64 x; x.d = v; return (u32)x.u; }
LM_INLINE double fraw(u64 v) { LmF64 x; x.u = v; return x.d; }

LM_INLINE u32 rotl32(u32 v, u32 n) {
    n &= 31;
    return n ? (v << n) | (v >> (32 - n)) : v;  // rlwinm on PowerPC
}
LM_INLINE u32 cntlzw(u32 v) {
#if defined(_XBOX)
    return _CountLeadingZeros(v);
#elif defined(_MSC_VER)
    unsigned long i; return _BitScanReverse(&i, v) ? 31 - i : 32;
#else
    return v ? (u32)__builtin_clz(v) : 32;
#endif
}
// The Gekko does not trap on division by zero; results as Dolphin's interpreter.
LM_INLINE u32 divw(u32 a, u32 b) {
    s32 x = (s32)a, y = (s32)b;
    if (y == 0 || (x == (s32)0x80000000 && y == -1)) return x < 0 ? 0xFFFFFFFFu : 0u;
    return (u32)(x / y);
}
LM_INLINE u32 divwu(u32 a, u32 b) { return b ? a / b : 0u; }

LM_INLINE double fmadd_(double a, double b, double c) {
#if defined(_XBOX)
    return a * b + c;  // the Xenon compiler contracts this into fmadd (fused, like the Gekko)
#else
    return fma(a, b, c);
#endif
}
LM_INLINE double fsqrt_(double v) { return sqrt(v); }
LM_INLINE double frsqrte_(double v) { return 1.0 / sqrt(v); }
LM_INLINE double fsel_(double a, double c, double b) { return a >= 0.0 ? c : b; }
LM_INLINE double fabs_(double v) { return fabs(v); }
double fctiw_(double v, int truncate);  // ppc_runtime.cpp

struct Ctx;
typedef void (*GuestFn)(Ctx&);
struct FuncEntry { u32 addr; GuestFn fn; };
struct SymEntry { const char* name; u32 addr; };

extern GuestFn* g_hooks;
extern u8* g_hookBusy;          // set while a hook runs the translated body (hook_call_original)       // one slot per hooks.txt line, filled by hle registration
void service_irq(Ctx& c);
void guest_call(Ctx& c, u32 addr);  // indirect calls (bctrl, blrl, unknown targets)

// Bring-up tracing (build with /DLM_TRACE): a ring of the last guest function entries, printed on fatal errors.
#if defined(LM_TRACE)
extern u32 g_traceRing[256];
extern u32 g_tracePos;
void lm_enter(u32 a);  // log.cpp (a call: the inline store crashed cl with C1001)
#define LM_ENTER(a) lm_enter(a)
#define LM_BACK(a) ((void)0)
#else
// Sampling profiler (runtime/prof.cpp, KB lesson MC3-27): every translated function records its entry; a thread on
// another hardware thread samples it once a millisecond and logs the most frequent functions every 10 s ([prof]).
// One store a call. Markers: 1 = scheduler idle (waiting for an interrupt), 2 = waiting for the render thread.
extern volatile u32 g_lmCurFn, g_lmCurLr;
#define LM_ENTER(a) (g_lmCurFn = (a), g_lmCurLr = c.lr)
// LM360 (0.3.9): after each call that returns into a function, the generator sets the marker back to it
// (ppc_localize.py add_back), so the samples count the function the CPU is in, not the last one entered. One store.
#define LM_BACK(a) (g_lmCurFn = (a))
#endif

#define IRQ_CHECK(c) do { if (g_irqPending) service_irq(c); } while (0)
void hle_missing(u32 index, Ctx& c);
#define HOOKED(i) 1
#define RUN_HOOK(i, c) do { GuestFn f_ = g_hooks[i]; if (f_) f_(c); else hle_missing(i, c); } while (0)

struct GuestThread;

struct Ctx {
    u32 r[32];
    double f[32];
    double ps1[32];
    u8 cr[8];
    u32 lr, ctr;
    u32 ca, so, ov;
    u32 fpscr;
    u32 gqr[8];
    u32 msr;
    u32 spr_misc[1024 / 32];  // rarely used SPRs, by number / 32 (see mfspr)
    GuestThread* thread;

    // --- condition register ---
    LM_INLINE bool crbit(int n) const { return ((cr[n >> 2] >> (3 - (n & 3))) & 1) != 0; }
    LM_INLINE void setCrbit(int n, bool v) {
        const u8 m = (u8)(1 << (3 - (n & 3)));
        cr[n >> 2] = v ? (u8)(cr[n >> 2] | m) : (u8)(cr[n >> 2] & ~m);
    }
    LM_INLINE void cmps(int f, s32 a, s32 b) { cr[f] = (u8)((a < b ? 8 : a > b ? 4 : 2) | so); }
    LM_INLINE void cmpu(int f, u32 a, u32 b) { cr[f] = (u8)((a < b ? 8 : a > b ? 4 : 2) | so); }
    LM_INLINE void setCr0(u32 v) { cmps(0, (s32)v, 0); }
    LM_INLINE void setCr1() { cr[1] = (u8)(fpscr >> 28); }
    LM_INLINE void fcmp(int f, double a, double b) {
        cr[f] = (u8)((a != a || b != b) ? 1 : a < b ? 8 : a > b ? 4 : 2);
        fpscr = (fpscr & ~0xF000u) | ((u32)cr[f] << 12);
    }
    u32 getCr() const {
        u32 v = 0;
        for (int i = 0; i < 8; i++) v |= (u32)(cr[i] & 0xF) << (28 - 4 * i);
        return v;
    }
    void mtcrf(u32 crm, u32 v) {
        for (int i = 0; i < 8; i++)
            if (crm & (0x80 >> i)) cr[i] = (u8)((v >> (28 - 4 * i)) & 0xF);
    }
    LM_INLINE u32 getXer() const { return (so << 31) | (ov << 30) | (ca << 29); }
    LM_INLINE void setXer(u32 v) { so = (v >> 31) & 1; ov = (v >> 30) & 1; ca = (v >> 29) & 1; }
    void mtfsf(u32 fm, u32 v) {
        for (int i = 0; i < 8; i++)
            if (fm & (0x80 >> i)) {
                const u32 m = 0xFu << (28 - 4 * i);
                fpscr = (fpscr & ~m) | (v & m);
            }
    }
    void mtfsfi(int field, u32 imm) {
        const u32 sh = 28 - 4 * field;
        fpscr = (fpscr & ~(0xFu << sh)) | (imm << sh);
    }

    // --- system (ppc_runtime.cpp) ---
    void mtmsr(u32 v);
    u32 mfspr(u32 spr);
    void mtspr(u32 spr, u32 v);
    u32 mftb(u32 spr);
    void syscall(u32 pc);
    void rfi(u32 pc);
    void trap(u32 pc, u32 to);
    void unimpl(u32 pc, u32 word);
    LM_INLINE void call(u32 addr) { guest_call(*this, addr); }

    // --- paired singles (quantized loads/stores through GQR) ---
    void psq_l(int fd, u32 ea, int w, int i);
    void psq_st(int fs, u32 ea, int w, int i);
};

LM_INLINE u32 sraw(Ctx& c, u32 s, u32 b) {
    const u32 n = b & 63;
    const s32 v = (s32)s;
    if (n >= 32) { c.ca = v < 0; return v < 0 ? 0xFFFFFFFFu : 0u; }
    c.ca = (v < 0) && n && (s & ((1u << n) - 1));
    return (u32)(v >> n);
}
LM_INLINE u32 srawi(Ctx& c, u32 s, u32 n) {
    const s32 v = (s32)s;
    c.ca = (v < 0) && n && (s & ((1u << n) - 1));
    return (u32)(v >> n);
}
void dcbz(u32 ea);
// Host-side bulk writes into guest memory go through this check too (bring-up).
void host_write_check(u32 a, u32 len, const char* who);
void lswi(Ctx& c, int rd, u32 ea, int n);
void stswi(Ctx& c, int rs, u32 ea, int n);

// Tables emitted by the recompiler (lm_table.cpp).
extern const FuncEntry g_funcTable[];
extern const u32 g_funcCount;
extern const SymEntry g_symTable[];
extern const u32 g_symCount;
extern const SymEntry g_hookTable[];
extern const u32 g_hookCount;
struct LocalizedEntry { u32 addr; GuestFn fn; GuestFn ref; };
extern const LocalizedEntry g_localized[];  // LM360: register-localized functions and their plain translations
extern const u32 g_localizedCount;

// LM360: register-localized leaf functions (Tools/recomp/ppc_recomp.py localize_body, Tools/recomp/localize.txt).
// Paired-single loads/stores into local variables, by Ctx::psq_l/psq_st's rules (GQR type < 4: plain floats).
void ps_load_slow(const Ctx& c, u32 ea, int w, int i, double& a, double& b);   // ppc_runtime.cpp
void ps_store_slow(const Ctx& c, u32 ea, int w, int i, double a, double b);
LM_INLINE void lm_psq_l(const Ctx& c, u32 ea, int w, int i, double& a, double& b) {
    if (((c.gqr[i] >> 16) & 7) < 4) {
        a = (double)rdf32(ea);
        b = w ? 1.0 : (double)rdf32(ea + 4);
    } else
        ps_load_slow(c, ea, w, i, a, b);
}
LM_INLINE void lm_psq_st(const Ctx& c, u32 ea, int w, int i, double a, double b) {
    if ((c.gqr[i] & 7) < 4) {
        wrf32(ea, (float)a);
        if (!w) wrf32(ea + 4, (float)b);
    } else
        ps_store_slow(c, ea, w, i, a, b);
}
