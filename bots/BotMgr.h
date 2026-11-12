// BotMgr: server-side bots for AvP2 multiplayer (avp2bots).
//
// Bots are the game's own single-player AI characters (AIMarine, AIAlien, AIPredator) spawned at
// multiplayer start points. Stock multiplayer maps have no AIVolumes, so the AI's path finding is
// redirected to a waypoint graph generated offline (navgen/navbuild.py -> botnav\<map>.nav).
//
// Files read from the server's working directory:
//   botnav\<map>.nav   waypoint graph for the current map (no file = no bots on that map)
//   botnav\bots.cfg    key=value: marines, aliens, predators, respawn (seconds), log (0/1), ... (LoadConfig);
//                      alien kit: feed, headbite, tail (0/1, default 1)
// File written:
//   avp2bots_bots.log  spawns, deaths, periodic status and stuck reports (for offline analysis)
//
// Include after stdafx.h (uses engine types).

#ifndef AVP2BOTS_BOTMGR_H
#define AVP2BOTS_BOTMGR_H

class CAI;
class CAIPath;
class CCharacter;
struct DamageStruct;

namespace avp2bots { class BotNav; }

void BotMgr_PreStartWorld();
void BotMgr_PostStartWorld();
void BotMgr_Update();

// The loaded waypoint graph, or 0 when the current map has none.
avp2bots::BotNav* BotMgr_GetNav();

// Appends a timestamped line to avp2bots_bots.log (when logging is on).
void BotMgr_Log(const char* fmt, ...);
bool BotMgr_Debug();
// Read-only accessors for the server extensions (ServerExt.h). BotInfo: 1 if hObj is one of our
// bots (alive or just killed), with its scoreboard name, species (0 marine, 1 alien, 2 predator) and slot
// (1..n per species, stable while the bot exists); BotCount/BotObject: transient index, LTNULL if dead.
int BotMgr_BotInfo(HOBJECT hObj, const char** display, int* species, int* slot);
int BotMgr_BotCount();
HOBJECT BotMgr_BotObject(int i);
bool BotMgr_IsBot(HOBJECT hObj);  // one of our bots (alive)

// Pickups: a PlayerOnly pickup a bot may take as a player would (Marine bots: health and armor).
bool BotMgr_CanTakePickup(HOBJECT hObj, const char* szMessage);
// A health (or armor) pickup a hurt Marine bot should go for; false when none is needed or available.
bool BotMgr_PickupGoal(CAI* pAI, LTVector* pDest);
// Ammunition like players (bots.cfg ammopickups): may this bot open an ammo box (HumanOnly box: Marines, else
// Predators; AmmoBox::SetupOpenState); one of the box's pickup messages for a bot that opened it (BotMgr counts the
// pools it uses); a PlayerOnly ammo pickup (spears stuck in walls) a bot takes: true = counted, the pickup goes.
bool BotMgr_CanOpenAmmoBox(HOBJECT hObj, bool humanBox);
void BotMgr_AmmoBoxPickup(HOBJECT hObj, const char* szMessage);
bool BotMgr_TakeAmmoPickup(HOBJECT hObj, const char* szMessage);
// A Stalker short of spears may run to a spear box in a fight when it is safe (BotRoamGoal bids over the attack);
// cancelled when the roam goal finds no way to a box.
bool BotMgr_AmmoRunWanted(CAI* pAI);
void BotMgr_AmmoRunCancel(CAI* pAI);

