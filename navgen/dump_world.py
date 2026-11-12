"""dump_world.py <map.DAT> [more.DAT ...] [--out DIR] [--width PX]

Dumps an AvP2 (LithTech Talon, version 70) world file into avp2bots/maps/out/:
    <map>.obj            collision geometry; main world, terrain and each world model as `o` groups
    <map>.mtl            colours for the OBJ surface categories
    <map>.objects.json   every object with all its properties (+ the world-model table)
    <map>_top.png        top-down render (X to the right, +Z up the image, looking down -Y)
and prints a summary.  Standard library + Pillow only.
"""
from __future__ import annotations

import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dat70  # noqa: E402

DEFAULT_OUT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                            "..", "maps", "out"))

START_CLASSES_HINT = ("StartPoint",)          # GameStartPoint in AvP2
PICKUP_CLASSES = ("PickupObject", "AmmoBox")
GAME_MODE_PROPS = ("SP_Story", "MP_DM", "MP_TeamDM", "MP_Hunt", "MP_Survivor",
                   "MP_Overrun", "MP_Evac")


def is_start_class(cls: str) -> bool:
    return any(h in cls for h in START_CLASSES_HINT)


def surf_category(flags: int) -> str:
    if flags & (dat70.SURF_SKY):
        return "sky"
    if not flags & dat70.SURF_SOLID:
        return "nonsolid"
    if flags & dat70.SURF_INVISIBLE:
        return "invisible_solid"
    return "solid"


# ------------------------------------------------------------------ OBJ ------

MTL = """# surface categories written by dump_world.py
newmtl solid
Kd 0.70 0.70 0.68
newmtl invisible_solid
Kd 0.95 0.55 0.20
d 0.6
newmtl sky
Kd 0.45 0.65 0.95
d 0.3
newmtl nonsolid
Kd 0.30 0.85 0.45
d 0.4
"""


def write_obj(w: dat70.World, path: str, mtl_name: str):
    """All geometry is written in file coordinates (x y z, +Y up, left-handed).
    cross(v1-v0, v2-v0) points along the plane normal (out of solid), so OBJ viewers
    get correct face normals; a right-handed viewer shows the level mirrored."""
    groups = []
    for m in w.static_models:
        groups.append(("%s_%s" % (w.model_kind(m), m.name), m))
    for m in w.world_models:
        groups.append(("%s_%s" % (w.model_kind(m), m.name), m))
    with open(path, "w", encoding="ascii", errors="replace", newline="\n") as f:
        f.write("# %s - collision geometry, LithTech DAT v%d\n" % (os.path.basename(w.path), w.version))
        f.write("# units: game units; +Y up; left-handed (X right, Z forward)\n")
        f.write("# groups: world_* = PhysicsBSP, terrain_* = static terrain, others = world models\n")
        f.write("#         (door_/brush_ block when blocks=1; ladder_/liquid_/volume_ never block)\n")
        f.write("mtllib %s\n" % mtl_name)
        base = 1
        for gname, m in groups:
            f.write("\no %s\n" % gname.replace(" ", "_"))
            f.write("# class=%s kind=%s blocks=%d moveable=%d polys=%d\n"
                    % (m.object_class or "-", w.model_kind(m), int(w.model_blocks(m)),
                       int(m.moveable), len(m.polys)))
            for x, y, z in m.points:
                f.write("v %.4f %.4f %.4f\n" % (x, y, z))
            mesh = m.mesh()
            cur = None
            for ti, (a, b, c) in enumerate(mesh.tris):
                cat = surf_category(mesh.tri_surface_flags[ti])
                if cat != cur:
                    f.write("usemtl %s\n" % cat)
                    cur = cat
                f.write("f %d %d %d\n" % (a + base, b + base, c + base))
            base += len(m.points)


# ------------------------------------------------------------------ JSON -----

