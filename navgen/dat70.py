"""dat70.py - reader for LithTech "Talon" world files (.DAT, version 70) as shipped
with Aliens vs. Predator 2 (retail 1.0.9.6).

Standard library only.  See FORMAT.md (same folder) for the byte-level layout and
the reference sources each part was taken from.

Coordinate system (as stored in the file, and as used by the engine):
    left-handed, +Y is UP, X/Z is the ground plane, 1 unit = 1 DEdit/game unit.
    Polygon vertices are wound clockwise when seen from the front (the side the
    plane normal points to); the plane normal points away from solid, into the
    walkable space.

Quick use:
    import dat70
    w = dat70.load("DM_Compound.DAT")
    mesh = w.collision_mesh()             # main world (PhysicsBSP), world space
    for wm in w.world_models: ...          # doors / movers / volumes, with names
    for o in w.objects: o.cls, o.props, o.pos, o.euler
"""
from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field

WORLD_VERSION = 70

# ---------------------------------------------------------------- flags ------

# WorldBsp::m_WorldInfoFlags (de_world.h in AVP2-Reconstructed)
WIF_MOVEABLE = 1 << 1      # engine keeps a second, transformable copy (doors, movers)
WIF_MAINWORLD = 1 << 2
WIF_TERRAIN = 1 << 3
WIF_PHYSICSBSP = 1 << 4    # the collision BSP of the main world
WIF_VISBSP = 1 << 5        # the visibility/render BSP of the main world

# Surface flags (LT2 SDK ltbasedefs.h)
SURF_SOLID = 1 << 0
SURF_NONEXISTANT = 1 << 1
SURF_INVISIBLE = 1 << 2
SURF_TRANSPARENT = 1 << 3
SURF_SKY = 1 << 4
SURF_BRIGHT = 1 << 5
SURF_FLATSHADE = 1 << 6
SURF_LIGHTMAP = 1 << 7
SURF_NOSUBDIV = 1 << 8
SURF_HULLMAKER = 1 << 9
SURF_ALWAYSLIGHTMAP = 1 << 10
SURF_DIRECTIONALLIGHT = 1 << 11
SURF_GOURAUDSHADE = 1 << 12
SURF_PORTAL = 1 << 13
SURF_SPRITEANIMATE = 1 << 14
SURF_PANNINGSKY = 1 << 15
SURF_XZONLY = 1 << 16
SURF_PHYSICSFIX = 1 << 17
SURF_TERRAINOCCLUDER = 1 << 18
SURF_ADDITIVE = 1 << 19
SURF_TIMEOFDAY = 1 << 20
SURF_VISBLOCKER = 1 << 21
SURF_NOTASTEP = 1 << 22
SURF_NOWALLWALK = 1 << 23
SURF_NOBLOCKLIGHT = 1 << 24

SURF_NAMES = {
    SURF_SOLID: "SOLID", SURF_NONEXISTANT: "NONEXISTANT", SURF_INVISIBLE: "INVISIBLE",
    SURF_TRANSPARENT: "TRANSPARENT", SURF_SKY: "SKY", SURF_BRIGHT: "BRIGHT",
    SURF_FLATSHADE: "FLATSHADE", SURF_LIGHTMAP: "LIGHTMAP", SURF_NOSUBDIV: "NOSUBDIV",
    SURF_HULLMAKER: "HULLMAKER", SURF_ALWAYSLIGHTMAP: "ALWAYSLIGHTMAP",
    SURF_DIRECTIONALLIGHT: "DIRECTIONALLIGHT", SURF_GOURAUDSHADE: "GOURAUDSHADE",
    SURF_PORTAL: "PORTAL", SURF_SPRITEANIMATE: "SPRITEANIMATE",
    SURF_PANNINGSKY: "PANNINGSKY", SURF_XZONLY: "XZONLY", SURF_PHYSICSFIX: "PHYSICSFIX",
    SURF_TERRAINOCCLUDER: "TERRAINOCCLUDER", SURF_ADDITIVE: "ADDITIVE",
    SURF_TIMEOFDAY: "TIMEOFDAY", SURF_VISBLOCKER: "VISBLOCKER",
    SURF_NOTASTEP: "NOTASTEP", SURF_NOWALLWALK: "NOWALLWALK",
    SURF_NOBLOCKLIGHT: "NOBLOCKLIGHT",
}

