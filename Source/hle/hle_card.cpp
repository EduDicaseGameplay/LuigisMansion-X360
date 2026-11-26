// HLE of the GameCube memory card: slot A is a folder of Dolphin-compatible .gci files next to default.xex
// (game:\saves\card_a). LuigisMansion-X360, own code. Semantics follow the LM PC port's hle_card.cpp (results, async
// completion through an interrupt, file numbers, CARDStat marshalling); the card itself (Aurora's host layer there)
// is written here: the GCI layout is Dolphin's (a 0x40-byte DEntry, then the file's 8 KiB blocks), the icon/banner
// offsets are the SDK's __CARDUpdateIconOffsets. A save made in Dolphin can be copied into the folder and back.
//
// Guest structures (card.h): CARDFileInfo chan @0, fileNo @4, offset @8, length @C, iBlock u16 @10.
// CARDStat (0x6C): fileName[32] @0, length @20, time @24, gameName[4] @28, company[2] @2C, bannerFormat u8 @2E,
// iconAddr @30, iconFormat u16 @34, iconSpeed u16 @36, commentAddr @38, offsetBanner @3C, offsetBannerTlut @40,
// offsetIcon[8] @44, offsetIconTlut @64, offsetData @68.
// DEntry (0x40): gameName[4] @0, company[2] @4, 0xFF @6, bannerFormat @7, fileName[32] @8, time @28, iconAddr @2C,
// iconFormat u16 @30, iconSpeed u16 @32, permission @34, copyTimes @35, startBlock u16 @36, length (blocks) u16 @38,
// 0xFFFF @3A, commentAddr @3C.
#include "hle_thread.h"
#include "../runtime/log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void guest_call_args(Ctx& c, u32 fn, u32 a0, u32 a1);

