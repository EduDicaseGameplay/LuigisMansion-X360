// Buffered log file. Lines go to a memory ring and are written by lm_log_flush (called once per frame
// and on fatal errors), so logging never waits on the disk inside the game (BRE v27 lesson).
#include "log.h"
#include "runtime.h"
#include <stdio.h>
#include <stdarg.h>

u32 g_traceRing[256];
u32 g_tracePos = 0;
u8 g_traceThread[256];
u8 g_curThreadIdx = 0;
bool g_watchArmed = false;  // set after the DOL is loaded
static int s_storeHits = 0;
static void dump_trace(const char* why) {
    lm_log("[watch] %s", why);
    for (int i = 24; i >= 1; i--) {
        const u32 x = g_traceRing[(g_tracePos - i) & 255];
        lm_log("[watch] -%2d %08X %s", i, x, sym_name(x));
    }
}
void watch_store(u32 a, u32 v, int size) {
    if (!g_watchArmed || s_storeHits >= 4) return;
    s_storeHits++;
    char b[128];
    _snprintf(b, sizeof(b), "guest store%d %08X at %08X", size * 8, v, a);
    dump_trace(b);
}
void host_write_check(u32 a, u32 len, const char* who) {
    const u32 lo = 0x8021D240u, hi = 0x8021D280u;
    const u32 s = 0x80000000u | (a & 0x01FFFFFF);
    if (!g_watchArmed || s_storeHits >= 4 || s + len <= lo || s >= hi) return;
    s_storeHits++;
    char b[128];
    _snprintf(b, sizeof(b), "host write %s %08X len %X", who, a, len);
    dump_trace(b);
}
#if defined(LM_TRACE)
// Bring-up watch at every guest function entry: the first word of jumptable_8021D260 (DOL value 80006454).
static bool s_watchHit = false;
void lm_enter(u32 a) {
    g_traceThread[g_tracePos & 255] = g_curThreadIdx;
    g_traceRing[g_tracePos++ & 255] = a;
    if (!s_watchHit && g_mem && *(u32*)(g_mem + 0x0021D260) != 0x80006454u && *(u32*)(g_mem + 0x0021D260) != 0) {
        // (value 0 before the DOL is loaded is skipped by the second test)
    }
    if (!s_watchHit && g_mem && g_mem[0x0021D260] == 0 && g_mem[0x0021D263] == 0 && g_mem[0x0021D262] == 0 && g_mem[0x0021D261] == 0 &&
        g_watchArmed) {
        s_watchHit = true;
        lm_log("[watch] 8021D260 zeroed, entering %08X (%s)", a, sym_name(a));
        for (int i = 64; i >= 1; i--) {
            const u32 x = g_traceRing[(g_tracePos - i) & 255];
            lm_log("[watch] -%2d %08X %s", i, x, sym_name(x));
        }
    }
}
#endif
static FILE* s_file = 0;
// Bring-up: write every line at once (a hang leaves the last lines on disk). Off once there is a frame loop.
bool g_logImmediate = true;
static char s_buf[256 * 1024];
static unsigned s_len = 0;
static unsigned s_onceKeys[2048];
static unsigned s_onceCount = 0;
#if defined(_XBOX)
static CRITICAL_SECTION s_lock;
#define LOCK() EnterCriticalSection(&s_lock)
#define UNLOCK() LeaveCriticalSection(&s_lock)
#else
#define LOCK()
#define UNLOCK()
#endif

