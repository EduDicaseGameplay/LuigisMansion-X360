#!/usr/bin/env python3
"""Static recompiler: GameCube main.dol (Gekko PowerPC) -> C++ for the Xbox 360 (LuigisMansion-X360).

Written for this port. Design informed by the PC project (luigis-mansion-recompiled, docs/RECOMPILER.md):
one C++ function per guest function, in-function branches as goto, `bl` as a direct call, indirect
calls through an address table, jump tables as switch. The output targets the XDK's VS2010 compiler
(C++03 with a few C++0x features) and also builds with clang on a PC for host tests.

Usage:
  ppc_recomp.py --dol main.dol --symbols symbols.txt --names GLMP01_names.txt
                --hooks hooks.txt --out <dir> [--per-file 300] [--only 8000xxxx,...]

Inputs come from the user's own disc and from the lm-decomp configs (CC0). The generated code
is derived from the game: it stays in Builds/ and must never be committed or shared.
"""
import argparse
import re
import os
import struct
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dol import Dol, load_symbols  # noqa: E402
import ppc_localize  # noqa: E402  (LM360 0.3.9: register localization with calls, and its checker)


def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


def mask32(mb, me):
    m = 0
    i = mb
    while True:
        m |= 1 << (31 - i)
        if i == me:
            break
        i = (i + 1) & 31
    return m


class Unsupported(Exception):
    pass


