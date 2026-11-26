// HLE of the audio hardware side (AI, DSP) and JAudio's DSP driver entry points.
// The DSP microcode is replaced by a host mixer: Source/thirdparty/sms_audio/dsp_mixer.cpp (the Super Mario Sunshine
// 360 port's, MIT; LM changes in its README.txt). It reads the voice blocks JAudio writes in guest memory and mixes
// one 80-sample subframe per DSPReleaseHalt into the buffers of the 0x82 sync frame, as the microcode does (LM PC
// port docs/AUDIO.md, "Frame protocol"). JAudio then interleaves them into the AI block (Jac_imixcopy) and hands
// it to AIInitDMA; at each AI interrupt that block goes to XAudio2 (Source/xbox360/audio360.cpp).
#include "../runtime/runtime.h"
#include "../runtime/log.h"
#include "hle_thread.h"
#include "../thirdparty/sms_audio/dsp_mixer.h"

void guest_call_args(Ctx& c, u32 fn, u32 a0, u32 a1);
void lm_gx_written(void* p, u32 n);  // Source/gx/lm_gx_glue.cpp
bool lm_audio_open(int rate);        // Source/xbox360/audio360.cpp
void lm_audio_push(const short* ai, unsigned frames);
unsigned lm_audio_queued();
unsigned lm_audio_underruns();
const u8* lm_aram_ptr(u32 addr);  // hle_aram.cpp

static u64 s_mixTb = 0;  // LM360 (0.3.8): time base ticks spent in port_dspmix_render
static bool guest_range(u32 a, u32 n) {
    if (a < 0x01800000u) a |= 0x80000000u;  // physical addresses (the DSP's view)
    return a >= 0x80000000u && a + n <= 0x81800000u && a + n >= a;
}
static u32 guest_addr(u32 a) { return a < 0x01800000u ? a | 0x80000000u : a; }

// AI DMA: JAudio hands a finished block (16-bit stereo, AI order R, L, 32 kHz) to AIInitDMA; it starts playing at
// the next AI interrupt, which is when it goes to the output. The interrupt is paced by the output's FIFO level so
// the game's audio clock follows XAudio2's (LM PC port hle_audio.cpp dmaThread does the same with its device).
static u32 s_aiCb = 0, s_aiLen = 0x280 * 4, s_aiAddr = 0;
static bool s_aiRunning = false, s_out = false, s_awaitingMix = false;
static u64 s_aiNext = 0, s_waitStart = 0;
static u32 s_ticks = 0, s_lateMix = 0, s_frames82 = 0, s_halts = 0, s_rendered = 0, s_mixes = 0, s_skipMix = 0;
// Output queue target: XAudio2's buffers + s_extraBlocks AI blocks (17.5 ms each). Each underrun raises it by one
// block (up to 8, 140 ms) and 30 s without one lowers it by one (down to 3), as the LM PC port's adaptive queue
// (docs/AUDIO.md "Underruns"): the guest CPU can be held for a while (room loads, a heavy frame).
static unsigned s_extraBlocks = 3, s_lastUnderruns = 0;
static u64 s_calmSince = 0;

