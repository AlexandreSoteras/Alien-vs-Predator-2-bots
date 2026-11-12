"""Summarise an avp2bots_bots.log and draw bot trajectories over the waypoint graph.

Usage: py -I analyze_log.py <avp2bots_bots.log> <map.nav> [out.png]

Prints: spawns and deaths per species, the kill matrix (killer species x victim species),
average lifetime, path failures, stuck reports. Writes a top-down PNG: nav graph in grey,
trajectories coloured by species, deaths as crosses, stuck reports as red rings.
"""
import collections
import re
import sys

SPECIES_COLOURS = {"Marine": (90, 170, 255), "Alien": (120, 230, 90), "Predator": (255, 170, 60)}


def species_of(name):
    m = re.match(r"avp2bot_([A-Za-z]+?)\d+_\d+", name or "")
    return m.group(1) if m else ("world" if name == "world" else "other")


def parse(path):
    ev = collections.defaultdict(list)
    tracks = collections.defaultdict(list)
    for line in open(path, encoding="latin-1"):
        parts = line.split()
        if len(parts) < 2:
            continue
        t = float(parts[0])
        kind = parts[1]
        if kind == "STATUS":
            m = re.search(r"STATUS (\S+) hp=(\S+) pos=(\S+) (\S+) (\S+) goal=(\S+) target=(\S+)", line)
            if m:
                tracks[m.group(1)].append((t, float(m.group(3)), float(m.group(4)), float(m.group(5)), m.group(6)))
        elif kind in ("SPAWN", "DEATH", "STUCK", "PATHFAIL", "SPAWNFAIL", "WORLD", "NAV"):
            ev[kind].append((t, line.strip()))
    return ev, tracks