class FuncGen:
    """Translates one guest function (or a fragment that starts inside one)."""

    def __init__(self, rc, start, end, name):
        self.rc, self.start, self.end, self.name = rc, start, end, name
        self.labels = set()
        self.lines = []
        self.uses_switch = False

    # ---- helpers -------------------------------------------------------
    def ea_d(self, ra, d):
        return "(u32)%d" % d if ra == 0 else "(c.r[%d] + (u32)%d)" % (ra, d)

    def ea_x(self, ra, rb):
        return "c.r[%d]" % rb if ra == 0 else "(c.r[%d] + c.r[%d])" % (ra, rb)

    def callee(self, target):
        """C++ expression calling a guest address from this function (direct when known)."""
        return self.rc.call_expr(target)

    def inside(self, t):
        return self.start <= t < self.end

    def cond(self, bo, bi):
        parts = []
        if not (bo & 4):
            parts.append("(--c.ctr %s 0)" % ("== " if bo & 2 else "!="))
        if not (bo & 16):
            bit = "c.crbit(%d)" % bi
            parts.append(bit if bo & 8 else "!" + bit)
        return " && ".join(parts) if parts else None

    # ---- main ----------------------------------------------------------
    def scan_labels(self):
        rc = self.rc
        a = self.start
        while a < self.end:
            w = rc.dol.word(a)
            op = w >> 26
            if op == 18 and not (w & 1):
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                t = (li if w & 2 else a + li) & 0xFFFFFFFF
                if self.inside(t):
                    self.labels.add(t)
            elif op == 16 and not (w & 1):
                bd = sx16(w & 0xFFFC)
                t = (bd if w & 2 else a + bd) & 0xFFFFFFFF
                if self.inside(t):
                    self.labels.add(t)
            elif op == 19 and ((w >> 1) & 0x3FF) == 528 and not (w & 1):
                for t in rc.jump_targets_in(self.start, self.end):
                    self.labels.add(t)
                self.uses_switch = True
            a += 4

    def emit(self):
        self.scan_labels()
        out = self.lines
        a = self.start
        while a < self.end:
            if a in self.labels or a == self.start:
                out.append("L_%08X:" % a)
            w = self.rc.dol.word(a)
            try:
                code = self.insn(a, w)
            except Unsupported as e:
                code = "c.unimpl(0x%08X, 0x%08X); /* %s */" % (a, w, e)
                self.rc.unsupported[str(e)] += 1
            out.append("\t" + code)
            a += 4
        # Falling off the end continues into the next function (rare; e.g. split symbols).
        last = self.rc.dol.word(self.end - 4)
        lop = last >> 26
        ends = (lop == 18 and not (last & 1)) or (lop == 19 and ((last >> 1) & 0x3FF) in (16, 528)
                                                    and ((last >> 21) & 20) == 20 and not (last & 1))
        if not ends and self.rc.dol.is_text(self.end):
            out.append("\t%s; return;" % self.callee(self.end))
        return out

    # ---- instruction translation ----------------------------------------
    def insn(self, a, w):
        op = w >> 26
        rd = (w >> 21) & 31
        ra = (w >> 16) & 31
        rb = (w >> 11) & 31
        rc_ = w & 1
        imm = w & 0xFFFF
        simm = sx16(imm)
        R = lambda n: "c.r[%d]" % n
        F = lambda n: "c.f[%d]" % n
        cr0 = (lambda e: " c.setCr0(%s);" % e) if rc_ else (lambda e: "")

        if op == 18:  # b / bl
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            t = (li if w & 2 else a + li) & 0xFFFFFFFF
            if w & 1:
                return "c.lr = 0x%08X; %s;" % (a + 4, self.callee(t))
            if self.inside(t) and t != self.start:
                return ("IRQ_CHECK(c); " if t <= a else "") + "goto L_%08X;" % t
            if t == self.start:
                return "IRQ_CHECK(c); goto L_%08X;" % t
            return "%s; return;" % self.callee(t)
        if op == 16:  # bc
            bo, bi = rd, ra
            bd = sx16(w & 0xFFFC)
            t = (bd if w & 2 else a + bd) & 0xFFFFFFFF
            cnd = self.cond(bo, bi)
            if w & 1:
                body = "{ c.lr = 0x%08X; %s; }" % (a + 4, self.callee(t))
            elif self.inside(t):
                body = ("{ IRQ_CHECK(c); goto L_%08X; }" if t <= a else "goto L_%08X;") % t
            else:
                body = "{ %s; return; }" % self.callee(t)
            return "if (%s) %s" % (cnd, body) if cnd else body
        if op == 19:
            xo = (w >> 1) & 0x3FF
            if xo == 16:  # bclr
                cnd = self.cond(rd, ra)
                if w & 1:
                    body = "{ u32 t_ = c.lr; c.lr = 0x%08X; c.call(t_); }" % (a + 4)
                else:
                    body = "return;"
                return ("if (%s) %s" % (cnd, body)) if cnd else body
            if xo == 528:  # bcctr
                cnd = self.cond(rd | 4, ra)  # BO bit 2 is always set for bcctr
                if w & 1:
                    body = "{ c.lr = 0x%08X; c.call(c.ctr); }" % (a + 4)
                else:
                    targets = sorted(self.rc.jump_targets_in(self.start, self.end))
                    if targets:
                        cases = " ".join("case 0x%08X: goto L_%08X;" % (t, t) for t in targets)
                        body = "{ switch (c.ctr) { %s default: c.call(c.ctr); return; } }" % cases
                    else:
                        body = "{ c.call(c.ctr); return; }"
                return ("if (%s) %s" % (cnd, body)) if cnd else body
            if xo == 0:
                return "c.cr[%d] = c.cr[%d];" % (rd >> 2, ra >> 2)
            crops = {257: "{A} & {B}", 129: "{A} & !{B}", 289: "{A} == {B}", 225: "!({A} & {B})",
                     33: "!({A} | {B})", 449: "{A} | {B}", 417: "{A} | !{B}", 193: "{A} != {B}"}
            if xo in crops:
                e = crops[xo].format(A="c.crbit(%d)" % ra, B="c.crbit(%d)" % rb)
                return "c.setCrbit(%d, (%s) != 0);" % (rd, e)
            if xo in (150,):
                return "/* isync */"
            if xo == 50:
                return "c.rfi(0x%08X);" % a
            raise Unsupported("op19 xo %d" % xo)
        if op == 7:
            return "%s = (u32)((s32)%s * %d);" % (R(rd), R(ra), simm)
        if op == 8:
            return "{ u32 a_ = %s; u32 i_ = (u32)%d; %s = i_ - a_; c.ca = i_ >= a_; }" % (R(ra), simm, R(rd))
        if op == 10:
            return "c.cmpu(%d, %s, 0x%Xu);" % (rd >> 2, R(ra), imm)
        if op == 11:
            return "c.cmps(%d, (s32)%s, %d);" % (rd >> 2, R(ra), simm)
        if op in (12, 13):
            s = "{ u32 a_ = %s; u32 d_ = a_ + (u32)%d; c.ca = d_ < a_; %s = d_;" % (R(ra), simm, R(rd))
            if op == 13:
                s += " c.setCr0(d_);"
            return s + " }"
        if op == 14:
            return "%s = %s;" % (R(rd), "(u32)%d" % simm if ra == 0 else "%s + (u32)%d" % (R(ra), simm))
        if op == 15:
            v = (imm << 16) & 0xFFFFFFFF
            return "%s = %s;" % (R(rd), "0x%08Xu" % v if ra == 0 else "%s + 0x%08Xu" % (R(ra), v))
        if op == 17:
            return "c.syscall(0x%08X);" % a
        if op in (20, 21, 23):
            sh, mb, me = rb, (w >> 6) & 31, (w >> 1) & 31
            m = mask32(mb, me)
            rot = "rotl32(%s, %s)" % (R(rd), ("%d" % sh) if op != 23 else "(%s & 31)" % R(rb))
            if op == 21 and sh == 0:
                rot = R(rd)
            if op == 20:
                return "%s = (%s & 0x%08Xu) | (%s & 0x%08Xu);%s" % (R(ra), rot, m, R(ra), (~m) & 0xFFFFFFFF, cr0(R(ra)))
            return "%s = %s & 0x%08Xu;%s" % (R(ra), rot, m, cr0(R(ra)))
        if op in (24, 25, 26, 27, 28, 29):
            v = imm if op in (24, 26, 28) else (imm << 16)
            o = {24: "|", 25: "|", 26: "^", 27: "^", 28: "&", 29: "&"}[op]
            if op in (24,) and rd == 0 and ra == 0 and imm == 0:
                return "/* nop */"
            s = "%s = %s %s 0x%08Xu;" % (R(ra), R(rd), o, v)
            if op in (28, 29):
                s += " c.setCr0(%s);" % R(ra)
            return s
        if op == 31:
            return self.op31(a, w, rd, ra, rb, rc_)
        if 32 <= op <= 55:
            return self.loadstore(op, rd, ra, simm)
        if op in (56, 57, 60, 61):  # psq_l / psq_lu / psq_st / psq_stu
            d = w & 0xFFF
            if d & 0x800:
                d -= 0x1000
            wbit, i = (w >> 15) & 1, (w >> 12) & 7
            ea = self.ea_d(ra, d) if op in (56, 60) else "(c.r[%d] + (u32)%d)" % (ra, d)
            s = ("c.psq_l(%d, %s, %d, %d);" if op in (56, 57) else "c.psq_st(%d, %s, %d, %d);") % (rd, ea, wbit, i)
            if op in (57, 61):
                s = "{ u32 ea_ = %s; %s c.r[%d] = ea_; }" % (ea, s.replace(ea, "ea_"), ra)
            return s
        if op == 4:
            return self.op4(a, w, rd, ra, rb, rc_)
        if op == 59:
            return self.op59(w, rd, ra, rb, rc_)
        if op == 63:
            return self.op63(a, w, rd, ra, rb, rc_)
        if op == 3:
            return "/* twi */"
        if op == 0 and w == 0:
            return "c.unimpl(0x%08X, 0); /* zero word */" % a
        raise Unsupported("op %d" % op)

    def loadstore(self, op, rd, ra, simm):
        upd = op in (33, 35, 37, 39, 41, 43, 45, 49, 51, 53, 55)
        ea = "(c.r[%d] + (u32)%d)" % (ra, simm) if upd else self.ea_d(ra, simm)
        tbl = {
            32: "c.r[%d] = rd32(EA);", 33: "c.r[%d] = rd32(EA);",
            34: "c.r[%d] = rd8(EA);", 35: "c.r[%d] = rd8(EA);",
            36: "wr32(EA, c.r[%d]);", 37: "wr32(EA, c.r[%d]);",
            38: "wr8(EA, c.r[%d]);", 39: "wr8(EA, c.r[%d]);",
            40: "c.r[%d] = rd16(EA);", 41: "c.r[%d] = rd16(EA);",
            42: "c.r[%d] = (u32)(s32)(s16)rd16(EA);", 43: "c.r[%d] = (u32)(s32)(s16)rd16(EA);",
            44: "wr16(EA, c.r[%d]);", 45: "wr16(EA, c.r[%d]);",
            48: "c.f[%d] = (double)rdf32(EA);", 49: "c.f[%d] = (double)rdf32(EA);",
            50: "c.f[%d] = rdf64(EA);", 51: "c.f[%d] = rdf64(EA);",
            52: "wrf32(EA, (float)c.f[%d]);", 53: "wrf32(EA, (float)c.f[%d]);",
            54: "wrf64(EA, c.f[%d]);", 55: "wrf64(EA, c.f[%d]);",
        }
        if op == 46:
            return "{ u32 ea_ = %s; for (int i_ = %d; i_ < 32; i_++, ea_ += 4) c.r[i_] = rd32(ea_); }" % (ea, rd)
        if op == 47:
            return "{ u32 ea_ = %s; for (int i_ = %d; i_ < 32; i_++, ea_ += 4) wr32(ea_, c.r[i_]); }" % (ea, rd)
        if op in (48, 49):
            # lfs also fills ps1 on the Gekko (single loaded into both halves).
            body = "c.f[%d] = c.ps1[%d] = (double)rdf32(EA);" % (rd, rd)
        else:
            body = tbl[op] % rd
        if upd:
            return "{ u32 ea_ = %s; %s c.r[%d] = ea_; }" % (ea, body.replace("EA", "ea_"), ra)
        return body.replace("EA", ea)

    def op31(self, a, w, rd, ra, rb, rc_):
        xo = (w >> 1) & 0x3FF
        R = lambda n: "c.r[%d]" % n
        cr0 = (lambda e: " c.setCr0(%s);" % e) if rc_ else (lambda e: "")
        oe = (w >> 10) & 1
        xo9 = xo & 0x1FF
        arith = {266: "add", 40: "subf", 10: "addc", 8: "subfc", 138: "adde", 136: "subfe", 202: "addze",
                 200: "subfze", 234: "addme", 232: "subfme", 104: "neg", 235: "mullw", 75: "mulhw",
                 11: "mulhwu", 491: "divw", 459: "divwu"}
        if xo9 in arith and (xo9 == xo or oe):
            n = arith[xo9]
            A, B, D = R(ra), R(rb), R(rd)
            if n == "add":
                s = "%s = %s + %s;" % (D, A, B)
            elif n == "subf":
                s = "%s = %s - %s;" % (D, B, A)
            elif n == "addc":
                s = "{ u32 a_ = %s; u32 d_ = a_ + %s; c.ca = d_ < a_; %s = d_; }" % (A, B, D)
            elif n == "subfc":
                s = "{ u32 a_ = %s, b_ = %s; c.ca = b_ >= a_; %s = b_ - a_; }" % (A, B, D)
            elif n == "adde":
                s = "{ u64 t_ = (u64)%s + %s + c.ca; c.ca = (u32)(t_ >> 32); %s = (u32)t_; }" % (A, B, D)
            elif n == "subfe":
                s = "{ u64 t_ = (u64)(u32)~%s + %s + c.ca; c.ca = (u32)(t_ >> 32); %s = (u32)t_; }" % (A, B, D)
            elif n == "addze":
                s = "{ u64 t_ = (u64)%s + c.ca; c.ca = (u32)(t_ >> 32); %s = (u32)t_; }" % (A, D)
            elif n == "subfze":
                s = "{ u64 t_ = (u64)(u32)~%s + c.ca; c.ca = (u32)(t_ >> 32); %s = (u32)t_; }" % (A, D)
            elif n == "addme":
                s = "{ u64 t_ = (u64)%s + c.ca + 0xFFFFFFFFull; c.ca = (u32)(t_ >> 32); %s = (u32)t_; }" % (A, D)
            elif n == "subfme":
                s = "{ u64 t_ = (u64)(u32)~%s + c.ca + 0xFFFFFFFFull; c.ca = (u32)(t_ >> 32); %s = (u32)t_; }" % (A, D)
            elif n == "neg":
                s = "%s = 0u - %s;" % (D, A)
            elif n == "mullw":
                s = "%s = (u32)((s32)%s * (s32)%s);" % (D, A, B)
            elif n == "mulhw":
                s = "%s = (u32)(((s64)(s32)%s * (s64)(s32)%s) >> 32);" % (D, A, B)
            elif n == "mulhwu":
                s = "%s = (u32)(((u64)%s * (u64)%s) >> 32);" % (D, A, B)
            elif n == "divw":
                s = "%s = divw(%s, %s);" % (D, A, B)
            elif n == "divwu":
                s = "%s = divwu(%s, %s);" % (D, A, B)
            if oe:
                s += " /* OE ignored */"
            return s + cr0(D)
        if xo == 0:
            return "c.cmps(%d, (s32)%s, (s32)%s);" % (rd >> 2, R(ra), R(rb))
        if xo == 32:
            return "c.cmpu(%d, %s, %s);" % (rd >> 2, R(ra), R(rb))
        logic = {28: "%s & %s", 60: "%s & ~%s", 124: "~(%s | %s)", 284: "~(%s ^ %s)", 316: "%s ^ %s",
                 412: "%s | ~%s", 444: "%s | %s", 476: "~(%s & %s)"}
        if xo in logic:
            if xo == 444 and rd == rb:
                return "%s = %s;%s" % (R(ra), R(rd), cr0(R(ra)))
            return "%s = %s;%s" % (R(ra), logic[xo] % (R(rd), R(rb)), cr0(R(ra)))
        if xo == 24:
            return "{ u32 s_ = %s & 63; %s = s_ & 32 ? 0u : %s << s_; }%s" % (R(rb), R(ra), R(rd), cr0(R(ra)))
        if xo == 536:
            return "{ u32 s_ = %s & 63; %s = s_ & 32 ? 0u : %s >> s_; }%s" % (R(rb), R(ra), R(rd), cr0(R(ra)))
        if xo == 792:
            return "%s = sraw(c, %s, %s);%s" % (R(ra), R(rd), R(rb), cr0(R(ra)))
        if xo == 824:
            return "%s = srawi(c, %s, %d);%s" % (R(ra), R(rd), rb, cr0(R(ra)))
        if xo == 26:
            return "%s = cntlzw(%s);%s" % (R(ra), R(rd), cr0(R(ra)))
        if xo == 922:
            return "%s = (u32)(s32)(s16)%s;%s" % (R(ra), R(rd), cr0(R(ra)))
        if xo == 954:
            return "%s = (u32)(s32)(s8)%s;%s" % (R(ra), R(rd), cr0(R(ra)))
        ea = self.ea_x(ra, rb)
        ldst = {23: "c.r[%d] = rd32(EA);", 87: "c.r[%d] = rd8(EA);", 279: "c.r[%d] = rd16(EA);",
                343: "c.r[%d] = (u32)(s32)(s16)rd16(EA);", 151: "wr32(EA, c.r[%d]);", 215: "wr8(EA, c.r[%d]);",
                407: "wr16(EA, c.r[%d]);", 534: "c.r[%d] = bswap32(rd32(EA));", 790: "c.r[%d] = bswap16(rd16(EA));",
                662: "wr32(EA, bswap32(c.r[%d]));", 918: "wr16(EA, bswap16(c.r[%d]));",
                535: "c.f[%d] = c.ps1[%d] = (double)rdf32(EA);", 599: "c.f[%d] = rdf64(EA);",
                663: "wrf32(EA, (float)c.f[%d]);", 727: "wrf64(EA, c.f[%d]);",
                983: "wr32(EA, fbits_lo(c.f[%d]));", 20: "c.r[%d] = rd32(EA);"}
        upd = {55: 23, 119: 87, 311: 279, 375: 343, 183: 151, 247: 215, 439: 407, 567: 535, 631: 599,
               695: 663, 759: 727}
        if xo in ldst:
            body = ldst[xo]
            body = body % ((rd, rd) if body.count("%d") == 2 else rd)
            return body.replace("EA", ea)
        if xo in upd:
            body = ldst[upd[xo]]
            body = body % ((rd, rd) if body.count("%d") == 2 else rd)
            return "{ u32 ea_ = c.r[%d] + c.r[%d]; %s c.r[%d] = ea_; }" % (ra, rb, body.replace("EA", "ea_"), ra)
        if xo == 150:  # stwcx.
            return "wr32(%s, c.r[%d]); c.cr[0] = (u8)(2 | c.so);" % (ea, rd)
        if xo == 1014:
            return "dcbz(%s);" % ea
        if xo in (54, 86, 246, 278, 470, 982, 598, 854, 566):
            return "/* cache/sync op %d */" % xo
        if xo == 19:
            return "c.r[%d] = c.getCr();" % rd
        if xo == 144:
            crm = (w >> 12) & 0xFF
            return "c.mtcrf(0x%02X, c.r[%d]);" % (crm, rd)
        if xo == 512:
            return "c.cr[%d] = (u8)((c.so << 3) | (c.ov << 2) | (c.ca << 1)); c.so = c.ov = c.ca = 0;" % (rd >> 2)
        if xo == 83:
            return "c.r[%d] = c.msr;" % rd
        if xo == 146:
            return "c.mtmsr(c.r[%d]);" % rd
        if xo in (339, 467, 371):
            spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
            if xo == 371:
                return "c.r[%d] = c.mftb(%d);" % (rd, spr)
            names = {8: "c.lr", 9: "c.ctr"}
            if spr in names:
                return ("c.r[%d] = %s;" % (rd, names[spr])) if xo == 339 else ("%s = c.r[%d];" % (names[spr], rd))
            if spr == 1:
                return ("c.r[%d] = c.getXer();" % rd) if xo == 339 else ("c.setXer(c.r[%d]);" % rd)
            if 912 <= spr <= 919:
                g = spr - 912
                return ("c.r[%d] = c.gqr[%d];" % (rd, g)) if xo == 339 else ("c.gqr[%d] = c.r[%d];" % (g, rd))
            return ("c.r[%d] = c.mfspr(%d);" % (rd, spr)) if xo == 339 else ("c.mtspr(%d, c.r[%d]);" % (spr, rd))
        if xo in (210, 242, 595, 659):
            return "/* segment register op */"
        if xo == 4:
            return "c.trap(0x%08X, %d);" % (a, rd)
        if xo == 597:  # lswi
            n = rb or 32
            return "lswi(c, %d, %s, %d);" % (rd, "0u" if ra == 0 else R(ra), n)
        if xo == 725:  # stswi
            n = rb or 32
            return "stswi(c, %d, %s, %d);" % (rd, "0u" if ra == 0 else R(ra), n)
        raise Unsupported("op31 xo %d" % xo)

    def op59(self, w, rd, ra, rb, rc_):
        xo5 = (w >> 1) & 31
        fc = (w >> 6) & 31
        F = lambda n: "c.f[%d]" % n
        ops = {
            18: "%s / %s" % (F(ra), F(rb)), 20: "%s - %s" % (F(ra), F(rb)), 21: "%s + %s" % (F(ra), F(rb)),
            25: "%s * %s" % (F(ra), F(fc)),
            29: "fmadd_(%s, %s, %s)" % (F(ra), F(fc), F(rb)), 28: "fmadd_(%s, %s, -%s)" % (F(ra), F(fc), F(rb)),
            31: "-fmadd_(%s, %s, %s)" % (F(ra), F(fc), F(rb)), 30: "-fmadd_(%s, %s, -%s)" % (F(ra), F(fc), F(rb)),
            24: "1.0 / %s" % F(rb), 22: "fsqrt_(%s)" % F(rb),
        }
        if xo5 not in ops:
            raise Unsupported("op59 xo %d" % xo5)
        s = "c.f[%d] = c.ps1[%d] = (double)(float)(%s);" % (rd, rd, ops[xo5])
        return s + (" c.setCr1();" if rc_ else "")

    def op63(self, a, w, rd, ra, rb, rc_):
        xo = (w >> 1) & 0x3FF
        xo5 = xo & 31
        fc = (w >> 6) & 31
        F = lambda n: "c.f[%d]" % n
        cr1 = " c.setCr1();" if rc_ else ""
        a5 = {18: "%s / %s" % (F(ra), F(rb)), 20: "%s - %s" % (F(ra), F(rb)), 21: "%s + %s" % (F(ra), F(rb)),
              25: "%s * %s" % (F(ra), F(fc)), 29: "fmadd_(%s, %s, %s)" % (F(ra), F(fc), F(rb)),
              28: "fmadd_(%s, %s, -%s)" % (F(ra), F(fc), F(rb)), 31: "-fmadd_(%s, %s, %s)" % (F(ra), F(fc), F(rb)),
              30: "-fmadd_(%s, %s, -%s)" % (F(ra), F(fc), F(rb)), 22: "fsqrt_(%s)" % F(rb),
              26: "frsqrte_(%s)" % F(rb), 23: "fsel_(%s, %s, %s)" % (F(ra), F(fc), F(rb))}
        if xo5 in a5 and (xo5 >= 18):
            return "c.f[%d] = %s;%s" % (rd, a5[xo5], cr1)
        if xo == 0 or xo == 32:
            return "c.fcmp(%d, %s, %s);" % (rd >> 2, F(ra), F(rb))
        if xo == 12:
            return "c.f[%d] = (double)(float)%s;%s" % (rd, F(rb), cr1)
        if xo == 14 or xo == 15:
            return "c.f[%d] = fctiw_(%s, %d);%s" % (rd, F(rb), 1 if xo == 15 else 0, cr1)
        if xo == 40:
            return "c.f[%d] = -%s;%s" % (rd, F(rb), cr1)
        if xo == 72:
            return "c.f[%d] = %s;%s" % (rd, F(rb), cr1)
        if xo == 136:
            return "c.f[%d] = -fabs_(%s);%s" % (rd, F(rb), cr1)
        if xo == 264:
            return "c.f[%d] = fabs_(%s);%s" % (rd, F(rb), cr1)
        if xo == 583:
            return "c.f[%d] = fraw((u64)c.fpscr);%s" % (rd, cr1)
        if xo == 711:
            fm = (w >> 17) & 0xFF
            return "c.mtfsf(0x%02X, fbits_lo(%s));" % (fm, F(rb))
        if xo == 70:
            return "c.fpscr &= ~(0x80000000u >> %d);" % rd
        if xo == 38:
            return "c.fpscr |= (0x80000000u >> %d);" % rd
        if xo == 134:
            return "c.mtfsfi(%d, %d);" % (rd >> 2, (w >> 12) & 15)
        if xo == 64:
            return "c.cr[%d] = (u8)((c.fpscr >> (28 - 4 * %d)) & 15);" % (rd >> 2, ra >> 2)
        raise Unsupported("op63 xo %d" % xo)

    def op4(self, a, w, rd, ra, rb, rc_):
        xo = (w >> 1) & 0x3FF
        xo5 = xo & 31
        xo6 = (w >> 1) & 63
        fc = (w >> 6) & 31
        if xo6 in (6, 7, 38, 39):  # psq_lx / psq_stx / psq_lux / psq_stux
            wbit, i = (w >> 10) & 1, (w >> 7) & 7
            st = xo6 in (7, 39)
            upd = xo6 in (38, 39)
            ea = "(c.r[%d] + c.r[%d])" % (ra, rb) if (upd or ra) else "c.r[%d]" % rb
            s = ("c.psq_st(%d, ea_, %d, %d);" if st else "c.psq_l(%d, ea_, %d, %d);") % (rd, wbit, i)
            return "{ u32 ea_ = %s; %s%s }" % (ea, s, (" c.r[%d] = ea_;" % ra) if upd else "")
        P0 = lambda n: "c.f[%d]" % n
        P1 = lambda n: "c.ps1[%d]" % n
        def two(e0, e1):
            return "{ double t0_ = (double)(float)(%s); double t1_ = (double)(float)(%s); c.f[%d] = t0_; c.ps1[%d] = t1_; }%s" % (
                e0, e1, rd, rd, " c.setCr1();" if rc_ else "")
        A, B, C = ra, rb, fc
        if xo5 == 21:
            return two("%s + %s" % (P0(A), P0(B)), "%s + %s" % (P1(A), P1(B)))
        if xo5 == 20:
            return two("%s - %s" % (P0(A), P0(B)), "%s - %s" % (P1(A), P1(B)))
        if xo5 == 25:
            return two("%s * %s" % (P0(A), P0(C)), "%s * %s" % (P1(A), P1(C)))
        if xo5 == 18:
            return two("%s / %s" % (P0(A), P0(B)), "%s / %s" % (P1(A), P1(B)))
        if xo5 == 29:
            return two("fmadd_(%s, %s, %s)" % (P0(A), P0(C), P0(B)), "fmadd_(%s, %s, %s)" % (P1(A), P1(C), P1(B)))
        if xo5 == 28:
            return two("fmadd_(%s, %s, -%s)" % (P0(A), P0(C), P0(B)), "fmadd_(%s, %s, -%s)" % (P1(A), P1(C), P1(B)))
        if xo5 == 31:
            return two("-fmadd_(%s, %s, %s)" % (P0(A), P0(C), P0(B)), "-fmadd_(%s, %s, %s)" % (P1(A), P1(C), P1(B)))
        if xo5 == 30:
            return two("-fmadd_(%s, %s, -%s)" % (P0(A), P0(C), P0(B)), "-fmadd_(%s, %s, -%s)" % (P1(A), P1(C), P1(B)))
        if xo5 == 12:
            return two("%s * %s" % (P0(A), P0(C)), "%s * %s" % (P1(A), P0(C)))
        if xo5 == 13:
            return two("%s * %s" % (P0(A), P1(C)), "%s * %s" % (P1(A), P1(C)))
        if xo5 == 14:
            return two("fmadd_(%s, %s, %s)" % (P0(A), P0(C), P0(B)), "fmadd_(%s, %s, %s)" % (P1(A), P0(C), P1(B)))
        if xo5 == 15:
            return two("fmadd_(%s, %s, %s)" % (P0(A), P1(C), P0(B)), "fmadd_(%s, %s, %s)" % (P1(A), P1(C), P1(B)))
        if xo5 == 10:
            return two("%s + %s" % (P0(A), P1(B)), P1(C))
        if xo5 == 11:
            return two(P0(C), "%s + %s" % (P0(A), P1(B)))
        if xo5 == 23:
            return two("fsel_(%s, %s, %s)" % (P0(A), P0(C), P0(B)), "fsel_(%s, %s, %s)" % (P1(A), P1(C), P1(B)))
        if xo5 == 24:
            return two("1.0 / %s" % P0(B), "1.0 / %s" % P1(B))
        if xo5 == 26:
            return two("frsqrte_(%s)" % P0(B), "frsqrte_(%s)" % P1(B))
        merges = {528: (P0(A), P0(B)), 560: (P0(A), P1(B)), 592: (P1(A), P0(B)), 624: (P1(A), P1(B))}
        if xo in merges:
            e0, e1 = merges[xo]
            return "{ double t0_ = %s; double t1_ = %s; c.f[%d] = t0_; c.ps1[%d] = t1_; }" % (e0, e1, rd, rd)
        if xo == 72:
            return "c.f[%d] = c.f[%d]; c.ps1[%d] = c.ps1[%d];" % (rd, rb, rd, rb)
        if xo == 40:
            return "c.f[%d] = -c.f[%d]; c.ps1[%d] = -c.ps1[%d];" % (rd, rb, rd, rb)
        if xo == 264:
            return "c.f[%d] = fabs_(c.f[%d]); c.ps1[%d] = fabs_(c.ps1[%d]);" % (rd, rb, rd, rb)
        if xo == 136:
            return "c.f[%d] = -fabs_(c.f[%d]); c.ps1[%d] = -fabs_(c.ps1[%d]);" % (rd, rb, rd, rb)
        if xo in (0, 32):
            return "c.fcmp(%d, c.f[%d], c.f[%d]);" % (rd >> 2, ra, rb)
        if xo in (64, 96):
            return "c.fcmp(%d, c.ps1[%d], c.ps1[%d]);" % (rd >> 2, ra, rb)
        if xo == 1014:
            return "dcbz(%s);" % self.ea_x(ra, rb)
        raise Unsupported("op4 xo %d" % xo)


