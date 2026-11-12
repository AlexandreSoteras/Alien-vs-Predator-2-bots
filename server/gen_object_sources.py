"""Regenerate object_sources.txt from Monolith's ObjectDLL/Object.dsp.

    py -I gen_object_sources.py game/AVP2 object_sources.txt

Lists every .cpp of the project, in project order (the order VC6 handed the
objects to the linker, which is also the order static initialisers run in),
with the on-disk spelling of each file name. Only needed if the .dsp changes;
the generated list is checked in next to CMakeLists.txt.
"""
import os
import re
import sys

root, out = sys.argv[1], sys.argv[2]
dsp = open(os.path.join(root, "ObjectDLL", "Object.dsp"), encoding="latin-1").read().replace("\r", "")

disk = {}
for d in ("ObjectDLL", "Shared"):
    for f in os.listdir(os.path.join(root, d)):
        disk[(d + "/" + f).lower()] = d + "/" + f

seen = []
for src in re.findall(r"^SOURCE=(.*)$", dsp, re.M):
    src = src.strip('"').replace("\\", "/")
    if not src.lower().endswith((".cpp", ".c")):
        continue
    if src.startswith("./"):
        key = "objectdll/" + src[2:].lower()
    elif src.lower().startswith("../shared/"):
        key = "shared/" + src[len("../shared/"):].lower()
    else:
        sys.exit("unexpected source path: " + src)
    if key not in disk:
        sys.exit("listed in the .dsp but missing on disk: " + src)
    if disk[key] not in seen:
        seen.append(disk[key])

with open(out, "w", newline="\n") as f:
    f.write("\n".join(seen) + "\n")
print(len(seen), "sources")
