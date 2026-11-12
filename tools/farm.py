"""Run bot matches on several isolated LAN dedicated servers in parallel and summarise them.

Each instance lives in avp2bots/farm/s<N>/: hard links to the game's executables and archives, its own
configs (LAN only, port 27900+N, one map), its own copy of our object.lto (botmod/) and the map's .nav.
Nothing in the real game folder is modified.

Usage:
  py -I farm.py --maps dm_compound,dm_verloc --duration 240 --parallel 6 [--bots "marines=4 aliens=3 predators=4"]
  py -I farm.py --maps all --duration 240          (every map with a nav/<map>.nav)
  py -I farm.py --maps dm_bunker --gametype tdm --ff 0   (Team Deathmatch, friendly fire off)
  py -I farm.py --maps bal9 --tag aim_new --reps 3 --parallel 27   (3 repetitions side by side -> farm/runs/aim_new/r1..r3)
  py -I farm.py ... --slot-base 30   (slots farm/s30.., ports 27930..: beside another farm or a soak)
Outputs: farm/s<N>/avp2bots_bots.log, farm/report.md (one row per map), farm/<map>.log and .png (trajectories).
With --tag: farm/runs/<tag>/r<k>/<map>.log, report.md and manifest.txt (build md5, options) per repetition;
analyse with tools/balance.py farm/runs/<tag> [--compare farm/runs/<other>].
"""
import argparse
import hashlib
import os
import re
import shutil
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
GAME = os.path.normpath(os.path.join(ROOT, ".."))
FARM = os.path.join(ROOT, "farm")
NAV = os.path.join(ROOT, "nav")
LINKS = ["AvP2Serv.exe", "server.dll", "ltmsg.dll", "libGeoIP-1.dll", "AVP2.REZ", "SOUNDS.REZ", "MULTI.REZ",
         "AVP2DLL.REZ", "AVP2L.REZ", "AVP2P.REZ", "AVP2P1.REZ", "AVP2P5.REZ",
         "LITHSERVER.REZ"]  # not loaded (customrez.txt loads botmod only): MspClient reads the MSP constant from it
GAME_TYPES = {"dm": ("Deathmatch", 1), "tdm": ("TeamDeathmatch", 2), "surv": ("Survivor", 4), "hunt": ("Hunt", 3)}
# the 9 Deathmatch maps of the balance measurements
MAP_SETS = {"bal9": "dm_bunker,dm_compound,dm_dropzone,dm_fury151,dm_leadworks,dm_outpost4,dm_quarantine,"
                    "dm_reservoir,dm_stronghold"}


def server_config(template, map_path, port, gametype="dm", ff=None, timelimit=120, lifecycle=0):
    """Copy the user's server config, forcing LAN, our port, one map, a long time limit, the game type and
    (when given) the friendly-fire setting."""
    text = open(template, encoding="latin-1").read().replace("\r\n", "\n")
    section = GAME_TYPES[gametype][0]
    out = []
    for sec in re.split(r"(?m)^(?=\[)", text):
        if sec.startswith("[" + section + "]"):
            body = [l for l in sec.splitlines()[1:] if l.strip() and not l.startswith("Map")
                    and not l.startswith(("TimeLimit", "FragLimit", "ScoreLimit", "Lifecycle"))]
            sec = (f"[{section}]\nMap00 = \"{map_path}\"\nTimeLimit = {timelimit}\nFragLimit = 0\nScoreLimit = 0\n"
                   f"Lifecycle = {lifecycle}\n" + "\n".join(body) + "\n\n")
        elif sec.startswith("[Settings]"):
            repl = {"LANConnection": "1", "Dedicated": "1", "PortID": str(port), "Password": '""',
                    "ServerName": f'"avp2bots farm {port}"', "GameType": str(GAME_TYPES[gametype][1])}
            lines = []
            for l in sec.splitlines():
                k = l.split("=")[0].strip()
                lines.append(f"{k} = {repl[k]}" if k in repl else l)
            sec = "\n".join(lines) + "\n\n"
        elif sec.startswith("[Preferences]") and ff is not None:
            sec = "\n".join(f"FriendlyFire = {ff}" if l.split("=")[0].strip() == "FriendlyFire" else l
                            for l in sec.splitlines()) + "\n\n"
        out.append(sec)
    return "".join(out).replace("\n", "\r\n")


