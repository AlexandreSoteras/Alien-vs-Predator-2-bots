# AvP2 world file (.DAT, version 70): what `dat70.py` parses

All values little-endian. `vec3` = 3 x float32. `lstr` = u16 length + bytes, no terminator
(`ILTStream::ReadString`). `cstr` = zero-terminated.

Checked on `DM_Compound.DAT` and `DM_Verloc.DAT`: every world-model record ends exactly at its
stored `nextPos`, the last one ends at `objectDataPos`, and the object list ends exactly at
`renderDataPos`.

## References

| Tag | Source |
|---|---|
| **ENG** | `lemiur/AVP2-Reconstructed` (decompiled `lithtech.exe`): `src/world/de_mainworld.cpp`, `src/world/world_tree.cpp`, `src/world/de_objects.cpp`, `src/server/server_extradata.cpp`, `src/shared/genericprop_setup.cpp`, `include/de_world.h` |
| **TCM** | `thecanonmaster/LTWorldConverter` (Pascal, v70 only): `ltworldreader.pas`, `ltworlddata.pas`, `ltworldobject.pas` |
| **SDK** | `nolffan/avp2src`: `src/LT2/sdk/inc/ltbasedefs.h`, `src/AVP2/ObjectDLL/{GameStartPoint,VolumeBrush,VolumeBrushTypes}.cpp` |
| **DR** | `burmaraider/DAT-Reader` `Assets/Scripts/DATv70/WorldBsp.cs` (only skimmed; agrees with TCM). `pokewiz/DAT-Reader` does not exist (404). |

LithTools source was not found under haekb's GitHub account; TCM is the code it credits.

## 1. File header: ENG `w_ReadWorldHeader`, `MainWorld::Load`; TCM `ReadHeader`, `ReadPropertiesAndExtents`

| Type | Field |
|---|---|
| u32 | version = 70 |
| u32 | objectDataPos (absolute offset of section 4) |
| u32 | renderDataPos (absolute offset of light-anim/lightmap data, not parsed) |
| u32 x 8 | unused (zero) |
| u32 + bytes | world info string (length, then chars; e.g. `AmbientLight 20 30 45;`) |
| f32 | LMGridSize |
| vec3, vec3 | extents min, max |

## 2. World tree layout: ENG `WorldTree::LoadLayout`, `WorldTreeNode::LoadLayout`

`vec3 boxMin, vec3 boxMax, u32 numNodes, u32 terrainDepth`, then a bit stream (LSB first, a new
byte is fetched when 8 bits are used): one bit per node, depth-first; 1 = node has 4 children
that follow. Only skipped over.

## 3. World models: ENG `MainWorld::Load` loop + `w_LoadWorldBsp`; TCM `TLTWorldBsp.Load`

`u32 nWorldModels`, then per model `u32 nextPos` (absolute offset of the next record) and a WorldBsp:

