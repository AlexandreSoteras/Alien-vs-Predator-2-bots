"""Survivor statistics from bot logs: rounds, how they end, mutations, who kills whom, survival times.

Usage: py -I tools/survivor_stats.py farm/runs/<tag> [--round <minutes>] [--compare farm/runs/<other>]

A round starts at "SURVIVOR new round" (the first one at the world start). It ends by elimination when the next
round starts more than 20 s before its time limit (+ the 15 s interlude), by time otherwise. Survivors are the bots
of the survivor race at the time (Marine by default); a "mutates" line moves a bot to the mutants.
"""
import argparse
import glob
import os
import random
import re
import statistics

RX_TIME = re.compile(r"^\s*([0-9.]+) ")
RX_DEATH = re.compile(r"DEATH avp2bot_([A-Za-z]+)\d+_\d+ killer=(\S+) lifetime=([0-9.]+)")
RX_KILLER = re.compile(r"avp2bot_([A-Za-z]+)\d+_\d+")


def parse(path, round_s):
    rounds = []          # dicts: start, end, mutations, deaths
    cur = {"start": 0.0, "mut": 0, "deaths": 0, "skill": 0, "mkill": 0}
    stuck = {"round": 0, "interlude": 0}
    surv_life = []
    last_t = 0.0
    starts = []
    for line in open(path, errors="replace"):
        m = RX_TIME.match(line)
        if not m:
            continue
        t = float(m.group(1))
        last_t = t
        if " SURVIVOR new round" in line:
            cur["end"] = t
            rounds.append(cur)
            starts.append(t)
            cur = {"start": t, "mut": 0, "deaths": 0, "skill": 0, "mkill": 0}
        elif " mutates" in line and " SURVIVOR " in line:
            cur["mut"] += 1
        elif " DEATH " in line:
            d = RX_DEATH.search(line)
            if not d:
                continue
            cur["deaths"] += 1
            victim, killer, life = d.group(1), d.group(2), float(d.group(3))
            k = RX_KILLER.match(killer)
            kspec = k.group(1) if k else ("player" if killer.lower().startswith("player") else killer)
            if victim == "Marine":
                surv_life.append(life)
                if kspec == "Alien":
                    cur["mkill"] += 1
            elif victim == "Alien" and kspec == "Marine":
                cur["skill"] += 1
        elif " STUCK " in line:
            # interlude: the last 15 s before a round start are counted after the fact below
            stuck.setdefault("times", []).append(t)
    cur["end"] = last_t
    cur["partial"] = True
    rounds.append(cur)
    for t in stuck.get("times", []):
        inter = any(s - 15.5 <= t <= s for s in starts)
        stuck["interlude" if inter else "round"] += 1
    out = []
    for r in rounds:
        dur = r["end"] - r["start"]
        if r.get("partial"):
            r["how"] = "partial"
        elif r["start"] == 0.0 and not out:
            # first round: no "new round" line at the world start; its start is 0
            r["how"] = "elim" if dur < round_s + 15 - 20 else "time"
        else:
            r["how"] = "elim" if dur < round_s + 15 - 20 else "time"
        r["dur"] = dur
        out.append(r)
    return out, stuck, surv_life


def summary(run, round_s):
    logs = sorted(glob.glob(os.path.join(run, "r*", "*.log")))
    allr, st, life = [], {"round": 0, "interlude": 0}, []
    for p in logs:
        r, s, l = parse(p, round_s)
        allr += [x for x in r if x["how"] != "partial"]
        st["round"] += s["round"]
        st["interlude"] += s["interlude"]
        life += l
    return logs, allr, st, life


def show(name, logs, rounds, st, life):
    n = len(rounds)
    elim = sum(1 for r in rounds if r["how"] == "elim")
    print(f"== {name}: {len(logs)} logs, {n} full rounds")
    if not n:
        return
    print(f"  rounds ended by elimination: {elim}/{n} ({100.0 * elim / n:.0f} %)")
    print(f"  round length: median {statistics.median(r['dur'] for r in rounds):.0f} s (with the 15 s interlude)")
    print(f"  mutations per round: mean {statistics.mean(r['mut'] for r in rounds):.1f}")
    print(f"  per round: mutants kill survivors {statistics.mean(r['mkill'] for r in rounds):.1f}, "
          f"survivors kill mutants {statistics.mean(r['skill'] for r in rounds):.1f}")
    if life:
        print(f"  survivor bot lifetime: median {statistics.median(life):.1f} s")
    print(f"  STUCK reports: {st['round']} in rounds, {st['interlude']} in interludes "
          f"({(st['round'] + st['interlude']) / max(1, len(logs)):.1f} per log)")


def boot(a, b, key, n=2000):
    da = []
    for _ in range(n):
        sa = [random.choice(a) for _ in a]
        sb = [random.choice(b) for _ in b]
        da.append(statistics.mean(key(x) for x in sa) - statistics.mean(key(x) for x in sb))
    da.sort()
    return da[int(0.025 * n)], da[int(0.975 * n)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--round", type=float, default=1.0, help="round time limit in minutes")
    ap.add_argument("--compare")
    a = ap.parse_args()
    rs = a.round * 60.0
    la, ra, sa, lifea = summary(a.run, rs)
    show(a.run, la, ra, sa, lifea)
    if a.compare:
        lb, rb, sb, lifeb = summary(a.compare, rs)
        show(a.compare, lb, rb, sb, lifeb)
        if ra and rb:
            for label, key in (("elimination share", lambda r: 1.0 if r["how"] == "elim" else 0.0),
                               ("mutations/round", lambda r: r["mut"]),
                               ("mutant kills/round", lambda r: r["mkill"]),
                               ("survivor kills/round", lambda r: r["skill"])):
                lo, hi = boot(ra, rb, key)
                d = statistics.mean(key(r) for r in ra) - statistics.mean(key(r) for r in rb)
                print(f"  A-B {label}: {d:+.2f} [{lo:+.2f} {hi:+.2f}]{' *' if lo > 0 or hi < 0 else ''}")


if __name__ == "__main__":
    main()