# Property types (LT2 SDK ltbasedefs.h)
PT_STRING, PT_VECTOR, PT_COLOR, PT_REAL, PT_FLAGS, PT_BOOL, PT_LONGINT, PT_ROTATION = range(8)
PT_NAMES = ["string", "vector", "color", "real", "flags", "bool", "longint", "rotation"]

# Object classes whose brush is a non-blocking container volume
# (avp2src ObjectDLL/VolumeBrush.cpp and VolumeBrushTypes.cpp; all derive from VolumeBrush).
LIQUID_CLASSES = {"Water", "CorrosiveFluid", "FreezingWater"}
LADDER_CLASSES = {"Ladder"}
VOLUME_CLASSES = {"VolumeBrush", "Ice", "PoisonGas", "Electricity", "EndlessFall",
                  "Wind", "ColdAir", "Weather"}


def surf_flag_names(flags: int) -> list[str]:
    return [n for b, n in SURF_NAMES.items() if flags & b]


# ---------------------------------------------------------------- reader -----

class DatError(Exception):
    pass


class _R:
    """Little-endian cursor over a bytes object."""
    __slots__ = ("d", "p")

    def __init__(self, data: bytes, pos: int = 0):
        self.d = data
        self.p = pos

    def _u(self, fmt, n):
        if self.p + n > len(self.d):
            raise DatError("read past end of file at %d" % self.p)
        v = struct.unpack_from(fmt, self.d, self.p)
        self.p += n
        return v

    def u8(self): return self._u("<B", 1)[0]
    def u16(self): return self._u("<H", 2)[0]
    def u32(self): return self._u("<I", 4)[0]
    def i32(self): return self._u("<i", 4)[0]
    def f32(self): return self._u("<f", 4)[0]
    def vec(self): return self._u("<3f", 12)

    def raw(self, n):
        if self.p + n > len(self.d):
            raise DatError("read past end of file at %d" % self.p)
        b = self.d[self.p:self.p + n]
        self.p += n
        return b

    def lstr(self):
        """ILTStream::ReadString: u16 length + bytes (no terminator)."""
        n = self.u16()
        return self.raw(n).decode("latin-1")

    def cstr(self):
        e = self.d.index(b"\0", self.p)
        s = self.d[self.p:e].decode("latin-1")
        self.p = e + 1
        return s


# ---------------------------------------------------------------- model ------

@dataclass
class Surface:
    O: tuple
    P: tuple
    Q: tuple
    texture_index: int
    flags: int
    unknown8: int
    color: tuple
    effect: str | None
    effect_param: str | None
    texture_flags: int
    texture: str = ""

    @property
    def solid(self): return bool(self.flags & SURF_SOLID)
    @property
    def invisible(self): return bool(self.flags & SURF_INVISIBLE)
    @property
    def sky(self): return bool(self.flags & (SURF_SKY | SURF_PANNINGSKY))


@dataclass
class Poly:
    index: int
    center: tuple
    lm_width: int
    lm_height: int
    surface: int
    plane: int
    verts: list            # point indices of the base ring (the true polygon)
    extra_verts: list      # point indices of the T-junction-fixed ring (render only)
    O: tuple = (0, 0, 0)
    P: tuple = (0, 0, 0)
    Q: tuple = (0, 0, 0)


@dataclass
class Node:
    poly: int
    leaf: int
    sides: tuple           # child node index, -1 = NODE_IN (solid), -2 = NODE_OUT (empty)


@dataclass
class Portal:
    name: str
    unknown: tuple
    center: tuple
    dims: tuple


