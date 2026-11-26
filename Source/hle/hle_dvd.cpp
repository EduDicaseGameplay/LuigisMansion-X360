// HLE of the DVD library: the disc's file system comes from sys\fst.bin (already in guest memory at
// the address in 0x80000038) and file data from the extracted files\ folder next to default.xex.
// Reads complete through the interrupt queue a little later, as on the console: the PC recompilation
// found that BootScene races an async open callback when reads finish instantly (docs/RECOMPILER.md).
//
// Guest structures (dvd.h): DVDCommandBlock next @0, prev @4, command @8, state @C, offset @10, length @14,
//   addr @18, currTransferSize @1C, transferredSize @20, id @24, callback @28, userData @2C;
//   DVDFileInfo = DVDCommandBlock + startAddr @30, length @34, callback @38. DVDDir: entryNum @0, location @4, next @8.
#include "hle_thread.h"
#include "../runtime/log.h"
#include <stdio.h>

void guest_call_args(Ctx& c, u32 fn, u32 a0, u32 a1);

struct FstEntry { u32 nameOff; u32 isDir; u32 a; u32 b; };
static u32 s_fst = 0, s_fstCount = 0, s_names = 0;
static u32 s_cwd = 0;
static char s_root[64] = "game:\\files";

static FstEntry fst(u32 i) {
    FstEntry e;
    const u32 w = rd32(s_fst + i * 12);
    e.isDir = w >> 24;
    e.nameOff = w & 0xFFFFFF;
    e.a = rd32(s_fst + i * 12 + 4);
    e.b = rd32(s_fst + i * 12 + 8);
    return e;
}

static void fst_init() {
    if (s_fst) return;
    extern u32 g_fstAddr;
    s_fst = g_fstAddr;
    s_fstCount = rd32(s_fst + 8);
    s_names = s_fst + s_fstCount * 12;
    lm_log("[dvd] FST %08X, %u entries", s_fst, s_fstCount);
}

static int name_cmp(u32 nameAddr, const char* s, int n) {
    for (int i = 0; i < n; i++) {
        char a = (char)rd8(nameAddr + i), b = s[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 1;
    }
    return rd8(nameAddr + n) != 0;
}

// Path -> entry number, relative to the current directory unless it starts with '/'.
static s32 path_to_entry(const char* path) {
    fst_init();
    u32 dir = path[0] == '/' ? 0 : s_cwd;
    const char* p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char* e = p;
        while (*e && *e != '/') e++;
        const int n = (int)(e - p);
        if (n == 1 && p[0] == '.') { p = e; continue; }
        if (n == 2 && p[0] == '.' && p[1] == '.') { dir = fst(dir).a; p = e; continue; }
        const u32 end = dir == 0 ? s_fstCount : fst(dir).b;
        u32 i = dir + 1;
        s32 found = -1;
        while (i < end) {
            FstEntry f = fst(i);
            if (!name_cmp(s_names + f.nameOff, p, n)) { found = (s32)i; break; }
            i = f.isDir ? f.b : i + 1;
        }
        if (found < 0) return -1;
        dir = (u32)found;
        p = e;
    }
    return (s32)dir;
}

// Host path of an entry: game:\files\<dirs>\<name>. Parents are worked out once from the FST ranges.
static u32* s_parent = 0;

static void build_parents() {
    s_parent = (u32*)malloc(s_fstCount * sizeof(u32));
    u32 stack[64], ends[64];
    int sp = 0;
    stack[0] = 0; ends[0] = s_fstCount;
    s_parent[0] = 0;
    for (u32 i = 1; i < s_fstCount; i++) {
        while (sp > 0 && i >= ends[sp]) sp--;
        s_parent[i] = stack[sp];
        FstEntry f = fst(i);
        if (f.isDir && sp < 63) { sp++; stack[sp] = i; ends[sp] = f.b; }
    }
}

static void entry_path(u32 entry, char* out, int size) {
    if (!s_parent) build_parents();
    u32 chain[32];
    int depth = 0;
    for (u32 e = entry; e && depth < 32; e = s_parent[e]) chain[depth++] = e;
    int o = _snprintf(out, size, "%s", s_root);
    for (int i = depth - 1; i >= 0 && o < size - 1; i--) {
        out[o++] = '\\';
        for (u32 q = s_names + fst(chain[i]).nameOff; o < size - 1; q++) {
            char ch = (char)rd8(q);
            if (!ch) break;
            out[o++] = ch;
        }
    }
    out[o] = 0;
}

