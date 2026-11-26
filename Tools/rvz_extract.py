#!/usr/bin/env python3
"""Read a GameCube RVZ image (Dolphin's format) and extract its file system.

LuigisMansion-X360 tool. Works on the user's own disc images only; nothing it
writes may be committed (GameData/ is in .gitignore).

Usage:
  rvz_extract.py info    <image.rvz>
  rvz_extract.py extract <image.rvz> <out_dir>      (sys/ + files/, like Dolphin)
  rvz_extract.py sha1    <image.rvz>                (SHA-1 of the full rebuilt ISO)
  rvz_extract.py iso     <image.rvz> <out.iso>

Format notes (Dolphin Source/Core/DiscIO/WIABlob.*, RVZ = WIA v1 + zstd + packing):
  header1 0x48 bytes, header2 (chunk size, disc header, tables), raw-data entries and
  group entries (both zstd compressed). Each group covers chunk_size bytes of the ISO.
  A group is zstd-compressed if bit 31 of its size is set; if rvz_packed_size != 0 the
  (decompressed) data is a run of [u32 size][data] where size bit 31 means "junk":
  17 u32 seed words that regenerate Nintendo's lagged Fibonacci padding.
"""
import hashlib
import os
import struct
import sys

import numpy as np
import zstandard

LFG_K, LFG_J, SEED_WORDS = 521, 32, 17
BLOCK = 0x8000


class Lfg:
    """Dolphin's LaggedFibonacciGenerator (junk data of GameCube/Wii discs)."""

    def __init__(self, seed_bytes):
        buf = np.zeros(LFG_K, dtype=np.uint32)
        buf[:SEED_WORDS] = np.frombuffer(seed_bytes, dtype=">u4").astype(np.uint32)
        b = [int(x) for x in buf]
        for i in range(SEED_WORDS, LFG_K):
            b[i] = ((b[i - 17] << 23) & 0xFFFFFFFF) ^ (b[i - 16] >> 9) ^ b[i - 1]
        b = [(x & 0xFF00FFFF) | ((x >> 2) & 0x00FF0000) for x in b]
        # Kept as big-endian words: their bytes in order are the output stream.
        self.buf = np.array(b, dtype=np.uint32)
        self.pos = 0
        for _ in range(4):
            self._forward()

    def _forward(self):
        b = self.buf
        b[:LFG_J] ^= b[LFG_K - LFG_J:LFG_K]
        for i in range(LFG_J, LFG_K, LFG_J):
            e = min(i + LFG_J, LFG_K)
            b[i:e] ^= b[i - LFG_J:e - LFG_J]

    def skip(self, n):
        self.pos += n
        while self.pos >= LFG_K * 4:
            self._forward()
            self.pos -= LFG_K * 4

    def get(self, n):
        out = bytearray()
        while n:
            raw = self.buf.astype(">u4").tobytes()
            take = min(n, LFG_K * 4 - self.pos)
            out += raw[self.pos:self.pos + take]
            self.pos += take
            n -= take
            if self.pos == LFG_K * 4:
                self._forward()
                self.pos = 0
        return bytes(out)


class Rvz:
    def __init__(self, path):
        self.f = open(path, "rb")
        h1 = self.f.read(0x48)
        if h1[:4] != b"RVZ\x01":
            raise ValueError("not an RVZ file: %r" % h1[:4])
        (self.version, self.version_compat, self.h2_size) = struct.unpack(">III", h1[4:16])
        self.iso_size, self.file_size = struct.unpack(">QQ", h1[0x24:0x34])
        h2 = self.f.read(self.h2_size)
        (self.disc_type, self.compression, self.level, self.chunk) = struct.unpack(">IIiI", h2[:16])
        self.disc_header = h2[16:16 + 0x80]
        o = 16 + 0x80
        (self.n_part, self.part_size, self.part_off) = struct.unpack(">IIQ", h2[o:o + 16])
        o += 16 + 20
        (self.n_raw, self.raw_off, self.raw_size) = struct.unpack(">IQI", h2[o:o + 16])
        o += 16
        (self.n_grp, self.grp_off, self.grp_size) = struct.unpack(">IQI", h2[o:o + 16])
        if self.compression != 5:
            raise ValueError("compression %d not supported (RVZ zstd only)" % self.compression)
        if self.n_part:
            raise ValueError("Wii partitions not supported (GameCube images only)")
        self.zd = zstandard.ZstdDecompressor()
        raw = self._table(self.raw_off, self.raw_size, self.n_raw * 24)
        self.raw = [struct.unpack(">QQII", raw[i * 24:i * 24 + 24]) for i in range(self.n_raw)]
        grp = self._table(self.grp_off, self.grp_size, self.n_grp * 12)
        self.grp = [struct.unpack(">III", grp[i * 12:i * 12 + 12]) for i in range(self.n_grp)]

    def _table(self, off, size, out_size):
        self.f.seek(off)
        return self.zd.decompress(self.f.read(size), max_output_size=out_size)

    def _group(self, index, disc_off, length):
        off, size, packed = self.grp[index]
        compressed = bool(size & 0x80000000)
        size &= 0x7FFFFFFF
        if size == 0:
            return bytes(length)
        self.f.seek(off << 2)
        data = self.f.read(size)
        if compressed:
            data = self.zd.decompress(data, max_output_size=max(self.chunk, packed) + 0x1000)
        if packed == 0:
            return data[:length]
        out = bytearray()
        p = 0
        pos = disc_off
        while len(out) < length and p < len(data):
            (n,) = struct.unpack(">I", data[p:p + 4])
            p += 4
            if n & 0x80000000:
                n &= 0x7FFFFFFF
                lfg = Lfg(data[p:p + SEED_WORDS * 4])
                p += SEED_WORDS * 4
                lfg.skip(pos % BLOCK)
                out += lfg.get(n)
            else:
                out += data[p:p + n]
                p += n
            pos += n
        return bytes(out[:length])

    def chunks(self):
        """Yield (iso_offset, bytes) covering the whole ISO in order."""
        yield 0, self.disc_header
        pos = 0x80
        for (d_off, d_size, g_index, g_count) in self.raw:
            skipped = d_off % BLOCK
            start = d_off - skipped
            end = d_off + d_size
            for g in range(g_count):
                g_start = start + g * self.chunk
                g_len = min(self.chunk, end - g_start)
                data = self._group(g_index + g, g_start, g_len)
                if g_start < pos:  # first group re-covers the disc header
                    data = data[pos - g_start:]
                    g_start = pos
                yield g_start, data
                pos = g_start + len(data)

    def read(self, off, length):
        """Random access read (decodes only the groups that are needed)."""
        out = bytearray()
        if off < 0x80:
            out += self.disc_header[off:off + length]
        for (d_off, d_size, g_index, g_count) in self.raw:
            skipped = d_off % BLOCK
            start = d_off - skipped
            for g in range(g_count):
                g_start = start + g * self.chunk
                g_end = min(g_start + self.chunk, d_off + d_size)
                lo, hi = max(off, g_start, 0x80), min(off + length, g_end)
                if lo >= hi:
                    continue
                data = self._group(g_index + g, g_start, g_end - g_start)
                out += data[lo - g_start:hi - g_start]
        return bytes(out[:length])