@dataclass
class WorldModel:
    """One WorldBsp record: the main world's PhysicsBSP / VisBSP, or a brush object."""
    index: int
    file_pos: int
    name: str
    info_flags: int
    max_tree_depth: int
    min_box: tuple
    max_box: tuple
    translation: tuple             # centre of the model; the engine places the object here
    textures: list
    points: list                   # [(x, y, z)] world space
    planes: list                   # [(nx, ny, nz, dist)]
    surfaces: list
    polys: list
    nodes: list
    root_node: int
    portals: list
    n_leafs: int = 0
    n_terrain_sections: int = 0
    # filled in by World after the objects are read:
    object_class: str | None = None
    object_index: int | None = None

    @property
    def is_physics_bsp(self): return bool(self.info_flags & WIF_PHYSICSBSP)
    @property
    def is_vis_bsp(self): return bool(self.info_flags & WIF_VISBSP)
    @property
    def is_terrain(self): return bool(self.info_flags & WIF_TERRAIN)
    @property
    def is_main_world(self): return bool(self.info_flags & (WIF_MAINWORLD | WIF_PHYSICSBSP | WIF_VISBSP))
    @property
    def is_static(self):
        """Never moves: main world BSPs and terrain (no WIF_MOVEABLE copy in the engine)."""
        return self.is_main_world or self.is_terrain
    @property
    def moveable(self): return bool(self.info_flags & WIF_MOVEABLE)

    def poly_normal(self, poly: Poly):
        return self.planes[poly.plane][:3]

    def poly_info(self, poly: Poly) -> dict:
        """Navigation-relevant classification of one polygon."""
        s = self.surfaces[poly.surface]
        return {
            "solid": s.solid,
            "invisible": s.invisible,
            "sky": s.sky,
            "nonexistant": bool(s.flags & SURF_NONEXISTANT),
            "physics_fix": bool(s.flags & SURF_PHYSICSFIX),
            "not_a_step": bool(s.flags & SURF_NOTASTEP),
            "no_wall_walk": bool(s.flags & SURF_NOWALLWALK),
            "transparent": bool(s.flags & SURF_TRANSPARENT),
            "flags": s.flags,
            "texture": s.texture,
        }

    def mesh(self, solid_only: bool = False, skip_sky: bool = False) -> "Mesh":
        """Fan-triangulate the base ring of every polygon (they are convex)."""
        m = Mesh(name=self.name, vertices=self.points)
        for poly in self.polys:
            s = self.surfaces[poly.surface]
            if solid_only and not s.solid:
                continue
            if skip_sky and s.sky:
                continue
            v = poly.verts
            n = self.planes[poly.plane][:3]
            for k in range(1, len(v) - 1):
                m.tris.append((v[0], v[k], v[k + 1]))
                m.tri_poly.append(poly.index)
                m.tri_model.append(self.index)
                m.tri_surface_flags.append(s.flags)
                m.tri_normal.append(n)
        return m


@dataclass
class Mesh:
    """Indexed triangle soup in world space.  tris index into vertices."""
    name: str
    vertices: list
    tris: list = field(default_factory=list)
    tri_poly: list = field(default_factory=list)           # source polygon index
    tri_model: list = field(default_factory=list)          # source WorldModel index
    tri_surface_flags: list = field(default_factory=list)  # SURF_* of the source polygon
    tri_normal: list = field(default_factory=list)         # plane normal of the source polygon

    def extend(self, other: "Mesh"):
        """Append another mesh (vertices are copied, indices rebased)."""
        if self.vertices is other.vertices:
            base = 0
        else:
            base = len(self.vertices)
            self.vertices = list(self.vertices) + list(other.vertices)
        self.tris.extend((a + base, b + base, c + base) for a, b, c in other.tris)
        self.tri_poly.extend(other.tri_poly)
        self.tri_model.extend(other.tri_model)
        self.tri_surface_flags.extend(other.tri_surface_flags)
        self.tri_normal.extend(other.tri_normal)
        return self

    def bbox(self):
        used = {i for t in self.tris for i in t}
        if not used:
            return None
        xs = [self.vertices[i][0] for i in used]
        ys = [self.vertices[i][1] for i in used]
        zs = [self.vertices[i][2] for i in used]
        return (min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs))


@dataclass
class Prop:
    name: str
    type: int
    flags: int
    value: object

    @property
    def type_name(self):
        return PT_NAMES[self.type] if 0 <= self.type < len(PT_NAMES) else "type%d" % self.type