def model_record(w: dat70.World, m: dat70.WorldModel) -> dict:
    flags_or = 0
    flags_and = 0xFFFFFFFF
    for p in m.polys:
        fl = m.surfaces[p.surface].flags
        flags_or |= fl
        flags_and &= fl
    mesh = m.mesh()
    return {
        "index": m.index, "name": m.name, "kind": w.model_kind(m),
        "object_class": m.object_class, "object_index": m.object_index,
        "blocks": w.model_blocks(m), "moveable": m.moveable,
        "info_flags": m.info_flags,
        "bbox_min": list(m.min_box), "bbox_max": list(m.max_box),
        "center": list(m.translation),
        "polys": len(m.polys), "tris": len(mesh.tris), "points": len(m.points),
        "all_solid": bool(flags_and & dat70.SURF_SOLID) if m.polys else False,
        "any_invisible": bool(flags_or & dat70.SURF_INVISIBLE),
        "any_sky": bool(flags_or & dat70.SURF_SKY),
    }


def write_json(w: dat70.World, path: str):
    model_of_object = {m.object_index: m for m in w.models if m.object_index is not None}
    objs = []
    for o in w.objects:
        d = o.to_dict()
        m = model_of_object.get(o.index)
        if m is not None:
            # The engine re-centres a brush object on its geometry (se_InitWorldModel).
            d["world_model"] = m.index
            d["world_model_kind"] = w.model_kind(m)
            d["runtime_pos"] = list(m.translation)
            d["bbox_min"] = list(m.min_box)
            d["bbox_max"] = list(m.max_box)
        objs.append(d)
    doc = {
        "map": os.path.basename(w.path),
        "version": w.version,
        "coordinate_system": "left-handed, +Y up, X/Z ground plane, game units; "
                             "rotations are Euler radians (pitch X, yaw Y, roll Z), "
                             "forward = (sin yaw * cos pitch, -sin pitch, cos yaw * cos pitch)",
        "header": {"object_data_pos": w.object_data_pos, "render_data_pos": w.render_data_pos,
                   "world_models_pos": w.world_models_pos, "info_string": w.info_string,
                   "extents_min": list(w.extents_min), "extents_max": list(w.extents_max)},
        "class_counts": w.class_counts(),
        "world_models": [model_record(w, m) for m in w.models],
        "objects": objs,
    }
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=1)


# ------------------------------------------------------------------ PNG ------

def _nice_length(target: float) -> float:
    e = 10 ** math.floor(math.log10(target))
    for k in (5, 2.5, 2, 1):
        if k * e <= target:
            return k * e
    return e


def _lerp(c0, c1, t):
    return tuple(int(round(a + (b - a) * t)) for a, b in zip(c0, c1))


