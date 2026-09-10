#!/usr/bin/env python3
"""Rebuild a browsable index of a Mewgenics build.

There is no source code inside the binary - this produces maps of it, so the
next person (or the next game version) starts from a searchable index instead
of a blank disassembler.

Outputs into --out (default ./analysis):
  source_files.txt  debug paths left in the binary: the project's module map
  classes.txt       glaiel:: class names recovered from RTTI
  functions.tsv     every function from .pdata + the strings it references
  gon_fields.txt    GON field names the engine reads (needs --getfield)

Usage:
  python build_index.py [exe] [--out DIR] [--getfield 0xRVA]

--getfield is the RVA of GonObject::operator[](const std::string&). Find it by
disassembling any function that reads a known GON field and taking the call
made right after the field-name string is materialised.

Needs: pip install capstone
"""
import struct, sys, os, collections
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DEFAULT_EXE = "C:/Program Files (x86)/Steam/steamapps/common/Mewgenics/Mewgenics.exe"


def sections(d):
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    opt = pe + 24
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    so = opt + struct.unpack_from("<H", d, pe + 20)[0]
    out = []
    for i in range(nsec):
        o = so + i * 40
        vsz, va, rsz, ptr = struct.unpack_from("<IIII", d, o + 8)
        out.append((d[o:o + 8].rstrip(b"\x00").decode("ascii", "replace"), va, vsz, ptr, rsz))
    return out, opt


def rip_target(addr, size, ops):
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


def main():
    args = [a for a in sys.argv[1:]]
    exe, out_dir, getfield = DEFAULT_EXE, "analysis", None
    i = 0
    while i < len(args):
        if args[i] == "--out":
            out_dir = args[i + 1]; i += 2
        elif args[i] == "--getfield":
            getfield = int(args[i + 1], 0); i += 2
        else:
            exe = args[i]; i += 1
    d = open(exe, "rb").read()
    secs, opt = sections(d)
    os.makedirs(out_dir, exist_ok=True)

    def r2o(rva):
        for _, va, vsz, ptr, rsz in secs:
            if va <= rva < va + max(vsz, rsz):
                return ptr + (rva - va)
        return None

    def o2r(off):
        for _, va, vsz, ptr, rsz in secs:
            if ptr <= off < ptr + rsz:
                return va + (off - ptr)
        return None

    def cstring(rva, maxlen=72):
        o = r2o(rva)
        if o is None or o == 0 or d[o - 1] != 0:
            return None
        raw = d[o:o + maxlen].split(b"\x00")[0]
        if 3 <= len(raw) <= maxlen and all(32 <= c < 127 for c in raw):
            return raw.decode("ascii")
        return None

    # ---- source file map -------------------------------------------------
    srcs = set()
    for ext in (b".cpp", b".h", b".hpp"):
        k = d.find(ext)
        while k >= 0:
            j = k
            lim = max(0, k - 200)
            while j > lim and 32 <= d[j - 1] < 127:
                j -= 1
            seg = d[j:k + len(ext)]
            pos = seg.find(b":")
            if 0 < pos < 3:
                srcs.add(seg[pos - 1:].decode("ascii", "replace"))
            k = d.find(ext, k + 1)
    open(os.path.join(out_dir, "source_files.txt"), "w").write("\n".join(sorted(srcs)))

    # ---- RTTI class names ------------------------------------------------
    cls, tag = set(), b".?AV"
    k = d.find(tag)
    while k >= 0:
        end = d.find(b"@@", k)
        if 0 < end - k < 80:
            name = d[k + 4:end]
            if name.endswith(b"@glaiel"):
                nm = name[:-7]
                if nm and all(48 <= c < 123 for c in nm):
                    cls.add(nm.decode("ascii", "replace"))
        k = d.find(tag, k + 1)
    open(os.path.join(out_dir, "classes.txt"), "w").write("\n".join(sorted(cls)))

    # ---- function index --------------------------------------------------
    pd_rva, pd_sz = struct.unpack_from("<II", d, opt + 112 + 24)
    pdo = r2o(pd_rva)
    funcs = sorted({struct.unpack_from("<III", d, pdo + i * 12)[:2] for i in range(pd_sz // 12)})
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    rows = []
    for b, e in funcs:
        o = r2o(b)
        if o is None or e <= b or e - b > 200000:
            continue
        seen = []
        for a, sz, mn, ops in md.disasm_lite(d[o:o + (e - b)], b):
            t = rip_target(a, sz, ops)
            if t is None:
                continue
            s = cstring(t)
            if s and s not in seen:
                seen.append(s)
            if len(seen) >= 24:
                break
        rows.append("0x%08X\t0x%08X\t%d\t%s" % (b, e, e - b, " | ".join(seen)))
    with open(os.path.join(out_dir, "functions.tsv"), "w", encoding="utf-8") as f:
        f.write("start\tend\tsize\treferenced_strings\n" + "\n".join(rows))

    # ---- GON fields ------------------------------------------------------
    n_fields = 0
    if getfield:
        tx = next(s for s in secs if s[0] == ".text")
        code = d[tx[3]:tx[3] + tx[4]]
        names = collections.Counter()
        k = code.find(b"\xe8")
        while k >= 0:
            if k + 5 <= len(code) and tx[1] + k + 5 + struct.unpack_from("<i", code, k + 1)[0] == getfield:
                site = tx[1] + k
                o = r2o(site - 160)
                best = None
                for a, sz, mn, ops in md.disasm_lite(d[o:o + 160], site - 160):
                    t = rip_target(a, sz, ops)
                    if t is None:
                        continue
                    s = cstring(t, 48)
                    if s and s.replace("_", "a").isalnum():
                        best = s
                if best:
                    names[best] += 1
            k = code.find(b"\xe8", k + 1)
        open(os.path.join(out_dir, "gon_fields.txt"), "w").write(
            "\n".join("%d\t%s" % (c, n) for n, c in names.most_common()))
        n_fields = len(names)

    print("source files : %d" % len(srcs))
    print("classes      : %d" % len(cls))
    print("functions    : %d" % len(rows))
    print("gon fields   : %s" % (n_fields if getfield else "skipped (pass --getfield)"))
    print("written to   : %s" % os.path.abspath(out_dir))


if __name__ == "__main__":
    main()
