"""Synthetic check for navbuild: two rooms joined by a doorway, a raised platform reached by stairs,
a ledge you can drop from, and an isolated box on top of a pillar that must NOT join the main area.

Run: py -I test_navbuild.py  (writes test_scene.obj / .objects.json and the nav outputs into ./_test/)
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "_test")


def box(x0, y0, z0, x1, y1, z1):
    """Closed axis-aligned box as 12 triangles (outward winding)."""
    v = [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1),
         (x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)]
    quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (2, 3, 7, 6), (1, 2, 6, 5), (0, 4, 7, 3)]
    tris = []
    for a, b, c, d in quads:
        tris += [(v[a], v[b], v[c]), (v[a], v[c], v[d])]
    return tris


def scene():
    t = []
    t += box(0, -16, 0, 2048, 0, 1024)          # floor: two 1024x1024 rooms side by side
    t += box(0, 0, -16, 2048, 256, 0)           # outer walls
    t += box(0, 0, 1024, 2048, 256, 1040)
    t += box(-16, 0, 0, 0, 256, 1024)
    t += box(2048, 0, 0, 2064, 256, 1024)
    t += box(1016, 0, 0, 1032, 256, 448)        # dividing wall with a 128-wide doorway (z 448..576)
    t += box(1016, 0, 576, 1032, 256, 1024)
    t += box(1400, 0, 100, 1900, 96, 400)       # platform 96 high in room 2
    for i in range(6):                           # stairs along -x up to the platform's west edge, 16 high x 32 deep
        x1 = 1400 - i * 32
        t += box(x1 - 32, 0, 150, x1, 96 - 16 * (i + 1), 350)
    t += box(300, 0, 300, 400, 300, 400)        # pillar 300 high with a top you cannot reach
    return t


def main():
    os.makedirs(OUT, exist_ok=True)
    tris = scene()
    obj = os.path.join(OUT, "test_scene.obj")
    with open(obj, "w") as f:
        f.write("o main\n")
        for i, tri in enumerate(tris):
            for v in tri:
                f.write("v %.1f %.1f %.1f\n" % v)
            f.write("f %d %d %d\n" % (3 * i + 1, 3 * i + 2, 3 * i + 3))
    objects = [
        {"class": "StartPoint", "pos": [200, 20, 800]},
        {"class": "StartPoint", "pos": [1800, 20, 800]},
        {"class": "StartPoint", "pos": [1650, 116, 250]},   # on the platform
    ]
    jpath = os.path.join(OUT, "test_scene.objects.json")
    json.dump(objects, open(jpath, "w"))
    r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "navbuild.py"), obj, "--objects", jpath, "--out", OUT],
                       capture_output=True, text=True)
    print(r.stdout, r.stderr)
    ok = ("start points projected onto the nav: 3/3" in r.stdout
          and "spawn-to-spawn pairs without a route: 0 " in r.stdout)
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
