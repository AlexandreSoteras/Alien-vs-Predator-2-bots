# Source hooks

`server/apply_bot_hooks.py` inserts these hooks into Monolith's 2002 server source (paths relative to `AVP2/`, or to the source root for `LT2/`). Each is small and marked `avp2bots:<tag>` in the code; the script documents each one next to its code.

| Tag | File | Why |
|---|---|---|
| include, prestart, poststart, update | GameServerShell.cpp | Call BotMgr. |
| findpath_pos, findpath_char | AIPathMgr.cpp | With no AIVolumes, path through our nav graph. |
| creategoal | AI.cpp | Register the `BotRoam` goal. |
| mpsets | AI.h | `GetCharacterSetName` keeps `*_MP` sets, so the AI can use MP classes and models. Otherwise it appends `_AI`. |
| ailayers | Character.cpp | In MP only players use the MPAnimLayers; AI keep their AI layers. Without this, Marine and Predator AI had no tracker set and never moved. |
| addweapon | AIButeMgr.h | Register AI weapons at runtime (for the bots' MP weapons). Attribute files are never edited, because the server CRC-checks the clients' files. |
| ladderalign | AIActionMoveTo.cpp | In MP, `AIActionLadderTo` first moves over the ladder spot, then climbs (it did both at once and bumped the head under the landing above on multi-level shafts: tds_dm_pod). With the generator's width-aware stand-point check, tds_dm_pod went from 6 ladder hangs / 18 stuck to 0 / 4. |
| ladderepsilon | AIActionMoveTo.cpp | In MP, the ladder action accepts 12 units of vertical tolerance instead of 0.5. |
| include, servercommand, consolecommand, mspatchkey | MultiplayerMgr.cpp | Bot admin commands; send `mspatch = 2.4` in status replies (MSP clients filter on it). |
| levelindex | MultiplayerMgr.h | `GetCurrentLevelIndex()` accessor. |
| friend | MultiplayerMgr.h | `friend struct BotScoreAccess` (BotMgr.cpp): bot kills update the existing client rows and team totals. |
| include, teamdamage | Destructible.cpp | With friendly fire off, the game only blocks damage between two players; also block it between a bot and a teammate (`BotMgr_TeamDamageBlocked`). |
| botkill | Destructible.cpp | `ProcessPlayerDeath`: a player killed by a bot is not a suicide (`BotMgr_PlayerKilledByBot` scores it). |
| wwnormal | AIStrategyFollowPath.cpp | `StartWallWalkTo` without a node uses the waypoint's normal (bots' wall climbs) instead of the AI's up vector. |
| include, pouncevolumes, facehugvolumes, lunge | AIAlienAttack.cpp | Alien pounce needed AIVolumes ("only pounce if we are in a volume"): on maps without volumes the target counts as reachable (normal pounce delay) and the lunge is allowed when the target stands on the waypoint graph (`BotMgr_LungeTargetOK`). |
| includemgr, botfire | AI.cpp | `CAI::FireWeapon`: bots use `damage` (default 1 = players' damage) instead of the difficulty factor (0.75 at medium: bots did 75 % of player damage) and `accuracy` (default 0.5) for the spread instead of the template Accuracy (0 = always the widest spread). |
| botskillfire | AI.cpp | `CAI::FireWeapon`: a bot that a server extension gave its own skill (`ServerExt_BotSkill`, 0..1, at each spawn) uses it as its accuracy for the spread. BotMgr uses the same skill for its aim error and multiplies its sight rate by 2^(2 x (skill - 0.5)). Without an extension (the public build) nothing changes. |
| botfirelog, botaim | AI.cpp | `CAI::FireWeapon`: `BotMgr_OnFire` may refuse the shot (a Predator sifting or out of energy), charges the shoulder cannon and logs SHOT in debug; `BotMgr_AimError(shooter, target, firePos, path, m_bFullyAimed)` keeps the AI's aim point (projectile lead, grenade arc) minus its single-player lag, makes the path a unit vector (so the barrel spread applies) and adds a person's drifting aim error; not fully aimed, the shot goes where the gun points. |
| botfirekey | AI.cpp | `CAI::HandleModelString`: bots ignore the animations' fire keys for ranged weapons (melee still strikes on them). The MP models either never send the key (rail gun, pulse rifle, flame thrower: 0 hits) or send it every frame (spear gun: ~100 shots/s, 784,000 damage per match). |
| spawndist | GameServerShell.cpp | MP `FindStartPoint` picks the start point whose nearest player is farthest; `BotMgr_StartPointDist` adds the live bots, so players and bots both spawn away from everyone (bots were ignored, and a bot spawning in a bot-only game got a random point: 29 % of deaths came within 5 s of a spawn). While a bot spawns, its species' cut-off start points get a distance of -1. |
| include, botrate | AIActionMisc.cpp | `AIActionFireWeapon::Update`: a bot fires several shots per frame when its weapon is faster than the server frames (one shot per frame capped the 15 shots/s Smart Gun at ~10). |
| include, botpounce | Character.cpp | `Touch_Notify_Pouncing`: a bot's pounce does its MP class's PounceWeaponDamage (Runner 195, Drone 150, Predalien 175) instead of the AI's 30 x difficulty. |
| botcloak | AIAttack.cpp | `FireOnTarget`: the AI fires at a cloaked target only when it fully sees it; bots also fire at one within 1200 units (`BotMgr_CanShootCloaked`), as players shoot at the shimmer. |
| botmelee | AIAttack.cpp | `AttackGoal::Melee`: `BotMgr_MeleeWeapon` swaps the Predator template's AI spear (50) for `Spear_MP` (60). |
| dmglog | Destructible.cpp | `BotMgr_OnDamage(victim, damager, amount, type)` before damage is processed: DMG lines (hits by bots) and DMGIN lines (damage bots take from anything else) in debug. |
| gameinforows, gameinfowrite | MultiplayerMgr.cpp | `SendGameInfo`: the client count includes bot scoreboard rows (option `scoreboard`, at most 15 - players) and the rows are written after the clients in the same format. |
| dmlimits | MultiplayerMgr.cpp | `CheckEndConditions_DM`: with `scoreboard`, a bot row at the frag or score limit ends the round. |
| listnum, listplayers | MultiplayerMgr.cpp | Option `listbots`: `numplayers` = players + min(bots, maxplayers - 1 - players) so the server never looks full; the players reply lists them as `[B] <class> <n>` after the people. |
| sierraup, sierraupclose | LT2/lithshared_src/gamespymgr/GameSpyMgr.cpp | `InitSierraMultiplayer` read `SierraUp.cfg` without a NULL check: internet servers without the file died at the first update. |
| mspmaster | LT2/lithshared_src/gamespymgr/GameSpyMgr.cpp | Heartbeats go to `master.avp2msp.com` (MSP master) instead of `master%i.gamespy.com`. |
| include, botdoor | Door.cpp | `Door::Activate`: bots count as players (not AI), so they open exactly the doors players open with Use (`ActivateTrigger`), not the AI-only ones. BotMgr sends `ACTIVATE` to a closed plain/rotating/hinged door when a bot is within 64 units of it (not switches, breakables, hackables). The generator excludes upright, unlocked `ActivateTrigger` doors from collision so paths go through them and lists them in the .nav (`usedoor <name>`); bots press Use only on those (pressing it on any Door object moved the APC on Alley and raised its stuck count from ~8 to 23). |
| include, dodge, backoff | AIAttack.cpp | Side steps and step backs needed the destination inside an AIVolume: on maps without volumes `BotMgr_MoveDestOK` checks a clear segment and floor (at most 24 lower). Dodge probability is 0 in the AI attributes anyway ("no dodge animations"); backoff is 0.75. |
| mspinclude, mspclient, msplen | GameServerShell.cpp | `OnClientEnterWorld` first unscrambles an MSP 2.4 client's data (`Msp_DecodeClientData`, `bots/MspClient.cpp`); `CreatePlayer` accepts its 0x88 bytes (`>= sizeof(MPClientData)`). |
| mspinclude, msplen1-6, mspname | MultiplayerMgr.cpp | The six client data length checks accept 0x88 bytes; `AddClient` cleans every name (`Msp_SanitizeName`: printable, at most 24 characters, else "NoName") so no client can freeze AvP2Serv.exe. |
| include, botseecloak | AISenseSee.cpp | `CAISenseSeeEnemy::GetRateMultiplier`: `BotMgr_CloakSeeFactor` gives a bot a player's eyes on a cloaked Predator (option `seecloak`). |
| include, botshotnote | Weapon.cpp | `CWeapon::Fire`: every character's last shot (players too) for the cloak rule (a Predator that has just fired gives itself away). |
| botpredheal | Character.h | Public wrappers `BotMedicomp`, `BotEnergySiftKey`, `BotCloakDelayStart` around the protected medicomp/sift code (option `predheal`). |
| firemove | AI.cpp | `CAI::Update`, between the action update and the movement update: `BotMgr_CombatMove` strafes Marine and Predator bots while they fire (option `combatmove`). |
| include, zigzag | AIStrategyApproachTarget.cpp | `UpdateTargetVisible`: alien bots path to a point beside a target that faces them (option `zigzag`). |
| botnapalm | Destructible.cpp | No forced death at the first napalm tick for bots (option `playerburn`). |
| botlimbs | Character.cpp | `HandleAlienDamage` (limb loss, forced crawl) skipped for bots (option `playerlimbs`). |
| botrecoil | AIActionMisc.cpp | `AIActionFireWeapon::Start` does not wait for the recoil animation for bots (option `playerrecoil`). |
| botimmune | AI.cpp | `CAI::IgnoreDamageMsg`: bots take their own splash, teammates' damage (when friendly fire allows) and acid pools (option `playerhits`); their minimum fire range becomes the ammo's AreaDamageRadius. |
| botcommit, botammo, botkeepideal | AI.cpp | `FireWeapon` after the aim check: `BotMgr_FireCommit` spends energy, spears and the disc only for shots that really leave, and picks the players' auto-target for the disc and cannon; `HasAmmo` and `ChooseBestWeapon` follow the Predator bot's spears and disc (options `predammo`, `predlock`). |
| include, botdisc | PredDisk.cpp | The disc of a Predator bot comes back or is recalled like a player's instead of being deleted. |
| friend | BodyProp.h | `friend struct BotBodyAccess`: alien bots choose the bodies they feed on (option `feed`). |
| botpouncehit | Character.cpp | `Touch_Notify_Pouncing`: `BotMgr_PounceHit` does what a player's pounce does on the server (`CPlayerObj::OnMessage` CP_POUNCE_DAMAGE): class damage plus a stun of damage / `PounceStunMod` s through the game's stun test (option `pouncestun`). |
| botmeleepath | AI.cpp | Bot melee strikes fire on a unit path so the barrel spread applies (claws landed 4.9 of 6 vectors, now 3.7, like a moving player; the combistick 6 of 6, now 2.6) (option `clawspread`). |
| botpouncerule | AIAlienAttack.cpp | The AI's pounce decision for bots follows the players' rule (`BotMgr_PounceRule`, option `pounce`). |
| botpounceflight | Shared/CharacterMovement.cpp | `CharacterMovementState_Pouncing` (server build only): a bot's pounce flies like a player's pounce jump (option `pounceflight`). |
| creategoaltracker | AI.cpp | `CreateGoal`: the `BotTracker` goal (Marine bots' motion tracker reaction, option `tracker`). |
| friend, include, botammobox, botammoboxtake | AmmoBox.h / AmmoBox.cpp | Bots open the ammo boxes of their species like players; BotMgr counts what they take (option `ammopickups`). |
| botammopickup | PickupObject.cpp | Bots take spears stuck in walls, counted by BotMgr. |
| srv* (srvjoin, srvleave, srvchat, srvname*, srvcamspawn, srvworld, srvdisconnect, srvdeath) | MultiplayerMgr.cpp, GameServerShell.cpp, Destructible.cpp | Server extension points (`bots/ServerExt.h`): a client joins, leaves, is renamed or chats, a world starts, a character dies; a spectator camera client never spawns. In the public build they call no-ops (`bots/ServerExtStub.cpp`); a server can build its own add-ons behind them. `srvnamesan` also cleans names after a rename. |
| hearsteps, botquiet | Character.cpp | `HandleFootstepKey`: in multiplayer, footstep keys reach the bots' hearing (`BotMgr_HearStep`); bots do not play single-player voice lines (option `hearsteps`). |
| include, hearjump, hearland, hearfirststep | PlayerObj.cpp | A player's jump, landing and first step reach the bots' hearing. |
| include, investglance, investglanceend, investnoanim, investnosearch | AIInvestigate.cpp | A bot's investigation is a glance (turn + 0.3-0.6 s), followed only if the sound keeps coming closer; no investigate animation, no 3-5 s search. |
| bothugrule | AIAlienAttack.cpp | Facehugger bots leap by the players' rule (option `huglike`). |
| bothugflight | Shared/CharacterMovement.cpp | The facehug leap flies like a player's (with `botpounceflight`). |
| botgrow, botnochar | Character.cpp | `UpdateChestbursterCycle`: for a bot, the engine no longer `SetCharacter`s the AI (in MP that rebuilds the attachments and left CAI::m_pWeaponPosition dangling: an access violation in `HasAmmo` <- `ChooseBestWeapon`, 8 in 52 lifecycle runs); BotMgr spawns the adult in place. The `CHARACTER` trigger is ignored for bots. |
| bothugattach | BodyProp.cpp | The body a bot hugs and its host are passed to BotMgr, which keeps the body awake (option `hugbody`). |
| botseesize | AISenseSee.cpp | Bots notice small targets more slowly (`BotMgr_SizeSeeFactor`, option `smallsight`). |
| botmissionfail | Character.cpp | `CCharacter::CheckMissionFailed`: never in multiplayer. A player killing an AI character the game calls his ally (a Marine bot killed by a Marine player in Deathmatch) sent the single-player "Mission failed!" screen to every client; stock multiplayer never had AI characters. |
| (manual commit) | Destructible.cpp | Fix a `GetCanBeOnFire(-1)` out-of-bounds read (random crash at level load). |
