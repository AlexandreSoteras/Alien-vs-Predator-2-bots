# avp2bots: server-side bots for Aliens vs. Predator 2

Draft admin guide (work in progress, not released yet).

avp2bots adds Alien, Predator and Marine bots to an AvP2 dedicated server. It aims to make a game against bots feel like a normal multiplayer game against people:

- Bots use the multiplayer classes: character set, model and skin, speed, health and armour.
- They carry each class's default weapons, taken from the game's own class weapon sets, and fire the players' multiplayer weapons.
- They follow the server's friendly-fire rule.

**Players install nothing.** They join with an unmodified game, such as the avpunknown.com all-in-one pack with the Master Server Patch 2.4.

## How it works

| Part | What it is |
|---|---|
| `object.lto` | The server game code, built from Monolith's 2002 source release plus the avp2bots code. It replaces the game code only on your server. |
| `botnav\<map>.nav` | A waypoint graph per map, generated automatically from the map file. **No nav file means no bots on that map.** |
| `botnav\bots.cfg` | Bot settings. |

The bots are the game's own AI characters, driven by the waypoint graph instead of the hand-placed AI volumes that only single-player maps have.

## Installation (dedicated server)

1. Create a folder named `botmod` next to `AvP2Serv.exe` and put `object.lto` in it.
2. In `customrez.txt`, replace the line `Rez0="lithserver.rez"` with `Rez0="botmod"`:
   ```
   [Rez]
   Rez0="botmod"
   ```
   Keep `lithserver.rez` in the game folder: the server no longer loads it, but reads from it the Master Server Patch 2.4 constant it needs to understand MSP 2.4 players (their name, class and hardware ID). Without it, every MSP 2.4 player is called "NoName" (the bot log says `MSPKEY not found`). Putting the old line back removes the bots.

   This replaces the Master Server Patch server code (`lithserver.rez`), so the MSP server-only extras (voting, CTF/Domination, MSP admin system and anticheat) are not available on a bot server. The server still registers with the MSP master server (`master.avp2msp.com`, when it is an internet server) and answers queries with `mspatch = 2.4`, so MSP clients see it in their browser.
3. Copy the `botnav` folder (the `.nav` files and `bots.cfg`) next to `AvP2Serv.exe`.
4. Start the server as usual (`AvP2Serv.exe -config <name>`).

## `botnav\bots.cfg`

One `key = value` per line; `#` starts a comment.