static void ai_tick(Ctx& c, u32, u32) {
    if (!s_aiRunning) return;
    const u64 now = time_base();
    // JAudio's DSP overload check force-stops voices when a frame starts right after the previous one (LM PC port
    // hle_audio.cpp waitForGuestMix): wait for the game's mix of the last interrupt, for at most 100 ms.
    if (s_awaitingMix) {
        if (!s_waitStart) s_waitStart = now;
        if (now - s_waitStart < kTimerClock / 10) { irq_post(ai_tick, 0, 0, now + kTimerClock / 2000); return; }
        s_lateMix++;
    }
    s_waitStart = 0;
    const u32 frames = s_aiLen / 4;
    if (s_out && s_aiAddr && guest_range(s_aiAddr, s_aiLen)) lm_audio_push((const short*)hostp(guest_addr(s_aiAddr)), frames);
    if (s_aiCb) { s_awaitingMix = true; guest_call_args(c, s_aiCb, 0, 0); }
    u64 period = (u64)frames * kTimerClock / 32000;
    if (!period) period = kTimerClock / 200;
    if (s_out) {
        const unsigned u = lm_audio_underruns();
        if (u != s_lastUnderruns) {
            s_lastUnderruns = u;
            s_calmSince = now;
            if (s_extraBlocks < 8) s_extraBlocks++;
        } else if (now - s_calmSince > 30ull * kTimerClock && s_extraBlocks > 3) {
            s_extraBlocks--;
            s_calmSince = now;
        }
        const unsigned kTargetQueued = 4 * 512 + s_extraBlocks * frames;
        const unsigned q = lm_audio_queued();
        if (q > kTargetQueued + 2 * frames) period = period * 21 / 20;       // ahead: 5% slower
        else if (q + 2 * frames < kTargetQueued) period = period * 9 / 10;  // behind: 10% faster
    }
    s_aiNext += period;
    if (s_aiNext + 4 * period < now) s_aiNext = now;  // a long stall: do not race to catch up
    if (++s_ticks % 300 == 0) {  // about every 5 s
        // LM360 (0.3.8): the mixer's own time (DSPReleaseHalt's render) in ms per second of the game's time
        static u64 s_lastNow = 0, s_lastMix = 0;
        const double mixMs = s_lastNow && now > s_lastNow ? (double)(s_mixTb - s_lastMix) * 1000.0 / (double)(now - s_lastNow) : 0.0;
        s_lastNow = now;
        s_lastMix = s_mixTb;
        lm_log("[audio] %u AI blocks (len %u at %08X), %d voices, %u queued (target +%u), %u underruns, %u late mixes; %u frames, %u halts, %u mixed, %u imixcopy, %u skipped; mixer %.1f ms a second",
               s_ticks, s_aiLen, s_aiAddr, port_dspmix_active_voices(), s_out ? lm_audio_queued() : 0, s_extraBlocks,
               s_out ? lm_audio_underruns() : 0, s_lateMix, s_frames82, s_halts, s_rendered, s_mixes, s_skipMix, mixMs);
    }
    irq_post(ai_tick, 0, 0, s_aiNext);
}

HLE_FUNC(DspBoot__Fv) { (void)c; }  // the microcode is the host mixer
// --- DSP frame protocol (JAudio microcode) -------------------------------------------------------
// Commands (DSPSendCommands(cmds, n)): 0x81xxxxxx setup (count, voice blocks, resampling table, AFC table, effect
// lines), 0x82SSLLLL sync frame (SS subframes of 80 samples, mixer level 4.12; then bufL, bufR), 0x80000000 wait.
// Each DSPReleaseHalt mixes one subframe and raises the DSP interrupt, which JAudio handles in DspSync (message to
// its audio thread, which updates the voices and releases the halt again).
static u32 s_bufL = 0, s_bufR = 0, s_subframes = 7, s_sub = 0, s_voices = 0, s_level = 0x2000;
static const u32 kDspSync = 0x801A0040;  // DspSync__Fv (GLMP01)

static void dsp_interrupt(Ctx& c, u32, u32) { guest_call_args(c, kDspSync, 0, 0); }

static const uint8_t* mix_aram(uint32_t addr) { return lm_aram_ptr(addr); }
static int16_t* mix_mram(uint32_t addr, uint32_t samples) {
    if (!addr || !guest_range(addr, samples * 2 + 2)) return 0;
    return (int16_t*)hostp(guest_addr(addr));
}
static void* guest_table(u32 a, u32 n) { return a && guest_range(a, n) ? hostp(guest_addr(a)) : 0; }

