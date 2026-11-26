# LM360 (0.3.9): register localization for any translated function, calls included, and an independent checker of
# its output. LuigisMansion-X360, own code.
#
# The plain translation keeps the Gekko's registers in the Ctx structure (c.r[], c.f[], c.ps1[]). The XDK compiler
# does no type-based alias analysis, so every guest store (a pointer write into guest memory) may change c for it:
# each register is read back from memory at every use and written at every assignment (load-hit-store stalls on
# the Xenon). localize() rewrites a function body so that the registers it uses live in local variables:
#   - at entry, the registers read before being written are loaded from c (the others start at 0, never read);
#   - before a call (f_X(c), c.call), an interrupt check and a return, the registers written since the last such
#     point (the "dirty" ones, forward data flow) are stored back to c, so c holds the truth for the callee;
#   - after a call or a serviced interrupt, the registers that may be read later (liveness, backward data flow)
#     are reloaded from c, which the callee may have changed.
# Everything else (the operations, their order, the memory accesses, cr/xer/lr/ctr, which stay in c) is the plain
# translation's text with c.r[n] renamed r_n: same results bit for bit, as long as each local holds the register's
# value whenever it is read, and c whenever other code can see it.
#
# check() proves that for the rewritten text, independently of how it was made: it parses each line into
# statements and runs an abstract interpretation over the function's control flow (labels, gotos, ifs, switches,
# returns) with two facts per register, "the local holds the truth" and "c holds the truth":
#   read of r_n: the local must hold it      assignment r_n = ...: local yes, c no
#   c.r[n] = r_n: the local must hold it; then c does      r_n = c.r[n]: c must hold it; then the local does
#   call / service_irq / return / end of function: c must hold every register; after a call no local does
# Facts meet by intersection at joins. Any statement it does not understand (a c.r[] left over, a call inside an
# expression, an unknown helper taking c, a compound assignment to a local) fails the check. A function that fails
# keeps its plain translation (localize returns None and the reason).
import re

REG_RE = re.compile(r"c\.(r|f|ps1)\[(\d+)\]")
PSQL_RE = re.compile(r"c\.psq_l\((\d+), (.+?), (\d), (\d)\);")
PSQST_RE = re.compile(r"c\.psq_st\((\d+), (.+?), (\d), (\d)\);")
CALL_RE = re.compile(r"(f_[0-9A-F]{8}\(c\)|c\.call\([^;]*?\));")
TAIL_RE = re.compile(r"(f_[0-9A-F]{8}\(c\)|c\.call\([^;]*?\));\s*return;")
IRQ_TXT = "IRQ_CHECK(c);"
GOTO_RE = re.compile(r"goto (L_[0-9A-F]{8});")
LABEL_RE = re.compile(r"^(L_[0-9A-F]{8}):$")
# instructions whose helpers read or write registers by number, switch context or stop: left plain
REJECT_RE = re.compile(r"c\.mtmsr\(|c\.rfi\(|c\.syscall\(|c\.mtspr\(|c\.trap\(|c\.unimpl\(|lswi\(|stswi\(|"
                       r"guest_call|RUN_HOOK|service_irq|g_irqPending|LM_BACK")
STMW_RE = re.compile(r"\{ u32 ea_ = (.+?); for \(int i_ = (\d+); i_ < 32; i_\+\+, ea_ \+= 4\) wr32\(ea_, c\.r\[i_\]\); \}")
LMW_RE = re.compile(r"\{ u32 ea_ = (.+?); for \(int i_ = (\d+); i_ < 32; i_\+\+, ea_ \+= 4\) c\.r\[i_\] = rd32\(ea_\); \}")
LOCAL_NAME = {"r": "r_%d", "f": "f_%d", "ps1": "p_%d"}
LOCAL_KIND = {"r": "r", "f": "f", "p": "ps1"}


def unroll_multiple(line):
    """stmw/lmw: the loop over registers becomes one access per register (same addresses, same order)."""
    line = STMW_RE.sub(lambda m: "{ u32 ea_ = %s; %s }" % (m.group(1), " ".join(
        "wr32(ea_ + %du, c.r[%d]);" % (4 * (k - int(m.group(2))), k) for k in range(int(m.group(2)), 32))), line)
    line = LMW_RE.sub(lambda m: "{ u32 ea_ = %s; %s }" % (m.group(1), " ".join(
        "c.r[%d] = rd32(ea_ + %du);" % (k, 4 * (k - int(m.group(2)))) for k in range(int(m.group(2)), 32))), line)
    return line