// Feeding (bots.cfg 'feed'): a hurt alien bot with nothing to fight heals on bodies as alien players do (claws
// and head bite on a body heal the attacker, CProjectile::ImpactDamageObject). BotMgr_FeedWanted: it is hurt and
// a body is worth it (BotRoam then outbids the goals that follow a fight); BotMgr_FeedGoal claims that body and
// gives the floor point to walk to; BotMgr_Feed runs every update while feeding (walkDone: the path to the body
// has ended) and says what to do; BotMgr_FeedEnd stops feeding (unreachable: no path, do not try it again).
enum { BOTFEED_DONE = 0, BOTFEED_WALK = 1, BOTFEED_EAT = 2 };
bool BotMgr_FeedWanted(CAI* pAI);
bool BotMgr_FeedGoal(CAI* pAI, LTVector* pDest, HOBJECT* phBody);
int BotMgr_Feed(CAI* pAI, HOBJECT hBody, bool walkDone);
void BotMgr_FeedEnd(CAI* pAI, bool unreachable = false);
// Debug log of damage dealt and taken by bots (DMG, DMGIN); nType is the DamageType.
void BotMgr_OnDamage(HOBJECT hVictim, HOBJECT hDamager, LTFLOAT fDamage, int nType);
// A bot is about to fire: Predator energy (false = no shot), shoulder cannon charge level, SHOT line in debug.
bool BotMgr_OnFire(HOBJECT hShooter);
// Predator weapons as a player's (bots.cfg 'predammo', 'predlock'):
// A bot's shot is leaving (CAI::FireWeapon, after BotMgr_OnFire and the aim check): spends a spear, marks the
// disc as thrown and, for the disc and shoulder cannon, returns the object their projectile homes on: the
// players' client auto-target (nearest visible character of the target's species on screen), or 0; a cannon
// shot with one is turned toward it (vPath), as the client does. Other shots: hTarget unchanged.
HOBJECT BotMgr_FireCommit(HOBJECT hShooter, HOBJECT hTarget, const LTVector& vFirePos, LTVector& vPath);
// CAI's HasAmmo for a bot's AI weapon: false for a Predator bot's spear gun without spears and its disc
// while thrown (true for everything else, and for every non-bot).
bool BotMgr_WeaponUsable(HOBJECT hAI, const char* szWeapon);
// A Predator bot under 'predammo': keeps its ideal weapon while out of ammo (the disc comes back; the AI
// would drop it for good) and keeps a stuck disc in the wall until it recalls it (an AI's infinite disc pool
// made CPredDisk remove it at once).
bool BotMgr_PredAmmoBot(HOBJECT hAI);
// The melee weapon a bot switches to (AttackGoal::Melee); def = the AI template's.
const char* BotMgr_MeleeWeapon(HOBJECT hAI, const char* def);
// FindStartPoint: adds the live bots' distances to a start point (as players count), returns how many.
// While a bot spawns, a start point cut off for its species gets a nearest distance of -1 (never picked
// when another point is available).
int BotMgr_StartPointDist(const LTVector& vPos, LTFLOAT& fFarDist, LTFLOAT& fNearDist);
// A bot's pounce damage: its multiplayer class's PounceWeaponDamage x bots.cfg damage (def = the AI's).
LTFLOAT BotMgr_PounceDamage(HOBJECT hAI, LTFLOAT def);
// A bot's pounce hits hTouched (hook 'botpouncehit'): what the server does for a player's pounce (CPlayerObj,
// CP_POUNCE_DAMAGE): the hit box resolved to its character, an upper-torso hit, the damage, then a stun of
// damage / PounceStunMod seconds (the victim's BaseStunChance decides, as for players; bots.cfg pouncestun).
void BotMgr_PounceHit(CCharacter* pBot, HOBJECT hTouched, DamageStruct& damage);
// A bot's melee strike (hook 'botmeleepath'): a unit path, as a player's, so the barrel spread applies (the AI's
// target-length path made all 6 claw vectors converge; bots.cfg clawspread).
void BotMgr_MeleePath(HOBJECT hAI, LTVector& vPath);
// May a bot fire at a cloaked target it does not fully see yet (AttackGoal::FireOnTarget)?
bool BotMgr_CanShootCloaked(HOBJECT hAI, HOBJECT hTarget);
// sight rate multiplier of a bot for a cloaked Predator (players' eyes); < 0 = keep the AI's own rule
LTFLOAT BotMgr_CloakSeeFactor(HOBJECT hAI, HOBJECT hStimulus, LTFLOAT fDistanceSqr);
// The sight rate factor for the target's size (bots.cfg 'smallsight', hook 'botseesize'): 1 for standing adults,
// down to 0.5 for small targets (chestburster, facehugger, a crouching human or Predator).
LTFLOAT BotMgr_SizeSeeFactor(HOBJECT hAI, HOBJECT hStimulus);
// a character fired a weapon (CWeapon::Fire): remembered for BotMgr_CloakSeeFactor
void BotMgr_NoteShot(HOBJECT hShooter);
// A bot's shot: the AI's aim point without its single-player lag, a unit path (so the barrel spread
// applies) and a person's drifting aim error (bots.cfg accuracy, distance, target speed).
// Returns false when the gun still points more than 6 degrees away from the target: no shot (a person does
// not fire a rail gun while still turning toward the target).
bool BotMgr_AimError(HOBJECT hShooter, HOBJECT hTarget, const LTVector& vFirePos, LTVector& vPath, bool bFullyAimed);
LTFLOAT BotMgr_DamageScale();  // bots.cfg 'damage': bots' weapon damage relative to players' (default 1)
LTFLOAT BotMgr_BotAccuracy(HOBJECT hBot); // that bot's accuracy: its server-extension skill, else bots.cfg 'accuracy'
LTFLOAT BotMgr_Accuracy();     // bots.cfg 'accuracy': 0 = widest spread .. 1 = narrowest (default 0.5)
bool BotMgr_AlienWallWalk();  // bots.cfg 'wallwalk': Alien bots climb walls
// Alien pounce on maps without AIVolumes: true when the target stands on the waypoint graph.
bool BotMgr_LungeTargetOK(const LTVector& vTarget);
// Alien pounce by the players' rule (hook 'botpouncerule', bots.cfg pounce): BotMgr_PounceRule says whether this AI
// follows it (an adult alien bot); BotMgr_PounceNow whether to pounce now (target within the class's pounce reach,
// a clear arc, on the ground or a wall, after a person's reaction time and the players' 2 s cooldown);
// BotMgr_PounceStarted starts the cooldown once the pounce action is set.
bool BotMgr_PounceRule(CAI* pAI);
bool BotMgr_PounceNow(CAI* pAI);
void BotMgr_PounceStarted(CAI* pAI);
// A bot's pounce flies like a player's (hook 'botpounceflight' in CharacterMovementState_Pouncing, bots.cfg
// pounceflight): true when BotMgr moved it this frame (the AI's own arc is skipped).
class CharacterMovement;
bool BotMgr_PounceFlight(CharacterMovement* pMovement);
// Facehugger bots (bots.cfg huglike, hook 'bothugrule'): BotMgr_HugRule says whether this AI leaps by the players'
// rule (a bot facehugger), BotMgr_HugNow whether to leap now (as BotMgr_PounceNow), BotMgr_HugStarted starts the
// cooldown; BotMgr_HugRoam: a bot facehugger without a victim keeps moving (BotRoam outbids the attack's wait).
bool BotMgr_HugRule(CAI* pAI);
bool BotMgr_HugNow(CAI* pAI);
void BotMgr_HugStarted(CAI* pAI);
bool BotMgr_HugRoam(CAI* pAI);
// AI side steps and step backs (dodge, backoff) on maps without AIVolumes: clear way and floor there.
bool BotMgr_MoveDestOK(CAI* pAI, const LTVector& vDest);
// Every frame after a bot's action (CAI::Update, hook 'firemove'): while a Marine or Predator bot fires at or
// aims at a visible target, it faces it and strafes along short checked legs, as players do (bots.cfg
// 'combatmove'). Also counts the debug CMOVE statistics for every bot.
void BotMgr_CombatMove(CAI* pAI);
// An alien bot's approach (CAIStrategyApproachTarget, hook 'zigzag'): when its target is in view and faces it,
// replaces vDest (the target's position) by a point ahead and to the side, alternating sides (bots.cfg
// 'zigzag'). Returns false (vDest unchanged) otherwise.
bool BotMgr_ZigZag(CAI* pAI, LTVector& vDest);
// Wall ambush of an alien bot with nothing to fight (bots.cfg 'wallambush', used by BotRoam): BotMgr_AmbushPick
// sometimes picks a point on a wall above a busy route and gives the destination to path to (BotMgr_FindPath then
// ends the path on the wall); BotMgr_AmbushUpdate runs every roam update (following: the path is still being
// followed; pathDone: it has ended) and says whether the bot is going there or waiting there (then the roam goal
// does nothing); BotMgr_AmbushEnd ends it (the roam goal ends, e.g. an enemy was seen).
enum { BOTAMB_NONE = 0, BOTAMB_GOING = 1, BOTAMB_WAITING = 2 };
bool BotMgr_AmbushPick(CAI* pAI, LTVector* pDest);
int BotMgr_AmbushUpdate(CAI* pAI, bool following, bool pathDone);
void BotMgr_AmbushEnd(CAI* pAI, const char* why);
// Chestburster bots hide until they grow (bots.cfg 'cbhide'; the roam goal asks every update). BOTCB_NONE: not a
// hiding chestburster (roam as usual); BOTCB_GO: run to *pDest; BOTCB_KEEP: keep following the current path;
// BOTCB_STAY: stand still (hidden, or biting back with the attack goal). BotMgr_CbHideFailed: no path to the spot.
enum { BOTCB_NONE = 0, BOTCB_KEEP = 1, BOTCB_GO = 2, BOTCB_STAY = 3 };
int BotMgr_CbHide(CAI* pAI, bool following, bool pathDone, LTVector* pDest);
void BotMgr_CbHideFailed(CAI* pAI);
// The class may wall-walk (CharacterButes CanWallWalk; a chestburster cannot) and bots.cfg 'wallwalk' is on.
bool BotMgr_CanWallWalk(CAI* pAI);

