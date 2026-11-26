"""DOL and lm-decomp config readers shared by the recompiler tools (LuigisMansion-X360)."""
import bisect
import re
import struct


class Dol:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        offs = struct.unpack(">18I", d[0:72])
        addrs = struct.unpack(">18I", d[72:144])
        sizes = struct.unpack(">18I", d[144:216])
        self.bss_addr, self.bss_size, self.entry = struct.unpack(">III", d[216:228])
        self.sections = []  # (addr, off, size, is_text)
        for i in range(18):
            if sizes[i]:
                self.sections.append((addrs[i], offs[i], sizes[i], i < 7))

    def has(self, a):
        return any(s[0] <= a < s[0] + s[2] for s in self.sections)

    def is_text(self, a):
        return any(s[3] and s[0] <= a < s[0] + s[2] for s in self.sections)

    def word(self, a):
        for (sa, so, ss, _) in self.sections:
            if sa <= a < sa + ss:
                return struct.unpack(">I", self.data[so + a - sa:so + a - sa + 4])[0]
        raise KeyError("%08X outside DOL" % a)

    def words(self, a, n):
        for (sa, so, ss, _) in self.sections:
            if sa <= a and a + n * 4 <= sa + ss:
                o = so + a - sa
                return struct.unpack(">%dI" % n, self.data[o:o + n * 4])
        return tuple(self.word(a + 4 * i) for i in range(n))


SYM_RE = re.compile(r"^(\S+) = \.?([\w.]+):0x([0-9A-Fa-f]+); // (.*)$")


def load_symbols(path):
    """Returns (functions {addr: (name, size)}, objects {addr: (name, size, section)})."""
    funcs, objs = {}, {}
    for line in open(path, encoding="utf-8"):
        m = SYM_RE.match(line.strip())
        if not m:
            continue
        name, sect, addr, rest = m.group(1), m.group(2), int(m.group(3), 16), m.group(4)
        size = 0
        ms = re.search(r"size:0x([0-9A-Fa-f]+)", rest)
        if ms:
            size = int(ms.group(1), 16)
        if "type:function" in rest:
            funcs[addr] = (name, size)
        elif "type:object" in rest:
            objs[addr] = (name, size, sect)
    return funcs, objs


def load_splits(path):
    """Returns a function addr -> translation unit name (text ranges only)."""
    ranges = []
    tu = None
    for line in open(path, encoding="utf-8"):
        if line and not line[0].isspace() and line.rstrip().endswith(":"):
            tu = line.rstrip()[:-1]
            continue
        m = re.match(r"\s+\.(text|init)\s+start:0x([0-9A-Fa-f]+) end:0x([0-9A-Fa-f]+)", line)
        if m and tu and tu != "Sections":
            ranges.append((int(m.group(2), 16), int(m.group(3), 16), tu))
    ranges.sort()
    starts = [r[0] for r in ranges]

    def tu_of(a):
        i = bisect.bisect_right(starts, a) - 1
        if i >= 0 and ranges[i][0] <= a < ranges[i][1]:
            return ranges[i][2]
        return None

    return tu_of