def load_nav(path):
    nodes, edges = [], []
    for line in open(path):
        if line.startswith("n "):
            _, _, x, y, z = line.split()
            nodes.append((float(x), float(y), float(z)))
        elif line.startswith("e "):
            p = line.split()
            edges.append((int(p[1]), int(p[2]), p[6]))
    return nodes, edges


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    ev, tracks = parse(sys.argv[1])
    duration = max((t for k in ev for t, _ in ev[k]), default=0.0)
    print(f"log span: {duration:.0f} s")
    spawns = collections.Counter(species_of(re.search(r"SPAWN (\S+)", l).group(1)) for _, l in ev["SPAWN"])
    print("spawns:", dict(spawns), "| spawn failures:", len(ev["SPAWNFAIL"]))

    kills = collections.Counter()
    lifetimes = collections.defaultdict(list)
    deaths_at = []
    for t, l in ev["DEATH"]:
        m = re.search(r"DEATH (\S+) killer=(\S+) lifetime=(\S+)", l)
        victim, killer, life = m.group(1), m.group(2), float(m.group(3))
        kills[(species_of(killer), species_of(victim))] += 1
        lifetimes[species_of(victim)].append(life)
        deaths_at.append(victim)
    print(f"deaths: {len(ev['DEATH'])}")
    names = ["Marine", "Alien", "Predator"]
    print("kill matrix (rows = killer, cols = victim):")
    print("            " + "".join(f"{n:>10s}" for n in names))
    for k in names + ["world", "other"]:
        row = [kills.get((k, v), 0) for v in names]
        if any(row) or k in names:
            print(f"  {k:10s}" + "".join(f"{c:10d}" for c in row))
    for sp in names:
        if lifetimes[sp]:
            print(f"  avg lifetime {sp}: {sum(lifetimes[sp]) / len(lifetimes[sp]):.1f} s over {len(lifetimes[sp])} deaths")
    print(f"path failures: {len(ev['PATHFAIL'])} (distinct targets: "
          f"{len(set(re.search(r'to=(\d+)', l).group(1) for _, l in ev['PATHFAIL']))})")
    stuck = [re.search(r"STUCK (\S+) pos=(\S+) (\S+) (\S+)", l) for _, l in ev["STUCK"]]
    print(f"stuck reports: {len(stuck)}")
    spots = collections.Counter((round(float(m.group(2)) / 128), round(float(m.group(4)) / 128)) for m in stuck)
    for (gx, gz), c in spots.most_common(8):
        print(f"  around x={gx * 128} z={gz * 128}: {c}")

    # distance covered per bot life
    dist = collections.defaultdict(float)
    for name, pts in tracks.items():
        for a, b in zip(pts, pts[1:]):
            dist[species_of(name)] += ((a[1] - b[1]) ** 2 + (a[3] - b[3]) ** 2) ** 0.5
    print("distance covered (units):", {k: round(v) for k, v in dist.items()})
    goals = collections.Counter(p[4] for pts in tracks.values() for p in pts)
    print("goal share in status samples:", dict(goals.most_common()))

    # per bot class (SPAWN ... class=X)
    cls_of = {}
    for _, l in ev["SPAWN"]:
        m = re.search(r"SPAWN (\S+) species=\S+ class=(\S+)", l)
        if m:
            cls_of[m.group(1)] = m.group(2)
    if cls_of and any(c != "-" for c in cls_of.values()):
        text = open(sys.argv[1], encoding="latin-1").read()
        weapons = collections.defaultdict(collections.Counter)
        for m in re.finditer(r"STATUS (\S+) .*? goal=\S+ target=\S+ weapon=(\S+)", text):
            weapons[cls_of.get(m.group(1), "?")][m.group(2)] += 1
        badanim = collections.Counter()
        for m in re.finditer(r"ANIM (\S+) set=\?", text):
            badanim[cls_of.get(m.group(1), "?")] += 1
        stats = collections.defaultdict(lambda: collections.Counter())
        for name, c in cls_of.items():
            stats[c]["spawns"] += 1
            pts = tracks.get(name, [])
            stats[c]["moved"] += sum(((a[1] - b[1]) ** 2 + (a[3] - b[3]) ** 2) ** 0.5
                                     for a, b in zip(pts, pts[1:]) if ((a[1] - b[1]) ** 2 + (a[3] - b[3]) ** 2) ** 0.5 < 1500)
            stats[c]["goal_samples"] += sum(1 for p in pts if p[4] != "-")
            stats[c]["samples"] += len(pts)
        for _, l in ev["DEATH"]:
            m = re.search(r"DEATH (\S+) killer=(\S+)", l)
            stats[cls_of.get(m.group(1), "?")]["deaths"] += 1
            if m.group(2) in cls_of:
                stats[cls_of[m.group(2)]]["kills"] += 1
        print("per class:")
        for c, st in sorted(stats.items()):
            w = ", ".join(f"{k} {v}" for k, v in weapons[c].most_common(3))
            print(f"  {c:12s} spawns={st['spawns']:2d} kills={st['kills']:2d} deaths={st['deaths']:2d} "
                  f"moved={st['moved']:6.0f} withGoal={st['goal_samples']}/{st['samples']} "
                  f"badAnim={badanim[c]} weapons: {w}")

    if len(sys.argv) > 3:
        from PIL import Image, ImageDraw
        nodes, edges = load_nav(sys.argv[2])
        xs = [n[0] for n in nodes]
        zs = [n[2] for n in nodes]
        x0, x1, z0, z1 = min(xs) - 64, max(xs) + 64, min(zs) - 64, max(zs) + 64
        scale = 1000.0 / max(x1 - x0, z1 - z0)
        img = Image.new("RGB", (int((x1 - x0) * scale), int((z1 - z0) * scale)), (16, 16, 20))
        d = ImageDraw.Draw(img)

        def px(x, z):
            return ((x - x0) * scale, (z - z0) * scale)

        for a, b, kind in edges:
            d.line([px(nodes[a][0], nodes[a][2]), px(nodes[b][0], nodes[b][2])], fill=(55, 55, 62), width=1)
        for name, pts in tracks.items():
            col = SPECIES_COLOURS.get(species_of(name), (200, 200, 200))
            for a, b in zip(pts, pts[1:]):
                if ((a[1] - b[1]) ** 2 + (a[3] - b[3]) ** 2) ** 0.5 < 1500:  # skip respawn jumps
                    d.line([px(a[1], a[3]), px(b[1], b[3])], fill=col, width=2)
        for m in stuck:
            cx, cz = px(float(m.group(2)), float(m.group(4)))
            d.ellipse([cx - 6, cz - 6, cx + 6, cz + 6], outline=(255, 40, 40), width=2)
        y = 8
        for sp, col in SPECIES_COLOURS.items():
            d.rectangle([8, y, 20, y + 10], fill=col)
            d.text((26, y - 1), sp, fill=(230, 230, 230))
            y += 16
        d.ellipse([8, y, 20, y + 12], outline=(255, 40, 40), width=2)
        d.text((26, y), "stuck", fill=(230, 230, 230))
        img.save(sys.argv[3])
        print("wrote", sys.argv[3])


if __name__ == "__main__":
    main()