def render_top(w: dat70.World, path: str, width: int = 1024):
    from PIL import Image, ImageDraw, ImageFont

    SS = 2                                         # supersampling
    BG = (18, 20, 24)
    FLOOR_LO, FLOOR_HI = (52, 78, 96), (214, 226, 220)     # low -> high floors
    SLOPE_TINT = (196, 170, 120)
    CEIL = (44, 40, 52)
    WALL = (10, 10, 12)
    C_START, C_START_OFF = (60, 220, 90), (150, 160, 150)
    C_LADDER, C_DOOR = (255, 150, 30), (235, 70, 200)
    C_PICKUP, C_AMMO = (255, 225, 60), (70, 200, 255)
    C_BRUSH, C_VOLUME = (120, 150, 200), (230, 70, 60)
    TEXT = (225, 228, 232)

    # triangles: static world + blocking brushes
    static = w.collision_mesh(solid_only=True)
    V = static.vertices
    tris = []
    for ti, (a, b, c) in enumerate(static.tris):
        if static.tri_surface_flags[ti] & dat70.SURF_SKY:
            continue
        tris.append((V[a], V[b], V[c], static.tri_normal[ti]))
    if not tris:
        raise SystemExit("no geometry to draw")

    xs = [p[0] for t in tris for p in t[:3]]
    ys = [p[1] for t in tris for p in t[:3]]
    zs = [p[2] for t in tris for p in t[:3]]
    for o in w.objects:                            # keep markers inside the frame
        if (is_start_class(o.cls) or o.cls in PICKUP_CLASSES) and o.pos:
            xs.append(o.pos[0]); zs.append(o.pos[2])
    minx, maxx, minz, maxz = min(xs), max(xs), min(zs), max(zs)
    miny, maxy = min(ys), max(ys)
    floor_ys = [max(p[1] for p in t[:3]) for t in tris if t[3][1] > 0.7]
    f_lo, f_hi = (min(floor_ys), max(floor_ys)) if floor_ys else (miny, maxy)

    margin = 28
    footer = 152
    scale = (width - 2 * margin) / (maxx - minx)
    map_h = int(math.ceil((maxz - minz) * scale))
    height = map_h + 2 * margin + footer
    W, H = width * SS, height * SS

    def px(x, z):
        return ((margin + (x - minx) * scale) * SS, (margin + (maxz - z) * scale) * SS)

    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)

    # painter's algorithm, bottom to top; ceilings first so they never hide a floor
    ceil = [t for t in tris if t[3][1] < -0.7]
    rest = [t for t in tris if t[3][1] >= -0.7]
    for t in ceil:
        d.polygon([px(p[0], p[2]) for p in t[:3]], fill=CEIL)
    rest.sort(key=lambda t: max(p[1] for p in t[:3]))
    for t in rest:
        ny = t[3][1]
        pts = [px(p[0], p[2]) for p in t[:3]]
        if ny > 0.7:
            y = max(p[1] for p in t[:3])
            k = (y - f_lo) / (f_hi - f_lo) if f_hi > f_lo else 0.5
            col = _lerp(FLOOR_LO, FLOOR_HI, k)
            if ny < 0.98:                          # ramp / sloped ground
                col = _lerp(col, SLOPE_TINT, 0.45)
            d.polygon(pts, fill=col)
        else:                                      # wall (or steep slope): outline only
            d.line(pts + [pts[0]], fill=WALL, width=SS)

    def box_outline(m, colour, wd=2, fill=None):
        (x0, _, z0), (x1, _, z1) = m.min_box, m.max_box
        a, b = px(x0, z1), px(x1, z0)
        # keep tiny brushes visible
        if b[0] - a[0] < 4 * SS: a, b = (a[0] - 2 * SS, a[1]), (b[0] + 2 * SS, b[1])
        if b[1] - a[1] < 4 * SS: a, b = (a[0], a[1] - 2 * SS), (b[0], b[1] + 2 * SS)
        d.rectangle([a, b], outline=colour, width=wd * SS, fill=fill)

    counts = {"brush": 0, "door": 0, "ladder": 0, "volume": 0, "liquid": 0}
    for m in w.world_models:
        kind = w.model_kind(m)
        if kind == "brush":
            if not w.model_blocks(m) or m.name.lower() == "skybox":
                continue
            # skip brushes that are just big invisible lids / sky boxes
            if (m.max_box[0] - m.min_box[0]) > 0.6 * (maxx - minx) and \
               (m.max_box[2] - m.min_box[2]) > 0.6 * (maxz - minz):
                continue
            box_outline(m, C_BRUSH, 1)
            counts["brush"] += 1
    for m in w.world_models:
        kind = w.model_kind(m)
        if kind in ("volume", "liquid"):
            if (m.max_box[0] - m.min_box[0]) > 0.9 * (maxx - minx):
                counts[kind] += 1
                continue                           # map-wide kill plane etc.: legend only
            box_outline(m, C_VOLUME, 1)
            counts[kind] += 1
        elif kind == "door":
            box_outline(m, C_DOOR, 2)
            counts["door"] += 1
        elif kind == "ladder":
            box_outline(m, C_LADDER, 2, fill=C_LADDER)
            counts["ladder"] += 1

    n_pick = n_ammo = n_start = n_start_off = 0
    for o in w.objects:
        if not o.pos:
            continue
        x, y = px(o.pos[0], o.pos[2])
        if o.cls == "PickupObject":
            r = 3.5 * SS
            d.ellipse([x - r, y - r, x + r, y + r], fill=C_PICKUP, outline=(0, 0, 0))
            n_pick += 1
        elif o.cls == "AmmoBox":
            r = 3 * SS
            d.rectangle([x - r, y - r, x + r, y + r], fill=C_AMMO, outline=(0, 0, 0))
            n_ammo += 1
    for o in w.objects:
        if not (is_start_class(o.cls) and o.pos):
            continue
        x, y = px(o.pos[0], o.pos[2])
        mp = any(o.get(k) for k in GAME_MODE_PROPS if k.startswith("MP_"))
        yaw = (o.euler or (0, 0, 0))[1]
        fx, fz = math.sin(yaw), math.cos(yaw)       # forward in X/Z; +Z is up the image
        r = 7 * SS
        tip = (x + fx * r * 1.5, y - fz * r * 1.5)
        left = (x - fz * r * 0.8 - fx * r * 0.6, y - fx * r * 0.8 + fz * r * 0.6)
        right = (x + fz * r * 0.8 - fx * r * 0.6, y + fx * r * 0.8 + fz * r * 0.6)
        if mp:
            d.polygon([tip, left, right], fill=C_START, outline=(0, 0, 0))
            n_start += 1
        else:
            d.polygon([tip, left, right], outline=C_START_OFF)
            n_start_off += 1

    # ---- footer: title, scale bar, legend
    try:
        font = ImageFont.truetype("arial.ttf", 13 * SS)
        font_b = ImageFont.truetype("arialbd.ttf", 15 * SS)
    except OSError:
        font = font_b = ImageFont.load_default()
    fy = (map_h + 2 * margin) * SS
    d.line([(margin * SS, fy), (W - margin * SS, fy)], fill=(70, 74, 82), width=SS)
    name = os.path.splitext(os.path.basename(w.path))[0]
    d.text((margin * SS, fy + 8 * SS), "%s  -  top-down collision map" % name, fill=TEXT, font=font_b)
    d.text((margin * SS, fy + 30 * SS),
           "X %.0f..%.0f   Z %.0f..%.0f   floors Y %.0f..%.0f"
           % (minx, maxx, minz, maxz, f_lo, f_hi), fill=(160, 166, 174), font=font)
    d.text((margin * SS, fy + 124 * SS), "+X right, +Z up the image, +Y (height) towards the viewer",
           fill=(160, 166, 174), font=font)

    bar_units = _nice_length((maxx - minx) / 5.0)
    bx0, by = margin * SS, fy + 62 * SS
    bx1 = bx0 + bar_units * scale * SS
    d.rectangle([bx0, by, bx1, by + 5 * SS], fill=TEXT)
    for xx in (bx0, (bx0 + bx1) / 2, bx1):
        d.line([(xx, by - 4 * SS), (xx, by + 5 * SS)], fill=TEXT, width=SS)
    d.text((bx0, by + 9 * SS), "0", fill=TEXT, font=font)
    d.text((bx1 - 10 * SS, by + 9 * SS), "%g units" % bar_units, fill=TEXT, font=font)

    # height ramp
    rx0, ry = bx0, fy + 100 * SS
    rw = 160 * SS
    for i in range(rw):
        d.line([(rx0 + i, ry), (rx0 + i, ry + 8 * SS)], fill=_lerp(FLOOR_LO, FLOOR_HI, i / rw))
    d.text((rx0 + rw + 8 * SS, ry - 4 * SS), "floor height %.0f -> %.0f" % (f_lo, f_hi), fill=TEXT, font=font)

    legend = [
        ("tri", C_START, "start point, MP (%d)" % n_start),
        ("tri_o", C_START_OFF, "start point, SP only (%d)" % n_start_off),
        ("dot", C_PICKUP, "PickupObject (%d)" % n_pick),
        ("sq", C_AMMO, "AmmoBox (%d)" % n_ammo),
        ("sq", C_LADDER, "ladder volume (%d)" % counts["ladder"]),
        ("box", C_DOOR, "door (%d)" % counts["door"]),
        ("box", C_BRUSH, "solid brush model (%d)" % counts["brush"]),
        ("box", C_VOLUME, "volume / liquid (%d)" % (counts["volume"] + counts["liquid"])),
        ("sq", SLOPE_TINT, "sloped floor"),
        ("line", (120, 120, 126), "wall edge"),
    ]
    lx0 = int(width * 0.53) * SS
    col_w = 235 * SS
    for i, (shape, col, label) in enumerate(legend):
        lx = lx0 + (i // 5) * col_w
        ly = fy + (10 + (i % 5) * 22) * SS
        cx, cy, r = lx + 8 * SS, ly + 8 * SS, 6 * SS
        if shape == "tri":
            d.polygon([(cx, cy - r), (cx - r, cy + r), (cx + r, cy + r)], fill=col, outline=(0, 0, 0))
        elif shape == "tri_o":
            d.polygon([(cx, cy - r), (cx - r, cy + r), (cx + r, cy + r)], outline=col)
        elif shape == "dot":
            d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=col)
        elif shape == "sq":
            d.rectangle([cx - r, cy - r, cx + r, cy + r], fill=col)
        elif shape == "box":
            d.rectangle([cx - r, cy - r, cx + r, cy + r], outline=col, width=2 * SS)
        else:
            d.line([(cx - r, cy), (cx + r, cy)], fill=col, width=2 * SS)
        d.text((lx + 24 * SS, ly), label, fill=TEXT, font=font)

    img = img.resize((width, height), Image.LANCZOS)
    img.save(path)
    return {"scale_px_per_unit": scale, "bounds_xz": (minx, maxx, minz, maxz)}