HLE_FUNC(DSPSendCommands__FPUlUl) {
    const u32 cmds = c.r[3], n = c.r[4];
    for (u32 i = 0; i < n; ) {
        const u32 w = rd32(cmds + i * 4);
        const u32 op = w >> 24;
        if (op == 0x81) {
            const u32 count = w & 0xFFFF, voices = rd32(cmds + (i + 1) * 4), interp = rd32(cmds + (i + 2) * 4),
                      afc = rd32(cmds + (i + 3) * 4), fx = rd32(cmds + (i + 4) * 4);
            s_voices = voices;
            port_dspmix_set_aram(mix_aram);
            port_dspmix_set_mram(mix_mram);
            port_dspmix_setup(count > 64 ? 64 : count, guest_table(voices, (count ? count : 1) * 0x180),
                              (const uint32_t*)guest_table(interp, 1024), (const uint32_t*)guest_table(afc, 64),
                              guest_table(fx, 4 * 0x20));
            lm_log("[audio] DSP setup: %u voices at %08X, tables %08X %08X, effect lines %08X", count, voices, interp, afc, fx);
            i += 5;
        } else if (op == 0x82) {
            s_subframes = (w >> 16) & 0xFF;
            if (!s_subframes) s_subframes = 7;
            s_level = w & 0xFFFF;
            s_bufL = rd32(cmds + (i + 1) * 4);
            s_bufR = rd32(cmds + (i + 2) * 4);
            s_sub = 0;
            s_frames82++;
            i += 3;
        } else i += 1;
    }
    lm_log_once(0x51000000u, "[audio] DSP frame: %u x 80 samples, level %04X, buffers %08X %08X", s_subframes, s_level, s_bufL, s_bufR);
    c.r[3] = 0;
}

HLE_FUNC(DSPReleaseHalt__Fv) {
    // Mix the next subframe and raise the DSP interrupt.
    s_halts++;
    if (s_voices && s_sub < s_subframes && guest_range(s_bufL, (s_sub + 1) * 160) && guest_range(s_bufR, (s_sub + 1) * 160)) {
        const u32 off = s_sub * 80 * 2;
        const u64 t0 = time_base();
        port_dspmix_render((int16_t*)hostp(guest_addr(s_bufL) + off), (int16_t*)hostp(guest_addr(s_bufR) + off), 80,
                           (uint16_t)s_level);
        s_rendered++;
        s_mixTb += time_base() - t0;
    }
    s_sub++;
    // The DSP interrupt is raised at once, in interrupt context (EE off), instead of through the interrupt queue.
    // DspSync (801A0040) only sends message 1 to the audio thread's queue (80422280), which then runs UpdateDSP for
    // the next subframe: the 7 subframes of a frame follow each other on JAudio's audio thread (priority 2, the
    // highest). Through the queue each of the 7 hops waited for the guest to take an interrupt; on the console, with
    // the game holding the CPU (0% idle in [prof]), frames came late: 6850 DSP frames for 7800 AI blocks in the
    // 0.3.1 log, each late frame a repeated block (the choppy sound) (Documentation/KNOWN_ISSUES.md LM-30).
    static int s_depth = 0;
    if (s_depth < 8) {
        s_depth++;
        const u32 msr = c.msr;
        c.msr &= ~0x8000u;
        guest_call_args(c, kDspSync, 0, 0);
        c.msr = msr;
        s_depth--;
    } else
        irq_post(dsp_interrupt, 0, 0, 0);
    c.r[3] = 0;
}

// JAI sound starts: interrupts off for the whole call.
static void jai_start(Ctx& c, const char* name) {
    const u32 msr = c.msr;
    c.msr &= ~0x8000u;
    hook_call_original(c, name);
    c.mtmsr(msr);
}
HLE_FUNC(fn_8019754C) { jai_start(c, "fn_8019754C"); }
HLE_FUNC(fn_80197810) { jai_start(c, "fn_80197810"); }
HLE_FUNC(fn_80197B30) { jai_start(c, "fn_80197B30"); }
HLE_FUNC(DSPWaitFinish__Fv) { (void)c; }
HLE_FUNC(DiplSec__FUl) { c.r[3] = 0; }