// Friendly-fire rule for bots: true when friendly fire is off and this damage is between a bot and a
// teammate (same species in team games), which the game only filters between two players.
bool BotMgr_TeamDamageBlocked(HOBJECT hVictim, HOBJECT hDamager);
// Single-player AI rules a player does not have. True when hObj is a bot and the rule's bots.cfg switch is on
// (default on): the hook then takes the player's path.
//   BOTRULE_NAPALM    hook 'botnapalm' (Destructible.cpp): napalm killed an AI at once; a player burns (playerburn)
//   BOTRULE_LIMBS     hook 'botlimbs' (Character.cpp): an alien AI could lose a limb while alive, and a lost leg
//                     made it crawl at InjuredSpeed; players keep their limbs (playerlimbs)
//   BOTRULE_RECOIL    hook 'botrecoil' (AIActionMisc.cpp): a recoiling AI could not start firing (playerrecoil)
//   BOTRULE_IMMUNITY  hook 'botimmune' (AI.cpp): an AI ignored damage from its allies, itself included, and from
//                     alien acid pools; players take it (friendly fire off still blocks teammates) (playerhits)
enum BotPlayerRule { BOTRULE_NAPALM, BOTRULE_LIMBS, BOTRULE_RECOIL, BOTRULE_IMMUNITY };
bool BotMgr_LikePlayer(HOBJECT hObj, int rule);
// Motion tracker (bots.cfg 'tracker', Marine bots): what a Marine player's HUD motion detector shows
// (ClientShellDLL MotionDetector.cpp): every 0.9 s, the moving solid characters within 1920 units (flat
// distance, through walls, cloaked ones too) in the half circle in front of him. A bot reacts to the nearest
// enemy blip that came closer since the last sweep, when it has no target: BotTrackerGoal turns it toward the
// blip and watches for one sweep ('tracker = 1': once per new contact; 'tracker = 2': again at each sweep that
// shows it coming). It still has to see the enemy to shoot. Returns the goal's bid (0 = nothing to do) and
// the blip's position.
LTFLOAT BotMgr_TrackerBid(CAI* pAI, LTVector* pBlip);
// Hearing (bots.cfg 'hearsteps'): what a player hears of the other characters, delivered to the bots' AI
// hearing (CAISenseHearEnemyFootstep, which the multiplayer game never fed: CCharacter::HandleFootstepKey
// notifies the AI in single player only). Called by hook 'hearsteps' for every footstep key of a character's
// animation (players and bots, as each client plays it: CharacterFX.cpp, radius 1000, full volume for every
// movement state), and by hooks 'hearjump', 'hearland', 'hearfirststep' for the jump, landing and first-step
// sounds a player's client sends (radius 600 for jump and landing: SoundButes [Jump] and [Land]).
enum BotHearKind { BOTHEAR_STEP, BOTHEAR_JUMP, BOTHEAR_LAND };
void BotMgr_HearStep(CCharacter* pSource, int nKind);
bool BotMgr_HearSteps();   // bots.cfg 'hearsteps'
// A bot's reaction in InvestigateGoal (hooks 'investglance', 'investnoanim', 'investnosearch'; with 'hearsteps'):
// a person who hears something glances toward it and goes on playing. QuickInvestigate: true for a bot (no
// investigate animation, no 3-5 s search). GlanceStart: the glance's length (turn, then 0.3-0.6 s, a head turn),
// or <= 0 for the stock behaviour. GlanceFollow at its end: go toward the stimulus (true) or resume (false); a
// footstep is followed only when its source was heard again, closer, during the glance.
bool BotMgr_QuickInvestigate(CAI* pAI);
LTFLOAT BotMgr_GlanceStart(CAI* pAI, HOBJECT hSource, bool bFootstep);
bool BotMgr_GlanceFollow(CAI* pAI, bool bFootstep);

