"""STUCK / UNSTICK reports per bot class, per 100 spawns of that class: py -I tools/stuck_by_class.py farm/runs/<tag> [...]

A bot's class comes from its SPAWN line (class=...); its later STUCK and UNSTICK lines are counted for that class.
"""
import collections
import glob
import os
import re
import sys

RX_SPAWN = re.compile(r" SPAWN (avp2bot_\S+) species=\S+ class=(\S+)")
RX_STUCK = re.compile(r" (STUCK|UNSTICK) (avp2bot_\S+)")


def main():
    for arg in sys.argv[1:]:
        spawns, stuck, unstick = collections.Counter(), collections.Counter(), collections.Counter()
        for f in sorted(glob.glob(os.path.join(arg, "r*", "*.log"))):
            cls = {}
            for line in open(f, encoding="cp1252", errors="replace"):
                m = RX_SPAWN.search(line)
                if m:
                    cls[m.group(1)] = m.group(2)
                    spawns[m.group(2)] += 1
                    continue
                m = RX_STUCK.search(line)
                if m and m.group(2) in cls:
                    (stuck if m.group(1) == "STUCK" else unstick)[cls[m.group(2)]] += 1
        print(f"== {arg}")
        for c in sorted(spawns, key=lambda k: -spawns[k]):
            n = spawns[c]
            print(f"  {c:12s} spawns {n:5d}  stuck {stuck[c]:4d} ({100 * stuck[c] / n:5.1f} per 100)  "
                  f"unstick {unstick[c]:3d} ({100 * unstick[c] / n:5.1f} per 100)")


if __name__ == "__main__":
    main()
