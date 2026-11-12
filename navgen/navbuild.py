"""Build a bot waypoint graph from world collision triangles (a small, pure-Python Recast-style pipeline).

Pipeline:
  1. rasterize triangles into a heightfield of solid spans (cell size CS on X/Z, Y is up)
  2. mark span tops walkable (slope), keep those with enough head clearance
  3. link neighbouring walkable spans whose height difference is climbable; add one-way drop links
  4. erode the walkable area by the agent radius
  5. cluster the surviving spans around seeds on a coarse lattice -> waypoints; adjacent clusters -> edges
  6. check that every start point lands on the main connected component
  7. write a plain-text .nav file for the server and a top-down PNG for inspection

Usage:
  py -I navbuild.py <map.obj> [--objects <map.objects.json>] [--out <dir>] [--cs 16] [--exclude-groups REGEX]
"""
import argparse
import collections
import heapq
import json
import math
import os
import re
import sys

# ---------------------------------------------------------------- parameters

DEFAULTS = dict(
    cs=16.0,            # horizontal cell size (game units)
    agent_height=120.0,  # Predator/adult Alien: CharacterButes DefaultDims <16, 60, 16> are half-extents
    agent_radius=16.0,   # half-width shared by humans, Predators and Aliens (exosuit/Praetorian not covered)
    climb=25.0,          # max step-up between neighbouring cells (engine DEFAULT_STAIRSTEP_HEIGHT)
    jump=60.0,           # max height of a one-way jump-up link (jump speed 550 u/s)
    max_slope=46.0,      # degrees; steeper surfaces are walls
    max_drop=260.0,      # max height of a one-way drop link
    max_climb=800.0,     # max height of a wall an Alien climbs ('climb'/'climbdown' links, Aliens only)
    merge=2.0,           # spans whose tops are this close merge their walkable flags
    seed_every=4,        # waypoint lattice spacing, in cells
)


# ---------------------------------------------------------------- geometry input

def load_obj(path, exclude=None, exclude_materials=None, exclude_names=()):
    """Return a list of triangles ((x,y,z),(x,y,z),(x,y,z)) from a Wavefront OBJ.

    Skips groups matching the regex 'exclude' or listed in 'exclude_names', and faces whose
    material matches 'exclude_materials'."""
    verts, tris = [], []
    group, skip_group, skip_mat = "", False, False
    rx = re.compile(exclude) if exclude else None
    rxm = re.compile(exclude_materials) if exclude_materials else None
    names = set(exclude_names)
    with open(path, encoding="latin-1") as f:
        for line in f:
            if line.startswith("v "):
                _, x, y, z = line.split()[:4]
                verts.append((float(x), float(y), float(z)))
            elif line.startswith(("o ", "g ")):
                group = line[2:].strip()
                skip_group = bool(rx and rx.search(group)) or group in names
            elif line.startswith("usemtl "):
                skip_mat = bool(rxm and rxm.search(line[7:].strip()))
            elif line.startswith("f ") and not (skip_group or skip_mat):
                idx = []
                for tok in line.split()[1:]:
                    i = int(tok.split("/")[0])
                    idx.append(i - 1 if i > 0 else len(verts) + i)
                for k in range(1, len(idx) - 1):
                    tris.append((verts[idx[0]], verts[idx[k]], verts[idx[k + 1]]))
    return tris


def tri_normal_y(a, b, c):
    ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
    vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
    nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
    ln = math.sqrt(nx * nx + ny * ny + nz * nz)
    return 0.0 if ln == 0 else ny / ln


def clip(poly, axis, value, keep_greater):
    """Sutherland-Hodgman clip of a polygon against an axis-aligned plane."""
    out = []
    n = len(poly)
    for i in range(n):
        p, q = poly[i], poly[(i + 1) % n]
        dp = (p[axis] - value) if keep_greater else (value - p[axis])
        dq = (q[axis] - value) if keep_greater else (value - q[axis])
        if dp >= 0:
            out.append(p)
        if (dp >= 0) != (dq >= 0):
            t = dp / (dp - dq)
            out.append(tuple(p[k] + (q[k] - p[k]) * t for k in range(3)))
    return out


# ---------------------------------------------------------------- heightfield

def rasterize(tris, P):
    cs = P["cs"]
    cos_slope = math.cos(math.radians(P["max_slope"]))
    xs = [v[0] for t in tris for v in t]
    zs = [v[2] for t in tris for v in t]
    ox, oz = min(xs), min(zs)
    w = int((max(xs) - ox) / cs) + 1
    h = int((max(zs) - oz) / cs) + 1
    columns = collections.defaultdict(list)  # (ix, iz) -> [(ymin, ymax, walkable)]
    for tri in tris:
        # winding is not trusted (it depends on the source format), so any near-horizontal face counts;
        # undersides of slabs are rejected later by the head-clearance test
        walkable = abs(tri_normal_y(*tri)) >= cos_slope
        tz0 = int((min(v[2] for v in tri) - oz) / cs)
        tz1 = int((max(v[2] for v in tri) - oz) / cs)
        for iz in range(max(tz0, 0), min(tz1, h - 1) + 1):
            z0, z1 = oz + iz * cs, oz + (iz + 1) * cs
            row = clip(clip(list(tri), 2, z0, True), 2, z1, False)
            if len(row) < 3:
                continue
            tx0 = int((min(v[0] for v in row) - ox) / cs)
            tx1 = int((max(v[0] for v in row) - ox) / cs)
            for ix in range(max(tx0, 0), min(tx1, w - 1) + 1):
                x0, x1 = ox + ix * cs, ox + (ix + 1) * cs
                cell = clip(clip(row, 0, x0, True), 0, x1, False)
                if len(cell) < 3:
                    continue
                ys = [v[1] for v in cell]
                columns[(ix, iz)].append((min(ys), max(ys), walkable))
    merged = {}
    for key, spans in columns.items():
        spans.sort()
        out = []
        for lo, hi, wk in spans:
            if out and lo <= out[-1][1]:
                plo, phi, pwk = out[-1]
                if abs(hi - phi) <= P["merge"]:
                    out[-1] = (plo, max(hi, phi), pwk or wk)
                elif hi > phi:
                    out[-1] = (plo, hi, wk)
            else:
                out.append((lo, hi, wk))
        merged[key] = out
    return merged, (ox, oz, w, h)