// A player (hVictim) died and hKiller is a bot: scores the kill for the bot's side (when 'score' is on) and
// returns true so the game does not count it as the player's suicide. False when hKiller is not a bot.
bool BotMgr_PlayerKilledByBot(HCLIENT hVictim, HOBJECT hKiller);

// Scoreboard rows for bots ('scoreboard' option): MultiplayerMgr::SendGameInfo adds this many bot rows
// (at most maxRows) to a joining player's client list, then writes them in the same format as clients.
int BotMgr_ScoreRowsForJoiner(int maxRows);
// Deathmatch end condition with scoreboard rows: a bot row at the frag or score limit (-1 = not checked).
bool BotMgr_RowReachedLimit(int fragLimit, int scoreLimit);

// Survivor: bots counted by MultiplayerMgr::Update_Survivor (team 0 survivors, 1 mutants, dead ones
// included like players), and turned back into survivors when it starts a new round.
int BotMgr_SurvivorCount(int team);
// Survivor and Hunt: bots per role (0 survivor/hunter, 1 mutant/prey), a pending role change counting.
int BotMgr_RoleCount(int team);
// Hunt: rebalance hunters with a bot (surplus = hunters - wanted); false when no bot can move.
bool BotMgr_HuntBalance(int surplus);

// Alien lifecycle: a facehugger's chestburst on a body (BodyProp, hook 'botcburst'). When the hugger is one
// of our bots, it is replaced by a chestburster bot on the body and true is returned.
bool BotMgr_OnChestburst(HOBJECT hHugger, HOBJECT hBody, int nHostSet);
// Alien lifecycle (hook 'bothugattach', BodyProp.cpp): a facehugger was attached to the body of its victim (a
// player's hugger keeps that body awake by being a client next to it; BotMgr keeps a bot's awake: LithTech stops
// updating objects no client is near, and the body's chestburst timer and animation stopped with it).
void BotMgr_OnFacehugAttach(HOBJECT hHugger, HOBJECT hBody, int nHostSet);
// Hook 'botgrow' (CCharacter::UpdateChestbursterCycle): a bot chestburster is grown; true = BotMgr spawns the adult
// (character set nNewSet, chosen by the engine from the host) in its place and the engine must not SetCharacter it.
bool BotMgr_ChestbursterGrown(HOBJECT hObj, int nNewSet);
// A chestburster bot keeps away from the fight while it grows (BotRoamGoal; bots.cfg 'cbhide').
bool BotMgr_HideDest(CAI* pAI, LTVector* pDest);
void BotMgr_SurvivorNewRound(bool lastRound);

// Server list ('listbots' option): how many bots the GameSpy replies count as players (at most
// maxPlayers - 1 - humans, so the server never looks full to a person), and their names and races.
int BotMgr_ListedBots(int humans, int maxPlayers);
const char* BotMgr_ListedBotName(int i);
int BotMgr_ListedBotRace(int i);
void BotMgr_WriteScoreRows(HMESSAGEWRITE hWrite, int count);

// Path finding through the waypoint graph, used by CAIPathMgr when the level has no AIVolumes.
// Fills pPath with move-to waypoints ending at vDest (an AI-centre position).
LTBOOL BotMgr_FindPath(CAI* pAI, const LTVector& vDest, CAIPath* pPath);

// Admin commands (from the server controller's "<serv> ..." / "/..." or the dedicated server console):
// addbot, removebot (kickbot), bots (botlist), botcount, botreload, bothelp. Returns false when 'text' is
// not a bot command; otherwise fills 'reply' with a one-line answer for the sender.
bool BotMgr_Command(const char* text, char* reply, int replySize);

#endif