def regs_rw(text):
    """(reads, writes) of guest registers in plain-translation text."""
    reads, writes = set(), set()
    for m in REG_RE.finditer(text):
        key = (m.group(1), int(m.group(2)))
        if re.match(r"\s*=(?!=)", text[m.end():]):
            writes.add(key)
        else:
            reads.add(key)
    for m in PSQL_RE.finditer(text):
        n = int(m.group(1))
        writes.update((("f", n), ("ps1", n)))
    for m in PSQST_RE.finditer(text):
        n = int(m.group(1))
        reads.update((("f", n), ("ps1", n)))
    return reads, writes


def lname(k, n):
    return LOCAL_NAME[k] % n


def wb_text(regs):
    return "".join("c.%s[%d] = %s; " % (k, n, lname(k, n)) for (k, n) in sorted(regs))


def rl_text(regs):
    return "".join("%s = c.%s[%d]; " % (lname(k, n), k, n) for (k, n) in sorted(regs))


def is_terminal(line):
    s = line.strip()
    if s.startswith("goto ") or s == "return;" or s.startswith("IRQ_CHECK(c); goto "):
        return True
    if re.match(r"^(f_[0-9A-F]{8}\(c\)|c\.call\([^;]*?\)); return;$", s) or s == "{ c.call(c.ctr); return; }":
        return True
    if s.startswith("{ switch (") and s.endswith("default: c.call(c.ctr); return; } }"):
        return True
    return False