def parse_fst(img):
    hdr = img.read(0, 0x440)
    dol_off, fst_off, fst_size = struct.unpack(">III", hdr[0x420:0x42C])
    fst = img.read(fst_off, fst_size)
    n = struct.unpack(">I", fst[8:12])[0]
    names = fst[n * 12:]
    files = []

    def name_at(o):
        e = names.index(b"\0", o)
        return names[o:e].decode("shift_jis", errors="replace")

    stack = [(n, "")]
    i = 1
    while i < n:
        while stack and i >= stack[-1][0]:
            stack.pop()
        flags_name, a, b = struct.unpack(">III", fst[i * 12:i * 12 + 12])
        is_dir = flags_name >> 24
        name = name_at(flags_name & 0xFFFFFF)
        path = stack[-1][1] + "/" + name
        if is_dir:
            stack.append((b, path))
        else:
            files.append((path, a, b))
        i += 1
    return hdr, dol_off, fst_off, fst_size, files


def dol_size(img, dol_off):
    h = img.read(dol_off, 0x100)
    offs = struct.unpack(">18I", h[0:72])
    sizes = struct.unpack(">18I", h[0x90:0x90 + 72])
    return max(o + s for o, s in zip(offs, sizes))


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    img = Rvz(path)
    if cmd == "info":
        hdr, dol_off, fst_off, fst_size, files = parse_fst(img)
        print("game id   :", hdr[:6].decode())
        print("disc/rev  :", hdr[6], hdr[7])
        print("title     :", hdr[0x20:0x60].split(b"\0")[0].decode("latin-1"))
        print("iso size  :", img.iso_size, "chunk", hex(img.chunk))
        print("dol       :", hex(dol_off), hex(dol_size(img, dol_off)))
        print("fst       :", hex(fst_off), hex(fst_size), len(files), "files")
    elif cmd == "extract":
        out = sys.argv[3]
        hdr, dol_off, fst_off, fst_size, files = parse_fst(img)
        sysd = os.path.join(out, "sys")
        os.makedirs(sysd, exist_ok=True)
        open(os.path.join(sysd, "boot.bin"), "wb").write(hdr[:0x440])
        open(os.path.join(sysd, "bi2.bin"), "wb").write(img.read(0x440, 0x2000))
        apl = img.read(0x2440, 0x20)
        apl_size = 0x20 + struct.unpack(">I", apl[0x14:0x18])[0] + struct.unpack(">I", apl[0x18:0x1C])[0]
        open(os.path.join(sysd, "apploader.img"), "wb").write(img.read(0x2440, apl_size))
        open(os.path.join(sysd, "main.dol"), "wb").write(img.read(dol_off, dol_size(img, dol_off)))
        open(os.path.join(sysd, "fst.bin"), "wb").write(img.read(fst_off, fst_size))
        # Read the ISO once in order and cut the files out of it (fast path).
        files_sorted = sorted(files, key=lambda f: f[1])
        k = 0
        for (c_off, data) in img.chunks():
            c_end = c_off + len(data)
            j = k
            while j < len(files_sorted) and files_sorted[j][1] < c_end:
                p, fo, fs = files_sorted[j]
                lo, hi = max(fo, c_off), min(fo + fs, c_end)
                dst = os.path.join(out, "files", p.lstrip("/"))
                if lo == fo or fs == 0:
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    open(dst, "wb").close()
                if lo < hi:
                    with open(dst, "ab") as w:
                        w.write(data[lo - c_off:hi - c_off])
                j += 1
            while k < len(files_sorted) and files_sorted[k][1] + files_sorted[k][2] <= c_end:
                k += 1
        print("extracted", len(files), "files to", out)
    elif cmd in ("sha1", "iso"):
        h = hashlib.sha1()
        w = open(sys.argv[3], "wb") if cmd == "iso" else None
        total = 0
        for (_, data) in img.chunks():
            h.update(data)
            total += len(data)
            if w:
                w.write(data)
        print(h.hexdigest(), total)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
