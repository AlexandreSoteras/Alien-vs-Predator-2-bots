"""List world (.DAT) files inside LithTech REZ archives and count AI-related objects in each.
Usage: py -I rez_ai_scan.py <rez> [<rez> ...]
"""
import re
import struct
import sys

AI_PATTERNS = {
    "AIVolume": re.compile(rb"AIVolume"),
    "AINode*": re.compile(rb"AINode[A-Za-z]*"),
    "AIAlien": re.compile(rb"AIAlien"),
    "AIMarine": re.compile(rb"AIMarine"),
    "AIPredator": re.compile(rb"AIPredator"),
    "AICorporate": re.compile(rb"AICorporate"),
    "Spawner": re.compile(rb"(?:Random|Multi)?Spawner"),
}


def read_cstr(buf, off):
    end = buf.index(b"\0", off)
    return buf[off:end].decode("latin-1"), end + 1


def walk(f, pos, size, path, out):
    f.seek(pos)
    buf = f.read(size)
    off = 0
    while off < len(buf):
        (etype,) = struct.unpack_from("<I", buf, off)
        off += 4
        if etype == 1:  # directory
            dpos, dsize, _t = struct.unpack_from("<III", buf, off)
            off += 12
            name, off = read_cstr(buf, off)
            walk(f, dpos, dsize, path + [name], out)
        elif etype == 0:  # resource
            rpos, rsize, _t, _rid = struct.unpack_from("<IIII", buf, off)
            off += 16
            ext = buf[off:off + 4][::-1].strip(b"\0").decode("latin-1")
            off += 4
            (nkeys,) = struct.unpack_from("<I", buf, off)
            off += 4
            name, off = read_cstr(buf, off)
            _desc, off = read_cstr(buf, off)
            off += 4 * nkeys
            out.append(("\\".join(path + [name]) + "." + ext, rpos, rsize))
        else:
            raise ValueError(f"unknown entry type {etype} at {pos + off - 4}")


def main():
    for rez in sys.argv[1:]:
        with open(rez, "rb") as f:
            head = f.read(256)
            base = head.index(b"\x1a") + 1
            _ver, root_pos, root_size = struct.unpack_from("<III", head, base)
            files = []
            walk(f, root_pos, root_size, [], files)
            worlds = [x for x in files if x[0].upper().endswith(".DAT") and "WORLDS" in x[0].upper()]
            print(f"== {rez}: {len(files)} files, {len(worlds)} worlds")
            for name, rpos, rsize in sorted(worlds):
                f.seek(rpos)
                data = f.read(rsize)
                counts = {k: len(p.findall(data)) for k, p in AI_PATTERNS.items()}
                if any(counts.values()):
                    print(f"  {name}: " + ", ".join(f"{k}={v}" for k, v in counts.items() if v))
            no_ai = [n for n, *_ in worlds]
            print(f"  (worlds scanned: {', '.join(sorted(n.split(chr(92))[-1] for n in no_ai))})")


if __name__ == "__main__":
    main()