def localize(body, addr, back=True):
    """Localized body (list of lines) or (None, reason)."""
    body = [unroll_multiple(l) for l in body]
    text = "\n".join(body)
    if "i_]" in text:
        return None, "register index loop"
    m = REJECT_RE.search(text)
    if m:
        return None, "uses " + m.group(0)
    n = len(body)
    labels = {}
    for i, l in enumerate(body):
        m = LABEL_RE.match(l.strip())
        if m:
            labels[m.group(1)] = i
    info = []
    used = set()
    for i, l in enumerate(body):
        s = l.strip()
        reads, writes = regs_rw(l)
        used |= reads | writes
        tails = list(TAIL_RE.finditer(l))
        tail_spans = [(t.start(), t.end()) for t in tails]
        calls = [c for c in CALL_RE.finditer(l) if not any(a <= c.start() < b for a, b in tail_spans)]
        irq = l.count(IRQ_TXT)
        if len(calls) + irq > 1:
            return None, "two calls in one instruction"
        sync = None
        if calls:
            sync = ("call", calls[0].start(), calls[0].end())
        elif irq:
            p = l.index(IRQ_TXT)
            sync = ("irq", p, p + len(IRQ_TXT))
        if sync and "return;" in l[sync[2]:]:
            return None, "return after a call in one instruction"
        pre = l[:sync[1]] if sync else l
        post = l[sync[2]:] if sync else ""
        rpre, wpre = regs_rw(pre)
        rpost, wpost = regs_rw(post)
        cond = "if (" in l
        succ = [labels[t] for t in GOTO_RE.findall(l) if t in labels]
        if len(succ) != len(GOTO_RE.findall(l)):
            return None, "goto outside the function"
        if not is_terminal(l) and i + 1 < n:
            succ.append(i + 1)
        info.append(dict(reads=reads, writes=writes, sync=sync, cond=cond, rpre=rpre, wpre=wpre, rpost=rpost,
                         wpost=wpost, succ=succ, label=bool(LABEL_RE.match(s))))
    preds = [[] for _ in range(n)]
    for i, d in enumerate(info):
        for j in d["succ"]:
            preds[j].append(i)
    # dirty (forward): registers written since the last point where c was made whole
    d_in = [set() for _ in range(n)]
    d_out = [set() for _ in range(n)]
    changed = True
    while changed:
        changed = False
        for i in range(n):
            d = info[i]
            inn = set()
            for p in preds[i]:
                inn |= d_out[p]
            if d["sync"] and d["sync"][0] == "call" and not d["cond"]:
                out = set(d["wpost"])
            else:
                out = inn | d["writes"]
            if inn != d_in[i] or out != d_out[i]:
                d_in[i], d_out[i] = inn, out
                changed = True
    # the registers each line stores back to c (before its call, interrupt check, tail call or return): a store
    # back reads the local, so it counts as a read for the liveness below (a path that skips a write joins one
    # that has it: without this the store could take a local left stale by an earlier call)
    for i, l in enumerate(body):
        d = info[i]
        wb = set()
        if d["sync"]:
            wb |= d_in[i] | d["wpre"]
        for m in TAIL_RE.finditer(l):
            wb |= d_in[i] | regs_rw(l[:m.start()])[1]
        for m in re.finditer(r"\breturn;", l):
            wb |= d_in[i] | regs_rw(l[:m.start()])[1]
        d["wbreads"] = wb
    end_wb = d_out[-1] if n and not is_terminal(body[-1]) else set()
    # liveness (backward): registers that may be read (or stored back) before being written from a point on
    live_in = [set() for _ in range(n)]
    live_out = [set() for _ in range(n)]
    changed = True
    while changed:
        changed = False
        for i in range(n - 1, -1, -1):
            d = info[i]
            out = set(end_wb) if i == n - 1 else set()
            for j in d["succ"]:
                out |= live_in[j]
            if d["sync"] and d["sync"][0] == "call" and not d["cond"]:
                inn = d["rpre"] | d["wbreads"]
            elif d["sync"] or d["cond"]:
                inn = d["reads"] | d["wbreads"] | out
            else:
                inn = d["reads"] | d["wbreads"] | (out - (d["writes"] - d["reads"]))
            if out != live_out[i] or inn != live_in[i]:
                live_out[i], live_in[i] = out, inn
                changed = True
    back_txt = ("LM_BACK(0x%08Xu); " % addr) if back else ""
    out_lines = []
    for i, l in enumerate(body):
        d = info[i]
        # tail calls and returns: c made whole first
        def sub_tail(m):
            r, w = regs_rw(l[:m.start()])  # writes earlier on the line (same text before substitution)
            return "{ %s%s; return; }" % (wb_text((d_in[i] | w) & used), rename(m.group(1)))
        def sub_ret(m):
            r, w = regs_rw(l[:m.start()])
            return "{ %sreturn; }" % wb_text((d_in[i] | w) & used)
        # operate on the original text, then rename the registers in the pieces that came from it
        pieces = []
        pos = 0
        events = []
        for m in TAIL_RE.finditer(l):
            events.append((m.start(), m.end(), "tail", m))
        if d["sync"]:
            events.append((d["sync"][1], d["sync"][2], d["sync"][0], None))
        tail_spans = [(a, b) for (a, b, k, _) in events if k == "tail"]
        for m in re.finditer(r"\breturn;", l):
            if not any(a <= m.start() < b for a, b in tail_spans):
                events.append((m.start(), m.end(), "ret", m))
        events.sort()
        for (a, b, kind, m) in events:
            pieces.append(rename(l[pos:a]))
            if kind == "tail":
                pieces.append(sub_tail(m))
            elif kind == "ret":
                pieces.append(sub_ret(m))
            else:
                dset = (d_in[i] | d["wpre"]) & used
                rset = (live_out[i] | d["rpost"]) & used
                if d["cond"] or kind == "irq":
                    rset |= dset
                if kind == "call":
                    pieces.append("{ %s%s; %s%s}" % (wb_text(dset), rename(l[a:b - 1]), rl_text(rset), back_txt))
                else:
                    pieces.append("if (g_irqPending) { %sservice_irq(c); %s%s}" % (wb_text(dset), rl_text(rset), back_txt))
            pos = b
        pieces.append(rename(l[pos:]))
        out_lines.append("".join(pieces))
    entry = live_in[0] if n else set()
    decl = []
    for (k, n_) in sorted(used):
        t = "u32" if k == "r" else "double"
        if (k, n_) in entry:
            decl.append("\t%s %s = c.%s[%d];" % (t, lname(k, n_), k, n_))
        else:
            decl.append("\t%s %s = 0;" % (t, lname(k, n_)))
    tail = []
    if n and not is_terminal(body[-1]):
        tail = ["\t{ %s}" % wb_text(d_out[-1] & used)]
    result = decl + out_lines + tail
    ok, why = check(result)
    if not ok:
        return None, "check: " + why
    return result, None