# ------------------------------------------------------------------ summary --

def floor_checks(w: dat70.World) -> list:
    """Ray straight down from every start point onto static world + blocking brushes."""
    mesh = w.collision_mesh(solid_only=True, include_brushes=True)
    out = []
    for o in w.objects:
        if is_start_class(o.cls) and o.pos:
            hit = dat70.raycast_down(mesh, o.pos)
            mp = [k for k in GAME_MODE_PROPS if o.get(k)]
            out.append((o, hit[0] if hit else None, mp))
    return out


def summarize(w: dat70.World) -> str:
    L = []
    name = os.path.basename(w.path)
    L.append("== %s (version %d) ==" % (name, w.version))
    L.append("header: objects @ %d, render data @ %d, world models @ %d; info '%s'"
             % (w.object_data_pos, w.render_data_pos, w.world_models_pos, w.info_string.strip()))
    stat = w.collision_mesh(solid_only=True)
    used = {i for t in stat.tris for i in t}
    L.append("static collision (solid surfaces): %d triangles, %d vertices used" % (len(stat.tris), len(used)))
    for m in w.static_models:
        mm = m.mesh()
        cats = {}
        for fl in mm.tri_surface_flags:
            c = surf_category(fl)
            cats[c] = cats.get(c, 0) + 1
        L.append("  %-12s %-8s %5d polys %5d tris %5d points  %s"
                 % (m.name, w.model_kind(m), len(m.polys), len(mm.tris), len(m.points), cats))
    nosky = [(a, b, c) for ti, (a, b, c) in enumerate(stat.tris)
             if not stat.tri_surface_flags[ti] & dat70.SURF_SKY]
    vs = [stat.vertices[i] for t in nosky for i in t]
    if vs:
        lo = tuple(min(v[k] for v in vs) for k in range(3))
        hi = tuple(max(v[k] for v in vs) for k in range(3))
        L.append("bbox (solid, no sky): min (%.0f, %.0f, %.0f) max (%.0f, %.0f, %.0f)  size %.0f x %.0f x %.0f"
                 % (lo + hi + tuple(h - l for l, h in zip(lo, hi))))
    L.append("bbox (file extents):  min (%.0f, %.0f, %.0f) max (%.0f, %.0f, %.0f)"
             % (tuple(w.extents_min) + tuple(w.extents_max)))
    es = dat70.edge_stats(stat)
    L.append("watertightness: %d polygons, %d boundary sub-edges, %.1f%% unmatched (%.1f%% by length)"
             % (es["polygons"], es["sub_edges"], 100 * es["open_fraction"], 100 * es["open_length_fraction"]))

    # flat floor heights
    hist = {}
    for ti, (a, b, c) in enumerate(stat.tris):
        if stat.tri_normal[ti][1] > 0.99 and not stat.tri_surface_flags[ti] & dat70.SURF_SKY:
            A, B, C = stat.vertices[a], stat.vertices[b], stat.vertices[c]
            area = abs((B[0] - A[0]) * (C[2] - A[2]) - (C[0] - A[0]) * (B[2] - A[2])) / 2
            hist[round(A[1])] = hist.get(round(A[1]), 0) + area
    top = sorted(hist.items(), key=lambda kv: -kv[1])[:8]
    L.append("largest flat floors (Y: area): " + ", ".join("%d: %.0f" % kv for kv in top))

    kinds = {}
    tri_wm = 0
    for m in w.world_models:
        k = w.model_kind(m) + ("" if w.model_blocks(m) else "(non-blocking)")
        kinds[k] = kinds.get(k, 0) + 1
        tri_wm += len(m.mesh().tris)
    L.append("world models: %d (%d triangles): %s" % (len(w.world_models), tri_wm, kinds))
    L.append("objects: %d" % len(w.objects))
    for cls, n in w.class_counts().items():
        L.append("  %4d  %s" % (n, cls))

    checks = floor_checks(w)
    classes = sorted({o.cls for o, _, _ in checks})
    L.append("start-point classes: %s" % (", ".join(classes) or "(none found)"))
    by_mode = {}
    for o, _, modes in checks:
        for k in modes:
            by_mode[k] = by_mode.get(k, 0) + 1
    L.append("  %d start points; enabled per game mode: %s" % (len(checks), by_mode))
    dists = sorted(d for _, d, _ in checks if d is not None)
    miss = [o.name for o, d, _ in checks if d is None]
    if dists:
        L.append("  ray down to floor: min %.1f  median %.1f  max %.1f  (no floor: %d)"
                 % (dists[0], dists[len(dists) // 2], dists[-1], len(miss)))
        far = [(o.name, d) for o, d, _ in checks if d is not None and d > 150]
        if far:
            L.append("  more than 150 above a floor: " + ", ".join("%s %.0f" % f for f in far))
    if miss:
        L.append("  no floor below: " + ", ".join(miss))
    for s in w.warnings:
        L.append("WARNING: " + s)
    return "\n".join(L)


def main(argv):
    out = DEFAULT_OUT
    width = 1024
    paths = []
    it = iter(argv)
    for a in it:
        if a == "--out":
            out = next(it)
        elif a == "--width":
            width = int(next(it))
        else:
            paths.append(a)
    if not paths:
        print(__doc__)
        return 2
    os.makedirs(out, exist_ok=True)
    for p in paths:
        w = dat70.load(p)
        base = os.path.splitext(os.path.basename(p))[0]
        write_obj(w, os.path.join(out, base + ".obj"), base + ".mtl")
        with open(os.path.join(out, base + ".mtl"), "w", encoding="ascii", newline="\n") as f:
            f.write(MTL)
        write_json(w, os.path.join(out, base + ".objects.json"))
        render_top(w, os.path.join(out, base + "_top.png"), width)
        print(summarize(w))
        print("wrote %s.{obj,mtl,objects.json} and %s_top.png in %s\n" % (base, base, out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
