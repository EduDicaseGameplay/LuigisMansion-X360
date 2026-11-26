// LM360: the platform hooks the Sunshine 360 GX core expects from its own port (xenon/platform), answered for
// LuigisMansion-X360. Built by Xbox360/build_gx.ps1 with the core's prelude, so the declarations match.
#include <xtl.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

extern "C" {
// No environment on the console: KEY=VALUE lines of game:\lm_env.txt stand in for it (diagnostics such as
// SMS_XE_DEBUG / SMS_XE_TRACE_FRAME of the GX core). Without the file every switch stays at its default.
static char s_env[4096];
static int s_envLoaded = 0;
char* getenv(const char* name) {
    if (!s_envLoaded) {
        s_envLoaded = 1;
        FILE* f = fopen("game:\\lm_env.txt", "rb");
        if (f) { size_t n = fread(s_env, 1, sizeof(s_env) - 2, f); s_env[n] = 0; fclose(f); }
    }
    const size_t len = strlen(name);
    for (char* p = s_env; *p; ) {
        char* e = p;
        while (*e && *e != '\n' && *e != '\r') e++;
        if ((size_t)(e - p) > len && !strncmp(p, name, len) && p[len] == '=') {
            static char val[256];
            size_t n = (size_t)(e - p) - len - 1;
            if (n > 255) n = 255;
            memcpy(val, p + len + 1, n);
            val[n] = 0;
            return val;
        }
        p = *e ? e + 1 : e;
    }
    // The GX core runs on its render thread (the core's default) since the display-copy bookkeeping fix (LM-10);
    // SMS_XE_GX_THREAD=0 in lm_env.txt puts it back on the game thread for comparison.
    return 0;
}

int clock_gettime(int, struct timespec* ts) {
    static LARGE_INTEGER f = {0};
    LARGE_INTEGER n;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&n);
    ts->tv_sec = (time_t)(n.QuadPart / f.QuadPart);
    ts->tv_nsec = (long)((n.QuadPart % f.QuadPart) * 1000000000LL / f.QuadPart);
    return 0;
}

FILE* xe_logfile(void) {
    static FILE* f = 0;
    if (!f) f = fopen("game:\\lm360_gx.log", "w");
    return f;
}

// The game-side waits of the core poll the Sunshine port's own OS and DSP. LM's OS is HLE in Source/hle and
// the render thread must never run it (Sunshine ARCHITECTURE.md trap), so these do nothing.
int port_audio_dsp_reply_ready(void) { return 0; }
void port_irq_check(void) {}
double port_idle_seconds(void) { return 0.0; }
void xe_bench_frame(double, double, double, double, double, double, unsigned, unsigned) {}
volatile LONG g_xeCondSignals = 0, g_xeCondWaits = 0;
float sms_msl_sinf(float x) { return sinf(x); }
float sms_msl_cosf(float x) { return cosf(x); }
}

double g_dspSeconds = 0.0;
void WINAPI xe_vblank(D3DVBLANKDATA*) {}