def rename(s):
    s = PSQL_RE.sub(lambda m: "lm_psq_l(c, %s, %s, %s, f_%s, p_%s);" % (m.group(2), m.group(3), m.group(4), m.group(1), m.group(1)), s)
    s = PSQST_RE.sub(lambda m: "lm_psq_st(c, %s, %s, %s, f_%s, p_%s);" % (m.group(2), m.group(3), m.group(4), m.group(1), m.group(1)), s)
    return REG_RE.sub(lambda m: lname(m.group(1), int(m.group(2))), s)


# ---------------------------------------------------------------------------------------------------------------
# The checker (independent of localize's own data flow)
LOC_RE = re.compile(r"\b([rfp])_([0-9]|[12][0-9]|3[01])\b")
SAFE_CFUNCS = {"srawi", "sraw", "lm_psq_l", "lm_psq_st"}
SAFE_CMETHODS = {"crbit", "setCrbit", "cmps", "cmpu", "setCr0", "setCr1", "fcmp", "getCr", "mtcrf", "getXer",
                 "setXer", "mtfsf", "mtfsfi", "mfspr", "mftb"}


class CheckError(Exception):
    pass


def _match_paren(s, i):
    depth = 0
    for j in range(i, len(s)):
        if s[j] == "(":
            depth += 1
        elif s[j] == ")":
            depth -= 1
            if depth == 0:
                return j
    raise CheckError("unbalanced parentheses")


def _match_brace(s, i):
    depth = 0
    for j in range(i, len(s)):
        if s[j] == "{":
            depth += 1
        elif s[j] == "}":
            depth -= 1
            if depth == 0:
                return j
    raise CheckError("unbalanced braces")


def _stmt_end(s, i):
    depth = 0
    for j in range(i, len(s)):
        ch = s[j]
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        elif ch == ";" and depth == 0:
            return j
        elif ch in "{}" and depth == 0:
            raise CheckError("brace inside a statement")
    raise CheckError("statement without ';'")


def parse(s):
    """Statements of one line: ('stmt', text), ('if', cond, [nodes]), ('block', [nodes]),
    ('switch', expr, [(case, [nodes])])."""
    nodes = []
    i = 0
    s = re.sub(r"/\*.*?\*/", " ", s).strip()  # comments (/* nop */)
    while i < len(s):
        if s[i].isspace():
            i += 1
            continue
        if s.startswith("if (", i) or s.startswith("if(", i):
            p = s.index("(", i)
            q = _match_paren(s, p)
            cond = s[p + 1:q]
            j = q + 1
            while j < len(s) and s[j].isspace():
                j += 1
            if j < len(s) and s[j] == "{":
                e = _match_brace(s, j)
                nodes.append(("if", cond, parse(s[j + 1:e])))
                i = e + 1
            else:
                e = _stmt_end(s, j)
                nodes.append(("if", cond, [("stmt", s[j:e].strip())]))
                i = e + 1
            if s.startswith("else", i):
                raise CheckError("else")
        elif s.startswith("switch (", i):
            p = s.index("(", i)
            q = _match_paren(s, p)
            expr = s[p + 1:q]
            j = s.index("{", q)
            e = _match_brace(s, j)
            inner = s[j + 1:e]
            arms = []
            for part in re.split(r"(?=\bcase [^:]+:|\bdefault:)", inner):
                part = part.strip()
                if not part:
                    continue
                m = re.match(r"(case [^:]+|default):(.*)$", part, re.S)
                if not m:
                    raise CheckError("switch arm")
                arms.append((m.group(1), parse(m.group(2))))
            nodes.append(("switch", expr, arms))
            i = e + 1
        elif s[i] == "{":
            e = _match_brace(s, i)
            nodes.append(("block", parse(s[i + 1:e])))
            i = e + 1
        elif s[i] == "}":
            raise CheckError("stray '}'")
        else:
            m = LABEL_RE.match(s[i:])
            if m:
                nodes.append(("label", m.group(1)))
                i = len(s)
                continue
            e = _stmt_end(s, i)
            nodes.append(("stmt", s[i:e].strip()))
            i = e + 1
    return nodes


