"""Balance figures from bot logs, with confidence intervals.

Usage:
  py -I tools/balance.py farm/runs/<tag>                      (every .log below the folder)
  py -I tools/balance.py farm/runs/<tag> --compare farm/runs/<other>
  py -I tools/balance.py farm/dm_compound.log farm/dm_bunker.log

Each log (one map, one repetition) is a sampling unit: the 95 % intervals come from a bootstrap over logs
(resampling whole logs), so they include the map-to-map and run-to-run spread.

K/D cannot average 1: deaths include suicides and world deaths (alien acid, falls), kills do not. "norm" is
the group's K/D divided by the K/D of all bots together (1.00 = average).
"Early" is the share of deaths within 5 s of spawning; "kpm" is kills per minute alive.
"""
import argparse
import collections
import glob
import os
import random
import re
import statistics

SPAWN = re.compile(r" SPAWN (\S+) species=(\S+) class=(\S+)")
DEATH = re.compile(r" DEATH (\S+) killer=(\S+) lifetime=([\d.]+)(?: weapon=(\S+))?")
SPECIES = ("Marine", "Alien", "Predator")
# navigation and robustness events, per log (PATHFAIL lines carry "(+N not logged)")
EVENT = re.compile(r" (STUCK|UNSTICK|LEARNBLOCK|FROZEN|BROKEN|PATHFAIL|OFFGRAPH) [^\n]*?(?:\(\+(\d+) not logged\))?$")
EVENTS = ("STUCK", "UNSTICK", "LEARNBLOCK", "PATHFAIL", "OFFGRAPH", "FROZEN", "BROKEN")


def expand(paths):
    files = []
    for a in paths:
        if os.path.isdir(a):
            files += sorted(glob.glob(os.path.join(a, "**", "*.log"), recursive=True))
        else:
            files += sorted(glob.glob(a))
    return files


def read_unit(path):
    """Per-group counters for one log: kills, deaths, early deaths, lifetimes, weapons, world deaths."""
    u = dict(k=collections.Counter(), d=collections.Counter(), early=collections.Counter(),
             life=collections.defaultdict(list), weapons=collections.Counter(), world=0, total_d=0, total_k=0,
             events=collections.Counter())
    cls = {}
    for line in open(path, encoding="latin-1"):
        m = EVENT.search(line)
        if m:
            u["events"][m.group(1)] += 1 + int(m.group(2) or 0)
        m = SPAWN.search(line)
        if m:
            cls[m.group(1)] = (m.group(2), m.group(3))
            continue
        m = DEATH.search(line)
        if not m:
            continue
        victim, killer, lt, weapon = m.groups()
        lt = float(lt)
        vsp, vcl = cls.get(victim, ("?", "?"))
        u["total_d"] += 1
        for g in (vsp, vcl):
            u["d"][g] += 1
            u["life"][g].append(lt)
            if lt < 5.0:
                u["early"][g] += 1
        if killer.startswith("avp2bot_") and killer != victim:
            ksp, kcl = cls.get(killer, ("?", "?"))
            u["k"][ksp] += 1
            u["k"][kcl] += 1
            u["total_k"] += 1
            if weapon and weapon != "-":
                u["weapons"][(kcl, weapon)] += 1
        elif not killer.startswith("avp2bot_"):
            u["world"] += 1
    return u


def stats(units, group):
    k = sum(u["k"][group] for u in units)
    d = sum(u["d"][group] for u in units)
    tk = sum(u["total_k"] for u in units)
    td = sum(u["total_d"] for u in units)
    life = [x for u in units for x in u["life"][group]]
    kd = k / d if d else 0.0
    mean_kd = tk / td if td else 1.0
    return dict(k=k, d=d, kd=kd, norm=kd / mean_kd if mean_kd else 0.0,
                life=statistics.median(life) if life else 0.0,
                early=sum(u["early"][group] for u in units) / d if d else 0.0,
                kpm=k / sum(life) * 60.0 if life else 0.0)


def bootstrap(units, group, key, n=1000, seed=1):
    rnd = random.Random(seed)
    vals = sorted(stats([rnd.choice(units) for _ in units], group)[key] for _ in range(n))
    return vals[int(0.025 * n)], vals[int(0.975 * n) - 1]


def groups_of(units):
    seen = set()
    for u in units:
        seen |= set(u["d"]) | set(u["k"])
    return list(SPECIES) + sorted(g for g in seen if g not in SPECIES and g != "?")


