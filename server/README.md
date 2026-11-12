# Building the bot server code (object.lto)

`object.lto` is the server game DLL of Aliens vs. Predator 2. avp2bots builds it from Monolith's official source release plus our changes. Monolith's code is not redistributable (see the release's EULA), so this folder only contains our build files, our fixes as patches, and the script that inserts the bot hooks.

| Path | What |
|---|---|
| `CMakeLists.txt`, `build.bat`, `compat/`, `gen_object_sources.py`, `object_sources.txt` | Build files (ours). Win32 Release DLL with Visual Studio 2022. |
| `patches/0001..0004` | Our fixes to Monolith's code, as `git am` patches: VS2022 compile fixes (ObjectDLL, Shared, SDK headers, `fsm.h`), lithshared VS2022 fixes, and an out-of-bounds read in `Destructible.cpp` that crashed the server at random at level load. |
| `apply_bot_hooks.py` | Inserts the avp2bots hooks (small, marked `avp2bots:<tag>`) into the sources. Idempotent. |
| `game/` | Not published: your local copy of the Monolith sources (a local git repository). |
| `../bots/` | Our bot code, compiled into the same DLL. |

## Steps

1. Get **"AvP2 Source Code v1.0.9.6"**, Monolith's official 2002 release (archived on archive.org as `AvP2_src`). Put its `AVP2` and `LT2` folders in `game/`: you need `AVP2/ObjectDLL`, `AVP2/Shared`, `AVP2/ServerRes`, the `AVP2/ClientShellDLL` headers, `LT2/sdk` and `LT2/lithshared` (headers, WONAPI sources).
2. The release ships the lithshared libraries only as VC6 `.lib` files, which VS2022 cannot link. Get their sources from <https://github.com/nolffan/avp2src> (`src/lithshared`, commit `2a08ad16ff725f7761fb7c8156b7e49cfc1c3c21`) and put them in `game/LT2/lithshared_src/`.
3. Make `game/` a git repository, commit everything as is, then apply our fixes:
   ```
   cd game
   git init && git add -A && git commit -m "Pristine import"
   git am ../patches/*.patch
   ```
4. Insert the bot hooks and build, from `server/`:
   ```
   py -I apply_bot_hooks.py
   build.bat
   ```
   The result is `out/object.lto` (plus `object.pdb` and `object.map` for crash analysis).

Requirements: Visual Studio 2022 with the C++ desktop tools (its bundled CMake is used) and Python 3.

## Hooks

`apply_bot_hooks.py` documents every hook next to its code. The list, with the reason for each, is in [`../docs/HOOKS.md`](../docs/HOOKS.md).