class Recompiler:
    def __init__(self, args):
        self.dol = Dol(args.dol)
        self.funcs, self.objs = load_symbols(args.symbols)
        self.names = {}
        if args.names and os.path.exists(args.names):
            for line in open(args.names):
                if line.startswith("#") or not line.strip():
                    continue
                p = line.split()
                self.names[int(p[0], 16)] = p[1]
        self.funcs = {a: v for a, v in self.funcs.items() if v[1] and self.dol.is_text(a)}
        self.funcs_by_addr = sorted(self.funcs)
        self.unsupported = defaultdict(int)
        # Jump tables: every jumptable_ object; entries are code addresses.
        self.jumptables = []
        for a, (n, s, sect) in self.objs.items():
            if n.startswith("jumptable_") and s:
                ents = [self.dol.word(a + 4 * i) for i in range(s // 4)]
                self.jumptables.append((a, [e for e in ents if self.dol.is_text(e)]))
        self.jt_by_func = defaultdict(set)
        import bisect
        self._bisect = bisect
        for (_, ents) in self.jumptables:
            for e in ents:
                f = self.containing(e)
                if f is not None:
                    self.jt_by_func[f].add(e)
        self.hooks = []
        if args.hooks and os.path.exists(args.hooks):
            by_name = {}
            for a, (n, s) in self.funcs.items():
                by_name[n] = a
            for a, n in self.names.items():
                by_name.setdefault(n, a)
            for line in open(args.hooks):
                line = line.split("#")[0].strip()
                if not line:
                    continue
                parts = line.split()
                n = parts[0]
                if len(parts) > 1 and all(ch in "0123456789abcdefABCDEF" for ch in parts[1]):
                    self.hooks.append((n, int(parts[1], 16)))
                elif n in by_name:
                    self.hooks.append((n, by_name[n]))
                elif n.startswith("fn_") or n.startswith("pal_"):
                    self.hooks.append((n, int(n.split("_")[1], 16)))
                else:
                    print("hooks: %s not found" % n, file=sys.stderr)
        self.hook_index = {a: i for i, (n, a) in enumerate(self.hooks)}
        self.localize = set()
        self.localized = []
        self.localize_all = bool(getattr(args, "localize_all", False))
        self.loc_done = 0
        self.loc_fail = defaultdict(int)
        self.loc_fail_examples = []
        if getattr(args, "localize", None) and os.path.exists(args.localize):
            for line in open(args.localize):
                line = line.split("#")[0].strip()
                if line:
                    self.localize.add(int(line.split()[0], 16))
        self.extra = {}  # fragment entry address -> containing function

    def containing(self, a):
        i = self._bisect.bisect_right(self.funcs_by_addr, a) - 1
        if i >= 0:
            f = self.funcs_by_addr[i]
            if a < f + self.funcs[f][1]:
                return f
        return None

    def jump_targets_in(self, start, end):
        f = self.containing(start)
        return [t for t in self.jt_by_func.get(f, ()) if start <= t < end]

    def call_expr(self, t):
        if t in self.funcs:
            return "f_%08X(c)" % t
        f = self.containing(t)
        if f is not None:
            self.extra[t] = f
            return "f_%08X(c)" % t
        return "c.call(0x%08Xu)" % t

    def display_name(self, a):
        n = self.funcs.get(a, ("", 0))[0]
        if n.startswith("fn_") and a in self.names:
            return self.names[a]
        return n or self.names.get(a, "")

    def gen_function(self, a, end, entry_name):
        g = FuncGen(self, a, end, entry_name)
        body = g.emit()
        head = "// %s\nvoid f_%08X(Ctx& c) {\n\tLM_ENTER(0x%08Xu);\n" % (entry_name, a, a)
        if a in self.hook_index:
            head += "\tif (!g_hookBusy[%d]) { RUN_HOOK(%d, c); return; }\n" % (self.hook_index[a], self.hook_index[a])
        plain = ppc_localize.add_back(body, a)
        # LM360: registers in local variables (Tools/recomp/ppc_localize.py), each result proved by its checker;
        # localize.txt's functions also keep their plain translation, f_X_ref, for the self-test (hle_fasttest.cpp)
        if (self.localize_all or a in self.localize) and a not in self.hook_index:
            lbody, why = ppc_localize.localize(body, a)
            if lbody is not None:
                self.loc_done += 1
                if a in self.localize:
                    self.localized.append(a)
                    ref = "// %s (plain translation, for the self-test)\nvoid f_%08X_ref(Ctx& c) {\n" % (entry_name, a)
                    return head + "\n".join(lbody) + "\n}\n" + ref + "\n".join(plain) + "\n}\n"
                return head + "\n".join(lbody) + "\n}\n"
            key = "check failed" if why.startswith("check") else why
            self.loc_fail[key] += 1
            if why.startswith("check") and len(self.loc_fail_examples) < 12:
                self.loc_fail_examples.append("%08X %s" % (a, why))
            if a in self.localize:
                print("localize: %08X not eligible (%s)" % (a, why), file=sys.stderr)
        return head + "\n".join(plain) + "\n}\n"

    def address_taken(self):
        """Code addresses the game can call indirectly that are not function starts: words in data sections
        pointing into the text, and lis/addi (or lis/ori) pairs in code building a text address. Each becomes a
        fragment entry, so guest_call finds it (indirect call to 80066F64 inside a function, drv01)."""
        found = set()
        texts = [(a, a + s) for (a, o, s, t) in self.dol.sections if t]

        def in_text(v):
            return (v & 3) == 0 and any(lo <= v < hi for lo, hi in texts)

        for (sa, so, ss, is_text) in self.dol.sections:
            if is_text:
                continue
            ws = self.dol.words(sa, ss // 4)
            for w in ws:
                if in_text(w):
                    found.add(w)
        for (lo, hi) in texts:
            ws = self.dol.words(lo, (hi - lo) // 4)
            hi_reg = {}
            for i, w in enumerate(ws):
                op = w >> 26
                rd, ra = (w >> 21) & 31, (w >> 16) & 31
                if op == 15 and ra == 0:  # lis rd, imm
                    hi_reg[rd] = (w & 0xFFFF) << 16
                elif op in (14, 24) and ra in hi_reg:  # addi / ori rd, ra, imm
                    imm = w & 0xFFFF
                    v = (hi_reg[ra] + (imm - 0x10000 if (op == 14 and imm & 0x8000) else imm)) & 0xFFFFFFFF
                    if in_text(v):
                        found.add(v)
                    hi_reg.pop(ra, None)
                elif op == 18 or op == 16 or op == 19:
                    hi_reg.clear()
        added = 0
        for v in found:
            if v in self.funcs:
                continue
            f = self.containing(v)
            if f is not None:
                self.extra[v] = f
                added += 1
        print("address-taken code entries: %d" % added)

    def run(self, out, per_file, only):
        os.makedirs(out, exist_ok=True)
        if not only:
            self.address_taken()
        todo = [a for a in self.funcs_by_addr if not only or a in only]
        chunks = []
        done_extra = set()
        texts = []
        for a in todo:
            n, s = self.funcs[a]
            texts.append((a, self.gen_function(a, a + s, self.display_name(a))))
        # Fragments discovered while translating (entries in the middle of a function).
        while True:
            pending = [t for t in self.extra if t not in done_extra and t not in self.funcs]
            if not pending:
                break
            for t in pending:
                f = self.extra[t]
                done_extra.add(t)
                texts.append((t, self.gen_function(t, f + self.funcs[f][1], "fragment of %s" % self.display_name(f))))
        texts.sort()
        all_entries = [a for a, _ in texts]
        # Forward declarations header.
        with open(os.path.join(out, "lm_funcs.h"), "w") as h:
            h.write("// Generated by Tools/recomp/ppc_recomp.py. Do not edit, never commit.\n#pragma once\n#include \"ppc_ctx.h\"\n")
            for a in all_entries:
                h.write("void f_%08X(Ctx& c);\n" % a)
            for a in self.localized:
                h.write("void f_%08X_ref(Ctx& c);\n" % a)
        nfiles = 0
        for i in range(0, len(texts), per_file):
            with open(os.path.join(out, "lm_code_%03d.cpp" % nfiles), "w") as f:
                f.write("// Generated by Tools/recomp/ppc_recomp.py from the user's main.dol. Never commit.\n")
                f.write("#include \"lm_funcs.h\"\n\n")
                for a, t in texts[i:i + per_file]:
                    f.write(t)
            nfiles += 1
        with open(os.path.join(out, "lm_table.cpp"), "w") as f:
            f.write("// Generated: guest address -> host function, names, hooks.\n#include \"lm_funcs.h\"\n")
            f.write("const FuncEntry g_funcTable[] = {\n")
            for a in all_entries:
                f.write("\t{0x%08Xu, f_%08X},\n" % (a, a))
            f.write("};\nconst u32 g_funcCount = %d;\n" % len(all_entries))
            f.write("const SymEntry g_symTable[] = {\n")
            for a in self.funcs_by_addr:
                f.write("\t{\"%s\", 0x%08Xu},\n" % (self.display_name(a).replace("\\", "_"), a))
            f.write("};\nconst u32 g_symCount = %d;\n" % len(self.funcs_by_addr))
            f.write("const SymEntry g_hookTable[] = {\n")
            for n, a in self.hooks:
                f.write("\t{\"%s\", 0x%08Xu},\n" % (n, a))
            f.write("\t{0, 0}\n};\nconst u32 g_hookCount = %d;\n" % len(self.hooks))
            f.write("const LocalizedEntry g_localized[] = {\n")
            for a in self.localized:
                f.write("\t{0x%08Xu, f_%08X, f_%08X_ref},\n" % (a, a, a))
            f.write("\t{0, 0, 0}\n};\nconst u32 g_localizedCount = %d;\n" % len(self.localized))
        print("functions %d, fragments %d, files %d, hooks %d, localized %d (self-test %d)" % (len(todo), len(done_extra), nfiles, len(self.hooks), self.loc_done, len(self.localized)))
        if self.loc_fail:
            print("left plain:", dict(self.loc_fail))
            for e in self.loc_fail_examples:
                print("  " + e)
        if self.unsupported:
            print("unsupported:", dict(self.unsupported))


# LM360: register localization: Tools/recomp/ppc_localize.py (0.3.9; the leaf-only version of 0.3.7 it replaces
# is in Backups/0.3.8-before-0.3.9/ppc_recomp.py).


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dol", required=True)
    ap.add_argument("--symbols", required=True)
    ap.add_argument("--names")
    ap.add_argument("--hooks")
    ap.add_argument("--out", required=True)
    ap.add_argument("--per-file", type=int, default=300)
    ap.add_argument("--only")
    ap.add_argument("--localize")
    ap.add_argument("--localize-all", action="store_true")
    a = ap.parse_args()
    only = set(int(x, 16) for x in a.only.split(",")) if a.only else None
    Recompiler(a).run(a.out, a.per_file, only)


if __name__ == "__main__":
    main()