namespace {

enum {
    CARD_READY = 0, CARD_BUSY = -1, CARD_WRONGDEVICE = -2, CARD_NOCARD = -3, CARD_NOFILE = -4, CARD_IOERROR = -5,
    CARD_BROKEN = -6, CARD_EXIST = -7, CARD_NOENT = -8, CARD_INSSPACE = -9, CARD_NOPERM = -10, CARD_LIMIT = -11,
    CARD_NAMETOOLONG = -12, CARD_ENCODING = -13, CARD_CANCELED = -14, CARD_FATAL = -128
};
const u32 kSector = 0x2000;          // 8 KiB blocks
const u32 kMemSizeMbit = 16;          // a 251-block card (2 MiB)
const u32 kUserBlocks = (kMemSizeMbit * 1024 * 1024 / 8) / kSector - 5;
const int kMaxFiles = 127;

struct File {
    bool used;
    u8 dentry[0x40];
    u8* data;  // blocks * kSector bytes
    char path[64];
};
File s_files[kMaxFiles];
bool s_mounted = false, s_loaded = false;
const char* kDir = "game:\\saves\\card_a";

u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
u32 be16(const u8* p) { return (u32)p[0] << 8 | p[1]; }
void put32(u8* p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
void put16(u8* p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
u32 blocksOf(const File& f) { return be16(f.dentry + 0x38); }

// Seconds since 2000-01-01 (the card's time base).
u32 card_time() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    const unsigned __int64 t = ((unsigned __int64)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10000000ull;
    const unsigned __int64 y2000 = 12591158400ull;  // seconds from 1601-01-01 to 2000-01-01
    return t > y2000 ? (u32)(t - y2000) : 0;
}

void file_path(const u8* dentry, char* out, int size) {
    char name[33];
    memcpy(name, dentry + 8, 32);
    name[32] = 0;
    int n = (int)strlen(name);
    if (n > 30) n = 30;  // FATX: 42 characters at most for "01-GLMP-" + name + ".gci"
    for (int i = 0; i < n; i++) {
        const char ch = name[i];
        if (ch < 32 || strchr("\\/:*?\"<>|+,;=[]", ch)) name[i] = '_';
    }
    name[n] = 0;
    _snprintf(out, size, "%s\\%.2s-%.4s-%s.gci", kDir, (const char*)dentry + 4, (const char*)dentry, name);
    out[size - 1] = 0;
}

bool save_file(File& f) {
    if (!f.path[0]) file_path(f.dentry, f.path, sizeof(f.path));
    FILE* h = fopen(f.path, "wb");
    if (!h) { lm_log("[card] cannot write %s", f.path); return false; }
    const bool ok = fwrite(f.dentry, 1, 0x40, h) == 0x40 && fwrite(f.data, 1, blocksOf(f) * kSector, h) == blocksOf(f) * kSector;
    fclose(h);
    if (!ok) lm_log("[card] short write %s", f.path);
    return ok;
}

void load_dir() {
    if (s_loaded) return;
    s_loaded = true;
    CreateDirectoryA("game:\\saves", NULL);
    CreateDirectoryA(kDir, NULL);
    char pattern[64];
    _snprintf(pattern, sizeof(pattern), "%s\\*.gci", kDir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    int n = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (n >= kMaxFiles) break;
            File& f = s_files[n];
            _snprintf(f.path, sizeof(f.path), "%s\\%s", kDir, fd.cFileName);
            f.path[sizeof(f.path) - 1] = 0;
            FILE* fp = fopen(f.path, "rb");
            if (!fp) continue;
            bool ok = fread(f.dentry, 1, 0x40, fp) == 0x40;
            const u32 blocks = ok ? blocksOf(f) : 0;
            ok = ok && blocks > 0 && blocks <= kUserBlocks;
            if (ok) {
                f.data = (u8*)malloc(blocks * kSector);
                ok = f.data && fread(f.data, 1, blocks * kSector, fp) == blocks * kSector;
            }
            fclose(fp);
            if (!ok) { free(f.data); memset(&f, 0, sizeof(f)); lm_log("[card] skipped %s (not a GCI save)", fd.cFileName); continue; }
            f.used = true;
            lm_log("[card] slot A file %d: %.32s (%u blocks) from %s", n, (const char*)f.dentry + 8, blocks, fd.cFileName);
            n++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    lm_log("[card] slot A: %s, %d file(s)", kDir, n);
}

u32 used_blocks() {
    u32 b = 0;
    for (int i = 0; i < kMaxFiles; i++) if (s_files[i].used) b += blocksOf(s_files[i]);
    return b;
}

int find_file(u32 nameAddr) {
    char name[33];
    for (int i = 0; i < 32; i++) name[i] = (char)rd8(nameAddr + i);
    name[32] = 0;
    for (int i = 0; i < kMaxFiles; i++)
        if (s_files[i].used && !strncmp((const char*)s_files[i].dentry + 8, name, 32) &&
            !memcmp(s_files[i].dentry, hostp(0x80000000), 4))  // this game's files only (__CARDAccess)
            return i;
    return -1;
}

s32 chan_ok(u32 chan) { return chan == 0 ? (s_mounted ? CARD_READY : CARD_NOCARD) : chan == 1 ? CARD_NOCARD : CARD_FATAL; }
bool valid_file(u32 chan, u32 fileNo) { return chan == 0 && fileNo < (u32)kMaxFiles && s_files[fileNo].used; }

// The SDK's __CARDUpdateIconOffsets.
void stat_from_dentry(u32 st, const File& f) {
    const u8* d = f.dentry;
    memcpy(hostp(st), d + 8, 32);
    wr32(st + 0x20, blocksOf(f) * kSector);
    wr32(st + 0x24, be32(d + 0x28));
    memcpy(hostp(st + 0x28), d, 4);
    memcpy(hostp(st + 0x2C), d + 4, 2);
    u32 banner = d[7], iconFmt = be16(d + 0x30), speed = be16(d + 0x32), offset = be32(d + 0x2C);
    if (offset == 0xFFFFFFFFu) { banner = 0; iconFmt = 0; speed = 0; offset = 0; }
    wr8(st + 0x2E, banner);
    wr32(st + 0x30, be32(d + 0x2C));
    wr16(st + 0x34, iconFmt);
    wr16(st + 0x36, speed);
    wr32(st + 0x38, be32(d + 0x3C));
    switch (banner & 3) {
    case 1: wr32(st + 0x3C, offset); offset += 96 * 32; wr32(st + 0x40, offset); offset += 2 * 256; break;
    case 2: wr32(st + 0x3C, offset); offset += 2 * 96 * 32; wr32(st + 0x40, 0xFFFFFFFFu); break;
    default: wr32(st + 0x3C, 0xFFFFFFFFu); wr32(st + 0x40, 0xFFFFFFFFu); break;
    }
    bool tlut = false;
    for (int i = 0; i < 8; i++) {
        switch ((iconFmt >> (2 * i)) & 3) {
        case 1: wr32(st + 0x44 + 4 * i, offset); offset += 32 * 32; tlut = true; break;
        case 2: wr32(st + 0x44 + 4 * i, offset); offset += 2 * 32 * 32; break;
        default: wr32(st + 0x44 + 4 * i, 0xFFFFFFFFu); break;
        }
    }
    if (tlut) { wr32(st + 0x64, offset); offset += 2 * 256; }
    else wr32(st + 0x64, 0xFFFFFFFFu);
    wr32(st + 0x68, offset);
}

// Async completion: callback(chan, result) from an interrupt, as the card's EXI interrupt delivers it.
struct Pending { u32 cb, chan; s32 result; };
Pending s_pending[32];
u32 s_pendingNext = 0;
void complete_irq(Ctx& c, u32 slot, u32) {
    const Pending p = s_pending[slot & 31];
    guest_call_args(c, p.cb, p.chan, (u32)p.result);
}
void complete(u32 cb, u32 chan, s32 result) {
    if (!cb) return;
    const u32 slot = s_pendingNext++ & 31;
    Pending p = {cb, chan, result};
    s_pending[slot] = p;
    irq_post(complete_irq, slot, 0, 0);
}

s32 do_mount(u32 chan) {
    if (chan != 0) return chan == 1 ? CARD_NOCARD : CARD_FATAL;
    load_dir();
    s_mounted = true;
    return CARD_READY;
}

s32 do_create(u32 chan, u32 name, u32 size, u32 fi) {
    s32 r = chan_ok(chan);
    if (r != CARD_READY) return r;
    if (rd8(name) == 0) return CARD_NOFILE;
    if (find_file(name) >= 0) return CARD_EXIST;
    if (!size || size % kSector) return CARD_FATAL;
    int slot = -1;
    for (int i = 0; i < kMaxFiles && slot < 0; i++) if (!s_files[i].used) slot = i;
    if (slot < 0) return CARD_NOENT;
    const u32 blocks = size / kSector;
    if (used_blocks() + blocks > kUserBlocks) return CARD_INSSPACE;
    File& f = s_files[slot];
    memset(&f, 0, sizeof(f));
    memset(f.dentry, 0xFF, 0x40);
    memcpy(f.dentry, hostp(0x80000000), 4);       // the disk ID's game code
    memcpy(f.dentry + 4, hostp(0x80000004), 2);   // and maker code
    f.dentry[7] = 0;
    memset(f.dentry + 8, 0, 32);
    for (int i = 0; i < 32; i++) { const u8 ch = (u8)rd8(name + i); f.dentry[8 + i] = ch; if (!ch) break; }
    put32(f.dentry + 0x28, card_time());
    put32(f.dentry + 0x2C, 0xFFFFFFFFu);
    put16(f.dentry + 0x30, 0);
    put16(f.dentry + 0x32, 0);
    f.dentry[0x34] = 0x04;  // public
    f.dentry[0x35] = 0;
    put16(f.dentry + 0x36, 5 + used_blocks());
    put16(f.dentry + 0x38, blocks);
    put32(f.dentry + 0x3C, 0xFFFFFFFFu);
    f.data = (u8*)malloc(blocks * kSector);
    if (!f.data) return CARD_IOERROR;
    memset(f.data, 0, blocks * kSector);
    f.used = true;
    if (!save_file(f)) { free(f.data); memset(&f, 0, sizeof(f)); return CARD_IOERROR; }
    wr32(fi + 0x00, chan);
    wr32(fi + 0x04, (u32)slot);
    wr32(fi + 0x08, 0);
    wr32(fi + 0x0C, size);
    wr16(fi + 0x10, be16(f.dentry + 0x36));
    lm_log("[card] created %.32s: %u blocks (%s)", (const char*)f.dentry + 8, blocks, f.path);
    return CARD_READY;
}

s32 do_read(u32 fi, u32 addr, u32 len, u32 offset) {
    const u32 chan = rd32(fi), fileNo = rd32(fi + 4);
    s32 r = chan_ok(chan);
    if (r != CARD_READY) return r;
    if (!valid_file(chan, fileNo)) return CARD_NOFILE;
    if (offset % 512 || len % 512) return CARD_FATAL;
    const File& f = s_files[fileNo];
    if (offset + len > blocksOf(f) * kSector) return CARD_LIMIT;
    memcpy(hostp(addr), f.data + offset, len);
    void lm_gx_written(void* p, u32 n);
    lm_gx_written(hostp(addr), len);
    wr32(fi + 0x08, offset + len);
    return CARD_READY;
}

s32 do_write(u32 fi, u32 addr, u32 len, u32 offset) {
    const u32 chan = rd32(fi), fileNo = rd32(fi + 4);
    s32 r = chan_ok(chan);
    if (r != CARD_READY) return r;
    if (!valid_file(chan, fileNo)) return CARD_NOFILE;
    if (offset % kSector || len % kSector) return CARD_FATAL;
    File& f = s_files[fileNo];
    if (offset + len > blocksOf(f) * kSector) return CARD_LIMIT;
    memcpy(f.data + offset, hostp(addr), len);
    put32(f.dentry + 0x28, card_time());
    if (!save_file(f)) return CARD_IOERROR;
    wr32(fi + 0x08, offset + len);
    lm_log_once(0x3C000000u ^ fileNo, "[card] wrote %.32s (%u bytes at %u)", (const char*)f.dentry + 8, len, offset);
    return CARD_READY;
}

s32 do_set_status(u32 chan, u32 fileNo, u32 st) {
    s32 r = chan_ok(chan);
    if (r != CARD_READY) return r;
    if (!valid_file(chan, fileNo)) return CARD_NOFILE;
    File& f = s_files[fileNo];
    const u32 iconAddr = rd32(st + 0x30), comment = rd32(st + 0x38);
    if ((iconAddr != 0xFFFFFFFFu && iconAddr >= 512 * 8 * 1024) || (comment != 0xFFFFFFFFu && comment % kSector > kSector - 64))
        return CARD_FATAL;
    f.dentry[7] = (u8)rd8(st + 0x2E);
    put32(f.dentry + 0x2C, iconAddr);
    put16(f.dentry + 0x30, rd16(st + 0x34));
    put16(f.dentry + 0x32, rd16(st + 0x36));
    put32(f.dentry + 0x3C, comment);
    put32(f.dentry + 0x28, card_time());
    stat_from_dentry(st, f);  // the offsets, as the SDK updates them
    return save_file(f) ? CARD_READY : CARD_IOERROR;
}

s32 do_format(u32 chan) {
    s32 r = chan_ok(chan);
    if (r != CARD_READY) return r;
    for (int i = 0; i < kMaxFiles; i++) {
        if (!s_files[i].used) continue;
        if (s_files[i].path[0]) DeleteFileA(s_files[i].path);
        free(s_files[i].data);
        memset(&s_files[i], 0, sizeof(File));
    }
    lm_log("[card] slot A formatted");
    return CARD_READY;
}

// Self-test (game:\lm360_cardtest.txt next to default.xex; tests only): the guest-facing operations on a scratch file
// in slot A, then the same file reloaded from disk. Guest scratch: 0x6000 bytes at 81000000, backed up and restored
// (nothing else runs during CARDInit's host code). As the LM PC port's LM_CARD_SELFTEST, plus the reload.
void selftest() {
    FILE* flag = fopen("game:\\lm360_cardtest.txt", "rb");
    if (!flag) return;
    fclose(flag);
    const u32 base = 0x81000000u, name = base, fi = base + 0x40, st = base + 0x80, buf = base + 0x100;
    static u8 backup[0x6000];
    memcpy(backup, hostp(base), sizeof(backup));
    bool ok = true;
    int step = 0;
#define EXPECT(what, got, want) do { step++; const s32 g_ = (s32)(got), w_ = (s32)(want);         if (g_ != w_) { ok = false; lm_log("[card] selftest %d %s: %d, expected %d", step, what, g_, w_); } } while (0)
    memset(hostp(base), 0, 0x6000);
    strcpy((char*)hostp(name), "lm360 card selftest");
    EXPECT("mount", do_mount(0), CARD_READY);
    { const int old = find_file(name); if (old >= 0) { DeleteFileA(s_files[old].path); free(s_files[old].data); memset(&s_files[old], 0, sizeof(File)); } }
    Ctx dummy; memset(&dummy, 0, sizeof(dummy));
    EXPECT("create", do_create(0, name, 0x4000, fi), CARD_READY);
    for (u32 i = 0; i < 0x4000; i++) wr8(buf + i, (i * 7 + 3) & 0xFF);
    EXPECT("write 1", do_write(fi, buf, 0x2000, 0), CARD_READY);
    EXPECT("write 2", do_write(fi, buf + 0x2000, 0x2000, 0x2000), CARD_READY);
    const u32 fileNo = rd32(fi + 4);
    if (valid_file(0, fileNo)) stat_from_dentry(st, s_files[fileNo]);
    wr32(st + 0x30, 0); wr32(st + 0x38, 0x1C00); wr8(st + 0x2E, 1); wr16(st + 0x34, 5); wr16(st + 0x36, 3);
    EXPECT("set status", do_set_status(0, fileNo, st), CARD_READY);
    EXPECT("offsetData", rd32(st + 0x68), 96 * 32 + 512 + 1024 + 1024 + 512);  // C8 banner + tlut, icons 0,1 C8 + tlut
    EXPECT("create exists", do_create(0, name, 0x2000, fi + 0x20), CARD_EXIST);
    // reload everything from disk
    for (int i = 0; i < kMaxFiles; i++) { free(s_files[i].data); memset(&s_files[i], 0, sizeof(File)); }
    s_loaded = false;
    load_dir();
    const int n = find_file(name);
    EXPECT("found after reload", n >= 0, 1);
    if (n >= 0) {
        wr32(fi, 0); wr32(fi + 4, (u32)n);
        memset(hostp(buf), 0, 0x4000);
        EXPECT("read", do_read(fi, buf, 0x4000, 0), CARD_READY);
        u32 bad = 0;
        for (u32 i = 0; i < 0x4000; i++) bad += rd8(buf + i) != ((i * 7 + 3) & 0xFF);
        EXPECT("data bytes wrong", bad, 0);
        stat_from_dentry(st, s_files[n]);
        EXPECT("length", rd32(st + 0x20), 0x4000);
        EXPECT("iconAddr", rd32(st + 0x30), 0);
        EXPECT("commentAddr", rd32(st + 0x38), 0x1C00);
        EXPECT("bannerFormat", rd8(st + 0x2E), 1);
        EXPECT("iconFormat", rd16(st + 0x34), 5);
        EXPECT("iconSpeed", rd16(st + 0x36), 3);
        EXPECT("game code", memcmp(s_files[n].dentry, hostp(0x80000000), 4), 0);
        lm_log("[card] selftest file on disk: %s", s_files[n].path);
        DeleteFileA(s_files[n].path);
        free(s_files[n].data);
        memset(&s_files[n], 0, sizeof(File));
    }
#undef EXPECT
    memcpy(hostp(base), backup, sizeof(backup));
    s_mounted = false;
    lm_log("[card] selftest %s (%d checks)", ok ? "PASSED" : "FAILED", step);
}

}  // namespace

HLE_FUNC(CARDInit) { (void)c; load_dir(); selftest(); }
HLE_FUNC(CARDProbe) { c.r[3] = c.r[3] == 0 ? 1u : 0u; }
HLE_FUNC(CARDProbeEx) {  // (chan, s32* memSize, s32* sectorSize)
    const u32 chan = c.r[3];
    if (chan != 0) { c.r[3] = (u32)(chan == 1 ? CARD_NOCARD : CARD_FATAL); return; }
    if (c.r[4]) wr32(c.r[4], kMemSizeMbit);
    if (c.r[5]) wr32(c.r[5], kSector);
    c.r[3] = CARD_READY;
}
HLE_FUNC(CARDMount) { c.r[3] = (u32)do_mount(c.r[3]); }
HLE_FUNC(CARDMountAsync) {  // (chan, workArea, detachCallback, attachCallback)
    const u32 chan = c.r[3], cb = c.r[6];
    const s32 r = do_mount(chan);
    complete(cb, chan, r);
    c.r[3] = (u32)r;
}
HLE_FUNC(__CARDMountCallback) { (void)c; }
HLE_FUNC(CARDUnmount) {
    const u32 chan = c.r[3];
    const s32 r = chan_ok(chan);
    if (chan == 0) s_mounted = false;
    c.r[3] = (u32)r;
}
HLE_FUNC(CARDCheck) { c.r[3] = (u32)chan_ok(c.r[3]); }
HLE_FUNC(CARDCheckExAsync) {  // (chan, s32* xferBytes, callback)
    const s32 r = chan_ok(c.r[3]);
    if (c.r[4]) wr32(c.r[4], 0);
    complete(c.r[5], c.r[3], r);
    c.r[3] = (u32)r;
}
HLE_FUNC(CARDFormat) { c.r[3] = (u32)do_format(c.r[3]); }
HLE_FUNC(CARDFormatAsync) {
    const s32 r = do_format(c.r[3]);
    complete(c.r[4], c.r[3], r);
    c.r[3] = (u32)r;
}
HLE_FUNC(CARDFreeBlocks) {  // (chan, s32* bytesNotUsed, s32* filesNotUsed)
    const s32 r = chan_ok(c.r[3]);
    if (r == CARD_READY) {
        int files = 0;
        for (int i = 0; i < kMaxFiles; i++) files += s_files[i].used ? 0 : 1;
        if (c.r[4]) wr32(c.r[4], (kUserBlocks - used_blocks()) * kSector);
        if (c.r[5]) wr32(c.r[5], (u32)files);
    }
    c.r[3] = (u32)r;
}
HLE_FUNC(CARDCreate) { c.r[3] = (u32)do_create(c.r[3], c.r[4], c.r[5], c.r[6]); }
HLE_FUNC(CARDCreateAsync) {
    const s32 r = do_create(c.r[3], c.r[4], c.r[5], c.r[6]);
    complete(c.r[7], c.r[3], r);
    c.r[3] = (u32)r;
}
HLE_FUNC(CARDOpen) {  // (chan, name, fileInfo)
    const u32 chan = c.r[3], fi = c.r[5];
    s32 r = chan_ok(chan);
    if (r == CARD_READY) {
        const int n = find_file(c.r[4]);
        if (n < 0) {
            r = CARD_NOFILE;
            wr32(fi + 0x04, (u32)-1);
        } else {
            wr32(fi + 0x00, chan);
            wr32(fi + 0x04, (u32)n);
            wr32(fi + 0x08, 0);
            wr32(fi + 0x0C, blocksOf(s_files[n]) * kSector);
            wr16(fi + 0x10, be16(s_files[n].dentry + 0x36));
        }
    }
    c.r[3] = (u32)r;
}
HLE_FUNC(CARDClose) {
    const s32 r = chan_ok(rd32(c.r[3]));
    wr32(c.r[3], (u32)-1);
    c.r[3] = (u32)r;
}
HLE_FUNC(CARDRead) { c.r[3] = (u32)do_read(c.r[3], c.r[4], c.r[5], c.r[6]); }
HLE_FUNC(CARDReadAsync) {
    const u32 chan = rd32(c.r[3]);
    const s32 r = do_read(c.r[3], c.r[4], c.r[5], c.r[6]);
    complete(c.r[7], chan, r);
    c.r[3] = (u32)r;
}
HLE_FUNC(CARDWrite) { c.r[3] = (u32)do_write(c.r[3], c.r[4], c.r[5], c.r[6]); }
HLE_FUNC(CARDWriteAsync) {
    const u32 chan = rd32(c.r[3]);
    const s32 r = do_write(c.r[3], c.r[4], c.r[5], c.r[6]);
    complete(c.r[7], chan, r);
    c.r[3] = (u32)r;
}
HLE_FUNC(CARDGetStatus) {  // (chan, fileNo, CARDStat*)
    const u32 chan = c.r[3], fileNo = c.r[4];
    s32 r = chan_ok(chan);
    if (r == CARD_READY) {
        if (!valid_file(chan, fileNo)) r = CARD_NOFILE;
        else stat_from_dentry(c.r[5], s_files[fileNo]);
    }
    c.r[3] = (u32)r;
}
HLE_FUNC(CARDSetStatus) { c.r[3] = (u32)do_set_status(c.r[3], c.r[4], c.r[5]); }
HLE_FUNC(CARDSetStatusAsync) {
    const s32 r = do_set_status(c.r[3], c.r[4], c.r[5]);
    complete(c.r[6], c.r[3], r);
    c.r[3] = (u32)r;
}
HLE_FUNC(__CARDGetControlBlock) { c.r[3] = (u32)chan_ok(c.r[3]); }
HLE_FUNC(__CARDSync) { c.r[3] = (u32)chan_ok(c.r[3]); }