@dataclass
class WorldObject:
    index: int
    cls: str
    props: list

    def get(self, name, default=None):
        for p in self.props:
            if p.name == name:
                return p.value
        return default

    @property
    def name(self): return self.get("Name", "")
    @property
    def pos(self):
        """World-space position ('Pos' property), or None."""
        v = self.get("Pos")
        return tuple(v) if v is not None else None
    @property
    def euler(self):
        """'Rotation' property: Euler angles in radians (pitch about X, yaw about Y,
        roll about Z), exactly as stored.  None if the object has no rotation."""
        v = self.get("Rotation")
        return tuple(v) if v is not None else None
    @property
    def yaw_deg(self):
        e = self.euler
        return math.degrees(e[1]) if e else None
    @property
    def forward(self):
        """World-space forward (+Z rotated by the Euler angles, yaw*pitch*roll)."""
        e = self.euler
        if not e:
            return None
        return euler_forward(*e)

    def to_dict(self):
        d = {"index": self.index, "class": self.cls, "name": self.name,
             "pos": list(self.pos) if self.pos else None,
             "euler_rad": list(self.euler) if self.euler else None,
             "yaw_deg": self.yaw_deg,
             "forward": list(self.forward) if self.euler else None,
             "quat_xyzw": list(euler_to_quat(*self.euler)) if self.euler else None,
             "properties": []}
        for p in self.props:
            v = p.value
            if isinstance(v, tuple):
                v = list(v)
            d["properties"].append({"name": p.name, "type": p.type_name,
                                    "flags": p.flags, "value": v})
        return d


def euler_forward(pitch, yaw, roll=0.0):
    """Forward vector for LithTech Euler angles (left-handed, Y up, +Z forward).
    Matrix = Yaw(Y) * Pitch(X) * Roll(Z); positive pitch looks down."""
    cp, sp = math.cos(pitch), math.sin(pitch)
    cy, sy = math.cos(yaw), math.sin(yaw)
    return (sy * cp, -sp, cy * cp)


def euler_to_quat(pitch, yaw, roll):
    """Quaternion (x, y, z, w) for Yaw(Y) * Pitch(X) * Roll(Z)."""
    cx, sx = math.cos(pitch / 2), math.sin(pitch / 2)
    cy, sy = math.cos(yaw / 2), math.sin(yaw / 2)
    cz, sz = math.cos(roll / 2), math.sin(roll / 2)
    return (cy * sx * cz + sy * cx * sz,
            sy * cx * cz - cy * sx * sz,
            cy * cx * sz - sy * sx * cz,
            cy * cx * cz + sy * sx * sz)


