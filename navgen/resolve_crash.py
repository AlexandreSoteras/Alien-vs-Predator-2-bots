"""Resolve object.lto offsets from avp2bots_crash.txt to function names using the linker map.

Usage: py -I resolve_crash.py <object.map> <avp2bots_crash.txt> [--last]
Every "object.lto+0xOFFSET" in the report is annotated with the nearest preceding public symbol
(undecorated with undname.exe from Visual Studio when it can be found).
"""
import glob
import os
import re
import subprocess
import sys

PREFERRED_BASE = 0x10000000


def load_map(path):
    syms = []
    in_publics = False
    for line in open(path, encoding="latin-1"):
        if "Publics by Value" in line:
            in_publics = True
            continue
        if in_publics and line.strip().startswith("entry point"):
            break
        if not in_publics:
            continue
        parts = line.split()
        if len(parts) >= 4 and re.match(r"^[0-9a-fA-F]{4}:[0-9a-fA-F]{8}$", parts[0]):
            try:
                addr = int(parts[2], 16)
            except ValueError:
                continue
            obj = parts[-1]
            syms.append((addr, parts[1], obj))
    syms.sort()
    return syms


def find_undname():
    pats = [r"C:\Program Files\Microsoft Visual Studio\2022\*\VC\Tools\MSVC\*\bin\Hostx64\x86\undname.exe",
            r"C:\Program Files\Microsoft Visual Studio\2022\*\VC\Tools\MSVC\*\bin\Hostx86\x86\undname.exe"]
    for p in pats:
        hits = glob.glob(p)
        if hits:
            return hits[-1]
    return None


def undecorate(names, tool):
    if not tool or not names:
        return {n: n for n in names}
    out = {}
    for n in names:
        try:
            r = subprocess.run([tool, n], capture_output=True, text=True, timeout=10)
            m = re.search(r'is :- "(.*)"', r.stdout)
            out[n] = m.group(1) if m else n
        except Exception:
            out[n] = n
    return out


def resolve(syms, offset):
    addr = PREFERRED_BASE + offset
    lo, hi = 0, len(syms)
    while lo < hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= addr:
            lo = mid + 1
        else:
            hi = mid
    if lo == 0:
        return None
    a, name, obj = syms[lo - 1]
    return name, addr - a, obj


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    syms = load_map(sys.argv[1])
    text = open(sys.argv[2], encoding="latin-1").read()
    if "--last" in sys.argv:
        idx = text.rfind("=== exception")
        text = text[idx:] if idx >= 0 else text
    offsets = re.findall(r"object\.lto\+0x([0-9A-Fa-f]+)", text)
    hits = {}
    for off in offsets:
        r = resolve(syms, int(off, 16))
        if r:
            hits[off] = r
    names = undecorate(sorted({h[0] for h in hits.values()}), find_undname())

    def annotate(m):
        off = m.group(1)
        if off not in hits:
            return m.group(0)
        name, delta, obj = hits[off]
        return f"{m.group(0)}  {names.get(name, name)} +0x{delta:X}  [{obj}]"

    print(re.sub(r"object\.lto\+0x([0-9A-Fa-f]+)", annotate, text))


if __name__ == "__main__":
    main()
