"""Generate bot navigation for every multiplayer map of a game type, from the version the server loads.

For each map the newest archive wins (later patches override earlier ones), exactly like the server's
resource load order. Each map is extracted, dumped (dump_world.py) and turned into a waypoint graph
(navbuild.py); a markdown report summarises the checks.

Usage: py -I build_all.py [--type dm] [--only REGEX] [--timeout 900]
Outputs: ../maps/<MAP>.DAT, ../maps/out/*, ../nav/<map>.nav + <map>_nav.png, ../docs/nav_report_<type>.md
"""
import argparse
import concurrent.futures
import os
import re
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
GAME = os.path.normpath(os.path.join(ROOT, ".."))
sys.path.insert(0, HERE)
from rez import walk  # noqa: E402

# lowest to highest priority: a later archive overrides an earlier one
ARCHIVES = ["AVP2.REZ", "MULTI.REZ", "AVP2P.REZ", "AVP2P1.REZ", "AVP2P5.REZ"]


def entries(path):
    with open(path, "rb") as f:
        head = f.read(256)
        base = head.index(b"\x1a") + 1
        _ver, root_pos, root_size = struct.unpack_from("<III", head, base)
        out = []
        walk(f, root_pos, root_size, [], out)
    return out


def collect(game_type):
    maps = {}
    rx = re.compile(r"^worlds\\multi\\" + re.escape(game_type) + r"\\([^\\]+)\.dat$", re.I)
    for arc in ARCHIVES:
        path = os.path.join(GAME, arc)
        if not os.path.exists(path):
            continue
        for name, pos, size in entries(path):
            m = rx.match(name)
            if m:
                maps[m.group(1).lower()] = (arc, name, pos, size)
    return maps


def run(cmd, timeout):
    t0 = time.time()
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout + r.stderr, time.time() - t0
    except subprocess.TimeoutExpired:
        return -1, "TIMEOUT", time.time() - t0


def grab(pattern, text, cast=str, default=None):
    m = re.search(pattern, text)
    return cast(m.group(1)) if m else default


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--type", default="dm")
    ap.add_argument("--only", default=None)
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--skip-dump", action="store_true", help="reuse maps/out exports (only rebuild the nav)")
    a = ap.parse_args()

    maps = collect(a.type)
    names = sorted(n for n in maps if not a.only or re.search(a.only, n, re.I))
    print(f"{len(names)} {a.type} maps")
    def one(n):
        arc, inner, pos, size = maps[n]
        dat = os.path.join(ROOT, "maps", inner.split("\\")[-1])
        with open(os.path.join(GAME, arc), "rb") as f:
            f.seek(pos)
            data = f.read(size)
        with open(dat, "wb") as o:
            o.write(data)
        base = os.path.splitext(os.path.basename(dat))[0]
        obj0 = os.path.join(ROOT, "maps", "out", base + ".obj")
        if a.skip_dump and os.path.exists(obj0):
            rc, out, t_dump = 0, "", 0.0
        else:
            rc, out, t_dump = run([sys.executable, "-I", os.path.join(HERE, "dump_world.py"), dat], a.timeout)
        obj = os.path.join(ROOT, "maps", "out", base + ".obj")
        js = os.path.join(ROOT, "maps", "out", base + ".objects.json")
        if rc != 0 or not os.path.exists(obj):
            print(f"  {n}: dump failed", flush=True)
            return dict(map=n, archive=arc, status="dump failed")
        # navbuild names its outputs after the OBJ; write them as lower-case <map>.nav like the server expects
        rc, out, t_nav = run([sys.executable, "-I", os.path.join(HERE, "navbuild.py"), obj, "--objects", js,
                              "--out", os.path.join(ROOT, "nav")], a.timeout)
        row = dict(map=n, archive=arc, size_mb=round(size / 1e6, 1), t=round(t_dump + t_nav))
        if rc != 0:
            row.update(status="nav failed", note=out.strip().splitlines()[-1][:80] if out.strip() else "")
        else:
            row.update(
                tris=grab(r": (\d+) triangles", out, int),
                starts=grab(r"start points projected onto the nav: (\d+/\d+)", out),
                bad_pairs=grab(r"spawn-to-spawn pairs without a route: (\d+)", out, int),
                nodes=grab(r"final graph: (\d+) waypoints", out, int),
                traps=grab(r"traps dropped: (\d+)", out, int),
                ladders=len(re.findall(r"^ladder .*->", out, re.M)),
                ladders_total=len(re.findall(r"^ladder ", out, re.M)),
                inside=grab(r"spawns inside (\d+/\d+)", out),
                platforms=grab(r"spawn platforms (\d+)", out, int),
                stranded=grab(r"stranded spawns (\d+)", out, int),
                main_nodes=grab(r"main area: (\d+) waypoints", out, int),
            )
            # ok = every spawn is projected and leads into the main play area (spawn platforms you drop from
            # are normal); 'check' = some bots would spawn in a part of the map they cannot leave
            all_starts = row["starts"] and row["starts"].split("/")[0] == row["starts"].split("/")[1]
            row["status"] = "ok" if all_starts and row["stranded"] == 0 else "check"
        print(f"  {n}: {row.get('status')} starts={row.get('starts')} inside={row.get('inside')} "
              f"stranded={row.get('stranded')} badpairs={row.get('bad_pairs')} "
              f"nodes={row.get('nodes')} ladders={row.get('ladders')}/{row.get('ladders_total')} {row.get('t')}s", flush=True)
        return row

    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        rows = list(ex.map(one, names))

    report = os.path.join(ROOT, "docs", f"nav_report_{a.type}.md")
    with open(report, "w", encoding="utf-8") as f:
        f.write(f"# Navigation report: {a.type} maps\n\n")
        f.write("Generated by navgen/build_all.py.\n\n"
                "- `starts`: multiplayer start points projected onto the graph.\n"
                "- `main area`: waypoints of the largest part of the map where every point reaches every other "
                "(the part holding the most spawns), and the spawns inside it.\n"
                "- `platforms`: spawns outside the main area that lead into it (you drop or jump down; normal in DM).\n"
                "- `stranded`: spawns that cannot reach the main area; bots spawned there stay cut off. "
                "A map is `ok` when every spawn is projected and none is stranded.\n"
                "- `bad pairs`: ordered spawn pairs with no route (includes platform spawns you cannot walk back to).\n"
                "- `traps`: waypoints you can reach but not leave (dropped).\n\n")
        f.write("| map | archive | status | starts | main area | platforms | stranded | bad pairs | waypoints | traps | "
                "ladders linked | triangles | time (s) |\n")
        f.write("|---|---|---|---|---|---|---|---|---|---|---|---|---|\n")
        for r in rows:
            f.write(f"| {r['map']} | {r['archive']} | {r['status']} | {r.get('starts', '')} | "
                    f"{r.get('main_nodes', '')} wp, {r.get('inside', '')} spawns | {r.get('platforms', '')} | "
                    f"{r.get('stranded', '')} | {r.get('bad_pairs', '')} | "
                    f"{r.get('nodes', '')} | {r.get('traps', '')} | {r.get('ladders', '')}/{r.get('ladders_total', '')} | "
                    f"{r.get('tris', '')} | {r.get('t', '')} |\n")
    print("wrote", report)


if __name__ == "__main__":
    main()