// The log file is written by a host thread (LM-33): every line used to be written and flushed to the console's
// HDD/USB at once (g_logImmediate) by the thread that logged it, most often the game's, holding the guest CPU for
// each write (0.3.4 console log: interrupts 30-90 ms late with nothing else to explain them). Lines now go to the
// buffer and the writer thread stores them within 250 ms; lm_log_flush() (fatal errors, exit) still writes at once.
#if defined(_XBOX)
static CRITICAL_SECTION s_fileLock;
static HANDLE s_logEvent = 0;
static char s_wbuf[256 * 1024];
static void write_pending() {  // takes the buffered lines and writes them (file lock held by the caller)
    LOCK();
    const unsigned n = s_len;
    if (n) memcpy(s_wbuf, s_buf, n);
    s_len = 0;
    UNLOCK();
    if (s_file && n) {
        fwrite(s_wbuf, 1, n, s_file);
        fflush(s_file);
    }
}
static DWORD WINAPI log_writer(LPVOID) {
    for (;;) {
        WaitForSingleObject(s_logEvent, 250);
        EnterCriticalSection(&s_fileLock);
        write_pending();
        LeaveCriticalSection(&s_fileLock);
    }
}
#endif

void lm_log_open(const char* path, const char* version) {
#if defined(_XBOX)
    InitializeCriticalSection(&s_lock);
    InitializeCriticalSection(&s_fileLock);
#endif
    s_file = fopen(path, "w");
    lm_log("LuigisMansion-X360 %s", version);
    lm_log_flush();
#if defined(_XBOX)
    s_logEvent = CreateEvent(0, FALSE, FALSE, 0);
    HANDLE t = CreateThread(0, 64 * 1024, log_writer, 0, CREATE_SUSPENDED, 0);
    if (t) { XSetThreadProcessor(t, 4); ResumeThread(t); }
    else s_logEvent = 0;
#endif
}

// Asks the writer thread to store the buffered lines soon (no file access on the calling thread).
void lm_log_kick() {
#if defined(_XBOX)
    if (s_logEvent) { SetEvent(s_logEvent); return; }
#endif
    lm_log_flush();
}

void lm_log_flush() {
#if defined(_XBOX)
    if (s_logEvent) {
        EnterCriticalSection(&s_fileLock);
        write_pending();
        LeaveCriticalSection(&s_fileLock);
        return;
    }
#endif
    LOCK();
    if (s_file && s_len) {
        fwrite(s_buf, 1, s_len, s_file);
        fflush(s_file);
    }
    s_len = 0;
    UNLOCK();
}

static void vlog(const char* fmt, va_list ap) {
    char line[1024];
    int n = _vsnprintf(line, sizeof(line) - 2, fmt, ap);
    if (n < 0 || n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    LOCK();
    if (s_len + n > sizeof(s_buf)) { UNLOCK(); lm_log_flush(); LOCK(); }  // full (rare): written here
    memcpy(s_buf + s_len, line, n);
    s_len += n;
    UNLOCK();
    if (g_logImmediate) lm_log_kick();
#if defined(_XBOX)
    line[n] = 0;
    OutputDebugStringA(line);
#endif
}

void lm_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

void lm_log_once(unsigned key, const char* fmt, ...) {
    LOCK();
    for (unsigned i = 0; i < s_onceCount; i++)
        if (s_onceKeys[i] == key) { UNLOCK(); return; }
    if (s_onceCount < sizeof(s_onceKeys) / sizeof(s_onceKeys[0])) s_onceKeys[s_onceCount++] = key;
    UNLOCK();
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

void lm_fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char line[1024];
    _vsnprintf(line, sizeof(line) - 1, fmt, ap);
    line[sizeof(line) - 1] = 0;
    va_end(ap);
    lm_log("[fatal] %s", line);
#if defined(LM_TRACE)
    lm_log("[trace] failing thread index %u (last 40 entries of that thread, oldest first):", g_curThreadIdx);
    int shown = 0;
    u32 idx[40];
    for (int i = 1; i <= 256 && shown < 40; i++) {
        const u32 k = (g_tracePos - i) & 255;
        if (g_traceThread[k] == g_curThreadIdx) idx[shown++] = k;
    }
    for (int i = shown - 1; i >= 0; i--) lm_log("[trace] %08X %s", g_traceRing[idx[i]], sym_name(g_traceRing[idx[i]]));
#endif
    lm_log_flush();
#if defined(_XBOX)
    for (;;) Sleep(1000);
#else
    abort();
#endif
}