static const char* entry_name_dbg(s32 e) {
    static char b[64]; int n = 0;
    for (u32 q = s_names + fst((u32)e).nameOff; n < 63; q++) { char ch = (char)rd8(q); if (!ch) break; b[n++] = ch; }
    b[n] = 0; return b;
}

// Disc offset -> file (entry) for reads.
static s32 entry_at_offset(u32 off) {
    for (u32 i = 1; i < s_fstCount; i++) {
        FstEntry f = fst(i);
        if (!f.isDir && f.a <= off && off < f.a + f.b) return (s32)i;
    }
    return -1;
}

struct OpenFile { s32 entry; FILE* f; };
static OpenFile s_open[8];
static int s_openNext = 0;

static FILE* host_file(s32 entry) {
    for (int i = 0; i < 8; i++)
        if (s_open[i].f && s_open[i].entry == entry) return s_open[i].f;
    char path[256];
    entry_path((u32)entry, path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (!f) { lm_log("[dvd] cannot open %s", path); return 0; }
    OpenFile& slot = s_open[s_openNext];
    s_openNext = (s_openNext + 1) & 7;
    if (slot.f) fclose(slot.f);
    slot.entry = entry;
    slot.f = f;
    return f;
}

// Reads [discOffset, +len) into guest memory. Returns bytes read.
void watch_check(const char* where);
void lm_gx_written(void* p, u32 n);  // Source/gx/lm_gx_glue.cpp

// --- drive thread -------------------------------------------------------------------
// The host file reads run on a host thread (HW thread 3), as the LM PC port does since its issue #25: on the console
// a read from the HDD/USB took 30-150 ms, and done on the guest CPU it held every guest thread (0.3.3 log: [late]
// interrupts up to 151 ms in DVDReadPrio from LM's file thread, the game stuttering while it streams rooms; LM-31).
// The guest side prepares a request (checks, file entry, size) and the guest thread that asked sleeps (synchronous
// reads) or goes on (asynchronous ones); a self-reposting interrupt item, run on the guest side only, sees the
// completion and finishes it there (memory notice to the GX core, callback, wakeup). Only the drive thread touches
// the host files once it runs.
struct DriveReq { volatile u32 state; s32 entry; u32 fileOff, n, dst, len, got; };  // state 0 free, 1 queued, 2 done
static DriveReq s_req[64];
static volatile u32 s_reqHead = 0, s_reqTail = 0;
static HANDLE s_driveEvent = 0;
static FILE* host_file(s32 entry);
static void transfer(DriveReq& r) {
    FILE* h = host_file(r.entry);
    u32 got = 0;
    if (h) {
        fseek(h, (long)r.fileOff, SEEK_SET);
        got = (u32)fread(hostp(r.dst), 1, r.n, h);
    }
    if (got < r.len) memset(hostp(r.dst + got), 0, r.len - got);  // reads round up to 32 bytes
    r.got = got;
}
static CRITICAL_SECTION s_fileLock;  // the host files (host_file's cache): drive thread and inline reads
static DWORD WINAPI drive_thread(LPVOID) {
    for (;;) {
        WaitForSingleObject(s_driveEvent, 50);
        while (s_reqTail != s_reqHead) {
            MemoryBarrier();
            DriveReq& r = s_req[s_reqTail & 63];
            EnterCriticalSection(&s_fileLock);
            transfer(r);
            LeaveCriticalSection(&s_fileLock);
            MemoryBarrier();
            r.state = 2;
            s_reqTail = s_reqTail + 1;
        }
    }
}
static bool drive_start() {
    static int s_started = 0;
    if (!s_started) {
        s_started = 1;
        InitializeCriticalSection(&s_fileLock);
        s_driveEvent = CreateEvent(0, FALSE, FALSE, 0);
        HANDLE t = CreateThread(0, 64 * 1024, drive_thread, 0, CREATE_SUSPENDED, 0);
        if (t) { XSetThreadProcessor(t, 3); ResumeThread(t); }
        else s_started = 2;
        lm_log("[dvd] drive thread %s", t ? "started (HW thread 3)" : "failed: reads stay on the guest CPU");
    }
    return s_started == 1;
}

// Checks a read and fills a request; false = nothing to read (refused / no file).
static bool disc_prepare(DriveReq& r, u32 dst, u32 len, u32 discOff) {
    fst_init();
    const s32 e = entry_at_offset(discOff);
    if (e < 0) { lm_log("[dvd] read at disc offset %08X: no file", discOff); return 0; }
    FstEntry f = fst((u32)e);
    u32 n = len;
    if (discOff + n > f.a + f.b) n = f.a + f.b - discOff;
    if ((dst & 0x01FFFFFF) < 0x00210AA0u || (dst & 0x01FFFFFF) + len > kMem1Size) {
        // Never a game buffer: the low memory and the code sections (below 0x80210AA0). The BSS above holds JAudio's static
        // stream buffers and is a valid target (refusing it stalled the title, dimA). A read to 0 here overwrote .data
        // (KNOWN_ISSUES LM-1). Refuse it and report who asked.
        extern u32 g_traceRing[256];
        extern u32 g_tracePos;
        lm_log("[dvd] refused read to %08X len %X (disc %08X)", dst, len, discOff);
        for (int i = 20; i >= 1; i--) {
            const u32 a = g_traceRing[(g_tracePos - i) & 255];
            lm_log("[dvd]   -%2d %08X %s", i, a, sym_name(a));
        }
        return false;
    }
    host_write_check(dst, len, "dvd read");
    lm_log_once(0x30000000u ^ dst ^ discOff, "[dvd] read %08X len %X disc %08X (%s)", dst, len, discOff, entry_name_dbg(e));
    r.entry = e;
    r.fileOff = discOff - f.a;
    r.n = n;
    r.dst = dst;
    r.len = len;
    r.got = 0;
    return true;
}

// Queues a prepared request on the drive thread; returns its slot, or -1 when it was read here (no drive thread,
// queue full, or a synchronous read with interrupts off: then the console keeps the CPU too). Asynchronous reads
// never wait here, whatever the interrupt state: JAudio's stream (fn_801946A4) issues them from callbacks with
// interrupts off, and reading inline made it wait for the whole drive queue (0.3.4 log: 37-94 ms, LM-31).
static int drive_submit(Ctx& c, const DriveReq& r, bool async) {
    if ((async || (c.msr & 0x8000)) && drive_start() && s_reqHead - s_reqTail < 63) {
        const u32 slot = s_reqHead & 63;
        s_req[slot] = r;
        s_req[slot].state = 1;
        MemoryBarrier();
        s_reqHead = s_reqHead + 1;
        SetEvent(s_driveEvent);
        return (int)slot;
    }
    DriveReq tmp = r;
    const bool lock = drive_start();
    if (lock) EnterCriticalSection(&s_fileLock);
    transfer(tmp);
    if (lock) LeaveCriticalSection(&s_fileLock);
    lm_gx_written(hostp(r.dst), r.len);
    return -1;
}


static void read_done(Ctx& c, u32 block, u32 result) {
    wr32(block + 0x0C, 0);  // DVD_STATE_END
    wr32(block + 0x20, result);
    const u32 cb = rd32(block + 0x28);
    if (cb) guest_call_args(c, cb, result, block);
}

// Drive model: one serialized timeline, each request 1 ms + bytes at 30 MB/s (faster than the console's drive,
// never instantaneous), and synchronous reads put the calling thread to sleep until their completion so other
// threads run meanwhile, as on the console. The LM loader and BootScene race their own callbacks otherwise
// (same lesson as the PC recompilation's DVD HLE; pad01/inl01 crash in fn_8001F3C4).
static u64 s_driveFree = 0;
static u64 drive_due(u32 len) {
    const u64 now = time_base();
    const u64 start = s_driveFree > now ? s_driveFree : now;
    s_driveFree = start + kTimerClock / 1000 + (u64)len * 135 / 100;  // 1.35 ticks/byte = 30 MB/s
    return s_driveFree;
}

// Completion of a drive request, polled from the interrupt queue (guest side): finishes once the host read is done
// and the drive model's time has come. a = slot | kind << 8 (0 async: b = command block; 1 sync: b = wait slot).
static u64 s_reqDue[64], s_reqStart[64];
static u32 s_reqBlock[64];
static volatile u32 s_syncDone[64];
static void sync_done(Ctx& c, u32 slot, u32);
static void drive_poll(Ctx& c, u32 a, u32 b) {
    const u32 slot = a & 63, kind = a >> 8;
    DriveReq& r = s_req[slot];
    const u64 now = time_base();
    if (r.state != 2 || now < s_reqDue[slot]) {
        const u64 next = now + kTimerClock / 2000;  // 0.5 ms
        irq_post(drive_poll, a, b, r.state == 2 && s_reqDue[slot] > next ? s_reqDue[slot] : next);
        return;
    }
    MemoryBarrier();
    lm_gx_written(hostp(r.dst), r.len);
    const u32 len = r.len;
    const u32 ms = (u32)((now - s_reqStart[slot]) / (kTimerClock / 1000));
    static u32 s_slowLogs = 0;
    if (ms > 100 && s_slowLogs++ < 100) lm_log("[drive] slow read: %u ms for %X bytes to %08X (%s)", ms, r.len, r.dst, kind ? "sync" : "async");
    r.state = 0;
    if (kind == 0) read_done(c, b, len);
    else sync_done(c, b, 0);
}

static void start_read(Ctx& c, u32 block, u32 addr, u32 len, u32 discOff, u32 cb) {
    wr32(block + 0x08, 1);  // DVD_COMMAND_READ
    wr32(block + 0x0C, 1);  // busy
    wr32(block + 0x10, discOff);
    wr32(block + 0x14, len);
    wr32(block + 0x18, addr);
    wr32(block + 0x28, cb);
    DriveReq r;
    if (!disc_prepare(r, addr, len, discOff)) { irq_post(read_done, block, 0, drive_due(0)); return; }
    const int slot = drive_submit(c, r, true);
    if (slot < 0) { irq_post(read_done, block, len, drive_due(len)); return; }
    s_reqDue[slot] = drive_due(len);
    s_reqBlock[slot] = block;
    s_reqStart[slot] = time_base();
    irq_post(drive_poll, (u32)slot, block, time_base());
}

// Synchronous reads: the thread sleeps on a host-only queue until the drive's completion interrupt.
static u32 s_syncNext = 0;
static void sync_done(Ctx& c, u32 slot, u32) { s_syncDone[slot] = 1; sched_wakeup(c, 0xF0000100u + slot); }
static void wait_drive(Ctx& c, u32 len) {
    const u32 slot = s_syncNext++ & 63;
    s_syncDone[slot] = 0;
    irq_post(sync_done, slot, 0, drive_due(len));
    while (!s_syncDone[slot]) sched_sleep_on(c, 0xF0000100u + slot);
}

// --- API -------------------------------------------------------------------------
static void guest_string(u32 a, char* out, int size) {
    int i = 0;
    for (; i < size - 1; i++) { char ch = (char)rd8(a + i); if (!ch) break; out[i] = ch; }
    out[i] = 0;
}

HLE_FUNC(DVDInit) { fst_init(); (void)c; }
HLE_FUNC(DVDConvertPathToEntrynum) {
    char p[256];
    guest_string(c.r[3], p, sizeof(p));
    c.r[3] = (u32)path_to_entry(p);
}
static void fill_fileinfo(u32 fi, u32 entry) {
    memset(hostp(fi), 0, 0x3C);
    FstEntry f = fst(entry);
    wr32(fi + 0x30, f.a);
    wr32(fi + 0x34, f.b);
}
HLE_FUNC(DVDFastOpen) {
    fst_init();
    const s32 e = (s32)c.r[3];
    if (e <= 0 || (u32)e >= s_fstCount || fst((u32)e).isDir) { c.r[3] = 0; return; }
    fill_fileinfo(c.r[4], (u32)e);
    c.r[3] = 1;
}
HLE_FUNC(DVDOpen) {
    char p[256];
    guest_string(c.r[3], p, sizeof(p));
    const s32 e = path_to_entry(p);
    if (e <= 0 || fst((u32)e).isDir) { lm_log("[dvd] DVDOpen %s: not found", p); c.r[3] = 0; return; }
    fill_fileinfo(c.r[4], (u32)e);
    c.r[3] = 1;
}
HLE_FUNC(DVDClose) { c.r[3] = 1; }

// DVDReadPrio(fileInfo, addr, length, offset, prio): synchronous.
static bool bad_dst(Ctx& c, u32 addr, u32 len) {
    if ((addr & 0x01FFFFFF) >= 0x210AA0 && (addr & 0x01FFFFFF) + len <= kMem1Size) return false;
    lm_log("[dvd] read to bad address %08X len %X refused, lr %08X (%s)", addr, len, c.lr, sym_name(c.lr));
    extern u32 g_traceRing[256];
    extern u32 g_tracePos;
    for (int i = 16; i >= 1; i--) lm_log("[dvd]   -%2d %s", i, sym_name(g_traceRing[(g_tracePos - i) & 255]));
    return true;
}

HLE_FUNC(DVDReadPrio) {
    const u32 fi = c.r[3], addr = c.r[4], len = c.r[5], off = c.r[6];
    if (bad_dst(c, addr, len)) { c.r[3] = (u32)-1; return; }
    wr32(fi + 0x0C, 1);  // busy while the drive works
    DriveReq r;
    if (!disc_prepare(r, addr, len, rd32(fi + 0x30) + off)) { wr32(fi + 0x0C, 0); c.r[3] = 0; return; }
    const int slot = drive_submit(c, r, false);
    if (slot >= 0) {
        // The host read runs on the drive thread; this guest thread sleeps until it is done (other threads run).
        const u32 wait = s_syncNext++ & 63;
        s_syncDone[wait] = 0;
        s_reqDue[slot] = drive_due(len);
        s_reqStart[slot] = time_base();
        irq_post(drive_poll, (u32)slot | 0x100u, wait, time_base());
        while (!s_syncDone[wait]) sched_sleep_on(c, 0xF0000100u + wait);
    } else if (c.msr & 0x8000)
        wait_drive(c, len);  // read here (no drive thread): the drive model's time still passes
    wr32(fi + 0x0C, 0);
    c.r[3] = len;
}
HLE_FUNC(DVDReadAsyncPrio) {
    const u32 fi = c.r[3], addr = c.r[4], len = c.r[5], off = c.r[6], cb = c.r[7];
    if (bad_dst(c, addr, len)) { c.r[3] = 0; return; }
    wr32(fi + 0x38, cb);
    start_read(c, fi, addr, len, rd32(fi + 0x30) + off, cb);
    c.r[3] = 1;
}
HLE_FUNC(DVDReadAbsAsyncPrio) {
    start_read(c, c.r[3], c.r[4], c.r[5], c.r[6], c.r[7]);
    c.r[3] = 1;
}
HLE_FUNC(DVDReadAbsAsyncForBS) { start_read(c, c.r[3], c.r[4], c.r[5], c.r[6], c.r[7]); c.r[3] = 1; }
HLE_FUNC(DVDGetCommandBlockStatus) {
    const u32 s = rd32(c.r[3] + 0x0C);
    c.r[3] = s;
}
HLE_FUNC(DVDGetFileInfoStatus) { c.r[3] = rd32(c.r[3] + 0x0C); }
HLE_FUNC(DVDGetDriveStatus) { c.r[3] = 0; }
HLE_FUNC(DVDCancel) { c.r[3] = 0; }
HLE_FUNC(DVDCancelAsync) { c.r[3] = 1; }
HLE_FUNC(DVDCheckDisk) { c.r[3] = 1; }
HLE_FUNC(DVDSetAutoInvalidation) { c.r[3] = 0; }
HLE_FUNC(DVDResume) { (void)c; }
HLE_FUNC(DVDReset) { (void)c; }
HLE_FUNC(DVDSetAutoFatalMessaging) { (void)c; }

HLE_FUNC(DVDChangeDir) {
    char p[256];
    guest_string(c.r[3], p, sizeof(p));
    const s32 e = path_to_entry(p);
    if (e < 0 || (e && !fst((u32)e).isDir)) { c.r[3] = 0; return; }
    s_cwd = (u32)e;
    c.r[3] = 1;
}
HLE_FUNC(DVDOpenDir) {
    char p[256];
    guest_string(c.r[3], p, sizeof(p));
    const s32 e = path_to_entry(p);
    if (e < 0 || (e && !fst((u32)e).isDir)) { c.r[3] = 0; return; }
    wr32(c.r[4], (u32)e);
    wr32(c.r[4] + 4, (u32)e + 1);
    wr32(c.r[4] + 8, e == 0 ? s_fstCount : fst((u32)e).b);
    c.r[3] = 1;
}
// DVDDirEntry: entryNum @0, isDir @4, name @8
HLE_FUNC(DVDReadDir) {
    const u32 dir = c.r[3], out = c.r[4];
    const u32 loc = rd32(dir + 4), end = rd32(dir + 8);
    if (loc >= end) { c.r[3] = 0; return; }
    FstEntry f = fst(loc);
    wr32(out, loc);
    wr32(out + 4, f.isDir ? 1 : 0);
    wr32(out + 8, s_names + f.nameOff);
    wr32(dir + 4, f.isDir ? f.b : loc + 1);
    c.r[3] = 1;
}
HLE_FUNC(DVDCloseDir) { c.r[3] = 1; }
HLE_FUNC(DVDGetCurrentDiskID) { c.r[3] = 0x80000000; }
