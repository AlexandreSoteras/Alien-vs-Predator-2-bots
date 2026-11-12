#include "stdafx.h"
#include "BotMgr.h"
#include "BotNav.h"
#include "MspClient.h"
#include "ServerExt.h"    // server extension points (no-ops unless a server adds its own: ServerExtStub.cpp)

#include "AI.h"
#include "AINode.h"
#include "AIPath.h"
#include "AITarget.h"
#include "AIAction.h"
#include "AIActionMoveTo.h"
#include "AIStrategyMove.h"
#include "AIButeMgr.h"
#include "AIActionMisc.h"
#include "AISenseMgr.h"
#include "CharacterMovement.h"
#include "CharacterAnimation.h"
#include "CharacterAnimationDefs.h"
#include "AnimationButeMgr.h"
#include "CharacterButeMgr.h"
#include "Destructible.h"
#include "WeaponMgr.h"
#include <typeinfo>
#include "Character.h"
#include "CharacterMgr.h"
#include "GameServerShell.h"
#include "GameStartPoint.h"
#include "MultiplayerMgr.h"
#include "PlayerObj.h"
#include "MsgIDs.h"
#include "Door.h"
#include "PickupObject.h"
#include "Switch.h"
#include "Breakable.h"
#include "HackableWorldmodel.h"
#include "WorldModelDebris.h"
#include "ServerUtilities.h"
#include "Spawner.h"
#include "PredDisk.h"
#include "CharacterHitBox.h"
#include "Projectile.h"
#include "FXButeMgr.h"
#include "AmmoBox.h"
#include "CharacterHitBox.h"
#include "BodyProp.h"
#include "Weapon.h"
#include "Weapons.h"
#include "CVarTrack.h"
#include "CharacterHitBox.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <map>
#include <vector>
#include <algorithm>

using avp2bots::BotNav;
using avp2bots::Vec3;

// the cloak's server variables (Character.cpp: CloakDelay, CloakCostPerSec, CloakInitialCost)
extern CVarTrack g_cvarCloakDelayTime;
extern CVarTrack g_cvarCloakCostPerSecond;
extern CVarTrack g_cvarCloakInitialCost;
// BodyProp declares this a friend (hook 'friend' in BodyProp.h): alien bots pick the bodies they feed on.
struct BotBodyAccess
{
    static LTFLOAT StartTime(const BodyProp* b) { return b->m_fStartTime; }
    static bool Limb(const BodyProp* b) { return b->m_bLimbProp != LTFALSE; }
};

namespace {

enum BotSpecies { BOT_MARINE = 0, BOT_ALIEN, BOT_PREDATOR, BOT_SPECIES_COUNT };

struct SpeciesDef
{
    const char* tag;        // used in bot names and logs
    const char* aiClass;    // object class to spawn
    const char* character;  // CharacterType (character bute set)
    const char* aiTemplate; // AttributeTemplate (AI bute set), must match the character's class
    int startClass;         // Species value FindStartPoint filters on
};

const SpeciesDef kSpecies[BOT_SPECIES_COUNT] = {
    {"Marine", "AIMarine", "Harris", "BasicMarine", Marine},
    {"Alien", "AIAlien", "Drone", "BasicAlien", Alien},
    {"Predator", "AIPredator", "Predator", "BasicPredator", Predator},
};

// Bot classes are the multiplayer classes: their *_MP character set (model, skin, speed, health; the
// AI uses it as is thanks to the 'mpsets' hook) and the default loadout of their
// MPClassWeaponSet (CharacterButes / Weapons attributes), using the matching AI weapons (AIButes
// "<name>_AIWeapon"). An empty weapon keeps the AI template's own weapon. A species' bots take its
// classes in turn (slot 1 -> first class, ...).
struct BotClass
{
    BotSpecies species;
    const char* name;
    const char* character;  // single-player character set the AI is built from (model, animations)
    const char* mpSet;      // multiplayer character set whose health and armor the bot gets
    const char* weapon[3];
    const char* aiClass;    // 0 = the species' AI class; set when the character set's class differs
    const char* aiTemplate; // 0 = the species' AI template (must match the character set's class)
};

const BotClass kClasses[] = {
    {BOT_MARINE, "Smartgunner", "Harrison_MP", "Harrison_MP", {"Smart_Gun", "Shotgun", "Pistol"}},
    {BOT_MARINE, "Demolitions", "Demolition_MP", "Demolition_MP", {"Pulse_Rifle", "Flame_Thrower", "Pistol"}},
    {BOT_MARINE, "Sniper", "Sniper_MP", "Sniper_MP", {"Rail_Gun", "GL_Pipe", "Pistol"}},
    {BOT_MARINE, "Gunner", "Minigunner_MP", "Minigunner_MP", {"SADAR", "Shotgun", "Pistol"}},
    {BOT_PREDATOR, "Maurauder", "Predator_MP", "Predator_MP", {"Predator_Pistol", "Disc", "Net_Gun"}},
    {BOT_PREDATOR, "Striker", "MHawkPredator_MP", "MHawkPredator_MP", {"Shoulder_Cannon", "Predator_Pistol", "Net_Gun"}},
    {BOT_PREDATOR, "Stalker", "LightPredator_MP", "LightPredator_MP", {"Spear_Gun", "Disc", ""}},
    {BOT_PREDATOR, "Bezerker", "HeavyPredator_MP", "HeavyPredator_MP", {"Disc", "Net_Gun", ""}},
    {BOT_ALIEN, "Drone", "Drone_MP", "Drone_MP", {"", "", ""}},
    {BOT_ALIEN, "Runner", "Runner_MP", "Runner_MP", {"", "", ""}},
    {BOT_ALIEN, "Predalien", "Predalien_MP", "Predalien_MP", {"", "", ""}},
};
const int kClassCount = sizeof(kClasses) / sizeof(kClasses[0]);

// A server extension can boost one bot (species, slot) at its spawn: health and armor multipliers and a
// fixed class. The server extensions may define ServerExt_BotSpawnOverride; without
// them the linker falls back on BotMgr_NoSpawnOverride (no boost).
extern "C" bool ServerExt_BotSpawnOverride(int species, int slot, float* healthScale, float* armorScale,
                                           const char** forcedClass);
extern "C" bool BotMgr_NoSpawnOverride(int, int, float*, float*, const char**)
{
    return false;
}
#pragma comment(linker, "/alternatename:_ServerExt_BotSpawnOverride=_BotMgr_NoSpawnOverride")

// A server extension can give one bot (species, slot) its own skill, 0..1, at each spawn (BotSkill below); without
// one the linker falls back on BotMgr_NoBotSkill and the bot follows bots.cfg.
extern "C" bool ServerExt_BotSkill(int species, int slot, float* skill);
extern "C" bool BotMgr_NoBotSkill(int, int, float*)
{
    return false;
}
#pragma comment(linker, "/alternatename:_ServerExt_BotSkill=_BotMgr_NoBotSkill")

// The alien lifecycle (server option 'Lifecycle', DM and Team DM): aliens start as facehuggers, a hug ends in
// a chestburster that grows into the adult of its host. These stages are not in the class rotation.
const BotClass kHugger = {BOT_ALIEN, "Facehugger", "Facehugger_MP", "Facehugger_MP", {"", "", ""}, 0, "BasicFaceHugger"};
const BotClass kBurster = {BOT_ALIEN, "Chestburster", "Chestburster_MP", "Chestburster_MP", {"", "", ""}, 0, "BasicChestBurster"};

const BotClass* ClassFor(BotSpecies sp, int slot)
{
    int n = 0;
    for (int i = 0; i < kClassCount; ++i)
        if (kClasses[i].species == sp)
            ++n;
    if (n == 0)
        return 0;
    int pick = (slot - 1) % n;
    for (int i = 0; i < kClassCount; ++i)
        if (kClasses[i].species == sp && pick-- == 0)
            return &kClasses[i];
    return 0;
}

// Why a combat-movement leg was refused (CheckLeg), for the debug CMOVE line.
enum LegResult { LEG_OK = 0, LEG_WALL, LEG_FLOOR, LEG_GRAPH, LEG_CLIP, LEG_LOS, LEG_RESULTS };

// Combat movement (hook 'firemove': Marines and Predators strafe while they fire) and the alien zig-zag
// (hook 'zigzag'): per-bot state, and statistics since the last status report (debug CMOVE line).
struct CombatMove
{
    bool moving;           // the action animation layer is ours (we set RunF/RunB last frame)
    bool hasLeg;
    LTVector legDest;      // AI-centre point the leg ends at
    LTFLOAT legStart, legUntil;
    int side;              // +1 / -1: side of the last leg (players alternate)
    LTVector progPos;      // progress check
    LTFLOAT progTime;
    int aborts;            // legs aborted in a row
    LTFLOAT pauseUntil;    // no new leg before then
    LTFLOAT lastTime;      // last call (frame time for the statistics)
    LTFLOAT nextLegLog;    // debug LEG lines: at most one per bot every 5 s
    // statistics, seconds with a target: moving (> 50 u/s), in a fire action, both, legs driven, waiting for anim
    float tAtt, tMove, tFire, tFireMove, tDrive, tWait;
    int legs, done, abNoProg, abTime, noLeg;
    int rej[LEG_RESULTS];
    // alien zig-zag
    int zzSide;
    LTFLOAT zzUntil;
    int zzSets, zzFlips, zzFail;

    CombatMove() { Reset(); }
    void Reset()
    {
        moving = hasLeg = false;
        legDest = LTVector(0, 0, 0);
        legStart = legUntil = 0.0f;
        side = 0;
        progPos = LTVector(0, 0, 0);
        progTime = 0.0f;
        aborts = 0;
        pauseUntil = lastTime = nextLegLog = 0.0f;
        zzSide = 0;
        zzUntil = 0.0f;
        ResetStats();
    }
    void ResetStats()
    {
        tAtt = tMove = tFire = tFireMove = tDrive = tWait = 0.0f;
        legs = done = abNoProg = abTime = noLeg = 0;
        for (int i = 0; i < LEG_RESULTS; ++i)
            rej[i] = 0;
        zzSets = zzFlips = zzFail = 0;
    }
};

// Wall ambush of an alien bot with nothing to fight (bots.cfg 'wallambush'): on its way to a wall spot, then
// hanging there; see BotMgr_AmbushPick.
enum { AMB_NONE = 0, AMB_GOING = 1, AMB_WAITING = 2 };
struct AmbushState
{
    int state;
    int spot;              // index in g_ambushSpots
    LTFLOAT since;         // state start
    LTFLOAT until;         // waiting ends then
    LTFLOAT nextTry;       // no new ambush before then
    LTVector waitPos;      // where it hangs
    AmbushState() { Reset(); }
    void Reset()
    {
        state = AMB_NONE;
        spot = -1;
        since = until = nextTry = 0.0f;
        waitPos = LTVector(0, 0, 0);
    }
};

// A chestburster bot hiding until it grows (bots.cfg 'cbhide'), see BotMgr_CbHide.
enum { CBH_NONE = 0, CBH_GOING = 1, CBH_HIDDEN = 2, CBH_FIGHT = 3 };
struct CbHideState
{
    int state;
    LTVector spot;
    LTFLOAT since, nextCheck, nextPick, hiddenAt, calmSince;
    float lastHp;
    LTVector lastPos;      // at the last threat check (is it moving?)
    int picks, flees, fights;
    CbHideState() { Reset(); }
    void Reset()
    {
        state = CBH_NONE;
        spot = LTVector(0, 0, 0);
        since = nextCheck = nextPick = hiddenAt = calmSince = 0.0f;
        lastHp = 0.0f;
        lastPos = LTVector(0, 0, 0);
        picks = flees = fights = 0;
    }
};

struct Bot
{
    BotSpecies species;
    int slot;              // 1..n within its species
    std::string name;      // current object name (unique per spawn)
    LTSmartLink hObj;
    bool alive;
    bool goalsSent;
    LTFLOAT respawnTime;   // when dead: time of the next spawn
    LTFLOAT spawnTime;
    int deaths;
    int scoreSet;          // character set whose scoring class values this bot (its multiplayer class)
    // Predator energy, as a player's (the AI's own is unlimited: CAIPredator::GetPredatorEnergy returns 1000)
    float energy;
    LTFLOAT energyTime;    // last upkeep
    LTFLOAT siftUntil;     // using the energy sift until then: no shooting
    LTFLOAT siftStart;     // when the sift started (its energy keys follow the players' animation)
    int siftKeys;          // energy sift keys done
    LTFLOAT mediStart;     // using the medicomp since then (0 = not), until mediUntil: no shooting
    LTFLOAT mediUntil;
    bool mediKeyDone;      // the medicomp's heal (medicomp_key) is done
    bool walkOnly;         // BotMgr made the bot walk (sift, medicomp)
    LTFLOAT lastShot;      // last shot (re-cloaking waits for a pause in the shooting)
    int cloakWas;          // cloak state at the last upkeep (activation costs energy)
    // Predator ammo as a player's (bots.cfg 'predammo'): spears left, the one disc out (thrown, flying or
    // stuck) or in hand; and the last auto-target of the disc/shoulder cannon (debug LOCK lines)
    int spears;
    bool discOut;
    LTFLOAT discTime;      // when the disc was thrown or last recalled
    LTFLOAT nextDiscCheck;
    LTSmartLink lockTarget;
    // ammunition a player counts and refills (bots.cfg ammopickups, altfire): the class carries the spear gun
    // (spears above) or the pulse rifle (its grenades: none at spawn, from ammo boxes)
    bool usesSpears;
    bool usesGrenades;
    int grenades;
    LTFLOAT altNext;       // no pulse-rifle grenade before then
    LTFLOAT altBackAt;     // holding the grenade launcher: back to the rifle at this time
    LTFLOAT altLastShot;   // last grenade (kill attribution)
    LTSmartLink goalBox;   // the ammo box the roam goal heads for
    // shotgun slugs and SADAR tracking rockets (none at spawn, from ammo boxes; bots.cfg altfire)
    bool usesSlugs;
    bool usesTrackers;
    int slugs;
    int trackers;
    LTFLOAT slugLastShot;
    LTFLOAT trackerLastShot;
    bool lastSadarTracker;   // the last SADAR shot was a tracker (kill attribution)
    // the SADAR's client lock (AutoTargetMgr::UpdateMarineSADAR): the character it is forming on and since when
    LTSmartLink sadarPotential;
    LTFLOAT sadarSince;
    bool sadarLocked;
    // a Stalker's run for spears in a fight (BotMgr_AmmoRunWanted), re-evaluated twice a second
    LTFLOAT ammoRunCheck;
    bool ammoRun;
    // the Predator's vision mode, as a player's (bots.cfg 'predlock'): 0 Normal, 1 HeatVision (locks humans),
    // 2 ElectroVision (aliens), 3 NavVision (Predators), in the Predator_Visions cycle order
    int vision;
    LTFLOAT visionPressAt;   // last vision key press
    LTFLOAT visionNeedSince; // the target has wanted another mode since then (0 = none wanted)
    int frozen;         // consecutive status reports in a fire action without a target, not moving
    LTFLOAT animWaitSince; // the animation has been waiting to switch tracker set since then (0 = not waiting)
    LTVector frozenPos;
    // scoreboard row ('scoreboard' option): a client ID the game's clients show like a player's
    uint32 rowId;          // 0 = no row
    int rowFrags, rowScore, rowConFrags;
    std::string display;   // name on the scoreboard
    // stuck detection
    LTVector checkPos;
    LTFLOAT checkTime;
    LTFLOAT lastHp;        // debug: health + armor at the previous status (damage taken in between)
    int stuckCount;        // consecutive stuck reports
    int offGraph;          // consecutive status reports far from any waypoint
    int survTeam;          // Survivor: 0 survivor, 1 mutant
    int survScore;         // Survivor: the score a player would have (shown on the bot's scoreboard row)
    // Hunt: survTeam is the role (0 hunter, 1 prey); a live bot that swaps roles respawns at switchAt
    int pendingRole;
    LTFLOAT switchAt;
    // alien lifecycle: 0 adult (or no lifecycle), 1 facehugger, 2 chestburster
    int stage;
    int hostSet;               // the hugged body's character set (decides the adult)
    const BotClass* lifeClass; // the adult a chestburster grew into, until its next death
    bool placeAt;              // next spawn at placePos/placeRot (chestburst, growth) instead of a start point
    LTVector placePos;
    LTRotation placeRot;
    LTFLOAT hugSince;          // when the hug flag was first seen (0 = not hugging)
    LTSmartLink hugBody;       // the body this facehugger is attached to (BotMgr_OnFacehugAttach)
    int hugHost;               // ... its character set (-1 = unknown)
    LTFLOAT hugAt;             // ... since then
    int growInto;              // a grown chestburster: the adult set the engine chose (BotMgr_ChestbursterGrown)
    LTFLOAT loseSince;         // attached without the hug flag since (the victim survived)
    const BotClass* forcedClass; // set by 'addbot <species> <class>'; 0 = take the slot's class
    float skill;               // this bot's skill 0..1 from a server extension (each spawn); < 0 = bots.cfg
    CombatMove cm;             // combat movement and zig-zag (reset at each spawn)
    AmbushState amb;           // wall ambush (reset at each spawn)
    CbHideState cbh;           // chestburster hiding (reset at each spawn)
    // debug lines of the player rules (BotMgr_LikePlayer)
    LTFLOAT burnLogAt;     // next BURN line allowed (one per second while burning)
    LTFLOAT recoilLogAt;   // next RECOILFIRE line allowed
    int recoilFires;       // fire actions started while recoiling, this life
    const BotClass* cls;       // class of the current spawn (0 = the species' template)
    // alien kit (adult aliens): the players' weapons of the class (WeaponMgr data, 0 = not in its weapon set)
    const WEAPON* kitClaws; const BARREL* kitClawsBarrel;
    const WEAPON* kitBite;  const BARREL* kitBiteBarrel;
    const WEAPON* kitTail;  const BARREL* kitTailBarrel;   // the fully charged tail barrel
    LTFLOAT tailRollAt;        // next chance to start a tail charge
    LTFLOAT tailSince;         // tail charge started (0 = not charging)
    LTFLOAT biteRollAt;        // next chance to go for a head bite on a live target
    LTFLOAT biteAt;            // a live head bite lands then (0 = none under way)
    LTSmartLink biteTarget;
    LTFLOAT specialTime;       // last head bite or tail strike that hurt its victim (DEATH lines name it)
    const char* specialName;
    HOBJECT specialVictim;
    // feeding on a body
    bool feeding;              // standing at the body, striking it
    LTSmartLink feedBody;
    LTFLOAT feedSince, feedNext, feedBiteAt, feedLookAt;
    int feedKey;               // claw cadence: which fire key of the claws' animation comes next
    int feedStrikes, feedMisses;
    float feedGained;
    HOBJECT feedBad[4];        // bodies this bot could not reach or hit (not tried again)
    int feedBadNext;
    LTSmartLink feedPending;   // the body the last search found (BotMgr_FeedWanted), with its floor point
    LTVector feedPendingDest;
    float feedPendingPath;
    // pounce (players' rule): the 2 s cooldown, when the current opening began, and a pounce under way (log)
    LTFLOAT pounceReadyAt, pounceSeenSince;
    bool pouncing;
    std::string pounceTouched;   // debug: what the pounce under way hit first
    float pounceDecision, pounceWaited;   // debug: distance and reaction wait of the last decision
    // the pounce flight (BotMgr_PounceFlight): the pounce it belongs to (start stamp), a player's flight or the AI's
    LTFLOAT flightStamp, flightStart;
    bool flightPlayer;
    LTVector flightVel;
    LTFLOAT hugHuntAt;         // facehugger: next victim choice (HugHunt)
    // motion tracker (Marines, 'tracker'; TrackerUpkeep, BotMgr_TrackerBid)
    LTFLOAT trackScanAt;       // next sweep of the detector
    LTFLOAT trackReactAt;      // the bot turns toward the contact from then (0 = no contact)
    LTFLOAT trackWatchUntil;   // ... and watches it until then (each sweep that still shows it extends this)
    LTVector trackPos;         // where the contact's blip is
    LTSmartLink trackThreat;
    HOBJECT trackLast;         // the contact the previous sweep reacted to or followed (0 = none)
    std::vector<std::pair<HOBJECT, LTVector> > trackPrev;  // character positions at the previous sweep
    // hearing ('hearsteps'; BotMgr_HearStep, HearUpkeep): when this bot last heard each source, its target
    struct HeardSrc { HOBJECT h; LTFLOAT t; float dist; };
    std::vector<HeardSrc> heard;   // last delivered sound of each source: when, how far
    HOBJECT glanceSrc;         // InvestigateGoal glance (BotMgr_GlanceStart): the footstep source, when, how far
    LTFLOAT glanceAt;
    float glanceDist;
    HOBJECT lastTarget;
    int hearOnsets;            // debug statistics of this life: hearing onsets, targets taken after hearing
};

struct Config
{
    int count[BOT_SPECIES_COUNT];
    LTFLOAT respawnDelay;
    bool log;
    float logMaxMB;        // 'logmax': rotate avp2bots_bots.log to .1 above this size (0 = never)
    bool debug;   // per-bot movement/animation lines and every path request
    bool ladders; // let bots climb ladders
    bool wallwalk; // Alien bots climb walls
    bool cloak;   // Predator bots spawn cloaked
    bool seecloak; // bots see cloaked Predators as players do (BotMgr_CloakSeeFactor)
    bool smallSight; // bots notice small targets more slowly (BotMgr_SizeSeeFactor)
    bool predHeal; // Predator bots use the medicomp, sift and re-cloak like players (decloak, walk only)
    bool mpWeapons; // the players' multiplayer weapons (0 = single-player AI weapons)
    LTFLOAT healthScale; // multiplier on the multiplayer class health and armor (1 = like players)
    LTFLOAT damageScale; // multiplier on the damage bots' weapons do (1 = like players)
    LTFLOAT accuracy;    // 0 = widest weapon spread, 1 = narrowest (a player standing still)
    bool classes; // multiplayer classes and loadouts (0 = the single-player Harris/Drone/Predator)
    bool dm;      // bots in Deathmatch
    bool score;   // kills by and of bots count on the players' and teams' scores
    int fill;     // > 0: keep humans + bots at this number, bots balancing the species (overrides the counts)
    bool scoreboard; // bots get rows on the players' scoreboard and kill messages (experimental)
    bool listbots;   // bots count as players in the server list (GameSpy/master server replies)
    bool survivor;   // bots in Survivor (the races of the server's Survivor settings)
    bool hunt;       // bots in Hunt (the server's hunter and prey races)
    bool lifecycle;  // follow the server's alien lifecycle option (facehugger -> chestburster -> adult)
    bool hugBody;    // keep a bot facehugger's body awake until its chestburst (as a hugging player does)
    bool testSleepBody; // test aid: put a hugged body to sleep at the attach (what no client nearby does)
    bool teamdm;  // bots in Team Deathmatch (teams = species, as for players)
    bool combatMove; // Marine and Predator bots strafe while they fire (hook 'firemove')
    bool zigzag;     // Alien bots zig-zag toward a target that faces them (hook 'zigzag')
    // single-player AI rules a player does not have; 1 = the bot follows the player's rule (BotMgr_LikePlayer)
    bool playerBurn;   // napalm burns a bot over time (0 = the AI's instant death)
    bool playerLimbs;  // alien bots keep their limbs (0 = the AI's limb loss and crawl)
    bool playerRecoil; // a bot starts firing during its recoil animation (0 = the AI waits)
    bool playerHits;   // a bot takes damage from itself, teammates and acid pools (0 = the AI's immunity)
    bool alwaysRun;    // bots always run (0 = the AI walks while investigating)
    int tracker;       // Marine bots use the motion tracker (BotMgr_TrackerBid): 0 off, 1 a glance at each new
                       // contact, 2 also hold and face it while it keeps coming
    bool hearSteps;    // bots hear footsteps, jumps and landings as players do (BotMgr_HearStep)
    float hearLevel;   // ... clearly enough to react: stimulus 1 - distance / radius at least this (0.3: 700 units
                       // for steps, 420 for jumps and landings; a choice, see BotMgr_HearStep)
    bool predAmmo; // Predator bots: spears run out (9 at spawn), one disc that must come back or be recalled
    bool predLock; // Predator bots: the disc and shoulder cannon home on the players' client auto-target only
    bool ammoPickups; // bots open the ammo boxes of their species and take stuck spears, counting the ammo they use
    bool altFire;  // Marine bots fire their alt barrels with the ammo a player would have (pulse-rifle grenades)
    bool feed;     // hurt alien bots heal on bodies (claws, head bite), as alien players do
    bool headbite; // Drone and Runner bots head-bite live targets (instant kill) under a cautious rule
    bool tail;     // alien bots use the charged tail at its multiplayer reach
    bool pounceStun; // a bot's pounce stuns its victim as a player's does
    bool clawSpread; // bot melee strikes spread like players' (unit path)
    bool pounce;     // alien bots pounce by the players' rule (0 = the AI's die roll)
    bool pounceFlight; // a bot's pounce flies like a player's (0 = the AI's arc to its predicted point)
    bool hugLike;      // bot facehuggers hunt and leap like players (0 = the single-player AI facehugger)
    bool wallAmbush;   // alien bots with nothing to fight sometimes wait on a wall near a busy route
    bool wallApproach; // alien bots keep off the open floor a target faces (routes over walls and ledges)
    bool cbHide;       // chestburster bots hide until they grow instead of attacking (lifecycle)
};

BotNav g_nav;
bool g_navLoaded = false;
std::vector<Bot> g_bots;
Config g_cfg;
FILE* g_log = 0;
int g_spawnSerial = 0;
LTFLOAT g_nextStatus = 0.0f;
LTFLOAT g_nextRelations = 0.0f;
LTFLOAT g_nextCommandPoll = 0.0f;
LTFLOAT g_nextTeamLog = 0.0f;
LTFLOAT g_nextFill = 0.0f;
LTFLOAT g_nextDoors = 0.0f;
LTFLOAT g_nextRows = 0.0f;
// debug: how often the size factor slowed a bot's sight, by kind of target (logged with the team scores)
static int g_playerHits[2] = {0, 0};    // players' client-side hits on bots: applied, refused (BotMgr_ValidateHit)
static int g_smallSight[3] = {0, 0, 0};   // facehugger/chestburster, other small class, crouching human/Predator

// Doors on the map (plain, rotating and hinged doors; not switches, breakables or hackables, which derive
// from Door too). A bot that comes close to a closed one "presses Use" on it, as players do; the door
// itself decides (hook 'botdoor': only doors with ActivateTrigger, like for players).
struct DoorInfo
{
    LTSmartLink hDoor;
    LTFLOAT nextTry;
    LTFLOAT checkAt;   // after an activation: when to see whether it opened (0 = nothing pending)
};
std::vector<DoorInfo> g_doors;

// Health and armor pickups (Marine bots take them like players, see hook 'botpickup', and go for them
// when hurt and not fighting).
struct PickupInfo
{
    LTSmartLink hPickup;
    bool health;  // else armor
};
std::vector<PickupInfo> g_pickups;
// Ammo boxes (bots.cfg ammopickups): a box gives each of its pickups (Ammo_Spears_9, Ammo_Combo_Pulse...) to the
// player of its species who touches or uses it (AmmoBox::SetupOpenState: HumanOnly boxes for humans, the others
// for Predators), then stays open until its RespawnTime (15 s on most maps). Bots count what they use of it.
struct AmmoBoxInfo
{
    LTSmartLink hBox;
    bool human;     // HumanOnly box (Marines), else a Predator box
    int spears;     // Spears in its pickups
    int grenades;   // Impact_Grenades in its pickups
    int slugs;      // Shotgun_Slugs
    int trackers;   // SADAR_Tracking_Rockets
    LTFLOAT bannedUntil;  // a bot got stuck on its way here: no box runs to it until then
};
std::vector<AmmoBoxInfo> g_boxes;
int g_lastHumans = -1;
std::string g_map;

void Log(const char* fmt, ...);
struct Bot;
int SurvivorRace(bool mutant);      // Species value, or -1
int HuntRace(int role);             // Species value of hunters (0) or prey (1), or -1
int HuntJoinRole();                 // role a new bot takes in Hunt (the players' join rule)
bool LifecycleOn();                 // the server's lifecycle option applies to bots now
void LifecycleBurst(Bot& b, const LTVector& pos, const LTRotation& rot, int hostSet, bool fallback = false);
int RaceSpecies(int race);          // BotSpecies for a Species value, or -1
void ScoreRowAdd(Bot& b);
void ScoreRowRemove(Bot& b);
void BuildAmbushData();             // wall ambush spots and busy routes of the map (BotMgr_AmbushPick)
void TrafficTick(LTFLOAT now);
void LogAmbushStats();
std::string ObjectLabel(HOBJECT h);
void LogPounceReach();
void HugHunt(Bot& b, CAI* ai, LTFLOAT now);
bool HugAvoid(const Bot& b, CAI* ai, const CCharacter* victim);
} // namespace

void BotMgr_Log(const char* fmt, ...)
{
    if (!g_log)
        return;
    fprintf(g_log, "%9.2f ", g_pLTServer ? g_pLTServer->GetTime() : 0.0f);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    // a public server runs for weeks: keep at most two files of 'logmax' MB (the log and .1)
    if (g_cfg.logMaxMB > 0.0f && ftell(g_log) > (long)(g_cfg.logMaxMB * 1048576.0f))
    {
        fclose(g_log);
        remove("avp2bots_bots.log.1");
        rename("avp2bots_bots.log", "avp2bots_bots.log.1");
        g_log = fopen("avp2bots_bots.log", "a");
    }
}

namespace {

void Log(const char* fmt, ...)
{
    if (!g_log)
        return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    va_end(ap);
    BotMgr_Log("%s", buf);
}

void LoadConfig()
{
    g_cfg.count[BOT_MARINE] = 2;
    g_cfg.count[BOT_ALIEN] = 2;
    g_cfg.count[BOT_PREDATOR] = 2;
    g_cfg.respawnDelay = 4.0f;
    g_cfg.log = true;
    g_cfg.logMaxMB = 20.0f;
    g_cfg.debug = false;
    g_cfg.classes = true;
    g_cfg.ladders = true;
    g_cfg.wallwalk = true;
    g_cfg.cloak = true;
    g_cfg.seecloak = true;
    g_cfg.smallSight = true;
    g_cfg.predHeal = true;
    g_cfg.mpWeapons = true;
    g_cfg.healthScale = 1.0f;
    g_cfg.damageScale = 1.0f;
    g_cfg.accuracy = 0.5f;
    g_cfg.dm = true;
    g_cfg.teamdm = true;
    g_cfg.score = true;
    g_cfg.fill = 0;
    g_cfg.scoreboard = false;
    g_cfg.listbots = false;
    g_cfg.survivor = true;
    g_cfg.hunt = true;
    g_cfg.lifecycle = true;
    g_cfg.hugBody = true;
    g_cfg.testSleepBody = false;
    g_cfg.playerBurn = true;
    g_cfg.playerLimbs = true;
    g_cfg.playerRecoil = true;
    g_cfg.playerHits = true;
    g_cfg.alwaysRun = true;
    g_cfg.tracker = 1;
    g_cfg.hearSteps = true;
    g_cfg.hearLevel = 0.3f;
    g_cfg.predAmmo = true;
    g_cfg.predLock = true;
    g_cfg.ammoPickups = true;
    g_cfg.altFire = true;
    g_cfg.combatMove = true;
    g_cfg.zigzag = true;
    g_cfg.feed = true;
    g_cfg.headbite = true;
    g_cfg.tail = true;
    g_cfg.pounceStun = true;
    g_cfg.clawSpread = true;
    g_cfg.pounce = true;
    g_cfg.pounceFlight = true;
    g_cfg.hugLike = true;
    g_cfg.wallAmbush = true;
    g_cfg.wallApproach = true;
    g_cfg.cbHide = true;

    FILE* f = fopen("botnav\\bots.cfg", "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char key[64];
        float value;
        if (line[0] == '#' || sscanf(line, " %63[^= ] = %f", key, &value) != 2)
            continue;
        if (!_stricmp(key, "marines")) g_cfg.count[BOT_MARINE] = (int)value;
        else if (!_stricmp(key, "aliens")) g_cfg.count[BOT_ALIEN] = (int)value;
        else if (!_stricmp(key, "predators")) g_cfg.count[BOT_PREDATOR] = (int)value;
        else if (!_stricmp(key, "respawn")) g_cfg.respawnDelay = value;
        else if (!_stricmp(key, "log")) g_cfg.log = value != 0.0f;
        else if (!_stricmp(key, "logmax")) g_cfg.logMaxMB = value < 0.0f ? 0.0f : value;
        else if (!_stricmp(key, "debug")) g_cfg.debug = value != 0.0f;
        else if (!_stricmp(key, "classes")) g_cfg.classes = value != 0.0f;
        else if (!_stricmp(key, "ladders")) g_cfg.ladders = value != 0.0f;
        else if (!_stricmp(key, "wallwalk")) g_cfg.wallwalk = value != 0.0f;
        else if (!_stricmp(key, "cloak")) g_cfg.cloak = value != 0.0f;
        else if (!_stricmp(key, "seecloak")) g_cfg.seecloak = value != 0.0f;
        else if (!_stricmp(key, "smallsight")) g_cfg.smallSight = value != 0.0f;
        else if (!_stricmp(key, "predheal")) g_cfg.predHeal = value != 0.0f;
        else if (!_stricmp(key, "mpweapons")) g_cfg.mpWeapons = value != 0.0f;
        else if (!_stricmp(key, "health")) g_cfg.healthScale = value;
        else if (!_stricmp(key, "damage")) g_cfg.damageScale = value < 0.0f ? 0.0f : value;
        else if (!_stricmp(key, "accuracy")) g_cfg.accuracy = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
        else if (!_stricmp(key, "deathmatch")) g_cfg.dm = value != 0.0f;
        else if (!_stricmp(key, "teamdeathmatch")) g_cfg.teamdm = value != 0.0f;
        else if (!_stricmp(key, "score")) g_cfg.score = value != 0.0f;
        else if (!_stricmp(key, "scoreboard")) g_cfg.scoreboard = value != 0.0f;
        else if (!_stricmp(key, "listbots")) g_cfg.listbots = value != 0.0f;
        else if (!_stricmp(key, "survivor")) g_cfg.survivor = value != 0.0f;
        else if (!_stricmp(key, "hunt")) g_cfg.hunt = value != 0.0f;
        else if (!_stricmp(key, "lifecycle")) g_cfg.lifecycle = value != 0.0f;
        else if (!_stricmp(key, "hugbody")) g_cfg.hugBody = value != 0.0f;
        else if (!_stricmp(key, "testsleepbody")) g_cfg.testSleepBody = value != 0.0f;
        else if (!_stricmp(key, "predammo")) g_cfg.predAmmo = value != 0.0f;
        else if (!_stricmp(key, "predlock")) g_cfg.predLock = value != 0.0f;
        else if (!_stricmp(key, "ammopickups")) g_cfg.ammoPickups = value != 0.0f;
        else if (!_stricmp(key, "altfire")) g_cfg.altFire = value != 0.0f;
        else if (!_stricmp(key, "playerburn")) g_cfg.playerBurn = value != 0.0f;
        else if (!_stricmp(key, "playerlimbs")) g_cfg.playerLimbs = value != 0.0f;
        else if (!_stricmp(key, "playerrecoil")) g_cfg.playerRecoil = value != 0.0f;
        else if (!_stricmp(key, "playerhits")) g_cfg.playerHits = value != 0.0f;
        else if (!_stricmp(key, "alwaysrun")) g_cfg.alwaysRun = value != 0.0f;
        else if (!_stricmp(key, "tracker")) g_cfg.tracker = value < 0.0f ? 0 : (value > 2.0f ? 2 : (int)value);
        else if (!_stricmp(key, "hearsteps")) g_cfg.hearSteps = value != 0.0f;
        else if (!_stricmp(key, "hearlevel")) g_cfg.hearLevel = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
        else if (!_stricmp(key, "feed")) g_cfg.feed = value != 0.0f;
        else if (!_stricmp(key, "headbite")) g_cfg.headbite = value != 0.0f;
        else if (!_stricmp(key, "tail")) g_cfg.tail = value != 0.0f;
        else if (!_stricmp(key, "pouncestun")) g_cfg.pounceStun = value != 0.0f;
        else if (!_stricmp(key, "clawspread")) g_cfg.clawSpread = value != 0.0f;
        else if (!_stricmp(key, "pounce")) g_cfg.pounce = value != 0.0f;
        else if (!_stricmp(key, "pounceflight")) g_cfg.pounceFlight = value != 0.0f;
        else if (!_stricmp(key, "huglike")) g_cfg.hugLike = value != 0.0f;
        else if (!_stricmp(key, "wallambush")) g_cfg.wallAmbush = value != 0.0f;
        else if (!_stricmp(key, "wallapproach")) g_cfg.wallApproach = value != 0.0f;
        else if (!_stricmp(key, "cbhide")) g_cfg.cbHide = value != 0.0f;
        else if (!_stricmp(key, "fill")) g_cfg.fill = value < 0.0f ? 0 : (value > 16.0f ? 16 : (int)value);
        else if (!_stricmp(key, "combatmove")) g_cfg.combatMove = value != 0.0f;
        else if (!_stricmp(key, "zigzag")) g_cfg.zigzag = value != 0.0f;
    }
    fclose(f);
}

std::string CurrentMapName()
{
    MultiplayerMgr* mp = g_pGameServerShell ? g_pGameServerShell->GetMultiplayerMgr() : LTNULL;
    if (!mp || !mp->GetGameInfo())
        return std::string();
    const char* full = mp->GetGameInfo()->m_szLevels[mp->GetCurrentLevelIndex()];
    const char* base = full;
    for (const char* c = full; *c; ++c)
        if (*c == '\\' || *c == '/')
            base = c + 1;
    std::string name(base);
    for (size_t i = 0; i < name.size(); ++i)
        name[i] = (char)tolower((unsigned char)name[i]);
    return name;
}

CAI* BotAI(const Bot& b)
{
    HOBJECT h = b.hObj.GetHOBJECT();
    return h ? dynamic_cast<CAI*>(g_pLTServer->HandleToObject(h)) : LTNULL;
}

void SendCommand(HOBJECT h, const char* cmd)
{
    HSTRING hs = g_pLTServer->CreateString(const_cast<char*>(cmd));
    SendTriggerMsgToObject(LTNULL, h, hs);
    g_pLTServer->FreeString(hs);
}

bool IsAlien(const CCharacter* c)
{
    const CharacterButes* cb = c ? c->GetButes() : 0;
    return cb && cb->m_eCharacterClass == ALIEN;
}

// Recent path failures per AI: the attack strategy re-asks every frame for an unreachable target.
struct PathFail
{
    int from, to;
    LTFLOAT until;
    int streak;   // consecutive failures from the same waypoint on an island (any success clears the entry)
    LTFLOAT lastLog;  // PATHFAIL lines: at most one per AI every 5 s outside debug
    int quiet;        // failures not logged since
};
std::map<HOBJECT, PathFail> g_pathFails;
std::map<HOBJECT, LTFLOAT> g_lastCannon;  // time of each bot's last shoulder cannon shot
// Where bots were respawned for being stuck on this map: when a second bot gets stuck at the same spot,
// the spot leaves the waypoint graph (something there stops bots that the map file does not show, e.g. a
// railing beside a ladder on Suk-Stronghold, where a whole TDM team spawning at the same point kept coming back).
struct StuckSpot
{
    LTVector feet;
    std::string bot;
    bool blocked;
};
std::vector<StuckSpot> g_stuckSpots;
// A bot's aim error drifts like a person's instead of being drawn anew for each shot: two values per bot
// (side and up, in units of its current error size) that follow a smooth random walk (BotMgr_AimError).
struct AimDrift
{
    float x, y;
    LTFLOAT time;
};
std::map<HOBJECT, AimDrift> g_aimDrift;

// Time of each character's last shot (players and bots), for the cloaked Predator rule.
std::map<HOBJECT, LTFLOAT> g_lastShot;

// Object handles are reused: forget what the per-bot tables hold for a handle a new bot gets.
void ForgetHandle(HOBJECT h)
{
    g_pathFails.erase(h);
    g_lastCannon.erase(h);
    g_aimDrift.erase(h);
    g_lastShot.erase(h);
}

// --- multiplayer weapons --------------------------------------------------------------------------
// A bot fires the players' multiplayer weapon (WeaponMgr weapon + its first barrel, damage x1). The AI
// needs per-weapon firing behaviour (ideal/fire ranges, bursts, sounds, animation reference) that only
// exists for single-player AI weapons, so each MP weapon borrows that part from its closest AI weapon.
struct MPWeaponDef
{
    const char* mp;        // WeaponMgr name used by players
    const char* aiModel;   // AI weapon (AIButes "<name>_AIWeapon") whose ranges, sounds and animations are borrowed
    int rank;              // lower = preferred as the bot's primary weapon
    // Firing rhythm, close to a player's: shots per second inside a burst, shots per burst, seconds between
    // bursts. The single-player values are tuned for scripted enemies (a rail gun shot every 10 s) and some
    // MP models fire on animation keys every frame or never, so bots fire on this timer only (hook botfirekey).
    // Rates come from the players' view-model fire animations (fire keys per loop, e.g. mSmartgun_pv Fire0:
    // 5 keys in 333 ms); the pauses stand for re-aiming, the disc's return and the Predator's energy
    // (54 points, 6 per plasma pistol shot, refilled with the energy sift).
    float rate;
    int minShots, maxShots;
    float minDelay, maxDelay;
    // Ranges (0 = the single-player AI weapon's). The ideal range decides when the AI walks toward its
    // target instead of shooting: the single-player Smart Gun's ends at 850 units, while its bullets reach 3000.
    float idealMin, idealMax, fireMax;
};

const MPWeaponDef kMPWeapons[] = {
    // the Smart Gun is a belt-fed hose that aims itself: players hold the trigger while the target is in view
    {"Smart_Gun_MP", "Smart_Gun", 1, 15.0f, 30, 60, 0.1f, 0.25f, 150.0f, 1500.0f, 2550.0f},
    {"Minigun_MP", "Minigun", 2, 15.8f, 15, 30, 0.2f, 0.4f, 150.0f, 1500.0f, 2000.0f},
    {"SADAR_MP", "SADAR", 3, 0.86f, 1, 1, 1.2f, 1.8f},
    {"Rail_Gun_MP", "Rail_Gun", 4, 1.0f, 1, 1, 1.0f, 1.6f},
    // the pulse rifle before the flame thrower: the first weapon is the AI's "ideal" one, whose ideal range
    // it walks into (a Demolitions bot walked up to 300 units of everyone, rarely firing its rifle)
    {"Pulse_Rifle_MP", "Pulse_Rifle", 5, 8.0f, 5, 12, 0.2f, 0.5f},
    // the AI fired the flame thrower up to 550 units, where its slow flames miss a moving target
    {"Flame_Thrower_MP", "Flame_Thrower", 6, 6.4f, 6, 15, 0.15f, 0.4f, 100.0f, 300.0f, 450.0f},
    {"Grenade_Launcher_MP", "GL_Pipe", 7, 0.86f, 1, 1, 1.2f, 1.6f},
    {"Shotgun_MP", "Shotgun", 8, 0.83f, 1, 1, 1.2f, 1.5f},
    {"Pistol_MP", "Pistol", 9, 4.0f, 2, 4, 0.3f, 0.7f},
    {"Shoulder_Cannon_MP", "Shoulder_Cannon", 1, 1.0f, 1, 1, 1.2f, 2.2f},  // charged shots (BotMgr_OnFire)
    {"Spear_Gun_MP", "Spear_Gun", 2, 2.0f, 1, 2, 0.6f, 1.2f},
    {"Disc_MP", "Disc", 3, 1.25f, 1, 1, 2.5f, 4.0f},
    {"Predator_Pistol_MP", "Predator_Pistol", 4, 2.2f, 2, 3, 1.5f, 3.0f},
    {"Net_Gun_MP", "Net_Gun", 5, 1.5f, 1, 1, 3.0f, 5.0f},
    // melee (rate 0: the AI model's animation-driven rhythm); never a primary weapon
    {"Spear_MP", "Spear", 99, 0.0f, 0, 0, 0.0f, 0.0f},
    // the wristblades of the Predator classes without a combistick (Stalker, Striker): melee only (rank 100
    // keeps them out of the loadout); the AI spear lends its melee ranges and animations
    {"Wristblades_MP", "Spear", 100, 0.0f, 0, 0, 0.0f, 0.0f},
};

const MPWeaponDef* FindMPWeapon(const char* name)
{
    for (size_t i = 0; i < sizeof(kMPWeapons) / sizeof(kMPWeapons[0]); ++i)
        if (!_stricmp(kMPWeapons[i].mp, name))
            return &kMPWeapons[i];
    return 0;
}

// Registers "<mp name>" as an AI weapon once; false when the game data lacks a piece.
bool EnsureMPWeapon(const MPWeaponDef& def)
{
    if (g_pAIWeaponButeMgr->GetIDByName(def.mp) >= 0)
        return true;
    int model = g_pAIWeaponButeMgr->GetIDByName(def.aiModel);
    WEAPON* weapon = g_pWeaponMgr->GetWeapon(def.mp);
    BARREL* barrel = weapon ? g_pWeaponMgr->GetBarrel(weapon->aBarrelTypes[0]) : 0;
    if (model < 0 || !weapon || !barrel)
    {
        Log("MPWEAPON %s unavailable (ai model %d, weapon %p, barrel %p)", def.mp, model, weapon, barrel);
        return false;
    }
    AIWBM_AIWeapon w = g_pAIWeaponButeMgr->GetAIWeapon(model);
    w.Name = def.mp;
    w.WeaponName = def.mp;
    w.BarrelName = barrel->szName;
    w.fDamageMultiplier = 1.0f;   // players' damage (single-player AI weapons use 0.2 to 0.3)
    if (def.idealMax > 0.0f)
    {
        w.fMinIdealRange = def.idealMin;
        w.fMaxIdealRange = def.idealMax;
    }
    if (def.fireMax > 0.0f)
        w.fMaxFireRange = def.fireMax;
    // A bot takes its own splash as a player does (rule 'playerhits', hook 'botimmune'), and a person does not fire
    // a rocket or a plasma bolt at a target inside its blast. The single-player fire ranges were made for an AI
    // that ignored its own splash (SADAR from 75 units, plasma pistol from 125, both with a 250-unit blast):
    // inside the blast radius of the MP ammo the AI takes another weapon (CAI::UpdateWeapon picks by range).
    if (g_cfg.playerHits)
    {
        const AMMO* ammo = g_pWeaponMgr->GetAmmo(barrel->nAmmoType);
        if (ammo && ammo->nAreaDamage > 0 && (LTFLOAT)ammo->nAreaDamageRadius > w.fMinFireRange)
        {
            Log("MPWEAPON %s: no shot inside its own blast (min fire range %.0f -> %d, ammo %s)", def.mp,
                w.fMinFireRange, ammo->nAreaDamageRadius, ammo->szName);
            w.fMinFireRange = (LTFLOAT)ammo->nAreaDamageRadius;
            if (w.fMinIdealRange < w.fMinFireRange)
                w.fMinIdealRange = w.fMinFireRange;
            if (w.fMaxIdealRange < w.fMinIdealRange)
                w.fMaxIdealRange = w.fMinIdealRange;
        }
    }
    if (def.rate > 0.0f)
    {
        w.bWaitForAnimation = LTFALSE;
        w.fBurstSpeed = def.rate;
        w.nMinBurstShots = def.minShots;
        w.nMaxBurstShots = def.maxShots;
        w.fMinBurstDelay = def.minDelay;
        w.fMaxBurstDelay = def.maxDelay;
    }
    w.bInfiniteAmmo = LTTRUE;     // bots do not pick up ammo
    // The AI animation styles know the single-player weapon names ("Pulse_Rifle"), not the "_MP" ones; without
    // this the bot would run and aim with the unarmed animations.
    if (w.AnimReference.empty())
        w.AnimReference = g_pAIWeaponButeMgr->GetAIWeapon(model).WeaponName;
    int id = g_pAIWeaponButeMgr->AddWeapon(w);
    Log("MPWEAPON %s registered as AI weapon %d (barrel %s, behaviour of %s)", def.mp, id, barrel->szName, def.aiModel);
    return true;
}

// The pulse rifle's grenade launcher (Pulse_Rifle_MP's second barrel, Pulse_Rifle_Grenade_Barrel_MP: Impact_Grenades,
// 50 + 250 splash over 200 units) as an AI weapon of its own (bots.cfg altfire). It borrows the single-player AI's
// grenade behaviour (PR_Grenade_AltFire: alt-fire sound and effects, one shot, 1.5-3 s between grenades) without its
// link back to the single-player rifle. A person fires one at a target beyond its blast and within a fair arc:
// 300-1200 units (chosen). A Marine player has none at spawn and gets them from ammo boxes (Ammo_Combo_Pulse 4).
const char* const kPRGrenade = "Pulse_Rifle_Grenade_MP";

void EnsurePRGrenade()
{
    if (g_pAIWeaponButeMgr->GetIDByName(kPRGrenade) >= 0)
        return;
    const int model = g_pAIWeaponButeMgr->GetIDByName("PR_Grenade_AltFire");
    WEAPON* weapon = g_pWeaponMgr->GetWeapon("Pulse_Rifle_MP");
    BARREL* barrel = weapon ? g_pWeaponMgr->GetBarrel(weapon->aBarrelTypes[1]) : 0;
    if (model < 0 || !barrel)
    {
        Log("MPWEAPON %s unavailable (ai model %d, barrel %p)", kPRGrenade, model, barrel);
        return;
    }
    AIWBM_AIWeapon w = g_pAIWeaponButeMgr->GetAIWeapon(model);
    w.Name = kPRGrenade;
    w.WeaponName = "Pulse_Rifle_MP";
    w.BarrelName = barrel->szName;
    w.fDamageMultiplier = 1.0f;
    w.AltWeaponName = "";
    w.fAltWeaponChance = 0.0f;
    w.fMinIdealRange = w.fMinFireRange = 300.0f;
    w.fMaxIdealRange = w.fMaxFireRange = 1200.0f;
    w.bWaitForAnimation = LTFALSE;
    w.fBurstSpeed = 1.0f;
    w.nMinBurstShots = w.nMaxBurstShots = 1;
    w.bInfiniteAmmo = LTTRUE;  // BotMgr counts the grenades (Bot::grenades)
    if (w.AnimReference.empty())
        w.AnimReference = g_pAIWeaponButeMgr->GetAIWeapon(model).WeaponName;
    const int id = g_pAIWeaponButeMgr->AddWeapon(w);
    Log("MPWEAPON %s registered as AI weapon %d (barrel %s, behaviour of PR_Grenade_AltFire, %.1f-%.1f s apart)",
        kPRGrenade, id, barrel->szName, w.fMinBurstDelay, w.fMaxBurstDelay);
}

// Alien claws: the AI's own claws barrel does 10 damage a hit; alien players hit with their class's barrel
// (Claws_Barrel 40 for a Drone, ClawsRunner_Barrel 30, ClawsPredalien_Barrel 60). A bot alien gets an AI weapon
// with the players' barrel and the AI claws' melee behaviour. The single-player tail alt-fire is not linked: it
// switched back to the claws after 0.1 s, before any tail animation's fire key (0.3 s and later), so its strikes
// landed as claws; bots charge and strike with the players' tail instead (AlienKitUpdate).
const char* AlienClaws(const char* mpSet)
{
    struct { const char* set; const char* barrel; const char* name; } const kClaws[] = {
        {"Drone_MP", "Claws_Barrel", "Claws_Drone_MP"},
        {"Runner_MP", "ClawsRunner_Barrel", "Claws_Runner_MP"},
        {"Predalien_MP", "ClawsPredalien_Barrel", "Claws_Predalien_MP"},
    };
    for (size_t i = 0; i < sizeof(kClaws) / sizeof(kClaws[0]); ++i)
    {
        if (!mpSet || _stricmp(mpSet, kClaws[i].set))
            continue;
        if (g_pAIWeaponButeMgr->GetIDByName(kClaws[i].name) >= 0)
            return kClaws[i].name;
        const int model = g_pAIWeaponButeMgr->GetIDByName("Claws");
        if (model < 0 || !g_pWeaponMgr->GetBarrel(const_cast<char*>(kClaws[i].barrel)))
        {
            Log("MPWEAPON %s unavailable", kClaws[i].name);
            return 0;
        }
        AIWBM_AIWeapon w = g_pAIWeaponButeMgr->GetAIWeapon(model);
        w.Name = kClaws[i].name;
        w.BarrelName = kClaws[i].barrel;
        w.fDamageMultiplier = 1.0f;
        w.AltWeaponName = "";       // no single-player tail (see above)
        w.fAltWeaponChance = 0.0f;
        int id = g_pAIWeaponButeMgr->AddWeapon(w);
        Log("MPWEAPON %s registered as AI weapon %d (barrel %s, behaviour of Claws)", kClaws[i].name, id,
            kClaws[i].barrel);
        return kClaws[i].name;
    }
    return 0;
}

// The default weapons of a multiplayer class (its MPClassWeaponSet), best first, at most 3.
std::vector<std::string> MPLoadout(const char* mpSet)
{
    std::vector<const MPWeaponDef*> found;
    int set = mpSet ? g_pCharacterButeMgr->GetSetFromModelType(mpSet) : -1;
    WEAPON_SET* ws = set >= 0 ? g_pWeaponMgr->GetWeaponSet((int)g_pCharacterButeMgr->GetMPClassWeaponSet(set)) : 0;
    for (int i = 0; ws && i < ws->nNumWeapons; ++i)
    {
        if (!ws->bDefaultWeapon[i])
            continue;
        const MPWeaponDef* def = FindMPWeapon(ws->szWeaponName[i]);
        if (def && def->rank < 100 && EnsureMPWeapon(*def))
            found.push_back(def);
    }
    for (size_t i = 1; i < found.size(); ++i)  // insertion sort by rank
        for (size_t j = i; j > 0 && found[j]->rank < found[j - 1]->rank; --j)
            std::swap(found[j], found[j - 1]);
    std::vector<std::string> out;
    for (size_t i = 0; i < found.size() && i < 3; ++i)
        out.push_back(found[i]->mp);
    return out;
}

// Registers every bot weapon at once, before the first bot exists. The AI weapon table is a std::vector and
// each AI keeps pointers into it, so a weapon added while bots are alive would leave them dangling pointers.
void RegisterAllWeapons()
{
    // cheap test: the last weapon registered below exists and the Marine senses are ours already
    const int lastId = g_pAIWeaponButeMgr->GetIDByName("Claws_Predalien_MP");
    const int marine = g_pAIButeMgr->GetTemplateIDByName("BasicMarine");
    if (lastId >= 0 && (marine < 0 ||
        g_pAIButeMgr->GetSense(marine, (CAISenseSeeEnemy*)0).fLongTimeDistance == 2000.0f))
        return;
    if (lastId < 0 && !g_bots.empty())
    {
        // bots alive hold pointers into the weapon table: adding to it now could move it under them
        for (size_t i = 0; i < g_bots.size(); ++i)
            if (g_bots[i].alive && BotAI(g_bots[i]))
            {
                Log("MPWEAPON table missing with bots alive: not registering (no new bots until the next map)");
                return;
            }
    }
    for (size_t i = 0; i < sizeof(kMPWeapons) / sizeof(kMPWeapons[0]); ++i)
        EnsureMPWeapon(kMPWeapons[i]);
    EnsurePRGrenade();
    AlienClaws("Drone_MP");
    AlienClaws("Runner_MP");
    AlienClaws("Predalien_MP");

    // Sight reaction like a player's. The single-player templates take up to 2 s to notice an enemy past
    // 1500 units and a Marine 1 s at mid range (the time the SeeEnemy stimulation needs to fill up); fights
    // with multiplayer damage last under a second, so this decided most of them. Bots: 0.2 s up to 400
    // units, rising to 1 s at 2000. Read by each AI when it is created, so set before the first bot.
    const char* const kTemplates[] = {"BasicMarine", "BasicAlien", "BasicPredator", "BasicFaceHugger", "BasicChestBurster"};
    for (size_t i = 0; i < sizeof(kTemplates) / sizeof(kTemplates[0]); ++i)
    {
        const int id = g_pAIButeMgr->GetTemplateIDByName(kTemplates[i]);
        if (id < 0)
            continue;
        if (g_pAIButeMgr->GetSense(id, (CAISenseSeeEnemy*)0).fLongTimeDistance == 2000.0f)
            continue;  // already set
        AIBM_SeeEnemySense& see = const_cast<AIBM_SeeEnemySense&>(g_pAIButeMgr->GetSense(id, (CAISenseSeeEnemy*)0));
        Log("SENSES %s: see enemy %.2f s within %.0f, %.2f s beyond %.0f -> 0.20 s within 400, 1.00 s beyond 2000",
            kTemplates[i], see.fIncreaseStimulationTime, see.fShortTimeDistance, see.fLongIncreaseStimulationTime,
            see.fLongTimeDistance);
        see.fIncreaseStimulationTime = 0.2f;
        see.fShortTimeDistance = 400.0f;
        see.fLongIncreaseStimulationTime = 1.0f;
        see.fLongTimeDistance = 2000.0f;
    }
}

// species of the bot being placed by Spawn (FindStartPoint skips its cut-off start points), -1 otherwise
int g_spawningSpecies = -1;

// Predator energy (WEAPONS.TXT): the Predator_Energy pool (54), 6 per plasma pistol shot (Predator_Pistol_Ammo_MP),
// 3 + 2 + 3 to charge the shoulder cannon to levels 0, 1, 2, cloak 5 to switch on then 0.1/s (server cvars
// CloakInitialCost, CloakCostPerSec). The energy sift refills it: 9 x 11 energy over its 4.5 s animation
// (pEnergySift_pv.abc "Select", energy_sift_key), during which a player cannot shoot.
// The engine charges an AI nothing (CAIPredator::GetPredatorEnergy returns 1000, so HandleCloakToggle always has
// the energy and UpdateCloakEffect never runs dry): Bot::energy is the bot's pool and follows the same rules.
float PredatorEnergyMax()
{
    AMMO_POOL* pool = g_pWeaponMgr ? g_pWeaponMgr->GetAmmoPool(const_cast<char*>("Predator_Energy")) : 0;
    const int n = pool ? pool->GetMaxAmount(LTNULL) : 0;
    return n > 0 ? (float)n : 54.0f;
}

// energy per shot of a Predator "ammo" (AmmoPoolAmount): Energy_Sift_Ammo 11 per key, Medicomp_Ammo 30
float EnergyAmmo(const char* ammo, float def)
{
    AMMO* a = g_pWeaponMgr ? g_pWeaponMgr->GetAmmo(const_cast<char*>(ammo)) : 0;
    return a && a->nAmmoPerShot > 0 ? (float)a->nAmmoPerShot : def;
}

float CloakVar(CVarTrack& v, float def) { return v.IsInitted() ? v.GetFloat() : def; }
float CloakInitialCost() { return CloakVar(g_cvarCloakInitialCost, 5.0f); }
float CloakCostPerSec() { return CloakVar(g_cvarCloakCostPerSecond, 0.1f); }
float CloakDelay() { return CloakVar(g_cvarCloakDelayTime, 5.0f); }

// The players' view-model animations ("Select", read from the model files): the sift's energy_sift_key times
// and length (pEnergySift_pv.abc: 9 keys, 4.5 s), the medicomp's medicomp_key and length (pMedicomp_pv.abc:
// the heal at 3.457 s, force_last_weapon_key at 5.935 s of 6.0 s). A player's client sends CP_ENERGY_SIFT /
// CP_MEDICOMP at each key (WeaponModel.cpp); the weapon cannot change or fire until the animation ends, and
// both weapons have SpeedMod 0: the player is encumbered (walks, no wall walking).
const float kSiftLength = 4.5f;
const float kSiftKeys[] = {1.679f, 2.002f, 2.261f, 2.325f, 2.648f, 2.971f, 3.100f, 3.294f, 3.552f};
const int kSiftKeyCount = sizeof(kSiftKeys) / sizeof(kSiftKeys[0]);
const float kMediLength = 6.0f;
const float kMediKey = 3.457f;
// A human presses the medicomp key when hurt with nobody to fight (GameServerShell: refused at full health or
// under Medicomp_Ammo energy); the threshold is a conservative human habit, not a game value.
const float kMediHealthFraction = 0.5f;
// Re-cloaking after a decloak: a person presses cloak again once the cloak delay is over and the shooting has
// paused for a moment (human habit, conservative).
const float kRecloakAfterShot = 1.5f;

// Spears a Predator player spawns with: the Spears pool's DefaultAmount (9; MaxAmount 27), which
// CPlayerObj::AquireDefaultWeapon gives for the spear gun's primary barrel.
int SpearsAtSpawn()
{
    AMMO_POOL* pool = g_pWeaponMgr ? g_pWeaponMgr->GetAmmoPool(const_cast<char*>("Spears")) : 0;
    return pool && pool->nDefaultAmount > 0 ? pool->nDefaultAmount : 9;
}

Bot* FindBot(HOBJECT h)
{
    for (size_t i = 0; i < g_bots.size(); ++i)
        if (g_bots[i].alive && g_bots[i].hObj.GetHOBJECT() == h)
            return &g_bots[i];
    return 0;
}

// While the sift or the medicomp plays, the bot walks like an encumbered player (predheal).
void WalkOnly(Bot& b, CAI* ai, bool on)
{
    if (on)
    {
        if (!g_cfg.predHeal)
            return;
        ai->GetMovement()->SetEncumbered(LTTRUE);
        b.walkOnly = true;
    }
    else if (b.walkOnly)
    {
        b.walkOnly = false;
        if (!ai->GetInjuredMovement())  // a lost leg keeps the engine's own encumbrance
            ai->GetMovement()->SetEncumbered(LTFALSE);
    }
}

void StartSift(Bot& b, CAI* ai, const char* why)
{
    const LTFLOAT now = g_pLTServer->GetTime();
    b.siftStart = now;
    b.siftUntil = now + kSiftLength;
    b.siftKeys = 0;
    WalkOnly(b, ai, true);
    if (g_cfg.debug)
        Log("SIFT %s energy=%.0f (%s)", b.name.c_str(), b.energy, why);
}

void StartMedicomp(Bot& b, CAI* ai)
{
    const LTFLOAT now = g_pLTServer->GetTime();
    b.mediStart = now;
    b.mediUntil = now + kMediLength;
    b.mediKeyDone = false;
    WalkOnly(b, ai, true);
    if (g_cfg.debug)
        Log("MEDICOMP %s start hp=%.0f/%.0f energy=%.0f", b.name.c_str(), ai->GetHitPoints(),
            ai->GetDestructible()->GetMaxHitPoints(), b.energy);
}

// A tracker set switch waits for the playing animation to send a break or to end; some animations never do,
// and the bot stood still with a path to follow (35 % of the stuck reports on six maps: Aliens in their idle
// set, Predators and Marines in some armed run sets). After a second of waiting while the new set allows
// movement and the old one does not, the switch is forced.
// --- motion tracker -------------------------------------------------------------------------------
// A Marine player's HUD motion detector (ClientShellDLL MotionDetector.cpp): every sweep (0.5 s pause +
// 0.4 s sweep, GetMDAnimDestRect) it shows the solid models that moved since the last sweep
// (ResetDisplayList), within MAX_MD_RANGE = 1920 units of flat distance (heights ignored, walls too), only in
// the half circle in front of the player (TestObjectForLocation, UpdateDispBlipList: forward dot > 0), with
// a beep pitched by the nearest one. Cloaked Predators are solid models: they show. Nothing behind shows.
const float kTrackRange = 1920.0f;
const LTFLOAT kTrackPeriod = 0.9f;
// A blip shows during the 0.4 s sweep; the bot reacts at its end (a person reading the HUD).
const LTFLOAT kTrackReact = 0.4f;

void TrackerUpkeep(Bot& b, CAI* ai)
{
    if (!g_cfg.tracker || b.species != BOT_MARINE)
        return;
    const LTFLOAT now = g_pLTServer->GetTime();
    if (now < b.trackScanAt)
        return;
    b.trackScanAt = now + kTrackPeriod;
    LTVector me = ai->GetPosition();
    me.y = 0.0f;
    LTVector fwd = ai->GetForward();
    fwd.y = 0.0f;
    std::vector<std::pair<HOBJECT, LTVector> > cur;
    CCharacter* best = 0;
    LTVector bestPos;
    float bestDist = 1e9f;
    int blips = 0;
    for (CCharacterMgr::CharacterIterator it = g_pCharacterMgr->BeginCharacters(); it != g_pCharacterMgr->EndCharacters(); ++it)
    {
        CCharacter* c = *it;
        if (!c || c == ai || c->IsDead())
            continue;
        const uint32 flags = g_pLTServer->GetObjectFlags(c->m_hObject);
        if (!(flags & FLAG_SOLID) || (flags & FLAG_REALLYCLOSE))
            continue;  // TestObjectForType
        LTVector p;
        g_pLTServer->GetObjectPos(c->m_hObject, &p);
        cur.push_back(std::make_pair(c->m_hObject, p));
        const LTVector* prev = 0;
        for (size_t k = 0; k < b.trackPrev.size(); ++k)
            if (b.trackPrev[k].first == c->m_hObject)
                prev = &b.trackPrev[k].second;
        if (!prev || (p - *prev).MagSqr() < 1.0f)
            continue;  // still since the last sweep (or new): no blip
        LTVector d = p;
        d.y = 0.0f;
        d -= me;
        const float dist = d.Mag();
        if (dist > kTrackRange || fwd.Dot(d) <= 0.0f)
            continue;  // out of range or behind: not on the detector
        ++blips;
        if (!g_pCharacterMgr->AreEnemies(*ai, *c))
            continue;
        LTVector pd = *prev;
        pd.y = 0.0f;
        pd -= me;
        if (dist >= pd.Mag())
            continue;  // not coming closer
        if (dist < bestDist)
        {
            best = c;
            bestDist = dist;
            bestPos = p;
        }
    }
    b.trackPrev.swap(cur);
    const HOBJECT last = b.trackLast;
    b.trackLast = best ? best->m_hObject : LTNULL;
    if (!best || ai->HasTarget())
        return;
    // mode 1: a glance at each new contact (turn toward it and watch for one sweep); mode 2: also keep facing a
    // contact while each sweep shows it coming closer
    const bool fresh = best->m_hObject != last;
    if (!fresh && g_cfg.tracker < 2)
        return;
    if (fresh || b.trackReactAt <= 0.0f || now > b.trackWatchUntil)
        b.trackReactAt = now + kTrackReact;
    b.trackWatchUntil = now + kTrackReact + kTrackPeriod;  // watch until the next sweep has been read
    b.trackPos = bestPos;
    b.trackThreat = best->m_hObject;
    if (g_cfg.debug)
    {
        LTVector d = bestPos;
        d.y = 0.0f;
        d -= me;
        const float fm = fwd.Mag();
        float cs = (fm > 0.01f && bestDist > 0.01f) ? fwd.Dot(d) / (fm * bestDist) : 1.0f;
        cs = cs < -1.0f ? -1.0f : (cs > 1.0f ? 1.0f : cs);
        Log("TRACK %s -> %s dist=%.0f off=%.0f blips=%d%s", b.name.c_str(), ObjectLabel(best->m_hObject).c_str(),
            bestDist, acosf(cs) * 57.29578f, blips, fresh ? "" : " (still coming)");
    }
}

// --- hearing ---------------------------------------------------------------------------------------
// A player's client plays every other character's footstep keys with FOOTSTEP_SOUND_RADIUS = 1000 at volume
// 100 whatever it does (CCharacterFX::PlayFootstepSound divides by the movement state only for the local
// player's own steps), plus a 'Move' sound (radius 900) with most of them; a player's jump and landing reach
// the others at radius 600 (SoundButes [Jump], [Land]). No occlusion: walls do not stop it.
const float kHearStepRadius = 1000.0f;
const float kHearJumpRadius = 600.0f;
// AIButes HearEnemyFootstepMaxNoiseDistance (BaseAIAttrib): the AI hears a sound of volume v up to v times
// this and its stimulus is 1 - distance / (v x this), so v = radius / this makes the stimulus fade to 0 at the
// sound's radius.
const float kHearNoiseDistance = 1000.0f;
const LTFLOAT kHearOnsetGap = 2.0f;   // a source heard again after this long counts (and is logged) anew

LTFLOAT HeardAgo(const Bot& b, HOBJECT h, LTFLOAT now)
{
    for (size_t k = 0; k < b.heard.size(); ++k)
        if (b.heard[k].h == h)
            return now - b.heard[k].t;
    return -1.0f;
}

void HearUpkeep(Bot& b, CAI* ai)
{
    // debug: a new target, and whether this bot heard it in the last 3 s (did hearing lead to the fight?)
    const HOBJECT t = ai->HasTarget() ? ai->GetTarget().GetObject().GetHOBJECT() : LTNULL;
    if (t == b.lastTarget)
        return;
    b.lastTarget = t;
    if (t && g_cfg.debug)
        Log("TARGETSET %s -> %s heard=%.1f", b.name.c_str(), ObjectLabel(t).c_str(), HeardAgo(b, t, g_pLTServer->GetTime()));
}

void AnimWatchdog(Bot& b, CAI* ai)
{
    CharacterAnimation* an = ai->GetAnimation();
    if (!an || !an->IsWaitingToSwitch() || an->MovementAllowed(LTTRUE) || !an->MovementAllowed(LTFALSE))
    {
        b.animWaitSince = 0.0f;
        return;
    }
    const LTFLOAT now = g_pLTServer->GetTime();
    if (b.animWaitSince == 0.0f)
    {
        b.animWaitSince = now;
        return;
    }
    if (now - b.animWaitSince < 1.0f)
        return;
    if (g_cfg.debug)
    {
        TrackerSet* ts = g_pAnimationButeMgr->GetTrackerSet((uint16)an->GetTrackerSet());
        Log("ANIMUNSTICK %s waited %.1f s to switch to %s", b.name.c_str(), now - b.animWaitSince, ts ? ts->szName : "?");
    }
    an->ForceTrackerSwitch();
    b.animWaitSince = 0.0f;
}

// --- alien kit: head bite, tail, feeding -----------------------------------------------------------------
// An alien player has more than claws (its MPClassWeaponSet): Drone and Runner also have Head_Bite, all three
// classes have Tail. The AI fights with its claws (AIActionFireMelee); BotMgr adds the rest and fires it through
// the weapon code like a player does (Strike). Timings come from the players' view-model animations:
const float kBiteWindup = 0.183f;   // aJaw_pv.abc Fire0 (500 ms): the bite's fire_key at 183 ms
const float kBiteLoop = 0.5f;
const float kClawKey2 = 0.161f;     // aClaws_pv.abc Fire0 (500 ms, all three claws): fire_keys at 0 and ~161 ms
const float kClawLoop = 0.5f;
const float kTailCharge = 1.6f;     // aTail_pv.abc Charging: CHARGE_KEY at 0.8 s and 1.6 s (barrel 1 -> 2 -> 3)
const float kTailHold = 2.0f;       // ... the Charging animation's length: a charge not released by then is dropped
// Live head bites as the client allows them (AutoTargetMgr in CSHELL.DLL, measured earlier): a non-Alien,
// non-Synthetic character whose head is within ~167 units and ~5.7 degrees of the aim, not while pouncing.
const float kBiteReachLive = 167.0f;
const float kBiteAimDeg = 5.7f;
// How often a bot tries a special attack: the single-player alien's rhythm for its alternate weapon
// (Claws_AIWeapon: AltWeaponProbability 0.3, AltWeaponDelay [1, 4] s). No human figure is known.
const float kAltChance = 0.3f;
const float kAltDelayMin = 1.0f, kAltDelayMax = 4.0f;

HOBJECT g_strikeShooter = LTNULL;   // DMG lines name a bot's head bite or tail, not its current AI weapon
const char* g_strikeName = 0;

const BARREL* LastBarrel(const BARREL* b)
{
    // the tail's barrels form a charging chain (Tail_Charging_Barrel_1 -> 2 -> 3); bots strike fully charged
    for (int n = 0; b && b->szNextBarrel[0] && n < 8; ++n)
    {
        const BARREL* next = g_pWeaponMgr->GetBarrel(const_cast<char*>(b->szNextBarrel));
        if (!next)
            break;
        b = next;
    }
    return b;
}

// The players' alien weapons of the bot's class (adults only; facehuggers and chestbursters have none of these).
void SetKit(Bot& b, const BotClass* cls)
{
    b.kitClaws = b.kitBite = b.kitTail = 0;
    b.kitClawsBarrel = b.kitBiteBarrel = b.kitTailBarrel = 0;
    if (b.species != BOT_ALIEN || b.stage != 0 || !cls || !cls->mpSet)
        return;
    const int set = g_pCharacterButeMgr->GetSetFromModelType(cls->mpSet);
    WEAPON_SET* ws = set >= 0 ? g_pWeaponMgr->GetWeaponSet((int)g_pCharacterButeMgr->GetMPClassWeaponSet(set)) : 0;
    for (int i = 0; ws && i < ws->nNumWeapons; ++i)
    {
        if (!ws->bDefaultWeapon[i])
            continue;
        const char* n = ws->szWeaponName[i];
        WEAPON* w = g_pWeaponMgr->GetWeapon(n);
        const BARREL* barrel = w ? g_pWeaponMgr->GetBarrel(w->aBarrelTypes[0]) : 0;
        if (!barrel)
            continue;
        if (!_strnicmp(n, "Claws", 5) && !b.kitClaws)
        {
            b.kitClaws = w;
            b.kitClawsBarrel = barrel;
        }
        else if (!_stricmp(n, "Head_Bite"))
        {
            b.kitBite = w;
            b.kitBiteBarrel = barrel;
        }
        else if (!_stricmp(n, "Tail"))
        {
            b.kitTail = w;
            b.kitTailBarrel = LastBarrel(barrel);
        }
    }
}

// a new spawn: no kit state carried over
void ResetKit(Bot& b)
{
    b.kitClaws = b.kitBite = b.kitTail = 0;
    b.kitClawsBarrel = b.kitBiteBarrel = b.kitTailBarrel = 0;
    b.tailRollAt = b.tailSince = b.biteRollAt = b.biteAt = 0.0f;
    b.biteTarget = (HOBJECT)LTNULL;
    b.specialTime = 0.0f;
    b.specialName = 0;
    b.specialVictim = LTNULL;
    b.feeding = false;
    b.feedBody = (HOBJECT)LTNULL;
    b.feedSince = b.feedNext = b.feedBiteAt = b.feedLookAt = 0.0f;
    b.feedKey = b.feedStrikes = b.feedMisses = 0;
    b.feedGained = 0.0f;
    for (int i = 0; i < 4; ++i)
        b.feedBad[i] = LTNULL;
    b.feedBadNext = 0;
    b.feedPending = (HOBJECT)LTNULL;
    b.feedPendingDest = LTVector(0.0f, 0.0f, 0.0f);
    b.feedPendingPath = 0.0f;
    b.pounceReadyAt = b.pounceSeenSince = 0.0f;
    b.pouncing = false;
    b.pounceTouched.clear();
    b.pounceDecision = b.pounceWaited = 0.0f;
    b.flightStamp = b.flightStart = 0.0f;
    b.flightPlayer = false;
    b.flightVel = LTVector(0.0f, 0.0f, 0.0f);
    b.hugHuntAt = 0.0f;
}

// One strike of an alien player's weapon from a bot at hAt (its target or a body), through the weapon code as a
// player's (CWeapon::Fire with hTestObj): the projectile code deals the damage, explodes a bitten head and heals
// the attacker (CProjectile::ImpactDamageObject). The weapon comes from the bot's own weapon container; the AI's
// current weapon is left as it is. The path is a unit vector, so the barrel's spread applies as for a player
// (bots.cfg accuracy). Returns the health the bot gained.
float Strike(CAI* ai, const WEAPON* wd, const BARREL* bd, HOBJECT hAt, const LTVector& aim, const char* name)
{
    CWeapon* cur = ai ? ai->GetCurrentWeaponPtr() : LTNULL;
    CWeapons* ws = cur ? cur->GetParent() : LTNULL;
    AMMO* ad = bd ? g_pWeaponMgr->GetAmmo(bd->nAmmoType) : LTNULL;
    AMMO_POOL* pool = ad ? g_pWeaponMgr->GetAmmoPool(const_cast<char*>(ad->szAmmoPool)) : LTNULL;
    if (!ws || !wd || !ad || !pool)
        return 0.0f;
    CWeapon* w = ws->GetWeapon((DBYTE)wd->nId);
    if (!w)
    {
        ws->ObtainWeapon((DBYTE)wd->nId, (uint8)bd->nId, (uint8)pool->nId);
        w = ws->GetWeapon((DBYTE)wd->nId);
        if (!w)
            return 0.0f;
    }
    const int oldBarrel = w->GetBarrelId(), oldAmmo = w->GetAmmoId();
    w->SetBarrelId(bd->nId);
    w->SetAmmoId(ad->nId);
    WFireInfo fi;
    fi.hFiredFrom = ai->m_hObject;
    fi.hTestObj = hAt;
    fi.vFirePos = ai->GetPosition();   // melee strikes start at the AI's centre (CAI::FireWeapon)
    fi.vFlashPos = fi.vFirePos;
    LTVector path = aim - fi.vFirePos;
    if (path.MagSqr() < 0.01f)
        path = ai->GetForward();
    path.Norm();
    fi.vPath = path;
    fi.nSeed = (DBYTE)GetRandom(2, 255);
    fi.fPerturbR = fi.fPerturbU = 1.0f - BotMgr_BotAccuracy(ai->m_hObject);
    fi.fDamageMultiplier = g_cfg.damageScale;
    const LTFLOAT before = ai->GetDestructible()->GetHitPoints();
    g_strikeShooter = ai->m_hObject;
    g_strikeName = name;
    w->Fire(fi);
    g_strikeShooter = LTNULL;
    g_strikeName = 0;
    w->SetBarrelId(oldBarrel);
    w->SetAmmoId(oldAmmo);
    return ai->GetDestructible()->GetHitPoints() - before;
}

// horizontal angle (degrees) between a facing and a direction; 0 when the direction is nearly vertical
float FlatAngleDeg(const LTVector& fwd, const LTVector& to)
{
    const float la = sqrtf(fwd.x * fwd.x + fwd.z * fwd.z), lb = sqrtf(to.x * to.x + to.z * to.z);
    if (la < 0.001f || lb < 16.0f)
        return 0.0f;
    return acosf(Clamp((fwd.x * to.x + fwd.z * to.z) / (la * lb), -1.0f, 1.0f)) * 57.29578f;
}

bool ClearLine(const LTVector& a, const LTVector& b)
{
    IntersectQuery q;
    IntersectInfo info;
    q.m_From = a;
    q.m_To = b;
    q.m_Flags = IGNORE_NONSOLID;   // world geometry
    return !g_pLTServer->IntersectSegment(&q, &info);
}

// a character's head as its model has it now (players aim at it), else the AI's head offset
LTVector HeadPoint(CCharacter* c)
{
    const ModelSkeleton sk = c->GetModelSkeleton();
    const ModelNode n = g_pModelButeMgr->GetSkeletonNode(sk, "Head_node");
    const char* node = n != eModelNodeInvalid ? g_pModelButeMgr->GetSkeletonNodeName(sk, n) : 0;
    HMODELNODE hNode;
    LTransform t;
    LTVector v;
    if (node && g_pModelLT->GetNode(c->m_hObject, const_cast<char*>(node), hNode) == LT_OK &&
        g_pModelLT->GetNodeTransform(c->m_hObject, hNode, t, LTTRUE) == LT_OK)
    {
        g_pLTServer->GetTransformLT()->GetPos(t, v);
        return v;
    }
    return c->GetPosition() + c->GetHeadOffset();
}

bool Pouncing(CAI* ai)
{
    const CharacterMovementState s = ai->GetMovement()->GetMovementState();
    return s == CMS_PRE_POUNCE_JUMP || s == CMS_POUNCE_JUMP || s == CMS_POUNCE_FALL || s == CMS_POUNCING;
}

// The weapon layer of an alien's animation ("Claws", "Head_Bite", "Tail"), so others see the strike. ref = 0 gives
// the layer back to the AI: its claws during a melee action (AIActionFireMelee sets them), else "None".
void SetWeaponAnim(CAI* ai, const char* ref)
{
    CharacterAnimation* an = ai->GetAnimation();
    if (!an)
        return;
    if (!ref)
    {
        ref = "None";
        const AIWBM_AIWeapon* wb = ai->GetCurrentWeaponButesPtr();
        CWeapon* w = ai->GetCurrentWeaponPtr();
        if (wb && w && w->GetWeaponData() && dynamic_cast<const AIActionFireMelee*>(ai->GetCurrentAction()))
            ref = !wb->AnimReference.empty() ? wb->AnimReference.c_str() : w->GetWeaponData()->szName;
    }
    an->SetLayerReference(ANIMSTYLELAYER_WEAPON, ref);
}

const char* ClassName(const Bot& b)
{
    return b.cls ? b.cls->name : kSpecies[b.species].tag;
}

// Per update, for a live adult alien bot that is not feeding: the tail (charge, then strike at the tail's reach)
// and the live head bite. Both are rare by construction; the claws stay the AI's.
void AlienKitUpdate(Bot& b, CAI* ai, LTFLOAT now)
{
    if (b.species != BOT_ALIEN || b.stage != 0 || b.feeding || !b.kitClaws)
        return;
    HOBJECT ht = ai->HasTarget() ? ai->GetTarget().GetObject().GetHOBJECT() : LTNULL;
    CCharacter* tc = ht ? dynamic_cast<CCharacter*>(g_pLTServer->HandleToObject(ht)) : LTNULL;
    if (tc && tc->IsDead())
        tc = LTNULL;

    // a head bite under way lands after the bite animation's wind-up if the head is still within the barrel's range
    // (the player has pressed fire); the jaw is shown meanwhile
    if (b.biteAt > 0.0f)
    {
        if (now < b.biteAt)
        {
            SetWeaponAnim(ai, "Head_Bite");
            return;
        }
        b.biteAt = 0.0f;
        HOBJECT hv = b.biteTarget.GetHOBJECT();
        CCharacter* vc = hv ? dynamic_cast<CCharacter*>(g_pLTServer->HandleToObject(hv)) : LTNULL;
        if (vc && !vc->IsDead() && b.kitBite)
        {
            const LTVector head = HeadPoint(vc);
            const float dist = (head - ai->GetPosition()).Mag();
            if (dist <= (float)b.kitBiteBarrel->nRange)
            {
                const float healed = Strike(ai, b.kitBite, b.kitBiteBarrel, hv, head, "Head_Bite");
                if (vc->IsDead())
                {
                    b.specialTime = now;
                    b.specialName = "Head_Bite";
                    b.specialVictim = hv;
                }
                if (g_cfg.debug)
                    Log("HEADBITE %s class=%s -> %s dist=%.0f %s healed=%.0f hp=%.0f", b.name.c_str(), ClassName(b),
                        ObjectLabel(hv).c_str(), dist, vc->IsDead() ? "kill" : "miss", healed, ai->GetHitPoints());
            }
            else if (g_cfg.debug)
                Log("HEADBITE %s class=%s -> %s dist=%.0f out of reach", b.name.c_str(), ClassName(b),
                    ObjectLabel(hv).c_str(), dist);
        }
        SetWeaponAnim(ai, 0);
        return;
    }

    // a tail charge under way: released at full charge when the target is within the tail's reach in a clear line
    // (the player keeps it in the crosshair while charging: the bot turns to it); dropped when the target comes
    // within the claws' reach first (the claws strike sooner) or when the charge has been held for the whole
    // Charging animation
    if (b.tailSince > 0.0f)
    {
        const float held = (float)(now - b.tailSince);
        const char* why = 0;
        float dist = 0.0f;
        if (!g_cfg.tail || !b.kitTail)
            why = "off";
        else if (!tc)
            why = "lost";
        else
        {
            const LTVector aim = tc->GetPosition() + tc->GetChestOffset();
            const LTVector to = aim - ai->GetPosition();
            dist = to.Mag();
            if (held < kTailCharge && dist <= (float)b.kitClawsBarrel->nRange)
                why = "close";
            else if (held >= kTailCharge && dist <= (float)b.kitTailBarrel->nRange && !Pouncing(ai) &&
                     ClearLine(ai->GetPosition(), aim))
            {
                ai->SnapTurn(to);
                const float hpBefore = tc->GetDestructible()->GetHitPoints() + tc->GetDestructible()->GetArmorPoints();
                Strike(ai, b.kitTail, b.kitTailBarrel, ht, aim, "Tail");
                const float hpAfter = tc->GetDestructible()->GetHitPoints() + tc->GetDestructible()->GetArmorPoints();
                if (hpAfter < hpBefore)
                {
                    b.specialTime = now;
                    b.specialName = "Tail";
                    b.specialVictim = ht;
                }
                if (g_cfg.debug)
                    Log("TAIL %s class=%s -> %s dist=%.0f held=%.1f damage=%.0f%s", b.name.c_str(), ClassName(b),
                        ObjectLabel(ht).c_str(), dist, held, hpBefore - hpAfter, tc->IsDead() ? " kill" : "");
                why = "struck";
            }
            else if (held >= kTailHold)
                why = "timeout";
        }
        if (why)
        {
            b.tailSince = 0.0f;
            if (g_cfg.debug && strcmp(why, "struck"))
                Log("TAILDROP %s class=%s reason=%s held=%.1f dist=%.0f", b.name.c_str(), ClassName(b), why, held, dist);
            SetWeaponAnim(ai, 0);
        }
        else
            SetWeaponAnim(ai, "Tail");
        return;
    }

    if (!tc || Pouncing(ai))
        return;
    const LTVector tpos = tc->GetPosition();
    const float dist = (tpos - ai->GetPosition()).Mag();

    // a new tail charge: only while the target is beyond the claws' reach (a player who can claw does not stop to
    // charge) and close enough to come within the tail's reach before the charge must be released
    if (g_cfg.tail && b.kitTail && now >= b.tailRollAt)
    {
        const float run = g_pCharacterButeMgr->GetRunSpeed(ai->GetCharacter());
        if (dist > (float)b.kitClawsBarrel->nRange && dist <= (float)b.kitTailBarrel->nRange + run * kTailHold)
        {
            b.tailRollAt = now + GetRandom(kAltDelayMin, kAltDelayMax);
            if (GetRandom(0.0f, 1.0f) < kAltChance)
            {
                b.tailSince = now;
                SetWeaponAnim(ai, "Tail");
                if (g_cfg.debug)
                    Log("TAILCHARGE %s class=%s -> %s dist=%.0f", b.name.c_str(), ClassName(b), ObjectLabel(ht).c_str(), dist);
                return;
            }
        }
    }

    // a live head bite (bots.cfg headbite): the client's rule (above), and only on a victim that cannot turn on the
    // alien, as people land it: stunned or netted, or with the alien behind or beside it (90 degrees or more off
    // its facing); then the special-attack rhythm
    if (g_cfg.headbite && b.kitBite && now >= b.biteRollAt)
    {
        const CharacterButes* tb = tc->GetButes();
        if (!tb || tb->m_eCharacterClass == ALIEN || tb->m_eCharacterClass == SYNTHETIC)
            return;
        const LTVector head = HeadPoint(tc);
        const LTVector to = head - ai->GetPosition();
        if (to.Mag() > kBiteReachLive || FlatAngleDeg(ai->GetForward(), to) > kBiteAimDeg)
            return;
        const LTVector& vf = tc->GetForward();
        const LTVector away = ai->GetPosition() - tpos;
        const bool unseen = vf.x * away.x + vf.z * away.z <= 0.0f;
        const bool helpless = tc->IsStunned() || tc->IsNetted();
        if (!(unseen || helpless) || !ClearLine(ai->GetPosition(), head))
            return;
        b.biteRollAt = now + GetRandom(kAltDelayMin, kAltDelayMax);
        if (GetRandom(0.0f, 1.0f) < kAltChance)
        {
            b.biteAt = now + kBiteWindup;
            b.biteTarget = ht;
            SetWeaponAnim(ai, "Head_Bite");
            if (g_cfg.debug)
                Log("HEADBITEGO %s class=%s -> %s dist=%.0f %s", b.name.c_str(), ClassName(b), ObjectLabel(ht).c_str(),
                    to.Mag(), helpless ? "helpless" : "unseen");
        }
    }
}

// Per update, for a live Predator bot: the cloak's costs, the sift's and the medicomp's keys, and what a player
// does between fights: sift when low, medicomp when hurt, cloak again after a decloak.
void EnergyUpkeep(Bot& b, CAI* ai)
{
    const LTFLOAT now = g_pLTServer->GetTime();
    const float dt = (float)(now - b.energyTime);
    b.energyTime = now;
    if (b.species != BOT_PREDATOR)
        return;
    const float maxEnergy = PredatorEnergyMax();
    // the cloak: CloakInitialCost when it goes on (spawning cloaked; a re-cloak below pays itself), CloakCostPerSec
    // while it ramps up or is on, and it drops when the pool is empty (CCharacter::UpdateCloakEffect)
    int cloak = (int)ai->GetCloakState();
    if (cloak != (int)CLOAK_STATE_INACTIVE && b.cloakWas == (int)CLOAK_STATE_INACTIVE)
        b.energy -= CloakInitialCost();
    const bool cloakOn = cloak == (int)CLOAK_STATE_RAMPUP || cloak == (int)CLOAK_STATE_ACTIVE;
    if (cloakOn)
        b.energy -= CloakCostPerSec() * dt;
    if (b.energy < 0.0f)
        b.energy = 0.0f;
    if (cloakOn && b.energy <= 0.0f)
    {
        ai->ForceCloakOff();
        if (g_cfg.debug)
            Log("CLOAKOFF %s out of energy", b.name.c_str());
    }
    b.cloakWas = (int)ai->GetCloakState();

    // the sift: Energy_Sift_Ammo (11) at each energy_sift_key, the pool capped at its maximum; each key also
    // shows the sift's effect and decloaks (CCharacter::HandleEnergySift)
    if (b.siftUntil > 0.0f)
    {
        const float perKey = EnergyAmmo("Energy_Sift_Ammo", 11.0f);
        while (b.siftKeys < kSiftKeyCount && now >= b.siftStart + kSiftKeys[b.siftKeys])
        {
            ++b.siftKeys;
            b.energy = b.energy + perKey > maxEnergy ? maxEnergy : b.energy + perKey;
            if (g_cfg.predHeal)
            {
                ai->BotEnergySiftKey();
                ai->ForceCloakOff(LTTRUE);  // HandleEnergySift skips it when the bot holds no weapon
            }
        }
        if (now >= b.siftUntil)
        {
            b.siftUntil = 0.0f;
            WalkOnly(b, ai, false);
        }
    }
    // the medicomp: at its medicomp_key, Medicomp_Ammo (30) energy, full health and a decloak (HandleMedicomp)
    if (b.mediUntil > 0.0f)
    {
        if (!b.mediKeyDone && now >= b.mediStart + kMediKey)
        {
            b.mediKeyDone = true;
            const float hp = ai->GetHitPoints();
            ai->BotMedicomp();
            b.energy -= EnergyAmmo("Medicomp_Ammo", 30.0f);
            if (b.energy < 0.0f)
                b.energy = 0.0f;
            if (g_cfg.debug)
                Log("MEDICOMP %s heal hp=%.0f->%.0f energy=%.0f", b.name.c_str(), hp, ai->GetHitPoints(), b.energy);
        }
        if (now >= b.mediUntil)
        {
            b.mediUntil = 0.0f;
            WalkOnly(b, ai, false);
        }
    }

    // between fights: the medicomp when hurt (a sift first when the energy is short), a sift when low
    if (b.siftUntil == 0.0f && b.mediUntil == 0.0f && !ai->HasTarget())
    {
        CDestructible* d = ai->GetDestructible();
        const bool hurt = g_cfg.predHeal && d->GetHitPoints() < kMediHealthFraction * d->GetMaxHitPoints();
        if (hurt && b.energy >= EnergyAmmo("Medicomp_Ammo", 30.0f))
            StartMedicomp(b, ai);
        else if (hurt)
            StartSift(b, ai, "hurt");
        else if (b.energy < 0.5f * maxEnergy)
            StartSift(b, ai, "no target");
    }

    // cloak again once the delay a decloak started is over (HandleCloakToggle refuses earlier, and in water)
    cloak = (int)ai->GetCloakState();
    if (g_cfg.predHeal && g_cfg.cloak && b.siftUntil == 0.0f && b.mediUntil == 0.0f && ai->HasCloakDevice()
        && (cloak == (int)CLOAK_STATE_INACTIVE || cloak == (int)CLOAK_STATE_RAMPDOWN)
        && now - ai->BotCloakDelayStart() >= CloakDelay() && now - b.lastShot >= kRecloakAfterShot
        && b.energy >= CloakInitialCost() && !ai->GetMovement()->IsInLiquidContainer(CM_CONTAINER_LOWER))
    {
        ai->HandleCloakToggle();
        cloak = (int)ai->GetCloakState();
        if (cloak == (int)CLOAK_STATE_RAMPUP)
        {
            b.energy -= CloakInitialCost();
            if (g_cfg.debug)
                Log("RECLOAK %s energy=%.0f shot=%.1fs ago target=%d", b.name.c_str(), b.energy, now - b.lastShot,
                    ai->HasTarget() ? 1 : 0);
        }
        b.cloakWas = cloak;
    }
}

bool Spawn(Bot& b)
{
    RegisterAllWeapons();
    const SpeciesDef& sp = kSpecies[b.species];
    // the players' rule (GameServerShell::FindStartPoint): the species' start point whose nearest player or
    // bot is farthest (random when nobody else is in), skipping the ones the generator found cut off from
    // the main play area for this kind of bot (e.g. above a pit only an Alien can climb out of)
    GameStartPoint* start = LTNULL;
    LTVector pos(0, 0, 0);
    LTRotation rot;
    rot.Init();
    // lifecycle: an alien (re)spawns as a facehugger, as players do; burster and adult stages are set by
    // the chestburst and the growth
    if (b.species == BOT_ALIEN && LifecycleOn())
    {
        if (b.stage == 0 && !b.lifeClass)
            b.stage = 1;
    }
    else
    {
        b.stage = 0;
        b.lifeClass = 0;
    }
    for (int attempt = 0; attempt < 12 && !b.placeAt; ++attempt)
    {
        g_spawningSpecies = b.species;
        GameStartPoint* sp2 = g_pGameServerShell->FindStartPoint(LTNULL, sp.startClass, LTNULL);
        g_spawningSpecies = -1;
        if (!sp2)
            break;
        LTVector p2;
        g_pLTServer->GetObjectPos(sp2->m_hObject, &p2);
        start = sp2;
        pos = p2;
        if (!g_nav.IsBadSpawn(Vec3(p2.x, p2.y, p2.z), b.species == BOT_ALIEN))
            break;
    }
    if (start && !b.placeAt)
        g_pLTServer->GetObjectRotation(start->m_hObject, &rot);
    if (b.placeAt)
    {
        pos = b.placePos;
        rot = b.placeRot;
        b.placeAt = false;
    }

    const BotClass* cls = b.forcedClass ? b.forcedClass : (g_cfg.classes ? ClassFor(b.species, b.slot) : 0);
    if (b.stage == 1)
        cls = &kHugger;
    else if (b.stage == 2)
        cls = &kBurster;
    else if (b.lifeClass)
        cls = b.lifeClass;
    // a server extension's skill for this bot (every stage), read at each spawn
    {
        float s = -1.0f;
        b.skill = ServerExt_BotSkill((int)b.species, b.slot, &s) ? Clamp(s, 0.0f, 1.0f) : -1.0f;
    }
    // a bot boosted by a server extension: its class, then its health and armor below
    float boostHp = 1.0f, boostArmor = 1.0f;
    const char* boostClass = 0;
    const bool boosted = b.stage == 0 &&
                         ServerExt_BotSpawnOverride((int)b.species, b.slot, &boostHp, &boostArmor, &boostClass);
    if (boosted && boostClass && boostClass[0])
    {
        const BotClass* bc = 0;
        for (int i = 0; i < kClassCount && !bc; ++i)
            if (kClasses[i].species == b.species && !_stricmp(kClasses[i].name, boostClass))
                bc = &kClasses[i];
        if (bc)
            cls = bc;
        else
            Log("BOOST %s%d: no %s class named %s, keeping %s", sp.tag, b.slot, sp.tag, boostClass,
                cls ? cls->name : "the template");
    }
    const char* character = cls ? cls->character : sp.character;
    const char* aiClass = (cls && cls->aiClass) ? cls->aiClass : sp.aiClass;
    const char* aiTemplate = (cls && cls->aiTemplate) ? cls->aiTemplate : sp.aiTemplate;

    char name[64];
    sprintf(name, "avp2bot_%s%d_%d", sp.tag, b.slot, ++g_spawnSerial);
    char props[512];
    int len = sprintf(props, "%s Name %s; CharacterType %s; AttributeTemplate %s", aiClass, name, character, aiTemplate);
    static const char* const kWeaponProps[3] = {"Weapon", "SecondaryWeapon", "TertiaryWeapon"};
    std::vector<std::string> loadout;
    if (cls && g_cfg.mpWeapons)
        loadout = MPLoadout(cls->mpSet);
    if (cls && g_cfg.mpWeapons && b.species == BOT_ALIEN && loadout.empty())
    {
        if (const char* claws = AlienClaws(cls->mpSet))
            loadout.push_back(claws);
    }
    if (cls && loadout.empty())
    {
        // fallback: the single-player AI weapons that match the class's default loadout
        for (int w = 0; w < 3; ++w)
            if (cls->weapon[w][0])
                loadout.push_back(cls->weapon[w]);
    }
    for (size_t w = 0; w < loadout.size() && w < 3; ++w)
        len += sprintf(props + len, "; %s %s", kWeaponProps[w], loadout[w].c_str());
    if (b.species == BOT_PREDATOR && g_cfg.cloak)
        len += sprintf(props + len, "; StartCloaked 1");

    BaseClass* obj = SpawnObject(props, pos, rot);
    CAI* ai = obj ? dynamic_cast<CAI*>(obj) : LTNULL;
    if (!ai)
    {
        Log("SPAWNFAIL %s props=\"%s\"", name, props);
        b.respawnTime = g_pLTServer->GetTime() + 5.0f;
        return false;
    }

    b.name = name;
    ForgetHandle(ai->m_hObject);
    b.hObj = LTSmartLink(ai->m_hObject);
    b.alive = true;
    b.goalsSent = false;
    b.spawnTime = g_pLTServer->GetTime();
    b.checkPos = pos;
    b.checkTime = b.spawnTime;
    b.stuckCount = 0;
    b.offGraph = 0;
    b.scoreSet = (cls && cls->mpSet) ? g_pCharacterButeMgr->GetSetFromModelType(cls->mpSet) : -1;
    if (b.scoreSet < 0)
        b.scoreSet = ai->GetCharacter();
    b.hugSince = 0.0f;
    b.hugBody = (HOBJECT)LTNULL;
    b.hugHost = -1;
    b.hugAt = 0.0f;
    b.growInto = -1;
    b.energy = PredatorEnergyMax();
    b.energyTime = g_pLTServer->GetTime();
    b.siftUntil = 0.0f;
    b.siftStart = 0.0f;
    b.siftKeys = 0;
    b.mediStart = 0.0f;
    b.mediUntil = 0.0f;
    b.mediKeyDone = false;
    b.walkOnly = false;
    b.lastShot = 0.0f;
    b.cloakWas = 0;
    b.spears = SpearsAtSpawn();
    b.discOut = false;
    b.discTime = 0.0f;
    b.nextDiscCheck = 0.0f;
    b.lockTarget = (HOBJECT)LTNULL;
    // a player spawns with the primary barrel's pool only (CPlayerObj::AquireDefaultWeapon): no pulse-rifle grenades
    b.usesSpears = std::find(loadout.begin(), loadout.end(), std::string("Spear_Gun_MP")) != loadout.end();
    b.usesGrenades = std::find(loadout.begin(), loadout.end(), std::string("Pulse_Rifle_MP")) != loadout.end();
    b.grenades = 0;
    b.altNext = 0.0f;
    b.altBackAt = 0.0f;
    b.altLastShot = -100.0f;
    b.goalBox = (HOBJECT)LTNULL;
    // slugs for a class with the shotgun, trackers with the SADAR: none at spawn either
    b.usesSlugs = std::find(loadout.begin(), loadout.end(), std::string("Shotgun_MP")) != loadout.end();
    b.usesTrackers = std::find(loadout.begin(), loadout.end(), std::string("SADAR_MP")) != loadout.end();
    b.slugs = b.trackers = 0;
    b.slugLastShot = b.trackerLastShot = -100.0f;
    b.lastSadarTracker = false;
    b.sadarPotential = (HOBJECT)LTNULL;
    b.sadarSince = 0.0f;
    b.sadarLocked = false;
    b.ammoRunCheck = 0.0f;
    b.ammoRun = false;
    // a player (re)spawns in Normal vision: the client resets the mode when he dies (PS_DYING/PS_DEAD:
    // ViewModeMGR::SetNormalMode)
    b.vision = 0;
    b.visionPressAt = 0.0f;
    b.visionNeedSince = 0.0f;
    b.frozen = 0;
    b.animWaitSince = 0.0f;
    b.loseSince = 0.0f;
    b.lastHp = 0.0f;
    b.burnLogAt = 0.0f;
    b.recoilLogAt = 0.0f;
    b.recoilFires = 0;
    b.cm.Reset();
    b.trackScanAt = g_pLTServer->GetTime() + GetRandom(0.0f, 0.9f);  // the sweeps of different bots are not in step
    b.trackReactAt = b.trackWatchUntil = 0.0f;
    b.trackThreat = (HOBJECT)LTNULL;
    b.trackLast = LTNULL;
    b.trackPrev.clear();
    b.amb.Reset();
    b.heard.clear();
    b.glanceSrc = LTNULL;
    b.glanceAt = 0.0f;
    b.glanceDist = 0.0f;
    b.lastTarget = LTNULL;
    b.hearOnsets = 0;
    b.cbh.Reset();
    b.cls = cls;
    ResetKit(b);
    if (g_cfg.mpWeapons)
        SetKit(b, cls);
    if (b.stage == 2)
        ai->SetChestburstedFrom((uint32)b.hostSet);  // the engine grows it into the host's adult
    // never let the engine put a bot to sleep, even with no player around
    g_pLTServer->SetObjectState(ai->m_hObject, OBJSTATE_ACTIVE);
    // health and armor of the matching multiplayer class (the single-player values, scaled by the
    // AI difficulty factor, make e.g. a Predator bot a 375 hit-point boss)
    if (cls && cls->mpSet)
    {
        int set = g_pCharacterButeMgr->GetSetFromModelType(cls->mpSet);
        if (set >= 0)
        {
            CDestructible* d = ai->GetDestructible();
            d->SetMaxHitPoints(g_pCharacterButeMgr->GetMaxHitPoints(set) * g_cfg.healthScale);
            d->SetHitPoints(g_pCharacterButeMgr->GetDefaultHitPoints(set) * g_cfg.healthScale);
            d->SetMaxArmorPoints(g_pCharacterButeMgr->GetMaxArmorPoints(set) * g_cfg.healthScale);
            d->SetArmorPoints(g_pCharacterButeMgr->GetDefaultArmorPoints(set) * g_cfg.healthScale);
            if (boosted)
            {
                d->SetMaxHitPoints(d->GetMaxHitPoints() * boostHp);
                d->SetHitPoints(d->GetHitPoints() * boostHp);
                d->SetMaxArmorPoints(d->GetMaxArmorPoints() * boostArmor);
                d->SetArmorPoints(d->GetArmorPoints() * boostArmor);
                Log("BOOST %s class=%s health x%.2f armor x%.2f", name, cls ? cls->name : "-", boostHp, boostArmor);
            }
        }
    }
    std::string weapons;
    for (size_t w = 0; w < loadout.size() && w < 3; ++w)
        weapons += (w ? "," : "") + loadout[w];
    const char* extName = ServerExt_BotName((int)b.species, b.slot); // a server extension may name this bot
    if (extName && extName[0])
        b.display = extName;
    else if (cls)
        b.display = std::string("[B] ") + cls->name + " " + (char)('0' + (b.slot % 10));
    else
        b.display = std::string("[B] ") + sp.tag + " " + (char)('0' + (b.slot % 10));
    ScoreRowAdd(b);
    Log("SPAWN %s species=%s class=%s char=%s hp=%.0f armor=%.0f weapons=%s at %.0f %.0f %.0f", name, sp.tag,
        cls ? cls->name : "-", character, ai->GetDestructible()->GetHitPoints(), ai->GetDestructible()->GetArmorPoints(),
        weapons.empty() ? "template" : weapons.c_str(), pos.x, pos.y, pos.z);
    if (b.skill >= 0.0f)
        Log("SKILL %s %.2f: accuracy %.2f, sight x%.2f", name, b.skill, b.skill, powf(2.0f, 2.0f * (b.skill - 0.5f)));
    return true;
}

// Multiplayer "team" of a character, as MultiplayerMgr::ClientsOnSameTeam sees players: its species.
int SpeciesOf(const CCharacter* c)
{
    const CharacterButes* cb = c ? c->GetButes() : 0;
    if (!cb)
        return -1;
    switch (cb->m_eCharacterClass)
    {
        case MARINE: case MARINE_EXOSUIT: return Marine;
        case PREDATOR: return Predator;
        case ALIEN: return Alien;
        case CORPORATE: case CORPORATE_EXOSUIT: case SYNTHETIC: return Corporate;
        default: return -1;
    }
}

int GameTypeNow()
{
    return (int)g_pGameServerShell->GetGameType().Get();
}

const char* GameTypeName(int t)
{
    switch (t)
    {
        case MP_DM: return "Deathmatch";
        case MP_TEAMDM: return "Team Deathmatch";
        case MP_HUNT: return "Hunt";
        case MP_SURVIVOR: return "Survivor";
        case MP_OVERRUN: return "Overrun";
        case MP_EVAC: return "Evac";
        default: return "single player";
    }
}

// Whether bots play in the current game type. Hunt, Survivor, Overrun and Evac change species and
// teams by their own rules, which bots do not follow yet.
bool SurvivorMode()
{
    return GameTypeNow() == MP_SURVIVOR;
}

bool HuntMode()
{
    return GameTypeNow() == MP_HUNT;
}

bool GameTypeAllowed()
{
    const int t = GameTypeNow();
    if (t == MP_SURVIVOR)
        return g_cfg.survivor && RaceSpecies(SurvivorRace(false)) >= 0 && RaceSpecies(SurvivorRace(true)) >= 0;
    if (t == MP_HUNT)
        return g_cfg.hunt && RaceSpecies(HuntRace(0)) >= 0 && RaceSpecies(HuntRace(1)) >= 0;
    return (t == MP_DM && g_cfg.dm) || (t == MP_TEAMDM && g_cfg.teamdm);
}

void UpdateRelations()
{
    // The teams players have (MultiplayerMgr::ClientsOnSameTeam): in Team Deathmatch a team is a species;
    // Deathmatch is every man for himself (friendly fire is always on there, MultiplayerMgr::AllowFriendlyFire).
    // Survivor (MultiplayerMgr::Update_Survivor): in TAG everyone hurts everyone, in CUT the races fight,
    // otherwise (no game yet, interlude, round end) nobody fights; bots also get the players' god mode then.
    const bool teams = GameTypeNow() != MP_DM;
    const bool surv = SurvivorMode();
    const int sub = surv && g_pGameServerShell->GetMultiplayerMgr() ? g_pGameServerShell->GetMultiplayerMgr()->GetServerSubState() : -1;
    const bool survPeace = surv && sub != MPMGR_SUBSTATE_SURVIVOR_TAG && sub != MPMGR_SUBSTATE_SURVIVOR_CUT;
    for (size_t i = 0; i < g_bots.size(); ++i)
    {
        CAI* ai = g_bots[i].alive ? BotAI(g_bots[i]) : LTNULL;
        if (!ai)
            continue;
        if (surv)
            ai->GetDestructible()->SetCanDamage(!(survPeace && sub != MPMGR_SUBSTATE_NONE));
        const int mine = SpeciesOf(ai);
        for (CCharacterMgr::CharacterIterator it = g_pCharacterMgr->BeginCharacters();
             it != g_pCharacterMgr->EndCharacters(); ++it)
        {
            CCharacter* other = *it;
            if (!other || other == ai)
                continue;
            bool teammates = teams && mine >= 0 && mine == SpeciesOf(other);
            if (surv)
                teammates = survPeace || (sub == MPMGR_SUBSTATE_SURVIVOR_CUT && mine >= 0 && mine == SpeciesOf(other));
            if (g_bots[i].stage == 1 && other->GetButes() && !other->GetButes()->m_bCanBeFacehugged)
                teammates = true;  // a facehugger cannot hurt aliens, synthetics or exosuits: no point chasing them
            else if (!teammates && HugAvoid(g_bots[i], ai, other))
            {
                teammates = true;  // a person does not charge a group that watches him (BotMgr_HugNow, HugAvoid)
                if (g_cfg.debug && g_pCharacterMgr->GetRelationship(*ai, *other) != CCharacterMgr::Friend)
                    Log("HUGAVOID %s -> %s", g_bots[i].name.c_str(), ObjectLabel(other->m_hObject).c_str());
            }
            const CCharacterMgr::Relationship want = teammates ? CCharacterMgr::Friend : CCharacterMgr::Foe;
            if (g_pCharacterMgr->GetRelationship(*ai, *other) != want)
                g_pCharacterMgr->SetRelationship(*ai, *other, want);
        }
    }
}

std::string ObjectLabel(HOBJECT h)
{
    if (!h)
        return "world";
    char buf[128] = "";
    g_pLTServer->GetObjectName(h, buf, sizeof(buf));
    if (buf[0])
        return buf;
    return "unnamed";
}

void LogStatus(Bot& b, LTFLOAT now)
{
    CAI* ai = BotAI(b);
    if (!ai)
        return;
    LTVector pos;
    g_pLTServer->GetObjectPos(ai->m_hObject, &pos);
    std::string goal = ai->GetCurrentSlotName();
    std::string target = "-";
    if (ai->HasTarget())
        target = ObjectLabel(ai->GetTarget().GetObject().GetHOBJECT());
    LTVector moved = pos - b.checkPos;
    const AIWBM_AIWeapon* wb = ai->GetCurrentWeaponButesPtr();
    if (g_cfg.debug)
    {
        // damage taken since the last status, with the last damager and its weapon (balance analysis)
        const LTFLOAT hpNow = ai->GetHitPoints() + ai->GetDestructible()->GetArmorPoints();
        if (b.lastHp > 0.0f && hpNow < b.lastHp - 0.5f)
        {
            HOBJECT hd = ai->GetDestructible()->GetLastDamager();
            CAI* dai = hd ? dynamic_cast<CAI*>(g_pLTServer->HandleToObject(hd)) : LTNULL;
            const char* dw = dai && dai->GetCurrentWeaponButesPtr() ? dai->GetCurrentWeaponButesPtr()->Name.c_str() : "-";
            Log("HIT %s -%.0f by %s weapon=%s", b.name.c_str(), b.lastHp - hpNow, ObjectLabel(hd).c_str(), dw);
        }
        b.lastHp = hpNow;
    }
    char energy[32] = "";
    if (b.species == BOT_PREDATOR)
    {
        if (b.siftUntil > 0.0f)
            sprintf(energy, " energy=sift");
        else if (b.mediUntil > 0.0f)
            sprintf(energy, " energy=medicomp");
        else
            sprintf(energy, " energy=%d", (int)b.energy);
    }
    Log("STATUS %s hp=%.0f pos=%.0f %.0f %.0f goal=%s target=%s weapon=%s cloak=%d%s",
        b.name.c_str(), ai->GetHitPoints(), pos.x, pos.y, pos.z, goal.empty() ? "-" : goal.c_str(), target.c_str(),
        wb ? wb->Name.c_str() : "-", (int)ai->GetCloakState(), energy);
    // a fire action that outlives its target for two reports (a bot frozen in place; one report happens when
    // the target has just died): should never happen, counted as broken by farm.py
    // (two kills in a row also give two such reports, but the bot moves in between)
    if (!ai->HasTarget() && dynamic_cast<const AIActionFireWeapon*>(ai->GetCurrentAction()) &&
        (b.frozen == 0 || (pos - b.frozenPos).MagSqr() < 32.0f * 32.0f))
    {
        if (b.frozen == 0)
            b.frozenPos = pos;
        if (++b.frozen >= 2)
            Log("FROZEN %s in a fire action without a target for %d reports", b.name.c_str(), b.frozen);
    }
    else
        b.frozen = 0;

    if (g_cfg.debug)
    {
        // movement diagnostics: current action, move strategy state, character movement state and speed
        const AIAction* act = ai->GetCurrentAction();
        const CAIStrategyMove* mv = ai->GetMovePtr();
        CharacterMovement* cm = ai->GetMovement();
        const LTVector& vel = cm->GetVelocity();
        // injured: the AI's forced crawl after losing a leg (hook 'botlimbs' keeps it at 0 for bots)
        Log("  MOVE %s action=%s canAct=%d move=%s cms=%s ctrl=%08lX speed=%.0f injured=%d",
            b.name.c_str(), act ? typeid(*act).name() : "none", (int)ai->CanDoNewAction(),
            !mv ? "none" : mv->IsSet() ? "set" : mv->IsDone() ? "done" : mv->IsStuck() ? "stuck" : "other",
            cm->GetMovementStateName(cm->GetMovementState()), (unsigned long)cm->GetControlFlags(), vel.Mag(),
            (int)ai->GetInjuredMovement());
        if (ai->HasTarget())
        {
            // what the attack code sees of the target: sense (partial/full), range, burst delay
            const CAITarget& t = ai->GetTarget();
            LTVector tp;
            g_pLTServer->GetObjectPos(t.GetObject(), &tp);
            Log("  TARGET %s partial=%d full=%d maintained=%d range=%.0f burstok=%d",
                b.name.c_str(), (int)t.IsPartiallyVisible(), (int)t.IsFullyVisible(), (int)t.IsMaintained(),
                (tp - pos).Mag(), (int)ai->BurstDelayExpired());
        }

        // animation diagnostics: movement is gated by the playing tracker set's MovementAllowed flag
        CharacterAnimation* an = ai->GetAnimation();
        if (an)
        {
            TrackerSet* ts = g_pAnimationButeMgr->GetTrackerSet((uint16)an->GetTrackerSet());
            const char* actionRef = an->GetLayerReference(ANIMSTYLELAYER_ACTION);
            // recoil: the AI's hit-reaction animation (players' MP tracker sets have none)
            Log("  ANIM %s set=%s trans=%d moveOK=%d moveOKnext=%d action=%s recoil=%d",
                b.name.c_str(), ts ? ts->szName : "?", (int)an->UsingTransAnim(), (int)an->MovementAllowed(LTTRUE),
                (int)an->MovementAllowed(LTFALSE), actionRef ? actionRef : "-", (int)ai->IsRecoiling());
        }

        // combat movement since the last report (seconds with a target: moving faster than 50 u/s, in a fire
        // action, both, moved by BotMgr_CombatMove, waiting for the animation to allow movement), legs, refusals
        const CombatMove& c = b.cm;
        Log("  CMOVE %s att=%.2f move=%.2f fire=%.2f firemove=%.2f drive=%.2f wait=%.2f legs=%d done=%d noprog=%d "
            "timeout=%d noleg=%d rej=wall%d,floor%d,graph%d,clip%d,los%d zz=%d flips=%d zzfail=%d",
            b.name.c_str(), c.tAtt, c.tMove, c.tFire, c.tFireMove, c.tDrive, c.tWait, c.legs, c.done, c.abNoProg,
            c.abTime, c.noLeg, c.rej[LEG_WALL], c.rej[LEG_FLOOR], c.rej[LEG_GRAPH], c.rej[LEG_CLIP], c.rej[LEG_LOS],
            c.zzSets, c.zzFlips, c.zzFail);
    }
    b.cm.ResetStats();

    // off the graph: a bot that fell somewhere the waypoints do not cover (off a wall, into a corner the
    // generator left out) gets paths that start far away and cannot be followed
    {
        const LTVector& dims = ai->GetDims();
        const Vec3 feet(pos.x, pos.y - dims.y, pos.z);
        const int n = g_nav.Nearest(feet);
        const float d = n >= 0 ? avp2bots::Dist(feet, g_nav.Node(n).pos) : 1e9f;
        if (d > 256.0f && !ai->IsDead())
        {
            b.offGraph++;
            Log("OFFGRAPH %s pos=%.0f %.0f %.0f nearest=%d at %.0f count=%d", b.name.c_str(), pos.x, pos.y, pos.z, n, d,
                b.offGraph);
        }
        else
            b.offGraph = 0;
    }

    // stuck: roaming but moved less than 48 units over the last 6 seconds
    if (now - b.checkTime >= 6.0f)
    {
        bool stuck = false;
        const bool onLadder = dynamic_cast<const AIActionLadderTo*>(ai->GetCurrentAction()) != 0;
        if (moved.Mag() < 48.0f)
        {
            // a bot hanging on a ladder (the action cannot be interrupted) is stuck whatever its goal; an alien
            // feeding on a body stands still on purpose
            // (and so does one waiting in a wall ambush)
            // (and so does a chestburster hiding or biting back until it grows)
            if ((!_stricmp(goal.c_str(), "BotRoam") && !b.feeding && b.amb.state != AMB_WAITING &&
                 b.cbh.state != CBH_HIDDEN && b.cbh.state != CBH_FIGHT) || onLadder)
                stuck = true;
            else if (IsAlien(ai) && ai->HasTarget())
            {
                // a melee alien that has not moved while its target is out of reach
                HOBJECT ht = ai->GetTarget().GetObject().GetHOBJECT();
                if (ht)
                {
                    LTVector tpos;
                    g_pLTServer->GetObjectPos(ht, &tpos);
                    stuck = (tpos - pos).Mag() > 250.0f;
                }
            }
        }
        if (stuck)
        {
            b.stuckCount++;
            Log("STUCK %s pos=%.0f %.0f %.0f goal=%s%s count=%d", b.name.c_str(), pos.x, pos.y, pos.z, goal.c_str(),
                onLadder ? " ladder" : "", b.stuckCount);
            // stuck on the way to an ammo box: no box runs to that box for two minutes
            if (HOBJECT hb = b.goalBox.GetHOBJECT())
            {
                for (size_t i = 0; i < g_boxes.size(); ++i)
                    if (g_boxes[i].hBox.GetHOBJECT() == hb)
                        g_boxes[i].bannedUntil = now + 120.0f;
                LTVector q;
                g_pLTServer->GetObjectPos(hb, &q);
                Log("AMMOBAN box at %.0f %.0f %.0f for 120 s (%s stuck on its way)", q.x, q.y, q.z, b.name.c_str());
                b.goalBox = (HOBJECT)LTNULL;
            }
        }
        else
            b.stuckCount = 0;
        b.checkPos = pos;
        b.checkTime = now;
    }
}


// Solid model objects (crates, machines, props...) are not part of the map file the waypoint graph was
// built from, so the AI would run into them. Block the waypoints and links they cover, and record them
// in botnav\<map>.obstacles so the generator can use them next time.
void ScanObstacles()
{
    const float margin = 16.0f;  // character radius
    int objects = 0, nodes = 0;
    FILE* f = 0;
    if (g_cfg.log)
    {
        std::string path = "botnav\\" + g_map + ".obstacles";
        f = fopen(path.c_str(), "w");
    }
    for (int pass = 0; pass < 2; ++pass)
    {
        HOBJECT h = LTNULL;
        while ((h = pass == 0 ? g_pLTServer->GetNextObject(h) : g_pLTServer->GetNextInactiveObject(h)) != LTNULL)
        {
            if (g_pLTServer->GetObjectType(h) != OT_MODEL)
                continue;
            if (!(g_pLTServer->GetObjectFlags(h) & FLAG_SOLID))
                continue;
            if (dynamic_cast<CCharacter*>(g_pLTServer->HandleToObject(h)))
                continue;
            LTVector pos, dims;
            g_pLTServer->GetObjectPos(h, &pos);
            g_pLTServer->GetObjectDims(h, &dims);
            if (dims.x < 4.0f || dims.z < 4.0f || dims.x > 600.0f || dims.z > 600.0f)
                continue;
            Vec3 mn(pos.x - dims.x - margin, pos.y - dims.y - 64.0f, pos.z - dims.z - margin);
            Vec3 mx(pos.x + dims.x + margin, pos.y + dims.y, pos.z + dims.z + margin);
            nodes += g_nav.BlockBox(mn, mx);
            ++objects;
            if (f)
            {
                char name[64] = "";
                g_pLTServer->GetObjectName(h, name, sizeof(name));
                fprintf(f, "o %.1f %.1f %.1f %.1f %.1f %.1f %s\n", pos.x, pos.y, pos.z, dims.x, dims.y, dims.z, name);
            }
        }
    }
    if (f)
        fclose(f);
    Log("OBSTACLES %d solid models, %d waypoints blocked", objects, nodes);
}

void FindDoors()
{
    g_doors.clear();
    g_pickups.clear();
    for (int pass = 0; pass < 2; ++pass)
    {
        HOBJECT h = LTNULL;
        while ((h = pass == 0 ? g_pLTServer->GetNextObject(h) : g_pLTServer->GetNextInactiveObject(h)) != LTNULL)
        {
            BaseClass* obj = g_pLTServer->HandleToObject(h);
            if (!dynamic_cast<Door*>(obj) || dynamic_cast<Switch*>(obj) || dynamic_cast<Breakable*>(obj) ||
                dynamic_cast<HackableWorldmodel*>(obj) || dynamic_cast<WorldModelDebris*>(obj) ||
                dynamic_cast<AlienTearDoor*>(obj))
                continue;
            // only the doors the waypoint graph goes through (not vehicles, effects or blockers that are
            // Door objects too: pressing Use on those would change the map for everyone)
            char dname[128] = "";
            g_pLTServer->GetObjectName(h, dname, sizeof(dname));
            if (!g_nav.IsUseDoor(dname))
                continue;
            DoorInfo d;
            d.hDoor = LTSmartLink(h);
            d.nextTry = 0.0f;
            d.checkAt = 0.0f;
            g_doors.push_back(d);
        }
    }
    Log("DOORS %d", (int)g_doors.size());
}

void UpdateDoors(LTFLOAT now)
{
    for (size_t k = 0; k < g_doors.size(); ++k)
    {
        DoorInfo& d = g_doors[k];
        HOBJECT hd = d.hDoor.GetHOBJECT();
        Door* door = hd ? dynamic_cast<Door*>(g_pLTServer->HandleToObject(hd)) : LTNULL;
        if (!door)
            continue;
        if (d.checkAt > 0.0f && now >= d.checkAt)
        {
            // still closed: not a door players open with Use (or locked); try again much later
            if (door->GetState() == DOORSTATE_CLOSED)
                d.nextTry = now + 30.0f;
            d.checkAt = 0.0f;
        }
        if (now < d.nextTry || door->GetState() != DOORSTATE_CLOSED)
            continue;
        LTVector dpos, ddims;
        g_pLTServer->GetObjectPos(hd, &dpos);
        g_pLTServer->GetObjectDims(hd, &ddims);
        for (size_t i = 0; i < g_bots.size(); ++i)
        {
            CAI* ai = g_bots[i].alive ? BotAI(g_bots[i]) : LTNULL;
            if (!ai)
                continue;
            const LTVector& p = ai->GetPosition();
            // distance from the bot to the door's box
            float dx = fabsf(p.x - dpos.x) - ddims.x, dy = fabsf(p.y - dpos.y) - ddims.y, dz = fabsf(p.z - dpos.z) - ddims.z;
            dx = dx > 0 ? dx : 0;
            dy = dy > 0 ? dy : 0;
            dz = dz > 0 ? dz : 0;
            if (dx * dx + dy * dy + dz * dz < 64.0f * 64.0f)
            {
                SendTriggerMsgToObject(ai, hd, "ACTIVATE");
                d.nextTry = now + 2.0f;
                d.checkAt = now + 0.5f;
                if (g_cfg.debug)
                    Log("DOOR %s activates %s", g_bots[i].name.c_str(), ObjectLabel(hd).c_str());
                break;
            }
        }
    }
}

// --- bot list management (used at world start and by the admin commands) ---------------------------

int NextSlot(BotSpecies sp)
{
    int slot = 1;
    for (bool used = true; used; ++slot)
    {
        used = false;
        for (size_t i = 0; i < g_bots.size(); ++i)
            if (g_bots[i].species == sp && g_bots[i].slot == slot)
                used = true;
        if (!used)
            return slot;
    }
    return slot;
}

void AddBot(BotSpecies sp, const BotClass* forced, LTFLOAT when)
{
    Bot b;
    b.species = sp;
    b.slot = NextSlot(sp);
    b.alive = false;
    b.goalsSent = false;
    b.respawnTime = when;
    b.spawnTime = 0.0f;
    b.deaths = 0;
    b.scoreSet = -1;
    b.survTeam = 0;
    b.survScore = 0;
    b.pendingRole = -1;
    b.switchAt = 0.0f;
    b.stage = 0;
    b.hostSet = -1;
    b.lifeClass = 0;
    b.placeAt = false;
    b.hugSince = 0.0f;
    b.loseSince = 0.0f;
    b.hugHost = -1;
    b.hugAt = 0.0f;
    b.growInto = -1;
    b.energy = 0.0f;
    b.energyTime = 0.0f;
    b.siftUntil = 0.0f;
    b.siftStart = 0.0f;
    b.siftKeys = 0;
    b.mediStart = 0.0f;
    b.mediUntil = 0.0f;
    b.mediKeyDone = false;
    b.walkOnly = false;
    b.lastShot = 0.0f;
    b.cloakWas = 0;
    b.spears = 0;
    b.usesSpears = false;
    b.usesGrenades = false;
    b.grenades = 0;
    b.altNext = 0.0f;
    b.altBackAt = 0.0f;
    b.altLastShot = -100.0f;
    b.usesSlugs = b.usesTrackers = false;
    b.slugs = b.trackers = 0;
    b.slugLastShot = b.trackerLastShot = -100.0f;
    b.lastSadarTracker = false;
    b.sadarSince = 0.0f;
    b.sadarLocked = false;
    b.ammoRunCheck = 0.0f;
    b.ammoRun = false;
    b.discOut = false;
    b.discTime = 0.0f;
    b.nextDiscCheck = 0.0f;
    b.vision = 0;
    b.visionPressAt = 0.0f;
    b.visionNeedSince = 0.0f;
    b.frozen = 0;
    b.animWaitSince = 0.0f;
    b.rowId = 0;
    b.rowFrags = b.rowScore = b.rowConFrags = 0;
    b.offGraph = 0;
    b.checkTime = 0.0f;
    b.stuckCount = 0;
    b.forcedClass = forced;
    b.skill = -1.0f;
    b.burnLogAt = 0.0f;
    b.recoilLogAt = 0.0f;
    b.recoilFires = 0;
    b.cls = 0;
    b.trackScanAt = b.trackReactAt = b.trackWatchUntil = 0.0f;
    b.trackLast = LTNULL;
    b.lastTarget = LTNULL;
    b.hearOnsets = 0;
    ResetKit(b);
    g_bots.push_back(b);
}

void RemoveBotAt(size_t i)
{
    if (i >= g_bots.size())
        return;
    HOBJECT h = g_bots[i].hObj.GetHOBJECT();
    if (h && g_bots[i].alive)
        g_pLTServer->RemoveObject(h);
    Log("REMOVE %s", g_bots[i].name.empty() ? "(not spawned yet)" : g_bots[i].name.c_str());
    ScoreRowRemove(g_bots[i]);
    g_bots.erase(g_bots.begin() + i);
}

void AddHuntBot(LTFLOAT when)
{
    const int role = HuntJoinRole();
    const int sp = RaceSpecies(HuntRace(role));
    if (sp < 0)
        return;
    AddBot((BotSpecies)sp, 0, when);
    g_bots.back().survTeam = role;
}

int CountSpecies(BotSpecies sp)
{
    int n = 0;
    for (size_t i = 0; i < g_bots.size(); ++i)
        if (g_bots[i].species == sp)
            ++n;
    return n;
}

int ParseSpecies(const char* word)
{
    if (!word || !*word) return -1;
    for (int s = 0; s < BOT_SPECIES_COUNT; ++s)
        if (!_strnicmp(word, kSpecies[s].tag, strlen(word)))
            return s;
    return -1;
}

const BotClass* ParseClass(int sp, const char* word)
{
    if (!word || !*word) return 0;
    for (int i = 0; i < kClassCount; ++i)
        if ((sp < 0 || kClasses[i].species == sp) && !_strnicmp(word, kClasses[i].name, strlen(word)))
            return &kClasses[i];
    return 0;
}

} // namespace

// ----------------------------------------------------------------------------------------------

// ----------------------------------------------------------------------------------------------
// Scores. Bots have no row on the scoreboard (they are not clients), but kills involving them follow the
// multiplayer rules (MultiplayerMgr::ClientKilledMsg_DM / _TeamDM) on the rows and team totals that exist:
// a player killing a bot gets the frag and the score of the bot's class; a player killed by a bot is not
// a suicide; in Team Deathmatch the bots' kills count for their species' team. Only data the clients
// already know is changed, so this needs nothing new on the client side. MultiplayerMgr declares this
// struct a friend (hook 'friend').

struct BotScoreAccess
{
    static MultiplayerMgr* Mgr() { return g_pGameServerShell ? g_pGameServerShell->GetMultiplayerMgr() : LTNULL; }

    // score value of a character set (the [Scoring] table of the server config)
    static int ValueOfSet(MultiplayerMgr* mp, int set)
    {
        if (set < 0)
            return 0;
        const int scoring = g_pCharacterButeMgr->GetScoringClass(set);
        for (int i = 0; i < MAX_MP_SCORING_CLASSES; i++)
            if (mp->m_GameInfo.m_nScoreClasses[i] == scoring)
                return mp->m_GameInfo.m_nScoreValues[i];
        return 0;
    }

    static void TeamAdd(MultiplayerMgr* mp, int team, int frags, int score)
    {
        if (team < 0 || team >= MAX_TEAMS || mp->m_GameInfo.m_nGameType.Get() != MP_TEAMDM)
            return;
        mp->m_TeamData[team].m_nFrags += frags;
        mp->m_TeamData[team].m_nScore += score;
        mp->m_TeamData[team].m_nUpdateFlags |= MP_CLIENT_UPDATE_FRAGS | MP_CLIENT_UPDATE_SCORE;
    }

    // A player was killed by a bot (called instead of the game's "killed himself" handling).
    // --- Survivor (ClientKilledMsg_Survivor) -------------------------------------------------------
    static int SurvRace(bool mutant)
    {
        MultiplayerMgr* mp = Mgr();
        return mp ? (mutant ? mp->m_GameInfo.m_nSurv_MutateRace : mp->m_GameInfo.m_nSurv_SurvivorRace) : -1;
    }

    static void MutateMessage(uint32 id)
    {
        if (!id)
            return;
        HMESSAGEWRITE hMessage = g_pLTServer->StartMessage(LTNULL, MID_MPMGR_SERVER_MESSAGE);
        hMessage->WriteByte(MP_SURVIVOR_MUTATE);
        hMessage->WriteDWord(id);
        g_pLTServer->EndMessage2(hMessage, MESSAGE_NAGGLE);
    }

    static void BotScoreAdd(Bot& b, int pts)
    {
        b.survScore += pts;
        if (b.rowId)
        {
            b.rowScore = b.survScore;
            RowUpdate(b);
        }
    }

    // a human killed by a bot in Survivor
    static void SurvPlayerKilled(MultiplayerMgr* mp, int v, CAI* killer)
    {
        MPMgrData& victim = mp->m_ClientData[v];
        Bot* kb = BotOf(killer->m_hObject);
        const int sub = mp->m_nServerSubState;
        const int vRace = victim.m_Data.m_nCharacterClass;
        const int kRace = SpeciesOf(killer);
        const int survRace = mp->m_GameInfo.m_nSurv_SurvivorRace, mutRace = mp->m_GameInfo.m_nSurv_MutateRace;
        bool mutate = false;
        if (kRace == vRace)
        {
            if (sub == MPMGR_SUBSTATE_SURVIVOR_TAG)
                mutate = true;
            else if (kb)
                BotScoreAdd(*kb, -10);   // MPMGR_SURVIVOR_FKILL_PTS
        }
        else if (sub == MPMGR_SUBSTATE_SURVIVOR_CUT && kRace == mutRace && vRace == survRace)
        {
            mutate = true;
            if (kb)
                BotScoreAdd(*kb, 10);    // MPMGR_SURVIVOR_MKILL_PTS
        }
        if (mutate && vRace == survRace && victim.m_nNextClass >= MAX_TEAMS)  // no mutation pending yet
        {
            MutateMessage(victim.m_nClientID);
            victim.m_nNextClass = (uint8)mutRace;
            victim.m_fMiscTimer = g_pLTServer->GetTime();
        }
        if (kb && kb->rowId)
            KillMessage(kRace == vRace && !mutate ? MP_SERV_MSG_KILLED_TEAMMATE : MP_SERV_MSG_KILLED_SOMEONE,
                        kb->rowId, victim.m_nClientID);
        Log("SURVIVOR player %u killed by %s (sub %d%s)", (unsigned)victim.m_nClientID,
            ObjectLabel(killer->m_hObject).c_str(), sub, mutate ? ", mutates" : "");
    }

    // a bot died in Survivor; returns true when it mutates
    static bool SurvBotKilled(MultiplayerMgr* mp, Bot& b, HOBJECT hKiller)
    {
        const int sub = mp->m_nServerSubState;
        const int survRace = mp->m_GameInfo.m_nSurv_SurvivorRace, mutRace = mp->m_GameInfo.m_nSurv_MutateRace;
        const int vRace = b.survTeam ? mutRace : survRace;
        CCharacter* other = hKiller ? dynamic_cast<CCharacter*>(g_pLTServer->HandleToObject(hKiller)) : LTNULL;
        const bool self = !other || hKiller == b.hObj.GetHOBJECT();
        bool mutate = false;
        if (self)
        {
            mutate = vRace == survRace;
            KillMessage(MP_SERV_MSG_KILLED_HIMSELF, b.rowId, 0);
        }
        else
        {
            CPlayerObj* pk = dynamic_cast<CPlayerObj*>(other);
            const int k = pk ? mp->FindClient(pk->GetClient()) : -1;
            Bot* kb = BotOf(hKiller);
            const int kRace = SpeciesOf(other);
            int pts = 0;
            bool streak = false;
            if (kRace == vRace)
            {
                if (sub == MPMGR_SUBSTATE_SURVIVOR_TAG)
                    mutate = streak = true;
                else
                    pts = -10;
            }
            else if (sub == MPMGR_SUBSTATE_SURVIVOR_CUT)
            {
                streak = true;
                if (kRace == mutRace && vRace == survRace)
                {
                    mutate = true;
                    pts = 10;
                }
            }
            if (k >= 0 && g_cfg.score)
            {
                MPMgrData& killer = mp->m_ClientData[k];
                killer.m_nScore += pts;
                if (streak)
                    mp->IncrementConsecutiveFrags(k);
                else if (pts < 0)
                    killer.m_nConsecutiveFrags = 0;
                killer.m_nUpdateFlags |= MP_CLIENT_UPDATE_SCORE | MP_CLIENT_UPDATE_CONFRAGS;
            }
            else if (kb && pts)
                BotScoreAdd(*kb, pts);
            const uint32 kid = k >= 0 ? mp->m_ClientData[k].m_nClientID : (kb ? kb->rowId : 0);
            if (kid && b.rowId && (pts || streak))
                KillMessage(pts < 0 ? MP_SERV_MSG_KILLED_TEAMMATE : MP_SERV_MSG_KILLED_SOMEONE, kid, b.rowId);
        }
        if (mutate)
            MutateMessage(b.rowId);
        mp->SetUpdateGameServ();
        return mutate;
    }

    // --- Hunt (ClientKilledMsg_Hunt, Update_Hunt) ----------------------------------------------------
    static int HuntRaceOf(int role)
    {
        MultiplayerMgr* mp = Mgr();
        return mp ? (role ? mp->m_GameInfo.m_nHunt_PreyRace : mp->m_GameInfo.m_nHunt_HunterRace) : -1;
    }

    static bool LifecycleOption()
    {
        MultiplayerMgr* mp = Mgr();
        return mp && mp->m_GameInfo.m_bLifecycle;
    }

    static int HuntRatio()
    {
        MultiplayerMgr* mp = Mgr();
        return mp ? (int)mp->m_GameInfo.m_nHunt_Ratio : 1;
    }

    static void BotFragAdd(Bot& b, int frags)
    {
        b.rowFrags += frags;
        b.rowConFrags = frags > 0 ? b.rowConFrags + 1 : 0;
        RowUpdate(b);
    }

    // a live bot changes role: removed and respawned as the other race after MPMGR_MUTATE_TIME, as
    // MPRespawn does to a live player
    static void BotSwitch(Bot& b, int role)
    {
        b.pendingRole = role;
        b.switchAt = g_pLTServer->GetTime() + 2.0f;
    }

    static void HuntPlayerKilled(MultiplayerMgr* mp, int v, CAI* killer)
    {
        MPMgrData& victim = mp->m_ClientData[v];
        Bot* kb = BotOf(killer->m_hObject);
        const int kRace = SpeciesOf(killer), vRace = victim.m_Data.m_nCharacterClass;
        const int hunters = mp->m_GameInfo.m_nHunt_HunterRace, prey = mp->m_GameInfo.m_nHunt_PreyRace;
        if (kRace == vRace)
        {
            if (kb)
                BotFragAdd(*kb, -1);
            if (kb && kb->rowId)
                KillMessage(MP_SERV_MSG_KILLED_TEAMMATE, kb->rowId, victim.m_nClientID);
            return;
        }
        if (kRace == prey && vRace == hunters)
        {
            // the prey that kills a hunter takes his place
            if (victim.m_nNextClass >= MAX_TEAMS)
            {
                victim.m_nNextClass = (uint8)prey;
                victim.m_fMiscTimer = g_pLTServer->GetTime();
            }
            if (kb)
                BotSwitch(*kb, 0);
            Log("HUNT %s (prey) killed player %u (hunter): they swap", kb ? kb->name.c_str() : "?",
                (unsigned)victim.m_nClientID);
        }
        else if (kb)
            BotFragAdd(*kb, 1);
        if (kb && kb->rowId)
            KillMessage(MP_SERV_MSG_KILLED_SOMEONE, kb->rowId, victim.m_nClientID);
    }

    // a bot died in Hunt; returns the role it respawns with
    static int HuntBotKilled(MultiplayerMgr* mp, Bot& b, HOBJECT hKiller)
    {
        const int hunters = mp->m_GameInfo.m_nHunt_HunterRace, prey = mp->m_GameInfo.m_nHunt_PreyRace;
        const int vRace = b.survTeam ? prey : hunters;
        CCharacter* other = hKiller ? dynamic_cast<CCharacter*>(g_pLTServer->HandleToObject(hKiller)) : LTNULL;
        if (!other || hKiller == b.hObj.GetHOBJECT())
        {
            BotFragAdd(b, -1);
            KillMessage(MP_SERV_MSG_KILLED_HIMSELF, b.rowId, 0);
            return b.survTeam;
        }
        CPlayerObj* pk = dynamic_cast<CPlayerObj*>(other);
        const int k = pk ? mp->FindClient(pk->GetClient()) : -1;
        Bot* kb = BotOf(hKiller);
        const int kRace = SpeciesOf(other);
        int role = b.survTeam;
        int frags = 0;
        if (kRace == vRace)
            frags = -1;
        else if (kRace == prey && vRace == hunters)
        {
            role = 1;   // the killed hunter comes back as prey; the killer becomes a hunter
            if (k >= 0 && mp->m_ClientData[k].m_nNextClass >= MAX_TEAMS)
            {
                mp->m_ClientData[k].m_nNextClass = (uint8)hunters;
                mp->m_ClientData[k].m_fMiscTimer = g_pLTServer->GetTime();
            }
            else if (kb)
                BotSwitch(*kb, 0);
        }
        else
            frags = 1;
        if (k >= 0 && frags && g_cfg.score)
        {
            MPMgrData& killer = mp->m_ClientData[k];
            killer.m_nFrags += frags;
            if (frags > 0)
                mp->IncrementConsecutiveFrags(k);
            else
                killer.m_nConsecutiveFrags = 0;
            killer.m_nUpdateFlags |= MP_CLIENT_UPDATE_FRAGS | MP_CLIENT_UPDATE_CONFRAGS;
        }
        else if (kb && frags)
            BotFragAdd(*kb, frags);
        const uint32 kid = k >= 0 ? mp->m_ClientData[k].m_nClientID : (kb ? kb->rowId : 0);
        if (kid && b.rowId)
            KillMessage(frags < 0 ? MP_SERV_MSG_KILLED_TEAMMATE : MP_SERV_MSG_KILLED_SOMEONE, kid, b.rowId);
        mp->SetUpdateGameServ();
        return role;
    }

    static void PlayerKilled(HCLIENT hVictim, CAI* killer)
    {
        MultiplayerMgr* mp = Mgr();
        const int v = mp ? mp->FindClient(hVictim) : -1;
        if (v < 0)
            return;
        MPMgrData& victim = mp->m_ClientData[v];
        victim.m_nConsecutiveFrags = 0;
        victim.m_bSpawnAsQueen = LTFALSE;
        victim.m_nUpdateFlags |= MP_CLIENT_UPDATE_CONFRAGS;
        if (mp->m_GameInfo.m_nGameType.Get() == MP_SURVIVOR)
        {
            SurvPlayerKilled(mp, v, killer);
            return;
        }
        if (mp->m_GameInfo.m_nGameType.Get() == MP_HUNT)
        {
            HuntPlayerKilled(mp, v, killer);
            return;
        }
        const int value = mp->GetClientScore(victim.m_nClientID);
        const int team = SpeciesOf(killer);
        const bool teamKill = team == victim.m_Data.m_nCharacterClass && mp->m_GameInfo.m_nGameType.IsTeamMode();
        if (g_cfg.score)
        {
            if (team == victim.m_Data.m_nCharacterClass)
                TeamAdd(mp, team, -1, -value);  // team kill (friendly fire on)
            else
                TeamAdd(mp, team, 1, value);
        }
        Bot* kb = BotOf(killer->m_hObject);
        if (kb && kb->rowId)
        {
            RowKill(*kb, teamKill, value);
            KillMessage(teamKill ? MP_SERV_MSG_KILLED_TEAMMATE : MP_SERV_MSG_KILLED_SOMEONE, kb->rowId, victim.m_nClientID);
        }
        mp->SetUpdateGameServ();
        Log("SCORE player %u killed by %s (team %d, value %d)", (unsigned)victim.m_nClientID,
            ObjectLabel(killer->m_hObject).c_str(), team, value);
    }

    // A bot died; hKiller is its last damager.
    static void BotKilled(Bot& b, HOBJECT hKiller)
    {
        MultiplayerMgr* mp = Mgr();
        if (!mp)
            return;
        if (mp->m_GameInfo.m_nGameType.Get() == MP_HUNT)
        {
            const int role = HuntBotKilled(mp, b, hKiller);
            if (role != b.survTeam)
            {
                b.survTeam = role;
                const int sp = RaceSpecies(HuntRaceOf(role));
                if (sp >= 0)
                    b.species = (BotSpecies)sp;
                b.forcedClass = 0;
                b.respawnTime = g_pLTServer->GetTime() + 2.0f;
                ScoreRowRemove(b);
                Log("HUNT %s comes back as %s", b.name.c_str(), role ? "prey" : "hunter");
            }
            return;
        }
        if (mp->m_GameInfo.m_nGameType.Get() == MP_SURVIVOR)
        {
            if (SurvBotKilled(mp, b, hKiller))
            {
                // the mutation: a mutant from now on (the round counts it at once), back after
                // MPMGR_MUTATE_TIME as the mutate race at a mutant start point
                const int sp = RaceSpecies(mp->m_GameInfo.m_nSurv_MutateRace);
                b.survTeam = 1;
                if (sp >= 0)
                    b.species = (BotSpecies)sp;
                b.forcedClass = 0;
                b.respawnTime = g_pLTServer->GetTime() + 2.0f;
                ScoreRowRemove(b);   // its row comes back with the new class at the respawn
                Log("SURVIVOR %s mutates", b.name.c_str());
            }
            return;
        }
        const bool teams = mp->m_GameInfo.m_nGameType.IsTeamMode() != LTFALSE;
        if (b.rowId)
        {
            // the dead bot's own row: kill streak over; a suicide costs a frag (ClientKilledMsg_DM)
            b.rowConFrags = 0;
            CCharacter* other = hKiller ? dynamic_cast<CCharacter*>(g_pLTServer->HandleToObject(hKiller)) : LTNULL;
            if (!other || hKiller == b.hObj.GetHOBJECT())
            {
                b.rowFrags--;
                b.rowScore -= ValueOfSet(mp, b.scoreSet);
                KillMessage(MP_SERV_MSG_KILLED_HIMSELF, b.rowId, 0);
            }
            RowUpdate(b);
        }
        if (Bot* kb = BotOf(hKiller))
        {
            if (kb != &b && kb->rowId)
            {
                const bool teamKill = teams && kb->species == b.species;
                RowKill(*kb, teamKill, ValueOfSet(mp, b.scoreSet));
                KillMessage(teamKill ? MP_SERV_MSG_KILLED_TEAMMATE : MP_SERV_MSG_KILLED_SOMEONE, kb->rowId, b.rowId);
            }
        }
        else if (CPlayerObj* pk = hKiller ? dynamic_cast<CPlayerObj*>(g_pLTServer->HandleToObject(hKiller)) : LTNULL)
        {
            const int k2 = mp->FindClient(pk->GetClient());
            if (k2 >= 0 && b.rowId)
            {
                const bool teamKill = teams && mp->m_ClientData[k2].m_Data.m_nCharacterClass == kSpecies[b.species].startClass;
                KillMessage(teamKill ? MP_SERV_MSG_KILLED_TEAMMATE : MP_SERV_MSG_KILLED_SOMEONE,
                            mp->m_ClientData[k2].m_nClientID, b.rowId);
            }
        }
        if (!g_cfg.score)
        {
            mp->SetUpdateGameServ();
            return;
        }
        const int victimTeam = (int)kSpecies[b.species].startClass;
        const int value = ValueOfSet(mp, b.scoreSet);
        CPlayerObj* player = hKiller ? dynamic_cast<CPlayerObj*>(g_pLTServer->HandleToObject(hKiller)) : LTNULL;
        const int k = player ? mp->FindClient(player->GetClient()) : -1;
        if (k >= 0)
        {
            MPMgrData& killer = mp->m_ClientData[k];
            const int team = killer.m_Data.m_nCharacterClass;
            if (mp->m_GameInfo.m_nGameType.IsTeamMode() && team == victimTeam)
            {
                // the traitor rule
                killer.m_nFrags--;
                killer.m_nConsecutiveFrags = 0;
                killer.m_nScore -= value;
                TeamAdd(mp, team, -1, -value);
            }
            else
            {
                killer.m_nFrags++;
                mp->IncrementConsecutiveFrags(k);
                killer.m_nScore += value;
                TeamAdd(mp, team, 1, value);
            }
            killer.m_nUpdateFlags |= MP_CLIENT_UPDATE_FRAGS | MP_CLIENT_UPDATE_SCORE | MP_CLIENT_UPDATE_CONFRAGS;
            Log("SCORE %s killed by player %u (value %d)", b.name.c_str(), (unsigned)killer.m_nClientID, value);
        }
        else
        {
            CCharacter* other = hKiller ? dynamic_cast<CCharacter*>(g_pLTServer->HandleToObject(hKiller)) : LTNULL;
            const HOBJECT self = b.hObj.GetHOBJECT();
            if (!other || hKiller == self)
                TeamAdd(mp, victimTeam, -1, -value);   // killed himself (fall, own grenade)
            else if (SpeciesOf(other) == victimTeam)
                TeamAdd(mp, victimTeam, -1, -value);   // team kill
            else
                TeamAdd(mp, SpeciesOf(other), 1, value);
        }
        mp->SetUpdateGameServ();
    }

    // --- scoreboard rows -------------------------------------------------------------------------
    // The clients keep one table of 16 rows keyed by client ID (scoreboard-design.md). A bot row uses an
    // ID from 200 up that no real client has; players + bot rows stay below 16 with one slot kept free for
    // a joining player. Each row is announced once (ADD_CLIENT) and removed once (REMOVE_CLIENT).

    static int RowCount()
    {
        int n = 0;
        for (size_t i = 0; i < g_bots.size(); ++i)
            if (g_bots[i].rowId)
                ++n;
        return n;
    }

    static bool RowRoom(int extra)
    {
        MultiplayerMgr* mp = Mgr();
        return mp && (int)mp->m_nClients + RowCount() + extra + 1 <= MAX_CLIENTS;
    }

    static void RowAdd(Bot& b)
    {
        MultiplayerMgr* mp = Mgr();
        if (!g_cfg.scoreboard || b.rowId || !mp || !RowRoom(1) || b.scoreSet < 0)
            return;
        uint32 id = 0;
        for (uint32 cand = 200; cand < 256 && !id; ++cand)
        {
            bool used = mp->FindClient(cand) != -1;
            for (size_t i = 0; i < g_bots.size() && !used; ++i)
                used = g_bots[i].rowId == cand;
            if (!used)
                id = cand;
        }
        if (!id)
            return;
        b.rowId = id;
        b.rowFrags = b.rowScore = b.rowConFrags = 0;
        if (Mgr()->m_GameInfo.m_nGameType.Get() == MP_SURVIVOR)
            b.rowScore = b.survScore;
        HMESSAGEWRITE hWrite = g_pLTServer->StartMessage(LTNULL, MID_MPMGR_ADD_CLIENT);
        hWrite->WriteString(const_cast<char*>(b.display.c_str()));
        hWrite->WriteByte((uint8)kSpecies[b.species].startClass);
        hWrite->WriteByte((uint8)b.scoreSet);
        hWrite->WriteDWord(b.rowId);
        g_pLTServer->EndMessage(hWrite);
        Log("ROW add %s id=%u \"%s\"", b.name.c_str(), (unsigned)b.rowId, b.display.c_str());
    }

    static void RowRemove(Bot& b)
    {
        if (!b.rowId)
            return;
        HMESSAGEWRITE hWrite = g_pLTServer->StartMessage(LTNULL, MID_MPMGR_REMOVE_CLIENT);
        hWrite->WriteDWord(b.rowId);
        g_pLTServer->EndMessage(hWrite);
        Log("ROW remove %s id=%u", b.name.c_str(), (unsigned)b.rowId);
        b.rowId = 0;
    }

    static void RowUpdate(const Bot& b)
    {
        if (!b.rowId)
            return;
        HMESSAGEWRITE hWrite = g_pLTServer->StartMessage(LTNULL, MID_MPMGR_UPDATE_CLIENT);
        hWrite->WriteDWord(b.rowId);
        hWrite->WriteByte(MP_CLIENT_UPDATE_FRAGS | MP_CLIENT_UPDATE_CONFRAGS | MP_CLIENT_UPDATE_SCORE);
        hWrite->WriteWord((uint16)(int16)b.rowFrags);
        hWrite->WriteByte((uint8)(b.rowConFrags > 100 ? 100 : b.rowConFrags));
        hWrite->WriteWord((uint16)(int16)b.rowScore);
        g_pLTServer->EndMessage2(hWrite, MESSAGE_NAGGLE);
    }

    static void KillMessage(uint8 type, uint32 killer, uint32 victim)
    {
        if (!g_cfg.scoreboard || !killer)
            return;
        HMESSAGEWRITE hWrite = g_pLTServer->StartMessage(LTNULL, MID_MPMGR_SERVER_MESSAGE);
        hWrite->WriteByte(type);
        hWrite->WriteDWord(killer);
        if (type != MP_SERV_MSG_KILLED_HIMSELF)
            hWrite->WriteDWord(victim);
        g_pLTServer->EndMessage2(hWrite, MESSAGE_NAGGLE);
    }

    static Bot* BotOf(HOBJECT h)
    {
        for (size_t i = 0; i < g_bots.size(); ++i)
            if (h && g_bots[i].hObj.GetHOBJECT() == h)
                return &g_bots[i];
        return 0;
    }

    // a bot row's frags and score after a kill (the rules of ClientKilledMsg_DM / _TeamDM)
    static void RowKill(Bot& killer, bool teamKill, int value)
    {
        if (teamKill)
        {
            killer.rowFrags--;
            killer.rowScore -= value;
            killer.rowConFrags = 0;
        }
        else
        {
            killer.rowFrags++;
            killer.rowScore += value;
            killer.rowConFrags++;
        }
        RowUpdate(killer);
    }

    // rows for the game info a joining player gets (at most 'max'), and writing them
    static int RowsForJoiner(int max)
    {
        const int n = RowCount();
        return n < max ? n : (max > 0 ? max : 0);
    }

    static void WriteRows(HMESSAGEWRITE hWrite, int n)
    {
        for (size_t i = 0; i < g_bots.size() && n > 0; ++i)
        {
            const Bot& b = g_bots[i];
            if (!b.rowId)
                continue;
            hWrite->WriteString(const_cast<char*>(b.display.c_str()));
            hWrite->WriteByte((uint8)kSpecies[b.species].startClass);
            hWrite->WriteByte((uint8)b.scoreSet);
            hWrite->WriteDWord(b.rowId);
            hWrite->WriteWord(0);                                   // ping
            hWrite->WriteWord((uint16)(int16)b.rowFrags);
            hWrite->WriteByte((uint8)(b.rowConFrags > 100 ? 100 : b.rowConFrags));
            hWrite->WriteWord((uint16)(int16)b.rowScore);
            hWrite->WriteByte(1);                                   // lives
            hWrite->WriteByte(1);                                   // verified
            hWrite->WriteByte(0);                                   // observing
            --n;
        }
    }

    // keep players + rows within the table as players join, give rows back as they leave
    static void CheckRows()
    {
        if (!g_cfg.scoreboard)
        {
            for (size_t i = 0; i < g_bots.size(); ++i)
                RowRemove(g_bots[i]);
            return;
        }
        for (int i = (int)g_bots.size() - 1; i >= 0 && !RowRoom(0); --i)
            RowRemove(g_bots[i]);
        for (size_t i = 0; i < g_bots.size(); ++i)
            if (!g_bots[i].rowId && g_bots[i].scoreSet >= 0)
                RowAdd(g_bots[i]);
    }

    // connected players per species (index = Species value); returns the total
    static int CountHumans(int perSpecies[MAX_TEAMS])
    {
        for (int t = 0; t < MAX_TEAMS; ++t)
            perSpecies[t] = 0;
        MultiplayerMgr* mp = Mgr();
        int n = 0;
        for (int i = 0; mp && i < MAX_CLIENTS; ++i)
        {
            if (!mp->m_ClientData[i].m_hClient)
                continue;
            ++n;
            const int c = mp->m_ClientData[i].m_Data.m_nCharacterClass;
            if (c >= 0 && c < MAX_TEAMS)
                perSpecies[c]++;
        }
        return n;
    }

    static void LogTeams()
    {
        MultiplayerMgr* mp = Mgr();
        if (!mp || mp->m_GameInfo.m_nGameType.Get() != MP_TEAMDM)
            return;
        Log("TEAMS frags/score: marines %d/%d aliens %d/%d predators %d/%d",
            mp->m_TeamData[Marine].m_nFrags, mp->m_TeamData[Marine].m_nScore,
            mp->m_TeamData[Alien].m_nFrags, mp->m_TeamData[Alien].m_nScore,
            mp->m_TeamData[Predator].m_nFrags, mp->m_TeamData[Predator].m_nScore);
    }
};

namespace {
void ApplyCounts(const int want[BOT_SPECIES_COUNT]);

// 'fill': bots make up the difference between the connected players and g_cfg.fill. Each bot goes to the
// species with the fewest players + bots so far (balanced teams in Team Deathmatch, a mix in Deathmatch).
void ApplyFill()
{
    int humans[MAX_TEAMS];
    const int nHumans = BotScoreAccess::CountHumans(humans);
    int total = g_cfg.fill - nHumans;
    if (total < 0)
        total = 0;
    int want[BOT_SPECIES_COUNT] = {0, 0, 0};
    for (int k = 0; k < total; ++k)
    {
        int best = -1, bestCount = 0;
        for (int s = 0; s < BOT_SPECIES_COUNT; ++s)
        {
            const int count = humans[kSpecies[s].startClass] + want[s];
            if (best < 0 || count < bestCount)
            {
                best = s;
                bestCount = count;
            }
        }
        want[best]++;
    }
    if (nHumans != g_lastHumans)
    {
        Log("FILL %d players, %d bots: %d marines, %d aliens, %d predators", nHumans, total, want[BOT_MARINE],
            want[BOT_ALIEN], want[BOT_PREDATOR]);
        g_lastHumans = nHumans;
    }
    ApplyCounts(want);
}
} // namespace

namespace {
void ScoreRowAdd(Bot& b)
{
    BotScoreAccess::RowAdd(b);
}

void ScoreRowRemove(Bot& b)
{
    BotScoreAccess::RowRemove(b);
}
} // namespace

namespace {
int SurvivorRace(bool mutant)
{
    return BotScoreAccess::SurvRace(mutant);
}

int RaceSpecies(int race)
{
    for (int s = 0; s < BOT_SPECIES_COUNT; ++s)
        if (kSpecies[s].startClass == race)
            return s;
    return -1;
}

LTFLOAT g_nextSurvTick = 0.0f;

void SurvivorTick(LTFLOAT now)
{
    // +1 a second for each living survivor while the races fight (Update_Survivor's survival points)
    MultiplayerMgr* mp = g_pGameServerShell->GetMultiplayerMgr();
    if (!mp || mp->GetServerSubState() != MPMGR_SUBSTATE_SURVIVOR_CUT || now < g_nextSurvTick)
        return;
    g_nextSurvTick = now + 1.0f;
    for (size_t i = 0; i < g_bots.size(); ++i)
        if (g_bots[i].alive && g_bots[i].survTeam == 0)
            BotScoreAccess::BotScoreAdd(g_bots[i], 1);
}
} // namespace

namespace {
int HuntRace(int role)
{
    return BotScoreAccess::HuntRaceOf(role);
}

int EffectiveRole(const Bot& b)
{
    return b.switchAt > 0.0f && b.pendingRole >= 0 ? b.pendingRole : b.survTeam;
}

int HuntJoinRole()
{
    // MultiplayerMgr::ValidateClassAndCharacter for Hunt, over players + bots
    int humans[MAX_TEAMS];
    BotScoreAccess::CountHumans(humans);
    int hunters = humans[HuntRace(0)], prey = humans[HuntRace(1)];
    for (size_t i = 0; i < g_bots.size(); ++i)
        (EffectiveRole(g_bots[i]) ? prey : hunters)++;
    if (hunters < 1)
        return 0;
    return (hunters + 1) * BotScoreAccess::HuntRatio() > prey ? 1 : 0;
}
} // namespace

int BotMgr_RoleCount(int team)
{
    if (!g_navLoaded || (GameTypeNow() != MP_SURVIVOR && GameTypeNow() != MP_HUNT))
        return 0;
    int n = 0;
    for (size_t i = 0; i < g_bots.size(); ++i)
        if (EffectiveRole(g_bots[i]) == team)
            ++n;
    return n;
}

int BotMgr_SurvivorCount(int team)
{
    return BotMgr_RoleCount(team);
}

bool BotMgr_HuntBalance(int surplus)
{
    // surplus = hunters - wanted hunters (players + bots). Move one bot (lowest-frag prey up, highest-frag
    // hunter down) when one can move; return false to let the game move a player.
    if (!surplus || !g_navLoaded || GameTypeNow() != MP_HUNT)
        return false;
    const int from = surplus < 0 ? 1 : 0;
    Bot* pick = 0;
    for (size_t i = 0; i < g_bots.size(); ++i)
    {
        Bot& b = g_bots[i];
        if (EffectiveRole(b) != from || b.switchAt > 0.0f)
            continue;
        if (!pick || (from == 1 ? b.rowFrags < pick->rowFrags : b.rowFrags > pick->rowFrags))
            pick = &b;
    }
    if (!pick)
        return false;
    if (pick->alive)
        BotScoreAccess::BotSwitch(*pick, 1 - from);
    else
    {
        pick->survTeam = 1 - from;
        const int sp = RaceSpecies(HuntRace(pick->survTeam));
        if (sp >= 0)
            pick->species = (BotSpecies)sp;
        pick->forcedClass = 0;
    }
    Log("HUNT balance: %s becomes %s", pick->name.c_str(), from ? "a hunter" : "prey");
    return true;
}

void BotMgr_SurvivorNewRound(bool lastRound)
{
    if (!g_navLoaded || GameTypeNow() != MP_SURVIVOR)
        return;
    const int sp = RaceSpecies(SurvivorRace(false));
    const LTFLOAT now = g_pLTServer->GetTime();
    for (size_t i = 0; i < g_bots.size(); ++i)
    {
        Bot& b = g_bots[i];
        if (b.alive)
        {
            CAI* ai = BotAI(b);
            if (ai)
                g_pLTServer->RemoveObject(ai->m_hObject);
            b.alive = false;
        }
        if (b.survTeam && sp >= 0)
        {
            b.species = (BotSpecies)sp;
            b.forcedClass = 0;
            ScoreRowRemove(b);
        }
        b.survTeam = 0;
        b.rowConFrags = 0;
        b.respawnTime = lastRound ? now + 30.0f : now + 0.2f;
    }
    Log("SURVIVOR new round%s: %d bots back as survivors", lastRound ? " (last)" : "", (int)g_bots.size());
}

int BotMgr_ListedBots(int humans, int maxPlayers)
{
    // Clients refuse to join a server whose numplayers reaches maxplayers, so one slot always shows
    // free while there is room for a person; with no room the true (player) count is reported.
    if (!g_cfg.listbots || !g_navLoaded)
        return 0;
    int room = maxPlayers - 1 - humans;
    if (room < 0)
        room = 0;
    const int n = (int)g_bots.size();
    return n < room ? n : room;
}

const char* BotMgr_ListedBotName(int i)
{
    static std::string name;
    if (i < 0 || i >= (int)g_bots.size())
        return "[B]";
    const Bot& b = g_bots[i];
    if (!b.display.empty())
        return b.display.c_str();
    name = std::string("[B] ") + kSpecies[b.species].tag;
    return name.c_str();
}

int BotMgr_ListedBotRace(int i)
{
    return (i >= 0 && i < (int)g_bots.size()) ? kSpecies[g_bots[i].species].startClass : 0;
}

bool BotMgr_RowReachedLimit(int fragLimit, int scoreLimit)
{
    if (!g_cfg.scoreboard)
        return false;
    for (size_t i = 0; i < g_bots.size(); ++i)
    {
        const Bot& b = g_bots[i];
        if (!b.rowId)
            continue;
        if ((fragLimit >= 0 && b.rowFrags >= fragLimit) || (scoreLimit >= 0 && b.rowScore >= scoreLimit))
        {
            Log("ENDROUND %s reached the limit (frags %d, score %d)", b.name.c_str(), b.rowFrags, b.rowScore);
            return true;
        }
    }
    return false;
}

int BotMgr_ScoreRowsForJoiner(int maxRows)
{
    return g_cfg.scoreboard ? BotScoreAccess::RowsForJoiner(maxRows) : 0;
}

void BotMgr_WriteScoreRows(HMESSAGEWRITE hWrite, int count)
{
    if (g_cfg.scoreboard && count > 0)
        BotScoreAccess::WriteRows(hWrite, count);
}

namespace {
bool LifecycleOn()
{
    const int t = GameTypeNow();
    return g_cfg.lifecycle && (t == MP_DM || t == MP_TEAMDM) && BotScoreAccess::LifecycleOption();
}

// the hugger's chestburst: a chestburster bot comes out of the body (not a death of the bot)
void LifecycleBurst(Bot& b, const LTVector& pos, const LTRotation& rot, int hostSet, bool fallback)
{
    CAI* ai = BotAI(b);
    if (ai)
        g_pLTServer->RemoveObject(ai->m_hObject);
    b.alive = false;
    b.stage = 2;
    b.hostSet = hostSet;
    b.placeAt = true;
    b.placePos = LTVector(pos.x, pos.y + 24.0f, pos.z);
    b.placeRot = rot;
    b.respawnTime = g_pLTServer->GetTime();
    Log(fallback ? "LIFECYCLE %s bursts (fallback) out of a %s after %.1f s" : "LIFECYCLE %s bursts out of a %s after %.1f s", b.name.c_str(),
        hostSet >= 0 ? g_pCharacterButeMgr->GetModelType(hostSet) : "body", b.hugAt > 0.0f ? g_pLTServer->GetTime() - b.hugAt : -1.0f);
}
} // namespace

bool BotMgr_OnChestburst(HOBJECT hHugger, HOBJECT hBody, int nHostSet)
{
    if (!hHugger || !hBody)
        return false;
    for (size_t i = 0; i < g_bots.size(); ++i)
    {
        Bot& b = g_bots[i];
        if (!b.alive || b.hObj.GetHOBJECT() != hHugger)
            continue;
        LTVector pos;
        LTRotation rot;
        g_pLTServer->GetObjectPos(hBody, &pos);
        g_pLTServer->GetObjectRotation(hBody, &rot);
        LifecycleBurst(b, pos, rot, nHostSet);
        return true;
    }
    return false;
}

void BotMgr_OnFacehugAttach(HOBJECT hHugger, HOBJECT hBody, int nHostSet)
{
    Bot* b = hHugger ? FindBot(hHugger) : 0;
    if (!b || !hBody)
        return;
    b->hugBody = hBody;
    b->hugHost = nHostSet;
    b->hugAt = g_pLTServer->GetTime();
    Log("LIFECYCLE %s hugs a %s", b->name.c_str(), nHostSet >= 0 ? g_pCharacterButeMgr->GetModelType(nHostSet) : "?");
    if (g_cfg.testSleepBody)
        g_pLTServer->SetObjectState(hBody, OBJSTATE_INACTIVE);
}

bool BotMgr_ChestbursterGrown(HOBJECT hObj, int nNewSet)
{
    Bot* b = hObj ? FindBot(hObj) : 0;
    if (!b)
        return false;
    b->growInto = nNewSet;
    return true;
}

bool BotMgr_HideDest(CAI* pAI, LTVector* pDest)
{
    // a chestburster has 50 hit points and a bite, and grows in 20 s (ChestbursterMutateTime + Delay): a person
    // playing one runs from the fight; the AI attacked whoever it saw. Of a few random places it can reach, go to
    // the one farthest from the nearest enemy.
    Bot* b = pAI ? FindBot(pAI->m_hObject) : 0;
    if (!b || b->stage != 2 || !g_cfg.cbHide || !g_nav.IsLoaded() || true)  // superseded by BotMgr_CbHide
        return false;
    const LTVector& p = pAI->GetPosition();
    const LTVector& dims = pAI->GetDims();
    const int from = g_nav.Nearest(Vec3(p.x, p.y - dims.y, p.z));
    if (from < 0)
        return false;
    float best = -1.0f;
    for (int k = 0; k < 6; ++k)
    {
        const int to = g_nav.RandomReachable(from, GetRandom(0.0f, 1.0f), 0, false);
        if (to < 0)
            continue;
        const Vec3& n = g_nav.Node(to).pos;
        const LTVector dest(n.x, n.y + dims.y, n.z);
        float nearest = 1e9f;
        for (CCharacterMgr::CharacterIterator it = g_pCharacterMgr->BeginCharacters(); it != g_pCharacterMgr->EndCharacters(); ++it)
        {
            CCharacter* c = *it;
            if (!c || c == pAI || c->IsDead() || !g_pCharacterMgr->AreEnemies(*pAI, *c))
                continue;
            const float d = (c->GetPosition() - dest).Mag();
            if (d < nearest)
                nearest = d;
        }
        if (nearest > best)
        {
            best = nearest;
            *pDest = dest;
        }
    }
    return best >= 0.0f;
}

bool BotMgr_PlayerKilledByBot(HCLIENT hVictim, HOBJECT hKiller)
{
    if (!hKiller || g_bots.empty())
        return false;
    CAI* ai = dynamic_cast<CAI*>(g_pLTServer->HandleToObject(hKiller));
    if (!ai)
        return false;
    BotScoreAccess::PlayerKilled(hVictim, ai);
    return true;
}

// PickupObject declares this a friend (hook 'friend' in PickupObject.h).
struct BotPickupAccess
{
    static const char* Message(PickupObject* p) { return p ? p->m_puButes.m_szMessage : ""; }
    // AmmoBox declares this a friend too (hook 'friend' in AmmoBox.h)
    static bool BoxHuman(AmmoBox* b) { return b && b->m_bHumanOnly; }
    static bool BoxReady(AmmoBox* b) { return b && !b->m_bOpened && !b->m_bLocked; }
    static const char* BoxPickup(AmmoBox* b, int i)
    {
        return b && i >= 0 && i < (int)MAX_PICKUPS && !b->m_strPickupCommand[i].IsNull() ? b->m_strPickupCommand[i].CStr() : 0;
    }
};

namespace {
bool StartsWithNoCase(const char* s, const char* prefix)
{
    for (; *prefix; ++s, ++prefix)
        if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
            return false;
    return true;
}

// How much of an ammo pool a pickup message gives: "Ammo <pool> <n>" items, comma-separated
// ("Ammo Pulse_Rifle_Bullets 99, Ammo Impact_Grenades 4"; PickupButes Message, CCharacter::ProcessPowerupMsg).
int AmmoInMessage(const char* msg, const char* pool)
{
    int total = 0;
    const size_t plen = strlen(pool);
    for (const char* s = msg; s && *s; )
    {
        while (*s == ' ' || *s == ',' || *s == '	')
            ++s;
        if (StartsWithNoCase(s, "Ammo "))
        {
            const char* name = s + 5;
            while (*name == ' ')
                ++name;
            if (!_strnicmp(name, pool, plen) && (name[plen] == ' ' || name[plen] == '	'))
                total += atoi(name + plen);
        }
        s = strchr(s, ',');
    }
    return total;
}

int PoolMax(const char* pool, int def)
{
    AMMO_POOL* p = g_pWeaponMgr ? g_pWeaponMgr->GetAmmoPool(const_cast<char*>(pool)) : 0;
    const int n = p ? p->GetMaxAmount(LTNULL) : 0;
    return n > 0 ? n : def;
}

void FindPickups()
{
    g_pickups.clear();
    g_boxes.clear();
    for (int pass = 0; pass < 2; ++pass)
    {
        HOBJECT h = LTNULL;
        while ((h = pass == 0 ? g_pLTServer->GetNextObject(h) : g_pLTServer->GetNextInactiveObject(h)) != LTNULL)
        {
            if (AmmoBox* box = dynamic_cast<AmmoBox*>(g_pLTServer->HandleToObject(h)))
            {
                AmmoBoxInfo bi;
                bi.hBox = LTSmartLink(h);
                bi.human = BotPickupAccess::BoxHuman(box);
                bi.spears = bi.grenades = bi.slugs = bi.trackers = 0;
                bi.bannedUntil = 0.0f;
                for (int i = 0; i < (int)MAX_PICKUPS; ++i)
                    if (const char* m = BotPickupAccess::BoxPickup(box, i))
                    {
                        bi.spears += AmmoInMessage(m, "Spears");
                        bi.slugs += AmmoInMessage(m, "Shotgun_Slugs");
                        bi.trackers += AmmoInMessage(m, "SADAR_Tracking_Rockets");
                        bi.grenades += AmmoInMessage(m, "Impact_Grenades");
                    }
                g_boxes.push_back(bi);
                continue;
            }
            PickupObject* p = dynamic_cast<PickupObject*>(g_pLTServer->HandleToObject(h));
            if (!p)
                continue;
            const char* msg = BotPickupAccess::Message(p);
            const bool health = StartsWithNoCase(msg, "Health");
            if (!health && !StartsWithNoCase(msg, "Armor"))
                continue;
            PickupInfo pi;
            pi.hPickup = LTSmartLink(h);
            pi.health = health;
            g_pickups.push_back(pi);
        }
    }
    int spearBoxes = 0, grenadeBoxes = 0;
    for (size_t i = 0; i < g_boxes.size(); ++i)
    {
        spearBoxes += g_boxes[i].spears > 0 && !g_boxes[i].human;
        grenadeBoxes += g_boxes[i].grenades > 0 && g_boxes[i].human;
    }
    Log("PICKUPS %d health/armor, %d ammo boxes (%d with spears, %d with pulse-rifle grenades)", (int)g_pickups.size(),
        (int)g_boxes.size(), spearBoxes, grenadeBoxes);
}
} // namespace

namespace {

// --- Predator weapons as a player's (bots.cfg 'predammo', 'predlock') ------------------------------------

bool IsWeapon(const AIWBM_AIWeapon* w, const char* name)
{
    return w && !_stricmp(w->Name.c_str(), name);
}

// (slugs and trackers: see SadarLockUpkeep below)
const float kSlugMin = 300.0f;
const LTFLOAT kSadarLockTime = 2.0f;

void SetShotBarrel(CWeapon* cw, const char* barrelName)
{
    BARREL* barrel = cw ? g_pWeaponMgr->GetBarrel(barrelName) : 0;
    if (!barrel || cw->GetBarrelId() == barrel->nId)
        return;
    cw->SetBarrelId(barrel->nId);
    cw->SetAmmoId(barrel->nAmmoType);
    AMMO* ammo = g_pWeaponMgr->GetAmmo(barrel->nAmmoType);
    AMMO_POOL* pool = ammo ? g_pWeaponMgr->GetAmmoPool(ammo->szAmmoPool) : 0;
    if (pool && cw->GetParent())
        cw->GetParent()->SetInfinitePool(pool->nId, LTTRUE);  // BotMgr counts it (Bot::slugs, Bot::trackers)
}

// A person fires a tracker only at an enemy: the lock (on any character, as the client forms it) must be on one.
bool SadarLockOnEnemy(const Bot& b, CAI* ai)
{
    CCharacter* c = dynamic_cast<CCharacter*>(g_pLTServer->HandleToObject(b.sadarPotential.GetHOBJECT()));
    return c && !c->IsDead() && g_pCharacterMgr->AreEnemies(*ai, *c);
}


// The disc this character threw that is still in the world (found as CCharacter::HandleDiskRetrieve does).
CPredDisk* FindDisc(HOBJECT hOwner)
{
    ObjArray<HOBJECT, MAX_OBJECT_ARRAY_SIZE> objs;
    g_pLTServer->FindNamedObjects(const_cast<char*>("PredatorDisk"), objs);
    for (int i = 0; i < (int)objs.NumObjects(); ++i)
    {
        CPredDisk* d = dynamic_cast<CPredDisk*>(g_pLTServer->HandleToObject(objs.GetObject(i)));
        if (d && d->GetFiredFrom() == hOwner)
            return d;
    }
    return 0;
}

// A Predator bot's disc while it is out. A player has one disc (Discs pool, MaxAmount 1): it comes back by
// itself after a hit or a bounce (CPredDisk::ShouldDiskStick sets the thrower as its target) and is caught
// (CPredDisk::HandlePickup); when it sticks in a wall the player recalls it (COMMAND_ID_DISC_RETRIEVE ->
// CPredDisk::HandleDiskRetrieve) for NormalRetrieveEnergyCost (5, the disc flies back) when he sees it, else
// TeleportRetrieveEnergyCost (10, back at once) (FX attributes, PredatorDiskClassData). The AI's own energy is
// unlimited (CAIPredator::GetPredatorEnergy), so the bot pays from its player-like energy (EnergyUpkeep).
void DiscUpkeep(Bot& b, CAI* ai)
{
    if (!g_cfg.predAmmo || b.species != BOT_PREDATOR || !b.discOut)
        return;
    const LTFLOAT now = g_pLTServer->GetTime();
    if (now < b.nextDiscCheck)
        return;
    b.nextDiscCheck = now + 0.1f;
    CPredDisk* disc = FindDisc(ai->m_hObject);
    if (!disc)
    {
        b.discOut = false;
        if (g_cfg.debug)
            Log("DISC %s back after %.1f s", b.name.c_str(), now - b.discTime);
        return;
    }
    LTVector v;
    g_pLTServer->GetVelocity(disc->m_hObject, &v);
    if (v.MagSqr() > 0.0f || b.siftUntil > 0.0f)
        return;  // still flying (or sifting: no hands free)
    // stuck: recall it as soon as it stops (the conservative choice: a player notices it at once)
    PREDDISKCLASSDATA* cd = g_pFXButeMgr ? dynamic_cast<PREDDISKCLASSDATA*>(
        g_pFXButeMgr->GetProjectileClassData(const_cast<char*>("PredatorDiskClassData"))) : 0;
    const bool los = TestLOSIgnoreCharacters(ai->m_hObject, disc->m_hObject, LTVector(0, 0, 0), LTTRUE) ? true : false;
    const float cost = (float)(cd ? (los ? cd->nNormRetrieveCost : cd->nTeleRetrieveCost) : (los ? 5 : 10));
    if (b.energy < cost)
        return;  // later, after a sift (EnergyUpkeep sifts out of combat when low)
    b.energy -= cost;
    b.discTime = now;
    if (g_cfg.debug)
        Log("DISC %s recall %s cost=%.0f energy=%.0f", b.name.c_str(), los ? "fly" : "teleport", cost, b.energy);
    disc->HandleDiskRetrieve();
}

// The weapon a bot's damage or kill came from, for the logs: its current weapon, except the disc's damage
// type (Discs_Ammo_MP InstDamageType, DT_HIGHCALIBER) when the bot no longer holds the disc (it switches as soon
// as the disc is thrown); the combistick has the same type but is in hand (a melee weapon) when it hits.
const char* KillWeaponLabel(HOBJECT hDamager, int nType)
{
    CAI* ai = hDamager ? dynamic_cast<CAI*>(g_pLTServer->HandleToObject(hDamager)) : LTNULL;
    const AIWBM_AIWeapon* w = ai ? ai->GetCurrentWeaponButesPtr() : 0;
    Bot* bot = ai ? FindBot(hDamager) : 0;
    if (bot && bot->species == BOT_PREDATOR && !(w && w->bMeleeWeapon))
    {
        AMMO* disc = g_pWeaponMgr ? g_pWeaponMgr->GetAmmo("Discs_Ammo_MP") : 0;
        if (disc && nType == (int)disc->eInstDamageType)
            return "Disc_MP";
        // likewise a spear still flying when the last one made the bot switch (DT_SPEAR)
        AMMO* spear = g_pWeaponMgr ? g_pWeaponMgr->GetAmmo("Spears_Ammo_MP") : 0;
        if (spear && nType == (int)spear->eInstDamageType && !IsWeapon(w, "Spear_Gun_MP"))
            return "Spear_Gun_MP";
    }
    // a slug (DT_HIGHCALIBER; pellets have their own type) or a tracking rocket (the last SADAR shot was one)
    if (bot && bot->species == BOT_MARINE)
    {
        const LTFLOAT t = g_pLTServer->GetTime();
        AMMO* s = g_pWeaponMgr ? g_pWeaponMgr->GetAmmo("Shotgun_Slugs_Ammo_MP") : 0;
        if (bot->usesSlugs && s && nType == (int)s->eInstDamageType && t - bot->slugLastShot < 3.0f)
            return "Shotgun_Slug_MP";
        AMMO* r = g_pWeaponMgr ? g_pWeaponMgr->GetAmmo("SADAR_Tracking_Rockets_Ammo_MP") : 0;
        if (bot->usesTrackers && bot->lastSadarTracker && r && t - bot->trackerLastShot < 5.0f
            && (nType == (int)r->eInstDamageType || nType == (int)r->eAreaDamageType))
            return "SADAR_Tracker_MP";
    }
    // a pulse-rifle grenade landing after the bot went back to the rifle (DT_EXPLODE from a Demolitions bot)
    if (bot && bot->usesGrenades && !IsWeapon(w, kPRGrenade) && g_pLTServer->GetTime() - bot->altLastShot < 5.0f)
    {
        AMMO* g = g_pWeaponMgr ? g_pWeaponMgr->GetAmmo("Impact_Grenades_Ammo_MP") : 0;
        if (g && (nType == (int)g->eInstDamageType || nType == (int)g->eAreaDamageType))
            return kPRGrenade;
    }
    return w ? w->Name.c_str() : "-";
}

// A Predator bot holding a weapon it cannot fire now (no spears, disc out) takes its best usable weapon at
// once, as a player switches, instead of waiting for the AI's periodic choice (AICombatGoal, ~1 s). The AI's
// weapon animation follows by itself (CCharacter::UpdateAnimation).
void PredWeaponUpkeep(Bot& b, CAI* ai)
{
    if (!g_cfg.predAmmo || b.species != BOT_PREDATOR || !ai->HasTarget())
        return;
    const AIWBM_AIWeapon* w = ai->GetCurrentWeaponButesPtr();
    if (!w || BotMgr_WeaponUsable(ai->m_hObject, w->Name.c_str()))
        return;
    LTVector tp;
    g_pLTServer->GetObjectPos(ai->GetTarget().GetObject(), &tp);
    ai->ChooseBestWeapon((tp - ai->GetPosition()).MagSqr());
    const AIWBM_AIWeapon* nw = ai->GetCurrentWeaponButesPtr();
    if (nw != w && g_cfg.debug)
        Log("PREDSWITCH %s %s -> %s (spears=%d disc=%s)", b.name.c_str(), w->Name.c_str(), nw ? nw->Name.c_str() : "-",
            b.spears, b.discOut ? "out" : "in");
}

// The players' auto-target of the disc and the shoulder cannon (client CAutoTargetMgr::UpdatePredShoulderCannon,
// used for both: sdk/pristine/proj/AVP2/ClientShellDLL/AutoTargetMgr.cpp:154-326). Every frame: the nearest
// character of the species the current vision mode targets (VisionModeButes: HeatVision humans, ElectroVision
// aliens, NavVision Predators), with FLAG_VISIBLE (a cloak keeps it), in front of the camera and on screen,
// closer than MAX_AUTO_TARGET_RANGE_SQ (5000000: 2236 units), and the first thing a ray from 20 units in front
// of the eye hits (projectiles and bodies skipped). No lock time. The fire message carries it
// (WeaponModel.cpp:3791) and the projectile homes on it (CPredDisk::Setup, CShoulderCannon::Setup).
// A bot is taken to be in the vision mode of its current target's species (a player switches with one key,
// ViewModeMGR::NextMode, at once); the screen is its character set's FOV (Predators 90 x 78 degrees).
LTBOOL AutoTargetFilter(HOBJECT h, void* user)
{
    HOBJECT self = (HOBJECT)user;
    if (h == self)
        return LTFALSE;
    if (IsCharacterHitBox(h))
    {
        // the shooter's own hit box, and a body's (the client skips bodies; the server keeps their hit boxes)
        CCharacterHitBox* hb = dynamic_cast<CCharacterHitBox*>(g_pLTServer->HandleToObject(h));
        if (hb && (hb->GetModelObject() == self || IsBodyProp(hb->GetModelObject())))
            return LTFALSE;
    }
    if (IsBodyProp(h) || dynamic_cast<CProjectile*>(g_pLTServer->HandleToObject(h)))
        return LTFALSE;
    return LTTRUE;
}

int VisionClass(const CCharacter* c)  // 0 none, 1 humans (IsHuman), 2 aliens, 3 Predators
{
    const CharacterButes* cb = c ? c->GetButes() : 0;
    if (!cb)
        return 0;
    switch (cb->m_eCharacterClass)
    {
        case MARINE: case CORPORATE: return 1;
        case ALIEN: return 2;
        case PREDATOR: return 3;
        default: return 0;  // exosuits and synthetics: no vision mode targets them
    }
}

// The Predator's vision modes, as the client has them (sdk/pristine ClientShellDLL):
// - Predator_Visions (VisionModeButes VisionSet2) cycles Normal -> HeatVision -> ElectroVision -> NavVision ->
//   Normal; the NextVision and PrevVision keys (COMMAND_ID_NEXT/PREV_VISIONMODE, GameClientShell.cpp:3156-3200)
//   step one mode forward or back (ViewModeMGR::NextMode/PrevMode, VisionModeButeMGR::GetNextMode); no direct key.
// - The auto-target locks only the species of the current mode (TargetHuman/Alien/Predator); Normal locks
//   nothing (AutoTargetMgr.cpp:208-237).
// - A player respawns in Normal (the mode is reset on PS_DYING/PS_DEAD); without his mask he cannot change
//   modes and is put back to Normal (CanChangeMode, CPlayerStats::SetHasMask); an EMP effect shows Normal
//   until it ends (ViewModeMGR::Update).
// - A switch is instant, with a 0.2 s screen transition (CameraMgrFX_VisionCycleCreate, cvar VisionTime 0.2).
// A bot heads for the mode of its current target's species, one key press per kVisionPress (0.25 s, a
// conservative human rate: unknown, chosen), the shorter way round; a mode locks kVisionTransition after the
// press that entered it.
const LTFLOAT kVisionPress = 0.25f;
const LTFLOAT kVisionTransition = 0.2f;

const char* VisionName(int mode)
{
    static const char* const kNames[4] = {"Normal", "HeatVision", "ElectroVision", "NavVision"};
    return mode >= 0 && mode < 4 ? kNames[mode] : "?";
}

void VisionUpkeep(Bot& b, CAI* ai)
{
    if (!g_cfg.predLock || b.species != BOT_PREDATOR)
        return;
    const LTFLOAT now = g_pLTServer->GetTime();
    if (!ai->HasMask())
    {
        if (b.vision != 0 && g_cfg.debug)
            Log("VISION %s %s -> Normal (mask lost)", b.name.c_str(), VisionName(b.vision));
        b.vision = 0;
        b.visionNeedSince = 0.0f;
        return;
    }
    int want = b.vision;
    if (ai->HasTarget())
    {
        CCharacter* tc = dynamic_cast<CCharacter*>(g_pLTServer->HandleToObject(ai->GetTarget().GetObject()));
        const int c = VisionClass(tc);
        if (c)
            want = c;
    }
    if (want == b.vision)
    {
        b.visionNeedSince = 0.0f;
        return;
    }
    if (b.visionNeedSince == 0.0f)
        b.visionNeedSince = now;
    if (now < std::max(b.visionNeedSince, b.visionPressAt) + kVisionPress)
        return;
    const int fwd = (want - b.vision + 4) % 4;  // NextVision presses needed; PrevVision: 4 - fwd
    const int step = fwd <= 2 ? 1 : -1;
    const int was = b.vision;
    b.vision = (b.vision + step + 4) % 4;
    b.visionPressAt = now;
    if (g_cfg.debug)
        Log("VISION %s %s -> %s (%s, wants %s for %s, %.2f s after the need)", b.name.c_str(), VisionName(was),
            VisionName(b.vision), step > 0 ? "next" : "prev", VisionName(want),
            ai->HasTarget() ? ObjectLabel(ai->GetTarget().GetObject()).c_str() : "-", now - b.visionNeedSince);
}

// The camera of a bot: its eye, looking along the shot (a player's crosshair is where his shot goes), and the
// screen size (tangents of half its FOV).
struct AutoTargetView
{
    LTVector from, f, r, u;
    float tx, ty;
};

bool AutoTargetViewOf(CAI* ai, const LTVector& dir, AutoTargetView& v)
{
    v.f = dir;
    v.u = LTVector(0.0f, 1.0f, 0.0f);
    v.r = v.u.Cross(v.f);
    if (v.r.MagSqr() < 0.01f)
    {
        v.u = ai->GetAimUp();
        v.r = v.u.Cross(v.f);
    }
    if (v.f.MagSqr() < 0.01f || v.r.MagSqr() < 0.01f)
        return false;
    v.f.Norm();
    v.r.Norm();
    v.u = v.f.Cross(v.r);
    v.u.Norm();
    v.from = ai->GetEyePosition() + v.f * 20.0f;
    const CPoint fov = g_pCharacterButeMgr->GetFOV(ai->GetCharacter());
    v.tx = tanf(0.5f * (float)(fov.x > 0 ? fov.x : 90) * 3.14159f / 180.0f);
    v.ty = tanf(0.5f * (float)(fov.y > 0 ? fov.y : 78) * 3.14159f / 180.0f);
    return true;
}

// Why a character is not a valid auto-target ("" = valid); d2 = its squared distance.
const char* AutoTargetCheck(CAI* ai, const AutoTargetView& v, CCharacter* c, float& d2, float maxD2 = 5000000.0f)
{
    d2 = 0.0f;
    if (!c || c == ai || c->IsDead() || !(g_pLTServer->GetObjectFlags(c->m_hObject) & FLAG_VISIBLE))
        return "hidden";
    LTVector pos;
    g_pLTServer->GetObjectPos(c->m_hObject, &pos);
    const LTVector to = pos - v.from;
    d2 = to.MagSqr();
    if (d2 >= maxD2)
        return "range";
    const float z = to.Dot(v.f);
    if (z <= 0.0f || fabsf(to.Dot(v.r)) >= z * v.tx || fabsf(to.Dot(v.u)) >= z * v.ty)
        return "offscreen";
    IntersectQuery q;
    IntersectInfo info;
    q.m_From = v.from;
    q.m_To = pos;
    q.m_Flags = INTERSECT_HPOLY | INTERSECT_OBJECTS | IGNORE_NONSOLID;
    q.m_FilterFn = AutoTargetFilter;
    q.m_pUserData = (void*)ai->m_hObject;
    // the client takes it when the first thing its ray hits is that character; here a model's ray test can let
    // the segment reach its centre without a hit: nothing in between counts as seen too
    if (!g_pLTServer->IntersectSegment(&q, &info))
        return "";
    if (info.m_hObject != c->m_hObject)
    {
        CCharacterHitBox* hb = IsCharacterHitBox(info.m_hObject)
            ? dynamic_cast<CCharacterHitBox*>(g_pLTServer->HandleToObject(info.m_hObject)) : 0;
        if (!hb || hb->GetModelObject() != c->m_hObject)
        {
            if (!g_cfg.debug)
                return "blocked";
            static char buf[96];
            HOBJECT who = hb ? (HOBJECT)hb->GetModelObject() : info.m_hObject;
            _snprintf(buf, sizeof(buf) - 1, "blocked-%s:%s", hb ? "hitbox" : (IsCharacter(who) ? "char" : "world"),
                      who && (hb || IsCharacter(who)) ? ObjectLabel(who).c_str() : "-");
            buf[sizeof(buf) - 1] = 0;
            return buf;
        }
    }
    return "";
}

// mode: the species the shooter's vision mode locks (VisionClass numbering; 0 = Normal, nothing)
HOBJECT AutoTarget(CAI* ai, HOBJECT hAITarget, const LTVector& dir, int mode, std::string& why)
{
    why.clear();
    CCharacter* tc = hAITarget ? dynamic_cast<CCharacter*>(g_pLTServer->HandleToObject(hAITarget)) : 0;
    AutoTargetView v;
    if (!mode || !AutoTargetViewOf(ai, dir, v))
    {
        why = "novision";
        return LTNULL;
    }
    if (tc && VisionClass(tc) != mode)
        why = std::string("mode-") + VisionName(mode);  // the AI's target is not of the species this mode locks
    float best = 5000000.0f;
    HOBJECT pick = LTNULL;
    for (CCharacterMgr::CharacterIterator it = g_pCharacterMgr->BeginCharacters(); it != g_pCharacterMgr->EndCharacters(); ++it)
    {
        CCharacter* c = *it;
        if (!c || c == ai || VisionClass(c) != mode)
            continue;
        float d2;
        const char* r = AutoTargetCheck(ai, v, c, d2);
        if (c == tc)
            why = r;  // for the debug line: why the AI's own target was not taken
        if (r[0] || d2 >= best)
            continue;
        best = d2;
        pick = c->m_hObject;
    }
    return pick;
}

} // namespace

bool BotMgr_WeaponUsable(HOBJECT hAI, const char* szWeapon)
{
    if (!szWeapon || g_bots.empty())
        return true;
    if (!_stricmp(szWeapon, kPRGrenade))
    {
        // the pulse rifle's grenades: only those the bot carries (bots.cfg altfire)
        Bot* b = FindBot(hAI);
        return b && g_cfg.altFire && b->grenades > 0;
    }
    if (!g_cfg.predAmmo)
        return true;
    Bot* bot = FindBot(hAI);
    if (!bot || bot->species != BOT_PREDATOR)
        return true;
    if (!_stricmp(szWeapon, "Spear_Gun_MP"))
        return bot->spears > 0;
    if (!_stricmp(szWeapon, "Disc_MP"))
        return !bot->discOut;
    return true;
}

bool BotMgr_PredAmmoBot(HOBJECT hAI)
{
    if (!g_cfg.predAmmo || !hAI || g_bots.empty())
        return false;
    Bot* bot = FindBot(hAI);
    return bot && bot->species == BOT_PREDATOR;
}

static void SpendShot(HOBJECT hShooter, CAI* ai, const AIWBM_AIWeapon* w, Bot* bot);

HOBJECT BotMgr_FireCommit(HOBJECT hShooter, HOBJECT hTarget, const LTVector& vFirePos, LTVector& vPath)
{
    Bot* bot = FindBot(hShooter);
    if (!bot)
        return hTarget;
    CAI* ai = BotAI(*bot);
    const AIWBM_AIWeapon* w = ai ? ai->GetCurrentWeaponButesPtr() : 0;
    SpendShot(hShooter, ai, w, bot);
    if (bot->species == BOT_MARINE && w && IsWeapon(w, kPRGrenade))
    {
        const LTFLOAT t = g_pLTServer->GetTime();
        if (bot->grenades > 0)
            --bot->grenades;
        bot->altLastShot = t;
        bot->altNext = t + GetRandom(w->fMinBurstDelay, w->fMaxBurstDelay);
        bot->altBackAt = t + 0.1f;  // back to the rifle (AltFireUpkeep)
        LTVector a, b;
        g_pLTServer->GetObjectPos(hShooter, &a);
        if (hTarget)
            g_pLTServer->GetObjectPos(hTarget, &b);
        Log("ALTFIRE %s %s left=%d dist=%.0f", bot->name.c_str(), kPRGrenade, bot->grenades, hTarget ? (b - a).Mag() : -1.0f);
    }
    if (bot->species == BOT_MARINE && w && g_cfg.altFire && ai)
    {
        CWeapon* cw = ai->GetCurrentWeaponPtr();
        const LTFLOAT t = g_pLTServer->GetTime();
        LTVector a, tp;
        g_pLTServer->GetObjectPos(hShooter, &a);
        if (hTarget)
            g_pLTServer->GetObjectPos(hTarget, &tp);
        const float dist = hTarget ? (tp - a).Mag() : -1.0f;
        if (IsWeapon(w, "Shotgun_MP"))
        {
            const bool slug = bot->slugs > 0 && hTarget && dist >= kSlugMin;
            SetShotBarrel(cw, slug ? "Shotgun_Slug_Barrel_MP" : "Shotgun_Barrel_MP");
            if (slug)
            {
                --bot->slugs;
                bot->slugLastShot = t;
                Log("ALTFIRE %s Shotgun_Slug left=%d dist=%.0f", bot->name.c_str(), bot->slugs, dist);
            }
        }
        else if (IsWeapon(w, "SADAR_MP"))
        {
            const HOBJECT lock = bot->sadarLocked && SadarLockOnEnemy(*bot, ai) ? bot->sadarPotential.GetHOBJECT() : LTNULL;
            const bool tracker = bot->trackers > 0 && lock;
            SetShotBarrel(cw, tracker ? "SADAR_Tracking_Barrel_MP" : "SADAR_Barrel_MP");
            bot->lastSadarTracker = tracker;
            if (tracker)
            {
                --bot->trackers;
                bot->trackerLastShot = t;
                Log("ALTFIRE %s SADAR_Tracker left=%d dist=%.0f lock=%s aitarget=%s", bot->name.c_str(), bot->trackers, dist,
                    ObjectLabel(lock).c_str(), hTarget ? ObjectLabel(hTarget).c_str() : "-");
                return lock;  // the tracking rocket homes on the locked character (CTrackingSADAR::Setup)
            }
        }
    }
    if (bot->species != BOT_PREDATOR || !w)
        return hTarget;
    const LTFLOAT now = g_pLTServer->GetTime();
    if (g_cfg.predAmmo && IsWeapon(w, "Spear_Gun_MP"))
    {
        if (bot->spears > 0)
            --bot->spears;
        if (g_cfg.debug)
            Log("SPEARS %s left=%d", bot->name.c_str(), bot->spears);
    }
    const bool disc = IsWeapon(w, "Disc_MP");
    const bool cannon = IsWeapon(w, "Shoulder_Cannon_MP");
    if (g_cfg.predAmmo && disc)
    {
        bot->discOut = true;
        bot->discTime = now;
        bot->nextDiscCheck = now + 0.1f;
        if (g_cfg.debug)
            Log("DISC %s out", bot->name.c_str());
    }
    if (!g_cfg.predLock || (!disc && !cannon))
        return hTarget;
    // the lock needs a vision mode that targets something (not Normal), out of its 0.2 s transition, and no EMP
    // effect (the client shows Normal during it)
    std::string why;
    HOBJECT lock = LTNULL;
    if (bot->vision == 0)
        why = "mode-Normal";
    else if (now < bot->visionPressAt + kVisionTransition)
        why = "mode-transition";
    else if (g_pLTServer->GetObjectUserFlags(hShooter) & USRFLG_CHAR_EMPEFFECT)
        why = "emp";
    else
        lock = AutoTarget(ai, hTarget, vPath, bot->vision, why);
    if (g_cfg.debug)
    {
        const HOBJECT was = bot->lockTarget.GetHOBJECT();
        LTVector a, b;
        g_pLTServer->GetObjectPos(hShooter, &a);
        if (hTarget)
            g_pLTServer->GetObjectPos(hTarget, &b);
        Log("LOCK %s %s weapon=%s on=%s aitarget=%s dist=%.0f vision=%s%s%s", bot->name.c_str(),
            lock ? (lock == was ? "kept" : "acquired") : (was ? "lost" : "none"), w->Name.c_str(),
            lock ? ObjectLabel(lock).c_str() : "-", hTarget ? ObjectLabel(hTarget).c_str() : "-",
            hTarget ? (b - a).Mag() : -1.0f, VisionName(bot->vision), why.empty() ? "" : " aitarget_not=", why.c_str());
    }
    bot->lockTarget = lock;
    if (cannon && lock)
    {
        // the client turns a shoulder cannon shot toward the auto-target's upper torso (CWeaponModel::GetFireInfo,
        // 'bCanSetDir', not for the disc): no aim error
        LTVector tp;
        bool node = false;
        ModelLT* pModelLT = g_pLTServer->GetModelLT();
        HMODELNODE hNode;
        if (pModelLT->GetNode(lock, const_cast<char*>("Torso_u_node"), hNode) == LT_OK)
        {
            LTransform t;
            if (pModelLT->GetNodeTransform(lock, hNode, t, LTTRUE) == LT_OK)
            {
                g_pLTServer->GetTransformLT()->GetPos(t, tp);
                node = true;
            }
        }
        if (!node)
        {
            LTVector dims;
            g_pLTServer->GetObjectPos(lock, &tp);
            g_pLTServer->GetObjectDims(lock, &dims);
            tp.y += dims.y * 0.5f;
        }
        LTVector to = tp - vFirePos;
        if (to.MagSqr() > 1.0f)
        {
            to.Norm();
            vPath = to;
        }
    }
    return lock;
}

bool BotMgr_CanTakePickup(HOBJECT hObj, const char* szMessage)
{
    // as a Marine player: health and armor (weapons and ammo would change the AI's own weapon set)
    if (!hObj || !szMessage || !BotMgr_IsBot(hObj))
        return false;
    CCharacter* c = dynamic_cast<CCharacter*>(g_pLTServer->HandleToObject(hObj));
    if (!c || SpeciesOf(c) != Marine)
        return false;
    return StartsWithNoCase(szMessage, "Health") || StartsWithNoCase(szMessage, "Armor");
}

// Ammo a bot takes from a box or a pickup message: the pools it counts (spears for a spear gun, pulse-rifle
// grenades for a pulse rifle), up to the pool's MaxAmount (Spears 27, Impact_Grenades 8) as for a player.
namespace {
int CreditAmmo(Bot& b, const char* msg, const char* how)
{
    int got = 0;
    if (b.usesSpears && g_cfg.predAmmo)
    {
        const int n = AmmoInMessage(msg, "Spears");
        const int was = b.spears;
        b.spears = std::min(PoolMax("Spears", 27), b.spears + n);
        got += b.spears - was;
    }
    if (b.usesGrenades && g_cfg.altFire)
    {
        const int n = AmmoInMessage(msg, "Impact_Grenades");
        const int was = b.grenades;
        b.grenades = std::min(PoolMax("Impact_Grenades", 8), b.grenades + n);
        got += b.grenades - was;
    }
    if (b.usesSlugs && g_cfg.altFire)
    {
        const int was = b.slugs;
        b.slugs = std::min(PoolMax("Shotgun_Slugs", 48), b.slugs + AmmoInMessage(msg, "Shotgun_Slugs"));
        got += b.slugs - was;
    }
    if (b.usesTrackers && g_cfg.altFire)
    {
        const int was = b.trackers;
        b.trackers = std::min(PoolMax("SADAR_Tracking_Rockets", 12), b.trackers + AmmoInMessage(msg, "SADAR_Tracking_Rockets"));
        got += b.trackers - was;
    }
    if (got > 0 || g_cfg.debug)
        Log("%s %s \"%s\" spears=%d grenades=%d slugs=%d trackers=%d", how, b.name.c_str(), msg, b.spears, b.grenades,
            b.slugs, b.trackers);
    return got;
}

// The ammo box a bot wants: a Predator with a spear gun at half its spawn spears or less; a Marine with a pulse
// rifle and no grenade left, or a SADAR and no tracker or no slug left (human habits, conservative: the thresholds
// are not game values). A Smartgunner never fires its shotgun (the Smart Gun covers every range): no box run.
enum { NEED_SPEARS = 1, NEED_GRENADES = 2, NEED_SLUGS = 4, NEED_TRACKERS = 8 };

int AmmoNeeds(const Bot& b, bool& human)
{
    if (!g_cfg.ammoPickups)
        return 0;
    if (b.species == BOT_PREDATOR)
    {
        human = false;
        return b.usesSpears && g_cfg.predAmmo && b.spears <= SpearsAtSpawn() / 2 ? NEED_SPEARS : 0;
    }
    if (b.species != BOT_MARINE || !g_cfg.altFire)
        return 0;
    human = true;
    int need = 0;
    if (b.usesGrenades && b.grenades == 0)
        need |= NEED_GRENADES;
    if (b.usesTrackers && b.trackers == 0)
        need |= NEED_TRACKERS;
    if (b.usesTrackers && b.usesSlugs && b.slugs == 0)
        need |= NEED_SLUGS;
    return need;
}

int BoxHolds(const AmmoBoxInfo& bi)
{
    return (bi.spears > 0 ? NEED_SPEARS : 0) | (bi.grenades > 0 ? NEED_GRENADES : 0) | (bi.slugs > 0 ? NEED_SLUGS : 0)
        | (bi.trackers > 0 ? NEED_TRACKERS : 0);
}

// The box the roam goal heads for: touching it opens it (AmmoBox MID_TOUCHNOTIFY, CanTouchActivate); a player can
// also press Use on it (CanActivate, both set on every multiplayer box): within reach the bot does, as for doors.
void AmmoUpkeep(Bot& b, CAI* ai)
{
    HOBJECT h = b.goalBox.GetHOBJECT();
    if (!h)
        return;
    AmmoBox* box = dynamic_cast<AmmoBox*>(g_pLTServer->HandleToObject(h));
    if (!box || !BotPickupAccess::BoxReady(box))
    {
        b.goalBox = (HOBJECT)LTNULL;
        return;
    }
    LTVector q;
    g_pLTServer->GetObjectPos(h, &q);
    if ((q - ai->GetPosition()).MagSqr() > 96.0f * 96.0f)
        return;
    if (g_cfg.debug)
        Log("AMMOUSE %s presses Use on the box at %.0f %.0f %.0f", b.name.c_str(), q.x, q.y, q.z);
    SendTriggerMsgToObject(ai, h, "ACTIVATE");
    b.goalBox = (HOBJECT)LTNULL;
}

// Shotgun slugs and SADAR tracking rockets (bots.cfg altfire). A player picks the shotgun's ammo and the SADAR's
// rockets with the next-ammo key (the barrels' NextBarrel: Shotgun_Barrel_MP <-> Shotgun_Slug_Barrel_MP, SADAR_Barrel_MP
// <-> SADAR_Tracking_Barrel_MP); a bot's shot takes the barrel it should have at that moment (SetShotBarrel), from
// the ammo it counts: none at spawn, 6 slugs per Ammo_Combo_Shotgun, 3 trackers per Ammo_Combo_SADAR.
// - Slugs (one 180 shot, spread 5-10) beyond kSlugMin, pellets (8 x 40, spread 40-100) closer: about where half the
//   pellets still hit a body (chosen from the barrels' spreads; the bots' shotgun fires up to 750 units).
// - A tracker homes on the projectile's target (CTrackingSADAR::Setup takes the fire message's target), which the
//   client sets only once the SADAR has locked: the same character kept in the central 40 % of the screen (30-70 %
//   of its width and height), in line of sight and in front, nearest the centre, for g_cvarSADARLockTime (2 s)
//   (AutoTargetMgr.cpp:1260-1470; any character, as the client does). Without a lock the bot fires a plain rocket.
HOBJECT SadarLockCandidate(CAI* ai)
{
    AutoTargetView v;
    if (!AutoTargetViewOf(ai, ai->GetAimForward(), v))
        return LTNULL;
    const float tx = v.tx, ty = v.ty;
    v.tx *= 0.4f;  // the central lock window
    v.ty *= 0.4f;
    float best = 1e30f;
    HOBJECT pick = LTNULL;
    for (CCharacterMgr::CharacterIterator it = g_pCharacterMgr->BeginCharacters(); it != g_pCharacterMgr->EndCharacters(); ++it)
    {
        CCharacter* c = *it;
        float d2;
        if (!c || c == ai || AutoTargetCheck(ai, v, c, d2, 1e30f)[0])
            continue;
        LTVector pos;
        g_pLTServer->GetObjectPos(c->m_hObject, &pos);
        const LTVector to = pos - v.from;
        const float z = to.Dot(v.f);
        const float sx = to.Dot(v.r) / (z * tx) * (4.0f / 3.0f);  // screen distance from the centre, 4:3
        const float sy = to.Dot(v.u) / (z * ty);
        if (sx * sx + sy * sy < best)
        {
            best = sx * sx + sy * sy;
            pick = c->m_hObject;
        }
    }
    return pick;
}

void SadarLockUpkeep(Bot& b, CAI* ai)
{
    const bool forming = g_cfg.altFire && b.species == BOT_MARINE && b.usesTrackers && b.trackers > 0
        && IsWeapon(ai->GetCurrentWeaponButesPtr(), "SADAR_MP");
    const HOBJECT cand = forming ? SadarLockCandidate(ai) : LTNULL;
    const LTFLOAT now = g_pLTServer->GetTime();
    if (!cand || cand != b.sadarPotential.GetHOBJECT())
    {
        if (b.sadarLocked && g_cfg.debug)
            Log("SADARLOCK %s lost", b.name.c_str());
        b.sadarPotential = cand;
        b.sadarSince = now;
        b.sadarLocked = false;
        return;
    }
    if (!b.sadarLocked && now - b.sadarSince > kSadarLockTime)
    {
        b.sadarLocked = true;
        if (g_cfg.debug)
            Log("SADARLOCK %s locked on %s", b.name.c_str(), ObjectLabel(cand).c_str());
    }
}

// A Stalker's run for spears in a fight, as a person does only when it is safe (bots.cfg ammopickups): few spears
// (half its spawn spears or less), its target far (over 800 units) or its disc out, no enemy within 800 units in
// line of sight, and a closed spear box within 1500 units (conservative choices, not game values). The roam goal
// then outbids the attack (BotRoamGoal::GetBid).
bool AmmoRunCheck(Bot& b, CAI* ai)
{
    if (!g_cfg.ammoPickups || !g_cfg.predAmmo || b.species != BOT_PREDATOR || !b.usesSpears || !ai->HasTarget())
        return false;
    if (b.spears > SpearsAtSpawn() / 2)
        return false;
    LTVector tp;
    g_pLTServer->GetObjectPos(ai->GetTarget().GetObject(), &tp);
    const LTVector eye = ai->GetEyePosition();
    const float far2 = 800.0f * 800.0f;
    if ((tp - eye).MagSqr() <= far2 && !b.discOut)
        return false;
    for (CCharacterMgr::CharacterIterator it = g_pCharacterMgr->BeginCharacters(); it != g_pCharacterMgr->EndCharacters(); ++it)
    {
        CCharacter* c = *it;
        if (!c || c == ai || c->IsDead() || !g_pCharacterMgr->AreEnemies(*ai, *c))
            continue;
        LTVector pos;
        g_pLTServer->GetObjectPos(c->m_hObject, &pos);
        if ((pos - eye).MagSqr() > far2)
            continue;
        IntersectQuery q;
        IntersectInfo info;
        q.m_From = eye;
        q.m_To = pos;
        q.m_Flags = INTERSECT_HPOLY | IGNORE_NONSOLID;
        if (!g_pLTServer->IntersectSegment(&q, &info))
            return false;  // an enemy close and in view
    }
    const LTVector& p = ai->GetPosition();
    for (size_t i = 0; i < g_boxes.size(); ++i)
    {
        const AmmoBoxInfo& bi = g_boxes[i];
        if (bi.human || bi.spears <= 0 || g_pLTServer->GetTime() < bi.bannedUntil)
            continue;
        HOBJECT h = bi.hBox.GetHOBJECT();
        AmmoBox* box = h ? dynamic_cast<AmmoBox*>(g_pLTServer->HandleToObject(h)) : 0;
        if (!box || !BotPickupAccess::BoxReady(box) || !(g_pLTServer->GetObjectFlags(h) & FLAG_VISIBLE))
            continue;
        LTVector q;
        g_pLTServer->GetObjectPos(h, &q);
        if ((q - p).MagSqr() <= 1500.0f * 1500.0f)
            return true;
    }
    return false;
}

// A Demolitions bot with grenades fires one at a target a person would lob it at: fully in view, 300-1200 units
// (the grenade weapon's ranges), then goes back to the rifle (one grenade per switch; the next after the
// grenade weapon's 1.5-3 s pause). A shot the aim check holds for 2 s is given up.
void AltFireUpkeep(Bot& b, CAI* ai)
{
    if (!g_cfg.altFire || b.species != BOT_MARINE || !b.usesGrenades)
        return;
    const LTFLOAT now = g_pLTServer->GetTime();
    const AIWBM_AIWeapon* w = ai->GetCurrentWeaponButesPtr();
    if (IsWeapon(w, kPRGrenade))
    {
        if (now >= b.altBackAt || b.grenades <= 0 || !ai->HasTarget())
        {
            ai->SetWeapon("Pulse_Rifle_MP");
            if (g_cfg.debug)
                Log("ALTSWITCH %s back to Pulse_Rifle_MP (grenades=%d)", b.name.c_str(), b.grenades);
        }
        return;
    }
    if (!IsWeapon(w, "Pulse_Rifle_MP") || b.grenades <= 0 || now < b.altNext || !ai->HasTarget())
        return;
    const CAITarget& tg = ai->GetTarget();
    if (!tg.IsFullyVisible())
        return;
    LTVector tp;
    g_pLTServer->GetObjectPos(tg.GetObject(), &tp);
    const float d = (tp - ai->GetPosition()).Mag();
    const AIWBM_AIWeapon& gw = g_pAIWeaponButeMgr->GetAIWeapon(kPRGrenade);
    if (d < gw.fMinFireRange || d > gw.fMaxFireRange)
        return;
    ai->SetWeapon(kPRGrenade, true);
    b.altBackAt = now + 2.0f;
    if (g_cfg.debug)
        Log("ALTSWITCH %s to %s dist=%.0f grenades=%d", b.name.c_str(), kPRGrenade, d, b.grenades);
}
} // namespace

bool BotMgr_CanOpenAmmoBox(HOBJECT hObj, bool humanBox)
{
    // a player of the box's species opens it by touching or using it, whatever it holds (AmmoBox::SetupOpenState)
    if (!g_cfg.ammoPickups || !hObj || g_bots.empty())
        return false;
    Bot* b = FindBot(hObj);
    CAI* ai = b ? BotAI(*b) : 0;
    if (!ai || ai->IsDead())
        return false;
    return humanBox ? b->species == BOT_MARINE : b->species == BOT_PREDATOR;
}

void BotMgr_AmmoBoxPickup(HOBJECT hObj, const char* szMessage)
{
    Bot* b = FindBot(hObj);
    if (b && szMessage && szMessage[0])  // empty slots of the box (PickUpN "None")
        CreditAmmo(*b, szMessage, "AMMOBOX");
    if (b)
        b->goalBox = (HOBJECT)LTNULL;
}

bool BotMgr_TakeAmmoPickup(HOBJECT hObj, const char* szMessage)
{
    // spears stuck in a wall or a body (Ammo_Spears_Stuck_Wall/Character: "Ammo Spears 1"), PlayerOnly pickups a
    // Predator player with a spear gun takes when he is not full
    if (!g_cfg.ammoPickups || !g_cfg.predAmmo || !hObj || !szMessage || g_bots.empty())
        return false;
    Bot* b = FindBot(hObj);
    if (!b || b->species != BOT_PREDATOR || !b->usesSpears || AmmoInMessage(szMessage, "Spears") <= 0)
        return false;
    if (b->spears >= PoolMax("Spears", 27))
        return false;
    return CreditAmmo(*b, szMessage, "AMMOPICK") > 0;
}

bool BotMgr_AmmoRunWanted(CAI* pAI)
{
    Bot* b = pAI && !g_bots.empty() ? FindBot(pAI->m_hObject) : 0;
    if (!b || b->species != BOT_PREDATOR)
        return false;
    const LTFLOAT now = g_pLTServer->GetTime();
    if (now < b->ammoRunCheck)
        return b->ammoRun;
    b->ammoRunCheck = now + 0.5f;
    const bool was = b->ammoRun;
    b->ammoRun = AmmoRunCheck(*b, pAI);
    if (b->ammoRun != was && g_cfg.debug)
        Log("AMMORUN %s %s spears=%d disc=%s", b->name.c_str(), b->ammoRun ? "starts" : "ends", b->spears,
            b->discOut ? "out" : "in");
    return b->ammoRun;
}

void BotMgr_AmmoRunCancel(CAI* pAI)
{
    // no way to the box: back to the fight, and no new run for 5 s
    Bot* b = pAI ? FindBot(pAI->m_hObject) : 0;
    if (!b || !b->ammoRun)
        return;
    b->ammoRun = false;
    b->ammoRunCheck = g_pLTServer->GetTime() + 5.0f;
    if (g_cfg.debug)
        Log("AMMORUN %s cancelled (no way to a box)", b->name.c_str());
}

bool BotMgr_PickupGoal(CAI* pAI, LTVector* pDest)
{
    if (!pAI || !pDest || !BotMgr_IsBot(pAI->m_hObject))
        return false;
    Bot* bot = FindBot(pAI->m_hObject);
    const LTVector& p = pAI->GetPosition();
    float best = 2500.0f * 2500.0f;
    bool found = false;
    if (SpeciesOf(pAI) == Marine && !g_pickups.empty())
    {
        CDestructible* d = pAI->GetDestructible();
        const bool needHealth = d->GetHitPoints() < 0.6f * d->GetMaxHitPoints();
        const bool needArmor = d->GetMaxArmorPoints() > 0.0f && d->GetArmorPoints() < 0.3f * d->GetMaxArmorPoints();
        for (size_t i = 0; (needHealth || needArmor) && i < g_pickups.size(); ++i)
        {
            if (g_pickups[i].health ? !needHealth : !needArmor)
                continue;
            HOBJECT h = g_pickups[i].hPickup.GetHOBJECT();
            if (!h || !(g_pLTServer->GetObjectFlags(h) & FLAG_VISIBLE))  // taken, waiting to respawn
                continue;
            LTVector q;
            g_pLTServer->GetObjectPos(h, &q);
            const float d2 = (q - p).MagSqr();
            if (d2 < best)
            {
                best = d2;
                *pDest = q;
                found = true;
            }
        }
        if (found)
            return true;
    }
    bool human = false;
    const int need = bot ? AmmoNeeds(*bot, human) : 0;
    if (!need)
        return false;
    if (bot->species == BOT_PREDATOR && bot->ammoRun)
        best = 1500.0f * 1500.0f;  // a run in a fight: only a box close by (BotMgr_AmmoRunWanted)
    HOBJECT pick = LTNULL;
    for (size_t i = 0; i < g_boxes.size(); ++i)
    {
        const AmmoBoxInfo& bi = g_boxes[i];
        if (bi.human != human || !(BoxHolds(bi) & need) || g_pLTServer->GetTime() < bi.bannedUntil)
            continue;
        HOBJECT h = bi.hBox.GetHOBJECT();
        AmmoBox* box = h ? dynamic_cast<AmmoBox*>(g_pLTServer->HandleToObject(h)) : 0;
        if (!box || !BotPickupAccess::BoxReady(box) || !(g_pLTServer->GetObjectFlags(h) & FLAG_VISIBLE))
            continue;  // open, waiting to respawn
        LTVector q;
        g_pLTServer->GetObjectPos(h, &q);
        const float d2 = (q - p).MagSqr();
        if (d2 < best)
        {
            best = d2;
            *pDest = q;
            pick = h;
        }
    }
    bot->goalBox = pick;
    if (pick && g_cfg.debug)
        Log("AMMOGOAL %s box at %.0f %.0f %.0f (%.0f away) spears=%d grenades=%d slugs=%d trackers=%d%s", bot->name.c_str(),
            pDest->x, pDest->y, pDest->z, sqrtf(best), bot->spears, bot->grenades, bot->slugs, bot->trackers,
            bot->ammoRun ? " (run in a fight)" : "");
    return pick != LTNULL;
}

// --- feeding ------------------------------------------------------------------------------------------------
// Alien players heal on bodies: each claw vector that hits a body heals its ClawHealPoints (Marine 1, Predator
// 5), a head bite its HeadBiteHealPoints (25) once (the head explodes), limbs a third of it, while the body can
// still fade and gib, i.e. until its fade-out starts 3 s before its removal (CProjectile::ImpactDamageObject,
// BodyProp::Damage, BodyProp::UpdateNormalDeath). A hurt alien bot with nothing to fight does the same.
namespace {
CVarTrack g_bodyLifetime, g_bodyFadeOff;

// seconds before the body starts fading out (and stops healing)
LTFLOAT BodyHealLeft(const BodyProp* body, LTFLOAT now)
{
    if (!g_bodyLifetime.IsInitted())
        g_bodyLifetime.Init(g_pLTServer, "BodyLifetime", NULL, 30.0f);
    if (!g_bodyFadeOff.IsInitted())
        g_bodyFadeOff.Init(g_pLTServer, "BodyFadeOffTime", NULL, 3.0f);
    return BotBodyAccess::StartTime(body) + g_bodyLifetime.GetFloat() - g_bodyFadeOff.GetFloat() - now;
}

// what a strike on this body would heal this bot (claws: per vector hit); false when nothing
bool BodyHeals(BodyProp* body, const Bot& b, float* claw, float* bite)
{
    *claw = *bite = 0.0f;
    if (!body->GetCanFade() || !body->GetCanGib())
        return false;
    *claw = body->GetClawHealPoints();
    *bite = (b.kitBite && body->IsHeadAvailable()) ? body->GetHeadBiteHealPoints() : 0.0f;
    if (*bite > 0.0f && g_pGameServerShell->LowViolence())
        *bite = *claw;   // with low violence a bite heals what the claws do
    return *claw > 0.0f || *bite > 0.0f;
}

LTVector BodyPoint(BodyProp* body, const char* node)
{
    LTVector v;
    g_pLTServer->GetObjectPos(body->m_hObject, &v);
    const ModelNode n = g_pModelButeMgr->GetSkeletonNode(body->GetModelSkeleton(), node);
    if (n != eModelNodeInvalid)
        body->GetNodePos(n, v, LTTRUE);
    return v;
}

bool FeedBad(const Bot& b, HOBJECT h)
{
    for (int i = 0; i < 4; ++i)
        if (b.feedBad[i] == h)
            return true;
    return false;
}

void FeedFinish(Bot& b, CAI* ai, const char* why)
{
    if (!b.feeding && !b.feedBody.GetHOBJECT() && b.feedSince == 0.0f)
        return;
    const LTFLOAT now = g_pLTServer->GetTime();
    if (g_cfg.debug)
        Log("FEEDEND %s class=%s reason=%s %s strikes=%d healed=%.0f hp=%.0f/%.0f time=%.1f", b.name.c_str(), ClassName(b),
            why, b.feeding ? "ate" : "walked", b.feedStrikes, b.feedGained, ai ? ai->GetHitPoints() : 0.0f,
            ai ? ai->GetDestructible()->GetMaxHitPoints() : 0.0f, b.feedSince > 0.0f ? now - b.feedSince : 0.0f);
    if (b.feeding && ai)
        SetWeaponAnim(ai, 0);
    b.feeding = false;
    b.feedBody = (HOBJECT)LTNULL;
    b.feedSince = 0.0f;
    b.feedBiteAt = 0.0f;
    b.feedStrikes = b.feedMisses = 0;
    b.feedGained = 0.0f;
}
} // namespace

namespace {
// The best body for a hurt alien bot: the nearest ones (within the pickup goal's 2500 units) that heal it, checked
// in order on the waypoint graph: walking time along the path at the class's run speed, plus one claw animation,
// must end before the body starts fading. Fills the bot's pending body; false when none qualifies.
bool FeedScan(Bot& b, CAI* ai, LTFLOAT now)
{
    b.feedPending = (HOBJECT)LTNULL;
    const LTVector& p = ai->GetPosition();
    std::vector<std::pair<float, HOBJECT> > cands;
    const HCLASS hBodyClass = g_pLTServer->GetClass("BodyProp");
    for (int pass = 0; pass < 2; ++pass)
    {
        HOBJECT h = LTNULL;
        while ((h = pass == 0 ? g_pLTServer->GetNextObject(h) : g_pLTServer->GetNextInactiveObject(h)) != LTNULL)
        {
            if (!g_pLTServer->IsKindOf(g_pLTServer->GetObjectClass(h), hBodyClass))
                continue;
            BodyProp* body = dynamic_cast<BodyProp*>(g_pLTServer->HandleToObject(h));
            float claw, bite;
            if (!body || BotBodyAccess::Limb(body) || FeedBad(b, h) || !BodyHeals(body, b, &claw, &bite))
                continue;
            LTVector q;
            g_pLTServer->GetObjectPos(h, &q);
            const float dist = (q - p).Mag();
            if (dist <= 2500.0f)
                cands.push_back(std::make_pair(dist, h));
        }
    }
    std::sort(cands.begin(), cands.end());
    const float run = g_pCharacterButeMgr->GetRunSpeed(ai->GetCharacter());
    const LTVector& dims = ai->GetDims();
    const int from = g_nav.Nearest(Vec3(p.x, p.y - dims.y, p.z));
    for (size_t i = 0; i < cands.size() && i < 3 && from >= 0; ++i)
    {
        BodyProp* body = dynamic_cast<BodyProp*>(g_pLTServer->HandleToObject(cands[i].second));
        if (!body)
            continue;
        // the floor under the body
        LTVector q;
        g_pLTServer->GetObjectPos(body->m_hObject, &q);
        IntersectQuery iq;
        IntersectInfo ii;
        iq.m_From = q;
        iq.m_To = LTVector(q.x, q.y - 200.0f, q.z);
        iq.m_Flags = IGNORE_NONSOLID;
        if (g_pLTServer->IntersectSegment(&iq, &ii))
            q.y = ii.m_Point.y;
        const int to = g_nav.Nearest(Vec3(q.x, q.y, q.z));
        std::vector<Vec3> pts;
        if (to < 0 || !g_nav.FindPath(from, to, &pts, 0, 0, 0, 0, g_cfg.wallwalk))
            continue;
        float len = 0.0f;
        Vec3 prev(p.x, p.y - dims.y, p.z);
        for (size_t k = 0; k < pts.size(); ++k)
        {
            len += avp2bots::Dist(prev, pts[k]);
            prev = pts[k];
        }
        if (run > 0.0f && len / run + kClawLoop > BodyHealLeft(body, now))
            continue;
        b.feedPending = body->m_hObject;
        b.feedPendingDest = q;
        b.feedPendingPath = len;
        return true;
    }
    return false;
}
} // namespace

bool BotMgr_FeedWanted(CAI* pAI)
{
    // hurt (under 70 % of its health), nothing to fight, and a body worth it found (a search at most once a second)
    if (!g_cfg.feed || !pAI || !g_navLoaded || pAI->HasTarget())
        return false;
    Bot* b = FindBot(pAI->m_hObject);
    if (!b || b->species != BOT_ALIEN || b->stage != 0 || !b->kitClaws)
        return false;
    CDestructible* d = pAI->GetDestructible();
    if (d->GetHitPoints() >= 0.7f * d->GetMaxHitPoints())
        return false;
    const LTFLOAT now = g_pLTServer->GetTime();
    BodyProp* pending = b->feedPending.GetHOBJECT() ?
        dynamic_cast<BodyProp*>(g_pLTServer->HandleToObject(b->feedPending.GetHOBJECT())) : LTNULL;
    float claw, bite;
    if (pending && BodyHeals(pending, *b, &claw, &bite))
        return true;
    if (now < b->feedLookAt)
        return false;
    b->feedLookAt = now + 1.0f;
    return FeedScan(*b, pAI, now);
}

bool BotMgr_FeedGoal(CAI* pAI, LTVector* pDest, HOBJECT* phBody)
{
    if (!pDest || !phBody || !BotMgr_FeedWanted(pAI))
        return false;
    Bot* b = FindBot(pAI->m_hObject);
    BodyProp* body = b ? dynamic_cast<BodyProp*>(g_pLTServer->HandleToObject(b->feedPending.GetHOBJECT())) : LTNULL;
    if (!body)
        return false;
    *pDest = b->feedPendingDest;
    *phBody = body->m_hObject;
    // claim it as the single-player Investigate goal does (AIInvestigate.cpp): out of the AI's dead-body list,
    // the AI senses stop reacting to it (the bot's own "investigate the body" would interrupt the meal)
    g_pCharacterMgr->RemoveDeadBody(body);
    FeedFinish(*b, pAI, "new");
    b->feedBody = body->m_hObject;
    b->feedPending = (HOBJECT)LTNULL;
    b->feedSince = 0.0f;
    if (g_cfg.debug)
    {
        CDestructible* d = pAI->GetDestructible();
        Log("FEEDGOAL %s class=%s body=%s (%s) path=%.0f left=%.1f hp=%.0f/%.0f", b->name.c_str(), ClassName(*b),
            ObjectLabel(body->m_hObject).c_str(), g_pCharacterButeMgr->GetModelType(body->GetCharacter()),
            b->feedPendingPath, BodyHealLeft(body, g_pLTServer->GetTime()), d->GetHitPoints(), d->GetMaxHitPoints());
    }
    return true;
}

int BotMgr_Feed(CAI* pAI, HOBJECT hBody, bool walkDone)
{
    Bot* b = pAI ? FindBot(pAI->m_hObject) : 0;
    if (!b)
        return BOTFEED_DONE;
    const LTFLOAT now = g_pLTServer->GetTime();
    BodyProp* body = hBody ? dynamic_cast<BodyProp*>(g_pLTServer->HandleToObject(hBody)) : LTNULL;
    CDestructible* d = pAI->GetDestructible();
    float claw = 0.0f, bite = 0.0f;
    const char* why = 0;
    if (!g_cfg.feed)
        why = "off";
    else if (!body)
        why = "gone";
    else if (!BodyHeals(body, *b, &claw, &bite))
        why = "fade";
    else if (pAI->HasTarget())
        why = "target";
    else if (d->GetHitPoints() >= d->GetMaxHitPoints())
        why = "full";
    else if (b->feedMisses >= 6)   // three claw animations without healing: the strikes cannot reach it
        why = "missed";
    if (why)
    {
        if (!strcmp(why, "missed") && hBody)
            b->feedBad[(b->feedBadNext++) & 3] = hBody;
        FeedFinish(*b, pAI, why);
        return BOTFEED_DONE;
    }

    const LTVector& pos = pAI->GetPosition();
    const LTVector torso = BodyPoint(body, "Torso_u_node");
    const LTVector to = torso - pos;
    if (!b->feeding)
    {
        // close enough: horizontally within the AI claws' minimum ideal range (Melee_AIWeapon IdealRange 64), or
        // at the end of the path within the claws' reach with nothing in between
        const float flat = sqrtf(to.x * to.x + to.z * to.z);
        const bool reach = to.Mag() <= (float)b->kitClawsBarrel->nRange && ClearLine(pos, torso);
        if (!(flat <= 64.0f && reach) && !(walkDone && reach))
        {
            if (!walkDone)
                return BOTFEED_WALK;
            b->feedBad[(b->feedBadNext++) & 3] = hBody;
            FeedFinish(*b, pAI, "unreachable");
            return BOTFEED_DONE;
        }
        b->feeding = true;
        b->feedSince = now;
        b->feedStrikes = b->feedMisses = 0;
        b->feedGained = 0.0f;
        b->feedKey = 0;
        b->feedNext = now;
        b->feedBiteAt = bite > 0.0f ? now + kBiteWindup : 0.0f;   // a head first (Drone, Runner), as players do
        pAI->SetNextAction(CAI::ACTION_IDLE);
        pAI->Stop();
    }

    // standing at the body, facing it, striking on the players' rhythm
    pAI->SnapTurn(to);
    if (b->feedBiteAt > 0.0f)
    {
        SetWeaponAnim(pAI, "Head_Bite");
        if (now < b->feedBiteAt)
            return BOTFEED_EAT;
        const LTVector head = BodyPoint(body, "Head_node");
        const float healed = Strike(pAI, b->kitBite, b->kitBiteBarrel, hBody, head, "Head_Bite");
        b->feedBiteAt = 0.0f;
        b->feedNext = now + (kBiteLoop - kBiteWindup);   // the rest of the bite animation
        b->feedStrikes++;
        b->feedGained += healed;
        b->feedMisses = healed > 0.0f ? 0 : b->feedMisses + 1;
        if (g_cfg.debug)
            Log("FEED %s class=%s body=%s weapon=Head_Bite heal=%.0f hp=%.0f/%.0f", b->name.c_str(), ClassName(*b),
                ObjectLabel(hBody).c_str(), healed, pAI->GetHitPoints(), d->GetMaxHitPoints());
        return BOTFEED_EAT;
    }
    SetWeaponAnim(pAI, "Claws");
    if (now >= b->feedNext && claw > 0.0f)
    {
        const float healed = Strike(pAI, b->kitClaws, b->kitClawsBarrel, hBody, torso, b->kitClaws->szName);
        b->feedNext = now + (b->feedKey == 0 ? kClawKey2 : kClawLoop - kClawKey2);
        b->feedKey ^= 1;
        b->feedStrikes++;
        b->feedGained += healed;
        b->feedMisses = healed > 0.0f ? 0 : b->feedMisses + 1;
        if (g_cfg.debug)
            Log("FEED %s class=%s body=%s weapon=%s heal=%.0f hp=%.0f/%.0f", b->name.c_str(), ClassName(*b),
                ObjectLabel(hBody).c_str(), b->kitClaws->szName, healed, pAI->GetHitPoints(), d->GetMaxHitPoints());
    }
    else if (claw <= 0.0f)
    {
        FeedFinish(*b, pAI, "empty");   // an exosuit after its bite: the claws heal nothing
        return BOTFEED_DONE;
    }
    return BOTFEED_EAT;
}

void BotMgr_FeedEnd(CAI* pAI, bool unreachable)
{
    Bot* b = pAI ? FindBot(pAI->m_hObject) : 0;
    if (!b)
        return;
    if (unreachable && b->feedBody.GetHOBJECT())
        b->feedBad[(b->feedBadNext++) & 3] = b->feedBody.GetHOBJECT();
    FeedFinish(*b, pAI, unreachable ? "nopath" : pAI->HasTarget() ? "target" : "goal");
}

void BotMgr_PreStartWorld()
{
    for (size_t i = 0; i < g_bots.size(); ++i)
        ScoreRowRemove(g_bots[i]);
    g_pathFails.clear();
    g_lastCannon.clear();
    g_stuckSpots.clear();
    g_aimDrift.clear();
    g_lastShot.clear();
    g_doors.clear();
    // the previous world's objects are gone with it
    g_bots.clear();
    g_nav.Clear();
    g_navLoaded = false;
}

void BotMgr_PostStartWorld()
{
    g_bots.clear();
    LoadConfig();
    if (g_cfg.log && !g_log)
        g_log = fopen("avp2bots_bots.log", "a");
    Msp_LoadKey(); // the MSP 2.4 scrambling constant, from lithserver.rez (once; logs MSPKEY)

    g_map = CurrentMapName();
    if (!GameTypeAllowed())
    {
        Log("WORLD map=%s game type %s: no bots (botnav\\bots.cfg: deathmatch, teamdeathmatch)", g_map.c_str(),
            GameTypeName(GameTypeNow()));
        return;
    }
    std::string path = "botnav\\" + g_map + ".nav";
    std::string err;
    g_navLoaded = !g_map.empty() && g_nav.Load(path.c_str(), &err);
    Log("WORLD map=%s nav=%s %s", g_map.c_str(), g_navLoaded ? "loaded" : "missing", g_navLoaded ? "" : err.c_str());
    if (!g_navLoaded)
        return;
    Log("NAV nodes=%d links=%d", g_nav.NodeCount(), g_nav.LinkCount());
    LogPounceReach();
    g_nav.UseLadders(g_cfg.ladders);
    ScanObstacles();
    FindDoors();
    FindPickups();
    BuildAmbushData();

    const LTFLOAT now = g_pLTServer->GetTime();
    g_lastHumans = -1;
    g_nextFill = now + 2.0f;
    if (g_cfg.fill > 0)
        return;  // BotMgr_Update adds them, counting the players
    const int survSp = SurvivorMode() ? RaceSpecies(SurvivorRace(false)) : -1;
    for (int s = 0; s < BOT_SPECIES_COUNT; ++s)
        for (int i = 0; i < g_cfg.count[s]; ++i)  // stagger the first spawns; in Survivor everyone starts as a survivor
        {
            if (HuntMode())
            {
                AddHuntBot(now + 2.0f + 0.5f * (float)g_bots.size());
                continue;
            }
            AddBot(survSp >= 0 ? (BotSpecies)survSp : (BotSpecies)s, 0, now + 2.0f + 0.5f * (float)g_bots.size());
        }
}

void BotMgr_Update()
{
    ServerExt_Update(); // server extensions, every frame, before any early return
    const LTFLOAT now = g_pLTServer->GetTime();
    // the command file works with no bots and on maps without a waypoint file (commands then say so)
    // commands dropped into botnav\command.txt (one per line) by scripts or the admin
    if (now >= g_nextCommandPoll)
    {
        g_nextCommandPoll = now + 1.0f;
        FILE* cf = fopen("botnav\\command.txt", "r");
        if (cf)
        {
            char line[256];
            std::vector<std::string> lines;
            while (fgets(line, sizeof(line), cf))
            {
                line[strcspn(line, "\r\n")] = 0;
                if (line[0] && line[0] != '#')
                    lines.push_back(line);
            }
            fclose(cf);
            remove("botnav\\command.txt");
            for (size_t i = 0; i < lines.size(); ++i)
            {
                char reply[512];
                bool ok = BotMgr_Command(lines[i].c_str(), reply, sizeof(reply));
                Log("COMMAND \"%s\" -> %s", lines[i].c_str(), ok ? reply : "not a bot command");
            }
        }
    }

    if (!g_navLoaded)
        return;
    if (g_cfg.fill > 0 && now >= g_nextFill)
    {
        ApplyFill();
        g_nextFill = now + 2.0f;
    }
    if (g_bots.empty())
        return;

    for (size_t i = 0; i < g_bots.size(); ++i)
    {
        Bot& b = g_bots[i];
        if (b.alive)
        {
            CAI* ai = BotAI(b);
            if (!ai || ai->IsDead())
            {
                HOBJECT killer = ai ? ai->GetDestructible()->GetLastDamager() : LTNULL;
                b.alive = false;
                b.deaths++;
                b.respawnTime = now + g_cfg.respawnDelay;
                b.stage = 0;      // any death: back to a facehugger (lifecycle) or the bot's class
                b.lifeClass = 0;
                b.placeAt = false;
                {
                    // the killer's weapon at that moment (for balance analysis)
                    const char* kw = KillWeaponLabel(killer, ai ? (int)ai->GetDestructible()->GetDeathType() : -1);
                    const Bot* kb = killer ? FindBot(killer) : 0;
                    if (kb && kb->specialName && ai && kb->specialVictim == ai->m_hObject && now - kb->specialTime < 0.5f)
                        kw = kb->specialName;   // a head bite or tail strike (BotMgr's, not the AI weapon)
                    Log("DEATH %s killer=%s lifetime=%.1f weapon=%s", b.name.c_str(), ObjectLabel(killer).c_str(),
                        now - b.spawnTime, kw);
                    if (g_cfg.debug && ai && killer && killer != ai->m_hObject)
                    {
                        // where the killer stood relative to the victim's facing (0 = in front, 180 = behind)
                        LTVector kp, vp, fwd = ai->GetForward();
                        g_pLTServer->GetObjectPos(killer, &kp);
                        g_pLTServer->GetObjectPos(ai->m_hObject, &vp);
                        LTVector to = kp - vp;
                        to.y = 0.0f;
                        fwd.y = 0.0f;
                        const float dist = to.Mag();
                        float off = -1.0f;
                        if (dist > 1.0f && fwd.Mag() > 0.01f)
                        {
                            float c = fwd.Dot(to) / (dist * fwd.Mag());
                            c = c < -1.0f ? -1.0f : (c > 1.0f ? 1.0f : c);
                            off = acosf(c) * 57.29578f;
                        }
                        Log("DEATHDIR %s killer=%s off=%.0f dist=%.0f", b.name.c_str(), ObjectLabel(killer).c_str(), off, dist);
                    }
                }
                BotScoreAccess::BotKilled(b, killer);
                FeedFinish(b, LTNULL, "death");
                b.tailSince = b.biteAt = 0.0f;
                continue;
            }
            // an AI whose CharacterType and AttributeTemplate disagree fails CAI::Consistent() and is left
            // without goals (the final build does not remove it): drop it and spawn again
            if (b.stage == 1)
            {
                // a facehugger on a face is frozen for ~15 s until the chestburst (BotMgr_OnChestburst)
                const bool hugging = (g_pLTServer->GetObjectUserFlags(ai->m_hObject) & USRFLG_CHAR_FACEHUG) != 0;
                if (hugging)
                {
                    if (b.hugSince == 0.0f)
                        b.hugSince = now;
                    // a hugging player is a client right at the body, which keeps it updated; a bot is not
                    if (g_cfg.hugBody && b.hugBody.GetHOBJECT())
                        g_pLTServer->SetObjectState(b.hugBody.GetHOBJECT(), OBJSTATE_ACTIVE);
                    b.stuckCount = 0;
                    b.offGraph = 0;
                    b.checkPos = ai->GetPosition();
                    b.checkTime = now;
                    if (now - b.hugSince > 30.0f)
                    {
                        // no chestburst came (last resort): burst from the body if it is still there
                        LTRotation r;
                        LTVector at = ai->GetPosition();
                        g_pLTServer->GetObjectRotation(ai->m_hObject, &r);
                        const HOBJECT hb = b.hugBody.GetHOBJECT();
                        if (hb)
                            g_pLTServer->GetObjectPos(hb, &at);
                        Log("LIFECYCLE %s: no chestburst after 30 s, bursting here (body %s, host %s)", b.name.c_str(),
                            hb ? ((g_pLTServer->GetObjectState(hb) & OBJSTATE_INACTIVE) ? "asleep" : "awake") : "gone",
                            b.hugHost >= 0 ? g_pCharacterButeMgr->GetModelType(b.hugHost) : "unknown");
                        LifecycleBurst(b, at, r, b.hugHost, true);
                        continue;
                    }
                }
                else if (ai->GetMovement()->GetMovementState() == CMS_FACEHUG_ATTACH)
                {
                    // attached but the victim survived: the movement never leaves this state by itself
                    if (b.loseSince == 0.0f)
                        b.loseSince = now;
                    else if (now - b.loseSince > 1.5f)
                    {
                        Log("UNSTICK %s: facehug failed, respawning", b.name.c_str());
                        g_pLTServer->RemoveObject(ai->m_hObject);
                        b.alive = false;
                        b.respawnTime = now + g_cfg.respawnDelay;
                        continue;
                    }
                }
                else
                    b.loseSince = 0.0f;
            }
            else if (b.stage == 2 && (b.growInto >= 0 || ai->GetCharacter() != g_pCharacterButeMgr->GetSetFromModelType("Chestburster_MP")))
            {
                // the engine grew the chestburster (UpdateChestbursterCycle; hook 'botgrow' hands the adult's set
                // here instead of switching the AI): the adult takes over in place
                const char* grown = g_pCharacterButeMgr->GetModelType(b.growInto >= 0 ? b.growInto : ai->GetCharacter());
                b.growInto = -1;
                const BotClass* adult = 0;
                for (int c = 0; c < kClassCount && !adult; ++c)
                    if (kClasses[c].species == BOT_ALIEN && grown && !_stricmp(kClasses[c].mpSet, grown))
                        adult = &kClasses[c];
                b.lifeClass = adult ? adult : ClassFor(BOT_ALIEN, b.slot);
                b.stage = 0;
                b.placeAt = true;
                b.placePos = ai->GetPosition();
                g_pLTServer->GetObjectRotation(ai->m_hObject, &b.placeRot);
                g_pLTServer->RemoveObject(ai->m_hObject);
                b.alive = false;
                b.respawnTime = now;
                Log("LIFECYCLE %s grows into a %s", b.name.c_str(), b.lifeClass ? b.lifeClass->name : "?");
                continue;
            }
            EnergyUpkeep(b, ai);
            VisionUpkeep(b, ai);
            AmmoUpkeep(b, ai);
            AltFireUpkeep(b, ai);
            SadarLockUpkeep(b, ai);
            DiscUpkeep(b, ai);
            PredWeaponUpkeep(b, ai);
            AnimWatchdog(b, ai);
            TrackerUpkeep(b, ai);
            HearUpkeep(b, ai);
            // a meal whose body was removed (gibbed) or whose goal ended without saying so
            if (b.feeding && !b.feedBody.GetHOBJECT())
                FeedFinish(b, ai, "gone");
            else if (b.feeding && _stricmp(ai->GetCurrentSlotName().c_str(), "BotRoam"))
                FeedFinish(b, ai, "goal");
            AlienKitUpdate(b, ai, now);
            HugHunt(b, ai, now);
            {
                // debug: every pounce a bot starts, under either rule (POUNCESTART; hits are POUNCESTUN lines)
                const CharacterMovementState pcms = ai->GetMovement()->GetMovementState();
                const bool pouncing = pcms == CMS_POUNCING || pcms == CMS_FACEHUG;
                if (!pouncing && b.pouncing && b.stage == 1)
                    b.pounceTouched = pcms == CMS_FACEHUG_ATTACH ? std::string("hug") : std::string();
                if (!pouncing && b.pouncing && g_cfg.debug)
                {
                    // how it ended: what it touched first (nothing: the flight ended short of anything)
                    HOBJECT ht = ai->HasTarget() ? ai->GetTarget().GetObject().GetHOBJECT() : LTNULL;
                    LTVector tp(0.0f, 0.0f, 0.0f);
                    if (ht)
                        g_pLTServer->GetObjectPos(ht, &tp);
                    Log("POUNCEEND %s class=%s touched=%s target_dist=%.0f", b.name.c_str(), ClassName(b),
                        b.pounceTouched.empty() ? "nothing" : b.pounceTouched.c_str(), ht ? (tp - ai->GetPosition()).Mag() : -1.0f);
                }
                if (pouncing && !b.pouncing)
                    b.pounceTouched.clear();
                if (pouncing && !b.pouncing && g_cfg.debug)
                {
                    const LTVector dv = ai->GetMovement()->GetCharacterVars()->m_vDestination - ai->GetPosition();
                    Log("POUNCESTART %s class=%s len=%.0f rule=%s", b.name.c_str(), ClassName(b), dv.Mag(),
                        BotMgr_PounceRule(ai) ? "player" : "ai");
                }
                b.pouncing = pouncing;
            }
            if (b.goalsSent && now - b.spawnTime > 3.0f && ai->GetCurrentSlotName().empty())
            {
                Log("BROKEN %s has no goal 3 s after spawning; removed", b.name.c_str());
                g_pLTServer->RemoveObject(ai->m_hObject);
                b.alive = false;
                b.respawnTime = now + g_cfg.respawnDelay;
                continue;
            }
            if (b.switchAt > 0.0f && now >= b.switchAt)
            {
                // a role swap of a live bot (Hunt): respawn as the new race, as MPRespawn does to a player
                g_pLTServer->RemoveObject(ai->m_hObject);
                b.alive = false;
                b.survTeam = b.pendingRole;
                const int sp = RaceSpecies(HuntRace(b.survTeam));
                if (sp >= 0)
                    b.species = (BotSpecies)sp;
                b.forcedClass = 0;
                b.pendingRole = -1;
                b.switchAt = 0.0f;
                b.respawnTime = now;
                ScoreRowRemove(b);
                Log("HUNT %s respawns as %s", b.name.c_str(), b.survTeam ? "prey" : "hunter");
                continue;
            }
            // isolated: every path from where the bot stands fails (a ledge or a corner of the graph cut off from
            // the rest, reached by a fall or a jump); a player would walk or drop off, the bot just waits there
            {
                std::map<HOBJECT, PathFail>::iterator pf = g_pathFails.find(ai->m_hObject);
                if (pf != g_pathFails.end() && pf->second.streak >= 5)
                {
                    Log("UNSTICK %s removed after %d failed paths from waypoint %d; respawning", b.name.c_str(),
                        pf->second.streak, pf->second.from);
                    g_pathFails.erase(pf);
                    g_pLTServer->RemoveObject(ai->m_hObject);
                    b.alive = false;
                    b.offGraph = 0;
                    b.stuckCount = 0;
                    b.respawnTime = now + g_cfg.respawnDelay;
                    continue;
                }
            }
            if (b.offGraph >= 3)
            {
                Log("UNSTICK %s removed after %d reports off the waypoint graph; respawning", b.name.c_str(), b.offGraph);
                g_pLTServer->RemoveObject(ai->m_hObject);
                b.alive = false;
                b.offGraph = 0;
                b.stuckCount = 0;
                b.respawnTime = now + g_cfg.respawnDelay;
                continue;
            }
            if (b.stuckCount >= (b.stage == 1 ? 2 : 3))  // a stuck facehugger lives short anyway
            {
                Log("UNSTICK %s removed after %d stuck reports; respawning", b.name.c_str(), b.stuckCount);
                if (!ai->IsDead() && g_nav.IsLoaded())
                {
                    LTVector feet = ai->GetPosition();
                    feet.y -= ai->GetDims().y;
                    std::string slot = b.name.substr(0, b.name.rfind('_'));  // avp2bot_Marine2: the same bot
                    bool done = false;
                    for (size_t k = 0; k < g_stuckSpots.size() && !done; ++k)
                    {
                        StuckSpot& sp = g_stuckSpots[k];
                        if (sp.blocked || sp.bot == slot || (sp.feet - feet).MagSqr() > 48.0f * 48.0f)
                            continue;
                        const LTVector c = (sp.feet + feet) * 0.5f;
                        int links = 0;
                        // the trap is often the start of a link the bot cannot take (a drop behind a railing): it
                        // stands up to ~40 units past the link's own line, so the box reaches 48 units around
                        const int n = g_nav.BlockBox(Vec3(c.x - 48.0f, c.y - 48.0f, c.z - 48.0f),
                                                     Vec3(c.x + 48.0f, c.y + 48.0f, c.z + 48.0f), &links);
                        sp.blocked = true;
                        done = true;
                        g_pathFails.clear();
                        Log("LEARNBLOCK %.0f %.0f %.0f: two bots stuck here (%s, %s); %d waypoints and %d links blocked for this map",
                            c.x, c.y, c.z, sp.bot.c_str(), slot.c_str(), n, links);
                    }
                    if (!done)
                    {
                        StuckSpot sp = {feet, slot, false};
                        g_stuckSpots.push_back(sp);
                    }
                }
                g_pLTServer->RemoveObject(ai->m_hObject);
                b.alive = false;
                b.stuckCount = 0;
                b.respawnTime = now + g_cfg.respawnDelay;
                continue;
            }
            if (!b.goalsSent && now - b.spawnTime > 0.25f)
            {
                if (b.stage == 2 && g_cfg.cbHide)
                    SendCommand(ai->m_hObject, "AG BotRoam");  // hides until it grows (BotMgr_CbHide)
                else
                {
                    SendCommand(ai->m_hObject, "AG Attack");
                    SendCommand(ai->m_hObject, "AG Investigate");
                    SendCommand(ai->m_hObject, "AG BotRoam");
                }
                if (g_cfg.tracker && b.species == BOT_MARINE)
                    SendCommand(ai->m_hObject, "AG BotTracker");   // the motion tracker's reaction (BotTrackerGoal)
                // players run (the default); the AI's Investigate goal walked toward what it heard. With the
                // run lock CAI::Walk() does nothing (AI.h), so the bot always runs.
                if (g_cfg.alwaysRun)
                    SendCommand(ai->m_hObject, "RUNLOCK on");
                b.goalsSent = true;
            }
            g_pLTServer->SetObjectState(ai->m_hObject, OBJSTATE_ACTIVE);
        }
        else if (now >= b.respawnTime)
        {
            Spawn(b);
        }
    }

    if (now >= g_nextDoors)
    {
        UpdateDoors(now);
        g_nextDoors = now + 0.25f;
    }
    if (g_cfg.wallAmbush)
        TrafficTick(now);
    if (SurvivorMode())
        SurvivorTick(now);
    if (now >= g_nextRows)
    {
        BotScoreAccess::CheckRows();
        g_nextRows = now + 1.0f;
    }
    if (now >= g_nextRelations)
    {
        UpdateRelations();
        g_nextRelations = now + 1.0f;
    }
    if (now >= g_nextStatus)
    {
        for (size_t i = 0; i < g_bots.size(); ++i)
            if (g_bots[i].alive)
                LogStatus(g_bots[i], now);
        if (now >= g_nextTeamLog)
        {
            BotScoreAccess::LogTeams();
            if (g_playerHits[0] || g_playerHits[1])
                Log("PLAYERHITS players' hits on bots so far: %d applied, %d refused", g_playerHits[0], g_playerHits[1]);
            if (g_cfg.debug)
            {
                LogAmbushStats();
                Log("SMALLSIGHT slowed sight updates: hugger/burster=%d other small=%d crouching=%d", g_smallSight[0],
                    g_smallSight[1], g_smallSight[2]);
            }
            g_nextTeamLog = now + 20.0f;
        }
        g_nextStatus = now + 2.0f;
    }
}

bool BotMgr_TeamDamageBlocked(HOBJECT hVictim, HOBJECT hDamager)
{
    // Called only when friendly fire is off. The game filters damage between two players of the same team;
    // apply the same rule when a bot is the victim or the attacker.
    if (!hVictim || !hDamager || hVictim == hDamager || g_bots.empty())
        return false;
    CCharacter* victim = dynamic_cast<CCharacter*>(g_pLTServer->HandleToObject(hVictim));
    CCharacter* damager = dynamic_cast<CCharacter*>(g_pLTServer->HandleToObject(hDamager));
    if (!victim || !damager)
        return false;
    if (!dynamic_cast<CAI*>(victim) && !dynamic_cast<CAI*>(damager))
        return false;  // two players: the game's own check handles it
    const int a = SpeciesOf(victim);
    return a >= 0 && a == SpeciesOf(damager) && GameTypeNow() != MP_DM;
}

namespace {
float Gauss()
{
    float u = GetRandom(0.0001f, 1.0f), v = GetRandom(0.0f, 6.2832f);
    return sqrtf(-2.0f * logf(u)) * cosf(v);
}
}

int BotMgr_StartPointDist(const LTVector& vPos, LTFLOAT& fFarDist, LTFLOAT& fNearDist)
{
    if (g_spawningSpecies >= 0 && g_nav.IsLoaded() &&
        g_nav.IsBadSpawn(Vec3(vPos.x, vPos.y, vPos.z), g_spawningSpecies == BOT_ALIEN))
        fNearDist = -1.0f;
    int n = 0;
    for (size_t i = 0; i < g_bots.size(); ++i)
    {
        if (!g_bots[i].alive)
            continue;
        CAI* ai = BotAI(g_bots[i]);
        if (!ai || ai->IsDead())
            continue;
        const float d = (ai->GetPosition() - vPos).Mag();
        ++n;
        fFarDist += d;
        if (fNearDist >= 0.0f && d < fNearDist)
            fNearDist = d;
    }
    return n;
}

LTFLOAT BotMgr_PounceDamage(HOBJECT hAI, LTFLOAT def)
{
    CAI* ai = dynamic_cast<CAI*>(g_pLTServer->HandleToObject(hAI));
    if (!ai)
        return def;
    const uint32 dmg = g_pCharacterButeMgr->GetPounceDamage(ai->GetCharacter());
    if (g_cfg.debug)
        Log("POUNCEHIT %s class damage %u (AI %.0f)", ObjectLabel(hAI).c_str(), dmg, def);
    return dmg > 0 ? (LTFLOAT)dmg * g_cfg.damageScale : def;
}

namespace {
CVarTrack g_pounceStunMod;   // the players' "PounceStunMod" server variable (PlayerObj.cpp, default 33)
}

void BotMgr_PounceHit(CCharacter* pBot, HOBJECT hTouched, DamageStruct& damage)
{
    // CPlayerObj::OnMessage, CP_POUNCE_DAMAGE (the server side of a player's pounce), for a bot
    if (!pBot || pBot->IsDead() || !hTouched)
        return;
    HOBJECT hVictim = hTouched;
    CCharacter* pChar = dynamic_cast<CCharacter*>(g_pLTServer->HandleToObject(hTouched));
    CCharacterHitBox* pHitBox = pChar ? dynamic_cast<CCharacterHitBox*>(g_pLTServer->HandleToObject(pChar->GetHitBox()))
                                      : dynamic_cast<CCharacterHitBox*>(g_pLTServer->HandleToObject(hTouched));
    if (pHitBox)
    {
        if (!pChar)
            pChar = dynamic_cast<CCharacter*>(g_pLTServer->HandleToObject(pHitBox->GetModelObject()));
        if (pChar)
        {
            hVictim = pChar->m_hObject;
            pChar->SetModelNodeLastHit(g_pModelButeMgr->GetSkeletonNode(pChar->GetModelSkeleton(), "Torso_u_node"));
        }
    }
    const LTFLOAT now = g_pLTServer->GetTime();
    Bot* b = FindBot(pBot->m_hObject);
    if (b && b->pounceTouched.empty())
        b->pounceTouched = pChar ? ObjectLabel(hVictim) : std::string("world:") + ObjectLabel(hVictim);

    // the instant damage (the class's PounceWeaponDamage, see BotMgr_PounceDamage)
    damage.hDamager = pBot->m_hObject;
    g_strikeShooter = pBot->m_hObject;
    g_strikeName = "Pounce";
    damage.DoDamage(pBot, hVictim);

    // the progressive stun: duration = damage / PounceStunMod; the victim's BaseStunChance (Marines and Predators
    // 100, aliens, exosuits and synthetics 0) and its health decide whether it holds, and for how long between
    // half and all of it (CDestructible::StunTest)
    const LTFLOAT dealt = damage.fDamage;
    float stunned = -1.0f;
    if (g_cfg.pounceStun)
    {
        if (!g_pounceStunMod.IsInitted())
            g_pounceStunMod.Init(g_pLTServer, "PounceStunMod", NULL, 33.0f);
        const float mod = g_pounceStunMod.GetFloat() > 0.0f ? g_pounceStunMod.GetFloat() : 33.0f;
        damage.fDuration = damage.fDamage / mod;
        damage.eType = DT_STUN;
        damage.fDamage = 0.0f;
        damage.DoDamage(pBot, hVictim);
        // what the victim kept (debug log): its DT_STUN progressive damage
        if (pChar && pChar->GetDestructible())
            for (int i = 0; i < MAX_PROGRESSIVE_DAMAGE; ++i)
            {
                const DamageStruct pd = pChar->GetDestructible()->GetProgressiveDamage(i);
                if (pd.eType == DT_STUN && pd.fDuration > 0.0f)
                    stunned = pd.fDuration;
            }
    }
    g_strikeShooter = LTNULL;
    g_strikeName = 0;

    // fire info, so the AI that hears weapon fire hears the pounce (as for a player)
    CharFireInfo info;
    g_pLTServer->GetObjectPos(pBot->m_hObject, &info.vFiredPos);
    info.vFiredDir = damage.vDir;
    if (WEAPON* w = g_pWeaponMgr->GetWeapon("Pounce"))
        info.nWeaponId = w->nId;
    if (BARREL* br = g_pWeaponMgr->GetBarrel("Pounce_Barrel"))
        info.nBarrelId = br->nId;
    if (AMMO* am = g_pWeaponMgr->GetAmmo("Pounce_Ammo"))
        info.nAmmoId = am->nId;
    info.fTime = now;
    pBot->SetLastFireInfo(info);

    if (b && pChar && pChar->IsDead())
    {
        b->specialTime = now;
        b->specialName = "Pounce";
        b->specialVictim = hVictim;
    }
    if (g_cfg.debug && pChar)   // pounces also touch walls and props (no damage there)
        Log("POUNCESTUN %s -> %s damage=%.0f stun=%.1f%s", ObjectLabel(pBot->m_hObject).c_str(), ObjectLabel(hVictim).c_str(),
            dealt, stunned, pChar && pChar->IsDead() ? " kill" : (stunned < 0.0f ? " (no stun)" : ""));
}

void BotMgr_MeleePath(HOBJECT hAI, LTVector& vPath)
{
    // a player's fire path is a unit vector; the barrel's spread (thousandths of it) is added to it
    if (g_cfg.clawSpread && vPath.MagSqr() > 0.0001f)
        vPath.Norm();
}

// How a player notices a cloaked Predator (the shimmer): close by, or at mid range when it moves or has just
// hit him or just fired (its shots show where it is). A still, silent cloaked Predator further away stays unseen. Used for sight
// (BotMgr_CloakSeeFactor) and for firing (BotMgr_CanShootCloaked). 2 = close, 1 = mid range and giving itself
// away, 0 = unseen.
static int CloakZone(HOBJECT hAI, HOBJECT hTarget, float d2)
{
    if (d2 < 400.0f * 400.0f)
        return 2;
    if (d2 >= 1200.0f * 1200.0f)
        return 0;
    LTVector v;
    g_pLTServer->GetVelocity(hTarget, &v);
    if (v.MagSqr() > 100.0f * 100.0f)
        return 1;
    std::map<HOBJECT, LTFLOAT>::const_iterator shot = g_lastShot.find(hTarget);
    if (shot != g_lastShot.end() && g_pLTServer->GetTime() - shot->second < 1.5f)
        return 1;  // it has just fired: the spear, disc or bolt shows where it is
    CAI* ai = dynamic_cast<CAI*>(g_pLTServer->HandleToObject(hAI));
    return ai && ai->GetDestructible()->GetLastDamager() == hTarget &&
                   g_pLTServer->GetTime() - ai->GetDestructible()->GetLastDamageTime() < 2.0f
               ? 1
               : 0;
}

LTFLOAT BotMgr_CloakSeeFactor(HOBJECT hAI, HOBJECT hStimulus, LTFLOAT fDistanceSqr)
{
    // The single-player AI sees a cloaked Predator only within 250 units and at 15 % of its sight rate
    // (CloakFalloffDistance [125, 250], CloakAlphaCap 0.15, BaseAIAttrib): a Marine bot was blind to a Stalker
    // sniping from 800 units, where a player sees the shimmer move or the spear come. Only Predators keep the
    // stock rule: a Predator player's NavVision shows cloaked Predators at full alpha (client CharacterFX.cpp,
    // m_bAlphaOverride), while an Alien player sees them like a Marine (alpha 0 still, 0.15 moving, plus the
    // shimmer); the single-player alien AI's cloak sensitivity of 0 was made for the Predator campaign.
    if (!g_cfg.seecloak || !hAI || !hStimulus || !BotMgr_IsBot(hAI))
        return -1.0f;
    CAI* ai = dynamic_cast<CAI*>(g_pLTServer->HandleToObject(hAI));
    CCharacter* tc = dynamic_cast<CCharacter*>(g_pLTServer->HandleToObject(hStimulus));
    if (!ai || !tc || tc->GetCloakState() == CLOAK_STATE_INACTIVE)
        return -1.0f;
    if (ai->GetCharacterClass() == PREDATOR)
        return -1.0f;
    LTFLOAT r, g, b, a = 1.0f;
    g_pLTServer->GetObjectColor(hStimulus, &r, &g, &b, &a);
    if (a >= 0.5f)
        return -1.0f;  // cloaking or decloaking, mostly visible: the stock rule scales by the alpha
    // half the normal sight rate close by, a third at mid range while it gives itself away
    switch (CloakZone(hAI, hStimulus, fDistanceSqr))
    {
        case 2: return 0.5f;
        case 1: return 0.33f;
        default: return 0.0f;
    }
}

static float BotSkillSeeFactor(HOBJECT hBot); // below, with BotMgr_BotAccuracy

LTFLOAT BotMgr_SizeSeeFactor(HOBJECT hAI, HOBJECT hStimulus)
{
    // A person notices a small target later than a standing adult: a chestburster (DefaultDims half-height 20, 40
    // units tall), a facehugger (15), a crouching Marine or Predator (CharacterMovement's crouched dims: half the
    // height). The AI's sight rate ignores the size. The reference is the smallest standing adult class (Runner,
    // half-height 40), so no adult class standing up changes; below it the rate is scaled by sqrt(h/40) (a
    // conservative guess: by height, not area), never below 0.5 (a small target moving in the open is still seen).
    // Aliens are judged by their class size: they crawl and wall-walk most of the time, a separate question.
    if (!hAI || !hStimulus || !BotMgr_IsBot(hAI))
        return 1.0f;
    const float skillF = BotSkillSeeFactor(hAI); // a server extension's per-bot skill
    if (!g_cfg.smallSight)
        return skillF;
    CCharacter* tc = dynamic_cast<CCharacter*>(g_pLTServer->HandleToObject(hStimulus));
    if (!tc)
        return skillF;
    float h = tc->GetMovement()->GetCharacterButes()->m_vDims.y;
    if (!IsAlien(tc))
    {
        const LTVector d = tc->GetDims();
        if (d.y > 0.0f && d.y < h)
            h = d.y;
    }
    const float kRef = 40.0f, kFloor = 0.5f;
    if (h >= kRef || h <= 0.0f)
        return skillF;
    const float cls = tc->GetMovement()->GetCharacterButes()->m_vDims.y;
    ++g_smallSight[cls < 25.0f ? 0 : (h < cls ? 2 : 1)];
    const float f = sqrtf(h / kRef);
    return skillF * (f < kFloor ? kFloor : f);
}

void BotMgr_NoteShot(HOBJECT hShooter)
{
    if (hShooter)
        g_lastShot[hShooter] = g_pLTServer->GetTime();
}

bool BotMgr_CanShootCloaked(HOBJECT hAI, HOBJECT hTarget)
{
    // The single-player AI only fires at a cloaked Predator once it fully sees it, which a human does not
    // wait for when the shimmer gives it away: close by, or at mid range when it moves or has just hit him
    // (its shots show where it is). A still, cloaked Predator further away stays unseen. The aim is worse
    // on a cloaked target (BotMgr_AimError).
    if (!hAI || !hTarget || !BotMgr_IsBot(hAI))
        return false;
    LTVector a, b;
    g_pLTServer->GetObjectPos(hAI, &a);
    g_pLTServer->GetObjectPos(hTarget, &b);
    return CloakZone(hAI, hTarget, (a - b).MagSqr()) > 0;
}

// Is this weapon one of the multiplayer class's default weapons (its MPClassWeaponSet)?
static bool ClassHasWeapon(const char* mpSet, const char* weapon)
{
    const int set = mpSet ? g_pCharacterButeMgr->GetSetFromModelType(mpSet) : -1;
    WEAPON_SET* ws = set >= 0 ? g_pWeaponMgr->GetWeaponSet((int)g_pCharacterButeMgr->GetMPClassWeaponSet(set)) : 0;
    for (int i = 0; ws && i < ws->nNumWeapons; ++i)
        if (ws->bDefaultWeapon[i] && !_stricmp(ws->szWeaponName[i], weapon))
            return true;
    return false;
}

const char* BotMgr_MeleeWeapon(HOBJECT hAI, const char* def)
{
    // a Predator bot's melee: the AI spear (AI_Spear_Barrel, 50) becomes the players' melee weapon of its class:
    // the combistick (Spear_Barrel_MP: 60 x 6 vectors swept sideways, 200 units) for the classes that carry one
    // (Maurauder, Bezerker), else the wristblades every Predator class has (Wristblades_Barrel_MP: 60 x 4 vectors,
    // 150 units: Stalker, Striker)
    if (!def || _stricmp(def, "Spear") || !BotMgr_IsBot(hAI))
        return def;
    const Bot* b = FindBot(hAI);
    if (b && b->cls && b->cls->mpSet && !ClassHasWeapon(b->cls->mpSet, "Spear_MP") &&
        g_pAIWeaponButeMgr->GetIDByName("Wristblades_MP") >= 0)
        return "Wristblades_MP";
    if (g_pAIWeaponButeMgr->GetIDByName("Spear_MP") >= 0)
        return "Spear_MP";
    return def;
}

bool BotMgr_OnFire(HOBJECT hShooter)
{
    // a shot about to be fired (CAI::FireWeapon); false = no shot (a Predator sifting, healing or out of energy)
    CAI* ai = dynamic_cast<CAI*>(g_pLTServer->HandleToObject(hShooter));
    const AIWBM_AIWeapon* w = ai ? ai->GetCurrentWeaponButesPtr() : 0;
    Bot* bot = FindBot(hShooter);
    // an alien bot feeding, charging its tail or biting: its strikes come from BotMgr (Strike), not from the claw
    // animation's fire keys (which would hit at the AI's last target)
    if (bot && (bot->feeding || bot->tailSince > 0.0f || bot->biteAt > 0.0f))
        return false;
    // with trackers, a person holds the SADAR while its lock forms on an enemy (2 s), then fires a tracker
    if (bot && w && g_cfg.altFire && bot->species == BOT_MARINE && bot->trackers > 0 && ai && IsWeapon(w, "SADAR_MP")
        && !bot->sadarLocked && SadarLockOnEnemy(*bot, ai)
        && g_pLTServer->GetTime() - bot->sadarSince < kSadarLockTime + 0.2f)
        return false;
    // one grenade per switch to the launcher, none without grenades (bots.cfg altfire)
    if (bot && w && IsWeapon(w, kPRGrenade) && (bot->grenades <= 0 || g_pLTServer->GetTime() < bot->altNext))
        return false;
    if (bot && bot->species == BOT_PREDATOR && w)
    {
        if (bot->siftUntil > 0.0f || bot->mediUntil > 0.0f)
            return false;
        // no spears left, or the disc not back yet (bots.cfg predammo): no shot; PredWeaponUpkeep switches
        if (!BotMgr_WeaponUsable(hShooter, w->Name.c_str()))
            return false;
        const bool pistol = !_stricmp(w->Name.c_str(), "Predator_Pistol_MP");
        const bool cannon = !_stricmp(w->Name.c_str(), "Shoulder_Cannon_MP");
        if ((pistol && bot->energy < 6.0f) || (cannon && bot->energy < 3.0f))
        {
            StartSift(*bot, ai, w->Name.c_str());
            return false;
        }
        // the energy is spent in BotMgr_FireCommit, once the shot really leaves (not held by the aim check)
    }
    if (g_cfg.debug)
    {
        float dist = -1.0f;
        if (ai && ai->HasTarget())
        {
            LTVector tp;
            g_pLTServer->GetObjectPos(ai->GetTarget().GetObject(), &tp);
            dist = (tp - ai->GetPosition()).Mag();
        }
        Log("SHOT %s weapon=%s dist=%.0f energy=%.0f", ObjectLabel(hShooter).c_str(), w ? w->Name.c_str() : "-", dist,
            bot ? bot->energy : -1.0f);
    }
    return true;
}

// A shot that really leaves (after the aim check): a Predator's energy, the shoulder cannon's charge.
static void SpendShot(HOBJECT hShooter, CAI* ai, const AIWBM_AIWeapon* w, Bot* bot)
{
    if (!ai || !w)
        return;
    if (bot)
        bot->lastShot = g_pLTServer->GetTime();
    if (bot && bot->species == BOT_PREDATOR && !_stricmp(w->Name.c_str(), "Predator_Pistol_MP"))
        bot->energy -= 6.0f;
    if (!_stricmp(w->Name.c_str(), "Shoulder_Cannon_MP"))
    {
        // Players charge the shoulder cannon (fire held: CP_CANNON_CHARGE, a charge level per step) and
        // CWeapon::Fire then uses the next barrels: level 1 = 40 + 60 splash over 200 units, level 2 =
        // 40 + 150 over 400 (uncharged 35 + 15 over 100). A bot charges during the pause before the shot.
        const LTFLOAT now = g_pLTServer->GetTime();
        std::map<HOBJECT, LTFLOAT>::iterator it = g_lastCannon.find(hShooter);
        const LTFLOAT waited = it == g_lastCannon.end() ? 10.0f : now - it->second;
        int level = waited >= 1.5f ? 2 : waited >= 0.75f ? 1 : 0;
        if (bot)
        {
            // the charge the energy allows: 3, then 2 and 3 more
            const float cost[3] = {3.0f, 5.0f, 8.0f};
            while (level > 0 && bot->energy < cost[level])
                --level;
            bot->energy -= cost[level];
        }
        ai->SetCannonChargeLevel((uint8)level);
        g_lastCannon[hShooter] = now;
    }
}

void BotMgr_OnDamage(HOBJECT hVictim, HOBJECT hDamager, LTFLOAT fDamage, int nType)
{
    // debug only: every hit a bot deals, with its weapon at that moment, and damage a bot takes from
    // anything else (falls, hazards, a damager that no longer exists)
    if (!g_cfg.debug || fDamage <= 0.0f || g_bots.empty())
        return;
    if (nType == DT_NAPALM)
    {
        // a burning bot (at most one line per second): with 'playerburn' it keeps living and losing health
        Bot* vb = FindBot(hVictim);
        const LTFLOAT now = g_pLTServer->GetTime();
        CAI* vai = vb ? BotAI(*vb) : LTNULL;
        if (vai && now >= vb->burnLogAt)
        {
            Log("BURN %s hp=%.0f armor=%.0f tick=%.2f from=%s", vb->name.c_str(), vai->GetHitPoints(),
                vai->GetDestructible()->GetArmorPoints(), fDamage, hDamager ? ObjectLabel(hDamager).c_str() : "none");
            vb->burnLogAt = now + 1.0f;
        }
    }
    CAI* dai = hDamager ? dynamic_cast<CAI*>(g_pLTServer->HandleToObject(hDamager)) : LTNULL;
    if (!dai || !BotMgr_IsBot(hDamager))
    {
        if (BotMgr_IsBot(hVictim))
            Log("DMGIN %s %.1f type=%d from=%s", ObjectLabel(hVictim).c_str(), fDamage, nType,
                hDamager ? ObjectLabel(hDamager).c_str() : "none");
        return;
    }
    const char* wn = (g_strikeName && hDamager == g_strikeShooter) ? g_strikeName : KillWeaponLabel(hDamager, nType);
    Log("DMG %s -> %s %.1f weapon=%s", ObjectLabel(hDamager).c_str(), ObjectLabel(hVictim).c_str(), fDamage, wn);
}

bool BotMgr_AimError(HOBJECT hShooter, HOBJECT hTarget, const LTVector& vFirePos, LTVector& vPath, bool bFullyAimed)
{
    // The AI's path: when fully aimed, "aim point - muzzle" where the aim point (TargetAimPoint, AIUtils.cpp)
    // already leads a projectile's target, adds the arc of a grenade and aims at the chest; it also adds the
    // single-player "lag" (a 30-50 unit offset that drifts, the AI's own miss). Not fully aimed: the gun's
    // forward vector. Bots keep the aim point, swap the lag for a person's error, and fire where the gun
    // points when it is not aimed yet.
    CAI* ai = dynamic_cast<CAI*>(g_pLTServer->HandleToObject(hShooter));
    // The path is as long as the distance, and CWeaponMgr::CalculateWeaponPath adds the barrel spread
    // (thousandths) before normalizing: on such a path the spread vanished (every shotgun pellet hit the same
    // point at any range). A player's path is a unit vector; so is a bot's.
    LTVector path = vPath;
    if (bFullyAimed && ai && ai->HasTarget())
    {
        const LTVector& lag = ai->GetTarget().GetLag();
        if (path.MagSqr() > lag.MagSqr())
            path -= lag;
    }
    const float len = path.Mag();
    if (len <= 0.0f)
        return true;
    LTVector dir = path / len;
    vPath = dir;
    if (ai && hTarget && !bFullyAimed)
    {
        // how far the gun points from the target: past 6 degrees, wait for the aim (~30 % of the rail gun,
        // SADAR and shoulder cannon shots left about 30 degrees off at the start of a fire action)
        LTVector tp;
        g_pLTServer->GetObjectPos(hTarget, &tp);
        LTVector to = tp - vFirePos;
        const float tl = to.Mag();
        const float off = tl > 1.0f ? acosf(Clamp(dir.Dot(to / tl), -1.0f, 1.0f)) * 57.3f : 0.0f;
        if (g_cfg.debug)
            Log("AIM %s aimed=0 off=%.1f dist=%.0f%s", ObjectLabel(hShooter).c_str(), off, tl, off > 6.0f ? " held" : "");
        if (off > 6.0f)
            return false;
    }
    if (!bFullyAimed || !ai || !hTarget)
        return true;

    // Error size: about 4 degrees x (1 - accuracy) plus a lag on a moving target of 20 % x (1 - accuracy)
    // of its speed (at accuracy 0.5: 2 degrees + 10 % of the speed, a running Alien at 550 u/s: ~55 units).
    // The Smart Gun aims itself for players (client AutoTargetMgr, TT_MARINE_SMARTGUN): no error, only its
    // barrel spread, except on a cloaked Predator (no lock; a shimmer: twice the error).
    const AIWBM_AIWeapon* w = ai->GetCurrentWeaponButesPtr();
    const bool tracking = w && !_stricmp(w->Name.c_str(), "Smart_Gun_MP");
    const float acc = BotMgr_BotAccuracy(hShooter);
    float miss = tracking ? 0.0f : 1.0f - acc;
    if (CCharacter* tc = dynamic_cast<CCharacter*>(g_pLTServer->HandleToObject(hTarget)))
        if (tc->GetCloakState() != CLOAK_STATE_INACTIVE)
            miss = 2.0f * (1.0f - acc);
    if (miss <= 0.0f)
    {
        // the lock follows the target: aim at where it is now (the AI's aim point is a few frames old,
        // 50 units behind a running Alien), at the chest
        LTVector tp;
        g_pLTServer->GetObjectPos(hTarget, &tp);
        tp += ai->GetTarget().GetTargetOffset();
        LTVector to = tp - vFirePos;
        if (to.MagSqr() > 1.0f)
        {
            to.Norm();
            vPath = to;
        }
        return true;
    }
    LTVector tpos, tvel;
    g_pLTServer->GetObjectPos(hTarget, &tpos);
    g_pLTServer->GetVelocity(hTarget, &tvel);
    const float dist = (tpos - vFirePos).Mag();
    if (dist < 1.0f)
        return true;
    const float radius = dist * tanf(4.0f * miss * 3.14159f / 180.0f) + 0.2f * miss * tvel.Mag();
    const float angle = atanf(radius / dist);

    // drift: an Ornstein-Uhlenbeck walk with a 0.35 s memory and a standard deviation of 1
    const LTFLOAT now = g_pLTServer->GetTime();
    std::map<HOBJECT, AimDrift>::iterator it = g_aimDrift.find(hShooter);
    if (it == g_aimDrift.end() || now - it->second.time > 2.0f)
    {
        AimDrift d = {Gauss(), Gauss(), now};
        it = g_aimDrift.insert(std::make_pair(hShooter, d)).first;
        it->second = d;
    }
    else
    {
        const float a = expf(-(float)(now - it->second.time) / 0.35f);
        const float b = sqrtf(1.0f - a * a);
        it->second.x = a * it->second.x + b * Gauss();
        it->second.y = a * it->second.y + b * Gauss();
        it->second.time = now;
    }
    LTVector side = dir.Cross(LTVector(0.0f, 1.0f, 0.0f));
    if (side.MagSqr() < 0.01f)
        side = LTVector(1.0f, 0.0f, 0.0f);
    side.Norm();
    const LTVector up = side.Cross(dir);
    // half the size per axis (about 85 % of the shots inside 'radius'), less vertical error
    LTVector nd = dir + side * tanf(0.5f * angle * it->second.x) + up * tanf(0.3f * angle * it->second.y);
    nd.Norm();
    vPath = nd;
    return true;
}

LTFLOAT BotMgr_DamageScale()
{
    return g_cfg.damageScale;
}

LTFLOAT BotMgr_Accuracy()
{
    return g_cfg.accuracy;
}

bool BotMgr_ValidateHit(HOBJECT hShooter, HOBJECT hTarget, const LTVector& vTargetPos, const LTVector& vFirePos)
{
    // The client hit-tests a player's bullets against what it shows, so the bot it saw may have moved since: the
    // server's check for a player target allows about 0.26 s of running; a bot gets a full second at the fastest
    // run (Runner, 550 units/s) to cover a high ping. A claimed position farther than that is refused.
    const Bot* b = hTarget ? FindBot(hTarget) : 0;
    if (!b)
        return false;
    LTVector pos;
    g_pLTServer->GetObjectPos(hTarget, &pos);
    const float off = (vTargetPos - pos).Mag();
    const bool ok = off <= 600.0f;
    ++g_playerHits[ok ? 0 : 1];
    if (g_cfg.debug || !ok)
        Log("PHIT %s -> %s off=%.0f dist=%.0f%s", ObjectLabel(hShooter).c_str(), b->name.c_str(), off,
            (pos - vFirePos).Mag(), ok ? "" : " refused");
    return ok;
}

// A bot's skill (ServerExt_BotSkill), 0..1, sets its accuracy (the bots.cfg 'accuracy' scale: aim error and weapon
// spread) and how fast it notices what it sees (BotSkillSeeFactor). No skill: bots.cfg 'accuracy', normal sight.
LTFLOAT BotMgr_BotAccuracy(HOBJECT hBot)
{
    const Bot* b = hBot ? FindBot(hBot) : 0;
    return (b && b->skill >= 0.0f) ? b->skill : g_cfg.accuracy;
}

static float BotSkillSeeFactor(HOBJECT hBot)
{
    // sight rate x 2^(2 x (skill - 0.5)): skill 1 notices twice as fast (0.1 s up close, 0.5 s at long range),
    // 0.5 = the normal bot, 0 = half as fast
    const Bot* b = hBot ? FindBot(hBot) : 0;
    return (b && b->skill >= 0.0f) ? powf(2.0f, 2.0f * (b->skill - 0.5f)) : 1.0f;
}

// Read-only accessors for the server extensions (ServerExt.h)
int BotMgr_BotInfo(HOBJECT hObj, const char** display, int* species, int* slot)
{
    if (!hObj)
        return 0;
    for (size_t i = 0; i < g_bots.size(); ++i)
        if (g_bots[i].hObj.GetHOBJECT() == hObj)
        {
            if (display)
                *display = g_bots[i].display.c_str();
            if (species)
                *species = (int)g_bots[i].species;
            if (slot)
                *slot = g_bots[i].slot;
            return 1;
        }
    return 0;
}

int BotMgr_BotCount()
{
    return (int)g_bots.size();
}

HOBJECT BotMgr_BotObject(int i)
{
    if (i < 0 || i >= (int)g_bots.size() || !g_bots[i].alive)
        return LTNULL;
    return g_bots[i].hObj.GetHOBJECT();
}

bool BotMgr_IsBot(HOBJECT hObj)
{
    if (!hObj)
        return false;
    for (size_t i = 0; i < g_bots.size(); ++i)
        if (g_bots[i].alive && g_bots[i].hObj.GetHOBJECT() == hObj)
            return true;
    return false;
}

LTFLOAT BotMgr_TrackerBid(CAI* pAI, LTVector* pBlip)
{
    // above AttackOther (30: a target only heard, or waiting for a new one), below Investigate (40) and an
    // enemy in sight (AttackSeeEnemy 60)
    const LTFLOAT kBid = 35.0f;
    if (!g_cfg.tracker || !pAI || g_bots.empty())
        return 0.0f;
    Bot* b = FindBot(pAI->m_hObject);
    const LTFLOAT now = g_pLTServer->GetTime();
    if (!b || b->trackReactAt <= 0.0f || now < b->trackReactAt || now > b->trackWatchUntil || pAI->HasTarget())
        return 0.0f;
    // not on a ladder, in a jump or a fall: only from the ground
    const CharacterMovementState s = pAI->GetMovement()->GetMovementState();
    if (s != CMS_IDLE && s != CMS_WALKING && s != CMS_RUNNING)
        return 0.0f;
    if (pBlip)
        *pBlip = b->trackPos;
    return kBid;
}

void BotMgr_HearStep(CCharacter* pSource, int nKind)
{
    if (!g_cfg.hearSteps || !pSource || g_bots.empty() || !g_pGameServerShell->GetGameType().IsMultiplayer())
        return;
    const float radius = nKind == BOTHEAR_STEP ? kHearStepRadius : kHearJumpRadius;
    CharFootstepSoundInfo heard;
    const LTFLOAT now = g_pLTServer->GetTime();
    heard.fTime = now;
    heard.fVolume = radius / kHearNoiseDistance;
    heard.vLocation = pSource->GetPosition();
    for (size_t i = 0; i < g_bots.size(); ++i)
    {
        Bot& b = g_bots[i];
        CAI* ai = b.alive ? BotAI(b) : LTNULL;
        if (!ai || ai == pSource || ai->IsDead())
            continue;
        const float dist = (ai->GetPosition() - heard.vLocation).Mag();
        if (dist >= radius || !g_pCharacterMgr->AreEnemies(*ai, *pSource))
            continue;  // out of earshot; the AI's footstep sense ignores non-enemies too
        // only a clearly audible sound: the client fades it from full volume at 10 % of the radius to nothing at the
        // radius; a person in a fight does not turn for the faintest steps (the threshold is a choice, 'hearlevel')
        if (1.0f - dist / radius < g_cfg.hearLevel)
            continue;
        ai->GetSenseMgr().Footstep(heard, pSource);
        size_t k = 0;
        while (k < b.heard.size() && b.heard[k].h != pSource->m_hObject)
            ++k;
        if (k == b.heard.size())
        {
            Bot::HeardSrc hs = {pSource->m_hObject, -1000.0f, 0.0f};
            b.heard.push_back(hs);
        }
        if (now - b.heard[k].t > kHearOnsetGap)
        {
            ++b.hearOnsets;
            if (g_cfg.debug)
                Log("HEAR %s <- %s dist=%.0f kind=%s", b.name.c_str(), ObjectLabel(pSource->m_hObject).c_str(), dist,
                    nKind == BOTHEAR_STEP ? "step" : nKind == BOTHEAR_JUMP ? "jump" : "land");
        }
        b.heard[k].t = now;
        b.heard[k].dist = dist;
    }
}

bool BotMgr_HearSteps()
{
    return g_cfg.hearSteps;
}

bool BotMgr_QuickInvestigate(CAI* pAI)
{
    return g_cfg.hearSteps && pAI && !g_bots.empty() && FindBot(pAI->m_hObject) != 0;
}

LTFLOAT BotMgr_GlanceStart(CAI* pAI, HOBJECT hSource, bool bFootstep)
{
    if (!BotMgr_QuickInvestigate(pAI))
        return -1.0f;
    Bot* b = FindBot(pAI->m_hObject);
    const LTFLOAT now = g_pLTServer->GetTime();
    b->glanceSrc = bFootstep ? hSource : LTNULL;
    b->glanceAt = now;
    b->glanceDist = 1e9f;
    for (size_t k = 0; k < b->heard.size(); ++k)
        if (b->heard[k].h == hSource)
            b->glanceDist = b->heard[k].dist;
    // the turn toward it runs meanwhile (InvestigateGoal::StartSuspicious); the glance ends after both
    const LTFLOAT len = GetRandom(0.3f, 0.6f);
    if (g_cfg.debug)
        Log("GLANCE %s toward %s (%s) %.2f s", b->name.c_str(), hSource ? ObjectLabel(hSource).c_str() : "-",
            bFootstep ? "steps" : "other", len);
    return len;
}

bool BotMgr_GlanceFollow(CAI* pAI, bool bFootstep)
{
    Bot* b = pAI ? FindBot(pAI->m_hObject) : 0;
    if (!b)
        return true;
    bool follow = !bFootstep;   // gunfire, a body...: go and look, as before (without the long wait)
    if (bFootstep && b->glanceSrc)
        for (size_t k = 0; k < b->heard.size(); ++k)
            if (b->heard[k].h == b->glanceSrc && b->heard[k].t > b->glanceAt && b->heard[k].dist < b->glanceDist)
                follow = true;   // still coming closer
    if (g_cfg.debug)
        Log("GLANCEEND %s %s", b->name.c_str(), follow ? "follow" : "resume");
    return follow;
}

bool BotMgr_LikePlayer(HOBJECT hObj, int rule)
{
    bool on = false;
    switch (rule)
    {
        case BOTRULE_NAPALM: on = g_cfg.playerBurn; break;
        case BOTRULE_LIMBS: on = g_cfg.playerLimbs; break;
        case BOTRULE_RECOIL: on = g_cfg.playerRecoil; break;
        case BOTRULE_IMMUNITY: on = g_cfg.playerHits; break;
    }
    if (!on || !hObj || g_bots.empty())
        return false;
    Bot* b = FindBot(hObj);
    if (!b)
        return false;
    if (rule == BOTRULE_RECOIL && g_cfg.debug)
    {
        // the hook asks only while the bot recoils: a fire action the AI would have refused
        const LTFLOAT now = g_pLTServer->GetTime();
        ++b->recoilFires;
        if (now >= b->recoilLogAt)
        {
            Log("RECOILFIRE %s starts firing while recoiling (%d this life)", b->name.c_str(), b->recoilFires);
            b->recoilLogAt = now + 2.0f;
        }
    }
    return true;
}

bool BotMgr_Debug()
{
    return g_cfg.debug;
}

bool BotMgr_AlienWallWalk()
{
    return g_cfg.wallwalk;
}

bool BotMgr_MoveDestOK(CAI* pAI, const LTVector& vDest)
{
    // a short side step or step back: nothing solid in the way and floor under the destination, at most a
    // stair step lower (no stepping off ledges)
    if (!pAI || !g_navLoaded)
        return false;
    const LTVector& pos = pAI->GetPosition();
    const LTVector& dims = pAI->GetDims();
    IntersectQuery q;
    IntersectInfo info;
    q.m_From = pos;
    q.m_To = vDest;
    q.m_Flags = IGNORE_NONSOLID;
    if (g_pLTServer->IntersectSegment(&q, &info))
        return false;
    q.m_From = vDest;
    q.m_To = LTVector(vDest.x, vDest.y - dims.y - 40.0f, vDest.z);
    if (!g_pLTServer->IntersectSegment(&q, &info))
        return false;
    return info.m_Point.y >= pos.y - dims.y - 24.0f;
}

// --- combat movement (hook 'firemove') and the alien zig-zag (hook 'zigzag') -----------------------------
// A player fires while moving, strafing from side to side. The server plays RunF for a strafing player
// (CPlayerObj::UpdateActionAnimation) and the clients turn its legs toward the movement with the strafe-yaw
// node control, which CCharacter::UpdateCharacterFX enables for every character in multiplayer, bots
// included. The AI stood still for a whole burst: AIActionFireWeapon never moves and cannot be interrupted,
// and movement flags are cleared every frame. BotMgr_CombatMove runs after the AI's action every frame: while
// a Marine or Predator bot fires at or aims at a visible target, it faces the target and runs along short
// "legs" to its side, each checked first on the world and on the waypoint graph.
namespace {

const float kLegStep = 32.0f;      // floor and waypoint samples along a leg
const float kLegNodeXZ = 64.0f;    // a sample must lie this close to a waypoint (they are about 64 units apart)
const float kLegNodeY = 40.0f;     // ... at about the same height
// floor rise and drop between two samples: the steepest walkable ramp of the waypoint generator (max_slope 46
// degrees) changes 33 units over 32; more is a ledge or a wall
const float kLegStepUp = 36.0f;
const float kLegStepDown = 36.0f;
const float kTanTilt = 0.577f;     // tan 30 degrees: a leg's tilt toward/away from the target; the zig-zag angle
const float kZigMin = 300.0f;      // aliens zig-zag between these distances to their target
const float kZigMax = 2000.0f;
// wall approach (bots.cfg 'wallapproach'): between the pounce's maximum range (AlienPounceRange 750, AIButes) and
// this distance (my choice), an alien bot's path to a target that faces it keeps off the open floor in front of it
const float kWallApproachMin = 750.0f;
const float kWallApproachMax = 2500.0f;

LTBOOL LegRayFilter(HOBJECT hObj, void* pUser)
{
    // characters move (WillClip checks them); their hit boxes and bodies are no obstacles
    if (!hObj || hObj == (HOBJECT)pUser)
        return LTFALSE;
    LPBASECLASS p = g_pLTServer->HandleToObject(hObj);
    if (p && (dynamic_cast<CCharacter*>(p) || dynamic_cast<CCharacterHitBox*>(p) || dynamic_cast<BodyProp*>(p)))
        return LTFALSE;
    return LTTRUE;
}

// World geometry and solid objects (props, doors) between two points.
bool RayHit(const LTVector& from, const LTVector& to, HOBJECT self, LTVector* pHit = 0)
{
    IntersectQuery q;
    IntersectInfo info;
    q.m_From = from;
    q.m_To = to;
    q.m_Flags = INTERSECT_OBJECTS | IGNORE_NONSOLID;
    q.m_FilterFn = LegRayFilter;
    q.m_pUserData = self;
    if (!g_pLTServer->IntersectSegment(&q, &info))
        return false;
    if (pHit)
        *pHit = info.m_Point;
    return true;
}

// The waypoint among 'node' and its walk neighbours closest to p (a floor point); -1 when none is close.
int GraphStep(int node, const Vec3& p)
{
    const avp2bots::NavNode& n = g_nav.Node(node);
    int best = node;
    float bestD = avp2bots::DistXZ(n.pos, p);
    for (size_t i = 0; i < n.links.size(); ++i)
    {
        const avp2bots::NavLink& l = n.links[i];
        if (l.kind != avp2bots::NAV_WALK || l.blocked || g_nav.IsBlocked(l.to))
            continue;
        const float d = avp2bots::DistXZ(g_nav.Node(l.to).pos, p);
        if (d < bestD)
        {
            best = l.to;
            bestD = d;
        }
    }
    if (bestD > kLegNodeXZ || fabsf(g_nav.Node(best).pos.y - p.y) > kLegNodeY)
        return -1;
    return best;
}

// The waypoint under a bot's feet, or -1 when it stands off the graph.
int FeetNode(CAI* ai)
{
    const LTVector& pos = ai->GetPosition();
    const Vec3 feet(pos.x, pos.y - ai->GetDims().y, pos.z);
    const int n = g_nav.Nearest(feet);
    if (n < 0 || avp2bots::DistXZ(g_nav.Node(n).pos, feet) > kLegNodeXZ || fabsf(g_nav.Node(n).pos.y - feet.y) > kLegNodeY)
        return -1;
    return n;
}

// Can the bot run straight from where it stands to 'dest' (an AI-centre point at its height), and see its
// target from there? Rays at waist and knee height and at the body's sides find walls and props; floor samples
// every 32 units find holes and ledges; each sample must lie on the walk graph, on the previous sample's
// waypoint or one linked to it (no hazards, no other level); no character in the way.
int CheckLeg(CAI* ai, int startNode, const LTVector& dest, bool needLos, const LTVector& aimTo)
{
    const LTVector pos = ai->GetPosition();
    const LTVector dims = ai->GetDims();
    const HOBJECT self = ai->m_hObject;
    LTVector d = dest - pos;
    d.y = 0.0f;
    const float len = d.Mag();
    if (len < 1.0f || startNode < 0)
        return LEG_GRAPH;
    const LTVector dir = d / len;
    const LTVector side(-dir.z, 0.0f, dir.x);
    const float feetY = pos.y - dims.y;
    const LTVector reach = dir * (len + dims.x);
    if (RayHit(pos, pos + reach, self))
        return LEG_WALL;
    const LTVector knee(pos.x, feetY + kLegStepUp + 2.0f, pos.z);
    if (RayHit(knee, knee + reach, self))
        return LEG_WALL;
    for (int s = -1; s <= 1; s += 2)
    {
        const LTVector k = knee + side * ((float)s * dims.x * 0.9f);
        if (RayHit(k, k + reach, self))
            return LEG_WALL;
    }
    const int steps = (int)ceilf(len / kLegStep);
    float floorY = feetY;
    int node = startNode;
    for (int i = 1; i <= steps; ++i)
    {
        const float t = (i == steps) ? len : (float)i * kLegStep;
        const LTVector p = pos + dir * t;
        LTVector hit;
        if (!RayHit(LTVector(p.x, floorY + kLegStepUp + 2.0f, p.z), LTVector(p.x, floorY - kLegStepDown - 16.0f, p.z),
                    self, &hit))
            return LEG_FLOOR;
        if (hit.y < floorY - kLegStepDown || hit.y > floorY + kLegStepUp)
            return LEG_FLOOR;
        floorY = hit.y;
        node = GraphStep(node, Vec3(p.x, floorY, p.z));
        if (node < 0)
            return LEG_GRAPH;
    }
    if (WillClip(*ai, pos, dest))
        return LEG_CLIP;
    if (needLos)
    {
        LTVector eye = dest + (ai->GetAimFromPosition() - pos);
        eye.y += floorY - feetY;
        if (RayHit(eye, aimTo, self))
            return LEG_LOS;
    }
    return LEG_OK;
}

// A new leg for a Marine or Predator bot facing its target at tpos. Players alternate sides; the leg keeps the
// distance inside the weapon's ideal band (the AI's own ranges): it leans 30 degrees toward the target beyond
// the band's middle and, for a Predator, away from it inside the minimum. Marines never move backward (their AI
// animations have no RunB). Length: 0.4-0.8 s of running, at least 64 units, halved when nothing fits.
bool SelectLeg(Bot& b, CAI* ai, LTFLOAT now, const LTVector& tpos, const LTVector& aimTo)
{
    CombatMove& c = b.cm;
    const int start = FeetNode(ai);
    if (start < 0)
    {
        c.rej[LEG_GRAPH]++;
        return false;
    }
    const LTVector pos = ai->GetPosition();
    LTVector to = tpos - pos;
    to.y = 0.0f;
    const float dist = to.Mag();
    if (dist < 1.0f)
        return false;
    to /= dist;
    const LTVector perp(-to.z, 0.0f, to.x);
    float tilt = 0.0f;
    if (const AIWBM_AIWeapon* w = ai->GetCurrentWeaponButesPtr())
    {
        if (dist > 0.5f * (w->fMinIdealRange + w->fMaxIdealRange))
            tilt = kTanTilt;
        else if (b.species == BOT_PREDATOR && dist < w->fMinIdealRange)
            tilt = -kTanTilt;
    }
    const CharacterMovement* cmv = ai->GetMovement();
    const float speed = cmv->GetMaxSpeed((ai->GetMovementFlags() & CM_FLAG_RUN) ? CMS_RUNNING : CMS_WALKING);
    float len = Clamp(speed * GetRandom(0.4f, 0.8f), 64.0f, 256.0f);
    const int first = c.side ? -c.side : (GetRandom(0.0f, 1.0f) < 0.5f ? 1 : -1);
    for (int pass = 0; pass < 2; ++pass, len *= 0.5f)
    {
        for (int si = 0; si < 2; ++si)
        {
            for (int ti = 0; ti < (tilt != 0.0f ? 2 : 1); ++ti)
            {
                const int s = si ? -first : first;
                const float k = ti ? 0.0f : tilt;
                LTVector dir = perp * (float)s + to * k;
                dir.Norm();
                const LTVector dest = pos + dir * len;
                const int r = CheckLeg(ai, start, dest, true, aimTo);
                if (r != LEG_OK)
                {
                    c.rej[r]++;
                    continue;
                }
                c.hasLeg = true;
                c.legDest = dest;
                c.legStart = now;
                c.legUntil = now + len / (speed > 1.0f ? speed : 1.0f) * 1.5f + 0.3f;
                c.side = s;
                c.progPos = pos;
                c.progTime = now;
                c.legs++;
                if (g_cfg.debug && now >= c.nextLegLog)
                {
                    const AIWBM_AIWeapon* w = ai->GetCurrentWeaponButesPtr();
                    Log("LEG %s side=%d tilt=%.2f len=%.0f dist=%.0f weapon=%s from=%.0f %.0f %.0f to=%.0f %.0f %.0f",
                        b.name.c_str(), s, k, len, dist, w ? w->Name.c_str() : "-", pos.x, pos.y, pos.z, dest.x,
                        dest.y, dest.z);
                    c.nextLegLog = now + 5.0f;
                }
                return true;
            }
        }
    }
    return false;
}

void AbortLeg(CombatMove& c, LTFLOAT now)
{
    c.hasLeg = false;
    if (++c.aborts >= 3)
    {
        c.pauseUntil = now + 1.0f;  // three in a row: stand and shoot for a second
        c.aborts = 0;
    }
}

bool IsFireOrAim(const AIAction* act)
{
    return act && (dynamic_cast<const AIActionFireWeapon*>(act) || dynamic_cast<const AIActionAim*>(act));
}

// One frame of combat movement; true when the bot's action animation layer is ours (RunF/RunB).
bool DriveCombatMove(Bot& b, CAI* ai, LTFLOAT now, float dt, const AIAction* act)
{
    CombatMove& c = b.cm;
    if (!IsFireOrAim(act) || !ai->HasTarget() || now < c.pauseUntil)
        return false;
    const CAITarget& t = ai->GetTarget();
    if (!t.IsValid() || !t.IsPartiallyVisible() || !t.GetCharacterPtr())
        return false;
    const AIWBM_AIWeapon* w = ai->GetCurrentWeaponButesPtr();
    if (!w || w->bMeleeWeapon)
        return false;
    CharacterMovement* cmv = ai->GetMovement();
    const CharacterMovementState cms = cmv->GetMovementState();
    if (cms != CMS_IDLE && cms != CMS_WALKING && cms != CMS_RUNNING)
        return false;
    if ((cmv->GetControlFlags() & (CM_FLAG_WALLWALK | CM_FLAG_DUCK)) || ai->IsNetted() || ai->IsStunned())
        return false;

    LTVector tpos;
    g_pLTServer->GetObjectPos(t.GetObject(), &tpos);
    const LTVector pos = ai->GetPosition();
    LTVector face = tpos - pos;
    face.y = 0.0f;
    if (face.MagSqr() < 1.0f)
        return false;
    // face the target, as a player faces what he shoots at (clients get the aim pitch, not its yaw)
    ai->SnapTurn(face);

    const bool canBack = b.species == BOT_PREDATOR;  // Marines never move backward (no RunB in their AI animations)
    if (c.hasLeg)
    {
        // what is left of the leg along the directions the bot may move in: as the bot turns with its target,
        // part of a Marine's leg can end up behind it, and that part is dropped
        LTVector rel = c.legDest - pos;
        rel.y = 0.0f;
        const float lf = ai->GetForward().Dot(rel);
        const float lr = ai->GetRight().Dot(rel);
        const float lfOK = (lf < 0.0f && !canBack) ? 0.0f : lf;
        if (lr * lr + lfOK * lfOK < 8.0f * 8.0f)
        {
            c.hasLeg = false;
            c.done++;
            c.aborts = 0;
        }
        else if (now > c.legUntil)
        {
            c.abTime++;
            if (g_cfg.debug && now >= c.nextLegLog)
            {
                Log("LEGABORT %s timeout left=%.0f (forward %.0f, right %.0f) after %.2f s at %.0f %.0f %.0f", b.name.c_str(),
                    rel.Mag(), lf, lr, (float)(now - c.legStart), pos.x, pos.y, pos.z);
                c.nextLegLog = now + 5.0f;
            }
            AbortLeg(c, now);
            if (now < c.pauseUntil)
                return false;
        }
    }
    if (!c.hasLeg && !SelectLeg(b, ai, now, tpos, tpos + t.GetTargetOffset()))
    {
        c.noLeg++;
        c.pauseUntil = now + 0.5f;
        return false;
    }

    LTVector rel = c.legDest - pos;
    rel.y = 0.0f;
    const float fr = ai->GetForward().Dot(rel);
    const float rr = ai->GetRight().Dot(rel);
    const bool back = canBack && fr < -2.0f;
    const bool run = (ai->GetMovementFlags() & CM_FLAG_RUN) != 0;
    // the players' rule: back pressed = RunB, otherwise RunF (strafing included)
    CharacterAnimation* an = ai->GetAnimation();
    an->SetLayerReference(ANIMSTYLELAYER_ACTION, back ? (run ? "RunB" : "WalkB") : (run ? "RunF" : "WalkF"));
    c.moving = true;
    if (!an->MovementAllowed())
    {
        // the run animation does not play yet (as AIActionMoveTo, wait for it: no sliding)
        c.tWait += dt;
        c.progPos = pos;
        c.progTime = now;
        c.legUntil += dt;
        return true;
    }
    if (rr > 2.0f)
        ai->StrafeRight(c.legDest);
    else if (rr < -2.0f)
        ai->StrafeLeft(c.legDest);
    if (fr > 2.0f)
        ai->Forward(c.legDest);
    else if (back)
        ai->Reverse(c.legDest);
    c.tDrive += dt;

    // progress: under a quarter of the running speed over 0.3 s means something holds the bot
    if (now - c.progTime >= 0.3f)
    {
        const float speed = cmv->GetMaxSpeed(run ? CMS_RUNNING : CMS_WALKING);
        LTVector moved = pos - c.progPos;
        moved.y = 0.0f;
        if (moved.Mag() < 0.25f * speed * (float)(now - c.progTime))
        {
            c.abNoProg++;
            if (g_cfg.debug && now >= c.nextLegLog)
            {
                Log("LEGABORT %s noprogress moved=%.0f in %.2f s at %.0f %.0f %.0f", b.name.c_str(), moved.Mag(),
                    (float)(now - c.progTime), pos.x, pos.y, pos.z);
                c.nextLegLog = now + 5.0f;
            }
            AbortLeg(c, now);
        }
        c.progPos = pos;
        c.progTime = now;
    }
    return true;
}

} // namespace

void BotMgr_CombatMove(CAI* pAI)
{
    if (!g_navLoaded || !pAI)
        return;
    Bot* pb = FindBot(pAI->m_hObject);
    if (!pb)
        return;
    Bot& b = *pb;
    CombatMove& c = b.cm;
    const LTFLOAT now = g_pLTServer->GetTime();
    float dt = c.lastTime > 0.0f ? (float)(now - c.lastTime) : 0.0f;
    if (dt < 0.0f)
        dt = 0.0f;
    if (dt > 0.25f)
        dt = 0.25f;
    c.lastTime = now;

    const AIAction* act = pAI->GetCurrentAction();
    if (pAI->HasTarget())
    {
        // statistics: time with a target, moving, firing (claws included), both
        LTVector vel = pAI->GetMovement()->GetVelocity();
        vel.y = 0.0f;
        const bool moving = vel.MagSqr() > 50.0f * 50.0f;
        const bool firing = act && (dynamic_cast<const AIActionFireWeapon*>(act) || dynamic_cast<const AIActionFireMelee*>(act));
        c.tAtt += dt;
        if (moving)
            c.tMove += dt;
        if (firing)
            c.tFire += dt;
        if (moving && firing)
            c.tFireMove += dt;
    }

    const bool drive = g_cfg.combatMove && b.species != BOT_ALIEN && b.stage == 0 && DriveCombatMove(b, pAI, now, dt, act);
    if (!drive)
    {
        // back to the standing aim the fire and aim actions use (other actions set their own animation)
        if (c.moving && IsFireOrAim(act))
            pAI->GetAnimation()->SetLayerReference(ANIMSTYLELAYER_ACTION, "StandAim");
        c.moving = false;
        c.hasLeg = false;
    }
}

bool BotMgr_ZigZag(CAI* pAI, LTVector& vDest)
{
    // A player's alien does not run straight at a Marine who faces it: it runs at an angle and switches sides.
    // While the target is in view and faces it, an alien bot heads for a point ahead and 30 degrees to the side
    // (the side switching every 0.4-0.8 s), checked like a strafe leg; otherwise the stock straight approach.
    if (!g_cfg.zigzag || !g_navLoaded || !pAI)
        return false;
    Bot* pb = FindBot(pAI->m_hObject);
    if (!pb || pb->species != BOT_ALIEN || pb->stage != 0 || !pAI->HasTarget())
        return false;
    CombatMove& c = pb->cm;
    const CAITarget& t = pAI->GetTarget();
    if (!t.IsPartiallyVisible() || !t.IsFacing())
        return false;
    CharacterMovement* cmv = pAI->GetMovement();
    const CharacterMovementState cms = cmv->GetMovementState();
    if ((cms != CMS_IDLE && cms != CMS_WALKING && cms != CMS_RUNNING) || (cmv->GetControlFlags() & CM_FLAG_WALLWALK))
        return false;
    const LTVector pos = pAI->GetPosition();
    LTVector to = vDest - pos;
    to.y = 0.0f;
    const float dist = to.Mag();
    if (dist < kZigMin || dist > kZigMax)
        return false;
    if (g_cfg.wallApproach && dist >= kWallApproachMin && dist <= kWallApproachMax)
        return false;  // farther than a pounce: the wall approach chooses the route (BotMgr_FindPath)
    to /= dist;
    const LTFLOAT now = g_pLTServer->GetTime();
    if (now >= c.zzUntil || !c.zzSide)
    {
        c.zzSide = c.zzSide ? -c.zzSide : (GetRandom(0.0f, 1.0f) < 0.5f ? 1 : -1);
        c.zzUntil = now + GetRandom(0.4f, 0.8f);
        c.zzFlips++;
    }
    const int start = FeetNode(pAI);
    if (start < 0)
    {
        c.zzFail++;
        return false;
    }
    const LTVector perp(-to.z, 0.0f, to.x);
    const float ahead = (0.5f * dist < 256.0f) ? 0.5f * dist : 256.0f;
    for (int i = 0; i < 2; ++i)
    {
        const int s = i ? -c.zzSide : c.zzSide;
        const LTVector p = pos + to * ahead + perp * ((float)s * kTanTilt * ahead);
        const int r = CheckLeg(pAI, start, p, false, p);
        if (r != LEG_OK)
        {
            c.rej[r]++;
            continue;
        }
        if (s != c.zzSide)
        {
            c.zzSide = s;  // the other side is blocked: switch now
            c.zzUntil = now + GetRandom(0.4f, 0.8f);
            c.zzFlips++;
        }
        vDest = p;
        c.zzSets++;
        if (g_cfg.debug && now >= c.nextLegLog)
        {
            Log("ZIGZAG %s side=%d dist=%.0f from=%.0f %.0f %.0f to=%.0f %.0f %.0f", pb->name.c_str(), s, dist, pos.x,
                pos.y, pos.z, p.x, p.y, p.z);
            c.nextLegLog = now + 5.0f;
        }
        return true;
    }
    c.zzFail++;
    return false;
}

namespace {

// --- wall ambush (bots.cfg 'wallambush') ------------------------------------------------------------------------
// Alien players wait on walls above busy routes and drop or pounce on whoever passes. The waypoint graph has floor
// waypoints only, but every Alien wall climb ('climb' link) has a point on the wall face, 32 units below the lip,
// that the bots already reach with a WallWalkTo. An alien bot with nothing to do (BotRoam) sometimes goes to such a
// point above a busy route and hangs there for a while; when an enemy shows up, its attack goal takes over (a
// pounce from the wall when in range, CCharacter accepts MID_FORCE_POUNCE while wall-walking; otherwise the first
// MoveTo stands it up and it drops). Busy routes: the shortest walking routes between start points and pickups,
// plus where the non-alien characters actually are (sampled every 2 s).
struct AmbushSpot
{
    int from;        // floor waypoint in front of the wall
    Vec3 enter;      // floor point in front of the wall
    Vec3 wall;       // the point on the wall face (below the lip)
    Vec3 normal;     // the wall's normal (horizontal)
};
std::vector<AmbushSpot> g_ambushSpots;
std::vector<LTVector> g_startPos;   // every start point (busy places; chestbursters hide away from them)
std::map<long long, float> g_traffic;              // busy-ness per 128-unit cell
std::vector<std::pair<int, int> > g_trafficPairs;  // routes still to walk through (a few per frame)
size_t g_trafficNext = 0;
LTFLOAT g_nextHeat = 0.0f;
int g_ambStarted = 0, g_ambWaits = 0, g_ambSprung = 0, g_ambTimeout = 0, g_ambFailed = 0;  // since map start
int g_approachBiased = 0, g_approachClimbs = 0;

const float kTrafficCell = 128.0f;
// a spot's wall point is above a standing Predator (DefaultDims y 60: 120 units tall) and at most 480 above the
// floor in front (my choice: a short drop); its route lies within the pounce's reach (AlienPounceRange max 750)
const float kAmbushMinHeight = 120.0f;
const float kAmbushMaxHeight = 480.0f;
const float kAmbushReach = 750.0f;
const float kAmbushTripMax = 1000.0f;   // spots farther than this from the alien are not considered (my choice)

long long TrafficKey(float x, float y, float z)
{
    const long long ix = (long long)floorf(x / kTrafficCell) + 100000;
    const long long iy = (long long)floorf(y / kTrafficCell) + 100000;
    const long long iz = (long long)floorf(z / kTrafficCell) + 100000;
    return (ix << 40) | (iy << 20) | iz;
}

void AddTraffic(const Vec3& p, float w)
{
    g_traffic[TrafficKey(p.x, p.y, p.z)] += w;
}

void BuildAmbushData()
{
    g_ambushSpots.clear();
    g_traffic.clear();
    g_trafficPairs.clear();
    g_trafficNext = 0;
    g_nextHeat = 0.0f;
    g_ambStarted = g_ambWaits = g_ambSprung = g_ambTimeout = g_ambFailed = 0;
    g_approachBiased = g_approachClimbs = 0;
    if (!g_navLoaded)
        return;
    for (int i = 0; i < g_nav.NodeCount(); ++i)
    {
        const avp2bots::NavNode& n = g_nav.Node(i);
        for (size_t k = 0; k < n.links.size(); ++k)
        {
            const avp2bots::NavLink& l = n.links[k];
            if (l.kind != avp2bots::NAV_CLIMB || !l.hasEnds)
                continue;
            const float h = l.portal.y - l.enter.y;
            if (h < kAmbushMinHeight || h > kAmbushMaxHeight)
                continue;
            AmbushSpot s = {i, l.enter, l.portal, l.normal};
            g_ambushSpots.push_back(s);
        }
    }
    // route ends: start points of every species and the pickups
    std::vector<int> ends;
    g_startPos.clear();
    for (int pass = 0; pass < 2; ++pass)
    {
        HOBJECT h = LTNULL;
        while ((h = pass == 0 ? g_pLTServer->GetNextObject(h) : g_pLTServer->GetNextInactiveObject(h)) != LTNULL)
        {
            BaseClass* o = g_pLTServer->HandleToObject(h);
            if (!dynamic_cast<GameStartPoint*>(o) && !dynamic_cast<PickupObject*>(o))
                continue;
            LTVector p;
            g_pLTServer->GetObjectPos(h, &p);
            if (dynamic_cast<GameStartPoint*>(o))
                g_startPos.push_back(p);
            const int n = g_nav.Nearest(Vec3(p.x, p.y - 24.0f, p.z));
            if (n >= 0)
                ends.push_back(n);
        }
    }
    // 64 random routes (my choice: enough to mark the main corridors, three A* searches per frame)
    if (ends.size() >= 2)
        for (int i = 0; i < 64; ++i)
        {
            const int a = ends[GetRandom(0, (int)ends.size() - 1)];
            const int b = ends[GetRandom(0, (int)ends.size() - 1)];
            if (a != b)
                g_trafficPairs.push_back(std::make_pair(a, b));
        }
    Log("AMBUSH spots=%d (climb walls %.0f-%.0f above the floor) route ends=%d routes=%d", (int)g_ambushSpots.size(),
        kAmbushMinHeight, kAmbushMaxHeight, (int)ends.size(), (int)g_trafficPairs.size());
}

// A few routes per frame, then where the walkers are, every 2 s.
void TrafficTick(LTFLOAT now)
{
    for (int i = 0; i < 3 && g_trafficNext < g_trafficPairs.size(); ++i, ++g_trafficNext)
    {
        std::vector<int> nodes;
        if (g_nav.FindPath(g_trafficPairs[g_trafficNext].first, g_trafficPairs[g_trafficNext].second, 0, &nodes, 0, 1))
            for (size_t k = 0; k < nodes.size(); ++k)
                AddTraffic(g_nav.Node(nodes[k]).pos, 1.0f);
        if (g_trafficNext + 1 == g_trafficPairs.size())
            Log("AMBUSH routes walked: %d busy cells", (int)g_traffic.size());
    }
    if (now < g_nextHeat || !g_pCharacterMgr)
        return;
    g_nextHeat = now + 2.0f;
    for (CCharacterMgr::CharacterIterator it = g_pCharacterMgr->BeginCharacters(); it != g_pCharacterMgr->EndCharacters(); ++it)
    {
        CCharacter* c = *it;
        if (!c || c->IsDead() || IsAlien(c))
            continue;
        const LTVector& p = c->GetPosition();
        AddTraffic(Vec3(p.x, p.y - c->GetDims().y, p.z), 1.0f);
    }
}

// How busy the floor in front of a spot is, within the pounce's reach.
float SpotScore(const AmbushSpot& s)
{
    const int r = (int)ceilf(kAmbushReach / kTrafficCell);
    float sum = 0.0f;
    for (int dx = -r; dx <= r; ++dx)
        for (int dz = -r; dz <= r; ++dz)
            for (int dy = -1; dy <= 0; ++dy)
            {
                const float x = s.enter.x + dx * kTrafficCell, z = s.enter.z + dz * kTrafficCell;
                const float y = s.enter.y + dy * kTrafficCell;
                const float rx = x - s.enter.x, rz = z - s.enter.z;
                if (rx * rx + rz * rz > kAmbushReach * kAmbushReach)
                    continue;
                if (rx * s.normal.x + rz * s.normal.z < -kTrafficCell)
                    continue;  // behind the wall
                std::map<long long, float>::const_iterator it = g_traffic.find(TrafficKey(x, y, z));
                if (it != g_traffic.end())
                    sum += it->second;
            }
    return sum;
}

void LogAmbushStats()
{
    Log("AMBSTAT started=%d waits=%d sprung=%d timeout=%d failed=%d approachpaths=%d approachclimbs=%d",
        g_ambStarted, g_ambWaits, g_ambSprung, g_ambTimeout, g_ambFailed, g_approachBiased, g_approachClimbs);
}

void AmbushEnd(Bot& b, const char* why, bool sprung)
{
    AmbushState& a = b.amb;
    if (a.state == AMB_NONE)
        return;
    const LTFLOAT now = g_pLTServer->GetTime();
    if (sprung)
        ++g_ambSprung;
    else if (!_stricmp(why, "timeout"))
        ++g_ambTimeout;
    else if (a.state == AMB_GOING)
        ++g_ambFailed;
    CAI* ai = BotAI(b);
    std::string target = "-";
    float range = 0.0f;
    if (ai && ai->HasTarget())
    {
        target = ObjectLabel(ai->GetTarget().GetObject().GetHOBJECT());
        LTVector tp;
        g_pLTServer->GetObjectPos(ai->GetTarget().GetObject(), &tp);
        range = (tp - ai->GetPosition()).Mag();
    }
    Log("AMBUSH END %s reason=%s state=%s after=%.1f target=%s range=%.0f", b.name.c_str(), why,
        a.state == AMB_WAITING ? "waiting" : "going", (float)(now - a.since), target.c_str(), range);
    a.state = AMB_NONE;
    a.spot = -1;
    a.nextTry = now + GetRandom(10.0f, 20.0f);
}

} // namespace

// --- chestburster hiding (bots.cfg 'cbhide') -----------------------------------------------------------------
// A chestburster is weak: Chestburster_MP has 50 hit points, runs at 200 (CharacterButes), cannot wall-walk
// (CanWallWalk 0) and bites. The engine grows it into the host's adult 20 s after the burst
// (CCharacter::UpdateChestbursterCycle: ChestbursterMutateTime 12.5 s of growth, then ChestbursterMutateDelay
// 7.5 s; damage and feeding do not change it). A player runs off and hides until then and does not take on
// armed enemies. The bot does the same: it has only the roam goal (no Attack, no Investigate), runs to a hiding
// waypoint chosen from the walk graph (away from its enemies and out of their sight, away from start points and
// busy routes, nooks preferred), stays there, flees again when an enemy sees it or hurts it, and bites back only
// when an enemy is right on it (the Attack goal is added then and removed once the enemy has gone).
namespace {

const float kCbCornered = 160.0f;   // an enemy this close and in sight: bite back (my choice: about a lunge)
const float kCbSeenRange = 1500.0f; // enemies farther away are ignored (my choice)
const float kCbCalm = 400.0f;       // the fight is over when no enemy is this close for 2 s (my choice)
const float kCbClose = 400.0f;      // an enemy this close with a clear line sees it whatever its facing (my choice)
const float kCbTripMin = 256.0f, kCbTripMax = 2500.0f;  // hiding spots from the burst: 2500 = 12.5 s at 200 u/s

void CbLog(const Bot& b, const char* what, const LTVector& p, const char* extra)
{
    if (g_cfg.debug || g_cfg.log)
        Log("CBHIDE %s %s at=%.0f %.0f %.0f%s", b.name.c_str(), what, p.x, p.y, p.z, extra ? extra : "");
}

// Live enemies of the bot (as UpdateRelations sets them) within range: positions and eye points.
void CbEnemies(CAI* ai, float range, std::vector<CCharacter*>& out)
{
    out.clear();
    const LTVector& pos = ai->GetPosition();
    for (CCharacterMgr::CharacterIterator it = g_pCharacterMgr->BeginCharacters(); it != g_pCharacterMgr->EndCharacters(); ++it)
    {
        CCharacter* c = *it;
        if (!c || c == ai || c->IsDead())
            continue;
        if (g_pCharacterMgr->GetRelationship(*ai, *c) != CCharacterMgr::Foe)
            continue;
        if ((c->GetPosition() - pos).MagSqr() > range * range)
            continue;
        out.push_back(c);
    }
}

// Can this enemy see the point (a clear line from its eye; characters do not block)?
bool CbVisible(CCharacter* c, const LTVector& p)
{
    LTVector eye = c->GetPosition();
    eye.y += c->GetDims().y * 0.8f;
    IntersectQuery q;
    IntersectInfo info;
    q.m_From = eye;
    q.m_To = p;
    q.m_Flags = IGNORE_NONSOLID;
    return !g_pLTServer->IntersectSegment(&q, &info);
}

float TrafficAround(const Vec3& p)
{
    float sum = 0.0f;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dz = -1; dz <= 1; ++dz)
        {
            std::map<long long, float>::const_iterator it =
                g_traffic.find(TrafficKey(p.x + dx * kTrafficCell, p.y, p.z + dz * kTrafficCell));
            if (it != g_traffic.end())
                sum += it->second;
        }
    return sum;
}

// A hiding waypoint for the chestburster, or -1. Candidates: waypoints the bot can walk to within ~1500 units of
// path (24 waypoint steps, walk/drop/jump links: it cannot climb); a spot out of every enemy's sight comes first
// (a person breaks the line of sight before anything else), then: far from the enemies, a short run (time in the
// open), away from the nearest enemy, quiet (low traffic, no start point near), a nook.
int CbPickSpot(Bot& b, CAI* ai, const std::vector<CCharacter*>& enemies, float* pScore)
{
    const LTVector& pos = ai->GetPosition();
    const LTVector& dims = ai->GetDims();
    const int from = g_nav.Nearest(Vec3(pos.x, pos.y - dims.y, pos.z));
    if (from < 0)
        return -1;
    LTVector away(0, 0, 0);
    float nearest = 1e9f;
    for (size_t e = 0; e < enemies.size(); ++e)
    {
        const float d = (enemies[e]->GetPosition() - pos).Mag();
        if (d < nearest)
        {
            nearest = d;
            away = pos - enemies[e]->GetPosition();
        }
    }
    away.y = 0.0f;
    if (away.MagSqr() > 1.0f)
        away.Norm();
    // breadth-first over the walk graph, 24 steps (about 1500 units: the waypoints are ~64 apart)
    std::vector<int> reach;
    std::map<int, int> depth;
    depth[from] = 0;
    reach.push_back(from);
    for (size_t q = 0; q < reach.size(); ++q)
    {
        const int k = reach[q];
        if (depth[k] >= 24)
            continue;
        const avp2bots::NavNode& node = g_nav.Node(k);
        for (size_t l = 0; l < node.links.size(); ++l)
        {
            const avp2bots::NavLink& lk = node.links[l];
            if (lk.blocked || g_nav.IsBlocked(lk.to) || depth.count(lk.to))
                continue;
            if (lk.kind != avp2bots::NAV_WALK && lk.kind != avp2bots::NAV_DROP && lk.kind != avp2bots::NAV_JUMP)
                continue;
            depth[lk.to] = depth[k] + 1;
            reach.push_back(lk.to);
        }
    }
    struct Cand { int n; float score; };
    std::vector<Cand> cands;
    for (int k = 0; k < 64 && reach.size() > 1; ++k)
    {
        const int n = reach[GetRandom(1, (int)reach.size() - 1)];
        const Vec3& np = g_nav.Node(n).pos;
        const LTVector sp(np.x, np.y + dims.y, np.z);
        const float trip = (sp - pos).Mag();
        if (trip < 150.0f)
            continue;
        float score = 0.0f;
        float dmin = 2000.0f;
        bool visible = false;
        for (size_t e = 0; e < enemies.size(); ++e)
        {
            const float d = (enemies[e]->GetPosition() - sp).Mag();
            if (d < dmin)
                dmin = d;
            if (!visible && d < 2500.0f && CbVisible(enemies[e], sp))
                visible = true;
        }
        if (visible)
            score -= 3.0f;
        score += 1.5f * dmin / 2000.0f;
        score -= trip / 1500.0f;
        if (away.MagSqr() > 0.5f)
        {
            LTVector dir = sp - pos;
            dir.y = 0.0f;
            if (dir.MagSqr() > 1.0f)
            {
                dir.Norm();
                score += 0.5f * dir.Dot(away);
            }
        }
        const float traffic = TrafficAround(np);
        score -= traffic > 20.0f ? 1.0f : traffic / 20.0f;
        for (size_t s = 0; s < g_startPos.size(); ++s)
            if ((g_startPos[s] - sp).MagSqr() < 600.0f * 600.0f)
            {
                score -= 1.0f;
                break;
            }
        int walks = 0;
        const avp2bots::NavNode& node = g_nav.Node(n);
        for (size_t l = 0; l < node.links.size(); ++l)
            walks += node.links[l].kind == avp2bots::NAV_WALK ? 1 : 0;
        if (walks <= 2)
            score += 0.3f;
        Cand c = {n, score};
        cands.push_back(c);
    }
    int best = -1;
    float bestScore = -1e9f;
    for (size_t i = 0; i < cands.size(); ++i)
        if (cands[i].score > bestScore)
        {
            best = cands[i].n;
            bestScore = cands[i].score;
        }
    if (pScore)
        *pScore = bestScore;
    return best;
}

} // namespace

int BotMgr_CbHide(CAI* pAI, bool following, bool pathDone, LTVector* pDest)
{
    if (!g_cfg.cbHide || !g_navLoaded || !pAI || !pDest)
        return BOTCB_NONE;
    Bot* pb = FindBot(pAI->m_hObject);
    if (!pb || pb->stage != 2)
        return BOTCB_NONE;
    Bot& b = *pb;
    CbHideState& h = b.cbh;
    const LTFLOAT now = g_pLTServer->GetTime();
    const LTVector pos = pAI->GetPosition();
    const float hp = pAI->GetHitPoints();
    if (h.since == 0.0f)
    {
        h.since = now;
        h.lastHp = hp;
        h.lastPos = pos;
    }

    // threats, twice a second
    if (now >= h.nextCheck)
    {
        h.nextCheck = now + 0.5f;
        std::vector<CCharacter*> enemies;
        CbEnemies(pAI, kCbSeenRange, enemies);
        // an enemy sees it: a clear line, and close or facing it (the AI's CAITarget::IsFacing: within 60 degrees);
        // one is closer to its hiding spot than it is (the spot is no good any more)
        float nearest = 1e9f;
        bool seen = false, spotTaken = false;
        for (size_t e = 0; e < enemies.size(); ++e)
        {
            const LTVector ep = enemies[e]->GetPosition();
            const float d = (ep - pos).Mag();
            if (!CbVisible(enemies[e], pos))
                continue;
            if (d < nearest)
                nearest = d;
            LTVector to = pos - ep;
            to.y = 0.0f;
            LTVector f = enemies[e]->GetForward();
            f.y = 0.0f;
            const bool facing = to.MagSqr() > 1.0f && f.MagSqr() > 0.01f && f.Dot(to) > 0.5f * to.Mag() * f.Mag();
            if (d < kCbClose || facing)
                seen = true;
            if (h.state == CBH_GOING && (ep - h.spot).MagSqr() < (h.spot - pos).MagSqr())
                spotTaken = true;
        }
        const bool hurt = hp < h.lastHp - 0.5f;
        h.lastHp = hp;
        const float moved = (pos - h.lastPos).Mag();
        h.lastPos = pos;
        if (h.state == CBH_FIGHT)
        {
            if (nearest < kCbCalm)
                h.calmSince = 0.0f;
            else if (h.calmSince == 0.0f)
                h.calmSince = now;
            else if (now - h.calmSince > 2.0f)
            {
                SendCommand(pAI->m_hObject, "KG Attack");
                h.state = CBH_NONE;
                h.nextPick = 0.0f;
                CbLog(b, "calm", pos, 0);
            }
            return BOTCB_STAY;
        }
        // trapped: an enemy right on it while it cannot get away (hidden, or not moving), after its first second
        // out of the body (the burst is where the host fell, often among enemies: run first)
        if (nearest < kCbCornered && now - h.since > 1.5f && (h.state == CBH_HIDDEN || moved < 24.0f))
        {
            SendCommand(pAI->m_hObject, "AG Attack");
            h.state = CBH_FIGHT;
            h.calmSince = 0.0f;
            ++h.fights;
            char extra[64];
            sprintf(extra, " enemy=%.0f moved=%.0f", nearest, moved);
            CbLog(b, "fight", pos, extra);
            return BOTCB_STAY;
        }
        // running: only an enemy close by or a hit turns it round (a far enemy closer to the spot is no reason)
        const bool flee = hurt || (h.state == CBH_HIDDEN && seen) || (h.state == CBH_GOING && seen && nearest < kCbClose);
        if (flee && h.state != CBH_NONE && now >= h.nextPick)
        {
            h.state = CBH_NONE;
            ++h.flees;
            char extra[64];
            sprintf(extra, " reason=%s enemy=%.0f", hurt ? "hurt" : spotTaken ? "spottaken" : "seen",
                    nearest < 1e8f ? nearest : -1.0f);
            CbLog(b, "flee", pos, extra);
        }
    }
    else if (h.state == CBH_FIGHT)
        return BOTCB_STAY;

    if (h.state == CBH_NONE)
    {
        if (now < h.nextPick)
            return following ? BOTCB_KEEP : BOTCB_STAY;
        h.nextPick = now + 2.0f;   // at most one new spot every 2 s
        std::vector<CCharacter*> enemies;
        CbEnemies(pAI, 3000.0f, enemies);
        float score = 0.0f;
        const int n = CbPickSpot(b, pAI, enemies, &score);
        if (n < 0)
        {
            CbLog(b, "nospot", pos, 0);
            return BOTCB_STAY;
        }
        const Vec3& np = g_nav.Node(n).pos;
        h.spot = LTVector(np.x, np.y + pAI->GetDims().y, np.z);
        h.state = CBH_GOING;
        ++h.picks;
        char extra[96];
        sprintf(extra, " spot=%.0f %.0f %.0f trip=%.0f score=%.2f enemies=%d", h.spot.x, h.spot.y, h.spot.z,
                (h.spot - pos).Mag(), score, (int)enemies.size());
        CbLog(b, "go", pos, extra);
        *pDest = h.spot;
        return BOTCB_GO;
    }
    if (h.state == CBH_GOING)
    {
        if (pathDone || (pos - h.spot).MagSqr() < 64.0f * 64.0f)
        {
            h.state = CBH_HIDDEN;
            h.hiddenAt = now;
            CbLog(b, "hidden", pos, 0);
            return BOTCB_STAY;
        }
        if (!following)
        {
            h.state = CBH_NONE;  // the path failed or ran out: another spot
            return BOTCB_STAY;
        }
        return BOTCB_KEEP;
    }
    return BOTCB_STAY;  // hidden: keep still and quiet
}

void BotMgr_CbHideFailed(CAI* pAI)
{
    Bot* pb = pAI ? FindBot(pAI->m_hObject) : 0;
    if (pb && pb->cbh.state == CBH_GOING)
        pb->cbh.state = CBH_NONE;
}

bool BotMgr_CanWallWalk(CAI* pAI)
{
    // the class's CanWallWalk (a chestburster cannot), besides bots.cfg 'wallwalk'
    return pAI && g_cfg.wallwalk && pAI->GetMovement()->GetCharacterButes()->m_bWallWalk;
}

bool BotMgr_AmbushPick(CAI* pAI, LTVector* pDest)
{
    if (!g_cfg.wallAmbush || !g_cfg.wallwalk || !g_navLoaded || !pAI || !pDest || g_ambushSpots.empty())
        return false;
    Bot* pb = FindBot(pAI->m_hObject);
    if (!pb || pb->species != BOT_ALIEN || pb->stage != 0 || pAI->HasTarget())
        return false;
    Bot& b = *pb;
    const LTFLOAT now = g_pLTServer->GetTime();
    if (b.amb.state != AMB_NONE || now < b.amb.nextTry)
        return false;
    // "sometimes": one roam destination in two (my choice), then not again for 10-20 s
    if (GetRandom(0.0f, 1.0f) >= 0.5f)
    {
        b.amb.nextTry = now + GetRandom(10.0f, 20.0f);
        return false;
    }
    const LTVector& pos = pAI->GetPosition();
    int best = -1;
    float bestScore = 0.0f;
    for (int k = 0; k < 32; ++k)
    {
        const int i = GetRandom(0, (int)g_ambushSpots.size() - 1);
        const AmbushSpot& s = g_ambushSpots[i];
        if (g_nav.IsBlocked(s.from))
            continue;
        const LTVector e(s.enter.x, s.enter.y, s.enter.z);
        if ((e - pos).MagSqr() > kAmbushTripMax * kAmbushTripMax)
            continue;
        bool taken = false;  // another alien bot's spot nearby
        for (size_t j = 0; j < g_bots.size() && !taken; ++j)
            if (g_bots[j].alive && &g_bots[j] != &b && g_bots[j].amb.state != AMB_NONE && g_bots[j].amb.spot >= 0)
                taken = avp2bots::Dist(g_ambushSpots[g_bots[j].amb.spot].wall, s.wall) < 300.0f;
        if (taken)
            continue;
        const float sc = SpotScore(s);
        if (sc > bestScore)
        {
            best = i;
            bestScore = sc;
        }
    }
    if (best < 0)
    {
        b.amb.nextTry = now + 5.0f;
        return false;
    }
    const AmbushSpot& s = g_ambushSpots[best];
    b.amb.state = AMB_GOING;
    b.amb.spot = best;
    b.amb.since = now;
    *pDest = LTVector(s.wall.x, s.wall.y, s.wall.z);
    ++g_ambStarted;
    Log("AMBUSH GO %s spot=%d wall=%.0f %.0f %.0f height=%.0f score=%.0f from=%.0f %.0f %.0f", b.name.c_str(), best,
        s.wall.x, s.wall.y, s.wall.z, s.wall.y - s.enter.y, bestScore, pos.x, pos.y, pos.z);
    return true;
}

int BotMgr_AmbushUpdate(CAI* pAI, bool following, bool pathDone)
{
    if (!pAI)
        return AMB_NONE;
    Bot* pb = FindBot(pAI->m_hObject);
    if (!pb || pb->amb.state == AMB_NONE)
        return AMB_NONE;
    Bot& b = *pb;
    AmbushState& a = b.amb;
    const LTFLOAT now = g_pLTServer->GetTime();
    if (a.spot < 0 || a.spot >= (int)g_ambushSpots.size())
    {
        AmbushEnd(b, "nospot", false);
        return AMB_NONE;
    }
    const AmbushSpot& s = g_ambushSpots[a.spot];
    const LTVector pos = pAI->GetPosition();
    CharacterMovement* cmv = pAI->GetMovement();
    if (a.state == AMB_GOING)
    {
        if (pathDone)
        {
            // on the wall: wall-walking, near the wall point (the AI's centre is about its dims off the face)
            const LTVector w(s.wall.x, s.wall.y, s.wall.z);
            const float d = (pos - w).Mag();
            if ((pAI->GetMovementFlags() & CM_FLAG_WALLWALK) && d < 96.0f && pAI->GetUp().y < 0.9f)
            {
                a.state = AMB_WAITING;
                a.since = now;
                // how long a person waits is unknown: 10-20 s (my choice)
                a.until = now + GetRandom(10.0f, 20.0f);
                a.waitPos = pos;
                ++g_ambWaits;
                Log("AMBUSH WAIT %s at=%.0f %.0f %.0f wall=%.0f up=%.2f %.2f %.2f cms=%s", b.name.c_str(), pos.x, pos.y,
                    pos.z, d, pAI->GetUp().x, pAI->GetUp().y, pAI->GetUp().z,
                    cmv->GetMovementStateName(cmv->GetMovementState()));
                return a.state;
            }
            Log("AMBUSH MISS %s at=%.0f %.0f %.0f wall=%.0f up=%.2f wallwalk=%d cms=%s", b.name.c_str(), pos.x, pos.y,
                pos.z, d, pAI->GetUp().y, (int)((pAI->GetMovementFlags() & CM_FLAG_WALLWALK) != 0),
                cmv->GetMovementStateName(cmv->GetMovementState()));
            AmbushEnd(b, "notonwall", false);
            return AMB_NONE;
        }
        if (!following)
        {
            AmbushEnd(b, "lostpath", false);
            return AMB_NONE;
        }
        return a.state;
    }
    // waiting: keep hanging on the wall
    pAI->WallWalk();
    if (now > a.until)
        AmbushEnd(b, "timeout", false);
    else if ((pos - a.waitPos).Mag() > 48.0f || !(pAI->GetMovementFlags() & CM_FLAG_WALLWALK))
        AmbushEnd(b, "fell", false);
    return a.state;
}

void BotMgr_AmbushEnd(CAI* pAI, const char* why)
{
    if (!pAI)
        return;
    Bot* pb = FindBot(pAI->m_hObject);
    if (!pb || pb->amb.state == AMB_NONE)
        return;
    // the roam goal ends because something better came up (an enemy seen or heard): while waiting, that springs
    // the ambush (the attack goal sets its target only after this)
    AmbushEnd(*pb, why, pb->amb.state == AMB_WAITING);
}


bool BotMgr_LungeTargetOK(const LTVector& vTarget)
{
    // the target stands on (or right above) the waypoint graph
    if (!g_navLoaded)
        return false;
    const Vec3 feet(vTarget.x, vTarget.y - 60.0f, vTarget.z);
    const int n = g_nav.Nearest(feet);
    return n >= 0 && avp2bots::Dist(feet, g_nav.Node(n).pos) < 160.0f;
}

// --- pounce ---------------------------------------------------------------------------------------------------
// A player pounces with a key (CGameClientShell::HandlePounceJump): at most every 2 s (g_vtPounceDelay), from the
// ground, a wall or a ladder (PlayerMovement::CanPounce), straight along his view lifted by up to 15 degrees at
// the class's PounceSpeed under half gravity (CharacterMovementState_Pre_PounceJump / _Pounce_Jump); the auto-target
// pounce is commented out in the client (AutoTargetMgr). So a person pounces whenever a target is within the reach
// of that flight, in sight with a clear path, and the 2 s are up. Bots keep the AI's pounce flight (AIActionPounce,
// straight at the target with the target's speed led, the class's PounceSpeed, a 10 % arc) and the players'
// damage and stun (BotMgr_PounceHit); this decides when.
namespace {
const float kPounceCooldown = 2.0f;    // GameClientShell.cpp g_vtPounceDelay
const float kPounceLiftDeg = 15.0f;    // CharacterMovementState_Pre_PounceJump: a level view is lifted by 15 degrees

// the flat reach of a player's pounce aimed level: v^2 sin(2 x 15 deg) / (g / 2) (the pounce flight halves gravity)
float PounceReach(float speed)
{
    LTVector g(0.0f, 0.0f, 0.0f);
    g_pLTServer->GetGlobalForce(&g);
    const float half = 0.5f * g.Mag();
    return half > 1.0f ? speed * speed * sinf(2.0f * kPounceLiftDeg * 3.14159265f / 180.0f) / half : 0.0f;
}

// the bots' sight reaction (RegisterAllWeapons): 0.2 s within 400 units, rising to 1 s at 2000
float ReactionTime(float dist)
{
    return 0.2f + 0.8f * Clamp((dist - 400.0f) / 1600.0f, 0.0f, 1.0f);
}

void LogPounceReach()
{
    LTVector g(0.0f, 0.0f, 0.0f);
    g_pLTServer->GetGlobalForce(&g);
    const char* const kSets[] = {"Drone_MP", "Runner_MP", "Predalien_MP"};
    char buf[256];
    int len = sprintf(buf, "POUNCEREACH gravity=%.0f", g.y);
    for (int i = 0; i < 3; ++i)
    {
        const int set = g_pCharacterButeMgr->GetSetFromModelType(kSets[i]);
        if (set < 0)
            continue;
        const float v = g_pCharacterButeMgr->GetCharacterButes(set).m_fBasePounceSpeed;
        len += sprintf(buf + len, " %s=%.0f(speed %.0f)", kSets[i], PounceReach(v), v);
    }
    Log("%s", buf);
}
} // namespace

bool BotMgr_PounceRule(CAI* pAI)
{
    if (!g_cfg.pounce || !pAI)
        return false;
    Bot* b = FindBot(pAI->m_hObject);
    return b && b->species == BOT_ALIEN && b->stage == 0;
}

bool BotMgr_PounceNow(CAI* pAI)
{
    Bot* b = pAI ? FindBot(pAI->m_hObject) : 0;
    if (!b)
        return false;
    const LTFLOAT now = g_pLTServer->GetTime();
    const char* why = 0;
    const CAITarget& t = pAI->GetTarget();
    CCharacter* tc = t.IsValid() ? dynamic_cast<CCharacter*>(g_pLTServer->HandleToObject(t.GetObject())) : LTNULL;
    const CharacterMovementState cms = pAI->GetMovement()->GetMovementState();
    const float speed = pAI->GetButes() ? pAI->GetButes()->m_fBasePounceSpeed : 0.0f;
    const float reach = PounceReach(speed);
    float dist = 0.0f;
    if (now < b->pounceReadyAt)
        why = "cooldown";
    else if (!tc || tc->IsDead() || speed <= 0.0f)
        why = "target";
    else if (!(pAI->GetMovement()->IsStandingOn() || cms == CMS_WALLWALKING))
        why = "air";   // the ground or a wall (aliens do not use ladders)
    else
    {
        // where the AI's pounce goes: the target's chest, led by its speed over the flight (Character.cpp MID_FORCE_POUNCE)
        const LTVector& p = pAI->GetPosition();
        LTVector aim = tc->GetPosition() + tc->GetChestOffset();
        dist = (aim - p).Mag();
        aim += tc->GetMovement()->GetVelocity() * (dist / speed * 1.1f);
        const float clawReach = b->kitClawsBarrel ? (float)b->kitClawsBarrel->nRange : 150.0f;
        if (dist <= clawReach)
            why = "close";     // within the claws' reach a person claws
        else if (dist > reach)
            why = "far";
        else if (!t.IsFullyVisible())
            why = "unseen";
        else
        {
            // a clear path: the straight line to the aim point with the flight's arc (10 % of the distance at mid-way)
            const LTVector mid = (p + aim) * 0.5f + LTVector(0.0f, 0.1f * (aim - p).Mag(), 0.0f);
            if (!ClearLine(p, mid) || !ClearLine(mid, aim))
                why = "blocked";
            else if (!BotMgr_LungeTargetOK(aim))
                why = "offgraph";   // the bot must find its way again after landing
        }
    }
    if (why)
    {
        b->pounceSeenSince = 0.0f;
        return false;
    }
    // the opening has lasted a person's reaction time
    if (b->pounceSeenSince == 0.0f)
        b->pounceSeenSince = now;
    if (now - b->pounceSeenSince < ReactionTime(dist))
        return false;
    b->pounceDecision = dist;
    b->pounceWaited = (float)(now - b->pounceSeenSince);
    return true;
}

void BotMgr_PounceStarted(CAI* pAI)
{
    Bot* b = pAI ? FindBot(pAI->m_hObject) : 0;
    if (!b)
        return;
    b->pounceReadyAt = g_pLTServer->GetTime() + kPounceCooldown;
    b->pounceSeenSince = 0.0f;
    if (g_cfg.debug)
    {
        const float speed = pAI->GetButes() ? pAI->GetButes()->m_fBasePounceSpeed : 0.0f;
        Log("POUNCE %s class=%s -> %s dist=%.0f reach=%.0f waited=%.2f from=%s", b->name.c_str(), ClassName(*b),
            ObjectLabel(pAI->GetTarget().GetObject()).c_str(), b->pounceDecision, PounceReach(speed), b->pounceWaited,
            pAI->GetMovement()->GetMovementState() == CMS_WALLWALKING ? "wall" : "ground");
    }
}

// --- the pounce flight ------------------------------------------------------------------------------------------
// A player's pounce (CharacterMovementState_Pre_PounceJump / _Pounce_Jump) is launched along his view, lifted by
// 15 degrees x (1 - the view's upward part) (a view below the horizon launches level, then lifted), at the class's
// PounceSpeed, under half gravity, and flies on until it touches something (Touch_Notify_Pounce_Jump). The AI's pounce
// (CMS_POUNCING) flies a fixed arc to its predicted point and stops there, so a target that moved meanwhile was
// missed although a player's flight would have gone on into it. A bot (bots.cfg pounceflight) flies like a player along
// the AI's aim (the target's chest, led by its speed); the AI's touch handling (Touch_Notify_Pouncing: damage, stun)
// still ends it. When the flight, had it missed, would come down off the waypoint graph (a pit, a ledge, a hazard)
// or nowhere within 3 s, the bot keeps the AI's flight.
namespace {
const float kFlightMax = 3.0f;   // longest flight followed (s); a level Drone pounce lands after ~0.6 s

LTVector HalfGravity()
{
    LTVector g(0.0f, 0.0f, 0.0f);
    g_pLTServer->GetGlobalForce(&g);
    return g * 0.5f;   // Pounce_Jump: "vVel += vForce * frametime * 0.5" with FLAG_GRAVITY off
}

// solid world models count as ground (outdoor maps such as Compound build their terrain from them); characters,
// bodies and props do not
LTBOOL FlightFilter(HOBJECT hObj, void*)
{
    return g_pLTServer->GetObjectType(hObj) == OT_WORLDMODEL;
}

bool FlightSegment(const LTVector& a, const LTVector& b, IntersectInfo* ii)
{
    IntersectQuery iq;
    iq.m_From = a;
    iq.m_To = b;
    iq.m_Flags = INTERSECT_OBJECTS | IGNORE_NONSOLID;
    iq.m_FilterFn = FlightFilter;
    return g_pLTServer->IntersectSegment(&iq, ii) != LTFALSE;
}

// where a flight from p with velocity v under acceleration a first meets the world (world geometry and world models)
bool FlightImpact(LTVector p, LTVector v, const LTVector& a, LTVector* hit, LTVector* normal)
{
    const float dt = 0.05f;
    for (float t = 0.0f; t < kFlightMax; t += dt)
    {
        const LTVector q = p + v * dt + a * (0.5f * dt * dt);
        IntersectInfo ii;
        if (FlightSegment(p, q, &ii))
        {
            *hit = ii.m_Point;
            *normal = ii.m_Plane.m_Normal;
            return true;
        }
        v += a * dt;
        p = q;
    }
    return false;
}

// the floor a missed flight ends on: the hit itself on a floor, else straight below a wall or ceiling hit; on the graph?
bool LandingOnGraph(const LTVector& hit, const LTVector& normal, LTVector* floor)
{
    *floor = hit;
    if (normal.y < 0.5f)
    {
        IntersectInfo ii;
        const LTVector from = hit + normal * 24.0f;
        if (!FlightSegment(from, from - LTVector(0.0f, 2000.0f, 0.0f), &ii))
            return false;
        *floor = ii.m_Point;
    }
    const Vec3 f(floor->x, floor->y, floor->z);
    const int n = g_nav.Nearest(f);
    return n >= 0 && avp2bots::Dist(f, g_nav.Node(n).pos) < 160.0f;   // the lunge check's tolerance
}

void EndFlight(CharacterMovement* m, CharacterVars* cv, const LTVector& vel)
{
    // as CharacterMovementState_Pouncing ends (upright, flags back), always onto the floor: no wall walking after
    // a bot's pounce (a bot left on a wall has no path)
    LTVector flat(vel.x, 0.0f, vel.z);
    if (flat.MagSqr() < 1.0f)
        flat = cv->m_vForward;
    LTRotation rRot;
    LTVector vUp(0.0f, 1.0f, 0.0f);
    m->m_pMath->AlignRotation(rRot, flat, vUp);
    m->SetRot(rRot);
    m->SetObjectFlags(OFT_Flags, cv->m_dwFlags, LTFALSE);
    m->SetObjectFlags(OFT_Flags2, cv->m_dwFlags2 & ~FLAG2_ORIENTMOVEMENT & ~FLAG2_SPHEREPHYSICS, LTFALSE);
    if (m->IsInLiquidContainer(CM_CONTAINER_MID))
        m->SetMovementState(CMS_SWIMMING);
    else
    {
        m->SetObjectDims(CM_DIMS_STANDING);
        m->SetMovementState(CMS_PRE_FALLING);
    }
}
} // namespace

bool BotMgr_PounceFlight(CharacterMovement* m)
{
    if (!m)
        return false;
    Bot* b = FindBot(m->GetObject());
    const bool hug = m->GetMovementState() == CMS_FACEHUG;
    if (!b || b->species != BOT_ALIEN || b->stage != (hug ? 1 : 0) || !(hug ? g_cfg.hugLike : g_cfg.pounceFlight))
        return false;
    CharacterVars* cv = m->GetCharacterVars();
    const LTFLOAT now = g_pLTServer->GetTime();

    // a new pounce (MID_FORCE_POUNCE stamps its start time): the player's launch, if its landing is safe
    if (cv->m_fStampTime != -1.0f && cv->m_fStampTime != b->flightStamp)
    {
        b->flightStamp = cv->m_fStampTime;
        b->flightPlayer = false;
        const float speed = m->GetCharacterButes()->m_fBasePounceSpeed;
        LTVector dir = cv->m_vDestination - cv->m_vStart;
        if (speed <= 0.0f || dir.MagSqr() < 1.0f)
            return false;
        dir.Norm();
        LTVector up = cv->m_vUp;
        if (up.MagSqr() < 0.01f)
            up = LTVector(0.0f, 1.0f, 0.0f);
        up.Norm();
        float ud = dir.Dot(up);
        if (ud > 1.0f)
            ud = 1.0f;
        if (ud < 0.0f)
        {
            // a view below the horizon launches level (Pre_PounceJump uses the character's forward)
            ud = 0.0f;
            dir -= up * dir.Dot(up);
            if (dir.MagSqr() < 0.0001f)
                return false;
            dir.Norm();
        }
        LTVector side = up - dir * dir.Dot(up);
        LTVector launch = dir;
        if (side.MagSqr() > 0.0001f)
        {
            side.Norm();
            const float a = (1.0f - ud) * 15.0f * 3.14159265f / 180.0f;
            launch = dir * cosf(a) + side * sinf(a);
        }
        const LTVector vel = launch * speed;
        LTVector hit, normal, floor;
        const bool lands = FlightImpact(cv->m_vPos, vel, HalfGravity(), &hit, &normal);
        const bool safe = lands && LandingOnGraph(hit, normal, &floor);
        if (g_cfg.debug)
            Log("POUNCEFLIGHT %s class=%s mode=%s lift=%.1f miss_lands=%.0f %.0f %.0f%s", b->name.c_str(), ClassName(*b),
                safe ? "player" : "ai", (1.0f - ud) * 15.0f, lands ? floor.x : 0.0f, lands ? floor.y : 0.0f,
                lands ? floor.z : 0.0f, !lands ? " (nothing within 3 s)" : (safe ? "" : " (off the graph)"));
        if (!safe)
            return false;
        b->flightPlayer = true;
        b->flightVel = vel;
        b->flightStart = now;
    }
    if (!b->flightPlayer)
        return false;   // the AI's flight for this pounce

    // the player's flight: no engine gravity, pounce dims, half gravity by hand, on until it touches something
    m->SetObjectFlags(OFT_Flags, (cv->m_dwFlags & ~FLAG_GRAVITY) | FLAG_DONTFOLLOWSTANDING, LTFALSE);
    m->SetObjectFlags(OFT_Flags2, cv->m_dwFlags2 & ~(FLAG2_SEMISOLID | FLAG2_ORIENTMOVEMENT | FLAG2_SPHEREPHYSICS), LTFALSE);
    m->SetObjectDims(CM_DIMS_POUNCING);
    m->m_pPhysics->SetFrictionCoefficient(cv->m_hObject, 0.0f);
    if (cv->m_fStampTime == -1.0f || now - b->flightStart > kFlightMax || m->IsInLiquidContainer(CM_CONTAINER_MID))
    {
        // touched something solid (Touch_Notify_Pouncing has done the damage), or flown too long
        b->flightPlayer = false;
        EndFlight(m, cv, b->flightVel);
        return true;
    }
    if (hug)
    {
        // a facehugger's leap ignores the world in its touch handling (Touch_Notify_Facehug: only characters end
        // it); a player's leap ends on the first wall or floor it meets (Pounce_Jump_TN): look one step ahead
        const float step = b->flightVel.Mag() * cv->m_fFrameTime * 1.5f;
        LTVector ahead = b->flightVel;
        IntersectInfo ii;
        if (ahead.MagSqr() > 1.0f)
        {
            ahead.Norm();
            if (FlightSegment(cv->m_vPos, cv->m_vPos + ahead * (step > 16.0f ? step : 16.0f), &ii))
            {
                b->flightPlayer = false;
                EndFlight(m, cv, b->flightVel);
                return true;
            }
        }
    }
    b->flightVel += HalfGravity() * cv->m_fFrameTime;
    m->SetVelocity(b->flightVel);
    LTVector flat(b->flightVel.x, 0.0f, b->flightVel.z);
    if (flat.MagSqr() > 1.0f)
    {
        LTRotation rRot;
        LTVector vUp(0.0f, 1.0f, 0.0f);
        m->m_pMath->AlignRotation(rRot, flat, vUp);
        m->SetRot(rRot);
    }
    return true;
}

// --- facehuggers (lifecycle) ------------------------------------------------------------------------------------
// A facehugger player (Facehugger_MP: 25 hp, runs at 550, PounceSpeed 1000, walks on walls) leaps with either fire
// button (CGameClientShell: COMMAND_ID_FIRING / ALT_FIRING force HandlePounceJump for a facehugger): the adults'
// pounce jump, at most every 2 s, lifted 15 degrees, under half gravity, on until it touches something; touching a
// character that can be hugged (CanBeFacehugged: Marines, Predators; not aliens, exosuits, synthetics) hugs it
// (CPlayerObj CP_POUNCE_DAMAGE -> DT_FACEHUG). The AI facehugger (BasicFaceHugger) leaps at its target within 250
// units on a 1-3 s timer, charges whoever it sees, and stands where it lost its target for 5-12 s (the attack
// goal's wait): in the first lifecycle tests bot huggers stood still in 91 % of those waits and lived 9-10 s.
// A bot facehugger (bots.cfg huglike) hunts like a person instead:
// - its leap follows the players' rule (BotMgr_HugNow: in reach, a clear arc, a landing on the graph, a person's
//   reaction, the 2 s cooldown) and flies like a player's (BotMgr_PounceFlight);
// - it picks its victim (HugHunt): a stunned or netted one first, then one that is not looking at it and has nobody
//   within leap reach to shoot it, then one not looking at it, then one alone, then the nearest;
// - it does not charge a victim that looks at it with company while it is still out of leap reach (HugAvoid: such
//   a victim is ignored, as a person backs off and waits for an opening);
// - with no victim it keeps moving (BotRoam outbids the attack goal's wait and the investigation).
namespace {
const float kHugSight = 2000.0f;   // the bots' sight reaction reaches its slowest (1 s) at 2000 units

bool Huggable(const CCharacter* c)
{
    return c && !c->IsDead() && c->GetButes() && c->GetButes()->m_bCanBeFacehugged;
}

// the character looks at p (within its half view of 45 degrees: a 90 degree view, a player's default field of view)
bool LooksAt(const CCharacter* c, const LTVector& p)
{
    LTVector to = p - c->GetPosition();
    to.y = 0.0f;
    LTVector f = c->GetForward();
    f.y = 0.0f;
    if (to.MagSqr() < 1.0f || f.MagSqr() < 0.0001f)
        return true;
    to.Norm();
    f.Norm();
    return f.Dot(to) > 0.7071f;
}

// someone besides the victim who could shoot a hugger leaping at it (any non-alien character within leap reach)
bool HasCompany(const CCharacter* victim, const CCharacter* hugger, float reach)
{
    for (CCharacterMgr::CharacterIterator it = g_pCharacterMgr->BeginCharacters(); it != g_pCharacterMgr->EndCharacters(); ++it)
    {
        const CCharacter* o = *it;
        if (!o || o == victim || o == hugger || o->IsDead() || IsAlien(o))
            continue;
        if ((o->GetPosition() - victim->GetPosition()).MagSqr() < reach * reach)
            return true;
    }
    return false;
}

float HugReach(const CAI* ai)
{
    return PounceReach(ai->GetButes() ? ai->GetButes()->m_fBasePounceSpeed : 0.0f);
}

// a victim to leave alone for now: it looks at the hugger, has company and the hugger is out of leap reach
bool HugAvoid(const Bot& b, CAI* ai, const CCharacter* victim)
{
    if (!g_cfg.hugLike || b.stage != 1 || !Huggable(victim))
        return false;
    const float reach = HugReach(ai);
    if ((victim->GetPosition() - ai->GetPosition()).MagSqr() <= reach * reach)
        return false;
    return LooksAt(victim, ai->GetPosition()) && HasCompany(victim, ai, reach);
}

int HugTier(CAI* ai, const CCharacter* v, float reach)
{
    if (v->IsStunned() || v->IsNetted())
        return 0;
    const bool unaware = !LooksAt(v, ai->GetPosition());
    const bool alone = !HasCompany(v, ai, reach);
    return unaware && alone ? 1 : unaware ? 2 : alone ? 3 : 4;
}

// twice a second: the victim a person would go for among the huggable enemies in sight
void HugHunt(Bot& b, CAI* ai, LTFLOAT now)
{
    if (!g_cfg.hugLike || b.stage != 1 || now < b.hugHuntAt)
        return;
    b.hugHuntAt = now + 0.5f;
    if (ai->GetMovement()->GetMovementState() == CMS_FACEHUG || ai->GetMovement()->GetMovementState() == CMS_FACEHUG_ATTACH)
        return;   // leaping or hugging
    const float reach = HugReach(ai);
    const LTVector& p = ai->GetPosition();
    CCharacter* best = 0;
    int bestTier = 99;
    float bestDist = 0.0f;
    for (CCharacterMgr::CharacterIterator it = g_pCharacterMgr->BeginCharacters(); it != g_pCharacterMgr->EndCharacters(); ++it)
    {
        CCharacter* v = *it;
        if (!Huggable(v) || v == ai || g_pCharacterMgr->GetRelationship(*ai, *v) != CCharacterMgr::Foe)
            continue;
        const LTVector chest = v->GetPosition() + v->GetChestOffset();
        const float d = (chest - p).Mag();
        IntersectInfo ii;
        if (d > kHugSight || FlightSegment(p, chest, &ii))
            continue;   // out of sight
        const int tier = HugTier(ai, v, reach);
        if (!best || tier < bestTier || (tier == bestTier && d < bestDist))
        {
            best = v;
            bestTier = tier;
            bestDist = d;
        }
    }
    if (!best)
        return;
    // switch only to a better kind of victim (or when there is none): no flipping between equals
    const HOBJECT cur = ai->HasTarget() ? ai->GetTarget().GetObject().GetHOBJECT() : LTNULL;
    CCharacter* cc = cur ? dynamic_cast<CCharacter*>(g_pLTServer->HandleToObject(cur)) : LTNULL;
    if (cc == best)
        return;
    if (cc && Huggable(cc) && HugTier(ai, cc, reach) <= bestTier)
        return;
    ai->Target(best->m_hObject);
    if (g_cfg.debug)
        Log("HUGTARGET %s -> %s tier=%d dist=%.0f (was %s)", b.name.c_str(), ObjectLabel(best->m_hObject).c_str(), bestTier,
            bestDist, cur ? ObjectLabel(cur).c_str() : "-");
}
} // namespace

bool BotMgr_HugRule(CAI* pAI)
{
    if (!g_cfg.hugLike || !pAI)
        return false;
    Bot* b = FindBot(pAI->m_hObject);
    return b && b->species == BOT_ALIEN && b->stage == 1;
}

bool BotMgr_HugNow(CAI* pAI)
{
    Bot* b = pAI ? FindBot(pAI->m_hObject) : 0;
    if (!b)
        return false;
    const LTFLOAT now = g_pLTServer->GetTime();
    const char* why = 0;
    const CAITarget& t = pAI->GetTarget();
    CCharacter* tc = t.IsValid() ? dynamic_cast<CCharacter*>(g_pLTServer->HandleToObject(t.GetObject())) : LTNULL;
    const CharacterMovementState cms = pAI->GetMovement()->GetMovementState();
    const float speed = pAI->GetButes() ? pAI->GetButes()->m_fBasePounceSpeed : 0.0f;
    const float reach = PounceReach(speed);
    float dist = 0.0f;
    if (now < b->pounceReadyAt)
        why = "cooldown";
    else if (!Huggable(tc) || speed <= 0.0f)
        why = "target";
    else if (!(pAI->GetMovement()->IsStandingOn() || cms == CMS_WALLWALKING))
        why = "air";
    else
    {
        // the AI's leap aims at the target's chest led by its speed (Character.cpp MID_FORCE_FACEHUG)
        const LTVector& p = pAI->GetPosition();
        LTVector aim = tc->GetPosition() + tc->GetChestOffset();
        dist = (aim - p).Mag();
        aim += tc->GetMovement()->GetVelocity() * (dist / speed * 1.1f);
        if (dist > reach)
            why = "far";
        else if (!t.IsFullyVisible())
            why = "unseen";
        else
        {
            const LTVector mid = (p + aim) * 0.5f + LTVector(0.0f, 0.1f * (aim - p).Mag(), 0.0f);
            IntersectInfo ii;
            if (FlightSegment(p, mid, &ii) || FlightSegment(mid, aim, &ii))
                why = "blocked";
            else if (!BotMgr_LungeTargetOK(aim))
                why = "offgraph";
        }
    }
    if (why)
    {
        b->pounceSeenSince = 0.0f;
        return false;
    }
    if (b->pounceSeenSince == 0.0f)
        b->pounceSeenSince = now;
    if (now - b->pounceSeenSince < ReactionTime(dist))
        return false;
    b->pounceDecision = dist;
    b->pounceWaited = (float)(now - b->pounceSeenSince);
    return true;
}

void BotMgr_HugStarted(CAI* pAI)
{
    Bot* b = pAI ? FindBot(pAI->m_hObject) : 0;
    if (!b)
        return;
    b->pounceReadyAt = g_pLTServer->GetTime() + kPounceCooldown;
    b->pounceSeenSince = 0.0f;
    if (g_cfg.debug)
    {
        CCharacter* tc = dynamic_cast<CCharacter*>(g_pLTServer->HandleToObject(pAI->GetTarget().GetObject()));
        Log("HUGLEAP %s -> %s dist=%.0f reach=%.0f waited=%.2f tier=%d", b->name.c_str(),
            ObjectLabel(pAI->GetTarget().GetObject()).c_str(), b->pounceDecision, HugReach(pAI), b->pounceWaited,
            tc ? HugTier(pAI, tc, HugReach(pAI)) : -1);
    }
}

bool BotMgr_HugRoam(CAI* pAI)
{
    // a bot facehugger with no victim keeps moving (the attack goal would stand and wait 5-12 s)
    if (!g_cfg.hugLike || !pAI || pAI->HasTarget())
        return false;
    Bot* b = FindBot(pAI->m_hObject);
    if (!b || b->stage != 1)
        return false;
    // not while leaping or on a face (the engine holds a hugger on its victim until the chestburst)
    const CharacterMovementState cms = pAI->GetMovement()->GetMovementState();
    return cms != CMS_FACEHUG && cms != CMS_FACEHUG_ATTACH &&
           !(g_pLTServer->GetObjectUserFlags(pAI->m_hObject) & USRFLG_CHAR_FACEHUG);
}

BotNav* BotMgr_GetNav()
{
    return g_navLoaded ? &g_nav : 0;
}

LTBOOL BotMgr_FindPath(CAI* pAI, const LTVector& vDest, CAIPath* pPath)
{
    if (!g_navLoaded || !pAI || !pPath)
        return LTFALSE;

    const LTVector& p = pAI->GetPosition();
    const LTVector& dims = pAI->GetDims();
    int a = g_nav.Nearest(Vec3(p.x, p.y - dims.y, p.z));
    int b = g_nav.Nearest(Vec3(vDest.x, vDest.y - dims.y, vDest.z));
    // a wall ambush (BotMgr_AmbushPick): the path goes to the floor in front of the wall, then onto the wall point
    const AmbushSpot* ambush = 0;
    {
        const Bot* ab = FindBot(pAI->m_hObject);
        if (ab && ab->amb.state == AMB_GOING && ab->amb.spot >= 0 && ab->amb.spot < (int)g_ambushSpots.size())
        {
            const AmbushSpot& s = g_ambushSpots[ab->amb.spot];
            if ((LTVector(s.wall.x, s.wall.y, s.wall.z) - vDest).MagSqr() < 4.0f)
            {
                ambush = &s;
                b = s.from;
            }
        }
    }
    const LTFLOAT now = g_pLTServer->GetTime();
    std::map<HOBJECT, PathFail>::iterator cached = g_pathFails.find(pAI->m_hObject);
    if (cached != g_pathFails.end() && cached->second.from == a && cached->second.to == b && now < cached->second.until)
        return LTFALSE;

    // Aliens do not climb ladders, they climb walls; everyone else may climb ladders when enabled
    const bool alien = IsAlien(pAI);
    const int allowLadders = (g_cfg.ladders && !alien) ? 1 : 0;
    const bool allowClimb = alien && g_cfg.wallwalk && pAI->GetMovement()->GetCharacterButes()->m_bWallWalk;
    std::vector<Vec3> pts;
    std::vector<avp2bots::NavLinkKind> kinds;
    std::vector<avp2bots::NavLink> links;
    // wall approach: an alien going for a target that faces it, farther than a pounce, keeps off the open floor in
    // front of the target: walk links ending there (the target's floor level, inside the 60-degree half cone of
    // CAITarget::IsFacing) cost twice as much, wall climbs half as much (cancelling their x2; values my choice)
    avp2bots::PathBias bias;
    const avp2bots::PathBias* pBias = 0;
    if (alien && allowClimb && g_cfg.wallApproach && !ambush && pAI->HasTarget())
    {
        const CAITarget& t = pAI->GetTarget();
        const CCharacter* tc = t.GetCharacterPtr();
        if (tc && t.IsPartiallyVisible() && t.IsFacing())
        {
            LTVector tp;
            g_pLTServer->GetObjectPos(t.GetObject(), &tp);
            const float d = (tp - p).Mag();
            LTVector f = tc->GetForward();
            f.y = 0.0f;
            if (d >= kWallApproachMin && d <= kWallApproachMax && (vDest - tp).MagSqr() < 256.0f * 256.0f && f.MagSqr() > 0.01f)
            {
                f.Norm();
                bias.eye = Vec3(tp.x, tp.y - tc->GetDims().y, tp.z);
                bias.fx = f.x;
                bias.fz = f.z;
                bias.cosHalf = 0.5f;
                bias.range = d + 256.0f;
                bias.floorDy = 64.0f;
                bias.walkMul = 2.0f;
                bias.climbMul = 0.5f;
                pBias = &bias;
            }
        }
    }
    if (a < 0 || b < 0 || !g_nav.FindPath(a, b, &pts, 0, &kinds, allowLadders, &links, allowClimb, pBias))
    {
        // count the failure toward 'isolated' only when the bot stands on a small island of the graph
        // (failing to reach an unreachable target from the main area is normal)
        const bool island = a >= 0 && g_nav.ReachableCount(a, 40, allowLadders, allowClimb) < 40;
        const int streak = !island ? 0
                         : (cached != g_pathFails.end() && cached->second.from == a) ? cached->second.streak + 1 : 1;
        const bool had = cached != g_pathFails.end();
        PathFail f = {a, b, now + 2.0f, streak, had ? cached->second.lastLog : -100.0f, had ? cached->second.quiet : 0};
        if (g_cfg.debug || now - f.lastLog >= 5.0f)
        {
            Log("PATHFAIL ai=%s from=%d to=%d dest=%.0f %.0f %.0f (+%d not logged)", ObjectLabel(pAI->m_hObject).c_str(),
                a, b, vDest.x, vDest.y, vDest.z, f.quiet);
            f.lastLog = now;
            f.quiet = 0;
        }
        else
            ++f.quiet;
        g_pathFails[pAI->m_hObject] = f;
        return LTFALSE;
    }

    // One step per steering point (floor heights). Ladder links become: walk to the ladder's foot (or top),
    // climb with a LadderTo to the other end, step off to the next waypoint. Those steps are never dropped.
    struct Step
    {
        Vec3 pos;
        CAIPathWaypoint::Instruction instruction;
        bool fixed;
        bool floor;  // keep the floor height (the waypoint after a LadderTo, see below)
        Vec3 normal; // WallWalkTo / WWJumpTo: surface normal
    };
    std::vector<Step> steps;
    float prevFloor = p.y - dims.y;
    int ladders = 0, climbs = 0;
    const Vec3 up(0.0f, 1.0f, 0.0f);
    for (size_t i = 0; i < kinds.size(); ++i)
    {
        const Vec3& portal = pts[2 * i];
        const Vec3& node = pts[2 * i + 1];
        if (kinds[i] == avp2bots::NAV_JUMP)
        {
            // run to the ledge, then jump onto the higher floor (the jump action takes a floor position)
            Step s1 = {portal, CAIPathWaypoint::eInstructionMoveTo, true, false, up};
            Step s2 = {node, CAIPathWaypoint::eInstructionJumpTo, true, false, up};
            steps.push_back(s1);
            steps.push_back(s2);
        }
        else if (kinds[i] == avp2bots::NAV_CLIMB)
        {
            // An Alien climbs the wall like the single-player wall-walking AI: run to the wall (WallWalkTo a
            // point on its face, below the lip: the engine turns the alien onto the wall and up), then a
            // WWJumpTo onto the ledge (a short glide over the lip), then a MoveTo stands it up again.
            // Positions are on the surfaces (no height lift); the wall normal reaches the move strategy
            // through hook 'wwnormal'.
            const avp2bots::NavLink& l = links[i];
            Step s1 = {l.enter, CAIPathWaypoint::eInstructionMoveTo, true, false, up};
            Step s2 = {portal, CAIPathWaypoint::eInstructionWallWalkTo, true, true, l.normal};
            Step s3 = {l.exit, CAIPathWaypoint::eInstructionWWJumpTo, true, true, up};
            Step s4 = {node, CAIPathWaypoint::eInstructionMoveTo, false, false, up};
            steps.push_back(s1);
            steps.push_back(s2);
            steps.push_back(s3);
            steps.push_back(s4);
            ++climbs;
        }
        else if (kinds[i] == avp2bots::NAV_CLIMBDOWN)
        {
            // Down the same wall: walk to the ledge, a WWJumpTo glides over the lip onto the wall (wall normal),
            // a WallWalkTo walks down the wall and onto the floor (the engine turns the alien at the corner),
            // then a MoveTo stands it up.
            const avp2bots::NavLink& l = links[i];
            Step s1 = {l.enter, CAIPathWaypoint::eInstructionMoveTo, true, false, up};
            Step s2 = {portal, CAIPathWaypoint::eInstructionWWJumpTo, true, true, l.normal};
            Step s3 = {l.exit, CAIPathWaypoint::eInstructionWallWalkTo, true, true, up};
            Step s4 = {node, CAIPathWaypoint::eInstructionMoveTo, false, false, up};
            steps.push_back(s1);
            steps.push_back(s2);
            steps.push_back(s3);
            steps.push_back(s4);
            ++climbs;
        }
        else if (kinds[i] == avp2bots::NAV_LADDER)
        {
            // AIActionLadderTo climbs to the height of the waypoint that follows it plus the character's half
            // height (it expects a floor position there), then moves across to it and lets go. So the step-off
            // waypoint is a floor position; going up it is raised a little so the feet clear the ledge, since
            // the action stops within 12 units of its target height (hook 'ladderepsilon').
            const avp2bots::NavLink& l = links[i];
            const Vec3 enter = l.hasEnds ? l.enter : Vec3(portal.x, prevFloor, portal.z);
            const Vec3 exit = l.hasEnds ? l.exit : node;
            const float rise = exit.y > enter.y ? 16.0f : 0.0f;
            Step s1 = {enter, CAIPathWaypoint::eInstructionMoveTo, true, false, up};
            Step s2 = {Vec3(portal.x, exit.y, portal.z), CAIPathWaypoint::eInstructionLadderTo, true, false, up};
            Step s3 = {Vec3(exit.x, exit.y + rise, exit.z), CAIPathWaypoint::eInstructionMoveTo, true, true, up};
            Step s4 = {node, CAIPathWaypoint::eInstructionMoveTo, false, false, up};
            steps.push_back(s1);
            steps.push_back(s2);
            steps.push_back(s3);
            steps.push_back(s4);
            ++ladders;
        }
        else
        {
            Step s1 = {portal, CAIPathWaypoint::eInstructionMoveTo, false, false, up};
            Step s2 = {node, CAIPathWaypoint::eInstructionMoveTo, false, false, up};
            steps.push_back(s1);
            steps.push_back(s2);
        }
        prevFloor = node.y;
    }
    if (ambush)
    {
        // to the floor in front of the wall, then the WallWalkTo of a climb, which ends the path (see below)
        Step s1 = {ambush->enter, CAIPathWaypoint::eInstructionMoveTo, true, false, up};
        steps.push_back(s1);
    }
    if (pBias)
    {
        ++g_approachBiased;
        g_approachClimbs += climbs;
        if (g_cfg.debug && g_approachBiased % 10 == 1)
        {
            // what the stock search would have taken (diagnostics only, one path in ten)
            std::vector<Vec3> plainPts;
            std::vector<avp2bots::NavLinkKind> plainKinds;
            g_nav.FindPath(a, b, &plainPts, 0, &plainKinds, allowLadders, 0, allowClimb);
            int plainClimbs = 0, inCone = 0, plainInCone = 0;
            float len = 0.0f, plainLen = 0.0f;
            Vec3 q(p.x, p.y - dims.y, p.z);
            for (size_t i = 0; i < pts.size(); ++i)
            {
                len += avp2bots::Dist(q, pts[i]);
                q = pts[i];
                inCone += (i % 2 == 1 && bias.InCone(pts[i])) ? 1 : 0;
            }
            q = Vec3(p.x, p.y - dims.y, p.z);
            for (size_t i = 0; i < plainPts.size(); ++i)
            {
                plainLen += avp2bots::Dist(q, plainPts[i]);
                q = plainPts[i];
                plainInCone += (i % 2 == 1 && bias.InCone(plainPts[i])) ? 1 : 0;
            }
            for (size_t i = 0; i < plainKinds.size(); ++i)
                plainClimbs += (plainKinds[i] == avp2bots::NAV_CLIMB || plainKinds[i] == avp2bots::NAV_CLIMBDOWN) ? 1 : 0;
            Log("WALLAPPROACH ai=%s climbs=%d (plain %d) length=%.0f (plain %.0f) waypoints in the open=%d (plain %d)",
                ObjectLabel(pAI->m_hObject).c_str(), climbs, plainClimbs, len, plainLen, inCone, plainInCone);
        }
    }

    // drop plain steering points that barely change direction (keeps the AI from stuttering every 32 units)
    std::vector<Step> simple;
    Vec3 prev(p.x, p.y - dims.y, p.z);
    for (size_t i = 0; i < steps.size(); ++i)
    {
        if (!steps[i].fixed && i + 1 < steps.size() && !simple.empty())
        {
            const Vec3& cur = steps[i].pos;
            const Vec3& nxt = steps[i + 1].pos;
            float ax = cur.x - prev.x, az = cur.z - prev.z;
            float bx = nxt.x - cur.x, bz = nxt.z - cur.z;
            float la = sqrtf(ax * ax + az * az), lb = sqrtf(bx * bx + bz * bz);
            bool straight = la > 1.0f && lb > 1.0f && (ax * bx + az * bz) / (la * lb) > 0.985f;
            bool flat = fabsf(cur.y - prev.y) < 8.0f && fabsf(nxt.y - cur.y) < 8.0f;
            if (straight && flat && avp2bots::Dist(prev, cur) < 256.0f)
                continue;
        }
        simple.push_back(steps[i]);
        prev = steps[i].pos;
    }

    // A path is recomputed often while chasing (about every 0.6 s): its first steering points may already be
    // behind the bot, which then turned back and forth around the same waypoint. Skip a leading plain step when
    // the bot is no farther from the step after it than the step itself is (special steps are kept).
    {
        const Vec3 me(p.x, p.y - dims.y, p.z);
        size_t skip = 0;
        while (skip + 1 < simple.size() && !simple[skip].fixed &&
               simple[skip].instruction == CAIPathWaypoint::eInstructionMoveTo &&
               fabsf(simple[skip].pos.y - me.y) < 32.0f &&
               avp2bots::DistXZ(me, simple[skip + 1].pos) <= avp2bots::DistXZ(simple[skip].pos, simple[skip + 1].pos) + 8.0f)
            ++skip;
        if (skip)
            simple.erase(simple.begin(), simple.begin() + skip);
    }

    pPath->ClearWaypoints();
    // AddWaypoint pushes to the front: add the destination first, then the steps backwards
    if (ambush)
    {
        // the ambush ends on the wall face: a WallWalkTo with the wall's normal (on the surface, no lift)
        CAIPathWaypoint last(CAINode::kInvalidID, CAIPathWaypoint::eInstructionWallWalkTo);
        last.SetArgumentNormal(LTVector(ambush->normal.x, ambush->normal.y, ambush->normal.z));
        last.SetArgumentPosition(LTVector(ambush->wall.x, ambush->wall.y, ambush->wall.z));
        pPath->AddWaypoint(last);
    }
    else
    {
        CAIPathWaypoint last(CAINode::kInvalidID, CAIPathWaypoint::eInstructionMoveTo);
        last.SetArgumentPosition(vDest);
        pPath->AddWaypoint(last);
    }
    for (int i = (int)simple.size() - 1; i >= 0; --i)
    {
        CAIPathWaypoint w(CAINode::kInvalidID, simple[i].instruction);
        w.SetArgumentNormal(LTVector(simple[i].normal.x, simple[i].normal.y, simple[i].normal.z));
        // JumpTo positions are floor positions (CAIStrategyFollowPath::StartJumpTo adds the AI's height), and so
        // is the step after a LadderTo; MoveTo ignores the height (it projects onto the AI's plane)
        const float lift = (simple[i].instruction == CAIPathWaypoint::eInstructionJumpTo || simple[i].floor) ? 0.0f : dims.y;
        w.SetArgumentPosition(LTVector(simple[i].pos.x, simple[i].pos.y + lift, simple[i].pos.z));
        pPath->AddWaypoint(w);
    }
    if (g_cfg.debug || ladders || climbs || ambush)
        Log("PATH ai=%s from=%d to=%d steps=%u ladders=%d climbs=%d%s%s expanded=%d", ObjectLabel(pAI->m_hObject).c_str(),
            a, b, (unsigned)simple.size(), ladders, climbs, pBias ? " wallapproach" : "", ambush ? " ambush" : "",
            g_nav.LastExpanded());
    g_pathFails.erase(pAI->m_hObject);  // a path found: not isolated
    return LTTRUE;
}

// ----------------------------------------------------------------------------------------------
// Admin commands

namespace {

void ApplyCounts(const int want[BOT_SPECIES_COUNT])
{
    const LTFLOAT now = g_pLTServer->GetTime();
    if (SurvivorMode() || HuntMode())
    {
        // Survivor and Hunt: the races come from the game, only the total counts; new bots join as survivors
        // (Survivor) or by the players' join rule (Hunt)
        int total = 0;
        for (int s = 0; s < BOT_SPECIES_COUNT; ++s)
            total += want[s];
        const int sp = RaceSpecies(SurvivorRace(false));
        while ((int)g_bots.size() < total && sp >= 0)
            AddBot((BotSpecies)sp, 0, now + 0.5f + 0.3f * (float)g_bots.size());
        while ((int)g_bots.size() < total && HuntMode())
            AddHuntBot(now + 0.5f + 0.3f * (float)g_bots.size());
        while ((int)g_bots.size() > total)
        {
            int pick = (int)g_bots.size() - 1;
            for (int i = (int)g_bots.size() - 1; i >= 0; --i)
                if (!g_bots[i].alive)
                {
                    pick = i;
                    break;
                }
            RemoveBotAt((size_t)pick);
        }
        return;
    }
    for (int s = 0; s < BOT_SPECIES_COUNT; ++s)
    {
        while (CountSpecies((BotSpecies)s) < want[s])
            AddBot((BotSpecies)s, 0, now + 0.5f + 0.3f * (float)CountSpecies((BotSpecies)s));
        while (CountSpecies((BotSpecies)s) > want[s])
        {
            // a dead (waiting to respawn) bot first, then the newest
            int pick = -1;
            for (int i = (int)g_bots.size() - 1; i >= 0 && pick < 0; --i)
                if (g_bots[i].species == s && !g_bots[i].alive)
                    pick = i;
            for (int i = (int)g_bots.size() - 1; i >= 0 && pick < 0; --i)
                if (g_bots[i].species == s)
                    pick = i;
            RemoveBotAt((size_t)pick);
        }
    }
}

int Reply(char* out, int size, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(out, size - 1, fmt, ap);
    va_end(ap);
    out[size - 1] = 0;
    return n;
}

} // namespace

bool BotMgr_Command(const char* text, char* reply, int replySize)
{
    if (!text || !reply || replySize < 2)
        return false;
    reply[0] = 0;
    char buf[256];
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char* argv[8];
    int argc = 0;
    for (char* tok = strtok(buf, " \t"); tok && argc < 8; tok = strtok(0, " \t"))
        argv[argc++] = tok;
    if (argc == 0)
        return false;
    const char* cmd = argv[0];
    if (*cmd == '/')
        ++cmd;

    const bool known = !_stricmp(cmd, "addbot") || !_stricmp(cmd, "removebot") || !_stricmp(cmd, "kickbot") ||
                       !_stricmp(cmd, "bots") || !_stricmp(cmd, "botlist") || !_stricmp(cmd, "botcount") ||
                       !_stricmp(cmd, "botreload") || !_stricmp(cmd, "bothelp") || !_stricmp(cmd, "botfill");
    if (!known)
        return false;

    if (!_stricmp(cmd, "bothelp"))
    {
        Reply(reply, replySize, "bots: addbot [marine|alien|predator] [class] | removebot [name|species|all] | "
                                "bots | botcount <marines> <aliens> <predators> | botfill <players> | botreload");
        return true;
    }
    if (!g_navLoaded)
    {
        Reply(reply, replySize, "bots: no waypoint file for map '%s' (botnav\\%s.nav), bots are off here",
              g_map.c_str(), g_map.c_str());
        return true;
    }

    if (!_stricmp(cmd, "addbot"))
    {
        int sp = argc > 1 ? ParseSpecies(argv[1]) : -1;
        const BotClass* cls = 0;
        if (argc > 1 && sp < 0)
        {
            cls = ParseClass(-1, argv[1]);  // "addbot sniper"
            if (!cls)
            {
                Reply(reply, replySize, "bots: unknown species or class '%s'", argv[1]);
                return true;
            }
            sp = cls->species;
        }
        else if (argc > 2)
        {
            cls = ParseClass(sp, argv[2]);
            if (!cls)
            {
                Reply(reply, replySize, "bots: unknown %s class '%s'", kSpecies[sp].tag, argv[2]);
                return true;
            }
        }
        if (sp < 0)
        {
            // the species with the fewest bots
            sp = 0;
            for (int s = 1; s < BOT_SPECIES_COUNT; ++s)
                if (CountSpecies((BotSpecies)s) < CountSpecies((BotSpecies)sp))
                    sp = s;
        }
        g_cfg.fill = 0;  // a manual change: keep the bots as they are from now on
        AddBot((BotSpecies)sp, cls, g_pLTServer->GetTime() + 0.5f);
        Reply(reply, replySize, "bots: added %s%s%s (%d bots)", kSpecies[sp].tag, cls ? " " : "",
              cls ? cls->name : "", (int)g_bots.size());
        Log("CMD addbot species=%s class=%s", kSpecies[sp].tag, cls ? cls->name : "-");
        return true;
    }

    if (!_stricmp(cmd, "removebot") || !_stricmp(cmd, "kickbot"))
    {
        if (g_bots.empty())
        {
            Reply(reply, replySize, "bots: there are no bots");
            return true;
        }
        if (argc > 1 && !_stricmp(argv[1], "all"))
        {
            int n = (int)g_bots.size();
            g_cfg.fill = 0;
            while (!g_bots.empty())
                RemoveBotAt(g_bots.size() - 1);
            Reply(reply, replySize, "bots: removed all %d bots", n);
            return true;
        }
        int pick = -1;
        int sp = argc > 1 ? ParseSpecies(argv[1]) : -1;
        for (int i = (int)g_bots.size() - 1; i >= 0 && pick < 0; --i)
        {
            if (argc < 2)
                pick = i;
            else if (sp >= 0 && g_bots[i].species == sp)
                pick = i;
            else if (sp < 0 && strstr(g_bots[i].name.c_str(), argv[1]))
                pick = i;
        }
        if (pick < 0)
        {
            Reply(reply, replySize, "bots: no bot matches '%s'", argv[1]);
            return true;
        }
        g_cfg.fill = 0;
        std::string name = g_bots[pick].name;
        RemoveBotAt((size_t)pick);
        Reply(reply, replySize, "bots: removed %s (%d left)", name.empty() ? "a bot" : name.c_str(), (int)g_bots.size());
        return true;
    }

    if (!_stricmp(cmd, "botfill"))
    {
        if (argc < 2)
        {
            Reply(reply, replySize, "bots: fill is %d (0 = off); usage botfill <players>", g_cfg.fill);
            return true;
        }
        int n = atoi(argv[1]);
        g_cfg.fill = n < 0 ? 0 : (n > 16 ? 16 : n);
        g_lastHumans = -1;
        if (g_cfg.fill > 0)
            ApplyFill();
        Reply(reply, replySize, g_cfg.fill > 0 ? "bots: filling up to %d players (now %d bots)"
                                               : "bots: fill off (%d bots stay)", g_cfg.fill, (int)g_bots.size());
        return true;
    }

    if (!_stricmp(cmd, "botcount"))
    {
        if (argc < 4)
        {
            Reply(reply, replySize, "bots: usage botcount <marines> <aliens> <predators>");
            return true;
        }
        int want[BOT_SPECIES_COUNT];
        want[BOT_MARINE] = atoi(argv[1]);
        want[BOT_ALIEN] = atoi(argv[2]);
        want[BOT_PREDATOR] = atoi(argv[3]);
        for (int s = 0; s < BOT_SPECIES_COUNT; ++s)
            want[s] = want[s] < 0 ? 0 : (want[s] > 16 ? 16 : want[s]);
        const bool wasFilling = g_cfg.fill > 0;
        g_cfg.fill = 0;  // fixed counts from now on
        ApplyCounts(want);
        Reply(reply, replySize, "bots: now %d marines, %d aliens, %d predators%s", CountSpecies(BOT_MARINE),
              CountSpecies(BOT_ALIEN), CountSpecies(BOT_PREDATOR), wasFilling ? " (fill turned off)" : "");
        return true;
    }

    if (!_stricmp(cmd, "botreload"))
    {
        LoadConfig();
        g_nav.UseLadders(g_cfg.ladders);
        g_lastHumans = -1;
        if (g_cfg.fill > 0)
        {
            ApplyFill();
            Reply(reply, replySize, "bots: botnav\\bots.cfg reloaded: filling up to %d players (%d bots)", g_cfg.fill,
                  (int)g_bots.size());
            return true;
        }
        ApplyCounts(g_cfg.count);
        Reply(reply, replySize, "bots: botnav\\bots.cfg reloaded: %d marines, %d aliens, %d predators",
              g_cfg.count[BOT_MARINE], g_cfg.count[BOT_ALIEN], g_cfg.count[BOT_PREDATOR]);
        return true;
    }

    // bots / botlist
    int len = Reply(reply, replySize, "bots on %s: %d marines, %d aliens, %d predators%s:", g_map.c_str(),
                    CountSpecies(BOT_MARINE), CountSpecies(BOT_ALIEN), CountSpecies(BOT_PREDATOR),
                    g_cfg.fill > 0 ? " (fill)" : "");
    for (size_t i = 0; i < g_bots.size() && len > 0 && len < replySize - 40; ++i)
    {
        const BotClass* c = g_bots[i].forcedClass ? g_bots[i].forcedClass : ClassFor(g_bots[i].species, g_bots[i].slot);
        len += Reply(reply + len, replySize - len, " %s%d(%s%s)", kSpecies[g_bots[i].species].tag, g_bots[i].slot,
                     c ? c->name : "-", g_bots[i].alive ? "" : ",dead");
    }
    return true;
}
