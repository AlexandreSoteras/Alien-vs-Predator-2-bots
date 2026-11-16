"""Evac round outcomes from bot logs (farm runs): py -I tools/evac_stats.py farm/runs/<tag> [...]

A round runs from one "EVAC round start" line to the next (or to the end of the world). Its outcome:
  evac      the defenders evacuated (EVACSTATE complete)
  defwiped  every defender bot ran out of lives
  attwiped  every attacker bot ran out of lives
  time      none of these (the time limit, or a round cut by the end of the run: the last one is not counted)
Bot-only runs: the teams are the bots (EVAC round start gives their numbers).
"""
import collections
import glob
import os
import re
import sys

RX_START = re.compile(r"^\s*([\d.]+) EVAC round start: (\d+) defender bots \((\d+) lives\), (\d+) attacker bots")
RX_KILLED = re.compile(r"^\s*([\d.]+) EVAC (avp2bot_\S+) killed by (\S+), (\d+) lives left")
RX_WORLD = re.compile(r"^\s*[\d.]+ WORLD map=(\S+)")


def rounds_of(path):
    out = []
    cur = None
    mapname = "?"
    for line in open(path, encoding="cp1252", errors="replace"):
        m = RX_WORLD.match(line)
        if m:
            # the round limit's last round ends the world: count it when it was decided, not a round cut short
            if cur and outcome(cur) != "time":
                out.append(cur)
            cur = None
            mapname = m.group(1)
            continue
        m = RX_START.match(line)
        if m:
            if cur:
                out.append(cur)
            cur = {"map": mapname, "t0": float(m.group(1)), "t1": float(m.group(1)), "def": int(m.group(2)),
                   "att": int(m.group(4)), "out": set(), "evac": False, "kills": 0}
            continue
        if not cur:
            continue
        if "EVACSTATE complete" in line:
            cur["evac"] = True
        m = RX_KILLED.match(line)
        if m:
            cur["kills"] += 1
            cur["t1"] = float(m.group(1))
            if int(m.group(4)) == 0:
                cur["out"].add(m.group(2).rsplit("_", 1)[0])
    return out  # the last round (cut by the end of the run) is dropped


def outcome(r):
    if r["evac"]:
        return "evac"
    dead_def = sum(1 for n in r["out"] if n.startswith("avp2bot_Marine"))
    dead_att = sum(1 for n in r["out"] if n.startswith("avp2bot_Alien"))
    if r["def"] and dead_def >= r["def"]:
        return "defwiped"
    if r["att"] and dead_att >= r["att"]:
        return "attwiped"
    return "time"


def main():
    per = collections.defaultdict(collections.Counter)
    for arg in sys.argv[1:]:
        for f in sorted(glob.glob(os.path.join(arg, "r*", "*.log"))):
            for r in rounds_of(f):
                per[r["map"]][outcome(r)] += 1
    print(f"{'map':16s} rounds   evac defwiped attwiped  time   defenders win")
    for m, c in sorted(per.items()):
        n = sum(c.values())
        win = c["evac"] + c["attwiped"]
        print(f"{m:16s} {n:6d} {c['evac']:7d} {c['defwiped']:8d} {c['attwiped']:8d} {c['time']:5d}   {100 * win / n:5.0f} %")


if __name__ == "__main__":
    main()
