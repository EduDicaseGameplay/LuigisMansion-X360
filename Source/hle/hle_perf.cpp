// Native versions of the game's hottest functions (the sampling profiler, runtime/prof.cpp). Same logic as the
// translated code, instruction by instruction; native C++ avoids the translated code's per-instruction costs on the
// Xenon (registers kept in the Ctx, condition-register bytes, interrupt checks), which make it several times slower
// than the GameCube for loops like these (knowledge base MC3-40: rewrite hot functions natively).
// LuigisMansion-X360, own code.
#include "../runtime/runtime.h"
#include "../runtime/log.h"

// Koga::ToolData::searchItemInfo(this, key) (PAL 800C01A8): the field whose name hash matches, or -1.
// Hash as the PAL code computes it: for each signed char, h = ((h << 8) + c) mod 33554393 (unsigned; the code does
// the modulo with a multiply-high by 4993 and the add fix-up, exact for every u32). Fields: 12 bytes from data+0x10,
// the hash first; the field count at data+4. Profile (Xenia, hall): ~19% of the game's code time with getValue.
static s32 tooldata_search(u32 self, u32 key) {
    const u32 data = rd32(self + 4);
    const s32 n = (s32)rd32(data + 4);
    u32 h = 0;
    for (u32 p = key;; p++) {
        const s32 ch = (s32)(s8)rd8(p);
        if (!ch) break;
        h = (h << 8) + (u32)ch;
        h %= 33554393u;
    }
    for (s32 i = 0; i < n; i++)
        if (rd32(data + 16 + (u32)i * 12) == h) return i;
    return -1;
}
HLE_FUNC(searchItemInfo__Q24Koga8ToolDataCFPCc) { c.r[3] = (u32)tooldata_search(c.r[3], c.r[4]); }

// Koga::ToolData::getValue(this, entry, key, u32* out) (PAL 800BFED4): the field's word of that entry, masked and
// shifted (a shift of 32 or more gives 0, as srw does); returns 1, or 0 when the field does not exist. Its callers
// were 5.8% of the game's time on the console (0.2.4 profile).
HLE_FUNC(getValue__Q24Koga8ToolDataCFiPCcPUl) {
    const u32 self = c.r[3], entry = c.r[4], out = c.r[6];
    const s32 idx = tooldata_search(self, c.r[5]);
    if (idx < 0) { c.r[3] = 0; return; }
    const u32 data = rd32(self + 4);
    const u32 field = data + 16 + (u32)idx * 12;
    const u32 word = rd32(data + rd32(data + 8) + (u32)((s32)entry * (s32)rd32(data + 12)) + rd16(field + 8));
    const u32 sh = rd8(field + 10) & 63;
    wr32(out, sh & 32 ? 0u : (word & rd32(field + 4)) >> sh);
    c.r[3] = 1;
}

// The object-list search at PAL 8009AEA4 (no name in the matched symbols): gets the enemy manager's list
// (Koga::EnManager::fn_800E98E0, PAL 800ECD0C, writes begin/end into an 8-byte struct) and walks its 16-byte entries:
// the first of type 2 (+4) whose object (+0) has `obj` at +0x808 (unkEnManager1::fn_800E9C5C, PAL 800ED088);
// returns that entry's object, or 0. Called for many objects every frame: ~19% of the game's code time (Xenia, hall).
HLE_FUNC(fn_8009AEA4) {
    const u32 obj = c.r[3], sp = c.r[1], lr = c.lr;
    c.r[1] = sp - 88;  // the original's frame, back chain included: the list struct goes at +52
    wr32(c.r[1], sp);
    c.r[3] = c.r[1] + 52;
    c.lr = 0x8009AEC8u;
    guest_call(c, 0x800ECD0Cu);
    const u32 begin = rd32(c.r[1] + 52), end = rd32(c.r[1] + 56);
    c.r[1] = sp;
    c.lr = lr;
    u32 p = begin;
    for (; p != end; p += 16) {
        if (rd32(p + 4) != 2) continue;
        const u32 e = rd32(p);
        if ((e ? rd32(e + 2056) : 0u) == obj) break;
    }
    c.r[3] = p != end ? rd32(p) : 0u;
}

