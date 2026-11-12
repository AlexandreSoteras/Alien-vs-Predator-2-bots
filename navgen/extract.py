"""Extract files from a LithTech .REZ archive.

Usage: py -I extract.py <archive.rez> <regex on the inner path> <out dir> [--list]
Example: py -I extract.py ../../MULTI.REZ "DM_Compound\\.DAT$" ../maps
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rez import walk  # noqa: E402


def entries(path):
    with open(path, "rb") as f:
        head = f.read(256)
        base = head.index(b"\x1a") + 1
        _ver, root_pos, root_size = struct.unpack_from("<III", head, base)
        out = []
        walk(f, root_pos, root_size, [], out)
    return out


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(2)
    rez, pattern, outdir = sys.argv[1], re.compile(sys.argv[2], re.I), sys.argv[3]
    only_list = "--list" in sys.argv
    matches = [e for e in entries(rez) if pattern.search(e[0])]
    if not only_list:
        os.makedirs(outdir, exist_ok=True)
    with open(rez, "rb") as f:
        for name, pos, size in matches:
            if only_list:
                print(f"{size:10d}  {name}")
                continue
            f.seek(pos)
            dest = os.path.join(outdir, name.split("\\")[-1])
            with open(dest, "wb") as o:
                o.write(f.read(size))
            print(f"extracted {name} -> {dest} ({size} bytes)")
    if not matches:
        print("no match")


if __name__ == "__main__":
    main()
