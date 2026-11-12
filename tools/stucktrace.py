"""For each STUCK report in debug logs: the bot's last PATH, its MOVE/ANIM lines just before, and the waypoint
links near its feet (to see whether drops/steps are involved).
Usage: py -I stucktrace.py <log>... (map name = log file name)"""
import collections
import math
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


def load_nav(m):
    nodes = {}
    out = collections.defaultdict(list)
    for line in open(os.path.join(ROOT, "nav", m + ".nav"), encoding="latin-1"):
        p = line.split()
        if not p:
            continue
        if p[0] == "n":
            nodes[int(p[1])] = tuple(map(float, p[2:5]))
        elif p[0] == "e":
            out[int(p[1])].append((int(p[2]), p[6] if len(p) > 6 else "walk", tuple(map(float, p[3:6]))))
    return nodes, out


for path in sys.argv[1:]:
    m = os.path.basename(path)[:-4]
    nodes, links = load_nav(m)
    lines = open(path, encoding="latin-1").read().splitlines()
    last = collections.defaultdict(lambda: collections.deque(maxlen=12))
    shown = 0
    for l in lines:
        mm = re.match(r"\s*([\d.]+) +(\w+) (?:ai=)?(avp2bot_\S+)", l.strip() and l or "")
        if not mm:
            continue
        kind, who = mm.group(2), mm.group(3)
        if kind == "STUCK" and shown < 6:
            pos = re.search(r"pos=(-?\d+) (-?\d+) (-?\d+)", l)
            x, y, z = map(float, pos.groups())
            feet = (x, y - 50, z)
            near = sorted(nodes, key=lambda i: math.dist(nodes[i], feet))[:3]
            print(f"=== {m} {l.strip()}")
            for h in last[who]:
                if any(k in h for k in (" PATH ", " MOVE ", " ANIM ", " TARGET ", " ROAM ")):
                    print("     " + h.strip()[:170])
            for i in near:
                print(f"     node {i} {nodes[i]} d={math.dist(nodes[i], feet):.0f} links={[(t, k) for t, k, _ in links[i]]}")
            shown += 1
        last[who].append(l)