| Type | Field |
|---|---|
| u32 x 8 | unused |
| u32 | worldInfoFlags (`WIF_*`, below) |
| u32 | maxTreeDepth |
| lstr | world name (`PhysicsBSP`, `VisBSP`, or the owning object's `Name`) |
| u32 x 12 | nPoints, nPlanes, nSurfaces, nPortals, nPolies, nLeafs, nVerts, visListSize, nLeafLists, nNodes, nLeafPolies, nPolyAnimRefs |
| vec3 x 3 | minBox, maxBox, worldTranslation (centre) |
| u32, u32 | textureNamesLen, nTextures |
| cstr x nTextures | texture names |
| (u8, u8) x nPolies | nBaseVerts, nExtraVerts per polygon |
| leaves x nLeafs | `u16 n`; if n == 0xFFFF: `u16 leafIndex` (shares lists); else n x (`u16 portalId, u16 size, size bytes`). Then `u32 nLeafPolies`, nLeafPolies x 4 bytes, `u32` unknown |
| planes x nPlanes | vec3 normal, f32 dist (`normal . p = dist`) |
| surfaces x nSurfaces | vec3 O, P, Q; u16 textureIndex; u32 flags (`SURF_*`); u8 unknown; u8 x 3 colour; u8 hasEffect; if hasEffect == 1: lstr effect, lstr param; u16 textureFlags |
| points x nPoints | vec3 |
| polies x nPolies | vec3 centre; u16 lmWidth, u16 lmHeight; u16 nAnimRefs; nAnimRefs x (u16, u16); u32 surfaceIndex; u32 planeIndex; vec3 O, P, Q; (nBase + nExtra) x (u16 pointIndex, u8 x 3 colour) |
| nodes x nNodes | u32 polyIndex; u16 leaf; i32 x 2 child (-1 = NODE_IN, -2 = NODE_OUT) |
| portals x nPortals | lstr name; u32, u32, u16 unknown; vec3 centre; vec3 dims |
| PBlockTable | u32 x 3 size; vec3 blockSize; vec3 origin; per block (size product): u16 nNodes, u16 root, nNodes x 6 bytes. ENG `PBlockTable::Load` |
| i32 | root node index |
| u32 | nTerrainSections, followed by the sections (**not parsed**, skipped via nextPos) |

Polygon vertices: the first `nBaseVerts` are the real convex polygon (used here, fan-triangulated).
The `nExtraVerts` that follow are a second ring with T-junction vertices that the renderer draws
instead (ENG: "Render the second ring if there is one").

Flags (ENG `de_world.h`): `WIF_MOVEABLE` 0x02, `WIF_MAINWORLD` 0x04, `WIF_TERRAIN` 0x08,
`WIF_PHYSICSBSP` 0x10, `WIF_VISBSP` 0x20. Observed: `PhysicsBSP` = 0x14, `VisBSP` = 0x20,
`Terrain` object = 0x08, every other brush object = 0x02.

Surface flags (SDK `ltbasedefs.h`): SOLID 0x1, NONEXISTANT 0x2, INVISIBLE 0x4, TRANSPARENT 0x8,
SKY 0x10, ..., PORTAL 0x2000, PANNINGSKY 0x8000, PHYSICSFIX 0x20000, NOTASTEP 0x400000,
NOWALLWALK 0x800000 (full table in `dat70.py`). ENG keeps only the low 29 bits (`Surface::SetFlags`).

### Which geometry collides

- `PhysicsBSP` is separate from `VisBSP` and is the one extracted. (In DM_Compound the VisBSP is
  just a 6-polygon box.)
- `WIF_TERRAIN` models (class `Terrain`) are static ground and are merged into the static
  collision mesh.
- Every other model belongs to the object whose `Name` equals the model name (ENG
  `w_FindWorldModel`). Points are stored in world space at the initial position. At runtime the
  object is placed at `worldTranslation`, not at its `Pos` property (ENG `se_InitWorldModel`,
  `obj_SetupWorldModelTransform`).
- Liquid / ladder / hazard are not surface flags: they come from the owning object's class
  (SDK `VolumeBrushTypes.cpp`: `Water`, `CorrosiveFluid`, `FreezingWater`, `Ladder`, `VolumeBrush`,
  `EndlessFall`, ...). Brush solidity is the object's `Solid` bool property.

## 4. Object list (at objectDataPos): ENG `w_AddStaticLights` (same record layout); TCM `ReadObject`, `ReadProperty`

`u32 nObjects`, then per object:

| Type | Field |
|---|---|
| u16 | dataLen (bytes after this field) |
| lstr | class name |
| u32 | nProps |
| props | lstr name; u8 type; u32 flags; u16 dataLen; data |

| Type id | Name | Data |
|---|---|---|
| 0 | string | lstr |
| 1 | vector | vec3 |
| 2 | color | vec3 (0..255) |
| 3 | real | f32 |
| 4 | flags | f32 holding an integer |
| 5 | bool | u8 |
| 6 | longint | f32 holding an integer |
| 7 | rotation | 16 bytes: Euler radians pitch(X), yaw(Y), roll(Z) + one unused f32 |

Rotation is Euler angles, not a quaternion (ENG `gr_GetEulerVectors` in `w_AddStaticLights`,
`gr_EulerToRotation` in `genericprop_setup.cpp`). `Pos` and `Rotation` are already world space.

## Coordinates

Left-handed, **+Y up**, X/Z ground plane, game units. Plane normals point out of solid.
`cross(v1 - v0, v2 - v0)` is parallel to the plane normal (verified on every polygon).
Yaw 0 faces +Z; forward = (sin yaw cos pitch, -sin pitch, cos yaw cos pitch). The yaw/pitch
convention is from DirLights (pitch +pi/2 points down); the matrix multiplication order for roll
was not verified against the engine.

## Not decoded

- Terrain sections inside a `WIF_TERRAIN` model (`TerrainSection::Load`: nodes, poly index list,
  PBlockTable, a `WTNodePath` whose loader is not in the reference, centre). They only
  re-partition polygons already read.
- Render data at renderDataPos (light animations, lightmaps).
- Leaf visibility lists and PBlockTable contents are skipped, not interpreted.
- Unknown fields: surface u8 after flags, leaf trailing u32, portal u32/u32/u16.
- Solid model props (`Prop` objects with .abc models) are not in the DAT geometry at all.