@dataclass
class World:
    path: str
    version: int
    object_data_pos: int
    render_data_pos: int
    header_dummies: tuple
    info_string: str
    lm_grid_size: float
    extents_min: tuple
    extents_max: tuple
    tree_box_min: tuple
    tree_box_max: tuple
    tree_num_nodes: int
    tree_terrain_depth: int
    world_models_pos: int
    models: list              # every WorldBsp record, in file order
    objects: list
    warnings: list = field(default_factory=list)

    # -- geometry accessors -------------------------------------------------
    @property
    def physics_bsp(self) -> WorldModel | None:
        for m in self.models:
            if m.is_physics_bsp:
                return m
        return None

    @property
    def vis_bsp(self) -> WorldModel | None:
        for m in self.models:
            if m.is_vis_bsp:
                return m
        return None

    @property
    def main_model(self) -> WorldModel:
        """The BSP the engine collides against: PhysicsBSP if present, else the main world."""
        return self.physics_bsp or next(
            (m for m in self.models if m.is_main_world), self.models[0])

    @property
    def static_models(self) -> list:
        """What never moves and always collides: the PhysicsBSP plus terrain models."""
        return [self.main_model] + [m for m in self.models if m.is_terrain and not m.is_main_world]

    @property
    def world_models(self) -> list:
        """Brush objects (doors, movers, glass, volumes, ladders, ...): every WorldBsp that
        is neither a main world BSP nor terrain."""
        return [m for m in self.models if not m.is_static]

    def collision_mesh(self, solid_only: bool = True, include_brushes: bool = False) -> Mesh:
        """World-space triangles of the static collision geometry (PhysicsBSP + terrain).
        include_brushes also adds every world model that blocks in its initial position
        (see model_blocks)."""
        out = Mesh(name="collision", vertices=[])
        models = list(self.static_models)
        if include_brushes:
            models += [m for m in self.world_models if self.model_blocks(m)]
        for m in models:
            out.extend(m.mesh(solid_only=solid_only))
        return out

    def model_by_name(self, name: str) -> WorldModel | None:
        ln = name.lower()
        for m in self.models:
            if m.name.lower() == ln:
                return m
        return None

    def objects_of(self, *classes) -> list:
        return [o for o in self.objects if o.cls in classes]

    def class_counts(self) -> dict:
        c = {}
        for o in self.objects:
            c[o.cls] = c.get(o.cls, 0) + 1
        return dict(sorted(c.items(), key=lambda kv: (-kv[1], kv[0])))

    def model_kind(self, m: WorldModel) -> str:
        """'world' | 'terrain' | 'liquid' | 'ladder' | 'volume' | 'door' | 'brush'."""
        if m.is_main_world:
            return "world"
        if m.is_terrain:
            return "terrain"
        c = m.object_class or ""
        if c in LADDER_CLASSES:
            return "ladder"
        if c in LIQUID_CLASSES:
            return "liquid"
        if c in VOLUME_CLASSES or "Volume" in c or "Trigger" in c:
            return "volume"
        if "Door" in c:
            return "door"
        return "brush"

    def model_blocks(self, m: WorldModel) -> bool:
        """True if the brush physically blocks players when in its initial position."""
        if m.is_static:
            return True
        if self.model_kind(m) in ("liquid", "ladder", "volume"):
            return False
        o = self.objects[m.object_index] if m.object_index is not None else None
        if o is not None and o.get("Solid") is not None:
            return bool(o.get("Solid"))
        return True


# ---------------------------------------------------------------- parsing ----

def _read_world_tree_layout(r: _R):
    """WorldTree::LoadLayout: box, counts, then one bit per node (1 = subdivide in 4)."""
    box_min = r.vec()
    box_max = r.vec()
    n_nodes = r.u32()
    terrain_depth = r.u32()
    cur_byte, cur_bit = 0, 8
    pending, count = 1, 0
    while pending:
        pending -= 1
        count += 1
        if cur_bit == 8:
            cur_byte = r.u8()
            cur_bit = 0
        if cur_byte & (1 << cur_bit):
            pending += 4
        cur_bit += 1
    return box_min, box_max, n_nodes, terrain_depth, count


def _read_pblock_table(r: _R):
    sx, sy, sz = r.u32(), r.u32(), r.u32()
    r.vec()          # block size
    r.vec()          # origin
    for _ in range(sx * sy * sz):
        n = r.u16()
        r.u16()      # root
        r.p += n * 6


