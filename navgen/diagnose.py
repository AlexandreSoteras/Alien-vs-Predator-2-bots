"""Explain why spawn areas of a map are disconnected in the bot navigation.

Usage: py -I diagnose.py <MAP base name as in maps/out, e.g. DM_HANGER> [--max-gap 6]

Groups the multiplayer start points by walk/ladder/jump/drop reachability (same rules as navbuild),
then for each smaller group finds the closest span pair to the main group and prints:
the column profiles between them (solid spans: floor, top, walkable) and the world models whose
bounding box touches that spot (doors, glass, volumes...), which are the usual culprits.
"""
import argparse
import collections
import importlib.util
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("navbuild", os.path.join(HERE, "navbuild.py"))
nb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nb)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("map")
    ap.add_argument("--max-gap", type=int, default=6, help="max cell distance between the two groups' spans")
    a = ap.parse_args()
    out = os.path.join(HERE, "..", "maps", "out")
    obj = os.path.join(out, a.map + ".obj")
    js = os.path.join(out, a.map + ".objects.json")
    P = dict(nb.DEFAULTS)
    info = nb.MapInfo(js, "start", "MP_DM")
    tris = nb.load_obj(obj, r"^(volume_|ladder_)", r"^(sky|nonsolid)$", info.auto_exclude)
    cols, bounds = nb.rasterize(tris, P)
    spans = nb.walkable_spans(cols, P)
    if info.hazards:
        spans = nb.drop_hazards(spans, bounds, P, info.hazards)
    adj, drops, jumps = nb.link(spans, P)
    ox, oz, _, _ = bounds
    cs = P["cs"]

    # undirected reachability (walk + drop + jump; ladders via their links later) to group spawns
    und = collections.defaultdict(set)
    for k, vs in adj.items():
        und[k] |= vs
    for d in (drops, jumps):
        for k, vs in d.items():
            for v in vs:
                und[k].add(v)
                und[v].add(k)
    comp = nb.components(set(spans), und)
    starts = []
    for s in info.starts:
        k = nb.find_span(spans, bounds, P, s)
        starts.append((s, k, comp.get(k)))
    groups = collections.Counter(c for _, _, c in starts if c is not None)
    print(f"{a.map}: {len(info.starts)} spawns in {len(groups)} groups: {groups.most_common()}")
    if len(groups) < 2:
        return
    main_c = groups.most_common(1)[0][0]
    by_comp = collections.defaultdict(list)
    for k, c in comp.items():
        by_comp[c].append(k)
    main_cols = collections.defaultdict(list)
    for k in by_comp[main_c]:
        main_cols[(k[0], k[1])].append(k)
    wms = json.load(open(js, encoding="utf-8"))["world_models"]

    for c, n in groups.most_common()[1:]:
        best = None
        for k in by_comp[c]:
            for dx in range(-a.max_gap, a.max_gap + 1):
                for dz in range(-a.max_gap, a.max_gap + 1):
                    for m in main_cols.get((k[0] + dx, k[1] + dz), ()):
                        d = abs(dx) + abs(dz) + abs(spans[k][0] - spans[m][0]) / 32.0
                        if best is None or d < best[0]:
                            best = (d, k, m)
        print(f"\n-- group {c} ({n} spawns)")
        if not best:
            print("   no span of the main group within", a.max_gap, "cells")
            continue
        _, k, m = best
        for tag, s in (("this group", k), ("main group", m)):
            print(f"   {tag}: cell {s[:2]} floor {spans[s][0]:.0f} ceiling {spans[s][1]:.0f} "
                  f"world x={ox + (s[0] + .5) * cs:.0f} z={oz + (s[1] + .5) * cs:.0f}")
        x0, x1 = sorted((k[0], m[0]))
        z0, z1 = sorted((k[1], m[1]))
        for ix in range(x0, x1 + 1):
            for iz in range(z0, z1 + 1):
                prof = [(round(lo), round(hi), "W" if wk else "-") for lo, hi, wk in cols.get((ix, iz), [])]
                print(f"   col {ix},{iz}: {prof}")
        wx0, wx1 = ox + x0 * cs - 32, ox + (x1 + 1) * cs + 32
        wz0, wz1 = oz + z0 * cs - 32, oz + (z1 + 1) * cs + 32
        fy = min(spans[k][0], spans[m][0]) - 16
        fy1 = max(spans[k][0], spans[m][0]) + P["agent_height"]
        hits = [w for w in wms if w["bbox_min"][0] <= wx1 and w["bbox_max"][0] >= wx0 and w["bbox_min"][2] <= wz1
                and w["bbox_max"][2] >= wz0 and w["bbox_min"][1] <= fy1 and w["bbox_max"][1] >= fy and w["kind"] != "world"]
        for w in hits:
            print(f"   world model: {w['name']} kind={w['kind']} class={w.get('object_class')} blocks={w.get('blocks')} "
                  f"bbox y {w['bbox_min'][1]:.0f}..{w['bbox_max'][1]:.0f}")


if __name__ == "__main__":
    main()
