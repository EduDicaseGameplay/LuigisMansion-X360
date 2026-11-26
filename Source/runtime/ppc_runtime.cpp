// Guest CPU runtime: memory, indirect calls, system instructions and paired singles.
// LuigisMansion-X360 (own code). Behaviour notes cite the Gekko manual and Dolphin's interpreter
// where the hardware leaves a result undefined.
#include "ppc_ctx.h"
#include "runtime.h"
#include "log.h"

u8* g_mem = 0;
volatile u32 g_irqPending = 0;
GuestFn* g_hooks = 0;
u8* g_hookBusy = 0;

static u8 s_lockedCache[kLockedCacheSize];
static GuestFn* s_dispatch = 0;  // one slot per 4 bytes of the text range
static u32 s_textLo = 0, s_textHi = 0;

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------
bool mem_init() {
#if defined(_XBOX)
    // 64 KiB pages, cached, read/write. The CPU reads this memory a lot: never write-combined.
    g_mem = (u8*)XPhysicalAlloc(kHostMemSize, MAXULONG_PTR, 0x10000, PAGE_READWRITE | MEM_LARGE_PAGES);
#else
    g_mem = (u8*)malloc(kHostMemSize);
#endif
    if (!g_mem) return false;
    memset(g_mem, 0, kHostMemSize);
    return true;
}

u32 mem_rd_slow(u32 a, int size) {
    if ((a >> 28) == 0xE) {  // locked cache (0xE0000000, 16 KiB)
        const u8* p = s_lockedCache + (a & (kLockedCacheSize - 1));
        if (size == 4) return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
        if (size == 2) return ((u32)p[0] << 8) | p[1];
        return p[0];
    }
    if ((a >> 24) == 0xCC) return mmio_read(a, size);
    // 0xCD.. (Wii only), 0xF... : treat as memory, report once per page.
    lm_log_once(a & 0xFFFFF000, "[mem] read %d bytes at %08X (outside MEM1/MMIO)", size, a);
    return 0;
}

void mem_wr_slow(u32 a, u32 v, int size) {
    if ((a >> 28) == 0xE) {
        u8* p = s_lockedCache + (a & (kLockedCacheSize - 1));
        if (size == 4) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
        else if (size == 2) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
        else p[0] = (u8)v;
        return;
    }
    if ((a >> 24) == 0xCC) { mmio_write(a, v, size); return; }
    lm_log_once(a & 0xFFFFF000, "[mem] write %d bytes %08X at %08X (outside MEM1/MMIO)", size, v, a);
}

void dcbz(u32 ea) {
    ea &= ~31u;
    if (is_slow(ea)) { for (int i = 0; i < 32; i += 4) mem_wr_slow(ea + i, 0, 4); return; }
    host_write_check(ea, 32, "dcbz");
    memset(hostp(ea), 0, 32);
}

void lswi(Ctx& c, int rd, u32 ea, int n) {
    int r = rd - 1, sh = 0;
    for (int i = 0; i < n; i++, sh--) {
        if (sh <= 0) { r = (r + 1) & 31; c.r[r] = 0; sh = 4; }
        c.r[r] |= rd8(ea + i) << (8 * (sh - 1));
    }
}
void stswi(Ctx& c, int rs, u32 ea, int n) {
    int r = rs - 1, sh = 0;
    for (int i = 0; i < n; i++, sh--) {
        if (sh <= 0) { r = (r + 1) & 31; sh = 4; }
        wr8(ea + i, (c.r[r] >> (8 * (sh - 1))) & 0xFF);
    }
}

// ---------------------------------------------------------------------------
// Indirect calls
// ---------------------------------------------------------------------------
void dispatch_init() {
    s_textLo = 0xFFFFFFFFu;
    s_textHi = 0;
    for (u32 i = 0; i < g_funcCount; i++) {
        if (g_funcTable[i].addr < s_textLo) s_textLo = g_funcTable[i].addr;
        if (g_funcTable[i].addr > s_textHi) s_textHi = g_funcTable[i].addr;
    }
    const u32 n = (s_textHi - s_textLo) / 4 + 1;
    s_dispatch = (GuestFn*)malloc(n * sizeof(GuestFn));
    memset(s_dispatch, 0, n * sizeof(GuestFn));
    for (u32 i = 0; i < g_funcCount; i++) s_dispatch[(g_funcTable[i].addr - s_textLo) / 4] = g_funcTable[i].fn;
    lm_log("[rt] %u functions, text %08X-%08X", g_funcCount, s_textLo, s_textHi);
}

GuestFn dispatch_lookup(u32 addr) {
    if (addr < s_textLo || addr > s_textHi || (addr & 3)) return 0;
    return s_dispatch[(addr - s_textLo) / 4];
}

void guest_call(Ctx& c, u32 addr) {
    GuestFn f = dispatch_lookup(addr);
    if (!f) {
        lm_log("[dbg] r3 %08X *r3 %08X  r12 %08X  r13+1164 %08X -> %08X", c.r[3], rd32(c.r[3]), c.r[12],
               rd32(c.r[13] + 1164), rd32(rd32(c.r[13] + 1164)));
        lm_fatal("indirect call to %08X has no entry (lr %08X)", addr, c.lr);
    }
    f(c);
}