def walkable_spans(columns, P):
    """Return {(ix, iz, k): (floor_y, ceiling_y)} for span tops a character can stand on."""
    res = {}
    for (ix, iz), spans in columns.items():
        for k, (lo, hi, wk) in enumerate(spans):
            ceiling = spans[k + 1][0] if k + 1 < len(spans) else float("inf")
            if not wk:
                # a low non-walkable lip right above a walkable span (stair nosing) counts as walkable
                if k > 0 and spans[k - 1][2] and hi - spans[k - 1][1] <= P["climb"] and lo - spans[k - 1][1] < 1.0:
                    wk = True
            if wk and ceiling - hi >= P["agent_height"]:
                res[(ix, iz, k)] = (hi, ceiling)
    return res


NEIGHBOURS = [(1, 0), (-1, 0), (0, 1), (0, -1)]


def link(spans, P):
    by_col = collections.defaultdict(list)
    for key in spans:
        by_col[(key[0], key[1])].append(key)
    adj = collections.defaultdict(set)    # two-way walk links
    drops = collections.defaultdict(set)  # one-way drop links
    jumps = collections.defaultdict(set)  # one-way jump-up links
    for key, (fy, cy) in spans.items():
        ix, iz, _ = key
        for dx, dz in NEIGHBOURS:
            best_drop = None
            for nk in by_col.get((ix + dx, iz + dz), ()):
                nfy, ncy = spans[nk]
                gap = min(cy, ncy) - max(fy, nfy)
                if abs(nfy - fy) <= P["climb"] and gap >= P["agent_height"]:
                    adj[key].add(nk)
                elif fy - nfy > P["climb"] and fy - nfy <= P["max_drop"] and ncy - fy >= P["agent_height"]:
                    if best_drop is None or nfy > spans[best_drop][0]:
                        best_drop = nk
            if best_drop is not None:
                drops[key].add(best_drop)
            # jump up onto a ledge a little higher than a stair step, with head room for the jump
            for nk in by_col.get((ix + dx, iz + dz), ()):
                nfy, ncy = spans[nk]
                rise = nfy - fy
                if P["climb"] < rise <= P["jump"] and ncy - nfy >= P["agent_height"] and cy - fy >= P["agent_height"] + rise:
                    jumps[key].add(nk)
                    break
    return adj, drops, jumps


def climb_pairs(spans, P):
    """Wall climbs for Aliens: (floor span, ledge span) neighbour pairs where the ledge is higher than a jump
    and at most max_climb above the floor, with room on the ledge and open space in front of the wall up to
    above the lip (the floor span's own ceiling)."""
    by_col = collections.defaultdict(list)
    for key in spans:
        by_col[(key[0], key[1])].append(key)
    pairs = []
    for key, (fy, cy) in spans.items():
        ix, iz, _ = key
        for dx, dz in NEIGHBOURS:
            for nk in by_col.get((ix + dx, iz + dz), ()):
                nfy, ncy = spans[nk]
                rise = nfy - fy
                if P["jump"] < rise <= P["max_climb"] and ncy - nfy >= P["agent_height"] and cy >= nfy + 40.0:
                    pairs.append((key, nk))
    return pairs


class ClimbPortal(tuple):
    """The (x, y, z) wall point of a climb edge (on the wall face below the lip), plus the floor point in
    front of the wall, the point on the ledge, and the wall's horizontal normal (nx, nz)."""

    def __new__(cls, wall, foot, top, normal):
        obj = super().__new__(cls, wall)
        obj.foot, obj.top, obj.normal = foot, top, normal
        return obj


