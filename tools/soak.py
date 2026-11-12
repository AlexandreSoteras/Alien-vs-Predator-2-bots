"""Endurance test: one LAN dedicated server per game type, a map rotation, for a long time.

Usage: py -I tools/soak.py --minutes 60 --maptime 3 [--gametypes dm,tdm] [--maps m1,m2,...] [--logmax 1]
Each server (farm/s<N>/, ports 27900+N) runs the whole map list in rotation, TimeLimit = --maptime minutes.
Every minute: alive?, memory (working set), number of map loads so far. At the end: farm/soak_<gametype>.md
with the memory trend, map loads, crashes, broken/frozen bots, stuck reports and the log rotation (.1 file).
"""
import argparse
import csv
import io
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import farm  # noqa: E402  (setup, query, paths)

DEFAULT_MAPS = ("dm_compound,dm_bunker,dm_outpost4,dm_reservoir,dm_quarantine,dm_leadworks,dm_dropzone,"
                "dm_fury151,dm_stronghold,dm_verloc,dm_alley,dm_colony")


def memory_kb(pid):
    out = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    for row in csv.reader(io.StringIO(out)):
        if len(row) >= 5 and row[1] == str(pid):
            return int(re.sub(r"[^\d]", "", row[4]) or 0)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=int, default=60)
    ap.add_argument("--maptime", type=int, default=3, help="TimeLimit per map, minutes")
    ap.add_argument("--gametypes", default="dm,tdm")
    ap.add_argument("--maps", default=DEFAULT_MAPS)
    ap.add_argument("--bots", default="marines=4 aliens=4 predators=4 respawn=4 log=1 debug=0 classes=1 ladders=1 cloak=1")
    ap.add_argument("--logmax", type=float, default=1.0, help="bots.cfg logmax in MB (small, to see the rotation)")
    ap.add_argument("--lto", default=os.path.join(farm.ROOT, "server", "out", "object.lto"))
    a = ap.parse_args()
    maps = [m.strip().lower() for m in a.maps.split(",") if m.strip()]
    gts = [g.strip() for g in a.gametypes.split(",") if g.strip()]

    servers = []
    for slot, gt in enumerate(gts):
        bots = a.bots + f" logmax={a.logmax:g}"
        d, port = farm.setup(slot, maps[0], bots, a.lto, gt, 0 if gt == "tdm" else None, a.maptime)
        # the whole rotation and every map's waypoints
        cfgp = os.path.join(d, "ServerData", "farm.txt")
        cfg = open(cfgp, encoding="latin-1", newline="").read()
        first = f'Map00 = "worlds\\multi\\dm\\{maps[0]}"'
        header = "[" + farm.GAME_TYPES[gt][0] + "]"
        at = cfg.index(header)
        at = cfg.index(first, at)  # the line farm.setup wrote in this game type's section
        rotation = "\r\n".join(f'Map{i:02d} = "worlds\\multi\\dm\\{m}"' for i, m in enumerate(maps))
        cfg = cfg[:at] + rotation + cfg[at + len(first):]
        open(cfgp, "w", encoding="latin-1", newline="").write(cfg)
        for m in maps:
            shutil.copyfile(os.path.join(farm.NAV, m + ".nav"), os.path.join(d, "botnav", m + ".nav"))
        for f in ("avp2bots_bots.log.1",):
            if os.path.exists(os.path.join(d, f)):
                os.remove(os.path.join(d, f))
        p = subprocess.Popen([os.path.join(d, "AvP2Serv.exe"), "-config", "farm"], cwd=d)
        servers.append(dict(gt=gt, d=d, port=port, p=p, samples=[]))
        print(f"[{time.strftime('%H:%M:%S')}] {gt}: port {port}, pid {p.pid}, {len(maps)} maps x {a.maptime} min", flush=True)

    start = time.time()
    while time.time() - start < a.minutes * 60:
        time.sleep(60)
        for s in servers:
            log = os.path.join(s["d"], "avp2bots_bots.log")
            alive = s["p"].poll() is None
            mem = memory_kb(s["p"].pid) if alive else 0
            s["samples"].append((round((time.time() - start) / 60), alive, mem))
            print(f"[{time.strftime('%H:%M:%S')}] {s['gt']}: alive={alive} mem={mem / 1024:.0f} MB "
                  f"log={os.path.getsize(log) / 1048576 if os.path.exists(log) else 0:.2f} MB", flush=True)

    for s in servers:
        alive = s["p"].poll() is None
        if alive:
            subprocess.run(["taskkill", "/F", "/PID", str(s["p"].pid)], capture_output=True)
        s["p"].wait(timeout=30)
        d = s["d"]
        text = ""
        for f in ("avp2bots_bots.log.1", "avp2bots_bots.log"):
            if os.path.exists(os.path.join(d, f)):
                text += open(os.path.join(d, f), encoding="latin-1").read()
        crash = os.path.join(d, "avp2bots_crash.txt")
        crashes = open(crash, encoding="latin-1").read().count("=== exception") if os.path.exists(crash) else 0
        worlds = re.findall(r" WORLD map=(\S+)", text)
        mems = [m for _, ok, m in s["samples"] if ok and m]
        out = os.path.join(farm.FARM, f"soak_{s['gt']}.md")
        with open(out, "w", encoding="utf-8") as f:
            f.write(f"# Soak {s['gt']}: {a.minutes} min, {len(maps)} maps x {a.maptime} min, logmax {a.logmax} MB\n\n")
            f.write(f"- alive at the end: {alive}; crashes: {crashes}\n")
            f.write(f"- map loads seen in the kept logs: {len(worlds)} ({', '.join(worlds[-12:])})\n")
            f.write(f"- log rotated (.1 exists): {os.path.exists(os.path.join(d, 'avp2bots_bots.log.1'))}; "
                    f"log sizes: {[round(os.path.getsize(os.path.join(d, x)) / 1048576, 2) for x in ('avp2bots_bots.log', 'avp2bots_bots.log.1') if os.path.exists(os.path.join(d, x))]} MB\n")
            if mems:
                f.write(f"- memory (MB): first {mems[0] / 1024:.0f}, max {max(mems) / 1024:.0f}, last {mems[-1] / 1024:.0f}\n")
            f.write(f"- broken {len(re.findall(' BROKEN ', text))}, frozen {len(re.findall(' FROZEN ', text))}, "
                    f"stuck {len(re.findall(' STUCK ', text))}, unstick {len(re.findall(' UNSTICK ', text))}, "
                    f"learnblock {len(re.findall(' LEARNBLOCK ', text))}, spawnfail {len(re.findall(' SPAWNFAIL ', text))}\n")
            f.write("\n| minute | alive | memory MB |\n|---|---|---|\n")
            for t, ok, m in s["samples"]:
                f.write(f"| {t} | {ok} | {m / 1024:.0f} |\n")
        if crashes:
            shutil.copyfile(crash, os.path.join(farm.FARM, f"soak_{s['gt']}_crash.txt"))
        print("wrote", out, flush=True)


if __name__ == "__main__":
    main()