const char* sym_name(u32 addr) {
    // Symbols are sorted by address; the containing function is the last one at or below addr.
    u32 lo = 0, hi = g_symCount;
    while (lo < hi) {
        const u32 mid = (lo + hi) / 2;
        if (g_symTable[mid].addr <= addr) lo = mid + 1; else hi = mid;
    }
    return lo ? g_symTable[lo - 1].name : "?";
}

// ---------------------------------------------------------------------------
// System instructions
// ---------------------------------------------------------------------------
extern u32 g_eeOffLr, g_eeOffFn, g_eeOffLrT[64], g_eeOffFnT[64];  // hle_os.cpp (diagnostics)
extern u8 g_curThreadIdx;
void Ctx::mtmsr(u32 v) {
    const bool wasOn = (msr & 0x8000) != 0;
    if (wasOn && !(v & 0x8000)) { g_eeOffLr = g_eeOffLrT[g_curThreadIdx & 63] = lr; g_eeOffFn = g_eeOffFnT[g_curThreadIdx & 63] = g_lmCurFn; }
    msr = v;
    if (!wasOn && (v & 0x8000) && g_irqPending) service_irq(*this);
}

u32 Ctx::mfspr(u32 spr) {
    switch (spr) {
    case 22: return 0x7FFFFFFF;           // DEC: SDK code that reads it is HLE'd
    case 287: return 0x00083214;          // PVR: Gekko
    case 1008: return spr_misc[0];        // HID0
    case 1009: return spr_misc[1];        // HID1
    case 920: return spr_misc[2] | 0xA0000000u;  // HID2 (paired singles, locked cache enabled)
    case 921: return spr_misc[3];         // WPAR
    case 1017: return spr_misc[4];        // L2CR
    default:
        lm_log_once(0x5000 + spr, "[spr] mfspr %u", spr);
        return 0;
    }
}

void Ctx::mtspr(u32 spr, u32 v) {
    switch (spr) {
    case 1008: spr_misc[0] = v; return;
    case 1009: spr_misc[1] = v; return;
    case 920: spr_misc[2] = v; return;
    case 921: spr_misc[3] = v; return;
    case 1017: spr_misc[4] = v; return;
    case 22: return;  // DEC writes: the alarm system is HLE'd
    default:
        lm_log_once(0x6000 + spr, "[spr] mtspr %u = %08X", spr, v);
    }
}

u32 Ctx::mftb(u32 spr) {
    const u64 t = time_base();
    return spr == 269 ? (u32)(t >> 32) : (u32)t;
}

void Ctx::syscall(u32 pc) { lm_log_once(pc, "[cpu] sc at %08X (%s)", pc, sym_name(pc)); }
void Ctx::rfi(u32 pc) { lm_fatal("rfi at %08X (%s): exception code must be HLE", pc, sym_name(pc)); }
void Ctx::trap(u32 pc, u32 to) {
    if (to == 31) lm_fatal("trap at %08X (%s)", pc, sym_name(pc));
}
void Ctx::unimpl(u32 pc, u32 word) {
    lm_fatal("unsupported instruction %08X at %08X (%s)", word, pc, sym_name(pc));
}

double fctiw_(double v, int truncate) {
    s32 i;
    if (v != v) i = (s32)0x80000000;
    else if (v >= 2147483647.0) i = 0x7FFFFFFF;
    else if (v <= -2147483648.0) i = (s32)0x80000000;
    else if (truncate) i = (s32)v;
    else {
        // Round to nearest even (the SDK never changes the rounding mode).
        double f = floor(v);
        double d = v - f;
        i = (s32)f;
        if (d > 0.5 || (d == 0.5 && (i & 1))) i++;
    }
    return fraw(0xFFF8000000000000ull | (u32)i);
}

// ---------------------------------------------------------------------------
// Paired singles: GQR = [st_scale 8-13 | st_type 0-2] in the low half, [ld_scale 24-29 | ld_type 16-18] high
// ---------------------------------------------------------------------------
static const float* dq_table() {
    static float t[64];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 64; i++) {
            const int s = i < 32 ? i : i - 64;  // 6-bit signed scale
            t[i] = (float)ldexp(1.0, -s);
        }
        init = true;
    }
    return t;
}

static LM_INLINE double ps_load_one(u32 ea, u32 type, u32 scale) {
    switch (type) {
    case 4: return (double)((float)rd8(ea) * dq_table()[scale]);
    case 5: return (double)((float)rd16(ea) * dq_table()[scale]);
    case 6: return (double)((float)(s8)rd8(ea) * dq_table()[scale]);
    case 7: return (double)((float)(s16)rd16(ea) * dq_table()[scale]);
    default: return (double)rdf32(ea);
    }
}

void Ctx::psq_l(int fd, u32 ea, int w, int i) {
    const u32 g = gqr[i];
    const u32 type = (g >> 16) & 7, scale = (g >> 24) & 63;
    const u32 sz = type == 4 || type == 6 ? 1 : type == 5 || type == 7 ? 2 : 4;
    f[fd] = ps_load_one(ea, type, scale);
    ps1[fd] = w ? 1.0 : ps_load_one(ea + sz, type, scale);
}