def _read_world_bsp(r: _R, index: int) -> WorldModel:
    """w_LoadWorldBsp (AVP2-Reconstructed src/world/de_mainworld.cpp)."""
    start = r.p
    r.p += 32                                  # 8 unused u32
    info_flags = r.u32()
    max_tree_depth = r.u32()
    name = r.lstr()
    (n_points, n_planes, n_surfaces, n_portals, n_polies, n_leafs, n_verts,
     vis_list_size, n_leaf_lists, n_nodes, n_leaf_polies, n_poly_anim_refs) = \
        struct.unpack_from("<12I", r.d, r.p)
    r.p += 48
    min_box, max_box, translation = r.vec(), r.vec(), r.vec()

    # texture names
    r.u32()                                    # total length of the names
    n_textures = r.u32()
    textures = [r.cstr() for _ in range(n_textures)]

    # polygon vertex counts
    counts = []
    for _ in range(n_polies):
        counts.append((r.u8(), r.u8()))        # base ring, extra ring

    # leaves (visibility lists) - skipped
    for _ in range(n_leafs):
        n = r.u16()
        if n == 0xFFFF:
            r.u16()
        else:
            for _ in range(n):
                r.u16()                        # portal id
                size = r.u16()
                r.p += size
        n_lp = r.u32()
        r.p += 4 * n_lp
        r.u32()

    planes = [struct.unpack_from("<4f", r.d, r.p + 16 * i) for i in range(n_planes)]
    r.p += 16 * n_planes
    if r.p > len(r.d):
        raise DatError("planes run past end of file")

    surfaces = []
    for _ in range(n_surfaces):
        O, P, Q = r.vec(), r.vec(), r.vec()
        tex = r.u16()
        flags = r.u32()
        unk = r.u8()
        color = (r.u8(), r.u8(), r.u8())
        has_effect = r.u8()
        effect = effect_param = None
        if has_effect == 1:
            effect = r.lstr()
            effect_param = r.lstr()
        tex_flags = r.u16()
        surfaces.append(Surface(O, P, Q, tex, flags, unk, color, effect, effect_param,
                                tex_flags, textures[tex] if tex < len(textures) else ""))

    points = [struct.unpack_from("<3f", r.d, r.p + 12 * i) for i in range(n_points)]
    r.p += 12 * n_points
    if r.p > len(r.d):
        raise DatError("points run past end of file")

    polys = []
    for i in range(n_polies):
        center = r.vec()
        lm_w, lm_h = r.u16(), r.u16()
        n_refs = r.u16()
        r.p += 4 * n_refs                       # light-anim refs (model u16, poly u16)
        surf = r.u32()
        plane = r.u32()
        if surf >= n_surfaces or plane >= n_planes:
            raise DatError("bad surface/plane index in poly %d of %s" % (i, name))
        O, P, Q = r.vec(), r.vec(), r.vec()
        nb, ne = counts[i]
        idx = []
        for _ in range(nb + ne):
            pi = r.u16()
            r.p += 3                            # vertex colour b, g, r
            if pi >= n_points:
                raise DatError("bad point index in poly %d of %s" % (i, name))
            idx.append(pi)
        polys.append(Poly(i, center, lm_w, lm_h, surf, plane, idx[:nb], idx[nb:], O, P, Q))

    nodes = []
    for _ in range(n_nodes):
        poly = r.u32()
        leaf = r.u16()
        nodes.append(Node(poly, leaf, (r.i32(), r.i32())))

    portals = []
    for _ in range(n_portals):
        pname = r.lstr()
        unk = (r.u32(), r.u32(), r.u16())
        portals.append(Portal(pname, unk, r.vec(), r.vec()))

    _read_pblock_table(r)
    root = r.i32()
    n_sections = r.u32()
    # Terrain sections follow (nodes, poly list, PBlockTable, node path, centre).  They only
    # re-partition polygons already read above, so they are not parsed; the caller seeks to
    # the next model with the stored offset.

    return WorldModel(index, start, name, info_flags, max_tree_depth, min_box, max_box,
                      translation, textures, points, planes, surfaces, polys, nodes, root,
                      portals, n_leafs, n_sections)


def _read_objects(r: _R, warnings: list) -> list:
    n = r.u32()
    objs = []
    for i in range(n):
        data_len = r.u16()
        start = r.p
        cls = r.lstr()
        n_props = r.u32()
        props = []
        for _ in range(n_props):
            pname = r.lstr()
            ptype = r.u8()
            pflags = r.u32()
            plen = r.u16()
            pstart = r.p
            if ptype == PT_STRING:
                val = r.lstr()
            elif ptype in (PT_VECTOR, PT_COLOR):
                val = r.vec()
            elif ptype == PT_REAL:
                val = r.f32()
            elif ptype == PT_BOOL:
                val = bool(r.u8())
            elif ptype in (PT_FLAGS, PT_LONGINT):
                # Both are stored as a float32 holding the integer value.
                val = int(r.f32()) if plen == 4 else r.raw(plen).hex()
            elif ptype == PT_ROTATION:
                val = r.vec()                  # Euler angles, radians (pitch, yaw, roll)
                if plen == 16:
                    r.p += 4                   # fourth float, unused (always 0)
            else:
                val = r.raw(plen).hex()
                warnings.append("object %d (%s): unknown property type %d" % (i, cls, ptype))
            if r.p != pstart + plen:
                warnings.append("object %d (%s) prop %s: length %d but consumed %d"
                                % (i, cls, pname, plen, r.p - pstart))
                r.p = pstart + plen
            props.append(Prop(pname, ptype, pflags, val))
        if r.p != start + data_len:
            warnings.append("object %d (%s): data length %d but consumed %d"
                            % (i, cls, data_len, r.p - start))
            r.p = start + data_len
        objs.append(WorldObject(i, cls, props))
    return objs