class State(object):
    __slots__ = ("L", "C")

    def __init__(self, L, C):
        self.L, self.C = L, C

    def join(self, o):
        return State(self.L & o.L, self.C & o.C)

    def __eq__(self, o):
        return self.L == o.L and self.C == o.C


def _reads(text, st):
    for m in LOC_RE.finditer(text):
        key = (m.group(1), int(m.group(2)))
        if key not in st.L:
            raise CheckError("%s_%d read while its local may be stale" % key)


def _need_c(st, universe, what):
    missing = universe - st.C
    if missing:
        raise CheckError("%s while c lacks %s" % (what, ", ".join("%s_%d" % k for k in sorted(missing))))


def _stmt(t, st, universe, edges):
    """Applies one statement; returns the state after it or None (no fallthrough)."""
    m = re.match(r"^goto (L_[0-9A-F]{8})$", t)
    if m:
        edges.append((m.group(1), st))
        return None
    if t == "return":
        _need_c(st, universe, "return")
        return None
    if re.match(r"^(f_[0-9A-F]{8}\(c\)|c\.call\([^;]*\)|service_irq\(c\))$", t):
        _reads(t[t.index("("):], st)
        _need_c(st, universe, "call")
        return State(frozenset(), universe)
    if re.match(r"^LM_BACK\(0x[0-9A-F]{8}u\)$", t):
        return st
    m = re.match(r"^c\.(r|f|ps1)\[(\d+)\] = ([rfp])_(\d+)$", t)
    if m:
        k, n = m.group(1), int(m.group(2))
        if LOCAL_KIND[m.group(3)] != k or int(m.group(4)) != n:
            raise CheckError("store of another register: " + t)
        if (m.group(3), n) not in st.L:
            raise CheckError("store of a stale local: " + t)
        return State(st.L, st.C | {(m.group(3), n)})
    m = re.match(r"^(?:(?:u32|double) )?([rfp])_(\d+) = c\.(r|f|ps1)\[(\d+)\]$", t)
    if m:
        k, n = m.group(3), int(m.group(4))
        if LOCAL_KIND[m.group(1)] != k or int(m.group(2)) != n:
            raise CheckError("load of another register: " + t)
        if (m.group(1), n) not in st.C:
            raise CheckError("load from a stale c: " + t)
        return State(st.L | {(m.group(1), n)}, st.C)
    m = re.match(r"^(?:u32|double) ([rfp])_(\d+) = 0$", t)
    if m:
        return st  # declaration of a local written before it is read
    if REG_RE.search(t):
        raise CheckError("register left in c: " + t)
    for m in re.finditer(r"\b([A-Za-z_][A-Za-z_0-9]*)\(c[,)]", t):
        if m.group(1) not in SAFE_CFUNCS:
            raise CheckError("call taking c inside a statement: " + t)
    for m in re.finditer(r"\bc\.([A-Za-z_][A-Za-z_0-9]*)\(", t):
        if m.group(1) not in SAFE_CMETHODS:
            raise CheckError("method of c: " + t)
    m = re.match(r"^lm_psq_l\(c, (.+), (\d), (\d), f_(\d+), p_(\d+)\)$", t)
    if m:
        _reads(m.group(1), st)
        a, b = ("f", int(m.group(4))), ("p", int(m.group(5)))
        return State(st.L | {a, b}, st.C - {a, b})
    if re.search(r"\b[rfp]_(?:[0-9]|[12][0-9]|3[01])\s*([-+*/|&^%]|<<|>>)=", t):
        raise CheckError("compound assignment: " + t)
    m = re.match(r"^(?:(?:u32|s32|double|u64|s64|float) )?([rfp])_([0-9]|[12][0-9]|3[01])\s*=(?!=)(.*)$", t, re.S)
    if m:
        targets = [(m.group(1), int(m.group(2)))]
        rhs = m.group(3)
        while True:  # a chain: f_0 = p_0 = value
            m2 = re.match(r"^\s*([rfp])_([0-9]|[12][0-9]|3[01])\s*=(?!=)(.*)$", rhs, re.S)
            if not m2:
                break
            targets.append((m2.group(1), int(m2.group(2))))
            rhs = m2.group(3)
        if re.search(r"\b[rfp]_(?:[0-9]|[12][0-9]|3[01])\s*=(?!=)", rhs):
            raise CheckError("assignment inside an expression: " + t)
        _reads(rhs, st)
        keys = set(targets)
        return State(st.L | keys, st.C - keys)
    if re.search(r"\b[rfp]_(?:[0-9]|[12][0-9]|3[01])\s*=(?!=)", t):
        raise CheckError("assignment inside an expression: " + t)
    _reads(t, st)
    return st