def setup(n, mapname, bots_cfg, lto, gametype="dm", ff=None, timelimit=120, lifecycle=0):
    d = os.path.join(FARM, f"s{n}")
    os.makedirs(os.path.join(d, "ServerData"), exist_ok=True)
    os.makedirs(os.path.join(d, "botmod"), exist_ok=True)
    os.makedirs(os.path.join(d, "botnav"), exist_ok=True)
    for f in LINKS:
        dst = os.path.join(d, f)
        if not os.path.exists(dst):
            os.link(os.path.join(GAME, f), dst)
    for f in ("serverconfig.txt", "messages.txt"):
        shutil.copyfile(os.path.join(GAME, f), os.path.join(d, f))
    open(os.path.join(d, "customrez.txt"), "w", newline="\r\n").write('[Rez]\nRez0="botmod"\n')
    port = 27900 + n
    cfg = server_config(os.path.join(GAME, "ServerData", "unnamed.txt"), f"worlds\\multi\\dm\\{mapname}", port, gametype, ff, timelimit, lifecycle)
    open(os.path.join(d, "ServerData", "farm.txt"), "w", encoding="latin-1", newline="").write(cfg)
    shutil.copyfile(lto, os.path.join(d, "botmod", "object.lto"))
    for old in os.listdir(os.path.join(d, "botnav")):
        os.remove(os.path.join(d, "botnav", old))
    shutil.copyfile(os.path.join(NAV, mapname + ".nav"), os.path.join(d, "botnav", mapname + ".nav"))
    words = bots_cfg.split()
    if not any(w.startswith("logmax=") for w in words):
        words.append("logmax=0")  # never rotate the log during a measured run
    open(os.path.join(d, "botnav", "bots.cfg"), "w", newline="\r\n").write(
        "\n".join(kv.replace("=", " = ") for kv in words) + "\n")
    for f in ("avp2bots_bots.log", "avp2bots_crash.txt"):
        p = os.path.join(d, f)
        if os.path.exists(p):
            os.remove(p)
    return d, port


def query(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1.5)
    try:
        s.sendto(b"\\status\\", ("127.0.0.1", port))
        data, _ = s.recvfrom(65535)
        parts = data.decode("latin-1").split("\\")[1:]
        return dict(zip(parts[0::2], parts[1::2]))
    except OSError:
        return None
    finally:
        s.close()