def parse(data: bytes, path: str = "") -> World:
    r = _R(data)
    version = r.u32()
    if version != WORLD_VERSION:
        raise DatError("world version %d, expected %d" % (version, WORLD_VERSION))
    object_data_pos = r.u32()
    render_data_pos = r.u32()
    dummies = struct.unpack_from("<8I", data, r.p)
    r.p += 32
    info = r.raw(r.u32()).decode("latin-1")
    lm_grid = r.f32()
    ext_min, ext_max = r.vec(), r.vec()
    tb_min, tb_max, t_nodes, t_depth, _ = _read_world_tree_layout(r)

    warnings = []
    models_pos = r.p
    n_models = r.u32()
    models = []
    for i in range(n_models):
        next_pos = r.u32()
        m = _read_world_bsp(r, i)
        if r.p > next_pos:
            warnings.append("world model %d (%s) overran its record" % (i, m.name))
        if m.n_terrain_sections == 0 and r.p != next_pos:
            warnings.append("world model %d (%s): %d unparsed bytes"
                            % (i, m.name, next_pos - r.p))
        models.append(m)
        r.p = next_pos
    if r.p != object_data_pos:
        warnings.append("world models end at %d, object data at %d" % (r.p, object_data_pos))

    r.p = object_data_pos
    objects = _read_objects(r, warnings)
    if r.p != render_data_pos:
        warnings.append("objects end at %d, render data at %d" % (r.p, render_data_pos))

    # Tie brush objects to their geometry: the object's Name is the WorldBsp name.
    by_name = {}
    for m in models:
        by_name.setdefault(m.name.lower(), m)
    for o in objects:
        m = by_name.get(o.name.lower()) if o.name else None
        if m is not None and not m.is_main_world and m.object_index is None:
            m.object_class = o.cls
            m.object_index = o.index

    return World(path, version, object_data_pos, render_data_pos, dummies, info, lm_grid,
                 ext_min, ext_max, tb_min, tb_max, t_nodes, t_depth, models_pos, models,
                 objects, warnings)


def load(path: str) -> World:
    with open(path, "rb") as f:
        return parse(f.read(), path)


# ---------------------------------------------------------------- queries ----

def raycast_down(mesh: Mesh, origin, max_dist: float = 1e9, min_ny: float = 0.0):
    """Cast a ray straight down (-Y) from origin.  Returns (distance, triangle index)
    of the nearest hit on an upward-facing triangle (normal.y > min_ny), or None."""
    ox, oy, oz = origin
    best = None
    V = mesh.vertices
    for ti, (a, b, c) in enumerate(mesh.tris):
        if mesh.tri_normal[ti][1] <= min_ny:
            continue
        ax, ay, az = V[a]
        bx, by, bz = V[b]
        cx, cy, cz = V[c]
        if ox < min(ax, bx, cx) or ox > max(ax, bx, cx) or oz < min(az, bz, cz) or oz > max(az, bz, cz):
            continue
        d = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz)
        if abs(d) < 1e-12:
            continue
        u = ((bz - cz) * (ox - cx) + (cx - bx) * (oz - cz)) / d
        v = ((cz - az) * (ox - cx) + (ax - cx) * (oz - cz)) / d
        w = 1.0 - u - v
        e = -1e-6
        if u < e or v < e or w < e:
            continue
        y = u * ay + v * by + w * cy
        dist = oy - y
        if dist < -1e-3 or dist > max_dist:
            continue
        if best is None or dist < best[0]:
            best = (dist, ti)
    return best


