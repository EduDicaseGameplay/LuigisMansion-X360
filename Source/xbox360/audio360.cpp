// Audio output on the Xbox 360: one XAudio2 source voice, 16-bit stereo PCM at the game's rate (32 kHz), big-endian
// as the CPU writes it. The technique is the Super Mario Sunshine 360 port's (sms-xenon-languages,
// xenon/platform/xe_audio.cpp, MIT): kBuffers buffers of kFrames circulate, each one XAudio2 has played is filled
// again on its thread and queued behind the others. LuigisMansion-X360 own code.
//
// The game side (Source/hle/hle_audio.cpp) pushes every AI block when it starts playing into a FIFO; the XAudio2
// callback takes from it. The FIFO level tells the game side how far ahead it is (its AI interrupt follows the
// device's clock, as the LM PC port's audio_out.cpp paces it).
#include <xtl.h>
#include <xaudio2.h>
#include <string.h>
#include "../runtime/log.h"

namespace {

const int kBuffers = 4, kFrames = 512;  // 4 x 16 ms at 32 kHz
const unsigned kFifo = 16384;           // frames (0.5 s), power of two
IXAudio2* g_xa;
IXAudio2SourceVoice* g_voice;
short g_pcm[kBuffers][kFrames * 2];
short g_fifo[kFifo * 2];
volatile unsigned g_rd = 0, g_wr = 0;  // frame counters (single producer: the game; single consumer: XAudio2)
volatile unsigned g_underruns = 0;

// Test aid (see dump() below): what XAudio2 is actually given, underrun silence included (audio_out.raw).
HANDLE g_outDump = INVALID_HANDLE_VALUE;
unsigned g_outDumped = 0;

void fill(short* out, int frames) {
    unsigned rd = g_rd;
    const unsigned avail = g_wr - rd;
    MemoryBarrier();
    int n = (int)(avail < (unsigned)frames ? avail : (unsigned)frames);
    for (int i = 0; i < n; i++, rd++) {
        const unsigned k = (rd & (kFifo - 1)) * 2;
        out[i * 2] = g_fifo[k];
        out[i * 2 + 1] = g_fifo[k + 1];
    }
    if (n < frames) {
        if (avail || g_wr) g_underruns++;
        memset(out + n * 2, 0, (frames - n) * 4);
    }
    MemoryBarrier();
    g_rd = rd;
    if (g_outDump != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(g_outDump, out, frames * 4, &w, NULL);
        g_outDumped += frames;
        if (g_outDumped >= 90u * 32000u) { HANDLE h = g_outDump; g_outDump = INVALID_HANDLE_VALUE; CloseHandle(h); }
    }
}

void submit(int i) {
    fill(g_pcm[i], kFrames);
    XAUDIO2_BUFFER b = {0};
    b.AudioBytes = sizeof g_pcm[i];
    b.pAudioData = (const BYTE*)g_pcm[i];
    b.pContext = (void*)(INT_PTR)i;
    g_voice->SubmitSourceBuffer(&b);
}

struct Callback : IXAudio2VoiceCallback {
    void STDMETHODCALLTYPE OnBufferEnd(void* ctx) { submit((int)(INT_PTR)ctx); }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() {}
    void STDMETHODCALLTYPE OnStreamEnd() {}
    void STDMETHODCALLTYPE OnBufferStart(void*) {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) {}
} g_callback;

}  // namespace

bool lm_audio_open(int rate) {
    if (g_voice) return true;
    IXAudio2MasteringVoice* master = NULL;
    HRESULT h = XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (SUCCEEDED(h)) h = g_xa->CreateMasteringVoice(&master);
    WAVEFORMATEX wf = {0};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = (DWORD)rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = (DWORD)rate * 4;
    if (SUCCEEDED(h)) h = g_xa->CreateSourceVoice(&g_voice, &wf, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &g_callback);
    if (FAILED(h)) {
        lm_log("[audio] XAudio2 failed: %08X (no sound)", (unsigned)h);
        g_voice = 0;
        return false;
    }
    for (int i = 0; i < kBuffers; i++) submit(i);
    g_voice->Start(0);
    lm_log("[audio] XAudio2 output: %d Hz, %d buffers of %d frames", rate, kBuffers, kFrames);
    return true;
}

// Test aid: with game:\lm360_audiodump.txt present, the first 90 s of output go to game:\audio_dump.raw (s16 BE
// stereo L, R, 32 kHz), so a test run can measure what was played.
static HANDLE s_dump = INVALID_HANDLE_VALUE;
static unsigned s_dumped = 0, s_dumpChecked = 0;
static void dump(const short* lr, unsigned frames) {
    if (!s_dumpChecked) {
        s_dumpChecked = 1;
        if (GetFileAttributesA("game:\\lm360_audiodump.txt") != (DWORD)-1) {
            s_dump = CreateFileA("game:\\audio_dump.raw", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            g_outDump = CreateFileA("game:\\audio_out.raw", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        }
    }
    if (s_dump == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(s_dump, lr, frames * 4, &w, NULL);
    s_dumped += frames;
    if (s_dumped >= 90u * 32000u) { CloseHandle(s_dump); s_dump = INVALID_HANDLE_VALUE; lm_log("[audio] dump written (%u frames)", s_dumped); }
}

// frames: AI order (R, L) 16-bit pairs; stored as (L, R) for XAudio2. Drops what does not fit.
void lm_audio_push(const short* ai, unsigned frames) {
    if (!g_voice) return;
    unsigned wr = g_wr;
    const unsigned room = kFifo - (wr - g_rd);
    if (frames > room) frames = room;
    for (unsigned i = 0; i < frames; i++, wr++) {
        const unsigned k = (wr & (kFifo - 1)) * 2;
        g_fifo[k] = ai[i * 2 + 1];
        g_fifo[k + 1] = ai[i * 2];
    }
    if (s_dump != INVALID_HANDLE_VALUE || !s_dumpChecked) {
        short tmp[2048 * 2];
        const unsigned n = frames < 2048 ? frames : 2048;
        for (unsigned i = 0; i < n; i++) { tmp[i * 2] = ai[i * 2 + 1]; tmp[i * 2 + 1] = ai[i * 2]; }
        dump(tmp, n);
    }
    MemoryBarrier();
    g_wr = wr;
}

// Frames waiting in the FIFO plus the ones XAudio2 holds (kBuffers queued).
unsigned lm_audio_queued() { return g_voice ? (g_wr - g_rd) + kBuffers * kFrames : 0; }
unsigned lm_audio_underruns() { return g_underruns; }