| Key | Default | Meaning |
|---|---|---|
| `marines`, `aliens`, `predators` | 2, 2, 2 | Number of bots of each species (when `fill` is 0). |
| `fill` | 0 | Keep the server at this many players: bots make up the difference with the connected players, and one leaves when a player joins. Each bot takes the species with the fewest players and bots, which balances the teams in Team Deathmatch. Overrides the three counts. Maximum 16. |
| `respawn` | 4 | Seconds before a dead bot comes back. |
| `classes` | 1 | Give bots the multiplayer classes in turn (Smartgunner, Demolitions, Sniper, Gunner / Drone, Runner, Predalien / Maurauder, Striker, Stalker, Bezerker). `0` = one basic character per species. |
| `mpweapons` | 1 | Bots fire the players' multiplayer weapons with their damage. `0` = the weaker single-player AI weapons. |
| `health` | 1.0 | Multiplier on the class health and armour (1 = same as players). |
| `damage` | 1.0 | Multiplier on the damage of the bots' weapons (1 = same as players). Without avp2bots the game's AI would do 0.75 (its medium difficulty factor). |
| `accuracy` | 0.5 | The bots' aim, the main difficulty setting. 1 = perfect aim and the narrowest weapon spread (a player standing still). Lower values widen the spread (0 = a player running) and add a human aim error: a few degrees at range plus a lag on moving targets (at 0.5: about 2 degrees plus 10 % of the target's speed). The Smart Gun has no aim error, only its spread (it aims itself for players too), except on a cloaked Predator; a cloaked Predator doubles the error. Bots also notice enemies like a player: in 0.2 s up close, about 1 s at long range. |
| `cloak` | 1 | Predator bots spawn cloaked. |
| `seecloak` | 1 | Bots see a cloaked Predator as a player does: up close (400 units) at half the normal speed, up to 1200 units when it moves, has just fired or has just hit them; a still, silent Predator further away stays unseen. Only Predator bots see cloaked Predators clearly (the players' Predator vision does too). `0` = the single-player AI's eyes (a Marine bot was blind past 250 units, an Alien bot saw through the cloak). |
| `predheal` | 1 | Predator bots handle energy, health and the cloak as players do: the medicomp when below half health with no enemy around (30 energy, full health, 6 s without shooting and walking only, the cloak drops), the energy sift drops the cloak and is walk-only, and they cloak again 1.5 s after their last shot once the server's cloak delay is over. |
| `predammo` | 1 | Predator bots have the players' ammunition: 9 spears per life, and one disc that comes back by itself or is recalled (5 or 10 energy) before it can be thrown again. A Stalker out of spears uses its disc. |
| `predlock` | 1 | The disc and shoulder cannon home only on what the players' auto-target would lock (the nearest visible character of the species the Predator's vision mode targets, in front of it, within 2236 units); a locked cannon shot goes for the upper torso, as a player's does. Like a player, a Predator bot locks only the species of its vision mode (heat: humans, electro: aliens, nav: Predators, normal: nothing), starts each life in normal vision and changes mode one key press at a time (0.25 s per press, then the game's 0.2 s transition). |
| `combatmove` | 1 | Marine and Predator bots strafe while they fire, in short checked side steps (they stood still for every burst). Predators also back away from a target that is too close. |
| `zigzag` | 1 | Alien bots zig-zag toward a target that is facing them. |
| `feed` | 1 | A hurt alien bot with no enemy in sight goes to a fresh body and heals on it as players do: a head bite (Drone, Runner) then claws (claw healing per hit: 5 on a Predator body, 1 on a Marine). |
| `headbite` | 1 | Drone and Runner bots head-bite a live target (an instant kill) when the players' auto-target would allow it (within 167 units, aimed at the head) and the victim is not facing them or is stunned or netted. |
| `tail` | 1 | Alien bots charge their tail (1.6 s) and strike with it up to 250 units, as players do. |
| `pouncestun` | 1 | An alien bot's pounce stuns its victim as a player's does (damage / the server's `PounceStunMod` seconds, by the game's own stun rule: Marines and Predators yes, aliens and exosuits no). |
| `pounce` | 1 | Alien bots pounce by the players' rule: at most every 2 s (the client's pounce delay), from the ground or a wall, when the target is beyond claw reach and within the class's level reach (Drone 720, Runner 605, Predalien 980 units), fully visible, with a clear line, after the bots' sight reaction. `0` = the single-player AI's 40 % die roll every 1-3 s. |
| `pounceflight` | 1 | A bot's pounce flies like a player's (launched along the aim with the client's lift, class pounce speed, half gravity, until it touches something) instead of the AI's arc that stopped at the predicted point; when a miss would land off the waypoints (a pit, a hazard) the bot keeps the AI's arc for that pounce. |
| `tracker` | 1 | Marine bots use the motion tracker as players do (front half circle, 1920 units, a sweep every 0.9 s, movers only): a new contact coming closer makes the bot turn toward it; it still has to see the enemy to shoot. `2` also re-faces at each sweep; `0` off. |
| `wallapproach` | 1 | Alien bots going for a target that faces them, 750 to 2500 units away, prefer routes over walls and ledges to the open floor in front of it. |
| `wallambush` | 1 | Alien bots with nothing to fight sometimes wait 10-20 s on a wall above a busy route (found from the routes between start points and pickups and where people actually go), then drop or pounce. |
| `ammopickups` | 1 | Bots open the ammo boxes of their species like players and count the ammunition: Predator bots restock spears (9 at spawn, up to 27; boxes and spears stuck in walls), Marine bots collect the alternate ammunition players only get from boxes (pulse-rifle grenades, shotgun slugs, SADAR tracking rockets). |
| `altfire` | 1 | Marine bots use their alternate ammunition when they have it: pulse-rifle grenades at 300-1200 units, shotgun slugs from 300 units, SADAR tracking rockets after the client's 2 s lock on an enemy. |
| `hearsteps` | 1 | Bots hear footsteps, jumps and landings as players do (in multiplayer the game never told the AI; a player hears others' steps to 1000 units, through walls). A bot glances toward a clearly audible sound (0.3-0.6 s) and goes on, following it only if it keeps coming closer; no single-player 3-6 s search. Bots no longer speak single-player voice lines. |
| `hearlevel` | 0.3 | How loud a sound must be to make a bot react (1 - distance / radius): 0.3 = steps within 700 units, jumps and landings within 420. |
| `huglike` | 1 | Facehugger bots hunt like facehugger players: the players' leap (500 units, 2 s apart, flying on until it touches something), victims chosen as a person would (stunned or netted first, then not looking at the hugger and alone), no head-on charge at a group. |
| `hugbody` | 1 | Keep a body awake while a facehugger bot is on it, so the chestburster comes out on time (the engine stops updating objects no player is near; with a human on the server the body froze and the bot fell back to a 30 s timer). |
| `cbhide` | 1 | Chestburster bots run and hide until they grow (out of enemies' sight, far from them and from start points, low traffic), as a 50-hit-point chestburster player does, and bite back only when cornered. |
| `smallsight` | 1 | Bots notice small targets more slowly, as a person does: chestbursters, facehuggers and crouching Marines or Predators (sight rate x the square root of their height / a Runner's, at least x0.5). Standing adults are not affected. |
| `clawspread` | 1 | Bot melee strikes (claws, combistick) keep their barrel's spread, as players' do; the single-player AI made all vectors converge on one point. |
| `playerburn`, `playerlimbs`, `playerrecoil`, `playerhits`, `alwaysrun` | 1 | Remove single-player AI rules players do not have: napalm killed an AI outright (players burn over time); Drone bots lost legs and crawled; the AI waited for its recoil animation to fire; the AI was immune to its own splash, its teammates and alien acid pools; it walked while investigating. Each `0` brings the AI rule back. |
| `ladders` | 1 | Marines and Predators may climb ladders. Aliens never do. |
| `wallwalk` | 1 | Alien bots climb walls to reach ledges and upper floors (like Alien players). |
| `deathmatch` | 1 | Bots play in Deathmatch. |
| `teamdeathmatch` | 1 | Bots play in Team Deathmatch (the teams are the species, as for players). |
| `lifecycle` | 1 | Follow the server's alien lifecycle option (Deathmatch and Team Deathmatch): with it on, alien bots spawn as facehuggers, a facehugger that gets a human or a Predator turns into a chestburster on the body, and the chestburster grows into a Drone (from a Marine), a Runner (from a Corporate) or a Predalien (from a Predator), as for players. Any death starts again as a facehugger. |
| `hunt` | 1 | Bots play in Hunt, with the server's hunter and prey races and ratio. The three counts add up to the number of bots; each bot takes its role by the players' join rule. |
| `survivor` | 1 | Bots play in Survivor, with the server's survivor and mutant races (Marine, Alien or Predator). The three counts add up to the number of bots; they all start each round as survivors. |
| `listbots` | 0 | Count the bots as players in the server list (the master server and the in-game browser), named `[B] <class> <n>`. One slot always shows free while there is room for a person, so the server never looks full to someone who wants to join; when people alone fill it, the real count is shown. Consider saying "bots" in your server name. |
| `scoreboard` | 0 | Experimental, not yet tested with a real client: bots get a row on the players' scoreboard (`[B] <class> <n>`) with their frags and score, and "X killed Y" messages. Players + bot rows stay under 16. |
| `score` | 1 | Kills by and of bots count: a player who kills a bot gets the frag and the bot's class score; a player killed by a bot is not counted as a suicide; in Team Deathmatch bot kills count for their team. `0` = bots never change scores. |
| `log` | 1 | Write `avp2bots_bots.log` (spawns, deaths, status, stuck reports). About 70 MB a day with 12 bots. |
| `logmax` | 20 | Size in MB at which the log is renamed `avp2bots_bots.log.1` (replacing the previous one) and started again, so at most two files are kept. `0` = never. |
| `debug` | 0 | Very verbose movement and animation log. |

## Commands

The same commands work from three places:

- **In game, as server controller.** First take control with `<serv> control <password>` (the `SCPassword` of your server config), then type for example `<serv> addbot marine sniper`.
- **In the dedicated server window.** Type the command with a leading slash, for example `/addbot alien`.
- **From a script.** Write commands, one per line, into `botnav\command.txt`. The server runs them within a second, logs the answers and deletes the file.

| Command | Effect |
|---|---|
| `addbot [marine\|alien\|predator] [class]` | Add a bot. With no species, it fills the species with the fewest bots. `addbot sniper` also works. |
| `removebot [name\|species\|all]` | Remove the last bot, or one matching the name or species, or all of them (`kickbot` is an alias). |
| `bots` | List the bots and their classes (`botlist` is an alias). |
| `botcount <marines> <aliens> <predators>` | Add or remove bots to reach these numbers (turns `fill` off). |
| `botfill <players>` | Keep the server at this many players with bots (see `fill`); `botfill 0` turns it off and keeps the current bots. `addbot`, `removebot` and `botcount` also turn it off. |
| `botreload` | Re-read `bots.cfg` and apply its counts. |
| `bothelp` | One-line help. |

## Maps

The nav generator builds the waypoint graph from the map file: walking, drops, jumps, ladders (Marines and Predators) and wall climbs (Aliens, up to 800 units). It then checks that every multiplayer start point leads into the main play area. See `docs/nav_report_dm.md` for the current list.

- Bots open the doors players open with the Use key, as they walk up to them. They do not press switches or ride lifts yet.
- On a map marked `check`, bots still play, but some areas are cut off from others. Typical causes are switch-operated doors and lifts.
- Start points that are cut off for a kind of bot (for example above a pit only an Alien can climb out of) are listed in the `.nav` file; bots of that kind do not spawn there. Players are not affected.
- A bot that stays stuck, or ends up somewhere the waypoints do not cover, is removed and respawned automatically.

## Game types and teams

- **Deathmatch** is free-for-all, as for players: the game always allows friendly fire in Deathmatch, so every bot fights everyone, its own species included.
- **Team Deathmatch**: the teams are the species. Bots never target their teammates. With friendly fire off, bots and players of the same team cannot hurt each other (the game only checks this between two players; avp2bots applies the same rule to bots).
- **Survivor**: bots follow the players' rules. Everyone starts as a survivor; in the tag phase the first kill makes the first mutant; then mutants hunt survivors and every survivor killed (or killing himself) mutates; a round ends when no survivor is left or at the time limit, and everyone starts again as a survivor. Bots score like players (+10 per survivor killed as a mutant, +1 a second alive as a survivor), visible on their scoreboard rows when `scoreboard = 1`. Bots and players are invulnerable during the interlude and between rounds, as players are.
- **Hunt**: bots follow the players' rules. Roles are given by the hunter/prey ratio, a prey that kills a hunter swaps places with him (bots respawn as the other race after 2 seconds, as players do), hunters score frags, and when the balance needs someone to switch, a bot is moved before a player.
- **Overrun and Evac**: no bots. These modes change species and teams by their own rules, which bots do not follow yet. The server log says so at map start.

## Known limitations (planned work)

- By default bots have no row on the scoreboard and there is no "X killed Y" message for kills involving a bot; their kills still count (see `score`). `scoreboard = 1` adds both, but has not been tested with a real client yet.
- Bots do not pick up weapons or ammo on the map (Marine bots do take health and armour when hurt). Marine and Alien ammunition is unlimited (the pauses in the fire rhythm stand for reloading); Predator bots have 9 spears per life and one disc (`predammo`).
- Predator bots use energy like players: 54 points, 6 per plasma pistol shot, 3 to 8 per shoulder cannon shot depending on the charge, 5 to cloak then 0.1 per second, 30 for the medicomp. When it runs out, or when it is below half with no enemy around, the bot uses the energy sift: 4.5 seconds without shooting, walking only, the cloak drops, then full energy (the players' sift animation). The medicomp and the re-cloak follow `predheal`.
- When two bots get stuck at the same spot, that spot is removed from the bots' navigation until the next map (`LEARNBLOCK` in the log). Bots stuck for about 18 seconds are respawned.
- Bots and players respawn by the players' rule: at the start point of their species whose nearest player or bot is farthest away. In the original game this rule only counts players, so a player could respawn next to a bot and the reverse.
- Borrowed from the single-player AI or chosen for the bots, rather than copied from players:
  - a cloaked Predator is seen and draws the bots' fire within 400 units, or within 1200 when it moves, has just fired or has just hit the bot (a player sees its shimmer or its shots); a still Predator further away stays unseen (`seecloak`);
  - the Smart Gun bot aims like the players' self-aiming Smart Gun (only the weapon spread), except at a cloaked Predator;
  - Predator bots fight in melee with their class's weapon: the combistick for the Maurauder and Bezerker, the wristblades for the Stalker and Striker (60 per hit for both);
  - the human habits: when a Predator bot heals or cloaks again, how long a strafe step lasts, when an alien bot feeds or tries a head bite (the single-player alien's alternate-weapon rhythm: a 30 % chance every 1 to 4 s).
- Bots fire at the players' rate (taken from the players' weapon animations: Smart Gun 15 shots/s, pulse rifle 8, flame thrower 6.4, rail gun and SADAR about 1), in bursts with short pauses. Alien bots strike with their class's claws (Drone 40, Runner 30, Predalien 60) and a charged tail, and pounce with their class's pounce damage (Runner 195, Drone 150, Predalien 175, as players); Predator bots use the combistick in melee (60, the damage of the multiplayer wristblades) and charge the shoulder cannon between shots.