def edge_stats(mesh: Mesh, quant: float = 0.05, tol: float = 0.05) -> dict:
    """Watertightness check on polygon boundaries.

    Vertices are welded by position, every polygon edge is split at any vertex lying on
    it (the BSP compiler splits polygons, so neighbours meet in T-junctions), and the
    resulting directed sub-edges are matched against their reverse.  In a closed surface
    every sub-edge a->b has exactly one partner b->a.  Returns edge counts and the
    unmatched ('open') share by count and by length."""
    weld, vid, pos = {}, [], []
    for (x, y, z) in mesh.vertices:
        k = (round(x / quant), round(y / quant), round(z / quant))
        i = weld.get(k)
        if i is None:
            i = weld[k] = len(pos)
            pos.append((x, y, z))
        vid.append(i)

    # polygon boundary edges (rebuild polygons from the triangle fans)
    polys = {}
    for ti, t in enumerate(mesh.tris):
        polys.setdefault((mesh.tri_model[ti], mesh.tri_poly[ti]), []).append(t)
    edges = []
    for tris in polys.values():
        ring = [tris[0][0], tris[0][1]] + [t[2] for t in tris]
        n = len(ring)
        for k in range(n):
            a, b = vid[ring[k]], vid[ring[(k + 1) % n]]
            if a != b:
                edges.append((a, b))

    used = {i for e in edges for i in e}
    cell = 64.0
    grid = {}
    for i in used:
        x, y, z = pos[i]
        grid.setdefault((int(x // cell), int(y // cell), int(z // cell)), []).append(i)

    count = {}
    for a, b in edges:
        ax, ay, az = pos[a]
        bx, by, bz = pos[b]
        dx, dy, dz = bx - ax, by - ay, bz - az
        ln2 = dx * dx + dy * dy + dz * dz
        ln = math.sqrt(ln2)
        cells = set()
        steps = int(ln / cell) + 1
        for s in range(steps + 1):
            t = s / steps
            cx, cy, cz = (int((ax + dx * t) // cell), int((ay + dy * t) // cell),
                          int((az + dz * t) // cell))
            for ox in (-1, 0, 1):
                for oy in (-1, 0, 1):
                    for oz in (-1, 0, 1):
                        cells.add((cx + ox, cy + oy, cz + oz))
        on = []
        for c in cells:
            for i in grid.get(c, ()):
                if i == a or i == b:
                    continue
                px, py, pz = pos[i]
                t = ((px - ax) * dx + (py - ay) * dy + (pz - az) * dz) / ln2
                if t <= 0.0 or t >= 1.0:
                    continue
                ex, ey, ez = ax + dx * t - px, ay + dy * t - py, az + dz * t - pz
                if ex * ex + ey * ey + ez * ez <= tol * tol:
                    on.append((t, i))
        on.sort()
        chain = [a] + [i for _, i in on] + [b]
        for k in range(len(chain) - 1):
            p, q = chain[k], chain[k + 1]
            if p != q:
                count[(p, q)] = count.get((p, q), 0) + 1

    total = open_n = 0
    total_len = open_len = 0.0
    for (p, q), n in count.items():
        m = count.get((q, p), 0)
        l = math.dist(pos[p], pos[q])
        total += n
        total_len += n * l
        if n > m:
            open_n += n - m
            open_len += (n - m) * l
    return {"polygons": len(polys), "edges": len(edges), "sub_edges": total,
            "open": open_n, "open_fraction": (open_n / total) if total else 0.0,
            "open_length_fraction": (open_len / total_len) if total_len else 0.0}


if __name__ == "__main__":
    import sys
    for p in sys.argv[1:]:
        w = load(p)
        print(p, "v%d" % w.version, "objects@%d render@%d" % (w.object_data_pos, w.render_data_pos))
        print("  info:", w.info_string)
        for m in w.models:
            print("  model %3d %-28s flags=0x%02x pts=%5d polys=%5d class=%s"
                  % (m.index, m.name, m.info_flags, len(m.points), len(m.polys), m.object_class))
        print("  objects:", w.class_counts())
        for s in w.warnings:
            print("  WARNING:", s)
