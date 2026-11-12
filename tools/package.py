"""Assemble a server-ready avp2bots release: release/avp2bots-<date>/ and a .zip of it.

Layout (copy the contents next to AvP2Serv.exe):
  botmod/object.lto        our server game code
  botnav/<map>.nav         waypoint graphs (every nav/*.nav, test files excluded)
  botnav/bots.cfg          default settings, commented
  customrez.txt.example    loads botmod/ last
  ADMIN.md, README.md      documentation
  LICENSE                  MIT (our files)

Usage: py -I tools/package.py [--lto server/out/object.lto] [--name avp2bots-test]
"""
import argparse
import datetime
import os
import shutil
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))

DEFAULT_CFG = """# avp2bots settings: one "key = value" per line, '#' starts a comment.
# The server reads this file at every map start; '/botreload' re-reads it at once.
# Every key is described in ADMIN.md.

# Bots per species (used when fill = 0)
marines = 2
aliens = 2
predators = 2

# Keep the server at this many players, bots making up the difference and balancing the teams (0 = off)
fill = 0

# Seconds before a dead bot comes back
respawn = 4

# Multiplayer classes and weapons, like players (0 = single-player characters and AI weapons)
classes = 1
mpweapons = 1

# Multiplier on the class health and armour (1 = same as players)
health = 1.0

# Difficulty: aim from 0 (wide spread and a large human aim error) to 1 (perfect aim),
# and damage multiplier (1 = same as players)
accuracy = 0.5
damage = 1.0

# Movement: ladders (Marines, Predators), wall climbing (Aliens)
ladders = 1
wallwalk = 1

# Predator bots spawn cloaked
cloak = 1

# Play like players (each 1 = the players' rule, 0 = the single-player AI's; see ADMIN.md):
# bots see cloaked Predators as players do
seecloak = 1
# Predator bots: medicomp, sift and re-cloak like players; 9 spears and one disc; disc and cannon lock like players
predheal = 1
predammo = 1
predlock = 1
# Marine and Predator bots strafe while firing; alien bots zig-zag toward a target facing them
combatmove = 1
zigzag = 1
# Alien bots: heal on bodies, head bite, charged tail
feed = 1
headbite = 1
tail = 1
pouncestun = 1
clawspread = 1
pounce = 1
pounceflight = 1
wallapproach = 1
wallambush = 1
# Marine bots use the motion tracker (front half circle, as players)
tracker = 1
# Bots take ammo boxes like players; Marine bots use the alternate ammo they picked up
ammopickups = 1
altfire = 1
# Bots hear footsteps like players and glance toward them
hearsteps = 1
# Alien lifecycle (server option Lifecycle): facehuggers hunt like players, chestbursters hide until they grow
huglike = 1
hugbody = 1
cbhide = 1
# Small targets (facehuggers, chestbursters, crouching players) are noticed more slowly
smallsight = 1
# Bots take burns, splash, friendly and acid damage like players, keep their limbs, always run
playerburn = 1
playerlimbs = 1
playerrecoil = 1
playerhits = 1
alwaysrun = 1

# Follow the server's alien lifecycle option (facehugger -> chestburster -> adult)
lifecycle = 1

# Game types with bots (Overrun and Evac never have bots)
deathmatch = 1
teamdeathmatch = 1
survivor = 1
hunt = 1

# Kills by and of bots count on the scores
score = 1

# Count bots as players in the server list (one slot always shows free); say "bots" in your server name
listbots = 0

# Experimental (not yet tested with a real client): bots on the scoreboard, with kill messages
scoreboard = 0

# Logs (avp2bots_bots.log next to AvP2Serv.exe, about 70 MB a day with 12 bots); debug = very verbose.
# logmax: at this size in MB the log becomes avp2bots_bots.log.1 and starts again (0 = never)
log = 1
logmax = 20
debug = 0
"""

CUSTOMREZ = """[Rez]
Rez0="botmod"
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lto", default=os.path.join(ROOT, "server", "out", "object.lto"))
    ap.add_argument("--name", default="avp2bots-" + datetime.date.today().strftime("%Y%m%d"))
    a = ap.parse_args()

    out = os.path.join(ROOT, "release", a.name)
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, "botmod"))
    os.makedirs(os.path.join(out, "botnav"))
    shutil.copyfile(a.lto, os.path.join(out, "botmod", "object.lto"))
    navs = sorted(f for f in os.listdir(os.path.join(ROOT, "nav")) if f.endswith(".nav") and not f.startswith("test"))
    for f in navs:
        shutil.copyfile(os.path.join(ROOT, "nav", f), os.path.join(out, "botnav", f))
    with open(os.path.join(out, "botnav", "bots.cfg"), "w", newline="\r\n") as f:
        f.write(DEFAULT_CFG)
    with open(os.path.join(out, "customrez.txt.example"), "w", newline="\r\n") as f:
        f.write(CUSTOMREZ)
    shutil.copyfile(os.path.join(ROOT, "docs", "ADMIN.md"), os.path.join(out, "ADMIN.md"))
    shutil.copyfile(os.path.join(ROOT, "README.md"), os.path.join(out, "README.md"))
    shutil.copyfile(os.path.join(ROOT, "LICENSE"), os.path.join(out, "LICENSE"))

    zpath = out + ".zip"
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for base, _, files in os.walk(out):
            for f in files:
                full = os.path.join(base, f)
                z.write(full, os.path.relpath(full, os.path.dirname(out)))
    size = os.path.getsize(zpath) / 1e6
    print(f"{out}: object.lto, {len(navs)} nav files, bots.cfg; {zpath} ({size:.1f} MB)")


if __name__ == "__main__":
    main()
