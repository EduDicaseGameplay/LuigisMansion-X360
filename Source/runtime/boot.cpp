// Loads the user's main.dol into guest memory and fills the low-memory boot block the way the
// apploader/IPL leaves it (values from the Dolphin SDK headers: OSBootInfo and the globals at 0x80000000).
#include "runtime.h"
#include "log.h"
#include <stdio.h>

static u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

bool boot_load_dol(const char* path, u32* entry) {
    FILE* f = fopen(path, "rb");
    if (!f) { lm_log("[boot] cannot open %s", path); return false; }
    u8 hdr[0x100];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); return false; }
    // The header's BSS range spans .sbss/.sdata/.sdata2 too: clear it BEFORE loading the sections,
    // or the small-data initialisers are wiped (found in boot11: the scene table pointer read 0).
    const u32 bss = be32(hdr + 216), bssSize = be32(hdr + 220);
    memset(hostp(bss), 0, bssSize);
    for (int i = 0; i < 18; i++) {
        const u32 off = be32(hdr + i * 4), addr = be32(hdr + 72 + i * 4), size = be32(hdr + 144 + i * 4);
        if (!size) continue;
        if ((addr & 0x01FFFFFF) + size > kMem1Size) { fclose(f); lm_log("[boot] section %d out of MEM1", i); return false; }
        fseek(f, (long)off, SEEK_SET);
        if (fread(hostp(addr), 1, size, f) != size) { fclose(f); lm_log("[boot] short read in section %d", i); return false; }
        lm_log("[boot] section %2d %08X size %06X", i, addr, size);
    }
    *entry = be32(hdr + 224);
    fclose(f);
    lm_log("[boot] bss %08X size %06X, entry %08X", bss, bssSize, *entry);
    return true;
}

void boot_setup_lowmem(const char* gameId, u32 fstAddr, u32 fstSize) {
    memcpy(hostp(0x80000000), gameId, 6);       // disk ID (game code, maker)
    wr32(0x8000001C, 0xC2339F3D);               // GameCube disc magic
    wr32(0x80000020, 0x0D15EA5E);               // boot info magic: normal boot
    wr32(0x80000024, 1);                        // version
    wr32(0x80000028, kMem1Size);                // physical memory size
    wr32(0x8000002C, 0x00000003);               // console type: retail (production board)
    wr32(0x80000030, 0);                        // arena lo: 0 = use the linker's end of BSS
    wr32(0x80000034, 0x81800000 - fstSize - 0x20); // arena hi: below the FST
    wr32(0x80000038, fstAddr);                  // FST location
    wr32(0x8000003C, fstSize);                  // FST max length
    wr32(0x800000CC, 1);                        // TV mode: 1 = PAL (the European game)
    wr32(0x800000F0, kMem1Size);                // simulated memory size
    wr32(0x800000F8, kBusClock);
    wr32(0x800000FC, kCoreClock);
    wr32(0x800030D8, 0);                        // system time (OSGetTime base)
}