def climb_links(pairs, spans_all, adj_all, keys, spans, owner, nodes, bounds, P):
    """One-way 'climb' edges (Aliens only) between waypoint areas joined by climbable walls: per area pair,
    the wall spot nearest the middle of the two waypoints."""
    ox, oz, _, _ = bounds
    cs = P["cs"]
    reach = max(1, int(math.ceil(P["agent_radius"] / cs))) + 1

    def nearest_kept(start):
        if start in keys:
            return start
        seen, q = {start}, collections.deque([(start, 0)])
        while q:
            k, dist = q.popleft()
            if dist >= reach:
                continue
            for nk in adj_all[k]:
                if nk not in seen:
                    if nk in keys:
                        return nk
                    seen.add(nk)
                    q.append((nk, dist + 1))
        return None

    def world(key):
        return (ox + (key[0] + 0.5) * cs, spans[key][0], oz + (key[1] + 0.5) * cs)

    best = {}
    for k, nk in pairs:
        ka, kb = nearest_kept(k), nearest_kept(nk)
        if ka is None or kb is None:
            continue
        a, b = owner.get(ka), owner.get(kb)
        if a is None or b is None or a == b:
            continue
        fx, fz = ox + (k[0] + 0.5) * cs, oz + (k[1] + 0.5) * cs
        tx, tz = ox + (nk[0] + 0.5) * cs, oz + (nk[1] + 0.5) * cs
        fy, ty = spans_all[k][0], spans_all[nk][0]
        normal = ((fx - tx) / cs, (fz - tz) / cs)        # out of the wall, towards the floor side
        face = ((fx + tx) / 2.0, (fz + tz) / 2.0)
        wall = (face[0] + normal[0], max(ty - 32.0, fy + 8.0), face[1] + normal[1])
        mx, mz = (nodes[a][0] + nodes[b][0]) / 2.0, (nodes[a][2] + nodes[b][2]) / 2.0
        d = (face[0] - mx) ** 2 + (face[1] - mz) ** 2
        if (a, b) not in best or d < best[(a, b)][0]:
            best[(a, b)] = (d, ClimbPortal(wall, world(ka), world(kb), normal))
    out = []
    for (a, b), (_, p) in best.items():
        out.append((a, b, p, "climb"))
        # the same wall downwards: from the ledge onto the wall below the lip, then down to the floor
        out.append((b, a, ClimbPortal(tuple(p), p.top, p.foot, p.normal), "climbdown"))
    return out


def erode(spans, adj, P):
    """Drop spans closer than the agent radius to a wall or ledge (a span missing a walk link in some direction)."""
    radius_cells = max(1, int(math.ceil(P["agent_radius"] / P["cs"])))
    dist = {}
    q = collections.deque()
    for key in spans:
        if len(adj[key]) < 4:
            dist[key] = 0
            q.append(key)
    while q:
        key = q.popleft()
        for nk in adj[key]:
            if nk not in dist:
                dist[nk] = dist[key] + 1
                q.append(nk)
    return {k: v for k, v in spans.items() if dist.get(k, 99) >= radius_cells}


def remap_drops(drops, adj, keys, P):
    """Drop links start and end on ledge cells that erosion removes; move both ends to the nearest kept span."""
    reach = max(1, int(math.ceil(P["agent_radius"] / P["cs"]))) + 1

    def nearest_kept(start):
        if start in keys:
            return start
        seen, q = {start}, collections.deque([(start, 0)])
        while q:
            k, dist = q.popleft()
            if dist >= reach:
                continue
            for nk in adj[k]:
                if nk not in seen:
                    if nk in keys:
                        return nk
                    seen.add(nk)
                    q.append((nk, dist + 1))
        return None

    out = collections.defaultdict(set)
    for src, targets in drops.items():
        s = nearest_kept(src)
        if s is None:
            continue
        for t in targets:
            d = nearest_kept(t)
            if d is not None and d != s:
                out[s].add(d)
    return out


def components(keys, adj):
    comp, cid = {}, 0
    for start in keys:
        if start in comp:
            continue
        stack = [start]
        comp[start] = cid
        while stack:
            k = stack.pop()
            for nk in adj[k]:
                if nk in keys and nk not in comp:
                    comp[nk] = cid
                    stack.append(nk)
        cid += 1
    return comp


# ---------------------------------------------------------------- waypoints

def cluster(spans, adj, drops, P, bounds, jumps=None):
    """Multi-source BFS from lattice seeds; returns waypoints, edges and the span->cluster map."""
    ox, oz, _, _ = bounds
    cs = P["cs"]
    every = P["seed_every"]
    keys = set(spans)
    owner = {}
    q = collections.deque()
    seeds = []
    for key in sorted(keys):
        ix, iz, _ = key
        if ix % every == every // 2 and iz % every == every // 2:
            owner[key] = len(seeds)
            seeds.append(key)
            q.append(key)
    # any region without a lattice seed gets its own seed
    while True:
        while q:
            k = q.popleft()
            for nk in adj[k]:
                if nk in keys and nk not in owner:
                    owner[nk] = owner[k]
                    q.append(nk)
        rest = [k for k in keys if k not in owner]
        if not rest:
            break
        k = min(rest)
        owner[k] = len(seeds)
        seeds.append(k)
        q.append(k)

    def world(key):
        ix, iz, _ = key
        return (ox + (ix + 0.5) * cs, spans[key][0], oz + (iz + 0.5) * cs)

    nodes = [world(s) for s in seeds]
    portals = {}  # (a, b, kind) -> list of border spans
    for k in keys:
        a = owner[k]
        for nk in adj[k]:
            if nk in owner and owner[nk] != a:
                portals.setdefault((min(a, owner[nk]), max(a, owner[nk]), "walk"), []).append(k)
        for nk in drops[k]:
            if nk in owner and owner[nk] != a:
                portals.setdefault((a, owner[nk], "drop"), []).append(k)
        for nk in (jumps or {}).get(k, ()):
            if nk in owner and owner[nk] != a:
                portals.setdefault((a, owner[nk], "jump"), []).append(k)
    edges = []
    for (a, b, kind), border in portals.items():
        ax, _, az = nodes[a]
        bx, _, bz = nodes[b]
        mx, mz = (ax + bx) / 2, (az + bz) / 2
        best = min(border, key=lambda s: (world(s)[0] - mx) ** 2 + (world(s)[2] - mz) ** 2)
        edges.append((a, b, world(best), kind))
    return nodes, edges, owner, seeds