def report(units, label):
    td = sum(u["total_d"] for u in units)
    tk = sum(u["total_k"] for u in units)
    world = sum(u["world"] for u in units)
    print(f"== {label}: {len(units)} logs, {td} deaths, {tk} kills by bots, K/D of all bots {tk / td if td else 0:.2f}, "
          f"world/self deaths {(td - tk) / td if td else 0:.0%} (no killer bot: {world})")
    print(f"{'group':14s} {'kills':>6s} {'deaths':>6s}  {'K/D':>5s} {'norm':>5s} {'95% CI norm':>13s}  "
          f"{'life':>5s} {'early':>5s} {'kpm':>5s}")
    for g in groups_of(units):
        s = stats(units, g)
        if not s["d"] and not s["k"]:
            continue
        lo, hi = bootstrap(units, g, "norm")
        print(f"{g:14s} {s['k']:6d} {s['d']:6d}  {s['kd']:5.2f} {s['norm']:5.2f}   [{lo:4.2f}-{hi:4.2f}]  "
              f"{s['life']:4.1f}s {s['early']:5.0%} {s['kpm']:5.2f}")
        if g == SPECIES[-1]:
            print("-- classes")


def events(units, label):
    per = {e: [u["events"][e] for u in units] for e in EVENTS}
    print(f"== {label}: navigation events per log (mean over {len(units)} logs): " +
          ", ".join(f"{e.lower()} {sum(v) / len(v):.1f}" for e, v in per.items()))


def compare_events(a, b):
    rnd = random.Random(11)
    print("== A - B events per log (95 % CI)")
    for e in EVENTS:
        va = [u["events"][e] for u in a]
        vb = [u["events"][e] for u in b]
        diffs = sorted(sum(rnd.choice(va) for _ in va) / len(va) - sum(rnd.choice(vb) for _ in vb) / len(vb)
                       for _ in range(1000))
        lo, hi = diffs[25], diffs[974]
        flag = " *" if lo > 0 or hi < 0 else ""
        print(f"{e.lower():10s} {sum(va) / len(va):6.1f} {sum(vb) / len(vb):6.1f} {sum(va) / len(va) - sum(vb) / len(vb):+6.1f}  [{lo:+5.1f} {hi:+5.1f}]{flag}")


def compare(a, b):
    print("== A - B (bootstrap over the logs of each side; a change is real when its interval excludes 0)")
    print(f"{'group':14s} {'norm A':>6s} {'norm B':>6s} {'diff':>6s} {'95% CI':>15s}  {'life A-B':>8s}")
    rnd = random.Random(7)
    for g in groups_of(a + b):
        sa, sb = stats(a, g), stats(b, g)
        if not (sa["d"] and sb["d"]):
            continue
        diffs = sorted(stats([rnd.choice(a) for _ in a], g)["norm"] - stats([rnd.choice(b) for _ in b], g)["norm"]
                       for _ in range(1000))
        lo, hi = diffs[25], diffs[974]
        flag = " *" if lo > 0 or hi < 0 else ""
        print(f"{g:14s} {sa['norm']:6.2f} {sb['norm']:6.2f} {sa['norm'] - sb['norm']:+6.2f}  [{lo:+5.2f} {hi:+5.2f}]{flag}  "
              f"{sa['life'] - sb['life']:+7.1f}s")


def weapons(files, units):
    total = collections.Counter()
    for u in units:
        total.update(u["weapons"])
    print("== kills per weapon")
    for (c, w), n in total.most_common(30):
        print(f"{c:14s} {w:24s} {n}")
    # debug logs only: shots (with the target's distance) and hits on bots
    shots = collections.Counter()
    near = collections.Counter()
    hits = collections.Counter()
    dmg = collections.Counter()
    for path in files:
        for line in open(path, encoding="latin-1"):
            m = re.search(r" SHOT \S+ weapon=(\S+)(?: dist=(-?\d+))?", line)
            if m:
                shots[m.group(1)] += 1
                if m.group(2) and 0 <= int(m.group(2)) < 600:
                    near[m.group(1)] += 1
                continue
            m = re.search(r" DMG (\S+) -> (avp2bot_\S+) ([\d.]+) weapon=(\S+)", line)
            if m and not m.group(2).endswith("_dead") and m.group(1) != m.group(2):
                hits[m.group(4)] += 1
                dmg[m.group(4)] += float(m.group(3))
    if shots:
        print("== per weapon (debug logs): shots, share fired under 600 units, damage events on bots, damage per shot")
        for w in sorted(shots, key=lambda w: -dmg[w]):
            print(f"{w:24s} {shots[w]:6d} {near[w] / shots[w]:5.0%} {hits[w]:6d} {dmg[w] / shots[w]:7.1f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+", help="logs, globs or folders (farm/runs/<tag>)")
    ap.add_argument("--compare", nargs="+", help="logs, globs or folders of the other build/setting")
    ap.add_argument("--weapons", action="store_true", help="kills per weapon and debug per-weapon figures")
    a = ap.parse_args()
    files = expand(a.paths)
    units = [read_unit(f) for f in files]
    if not units:
        raise SystemExit("no logs")
    report(units, "A" if a.compare else "run")
    events(units, "A" if a.compare else "run")
    if a.compare:
        files_b = expand(a.compare)
        units_b = [read_unit(f) for f in files_b]
        report(units_b, "B")
        events(units_b, "B")
        compare(units, units_b)
        compare_events(units, units_b)
    if a.weapons:
        weapons(files, units)


if __name__ == "__main__":
    main()