def _run(nodes, st, universe, edges):
    for node in nodes:
        if st is None:
            return None  # code after a goto/return in the same line: unreachable
        kind = node[0]
        if kind == "stmt":
            st = _stmt(node[1], st, universe, edges)
        elif kind == "block":
            st = _run(node[1], st, universe, edges)
        elif kind == "if":
            _reads(node[1], st)
            if REG_RE.search(node[1]):
                raise CheckError("register left in c: " + node[1])
            for m in re.finditer(r"\b([A-Za-z_][A-Za-z_0-9]*)\(c[,)]", node[1]):
                if m.group(1) not in SAFE_CFUNCS:
                    raise CheckError("call in a condition")
            for m in re.finditer(r"\bc\.([A-Za-z_][A-Za-z_0-9]*)\(", node[1]):
                if m.group(1) not in SAFE_CMETHODS:
                    raise CheckError("method of c in a condition")
            body = _run(node[2], st, universe, edges)
            st = st if body is None else st.join(body)
        elif kind == "switch":
            _reads(node[1], st)
            if REG_RE.search(node[1]):
                raise CheckError("register left in c: " + node[1])
            after = None
            has_default = False
            for case, arm in node[2]:
                has_default |= case == "default"
                r = _run(arm, st, universe, edges)
                if r is not None:
                    after = r if after is None else after.join(r)
            if not has_default:
                after = st if after is None else after.join(st)
            st = after
        elif kind == "label":
            pass
    return st


def check(lines):
    """(True, '') when the localized text keeps every register's truth where it is used, else (False, why)."""
    universe = set()
    for l in lines:
        for m in LOC_RE.finditer(l):
            universe.add((m.group(1), int(m.group(2))))
    universe = frozenset(universe)
    labels = {}
    parsed = []
    try:
        for i, l in enumerate(lines):
            m = LABEL_RE.match(l.strip())
            if m:
                labels[m.group(1)] = i
                parsed.append([])
            else:
                parsed.append(parse(l))
        states = [None] * len(lines)
        states[0] = State(frozenset(), universe) if lines else None
        work = [0] if lines else []
        while work:
            i = work.pop()
            edges = []
            ft = _run(parsed[i], states[i], universe, edges)
            targets = [(labels.get(t), s) for (t, s) in edges]
            if ft is not None:
                if i + 1 < len(lines):
                    targets.append((i + 1, ft))
                else:
                    _need_c(ft, universe, "end of function")
            for j, s in targets:
                if j is None:
                    raise CheckError("goto to an unknown label")
                old = states[j]
                new = s if old is None else old.join(s)
                if old is None or not (new == old):
                    states[j] = new
                    if j not in work:
                        work.append(j)
    except CheckError as e:
        return False, "line %d: %s" % (i + 1 if lines else 0, e)
    return True, ""


def add_back(body, addr):
    """Plain translation: the profiler's marker set back to this function after each call that returns here."""
    out = []
    for l in body:
        tails = [(m.start(), m.end()) for m in TAIL_RE.finditer(l)]
        def sub_call(m):
            if any(a <= m.start() < b for a, b in tails):
                return m.group(0)
            return "%s; LM_BACK(0x%08Xu);" % (m.group(1), addr)
        l = CALL_RE.sub(sub_call, l)
        l = l.replace(IRQ_TXT, "if (g_irqPending) { service_irq(c); LM_BACK(0x%08Xu); }" % addr)
        out.append(l)
    return out