def find_span(spans, bounds, P, pos):
    """The walkable span under a world position (start points sit a little above the floor)."""
    ox, oz, _, _ = bounds
    ix, iz = int((pos[0] - ox) / P["cs"]), int((pos[2] - oz) / P["cs"])
    by_col = collections.defaultdict(list)
    for k in spans:
        by_col[(k[0], k[1])].append(k)
    for r in range(0, 4):  # widen the search ring until a floor below the point is found
        best, best_d = None, None
        for jx in range(ix - r, ix + r + 1):
            for jz in range(iz - r, iz + r + 1):
                for k in by_col.get((jx, jz), ()):
                    d = pos[1] - spans[k][0]
                    # map start points and pickups float well above the floor (up to ~600u in DM_Compound);
                    # the game drops players to the ground, so project down
                    if -P["climb"] <= d <= 1200 and (best_d is None or d < best_d):
                        best, best_d = k, d
        if best:
            return best
    return None


# ---------------------------------------------------------------- output

def strongly_connected(vertices, graph):
    """Kosaraju, iterative: {vertex: component id} for a directed graph given as {v: set(successors)}."""
    order, done = [], set()
    for root in vertices:
        if root in done:
            continue
        done.add(root)
        stack = [(root, iter(graph.get(root, ())))]
        while stack:
            v, it = stack[-1]
            for w in it:
                if w not in done:
                    done.add(w)
                    stack.append((w, iter(graph.get(w, ()))))
                    break
            else:
                stack.pop()
                order.append(v)
    rev = collections.defaultdict(set)
    for v, ws in graph.items():
        for w in ws:
            rev[w].add(v)
    comp, cid = {}, 0
    for root in reversed(order):
        if root in comp:
            continue
        comp[root] = cid
        stack = [root]
        while stack:
            v = stack.pop()
            for w in rev.get(v, ()):
                if w not in comp:
                    comp[w] = cid
                    stack.append(w)
        cid += 1
    return comp


def write_nav(path, mapname, P, nodes, edges, badspawns=(), use_doors=()):
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("AVP2NAV 1\n")
        f.write(f"map {mapname}\n")
        f.write("params " + " ".join(f"{k}={v}" for k, v in P.items()) + "\n")
        f.write(f"nodes {len(nodes)}\n")
        for i, (x, y, z) in enumerate(nodes):
            f.write(f"n {i} {x:.1f} {y:.1f} {z:.1f}\n")
        f.write(f"edges {len(edges)}\n")
        for a, b, p, kind in edges:
            px, py, pz = p
            extra = ""
            if isinstance(p, LadderPortal):
                # foot stand point, top stand point (floor positions)
                extra = " " + " ".join(f"{c:.1f}" for c in (*p.foot, *p.top))
            elif isinstance(p, ClimbPortal):
                # floor point in front of the wall, point on the ledge, wall normal (x, z)
                extra = " " + " ".join(f"{c:.2f}" if i >= 6 else f"{c:.1f}"
                                       for i, c in enumerate((*p.foot, *p.top, *p.normal)))
            f.write(f"e {a} {b} {px:.1f} {py:.1f} {pz:.1f} {kind}{extra}\n")
        # doors the graph goes through: bots press Use on these (and only these) as they reach them
        for name in use_doors:
            f.write(f"usedoor {name}\n")
        # start points bots should not spawn at: 'walkers' (Marines, Predators), 'aliens' or 'all'
        for x, y, z, who in badspawns:
            f.write(f"badspawn {x:.1f} {y:.1f} {z:.1f} {who}\n")


def render(path, P, bounds, spans_all, spans, comp, main_comp, nodes, edges, starts, start_hits, tris=None):
    from PIL import Image, ImageDraw
    ox, oz, w, h = bounds
    cs = P["cs"]
    scale = 1024.0 / max(w, h)
    img = Image.new("RGB", (int(w * scale) + 1, int(h * scale) + 1), (18, 18, 22))
    d = ImageDraw.Draw(img)
    ys = [v[0] for v in spans_all.values()] or [0.0]
    ymin, ymax = min(ys), max(ys)

    def px(x, z):
        return ((x - ox) / cs * scale, (z - oz) / cs * scale)

    # walls/geometry footprint in dark grey
    if tris:
        for t in tris:
            if tri_normal_y(*t) < 0.3:
                d.polygon([px(v[0], v[2]) for v in t], outline=(55, 55, 60))
    # all walkable spans (before erosion) dim, kept spans coloured by height; red = not main component
    for (ix, iz, k), (fy, _) in sorted(spans_all.items(), key=lambda kv: kv[1][0]):
        x0, z0 = ix * scale, iz * scale
        if (ix, iz, k) in spans:
            t = (fy - ymin) / ((ymax - ymin) or 1)
            col = (int(40 + 60 * t), int(110 + 140 * t), int(60 + 60 * t)) if comp.get((ix, iz, k)) == main_comp else (200, 60, 60)
        else:
            col = (45, 45, 70)
        d.rectangle([x0, z0, x0 + scale, z0 + scale], fill=col)
    for a, b, (pxw, _, pzw), kind in edges:
        ax, az = px(nodes[a][0], nodes[a][2])
        bx, bz = px(nodes[b][0], nodes[b][2])
        mx, mz = px(pxw, pzw)
        col = {"walk": (240, 200, 60), "drop": (255, 120, 220), "ladder": (60, 230, 255), "jump": (255, 60, 60),
               "climb": (120, 255, 120), "climbdown": (120, 255, 120)}.get(kind, (255, 255, 255))
        d.line([ax, az, mx, mz, bx, bz], fill=col, width=3 if kind == "ladder" else 1)
    for x, _, z in nodes:
        cx, cz = px(x, z)
        d.ellipse([cx - 2, cz - 2, cx + 2, cz + 2], fill=(255, 255, 255))
    for s, hit in zip(starts, start_hits):
        cx, cz = px(s[0], s[2])
        col = (60, 160, 255) if hit else (255, 0, 0)
        d.rectangle([cx - 5, cz - 5, cx + 5, cz + 5], outline=col, width=2)
    # scale bar: 512 units
    bar = 512 / cs * scale
    d.line([10, img.height - 12, 10 + bar, img.height - 12], fill=(255, 255, 255), width=3)
    d.text((14 + bar, img.height - 20), "512 u", fill=(255, 255, 255))
    img.save(path)