def summarise(log):
    text = open(log, encoding="latin-1").read() if os.path.exists(log) else ""
    spawns = len(re.findall(r" SPAWN ", text))
    deaths = re.findall(r" DEATH (\S+) killer=(\S+)", text)
    bot_kills = sum(1 for v, k in deaths if k.startswith("avp2bot_") and k != v)

    def species(name):
        return re.sub(r"\d.*$", "", name[len("avp2bot_"):])
    team_kills = sum(1 for v, k in deaths if k.startswith("avp2bot_") and k != v and species(k) == species(v))
    t = re.findall(r"^\s*([\d.]+) ", text, re.M)
    return dict(span=round(float(t[-1])) if t else 0, spawns=spawns, deaths=len(deaths), bot_kills=bot_kills,
                team_kills=team_kills,
                stuck=len(re.findall(r" STUCK ", text)),
                # PATHFAIL lines are rate-limited outside debug: "(+N not logged)"
                pathfail=sum(1 + int(n or 0) for n in re.findall(r" PATHFAIL [^\n]*?(?:\(\+(\d+) not logged\))?$", text, re.M)),
                broken=len(re.findall(r" BROKEN ", text)) + len(re.findall(r" FROZEN ", text)),
                spawnfail=len(re.findall(r" SPAWNFAIL ", text)),
                ladder_paths=len(re.findall(r" PATH .* ladders=[1-9]", text)),
                climb_paths=len(re.findall(r" PATH .* climbs=[1-9]", text)),
                navloaded="nav=loaded" in text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--maps", required=True)
    ap.add_argument("--duration", type=int, default=240)
    ap.add_argument("--parallel", type=int, default=6, help="servers at once (repetitions run side by side)")
    ap.add_argument("--slot-base", type=int, default=0,
                    help="first farm/s<N> slot and port 27900+N (to run beside another farm or a soak)")
    ap.add_argument("--bots", default="marines=4 aliens=3 predators=4 respawn=4 log=1 debug=0 classes=1 ladders=1 cloak=1")
    ap.add_argument("--lto", default=os.path.join(ROOT, "server", "out", "object.lto"))
    ap.add_argument("--gametype", choices=sorted(GAME_TYPES), default="dm")
    ap.add_argument("--timelimit", type=int, default=120, help="minutes (per round in Survivor)")
    ap.add_argument("--lifecycle", type=int, choices=(0, 1), default=0, help="the alien lifecycle option")
    ap.add_argument("--ff", type=int, choices=(0, 1), default=None, help="friendly fire (default: the user's config)")
    ap.add_argument("--tag", default=None, help="keep this run in farm/runs/<tag>/r<k>/")
    ap.add_argument("--reps", type=int, default=1, help="repetitions of the whole map list")
    a = ap.parse_args()
    a.maps = MAP_SETS.get(a.maps, a.maps)
    if a.maps == "all":
        maps = sorted(f[:-4] for f in os.listdir(NAV) if f.endswith(".nav") and not f.startswith("test"))
    else:
        maps = [m.strip().lower() for m in a.maps.split(",") if m.strip()]
    lto_md5 = hashlib.md5(open(a.lto, "rb").read()).hexdigest()
    os.makedirs(FARM, exist_ok=True)

    out_dirs = {}
    for rep in range(1, a.reps + 1):
        out_dir = os.path.join(FARM, "runs", a.tag, f"r{rep}") if a.tag else FARM
        os.makedirs(out_dir, exist_ok=True)
        out_dirs[rep] = out_dir
        if a.tag:
            with open(os.path.join(out_dir, "manifest.txt"), "w", encoding="utf-8") as f:
                f.write(f"tag={a.tag}\nrep={rep}/{a.reps}\ndate={time.strftime('%Y-%m-%d %H:%M:%S')}\n"
                        f"lto={a.lto}\nlto_md5={lto_md5}\nmaps={','.join(maps)}\nduration={a.duration}\n"
                        f"gametype={a.gametype}\nff={a.ff}\nbots={a.bots}\n")
    if a.tag:
        print(f"== {a.tag}: {a.reps} repetition(s) x {len(maps)} maps, {a.parallel} servers at once, "
              f"slots {a.slot_base}+ (object.lto md5 {lto_md5})", flush=True)

    # every (repetition, map) is a job; batches of --parallel servers run side by side
    jobs = [(rep, m) for rep in range(1, a.reps + 1) for m in maps]
    results = {rep: [] for rep in out_dirs}
    while jobs:
        batch, jobs = jobs[:a.parallel], jobs[a.parallel:]
        procs = []
        for k, (rep, m) in enumerate(batch):
            d, port = setup(a.slot_base + k, m, a.bots, a.lto, a.gametype, a.ff, a.timelimit, a.lifecycle)
            p = subprocess.Popen([os.path.join(d, "AvP2Serv.exe"), "-config", "farm"], cwd=d)
            procs.append((rep, m, d, port, p))
            print(f"[{time.strftime('%H:%M:%S')}] start r{rep} {m} on port {port} (pid {p.pid})", flush=True)
        time.sleep(12)
        status = {(rep, m): query(port) for rep, m, d, port, p in procs}
        time.sleep(max(0, a.duration - 12))
        for rep, m, d, port, p in procs:
            out_dir = out_dirs[rep]
            alive = p.poll() is None
            st = status.get((rep, m)) or {}
            if alive:
                subprocess.run(["taskkill", "/F", "/PID", str(p.pid)], capture_output=True)
            p.wait(timeout=30)
            crash = os.path.join(d, "avp2bots_crash.txt")
            crashes = open(crash, encoding="latin-1").read().count("=== exception") if os.path.exists(crash) else 0
            r = summarise(os.path.join(d, "avp2bots_bots.log"))
            r.update(map=m, alive=alive, crashes=crashes, mapname=st.get("mapname", "-"), mspatch=st.get("mspatch", "-"))
            keep = os.path.join(out_dir, f"{m}.log")
            if os.path.exists(os.path.join(d, "avp2bots_bots.log")):
                shutil.copyfile(os.path.join(d, "avp2bots_bots.log"), keep)
                subprocess.run([sys.executable, "-I", os.path.join(ROOT, "navgen", "analyze_log.py"), keep,
                                os.path.join(NAV, m + ".nav"), os.path.join(out_dir, f"{m}.png")], capture_output=True)
            if crashes:
                shutil.copyfile(crash, os.path.join(out_dir, f"{m}_crash.txt"))
            results[rep].append(r)
            print(f"  r{rep} {m}: alive={alive} crashes={crashes} spawns={r['spawns']} deaths={r['deaths']} "
                  f"botkills={r['bot_kills']} teamkills={r['team_kills']} stuck={r['stuck']} pathfail={r['pathfail']} broken={r['broken']} "
                  f"ladderpaths={r['ladder_paths']} climbpaths={r['climb_paths']}", flush=True)

    for rep, out_dir in out_dirs.items():
        with open(os.path.join(out_dir, "report.md"), "w", encoding="utf-8") as f:
            f.write(f"# Bot farm report ({a.duration} s per map, {GAME_TYPES[a.gametype][0]}, friendly fire "
                    f"{'from config' if a.ff is None else a.ff})\n\nBots: `{a.bots}`\n\n")
            f.write("| map | server alive | crashes | nav | spawns | deaths | bot kills | same-species kills | stuck | "
                    "path fails | broken | ladder paths | climb paths |\n|---|---|---|---|---|---|---|---|---|---|---|---|---|\n")
            for r in results[rep]:
                f.write(f"| {r['map']} | {r['alive']} | {r['crashes']} | {r['navloaded']} | {r['spawns']} | {r['deaths']} | "
                        f"{r['bot_kills']} | {r['team_kills']} | {r['stuck']} | {r['pathfail']} | {r['broken']} | "
                        f"{r['ladder_paths']} | {r['climb_paths']} |\n")
        print("wrote", os.path.join(out_dir, "report.md"))
        if out_dir != FARM:
            shutil.copyfile(os.path.join(out_dir, "report.md"), os.path.join(FARM, "report.md"))


if __name__ == "__main__":
    main()
