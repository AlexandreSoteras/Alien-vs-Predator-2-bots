"""Alien lifecycle statistics from bot logs (farm runs with --lifecycle 1, or the test server's log).

Usage: py -I tools\\lifecycle_stats.py <run folder or log>... [--compare <run folder or log>...]

Per stage (Facehugger, Chestburster, adult classes): lives, deaths, median life, kills; then the cycle:
hugs that led to a chestburst (engine path "bursts out of a <host>" vs the 30 s fallback "bursting here"),
chestbursters that grew, and the time from a facehugger's spawn to its adult. Only logs where facehuggers
spawned count (lifecycle on).
"""
import collections
import glob
import os
import re
import statistics
import sys


def logs_of(args):
    out = []
    for a in args:
        if os.path.isdir(a):
            out += sorted(glob.glob(os.path.join(a, "r*", "*.log"))) or sorted(glob.glob(os.path.join(a, "*.log")))
        else:
            out.append(a)
    return out


def analyse(paths):
    lives = collections.Counter()
    deaths = collections.Counter()
    kills = collections.Counter()
    life = collections.defaultdict(list)
    ev = collections.Counter()
    to_adult = []
    hug_kills = 0
    used = 0
    for p in paths:
        cls = {}
        hug_start = {}     # bot slot prefix -> time its facehugger life began
        lines = open(p, encoding="latin-1").read().splitlines()
        if not any("class=Facehugger" in l for l in lines):
            continue
        used += 1
        for line in lines:
            parts = line.split()
            if len(parts) < 3:
                continue
            try:
                t = float(parts[0])
            except ValueError:
                continue
            m = re.search(r" SPAWN (\S+) species=(\w+) class=(\w+)", line)
            if m:
                name, c = m.group(1), m.group(3)
                cls[name] = c
                lives[c] += 1
                slot = name.rsplit("_", 1)[0]
                if c == "Facehugger":
                    hug_start[slot] = t
                elif c not in ("Chestburster",) and m.group(2) == "Alien" and slot in hug_start:
                    to_adult.append(t - hug_start.pop(slot))
                continue
            m = re.search(r" DEATH (\S+) killer=(\S+) lifetime=([\d.]+)", line)
            if m:
                v = cls.get(m.group(1), "?")
                deaths[v] += 1
                life[v].append(float(m.group(3)))
                k = cls.get(m.group(2))
                if k:
                    kills[k] += 1
                    if k == "Facehugger":
                        hug_kills += 1
                slot = m.group(1).rsplit("_", 1)[0]
                if v in ("Facehugger", "Chestburster"):
                    hug_start.pop(slot, None)
                continue
            if " LIFECYCLE " in line:
                if "no chestburst after" in line:
                    ev["burst: 30 s fallback"] += 1
                elif "bursts out of a body" in line:
                    pass  # the line after the fallback
                elif "bursts out of a" in line:
                    ev["burst: engine (host known)"] += 1
                elif "grows into" in line:
                    ev["grew to adult"] += 1
            if " UNSTICK " in line and "facehug failed" in line:
                ev["hug failed (victim survived)"] += 1
    return used, lives, deaths, kills, life, ev, to_adult, hug_kills


def report(title, r):
    used, lives, deaths, kills, life, ev, to_adult, hug_kills = r
    print(f"== {title}: {used} logs with the lifecycle on")
    print(f"{'stage/class':14s} {'lives':>6s} {'deaths':>6s} {'kills':>6s} {'med life':>9s}")
    order = ["Facehugger", "Chestburster", "Drone", "Runner", "Predalien"]
    for c in order + sorted(k for k in lives if k not in order):
        if not lives[c] and not deaths[c]:
            continue
        l = life[c]
        med = statistics.median(l) if l else 0.0
        print(f"{c:14s} {lives[c]:6d} {deaths[c]:6d} {kills[c]:6d} {med:8.1f}s")
    fh = lives["Facehugger"]
    bursts = ev["burst: 30 s fallback"] + ev["burst: engine (host known)"]
    print(f"hugs that burst: {bursts} of {fh} facehugger lives ({100.0 * bursts / max(1, fh):.0f} %), "
          f"engine {ev['burst: engine (host known)']}, fallback {ev['burst: 30 s fallback']}; "
          f"failed hugs {ev['hug failed (victim survived)']}; grew to adult {ev['grew to adult']}")
    if to_adult:
        print(f"facehugger spawn -> adult: n={len(to_adult)} median {statistics.median(to_adult):.1f}s")


def main():
    args = sys.argv[1:]
    if "--compare" in args:
        i = args.index("--compare")
        report("A", analyse(logs_of(args[:i])))
        report("B", analyse(logs_of(args[i + 1:])))
    else:
        report("run", analyse(logs_of(args)))


if __name__ == "__main__":
    main()