# ---------------------------------------------------------------- main

def prop(o, name, default=None):
    props = o.get("properties") or []
    if isinstance(props, dict):
        return props.get(name, default)
    for p in props:
        if p.get("name") == name:
            return p.get("value")
    return default


class MapInfo:
    """What navbuild needs from dump_world.py's <map>.objects.json."""

    def __init__(self, path, cls_regex, start_prop):
        self.starts, self.ladders, self.hazards, self.auto_exclude, self.water = [], [], [], [], []
        self.use_doors = []
        if not path or not os.path.exists(path):
            return
        data = json.load(open(path, encoding="utf-8"))
        objs = data["objects"] if isinstance(data, dict) and "objects" in data else data
        rx = re.compile(cls_regex, re.I)
        for o in objs:
            cls = o.get("class") or ""
            if rx.search(cls) and (not start_prop or prop(o, start_prop, True)):
                pos = o.get("pos") or prop(o, "Pos")
                if pos and len(pos) >= 3:
                    self.starts.append(tuple(float(c) for c in pos[:3]))
        if not isinstance(data, dict):
            return
        wms = data.get("world_models", [])
        world = [w for w in wms if w.get("kind") in ("world", "terrain")]
        door_props = {}
        for o in objs:
            if "door" in (o.get("class") or "").lower() and o.get("name"):
                door_props[o["name"].lower()] = {p["name"]: p.get("value") for p in o.get("properties", [])}
        self.use_doors = []
        span_x = max((w["bbox_max"][0] - w["bbox_min"][0]) for w in world) if world else 1e9
        span_z = max((w["bbox_max"][2] - w["bbox_min"][2]) for w in world) if world else 1e9
        self.water = []
        for w in wms:
            kind, name = w.get("kind"), w.get("name", "")
            cls = w.get("object_class") or ""
            bmin, bmax = w["bbox_min"], w["bbox_max"]
            if kind == "ladder":
                self.ladders.append((name, bmin, bmax))
            elif kind == "volume":
                # volumes never block (their OBJ groups are excluded); the harmful ones are no-go zones
                if cls in ("Electricity", "PoisonGas") or re.search(r"damage|death|kill|lava|acid", name, re.I):
                    self.hazards.append((name, bmin, bmax))
            elif kind == "liquid":
                # liquids are volumes you move through, not walls or floors
                self.auto_exclude.append("liquid_" + name)
                if cls == "CorrosiveFluid":
                    self.hazards.append((name, bmin, bmax))
                else:
                    self.water.append((name, bmin, bmax))
            elif kind == "brush" and w.get("blocks") is False:
                # brushes whose Solid property is off (decoration, effects): nothing collides with them
                self.auto_exclude.append("brush_" + name)
            elif kind == "brush" and (name.lower() == "skybox" or
                                      (bmax[0] - bmin[0] > 0.8 * span_x and bmax[2] - bmin[2] > 0.8 * span_z)):
                # the sky box, and map-sized translucent layers (fog/cloud sheets) do not block anyone in game
                self.auto_exclude.append("brush_" + name)
            elif kind == "door" and w.get("blocks") is False:
                self.auto_exclude.append("door_" + name)
            elif kind == "door" and name.lower() in door_props:
                # doors players open with the Use key (ActivateTrigger, not locked) and that stand upright (not
                # hatches in a floor): bots open them the same way, so the graph goes through them
                p = door_props[name.lower()]
                if (p.get("ActivateTrigger") and not p.get("Locked") and bmax[1] - bmin[1] >= 64
                        and not re.search(r"sky", name, re.I)):
                    self.auto_exclude.append("door_" + name)
                    self.use_doors.append(name)


def drop_deep_water(spans, bounds, P, water, max_depth=72.0):
    """Floors under more than max_depth of water need swimming, which the bots do not do yet."""
    ox, oz, _, _ = bounds
    cs = P["cs"]
    out = {}
    for (ix, iz, k), (fy, cy) in spans.items():
        x, z = ox + (ix + 0.5) * cs, oz + (iz + 0.5) * cs
        deep = any(bmin[0] <= x <= bmax[0] and bmin[2] <= z <= bmax[2] and bmin[1] - 1 <= fy < bmax[1] - max_depth
                   for _, bmin, bmax in water)
        if not deep:
            out[(ix, iz, k)] = (fy, cy)
    return out