// AI: the DMA interrupt drives JAudio's frame work; registered callback called every 5 ms block.
HLE_FUNC(AIInit) { (void)c; }
HLE_FUNC(AIInitDMA) { s_aiAddr = c.r[3]; s_aiLen = c.r[4] ? c.r[4] : s_aiLen; }
HLE_FUNC(AIStartDMA) {
    (void)c;
    if (s_aiRunning) return;
    s_aiRunning = true;
    if (!s_out) s_out = lm_audio_open(32000);
    s_aiNext = time_base();
    irq_post(ai_tick, 0, 0, s_aiNext);
}
HLE_FUNC(AIStopDMA) { s_aiRunning = false; (void)c; }
HLE_FUNC(AIGetDMAStartAddr) { c.r[3] = s_aiAddr; }
HLE_FUNC(AIRegisterDMACallback) { const u32 old = s_aiCb; s_aiCb = c.r[3]; c.r[3] = old; }
HLE_FUNC(AIGetDMABytesLeft) { c.r[3] = 0; }
HLE_FUNC(AIGetDMALength) { c.r[3] = s_aiLen; }
HLE_FUNC(AISetStreamVolLeft) { (void)c; }
HLE_FUNC(AISetStreamVolRight) { (void)c; }
HLE_FUNC(AISetStreamPlayState) { (void)c; }
HLE_FUNC(AIGetStreamPlayState) { c.r[3] = 0; }
HLE_FUNC(AISetDSPSampleRate) { (void)c; }
HLE_FUNC(AIGetDSPSampleRate) { c.r[3] = 1; }
HLE_FUNC(DSPCheckMailFromDSP) { c.r[3] = 1; }
HLE_FUNC(DSPCheckMailToDSP) { c.r[3] = 0; }
HLE_FUNC(DSPReadMailFromDSP) { c.r[3] = 0; }
HLE_FUNC(DSPSendMailToDSP) { (void)c; }
HLE_FUNC(DSPInit) { (void)c; }
HLE_FUNC(DSPAssertInt) { (void)c; }

// Jac_imixcopy(bufR, bufL, dac, frames): JAudio interleaves the DSP's two buffers into the next AI block (LM PC port
// hle_audio.cpp). Runs as translated; the check only guards against a block that is not in main RAM.
HLE_FUNC(Jac_imixcopy__FPsPsPsl) {
    const u32 out = c.r[5], n = c.r[6];
    s_awaitingMix = false;
    if (!guest_range(out, n * 4) || !guest_range(c.r[3], n * 2) || !guest_range(c.r[4], n * 2)) {
        s_skipMix++;
        lm_log_once(0x50000000u, "[audio] Jac_imixcopy(%08X, %08X, out %08X, %u) outside main RAM: skipped", c.r[3], c.r[4], out, n);
        return;
    }
    s_mixes++;
    hook_call_original(c, "Jac_imixcopy__FPsPsPsl");
}

// JAudio's DSP overload check (UpdateDSPchannelAll, the only caller of BreakLowerActiveDSPchannel, PAL 801A4980):
// every DSP subframe update takes OSGetTime; when the time since the previous one exceeds 1/1.1 of the gap before
// the frame's first update, it decides the DSP cannot keep up and force-stops the lowest active voice, whatever
// its priority. The host mixer never falls behind; the check fires when the guest audio thread was held up between
// two subframes (another guest thread holding the CPU, the AFC streamer's start-up work), and it stopped the
// streamed music. The LM PC port skips the stop since its 1.1.5 (docs/AUDIO.md "Underruns, the DSP overload
// check, peaks"); so does this port. Each skip is logged with the subframe count (first 10, then every 100th).
HLE_FUNC(BreakLowerActiveDSPchannel__FUc) {
    static u32 s_count = 0;
    if (++s_count <= 10 || s_count % 100 == 0)
        lm_log("[audio] JAudio's DSP overload check would stop a voice (limit %u, subframe %u, %u so far): skipped",
               c.r[3], s_rendered, s_count);
    c.r[3] = 0;
}
