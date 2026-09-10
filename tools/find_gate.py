#!/usr/bin/env python3
"""Locate Mewgenics' house-shop weekday gate in any build, structurally.

The shipped mod matches a fixed byte signature. If a game update recompiles
that code the signature stops matching and the mod declines to patch. This
script re-derives the site from scratch and prints a fresh signature.

It anchors on things that survive recompilation, not addresses:

  1. the divide-by-7 magic constant -> the civil-calendar routine
  2. callers of that routine which also reference the shop GON field names
  3. inside that caller: call <calendar> ; cmp [rax+0xc], reg ; jcc

Usage:  python find_gate.py [path-to-Mewgenics.exe]
Needs:  pip install capstone
"""
import struct, sys, collections
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DEFAULT_EXE = "C:/Program Files (x86)/Steam/steamapps/common/Mewgenics/Mewgenics.exe"
MAGIC_DIV7 = 0x4924924924924925
SHOP_STRINGS = ("breakdown", "TracyHouseShop", "TracyHouseShopSpecialDay")


class PE:
    def __init__(self, data):
        self.d = data
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        opt = pe + 24
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        so = opt + struct.unpack_from("<H", data, pe + 20)[0]
        self.secs = []
        for i in range(nsec):
            o = so + i * 40
            vsz, va, rsz, ptr = struct.unpack_from("<IIII", data, o + 8)
            name = data[o:o + 8].rstrip(b"\x00").decode("ascii", "replace")
            self.secs.append((name, va, vsz, ptr, rsz))
        pd_rva, pd_sz = struct.unpack_from("<II", data, opt + 112 + 24)
        pdo = self.r2o(pd_rva)
        self.funcs = sorted({struct.unpack_from("<III", data, pdo + i * 12)[:2]
                             for i in range(pd_sz // 12)})
        self.text = next(s for s in self.secs if s[0] == ".text")

    def r2o(self, rva):
        for _, va, vsz, ptr, rsz in self.secs:
            if va <= rva < va + max(vsz, rsz):
                return ptr + (rva - va)
        return None

    def o2r(self, off):
        for _, va, vsz, ptr, rsz in self.secs:
            if ptr <= off < ptr + rsz:
                return va + (off - ptr)
        return None

    def func_of(self, rva):
        for b, e in self.funcs:
            if b <= rva < e:
                return (b, e)
        return None

    def cstring(self, rva, maxlen=64):
        o = self.r2o(rva)
        if o is None or o == 0:
            return None
        if self.d[o - 1] != 0:          # must start a string, not point mid-way
            return None
        raw = self.d[o:o + maxlen].split(b"\x00")[0]
        if 2 <= len(raw) <= maxlen and all(32 <= c < 127 for c in raw):
            return raw.decode("ascii")
        return None


def rip_target(addr, size, ops):
    """Resolve '[rip + 0x1234]' in a capstone op_str to an absolute RVA."""
    i = ops.find("[rip ")
    if i < 0:
        return None
    j = ops.find("]", i)
    body = ops[i + 5:j].strip()
    if not body or body[0] not in "+-":
        return None
    try:
        val = int(body[1:].strip(), 16)
    except ValueError:
        return None
    return addr + size + (val if body[0] == "+" else -val)


def main(path):
    pe = PE(open(path, "rb").read())
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    _, tva, _, tptr, trsz = pe.text
    code = pe.d[tptr:tptr + trsz]

    # ---- 1. calendar routine, via the divide-by-7 idiom -------------------
    needle = struct.pack("<Q", MAGIC_DIV7)
    cands = collections.Counter()
    i = code.find(needle)
    while i >= 0:
        f = pe.func_of(pe.o2r(tptr + i))
        if f:
            cands[f] += 1
        i = code.find(needle, i + 1)
    if not cands:
        print("no divide-by-7 constant found; calendar routine not located")
        return 1

    # ---- 2. callers that also mention the shop definitions ----------------
    target = None
    for cal, _ in cands.most_common():
        callers = set()
        j = code.find(b"\xe8")
        while j >= 0:
            if j + 5 <= len(code):
                dst = tva + j + 5 + struct.unpack_from("<i", code, j + 1)[0]
                if dst == cal[0]:
                    f = pe.func_of(tva + j)
                    if f:
                        callers.add(f)
            j = code.find(b"\xe8", j + 1)
        for fn in callers:
            seen = set()
            o = pe.r2o(fn[0])
            for a, sz, mn, ops in md.disasm_lite(pe.d[o:o + (fn[1] - fn[0])], fn[0]):
                t = rip_target(a, sz, ops)
                if t is not None:
                    s = pe.cstring(t)
                    if s in SHOP_STRINGS:
                        seen.add(s)
            if len(seen) >= 2:
                target = (fn, cal)
                break
        if target:
            break
    if not target:
        print("calendar found, but no caller referencing the shop definitions")
        return 1
    (fb, fe), (cb, _) = target
    print("calendar routine  : RVA 0x%X" % cb)
    print("day-rollover func : RVA 0x%X..0x%X" % (fb, fe))

    # ---- 3. call <calendar> ; cmp [rax+0xc], reg ; jcc --------------------
    o = pe.r2o(fb)
    ins = list(md.disasm(pe.d[o:o + (fe - fb)], fb))
    for k, x in enumerate(ins):
        if x.mnemonic != "call" or not x.op_str.startswith("0x") or int(x.op_str, 16) != cb:
            continue
        for y in ins[k + 1:k + 4]:
            if y.mnemonic == "cmp" and "[rax + 0xc]" in y.op_str:
                nxt = ins[ins.index(y) + 1]
                if not nxt.mnemonic.startswith("j"):
                    continue
                off = pe.r2o(nxt.address)
                print()
                print("weekday test      : %s %s" % (y.mnemonic, y.op_str))
                print("GATE              : RVA 0x%X  file 0x%X  %d bytes  [%s]"
                      % (nxt.address, off, nxt.size, nxt.bytes.hex()))
                print("patch             : overwrite those %d bytes with 0x90" % nxt.size)
                emit_signature(pe, md, ins, k, nxt)
                return 0
    print("gate not found inside the day-rollover function")
    return 1


def emit_signature(pe, md, ins, call_idx, gate):
    """Print a C signature array covering the shop-level test up to the gate."""
    start = max(0, call_idx - 8)
    window = ins[start:ins.index(gate) + 1]
    out, gate_off = [], 0
    for x in window:
        if x is gate:
            gate_off = len(out)
            out.extend(["0x%02X" % b for b in x.bytes[:2]])   # opcode only
            break
        b = list(x.bytes)
        enc = x.encoding
        mask = set()
        if enc.disp_size:
            mask.update(range(enc.disp_offset, enc.disp_offset + enc.disp_size))
        if enc.imm_size and (x.mnemonic.startswith("j") or x.mnemonic == "call"):
            mask.update(range(enc.imm_offset, enc.imm_offset + enc.imm_size))
        out.extend("0xFFFF" if i in mask else "0x%02X" % v for i, v in enumerate(b))
    print()
    print("Paste into dllmain.cpp (0xFFFF = wildcard):")
    print()
    print("static const int SIG[] = {")
    for i in range(0, len(out), 12):
        print("    " + ", ".join(out[i:i + 12]) + ",")
    print("};")
    print("static const size_t GATE_OFF  = %d;" % gate_off)
    print("static const size_t GATE_SIZE = %d;" % gate.size)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_EXE))