def drop_hazards(spans, bounds, P, hazards):
    ox, oz, _, _ = bounds
    cs = P["cs"]
    out = {}
    for (ix, iz, k), (fy, cy) in spans.items():
        x, z = ox + (ix + 0.5) * cs, oz + (iz + 0.5) * cs
        bad = any(bmin[0] <= x <= bmax[0] and bmin[2] <= z <= bmax[2] and bmin[1] - P["agent_height"] <= fy <= bmax[1]
                  for _, bmin, bmax in hazards)
        if not bad:
            out[(ix, iz, k)] = (fy, cy)
    return out


class LadderPortal(tuple):
    """The (x, y, z) climb point of a ladder edge, plus where to stand at its foot and where to step off at
    its top (floor positions). Unpacks like a plain portal tuple."""

    def __new__(cls, portal, foot, top):
        obj = super().__new__(cls, portal)
        obj.foot, obj.top = foot, top
        return obj


def ladder_links(spans, bounds, P, owner, comp, ladders, columns=None):
    """For each ladder volume, link the waypoint at its foot with the one at its top (two-way 'ladder' edges).

    With 'columns' (the solid intervals per grid cell), a link needs a free vertical shaft inside the ladder
    box, from the foot floor to standing height above the top floor, with a floor waypoint next to it at
    both ends; the edge then records the shaft position and both stand points. Ladders that end under a
    ceiling, or whose top is behind a wall, get no link (bots would hang on them)."""
    ox, oz, _, _ = bounds
    cs = P["cs"]
    H = P["agent_height"]
    edges, report = [], []

    def solid_overlap(ix, iz, y0, y1):
        for lo, hi, _ in columns.get((ix, iz), ()):
            if hi > y0 and lo < y1:
                return True
        return False

    def shaft_clear(jx, jz, y0, y1):
        # a character centred on grid corner (jx, jz) covers the 2x2 cells around it
        return not any(solid_overlap(ix, iz, y0, y1) for ix in (jx - 1, jx) for iz in (jz - 1, jz))

    def path_clear(x0, z0, x1, z1, y0, y1):
        # the character's width counts: at each sample, the 2x2 cells around the nearest grid corner
        n = max(1, int(math.hypot(x1 - x0, z1 - z0) / 8.0))
        for s in range(n + 1):
            t = s / n
            x, z = x0 + (x1 - x0) * t, z0 + (z1 - z0) * t
            if not shaft_clear(int(round((x - ox) / cs)), int(round((z - oz) / cs)), y0, y1):
                return False
        return True

    comp_size = collections.Counter(comp.values())
    for name, bmin, bmax in ladders:
        reach = 64.0
        cx, cz = (bmin[0] + bmax[0]) / 2, (bmin[2] + bmax[2]) / 2

        # every floor level you can step onto along the ladder: the foot, intermediate landings, the top
        levels = {}  # rounded floor height -> list of (span key, world pos)
        for (ix, iz, k), (fy, _) in spans.items():
            if owner.get((ix, iz, k)) is None:
                continue
            x, z = ox + (ix + 0.5) * cs, oz + (iz + 0.5) * cs
            if not (bmin[0] - reach <= x <= bmax[0] + reach and bmin[2] - reach <= z <= bmax[2] + reach):
                continue
            if not (bmin[1] - 32 <= fy <= bmax[1] + 48):
                continue
            levels.setdefault(int(round(fy / 48.0)), []).append(((ix, iz, k), (x, fy, z)))
        ordered = [levels[l] for l in sorted(levels)]
        linked, refused = [], []
        if columns is None:
            for lows, highs in zip(ordered, ordered[1:]):
                lo = min(lows, key=lambda s: (s[1][0] - cx) ** 2 + (s[1][2] - cz) ** 2)
                hi = min(highs, key=lambda s: (s[1][0] - cx) ** 2 + (s[1][2] - cz) ** 2)
                if owner[lo[0]] != owner[hi[0]] and hi[1][1] - lo[1][1] > P["climb"]:
                    edges.append((owner[lo[0]], owner[hi[0]], (cx, (lo[1][1] + hi[1][1]) / 2, cz), "ladder"))
                    linked.append(f"{lo[1][1]:.0f}->{hi[1][1]:.0f}")
        else:
            jx0, jx1 = int(math.ceil((bmin[0] - 8 - ox) / cs)), int(math.floor((bmax[0] + 8 - ox) / cs))
            jz0, jz1 = int(math.ceil((bmin[2] - 8 - oz) / cs)), int(math.floor((bmax[2] + 8 - oz) / cs))
            shafts = [(jx, jz, ox + jx * cs, oz + jz * cs) for jx in range(jx0, jx1 + 1) for jz in range(jz0, jz1 + 1)]

            def stand_points(group, sx, sz):
                # floors of a level you can walk to in a straight line from the shaft, at stepping height
                return [s for s in group if math.hypot(s[1][0] - sx, s[1][2] - sz) <= 96.0
                        and path_clear(sx, sz, s[1][0], s[1][2], s[1][1] + P["climb"], s[1][1] + H)]

            def pick(cands, sx, sz):
                # prefer the floor of the larger walk area (not a pipe or a ledge beside the ladder), then the nearest
                return min(cands, key=lambda s: (-comp_size.get(comp.get(s[0]), 0), math.hypot(s[1][0] - sx, s[1][2] - sz)))

            # each level links to the nearest higher level the shaft really leads to (landings that are not
            # reachable from the shaft are skipped; a shaft capped by a ceiling links nothing above it)
            for i in range(len(ordered) - 1):
                best = None
                for j in range(i + 1, len(ordered)):
                    for jx, jz, sx, sz in shafts:
                        feet = stand_points(ordered[i], sx, sz)
                        tops = stand_points(ordered[j], sx, sz)
                        if not feet or not tops:
                            continue
                        foot, top = pick(feet, sx, sz), pick(tops, sx, sz)
                        if top[1][1] - foot[1][1] <= P["climb"] or owner[foot[0]] == owner[top[0]]:
                            continue
                        if not shaft_clear(jx, jz, foot[1][1] + P["climb"], top[1][1] + H + 16):
                            continue
                        score = (math.hypot(foot[1][0] - sx, foot[1][2] - sz) + math.hypot(top[1][0] - sx, top[1][2] - sz)
                                 + 0.25 * math.hypot(sx - cx, sz - cz))
                        if best is None or score < best[0]:
                            best = (score, (sx, sz), foot, top)
                    if best is not None:
                        break
                ylo = ordered[i][0][1][1]
                if best is None:
                    refused.append(f"{ylo:.0f}->up")
                    continue
                _, (sx, sz), foot, top = best
                portal = LadderPortal((sx, (foot[1][1] + top[1][1]) / 2, sz), foot[1], top[1])
                edges.append((owner[foot[0]], owner[top[0]], portal, "ladder"))
                linked.append(f"{foot[1][1]:.0f}->{top[1][1]:.0f} at {sx:.0f},{sz:.0f}")
        parts = []
        if linked:
            parts.append("levels " + ", ".join(linked))
        if refused:
            parts.append("no free shaft for " + ", ".join(refused))
        report.append(f"{name}: " + ("; ".join(parts) if parts else f"not linked ({len(levels)} floor levels found)"))
    return edges, report


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("obj")
    ap.add_argument("--objects")
    ap.add_argument("--out", default=None)
    ap.add_argument("--exclude-groups", default=r"^(volume_|ladder_)", help="regex of OBJ groups that never block")
    ap.add_argument("--exclude-materials", default=r"^(sky|nonsolid)$", help="regex of OBJ materials that never block")
    ap.add_argument("--start-class", default=r"start", help="regex of start point class names")
    ap.add_argument("--start-prop", default="MP_DM", help="bool property a start point must have set (empty = any)")
    for k, v in DEFAULTS.items():
        ap.add_argument("--" + k.replace("_", "-"), type=type(v), default=v)
    a = ap.parse_args()
    P = {k: getattr(a, k) for k in DEFAULTS}
    mapname = os.path.splitext(os.path.basename(a.obj))[0].lower()
    out = a.out or os.path.dirname(os.path.abspath(a.obj))
    os.makedirs(out, exist_ok=True)

    info = MapInfo(a.objects, a.start_class, a.start_prop)
    if info.auto_exclude:
        print(f"ignoring {len(info.auto_exclude)} non-blocking world models (non-solid brushes, liquids, sky, "
              f"{len(info.use_doors)} doors opened with the Use key)")
    tris = load_obj(a.obj, a.exclude_groups, a.exclude_materials, info.auto_exclude)
    print(f"{mapname}: {len(tris)} triangles")
    columns, bounds = rasterize(tris, P)
    spans_all = walkable_spans(columns, P)
    if info.hazards:
        n0 = len(spans_all)
        spans_all = drop_hazards(spans_all, bounds, P, info.hazards)
        print(f"hazard volumes {[h[0] for h in info.hazards][:8]} removed {n0 - len(spans_all)} spans")
    if info.water:
        n0 = len(spans_all)
        spans_all = drop_deep_water(spans_all, bounds, P, info.water)
        print(f"water volumes: {len(info.water)}, deep-water spans removed: {n0 - len(spans_all)}")
    adj, drops, jumps = link(spans_all, P)
    climbs = climb_pairs(spans_all, P)
    spans = erode(spans_all, adj, P)
    keys = set(spans)
    drops = remap_drops(drops, adj, keys, P)
    jumps = remap_drops(jumps, adj, keys, P)
    adj_all = adj
    adj = {k: {n for n in v if n in keys} for k, v in adj.items() if k in keys}
    adj = collections.defaultdict(set, adj)
    comp = components(keys, adj)
    sizes = collections.Counter(comp.values())
    main_comp = sizes.most_common(1)[0][0] if sizes else None
    print(f"grid {bounds[2]}x{bounds[3]} cells of {P['cs']}u; walkable spans {len(spans_all)} -> {len(spans)} after erosion")
    print(f"walk components: {len(sizes)}; largest {sizes.most_common(5)}")

    nodes, edges, owner, seeds = cluster(spans, adj, drops, P, bounds, jumps)
    ledges, lreport = ladder_links(spans, bounds, P, owner, comp, info.ladders, columns)
    edges += ledges
    for line in lreport:
        print("ladder", line)
    edges += climb_links(climbs, spans_all, adj_all, keys, spans, owner, nodes, bounds, P)
    kinds = collections.Counter(e[3] for e in edges)
    print(f"waypoints {len(nodes)}, edges {len(edges)} {dict(kinds)}")

    # reachability over the waypoint graph (walk + ladder both ways, drops/jumps one way) from the start
    # points. 'graph' is what Marines and Predators can use (the spawn checks below); 'graph_any' adds the
    # Aliens' wall climbs (one way, up) and decides which waypoints are kept.
    graph = collections.defaultdict(set)
    graph_any = collections.defaultdict(set)
    for ea, eb, _, kind in edges:
        graph_any[ea].add(eb)
        if kind not in ("drop", "jump", "climb", "climbdown"):
            graph_any[eb].add(ea)
        if kind in ("climb", "climbdown"):
            continue
        graph[ea].add(eb)
        if kind not in ("drop", "jump"):
            graph[eb].add(ea)

    starts = info.starts
    badspawns = []
    start_nodes = []
    for s in starts:
        k = find_span(spans, bounds, P, s)
        start_nodes.append(owner.get(k) if k is not None else None)
    hits = [n is not None for n in start_nodes]
    if starts:
        valid = [n for n in start_nodes if n is not None]
        print(f"start points projected onto the nav: {sum(hits)}/{len(starts)}")
        # every start must reach every other start (strong connectivity among spawns)
        bad_pairs = 0
        for src in set(valid):
            seen, stack = {src}, [src]
            while stack:
                k = stack.pop()
                for nk in graph[k]:
                    if nk not in seen:
                        seen.add(nk)
                        stack.append(nk)
            bad_pairs += sum(1 for dst in set(valid) if dst not in seen)
        print(f"spawn-to-spawn pairs without a route: {bad_pairs} of {len(set(valid)) ** 2}")
        hits = [n is not None for n in start_nodes]

        # The main play area = the strongly connected part of the graph holding the most spawns (every point
        # in it reaches every other). A spawn outside it that can still reach it is a spawn platform you drop
        # from (normal in DM); a spawn that cannot reach it strands its bots in a separate part of the map.
        def main_area(g):
            scc = strongly_connected(range(len(nodes)), g)
            per = collections.Counter(scc[n] for n in valid)
            main_scc = per.most_common(1)[0][0] if per else None
            main = {n for n in range(len(nodes)) if scc[n] == main_scc}
            rev = collections.defaultdict(set)
            for k, vs in g.items():
                for v in vs:
                    rev[v].add(k)
            reach, stack = set(main), list(main)
            while stack:  # everything that can reach the main area
                k = stack.pop()
                for nk in rev[k]:
                    if nk not in reach:
                        reach.add(nk)
                        stack.append(nk)
            return main, reach

        main_nodes, seen = main_area(graph)
        _, seen_aliens = main_area(graph_any)
        inside = sum(1 for n in valid if n in main_nodes)
        stranded = sum(1 for n in valid if n not in seen)
        print(f"main area: {len(main_nodes)} waypoints, spawns inside {inside}/{len(valid)}, "
              f"spawn platforms {len(valid) - inside - stranded}, stranded spawns {stranded}")
        # spawns bots should not use (written to the .nav): not on the graph, or cut off from the main area
        for s, n in zip(starts, start_nodes):
            who = "all" if n is None else ("all" if n not in seen and n not in seen_aliens else
                                           "walkers" if n not in seen else "aliens" if n not in seen_aliens else None)
            if who:
                badspawns.append((s[0], s[1], s[2], who))
            if n is not None and n not in seen:
                print(f"stranded spawn at {s[0]:.0f} {s[1]:.0f} {s[2]:.0f} (waypoint {n} at "
                      f"{nodes[n][0]:.0f} {nodes[n][1]:.0f} {nodes[n][2]:.0f}), aliens "
                      f"{'too' if n not in seen_aliens else 'can climb out'}")

        # keep only waypoints reachable from a spawn AND from which a spawn can be reached again
        # (wall tops, outer slopes and pits with no exit are useless or traps for bots)
        def bfs(adjacency, sources):
            seen, stack = set(sources), list(sources)
            while stack:
                k = stack.pop()
                for nk in adjacency[k]:
                    if nk not in seen:
                        seen.add(nk)
                        stack.append(nk)
            return seen

        reverse = collections.defaultdict(set)
        for k, vs in graph_any.items():
            for v in vs:
                reverse[v].add(k)
        fwd = bfs(graph_any, set(valid))
        back = bfs(reverse, set(valid))
        keep = fwd & back
        traps = fwd - back
        print(f"waypoints reachable from spawns: {len(fwd)}; with a way back: {len(keep)}; traps dropped: {len(traps)}; "
              f"unreachable dropped: {len(nodes) - len(fwd)}")
        remap = {old: new for new, old in enumerate(sorted(keep))}
        nodes = [nodes[old] for old in sorted(keep)]
        edges = [(remap[ea], remap[eb], p, kind) for ea, eb, p, kind in edges if ea in remap and eb in remap]
        owner = {k: remap[o] for k, o in owner.items() if o in remap}
        comp = {k: 0 for k in owner}  # render: kept = main
        main_comp = 0
        kinds = collections.Counter(e[3] for e in edges)
        print(f"final graph: {len(nodes)} waypoints, {len(edges)} edges {dict(kinds)}")

    nav_path = os.path.join(out, mapname + ".nav")
    png_path = os.path.join(out, mapname + "_nav.png")
    write_nav(nav_path, mapname, P, nodes, edges, badspawns, info.use_doors)
    render(png_path, P, bounds, spans_all, spans, comp, main_comp, nodes, edges, starts, hits, tris)
    print("wrote", nav_path, "and", png_path)


if __name__ == "__main__":
    main()