// JKRArchive::CArcName::store(name) / store(name, endChar) (PAL 801D72EC / 801D738C): the archive path lookups
// (getResource by name) lower-case every name with MSL tolower, ~4% of the game's code time after the two above.
// Same logic as the Sunshine decomp's JKRArchivePri.cpp (knowledge base, sms-xenon-languages) and the PAL code:
// u16 hash @0 = tolower(c) + hash * 3, u16 length @2, up to 32 characters from +4 then a 0. MSL tolower: -1 stays -1,
// else the byte from its table at 8021CAA0 (indexed by c & 0xFF).
static u32 lm_tolower(s32 ch) { return ch == -1 ? 0xFFFFFFFFu : rd8(0x8021CAA0u + ((u32)ch & 0xFF)); }
static u32 arcname_store(u32 self, u32 name, bool stopAt, s32 endChar) {
    u32 hash = 0, len = 0;
    for (;; name++) {
        const s32 ch = (s32)(s8)rd8(name);
        if (!ch || (stopAt && ch == endChar)) break;
        const u32 lc = lm_tolower(ch);
        hash = (lc + hash * 3) & 0xFFFF;
        if ((s32)len < 32) { wr8(self + 4 + len, (u32)(s32)(s8)lc); len++; }
    }
    wr16(self, hash);
    wr16(self + 2, len);
    wr8(self + 4 + len, 0);
    return name;
}
HLE_FUNC(store__Q210JKRArchive8CArcNameFPCc) { arcname_store(c.r[3], c.r[4], false, 0); }
HLE_FUNC(store__Q210JKRArchive8CArcNameFPCcc) {
    const u32 end = arcname_store(c.r[3], c.r[4], true, (s32)(s8)c.r[5]);
    c.r[3] = rd8(end) ? end + 1 : 0u;
}

// JKRArchive::findTypeResource(this, type, name) (PAL 801D6FD4): the archive's directory node of that type (16-byte
// nodes; the count at info+0), then its 20-byte file entries: the first whose u16 hash (+2) equals the lower-cased
// name's and whose string (string table + (word@+4 & 0xFFFFFF)) equals it. Same as the Sunshine decomp's
// JKRArchivePri.cpp and the PAL code (isSameName: hash, then strcmp == 0). 4.8% of the console's game time (0.2.4).
HLE_FUNC(findTypeResource__10JKRArchiveCFUlPCc) {
    const u32 self = c.r[3], type = c.r[4], name = c.r[5];
    c.r[3] = 0;
    if (!type) return;
    char str[33];
    u32 hash = 0, len = 0;
    for (u32 p = name;; p++) {
        const s32 ch = (s32)(s8)rd8(p);
        if (!ch) break;
        const u32 lc = lm_tolower(ch);
        hash = (lc + hash * 3) & 0xFFFF;
        if ((s32)len < 32) str[len++] = (char)(s8)lc;
    }
    str[len] = 0;
    const u32 info = rd32(self + 68), dirs = rd32(self + 72);
    const u32 n = rd32(info);
    u32 node = 0;
    for (u32 i = 0; i < n; i++)
        if (rd32(dirs + i * 16) == type) { node = dirs + i * 16; break; }
    if (!node) return;
    const u32 strtab = rd32(self + 80);
    const s32 num = (s32)rd16(node + 10);
    u32 e = rd32(self + 76) + rd32(node + 12) * 20;
    for (s32 i = 0; i < num; i++, e += 20) {
        if (rd16(e + 2) != hash) continue;
        u32 q = strtab + (rd32(e + 4) & 0xFFFFFF);
        u32 k = 0;
        for (;; k++) {
            const u32 a = rd8(q + k), b = (u8)str[k];
            if (a != b) break;
            if (!a) { c.r[3] = e; return; }
        }
    }
}