static LM_INLINE void ps_store_one(u32 ea, double v, u32 type, u32 scale) {
    if (type < 4) { wrf32(ea, (float)v); return; }
    const double x = v / dq_table()[scale];
    double lo, hi;
    switch (type) {
    case 4: lo = 0; hi = 255; break;
    case 5: lo = 0; hi = 65535; break;
    case 6: lo = -128; hi = 127; break;
    default: lo = -32768; hi = 32767; break;
    }
    const s32 q = (s32)(x < lo ? lo : x > hi ? hi : x);
    if (type == 4 || type == 6) wr8(ea, (u32)q & 0xFF);
    else wr16(ea, (u32)q & 0xFFFF);
}

void Ctx::psq_st(int fs, u32 ea, int w, int i) {
    const u32 g = gqr[i];
    const u32 type = g & 7, scale = (g >> 8) & 63;
    const u32 sz = type == 4 || type == 6 ? 1 : type == 5 || type == 7 ? 2 : 4;
    ps_store_one(ea, f[fs], type, scale);
    if (!w) ps_store_one(ea + sz, ps1[fs], type, scale);
}

// ---------------------------------------------------------------------------
// Hooks / HLE registry
// ---------------------------------------------------------------------------
struct HleReg { const char* name; GuestFn fn; HleReg* next; };
static HleReg* s_hleList = 0;
static u32 s_hleMissingCount = 0;

HleRegistrar::HleRegistrar(const char* name, GuestFn fn) {
    HleReg* r = (HleReg*)malloc(sizeof(HleReg));
    r->name = name; r->fn = fn; r->next = s_hleList; s_hleList = r;
}

static u8* s_missingSeen = 0;

// A hook slot without a host implementation: log once, return 0 in r3. Hooks are SDK functions that
// touch hardware (Tools/recomp/make_hooks.py), so skipping them is safer than running the translated body.
void hle_missing(u32 index, Ctx& c) {
    if (!s_missingSeen[index]) {
        s_missingSeen[index] = 1;
        lm_log("[hle] missing %s (%08X), lr %08X (%s)", g_hookTable[index].name, g_hookTable[index].addr, c.lr, sym_name(c.lr));
    }
    c.r[3] = 0;
}

void hooks_init() {
    g_hooks = (GuestFn*)malloc((g_hookCount + 1) * sizeof(GuestFn));
    s_missingSeen = (u8*)malloc(g_hookCount + 1);
    g_hookBusy = (u8*)malloc(g_hookCount + 1);
    memset(g_hookBusy, 0, g_hookCount + 1);
    memset(s_missingSeen, 0, g_hookCount + 1);
    u32 found = 0;
    for (u32 i = 0; i < g_hookCount; i++) {
        g_hooks[i] = 0;
        for (HleReg* r = s_hleList; r; r = r->next)
            if (strcmp(r->name, g_hookTable[i].name) == 0) { g_hooks[i] = r->fn; found++; break; }
    }
    lm_log("[hle] %u hooks, %u implemented, %u missing (logged on first call)", g_hookCount, found, g_hookCount - found);
    (void)s_hleMissingCount;
}

const char* hook_name(u32 i) { return i < g_hookCount ? g_hookTable[i].name : "?"; }
u32 hook_addr(u32 i) { return i < g_hookCount ? g_hookTable[i].addr : 0; }

// Runs the translated body of a hooked function from inside its hook (wrappers).
void hook_call_original(Ctx& c, const char* name) {
    for (u32 i = 0; i < g_hookCount; i++) {
        if (strcmp(g_hookTable[i].name, name) != 0) continue;
        const u8 was = g_hookBusy[i];
        g_hookBusy[i] = 1;
        guest_call(c, g_hookTable[i].addr);
        g_hookBusy[i] = was;
        return;
    }
    lm_fatal("hook_call_original: no hook %s", name);
}

// LM360: Ctx::psq_l / psq_st into local variables (register-localized functions, ppc_ctx.h lm_psq_l/lm_psq_st).
void ps_load_slow(const Ctx& c, u32 ea, int w, int i, double& a, double& b) {
    const u32 g = c.gqr[i];
    const u32 type = (g >> 16) & 7, scale = (g >> 24) & 63;
    const u32 sz = type == 4 || type == 6 ? 1 : type == 5 || type == 7 ? 2 : 4;
    a = ps_load_one(ea, type, scale);
    b = w ? 1.0 : ps_load_one(ea + sz, type, scale);
}
void ps_store_slow(const Ctx& c, u32 ea, int w, int i, double a, double b) {
    const u32 g = c.gqr[i];
    const u32 type = g & 7, scale = (g >> 8) & 63;
    const u32 sz = type == 4 || type == 6 ? 1 : type == 5 || type == 7 ? 2 : 4;
    ps_store_one(ea, a, type, scale);
    if (!w) ps_store_one(ea + sz, b, type, scale);
}
