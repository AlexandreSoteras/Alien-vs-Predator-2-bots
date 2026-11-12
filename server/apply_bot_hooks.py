"""Insert the avp2bots hooks into Monolith's ObjectDLL sources (server/game/AVP2).

The hooks are small and marked "avp2bots". The script is idempotent and keeps each file's encoding
(Windows-1252) and line endings. Run: py -I apply_bot_hooks.py
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "game", "AVP2")
MARK = "avp2bots"


def patch(rel, edits):
    path = os.path.join(ROOT, rel)
    raw = open(path, "rb").read()
    crlf = b"\r\n" in raw
    text = raw.decode("cp1252").replace("\r\n", "\n")
    changed = False
    for name, pattern, replacement in edits:
        tag = f"{MARK}:{name}"
        # whole tag only: "avp2bots:botcburst" must not match "avp2bots:botcburstanim"
        if re.search(re.escape(tag) + r"(?![A-Za-z0-9_])", text):
            print(f"  {rel}: {name} already applied")
            continue
        new, n = re.subn(pattern, lambda m: replacement(m).replace("@TAG", tag), text, count=1, flags=re.M)
        if n != 1:
            print(f"  {rel}: {name} anchor NOT FOUND")
            sys.exit(1)
        text = new
        changed = True
        print(f"  {rel}: {name} applied")
    if changed:
        out = text.replace("\n", "\r\n") if crlf else text
        open(path, "wb").write(out.encode("cp1252"))


def main():
    print("GameServerShell.cpp")
    patch("ObjectDLL/GameServerShell.cpp", [
        ("include", r'^(#include "GameServerShell.h"[^\n]*\n)',
         lambda m: m.group(1) + '#include "BotMgr.h" // @TAG\n'),
        # FindStartPoint (MP): a player spawns at the start point whose nearest other player is farthest; bots
        # are not players, so they were ignored both ways (and a bot spawning picked a random point)
        ("spawndist",
         r"^(\t\t\t\t\t\t\tif\(fTemp < fNearDist\[i\]\)\n\t\t\t\t\t\t\t\tfNearDist\[i\] = fTemp;\n\t\t\t\t\t\t\}\n\t\t\t\t\t\}\n)",
         lambda m: m.group(1) + "\t\t\t\t\t// @TAG: bots count as players (both for a player and for a bot spawning)\n"
                   "\t\t\t\t\tnPlayers += BotMgr_StartPointDist(vPos, fFarDist[i], fNearDist[i]);\n"),
        ("prestart", r"^(\tm_charMgr\.PreStartWorld\(\);\n)",
         lambda m: m.group(1) + "\tBotMgr_PreStartWorld(); // @TAG\n"),
        ("poststart", r"^(\t\tm_bHasQueenStartPoint = ValidateQueenStartPoints\(\);\n)",
         lambda m: m.group(1) + "\n\t\t// spawn bots on maps that have a waypoint graph\n\t\tBotMgr_PostStartWorld(); // @TAG\n"),
        ("update", r"^(\t// Update the music\.\n\tg_pMusicMgr->Update\(\);\n)",
         lambda m: m.group(1) + "\n\tBotMgr_Update(); // @TAG\n"),
    ])

    print("AIPathMgr.cpp")
    fallback_pos = ("\t// @TAG: stock multiplayer maps have no AIVolumes; path through the bot waypoint graph instead\n"
                    "\tif( g_pAIVolumeMgr->GetNumVolumes() == 0 && BotMgr_GetNav() )\n"
                    "\t\treturn BotMgr_FindPath(pAI, vPosDest, pPath);\n\n")
    fallback_char = ("\t// @TAG: stock multiplayer maps have no AIVolumes; path through the bot waypoint graph instead\n"
                     "\tif( g_pAIVolumeMgr->GetNumVolumes() == 0 && BotMgr_GetNav() && pChar )\n"
                     "\t{\n\t\tLTVector vCharPos;\n\t\tg_pLTServer->GetObjectPos(pChar->m_hObject, &vCharPos);\n"
                     "\t\treturn BotMgr_FindPath(pAI, vCharPos, pPath);\n\t}\n\n")
    patch("ObjectDLL/AIPathMgr.cpp", [
        ("include", r'^(#include "AIPathMgr.h"[^\n]*\n)',
         lambda m: m.group(1) + '#include "BotMgr.h" // @TAG\n'),
        ("findpath_pos",
         r"^(LTBOOL CAIPathMgr::FindPath\(CAI\* pAI, const LTVector& vPosDest, CAIPath\* pPath, [^\n]*\n\{\n)",
         lambda m: m.group(1) + fallback_pos),
        ("findpath_char",
         r"^(LTBOOL CAIPathMgr::FindPath\(CAI\* pAI, const CCharacter \* pChar, CAIPath\* pPath, [^\n]*\n\{\n)",
         lambda m: m.group(1) + fallback_char),
    ])

    print("AI.cpp")
    patch("ObjectDLL/AI.cpp", [
        ("include", r'^(#include "AI.h"[^\n]*\n)',
         lambda m: m.group(1) + '#include "BotRoamGoal.h" // @TAG\n'),
        ("creategoal",
         r'^(\telse if\( stricmp\(szGoalName, "NodeCheck"\) == 0 \)\n\t\{\n\t\tpGoal = new NodeCheckGoal\(this, szGoalName\);\n\t\}\n)',
         lambda m: m.group(1) + '\telse if( stricmp(szGoalName, "BotRoam") == 0 ) // @TAG\n\t\tpGoal = new BotRoamGoal(this, szGoalName);\n'),
    ])

    print("AI.cpp (fire)")
    patch("ObjectDLL/AI.cpp", [
        ("includemgr", r'^(#include "AI.h"[^\n]*\n)',
         lambda m: m.group(1) + '#include "BotMgr.h" // @TAG\n'),
        ("botfire",
         r"^\tfireInfo\.fPerturbR\t= \(1\.0f - GetAccuracy\(\)\);\n\tfireInfo\.fPerturbU\t= \(1\.0f - GetAccuracy\(\)\);\n"
         r"\tfireInfo\.fDamageMultiplier = m_pCurrentWeaponButes->fDamageMultiplier;\n"
         r"\tfireInfo\.fDamageMultiplier \*= GetDifficultyFactor\(\);\n",
         lambda m: "\t// @TAG: bots use the bots.cfg 'accuracy' (spread: 0 = widest, 1 = narrowest, as a player standing still)\n"
                   "\t// and 'damage' (1 = the players' damage) instead of the single-player accuracy and difficulty factor\n"
                   "\tconst bool bBot = BotMgr_IsBot(m_hObject);\n"
                   "\tfireInfo.fPerturbR\t= (1.0f - (bBot ? BotMgr_Accuracy() : GetAccuracy()));\n"
                   "\tfireInfo.fPerturbU\t= (1.0f - (bBot ? BotMgr_Accuracy() : GetAccuracy()));\n"
                   "\tfireInfo.fDamageMultiplier = m_pCurrentWeaponButes->fDamageMultiplier;\n"
                   "\tfireInfo.fDamageMultiplier *= bBot ? BotMgr_DamageScale() : GetDifficultyFactor();\n"),
        # a bot given its own skill by a server extension (ServerExt_BotSkill) uses it as its accuracy (spread)
        ("botskillfire",
         r"^(\tfireInfo\.fDamageMultiplier \*= bBot \? BotMgr_DamageScale\(\) : GetDifficultyFactor\(\);\n)",
         lambda m: m.group(1) + "\tif(bBot) // @TAG: per-bot skill (BotMgr_BotAccuracy: bots.cfg 'accuracy' without one)\n"
                   "\t\tfireInfo.fPerturbR = fireInfo.fPerturbU = 1.0f - BotMgr_BotAccuracy(m_hObject);\n"),
    ])

    patch("ObjectDLL/AIActionMisc.cpp", [
        ("include", r'^(#include "AIActionMisc.h"[^\n]*\n)',
         lambda m: m.group(1) + '#include "BotMgr.h" // @TAG\n'),
        # one shot per server frame caps a 15 shots/s Smart Gun at the frame rate (about 10/s)
        ("botrate",
         r"^(\t\tif\( g_pLTServer->GetTime\(\) - m_fLastShotTime  > fShotInterval \)\n\t\t\{\n\t\t\t// Fire our weapon\.\n"
         r"\t\t\tGetAIPtr\(\)->FireWeapon\(\);\n\n\t\t\tm_fLastShotTime = g_pLTServer->GetTime\(\);\n\n"
         r"\t\t\t--m_nBurstShotsLeft;\n\t\t\}\n)(\n\t\tif \(m_nBurstShotsLeft <= 0 \|\| GetAIPtr\(\)->OutOfAmmo\(\))",
         lambda m: "\t\t// @TAG: bots keep the weapon's real rate when it is faster than the server frames\n"
                   "\t\tif( BotMgr_IsBot(GetAIPtr()->m_hObject) )\n\t\t{\n"
                   "\t\t\tconst LTFLOAT fNow = g_pLTServer->GetTime();\n"
                   "\t\t\tif( !GetAIPtr()->GetCurrentWeaponPtr() || !GetAIPtr()->GetTarget().IsValid() )\n"
                   "\t\t\t\tm_nBurstShotsLeft = 0;  // no weapon or no target any more: end the action\n"
                   "\t\t\tif( fNow - m_fLastShotTime > 3.0f*fShotInterval )\n"
                   "\t\t\t\tm_fLastShotTime = fNow - fShotInterval;  // a new burst: first shot now\n"
                   "\t\t\tfor( int nShots = 0; nShots < 4 && m_nBurstShotsLeft > 0 && fNow - m_fLastShotTime >= fShotInterval\n"
                   "\t\t\t\t&& GetAIPtr()->GetCurrentWeaponPtr() && GetAIPtr()->GetTarget().IsValid(); ++nShots )\n"
                   "\t\t\t{\n"
                   "\t\t\t\t// a refused shot (gun not on target yet, Predator sifting) is tried again next frame;\n"
                   "\t\t\t\t// it still counts against the burst, so the action ends if the shot never comes\n"
                   "\t\t\t\t--m_nBurstShotsLeft;\n"
                   "\t\t\t\tif( !GetAIPtr()->FireWeapon() )\n\t\t\t\t\tbreak;\n"
                   "\t\t\t\tm_fLastShotTime += fShotInterval;\n"
                   "\t\t\t}\n\t\t}\n\t\telse\n"
                   + m.group(1) + m.group(2)),
    ])

    patch("ObjectDLL/AI.cpp", [
        ("botfirekey",
         r"^(\telse if\( stricmp\(pKey, WEAPON_KEY_FIRE\) == 0 \|\| stricmp\(pKey, \"Fire\"\) == 0\)\n\t\{\n)\t\tFireWeapon\(\);\n",
         lambda m: m.group(1) + "\t\t// @TAG: bots fire ranged weapons on the fire action's timer only (the MP models' animations\n"
                   "\t\t// either never send the fire key or send it every frame); melee attacks still strike on the key\n"
                   "\t\tif( !BotMgr_IsBot(m_hObject) || (m_pCurrentWeaponButes && m_pCurrentWeaponButes->bMeleeWeapon) )\n"
                   "\t\t\tFireWeapon();\n"),
        ("botfirelog", r"^(\tconst bool bBot = BotMgr_IsBot\(m_hObject\);\n)",
         lambda m: m.group(1) + "\tif(bBot && !BotMgr_OnFire(m_hObject)) return false; // @TAG: Predator energy, cannon charge, debug log\n"),
        ("botaim", r"^(\tif\(bBot && !BotMgr_OnFire\(m_hObject\)\) return false; // avp2bots:botfirelog: [^\n]*\n)",
         lambda m: m.group(1) + "\t// @TAG: a human's aim error (distance, target speed, bots.cfg accuracy); none for melee\n"
                   "\tif(bBot && m_pTarget && !m_pCurrentWeaponButes->bMeleeWeapon\n"
                   "\t\t&& !BotMgr_AimError(m_hObject, m_pTarget->GetObject(), fireInfo.vFirePos, fireInfo.vPath, m_bFullyAimed ? true : false))\n"
                   "\t\treturn false;  // the gun is still turning toward the target\n"),
        # the AI's melee path is "target - centre" (its length is the distance) and CWeaponMgr::CalculateWeaponPath adds
        # the barrel's spread (thousandths) before normalizing, so the spread vanished: all 6 claw vectors hit one point
        ("botmeleepath", r"^(\t\treturn false;  // the gun is still turning toward the target\n)",
         lambda m: m.group(1) + "\t// @TAG: a bot's melee path is a unit vector like a player's, so the barrel spread applies\n"
                   "\tif(bBot && m_pCurrentWeaponButes->bMeleeWeapon)\n"
                   "\t\tBotMgr_MeleePath(m_hObject, fireInfo.vPath);\n"),
    ])

    print("AI.cpp (combat movement)")
    patch("ObjectDLL/AI.cpp", [
        # AIActionFireWeapon never moves the AI (and cannot be interrupted): a bot stood still for every burst.
        # After the action's update and before the movement update that applies this frame's changes.
        ("firemove",
         r"^(\t\t\t\tm_pCurrentAction->Update\(\);\n\t\t\t\}\n)"
         r"(\n\t\t\t// Update our movement \(this will be a second time\) so that any\n)",
         lambda m: m.group(1) + "\n\t\t\t// @TAG: Marine and Predator bots strafe while they fire, as players do\n"
                   "\t\t\tif( BotMgr_IsBot(m_hObject) )\n\t\t\t\tBotMgr_CombatMove(this);\n" + m.group(2)),
    ])

    print("AIStrategyApproachTarget.cpp")
    patch("ObjectDLL/AIStrategyApproachTarget.cpp", [
        ("include", r'^(#include "AIStrategyApproachTarget.h"[^\n]*\n)',
         lambda m: m.group(1) + '#include "BotMgr.h" // @TAG\n'),
        # the approach heads straight at the target, re-pathing every 0.1-0.3 s for aliens
        ("zigzag",
         r"^(\t\tm_vLastKnownPos = target\.GetPosition\(\);\n)\t\tif\( !m_StrategyFollowPath\.Set\( m_vLastKnownPos \) \)\n",
         lambda m: m.group(1) + "\t\t// @TAG: an alien bot whose target faces it runs ahead and to the side, alternating sides\n"
                   "\t\tLTVector vZig = m_vLastKnownPos;\n"
                   "\t\tconst bool bZig = BotMgr_ZigZag(GetAIPtr(), vZig);\n"
                   "\t\tif( !(bZig && m_StrategyFollowPath.Set( vZig )) && !m_StrategyFollowPath.Set( m_vLastKnownPos ) )\n"),
    ])

    print("Character.cpp")
    patch("ObjectDLL/Character.cpp", [
        ("ailayers",
         r"^\tif\(g_pGameServerShell->GetGameType\(\)\.IsMultiplayer\(\)\)\n"
         r"(\t\{\n\t\tfor\(int i = 0; i < MAX_ANIM_LAYERS; i\+\+\)\n\t\t\{\n"
         r"\t\t\tchar \*szLayer = m_cmMovement\.GetCharacterButes\(\)->m_szMPAnimLayers\[i\];)",
         lambda m: "\t// @TAG: only players take the multiplayer animation layers. AI characters (the bots) keep\n"
                   "\t// their AI layers: the MP Marine/Predator styles have a 4th layer the AI never sets, so no\n"
                   "\t// tracker set matched and the AI could not move.\n"
                   "\tif(g_pGameServerShell->GetGameType().IsMultiplayer() && dynamic_cast<CPlayerObj*>(this))\n"
                   + m.group(1)),
        ("include", r'^(#include "Character\.h"[^\n]*\n)',
         lambda m: m.group(1) + '#include "BotMgr.h" // @TAG\n'),
        # Touch_Notify_Pouncing: an AI's pounce does AI_Pounce_Ammo (30) x the difficulty factor, about 22; a
        # player's does its class's PounceWeaponDamage (Runner 195, Drone 150, Predalien 175)
        ("botpounce", r"^(\t\t\tdamage\.fDamage \*= GetDifficultyFactor\(\);\n)",
         lambda m: m.group(1) + "\t\t\t// @TAG: a bot pounces with its multiplayer class's damage, as a player\n"
                   "\t\t\tif( BotMgr_IsBot(pMovement->GetObject()) )\n"
                   "\t\t\t\tdamage.fDamage = BotMgr_PounceDamage(pMovement->GetObject(), damage.fDamage);\n"),
        # the rest of a player's pounce (CPlayerObj CP_POUNCE_DAMAGE): the hit box resolved to its character, an
        # upper-torso hit, then a stun of damage / PounceStunMod seconds (4.5 s for a Drone) that the AI's lacked
        ("botpouncehit",
         r"^(\t\t\t\tdamage\.fDamage = BotMgr_PounceDamage\(pMovement->GetObject\(\), damage\.fDamage\);\n\n"
         r"\t\t\tCCharacter \*pCharacter = [^\n]*\n\n)\t\t\tif\(pCharacter\)\n\t\t\t\tdamage\.DoDamage\(pCharacter, pInfo->m_hObject\);\n",
         lambda m: m.group(1) + "\t\t\t// @TAG: a bot's pounce lands as a player's: damage, then the stun\n"
                   "\t\t\tif( pCharacter && BotMgr_IsBot(pMovement->GetObject()) )\n"
                   "\t\t\t\tBotMgr_PounceHit(pCharacter, pInfo->m_hObject, damage);\n"
                   "\t\t\telse if(pCharacter)\n\t\t\t\tdamage.DoDamage(pCharacter, pInfo->m_hObject);\n"),
    ])

    print("Character.h")
    patch("ObjectDLL/Character.h", [
        # the medicomp and the energy sift are protected and run when a player's view-model animation sends its
        # key (CP_MEDICOMP, CP_ENERGY_SIFT); the cloak delay a forced decloak starts is private
        ("botpredheal", r"^(\t\tvirtual void\tForceCloakOff\(LTBOOL bDelay=LTFALSE\);\n)",
         lambda m: m.group(1) + "\t\t// @TAG: BotMgr runs the players' Predator code for bots at the players' animation keys\n"
                   "\t\tvoid\t\t\tBotMedicomp()\t\t\t\t{ HandleMedicomp(); }\n"
                   "\t\tvoid\t\t\tBotEnergySiftKey()\t\t\t{ HandleEnergySift(); }\n"
                   "\t\tLTFLOAT\t\t\tBotCloakDelayStart() const\t{ return m_fCloakDelayTime; }\n"),
    ])

    print("CharacterAnimation.h")
    patch("Shared/CharacterAnimation.h", [
        # a new tracker set starts only when the old one's animation sends a break or ends; some never do, and the
        # bot stood in place (an Alien in its idle set, a Predator in its cannon run set) with a path to follow
        ("animwait", r"^(\t\tLTBOOL\tMovementAllowed\(LTBOOL bLastTracker = LTTRUE\);[^\n]*\n)",
         lambda m: m.group(1) + "\t\t// @TAG: BotMgr's watchdog: a tracker set switch waiting too long is forced\n"
                   "\t\tLTBOOL\tIsWaitingToSwitch() const\t{ return m_bWaiting && (m_nLastTrackerSet != m_nTrackerSet); }\n"
                   "\t\tvoid\tForceTrackerSwitch()\t\t{ m_bWaiting = LTFALSE; }\n"),
    ])

    print("AI.h")
    patch("ObjectDLL/AI.h", [
        ("mpsets",
         r'^(\t\tvirtual std::string GetCharacterSetName\(const std::string & root_name\) const )'
         r'\{ return root_name \+ std::string\("_AI"\); \}\n',
         lambda m: "\t\t// @TAG: a multiplayer character set (\"*_MP\") is used as it is, so bots get the\n"
                   "\t\t// players' classes, models, skins and values; every other name maps to its \"_AI\" variant.\n"
                   + m.group(1) + "\n\t\t{\n"
                   "\t\t\tif( root_name.size() > 3 && 0 == _stricmp(root_name.c_str() + root_name.size() - 3, \"_MP\") )\n"
                   "\t\t\t\treturn root_name;\n"
                   "\t\t\treturn root_name + std::string(\"_AI\");\n\t\t}\n"),
    ])

    print("AIActionMoveTo.cpp")
    patch("ObjectDLL/AIActionMoveTo.cpp", [
        ("include", r'^(#include "AIActionMoveTo.h"[^\n]*\n)',
         lambda m: m.group(1) + '#include "GameServerShell.h" // @TAG\n'),
        ("ladderepsilon",
         r"^(void AIActionLadderTo::Update\(\)\n(?:.*\n)*?)\tconst LTFLOAT vert_epsilon = 0\.5f;\n",
         lambda m: m.group(1) + "\t// @TAG: generated ladder targets are not hand-placed nodes; in multiplayer (only bots are AI)\n"
                   "\t// accept being close in height, or a bot that overshoots hangs on the ladder for good.\n"
                   "\tconst LTFLOAT vert_epsilon = g_pGameServerShell->GetGameType().IsMultiplayer() ? 12.0f : 0.5f;\n"),
    ])

    print("AIStrategyFollowPath.cpp")
    patch("ObjectDLL/AIStrategyFollowPath.cpp", [
        ("wwnormal",
         r"^(\t\t_ASSERT\(!\"CAIStrategyFollowPath::StartWallWalkTo does not have a node!\"\);\n)"
         r"\t\tGetAIPtr\(\)->GetMovePtr\(\)->SetWallWalk\( m_Waypoint\.GetArgumentPosition\(\), GetAIPtr\(\)->GetUp\(\), pDestNode \);\n",
         lambda m: m.group(1)
         + "\t\t// @TAG: without a node (bot paths), use the surface normal the waypoint carries\n"
         + "\t\tGetAIPtr()->GetMovePtr()->SetWallWalk( m_Waypoint.GetArgumentPosition(), m_Waypoint.GetArgumentNormal(), pDestNode );\n"),
    ])

    print("AIAlienAttack.cpp")
    in_volumes = (r"^(\t+)const bool bTargetInVolumes = !target\.GetCharacterPtr\(\) \|\| "
                  r"target\.GetCharacterPtr\(\)->GetCurrentVolumePtr\(\);\n")
    in_volumes_fix = (lambda m: m.group(1) + "// @TAG: maps without AIVolumes (multiplayer bots) count the target as reachable,\n"
                      + m.group(1) + "// so pounces keep their normal delay\n"
                      + m.group(1) + "const bool bTargetInVolumes = !target.GetCharacterPtr() || "
                      "target.GetCharacterPtr()->GetCurrentVolumePtr() || g_pAIVolumeMgr->GetNumVolumes() == 0;\n")
    patch("ObjectDLL/AIAlienAttack.cpp", [
        ("include", r'^(#include "AIAlienAttack.h"[^\n]*\n)',
         lambda m: m.group(1) + '#include "AIVolumeMgr.h" // @TAG\n#include "BotMgr.h" // @TAG\n'),
        ("pouncevolumes", in_volumes, in_volumes_fix),
        ("facehugvolumes", in_volumes, in_volumes_fix),
        ("lunge",
         r"^(\t\t\t\t\t\t// Don't worry about if the target is in a volume or not\.\n\t\t\t\t\t\treturn true;\n"
         r"\t\t\t\t\t\}\n\t\t\t\t\}\n)",
         lambda m: m.group(1)
         + "\t\t\t\t// @TAG: maps without AIVolumes (multiplayer bots): lunge at a target that stands on the bot\n"
         + "\t\t\t\t// waypoint graph, so the alien lands where it can find its way again\n"
         + "\t\t\t\telse if( g_pAIVolumeMgr->GetNumVolumes() == 0 )\n"
         + "\t\t\t\t{\n\t\t\t\t\treturn BotMgr_LungeTargetOK(vTargetPos);\n\t\t\t\t}\n"),
        # the AI pounces on a die roll (AlienPounceProbability 0.4 every 1-3 s); a player pounces when he has the
        # target in reach, at most every 2 s: bots follow the players' rule (BotMgr_PounceNow, bots.cfg pounce)
        # a facehugger player leaps with either fire button (the pounce jump, 2 s cooldown); the AI facehugger leaps on
        # a 1-3 s timer within 250 units: bot facehuggers follow the players' rule (BotMgr_HugNow, bots.cfg huglike)
        ("bothugrule",
         r"^(\t\t// Check for facehugging\.\n\t\tif\( target\.GetCharacterPtr\(\) && target\.GetCharacterPtr\(\)->GetButes\(\)->m_bCanBeFacehugged \)\n"
         r"\t\t\{\n)\t\t\tif\( m_Butes\.fFacehugChance > 0\.0f \)\n",
         lambda m: m.group(1)
         + "\t\t\t// @TAG: bot facehuggers leap when a player would (reach, clear arc, reaction, 2 s cooldown)\n"
         + "\t\t\tif( BotMgr_HugRule(GetAIPtr()) )\n\t\t\t{\n"
         + "\t\t\t\tif( BotMgr_HugNow(GetAIPtr()) && GetAIPtr()->SetNextAction(CAI::ACTION_FACEHUG) )\n\t\t\t\t{\n"
         + "\t\t\t\t\tBotMgr_HugStarted(GetAIPtr());\n"
         + "\t\t\t\t\tm_RootFSM.SetNextState( Root::eStateEngageTarget );\n"
         + "\t\t\t\t\treturn;\n\t\t\t\t}\n\t\t\t}\n"
         + "\t\t\telse if( m_Butes.fFacehugChance > 0.0f )\n"),
        ("botpouncerule", r"^(\t\t)// Check for pouncing\.\n\t\tif\( m_Butes\.fPounceChance > 0\.0f \)\n",
         lambda m: m.group(1) + "// @TAG: bots pounce when a player would (reach, clear arc, reaction, 2 s cooldown)\n"
         + m.group(1) + "if( BotMgr_PounceRule(GetAIPtr()) )\n"
         + m.group(1) + "{\n"
         + m.group(1) + "\tif( BotMgr_PounceNow(GetAIPtr()) && GetAIPtr()->SetNextAction(CAI::ACTION_POUNCE) )\n"
         + m.group(1) + "\t{\n"
         + m.group(1) + "\t\tBotMgr_PounceStarted(GetAIPtr());\n"
         + m.group(1) + "\t\tm_RootFSM.SetNextState( Root::eStateEngageTarget );\n"
         + m.group(1) + "\t\treturn;\n"
         + m.group(1) + "\t}\n"
         + m.group(1) + "}\n"
         + m.group(1) + "// Check for pouncing.\n"
         + m.group(1) + "else if( m_Butes.fPounceChance > 0.0f )\n"),
    ])

    print("CharacterMovement.cpp (pounce flight)")
    patch("Shared/CharacterMovement.cpp", [
        # the AI's pounce (CMS_POUNCING) flies a fixed arc to its predicted point and stops there; a player's pounce
        # flies on under half gravity until it touches something: a bot's flies like a player's (BotMgr_PounceFlight).
        # Server build only; the touch handling (Touch_Notify_Pouncing: damage, stun, end) is unchanged
        ("botpounceflight", r"^(void CharacterMovementState_Pouncing\(CharacterMovement \*pMovement\)\n\{\n)",
         lambda m: m.group(1) + "#ifndef _CLIENTBUILD\n"
                   "\t// @TAG: a bot's pounce flies like a player's pounce jump\n"
                   "\t{\n\t\textern bool BotMgr_PounceFlight(CharacterMovement* pMovement);\n"
                   "\t\tif( pMovement->GetMovementState() == CMS_POUNCING && BotMgr_PounceFlight(pMovement) )\n"
                   "\t\t\treturn;\n\t}\n#endif\n"),
        # a bot facehugger's leap (CMS_FACEHUG runs this state's code too) flies the same way
        ("bothugflight",
         r"^\t\tif\( pMovement->GetMovementState\(\) == CMS_POUNCING && BotMgr_PounceFlight\(pMovement\) \)\n",
         lambda m: "\t\t// @TAG: also a bot facehugger's leap\n"
                   "\t\tif( (pMovement->GetMovementState() == CMS_POUNCING || pMovement->GetMovementState() == CMS_FACEHUG)\n"
                   "\t\t\t&& BotMgr_PounceFlight(pMovement) )\n"),
    ])

    print("AISenseSee.cpp")
    patch("ObjectDLL/AISenseSee.cpp", [
        ("include", r'^(#include "AISenseSee.h"[^\n]*\n)',
         lambda m: m.group(1) + '#include "BotMgr.h" // @TAG\n'),
        # a bot sees a cloaked Predator as a player would, instead of the single-player AI's falloff (blind past
        # 250 units); BotMgr_CloakSeeFactor returns < 0 to keep the stock code (not a bot, alien or predator eyes)
        ("botseecloak", r"^(\t// Handle cloaked targets\.\n)",
         lambda m: "\t// @TAG: a bot sees a cloaked Predator as a player would\n"
                   "\t{\n"
                   "\t\tconst LTFLOAT fBotCloak = BotMgr_CloakSeeFactor(GetAIPtr()->m_hObject, hStimulus, fDistanceSqr);\n"
                   "\t\tif( fBotCloak >= 0.0f )\n"
                   "\t\t\treturn fRateMultiplier*fBotCloak;\n"
                   "\t}\n\n" + m.group(1)),
        # a bot notices a small target more slowly, as a person does (BotMgr_SizeSeeFactor, 1 = no change)
        ("botseesize", r"^(\t// avp2bots:botseecloak: a bot sees a cloaked Predator as a player would\n)",
         lambda m: "\t// @TAG: a bot notices a small target (chestburster, facehugger, a crouching human) more slowly\n"
                   "\tfRateMultiplier *= BotMgr_SizeSeeFactor(GetAIPtr()->m_hObject, hStimulus);\n\n" + m.group(1)),
    ])

    print("Weapon.cpp")
    patch("ObjectDLL/Weapon.cpp", [
        ("include", r'^(#include "Weapon.h"[^\n]*\n)',
         lambda m: m.group(1) + '#include "BotMgr.h" // @TAG\n'),
        # every shot of every character (players too): a cloaked Predator that has just fired is seen by bots at
        # mid range, as its spear, disc or bolt shows a player where it is (BotMgr_CloakSeeFactor)
        ("botshotnote", r"^(\t// See if we need to turn off cloaking\n\tif\(m_pBarrelData->bDecloak && IsCharacter\(m_hOwner\)\)\n)",
         lambda m: "\tBotMgr_NoteShot(m_hOwner); // @TAG\n\n" + m.group(1)),
    ])

    print("MSP 2.4 client data")
    patch("ObjectDLL/GameServerShell.cpp", [
        ("mspinclude", r'^(#include "GameServerShell.h"[^\n]*\n)',
         lambda m: m.group(1) + '#include "MspClient.h" // @TAG\n'),
        # MSP 2.4 clients scramble their client data (0x88 bytes); unscramble it before anything reads it
        ("mspclient", r"^(LPBASECLASS CGameServerShell::OnClientEnterWorld\(HCLIENT hClient, void \*pClientData, uint32 clientDataLen\)\n\{\n\tif \(!hClient\) return LTNULL;\n)",
         lambda m: m.group(1) + "\tMsp_DecodeClientData(hClient, pClientData, clientDataLen); // @TAG\n"),
        ("msplen", r"^(\t\tif\(pClientData && \(clientDataLen) == (sizeof\(MPClientData\)\)\)\n)",
         lambda m: m.group(1) + " >= " + m.group(2)[:-1] + " // @TAG: MSP 2.4 clients send 0x88 bytes\n"),
    ])
    patch("ObjectDLL/MultiplayerMgr.cpp", [
        ("mspinclude", r'^(#include "MultiplayerMgr.h"[^\n]*\n)',
         lambda m: m.group(1) + '#include "MspClient.h" // @TAG\n'),
        ("msplen1", r"^(\s*if\()(nLength|nDataLength) == sizeof\(MPClientData\)\)\n",
         lambda m: m.group(1) + m.group(2) + " >= sizeof(MPClientData)) // @TAG: MSP 2.4 clients send 0x88 bytes\n"),
        ("msplen2", r"^(\s*if\()(nLength|nDataLength) == sizeof\(MPClientData\)\)\n",
         lambda m: m.group(1) + m.group(2) + " >= sizeof(MPClientData)) // @TAG: MSP 2.4 clients send 0x88 bytes\n"),
        ("msplen3", r"^(\s*if\()(nLength|nDataLength) == sizeof\(MPClientData\)\)\n",
         lambda m: m.group(1) + m.group(2) + " >= sizeof(MPClientData)) // @TAG: MSP 2.4 clients send 0x88 bytes\n"),
        ("msplen4", r"^(\s*if\()(nLength|nDataLength) == sizeof\(MPClientData\)\)\n",
         lambda m: m.group(1) + m.group(2) + " >= sizeof(MPClientData)) // @TAG: MSP 2.4 clients send 0x88 bytes\n"),
        ("msplen5", r"^(\s*if\()(nLength|nDataLength) == sizeof\(MPClientData\)\)\n",
         lambda m: m.group(1) + m.group(2) + " >= sizeof(MPClientData)) // @TAG: MSP 2.4 clients send 0x88 bytes\n"),
        ("msplen6", r"^(\s*if\()(nLength|nDataLength) == sizeof\(MPClientData\)\)\n",
         lambda m: m.group(1) + m.group(2) + " >= sizeof(MPClientData)) // @TAG: MSP 2.4 clients send 0x88 bytes\n"),
        # a name the server app can read (a line end in a name froze AvP2Serv.exe), whatever the client sent
        ("mspname", r"^(\s*)(memcpy\(&m_ClientData\[i\]\.m_Data, pData, sizeof\(MPClientData\)\);\n)",
         lambda m: m.group(1) + m.group(2) + m.group(1) + "Msp_SanitizeName(m_ClientData[i].m_Data.m_szName); // @TAG\n"),
    ])

    print("AIAttack.cpp")

    def side_step(var):
        return (r"^(\t\t\t\t)if\( pCurrentVolume && pCurrentVolume->ContainsWithNeighbor\(" + var
                + r", LTVector\(vMyDims\.x, 0, vMyDims\.z\) \) \)\n",
                lambda m: m.group(1) + "// @TAG: maps without AIVolumes (multiplayer bots): check the ground with the bot code\n"
                + m.group(1) + "if( (pCurrentVolume && pCurrentVolume->ContainsWithNeighbor(" + var
                + ", LTVector(vMyDims.x, 0, vMyDims.z) ))\n"
                + m.group(1) + "\t|| (!pCurrentVolume && g_pAIVolumeMgr->GetNumVolumes() == 0 && BotMgr_MoveDestOK(GetAIPtr(), "
                + var + ")) )\n")
    patch("ObjectDLL/AIAttack.cpp", [
        ("include", r'^(#include "AIAttack.h"[^\n]*\n)',
         lambda m: m.group(1) + '#include "AIVolumeMgr.h" // @TAG\n#include "BotMgr.h" // @TAG\n'),
        ("dodge",) + side_step("vDodgeDest"),
        ("backoff",) + side_step("vBackoffDest"),
        ("botcloak", r"^(\t\t\t\tif \(!GetAIPtr\(\)->GetTarget\(\)\.IsFullyVisible\(\))\)\n",
         lambda m: "\t\t\t\t// @TAG: a player sees a cloaked Predator's shimmer at close and mid range\n"
                   + m.group(1) + "\n\t\t\t\t\t&& !BotMgr_CanShootCloaked(GetAIPtr()->m_hObject, GetAIPtr()->GetTarget().GetObject()))\n"),
        ("botmelee", r"^(\t\tGetAIPtr\(\)->SetWeapon\( )m_AttackButes\.strMeleeWeapon\.c_str\(\)(, true\);\n)",
         lambda m: m.group(1) + "BotMgr_MeleeWeapon(GetAIPtr()->m_hObject, m_AttackButes.strMeleeWeapon.c_str())"
                   + m.group(2).rstrip("\n") + " // @TAG: the players' melee weapon for bots\n"),
    ])

    print("Door.cpp")
    patch("ObjectDLL/Door.cpp", [
        ("include", r'^(#include "Door.h"[^\n]*\n)',
         lambda m: m.group(1) + '#include "BotMgr.h" // @TAG\n'),
        ("botdoor", r"^(void Door::Activate\(HOBJECT hObj\)\n\{\n\tif \(!hObj\) return;\n\n)\tLTBOOL bAIActivated = IsAI\(hObj\);\n",
         lambda m: m.group(1) + "\t// @TAG: bots open doors like players (only doors with ActivateTrigger set)\n"
                   "\tLTBOOL bAIActivated = IsAI(hObj) && !BotMgr_IsBot(hObj);\n"),
    ])

    print("AIActionMoveTo.cpp (ladder alignment)")
    patch("ObjectDLL/AIActionMoveTo.cpp", [
        ("ladderalign",
         r"^\t\tif\( GetAIPtr\(\)->GetMovement\(\)->GetMovementState\(\) == CMS_CLIMBING \)\n",
         lambda m: "\t\t// @TAG: in multiplayer (bots), get over the ladder's spot before climbing: generated stand points can\n"
                   "\t\t// be a body length from a shaft, under the floor above, and climbing on the way bumps the head there\n"
                   "\t\tif( GetAIPtr()->GetMovement()->GetMovementState() == CMS_CLIMBING\n"
                   "\t\t\t&& ( !g_pGameServerShell->GetGameType().IsMultiplayer() || m_vDest.Equals(m_vExitDest,1.0f)\n"
                   "\t\t\t\t|| rel_proj_dest.MagSqr() < 24.0f*24.0f ) )\n"),
    ])

    print("PickupObject.h / .cpp")
    patch("ObjectDLL/PickupObject.h", [
        ("friend", r"^(class PickupObject : public GameBase\n\{\n\tpublic:\n)",
         lambda m: m.group(1) + "\t\tfriend struct BotPickupAccess; // @TAG: bots look up health and armor pickups\n"),
    ])
    patch("ObjectDLL/PickupObject.cpp", [
        ("include", r'^(#include "PickupObject.h"[^\n]*\n)',
         lambda m: m.group(1) + '#include "BotMgr.h" // @TAG\n'),
        ("botpickup", r"^\tif\(m_puButes\.m_bPlayerOnly\)\n\t\{\n\t\tif\(!IsPlayer\(hSender\)\)\n",
         lambda m: "\t// @TAG: Marine bots take health and armor like players do (other pickups stay for players)\n"
                   "\tif(m_puButes.m_bPlayerOnly)\n\t{\n"
                   "\t\tif(!IsPlayer(hSender) && !BotMgr_CanTakePickup(hSender, m_puButes.m_szMessage))\n"),
    ])

    print("BodyProp.cpp (lifecycle)")
    patch("ObjectDLL/BodyProp.cpp", [
        ("include", r'^(#include "BodyProp.h"[^\n]*\n)',
         lambda m: m.group(1) + '#include "BotMgr.h" // @TAG\n'),
        ("botcburstanim",
         r"^\t\t\t\tif\(m_hFaceHugger && !IsAI\(m_hFaceHugger\)\)\n",
         lambda m: "\t\t\t\t// @TAG: a bot facehugger goes on to the chestburst like a player's (other AI are removed)\n"
                   "\t\t\t\tif(m_hFaceHugger && (!IsAI(m_hFaceHugger) || BotMgr_IsBot(m_hFaceHugger)))\n"),
        ("botcburst",
         r"^\t\tif\(m_hFaceHugger && !IsAI\(m_hFaceHugger\)\)\n",
         lambda m: "\t\t// @TAG: a bot's chestburst: BotMgr replaces the hugger with a chestburster bot on this body\n"
                   "\t\tif(m_hFaceHugger && BotMgr_OnChestburst(m_hFaceHugger, m_hObject, m_nCharacterSet))\n"
                   "\t\t{\n\t\t}\n\t\telse if(m_hFaceHugger && !IsAI(m_hFaceHugger))\n"),
    ])

    print("BodyProp.h (feeding)")
    patch("ObjectDLL/BodyProp.h", [
        # alien bots feed on bodies like alien players: BotMgr reads when a body started dying (it stops healing at
        # its fade-out) and whether it is a severed limb
        ("friend", r"^(class BodyProp : public Prop\n\{\n\tpublic:\n)",
         lambda m: m.group(1) + "\t\tfriend struct BotBodyAccess; // @TAG: alien bots pick bodies to feed on\n"),
    ])

    print("AIButeMgr.h")
    patch("ObjectDLL/AIButeMgr.h", [
        ("addweapon",
         r"^(\tprotected :\n\n\t\tvoid SetAIWeapon\(uint32 iTemplate, const std::string & szTagName\);\n)",
         lambda m: "\t\t// @TAG: registers an AI weapon at runtime (the bots' multiplayer weapons) without touching\n"
                   "\t\t// the attribute files, which the server checks against the clients'. Returns its id.\n"
                   "\t\tint AddWeapon(const AIWBM_AIWeapon & weapon)\n\t\t{\n"
                   "\t\t\tm_TagNames.push_back(weapon.Name + \"_AIWeapon\");\n"
                   "\t\t\tm_aWeapons.push_back(weapon);\n"
                   "\t\t\treturn (int)m_aWeapons.size() - 1;\n\t\t}\n\n" + m.group(1)),
    ])

    reply_code = ("\t\t\tif(szBotReply[0])\n\t\t\t{\n"
                  "\t\t\t\tHMESSAGEWRITE hBotReply = g_pLTServer->StartMessage(@TO, MID_MPMGR_MESSAGE);\n"
                  "\t\t\t\thBotReply->WriteDWord(-1);\n"
                  "\t\t\t\thBotReply->WriteString(szBotReply);\n"
                  "\t\t\t\tg_pLTServer->EndMessage(hBotReply);\n"
                  "\t\t\t\tg_pLTServer->CPrint(\"%s\", szBotReply);\n\t\t\t}\n")
    print("MultiplayerMgr.cpp")
    patch("ObjectDLL/MultiplayerMgr.cpp", [
        ("include", r'^(#include "MultiplayerMgr.h"[^\n]*\n)',
         lambda m: m.group(1) + '#include "BotMgr.h" // @TAG\n'),
        ("servercommand", r'^(\tif\(!_strnicmp\(szMsg, "lifecycle", 9\)\)\n)',
         lambda m: "\t// @TAG: bot administration commands (addbot, removebot, bots, botcount, botreload, bothelp)\n"
                   "\t{\n\t\tchar szBotReply[256];\n\t\tif(BotMgr_Command(szMsg, szBotReply, sizeof(szBotReply)))\n\t\t{\n"
                   + reply_code.replace("@TO", "(bFromMsg && senderID >= 0 && senderID < MAX_CLIENTS) ? "
                                                "m_ClientData[senderID].m_hClient : LTNULL")
                   + "\t\t\treturn LTTRUE;\n\t\t}\n\t}\n\n" + m.group(1)),
        ("consolecommand",
         r"^(\t\tcase SERVERSHELL_MESSAGE:\n\t\t\{\n\t\t\tchar szMsg\[256\];\n\t\t\tmemStream\.read\(szMsg, 256\);\n)",
         lambda m: m.group(1) + "\t\t\t// @TAG: \"/<bot command>\" typed in the dedicated server window\n"
                   "\t\t\tif(szMsg[0] == '/')\n\t\t\t{\n\t\t\t\tchar szBotReply[256];\n"
                   "\t\t\t\tif(BotMgr_Command(szMsg + 1, szBotReply, sizeof(szBotReply)))\n\t\t\t\t{\n"
                   + reply_code.replace("@TO", "LTNULL").replace("\t\t\t", "\t\t\t\t\t", 1).replace("\n\t\t\t", "\n\t\t\t\t\t")
                   + "\t\t\t\t\tbreak;\n\t\t\t\t}\n\t\t\t}\n"),
        ("mspatchkey", r'^(\tSendResponseInfo\("hostname", szName\);\n)',
         lambda m: "\t// @TAG: Master Server Patch 2.4 clients hide servers that do not report this key\n"
                   "\t// (JoinVersionFilter); the MSP server code sends it right before the host name.\n"
                   "\t{\n\t\tchar szMSPatch[] = \"2.4\";\n\t\tSendResponseInfo(\"mspatch\", szMSPatch);\n\t}\n"
                   + m.group(1)),
    ])

    print("Destructible.cpp")
    patch("ObjectDLL/Destructible.cpp", [
        ("include", r'^(#include "Destructible.h"[^\n]*\n)',
         lambda m: m.group(1) + '#include "BotMgr.h" // @TAG\n'),
        ("teamdamage",
         r"^(\t\t\t\tif\(g_pGameServerShell->GetMultiplayerMgr\(\)->ClientsOnSameTeam\(hClient1, hClient2\)\)\n"
         r"\t\t\t\t\{\n\t\t\t\t\tdamage\.fDamage = fDamage = 0\.0f;\n\t\t\t\t\}\n\t\t\t\}\n)",
         lambda m: m.group(1)
         + "\t\t\t// @TAG: the same rule between a bot and a teammate (bots are not clients)\n"
         + "\t\t\telse if(BotMgr_TeamDamageBlocked(m_hObject, hDamager))\n"
         + "\t\t\t{\n\t\t\t\tdamage.fDamage = fDamage = 0.0f;\n\t\t\t}\n"),
        ("dmglog",
         r"^(\t// See if we should actually process damage\.\.\.\n)",
         lambda m: "\t// @TAG: damage done by bots, for balance analysis (bots.cfg debug only)\n"
                   "\tif(g_pGameServerShell->GetGameType().IsMultiplayer())\n"
                   "\t\tBotMgr_OnDamage(m_hObject, hDamager, fDamage, (int)damage.eType);\n\n" + m.group(1)),
        ("botkill",
         r"^(\tif\(hDamager\)\n\t\tpKiller = IsPlayer\(hDamager\) \? )",
         lambda m: "\t// @TAG: a player killed by a bot is not a suicide; the kill is scored for the bot's side\n"
                   "\tif(hDamager && pVictim && BotMgr_PlayerKilledByBot(pVictim->GetClient(), hDamager))\n"
                   "\t\treturn;\n\n" + m.group(1)),
    ])

    print("MultiplayerMgr.cpp (game info)")
    patch("ObjectDLL/MultiplayerMgr.cpp", [
        ("gameinforows",
         r"^\thWrite->WriteByte\(m_nClients\);\n",
         lambda m: "\t// @TAG: bot scoreboard rows (option 'scoreboard'): players + rows stay within the clients' table\n"
                   "\tconst int nBotRows = BotMgr_ScoreRowsForJoiner(MAX_CLIENTS - 1 - (int)m_nClients);\n"
                   "\thWrite->WriteByte((uint8)(m_nClients + nBotRows));\n"),
        ("gameinfowrite",
         r"^(\tif\(m_nServerController != 0xFF\)\n\t\thWrite->WriteDWord\(m_ClientData\[m_nServerController\]\.m_nClientID\);\n)",
         lambda m: "\tBotMgr_WriteScoreRows(hWrite, nBotRows); // @TAG\n\n" + m.group(1)),
    ])

    print("MultiplayerMgr.cpp (end conditions)")
    patch("ObjectDLL/MultiplayerMgr.cpp", [
        ("dmlimits", r"^(LTBOOL MultiplayerMgr::CheckEndConditions_DM\(\)\n\{\n)",
         lambda m: m.group(1)
         + "\t// @TAG: with bot scoreboard rows (option 'scoreboard'), a bot reaching the frag or score limit\n"
         + "\t// ends the round like a player would\n"
         + "\tif(BotMgr_RowReachedLimit((m_GameInfo.m_nEndConditionFlags & MP_END_CONDITION_FRAGS) ? (int)(int16)m_GameInfo.m_nFragLimit : -1,\n"
         + "\t\t(m_GameInfo.m_nEndConditionFlags & MP_END_CONDITION_SCORE) ? (int)(int16)m_GameInfo.m_nScoreLimit : -1))\n"
         + "\t\treturn LTTRUE;\n\n"),
    ])

    print("MultiplayerMgr.cpp (survivor)")
    patch("ObjectDLL/MultiplayerMgr.cpp", [
        ("survcount",
         r"^(\tint nMutants = NumberOfClass\(m_GameInfo\.m_nSurv_MutateRace, LTTRUE\);\n)",
         lambda m: m.group(1)
         + "\t// @TAG: bots play Survivor too: count them as survivors / mutants and as participants\n"
         + "\tnSurvivors += BotMgr_SurvivorCount(0);\n"
         + "\tnMutants += BotMgr_SurvivorCount(1);\n"
         + "\tconst int nSurvClients = (int)m_nClients + BotMgr_SurvivorCount(0) + BotMgr_SurvivorCount(1);\n"),
        ("survclients1",
         r"^(\t\t// Make sure the sub state is set properly\n)\t\tif\(m_nClients < 2\)\n(\t\t\{\n\t\t\tm_nServerSubState = MPMGR_SUBSTATE_NONE;\n\t\t\}\n\t\telse if\(nMutants < 1\)\n)",
         lambda m: m.group(1) + "\t\tif(nSurvClients < 2) // @TAG\n" + m.group(2)),
        ("survclients2",
         r"^\t\t\t\(\(m_nClients > 1\) && \(nMutants > 0\) && \(nSurvivors < 1\)\)\)\n",
         lambda m: "\t\t\t((nSurvClients > 1) && (nMutants > 0) && (nSurvivors < 1))) // @TAG\n"),
        ("survround",
         r"^(\t\t\t\t\t\tChangeClientClass\(m_ClientData\[i\]\.m_hClient, m_GameInfo\.m_nSurv_SurvivorRace, LTFALSE\);\n"
         r"\t\t\t\t\t\}\n\n\t\t\t\t\tm_ClientData\[i\]\.m_nNextClass = -1;\n\t\t\t\t\tm_ClientData\[i\]\.m_nConsecutiveFrags = 0;\n"
         r"\t\t\t\t\}\n\t\t\t\}\n)",
         lambda m: m.group(1) + "\n\t\t\t// @TAG: the bots become survivors again for the new round\n"
                   "\t\t\tBotMgr_SurvivorNewRound(bLastRound);\n"),
        ("survlimits",
         r"^(LTBOOL MultiplayerMgr::CheckEndConditions_Survivor\(\)\n\{\n)",
         lambda m: m.group(1)
         + "\t// @TAG: with bot scoreboard rows, a bot reaching the score limit ends the map like a player would\n"
         + "\tif(BotMgr_RowReachedLimit(-1, (m_GameInfo.m_nEndConditionFlags & MP_END_CONDITION_SCORE) ? "
           "(int)(int16)m_GameInfo.m_nScoreLimit : -1))\n\t\treturn LTTRUE;\n\n"),
    ])

    print("MultiplayerMgr.cpp (hunt)")
    patch("ObjectDLL/MultiplayerMgr.cpp", [
        ("huntjoin",
         r"^(\t\t\tint nPrey = NumberOfClass\(m_GameInfo\.m_nHunt_PreyRace, LTTRUE\);\n)",
         lambda m: m.group(1) + "\t\t\t// @TAG: bots are hunters and prey too\n"
                   "\t\t\tnHunters += BotMgr_RoleCount(0);\n\t\t\tnPrey += BotMgr_RoleCount(1);\n"),
        ("huntcount",
         r"^(void MultiplayerMgr::Update_Hunt\(\)\n\{\n\t// Count the number of hunters and prey in the level\n"
         r"\tint nHunters = NumberOfClass\(m_GameInfo\.m_nHunt_HunterRace, LTTRUE\);\n"
         r"\tint nPrey = NumberOfClass\(m_GameInfo\.m_nHunt_PreyRace, LTTRUE\);\n)",
         lambda m: m.group(1) + "\t// @TAG: bots are hunters and prey too\n"
                   "\tnHunters += BotMgr_RoleCount(0);\n\tnPrey += BotMgr_RoleCount(1);\n"
                   "\tconst int nHuntClients = (int)m_nClients + BotMgr_RoleCount(0) + BotMgr_RoleCount(1);\n"),
        ("huntdest",
         r"^\tint nDestHunters = m_nClients / \(m_GameInfo\.m_nHunt_Ratio \+ 1\);\n",
         lambda m: "\tint nDestHunters = nHuntClients / (m_GameInfo.m_nHunt_Ratio + 1); // @TAG\n"),
        ("huntbalance",
         r"^\tif\(nHunters < nDestHunters\)\n",
         lambda m: "\t// @TAG: rebalance with a bot when one can move, so people are not switched for the bots' sake\n"
                   "\tif(BotMgr_HuntBalance(nHunters - nDestHunters))\n\t{\n\t}\n\telse if(nHunters < nDestHunters)\n"),
        ("huntlimits",
         r"^(LTBOOL MultiplayerMgr::CheckEndConditions_Hunt\(\)\n\{\n)",
         lambda m: m.group(1)
         + "\t// @TAG: with bot scoreboard rows, a bot at the frag limit ends the map like a player would\n"
         + "\tif(BotMgr_RowReachedLimit((m_GameInfo.m_nEndConditionFlags & MP_END_CONDITION_FRAGS) ? "
           "(int)(int16)m_GameInfo.m_nFragLimit : -1, -1))\n\t\treturn LTTRUE;\n\n"),
    ])

    print("MultiplayerMgr.cpp (server list)")
    patch("ObjectDLL/MultiplayerMgr.cpp", [
        ("listnum", r'^\tSendResponseInfo\("numplayers", m_nClients\);\n',
         lambda m: "\t// @TAG: option 'listbots': bots count as players in the server list (never filling the last slot)\n"
                   "\tSendResponseInfo(\"numplayers\", m_nClients + BotMgr_ListedBots(m_nClients, m_GameInfo.m_nPlayerLimit));\n"),
        ("listplayers",
         r"^(\t\t\tnClient\+\+;\n\t\t\}\n\t\}\n)(\n\treturn TRUE;\n\}\n)",
         lambda m: m.group(1)
         + "\n\t// @TAG: option 'listbots': the listed bots after the players (same count as numplayers)\n"
         + "\tconst int nBots = BotMgr_ListedBots(m_nClients, m_GameInfo.m_nPlayerLimit);\n"
         + "\tfor(int b = 0; b < nBots; b++, nClient++)\n\t{\n"
         + "\t\tsprintf(sPlayer, \"player_%i\", nClient);\n"
         + "\t\tSendResponseInfo(sPlayer, const_cast<char*>(BotMgr_ListedBotName(b)));\n"
         + "\t\tsprintf(sRace, \"race_%i\", nClient);\n"
         + "\t\tSendResponseInfo(sRace, BotMgr_ListedBotRace(b));\n"
         + "\t\tsprintf(sPing, \"ping_%i\", nClient);\n"
         + "\t\tSendResponseInfo(sPing, 1);\n\t}\n"
         + m.group(2)),
    ])

    print("GameSpyMgr.cpp")
    patch("../LT2/lithshared_src/gamespymgr/GameSpyMgr.cpp", [
        ("sierraup",
         r"^(\tFILE\* pFile = fopen\(\"SierraUp\.cfg\", \"r\"\);\n\n)\twhile \(fgets\(sLine, 256, pFile\)\)\n",
         lambda m: m.group(1) + "\t// @TAG: without SierraUp.cfg (minimal server copies) the retail code crashed here\n"
                   "\twhile (pFile && fgets(sLine, 256, pFile))\n"),
        ("sierraupclose",
         r"^(\t\t\tAddDirServer\(sAddr\);\n\t\t\}\n\t\}\n)",
         lambda m: m.group(1) + "\tif (pFile) fclose(pFile); // @TAG\n"),
        ("mspmaster", r'^(\t)m_sGameSpyAddr\.Format\("master%i\.gamespy\.com", nGameLoc\);\n',
         lambda m: m.group(1) + "// @TAG: heartbeat to the Master Server Patch master server, like the MSP server code\n"
                   + m.group(1) + "// (GameSpy's master servers are gone); internet servers only, LAN servers send none\n"
                   + m.group(1) + 'm_sGameSpyAddr = "master.avp2msp.com";\n'),
    ])

    print("MultiplayerMgr.h")
    patch("ObjectDLL/MultiplayerMgr.h", [
        ("friend", r"^(class MultiplayerMgr : public CGameSpyMgr\n\{\n)",
         lambda m: m.group(1) + "\t\tfriend struct BotScoreAccess; // @TAG: bots score kills on the clients' rows and teams\n\n"),
        ("levelindex", r"^(\t\tGameType\tGetGameType\(\)\t+\{ return m_GameInfo\.m_nGameType; \}\n)",
         lambda m: m.group(1) + "\t\tuint8\t\tGetCurrentLevelIndex() const\t{ return m_nCurrentLevel; } // @TAG\n"),
    ])

    # Single-player AI rules a player does not have: bots take the player's path (BotMgr_LikePlayer, bots.cfg
    # switches playerburn, playerlimbs, playerrecoil, playerhits; default on).
    print("Destructible.cpp (player rules)")
    patch("ObjectDLL/Destructible.cpp", [
        # DoDamage: any napalm damage (a progressive tick included) killed an AI that can burn at once
        ("botnapalm",
         r"^\tif\(IsAI\(m_hObject\) && \(eType == DT_NAPALM\) && pChar\)\n",
         lambda m: "\t// @TAG: a burning bot takes the napalm damage over time, as a player does\n"
                   "\tif(IsAI(m_hObject) && (eType == DT_NAPALM) && pChar && !BotMgr_LikePlayer(m_hObject, BOTRULE_NAPALM))\n"),
    ])

    print("Character.cpp (player rules)")
    patch("ObjectDLL/Character.cpp", [
        # HandleAlienDamage: an alien AI could lose a limb while alive; a lost leg made it crawl (InjuredSpeed 55,
        # no run, no wall walk). Players never lose limbs.
        ("botlimbs",
         r"^(\t// Don't do any more of this if I am a player\n)\tif\(!IsPlayer\(m_hObject\)\)\n",
         lambda m: m.group(1) + "\t// @TAG: bots keep their limbs, as players do\n"
                   "\tif(!IsPlayer(m_hObject) && !BotMgr_LikePlayer(m_hObject, BOTRULE_LIMBS))\n"),
    ])

    print("AIActionMisc.cpp (player rules)")
    patch("ObjectDLL/AIActionMisc.cpp", [
        # AIActionFireWeapon::Start: a recoiling AI could not start firing; a player's recoil is only an animation
        ("botrecoil",
         r"^(\t// unable to fire under these conditions\n)\tif \(GetAIPtr\(\)->IsRecoiling\(\) \|\| GetAIPtr\(\)->IsNetted\(\)\)\n",
         lambda m: m.group(1) + "\t// @TAG: a bot fires through its recoil animation, as a player does (a net still stops it)\n"
                   "\tif ((GetAIPtr()->IsRecoiling() && !BotMgr_LikePlayer(GetAIPtr()->m_hObject, BOTRULE_RECOIL)) || GetAIPtr()->IsNetted())\n"),
    ])

    print("AI.cpp (player rules)")
    patch("ObjectDLL/AI.cpp", [
        # CAI::IgnoreDamageMsg: an AI ignored damage from its allies (itself included: its own splash) and from
        # alien acid pools (BodyProp); players take both. Friendly fire off still zeroes teammate damage
        # (hook 'teamdamage'), and an ally never becomes a target (AICombatGoal::IsMoreDangerous, CAITarget).
        ("botimmune",
         r"^(LTBOOL CAI::IgnoreDamageMsg\(const DamageStruct & damage_msg\)\n\{\n\tif\( !m_bActive \)\n\t\treturn LTTRUE;\n\n)"
         r"\tif\( damage_msg\.hDamager \)\n",
         lambda m: m.group(1) + "\t// @TAG: bots take damage from themselves, teammates and acid pools, as players do\n"
                   "\tif( damage_msg.hDamager && !BotMgr_LikePlayer(m_hObject, BOTRULE_IMMUNITY) )\n"),
    ])

    # Predator weapons as a player's (bots.cfg predammo / predlock): finite spears, one disc that must come back
    # or be recalled, and the disc / shoulder cannon homing on the players' client auto-target
    print("AI.cpp (Predator ammo, auto-target)")
    patch("ObjectDLL/AI.cpp", [
        ("botcommit", r"^(\t\treturn false;  // the gun is still turning toward the target\n)",
         lambda m: m.group(1)
         + "\t// @TAG: Predator weapons as a player's: a spear or the disc is spent, and the disc and shoulder\n"
         + "\t// cannon home on the players' client auto-target instead of whatever the AI targets\n"
         + "\tif(bBot)\n"
         + "\t\tfireInfo.hTestObj = BotMgr_FireCommit(m_hObject, fireInfo.hTestObj, fireInfo.vFirePos, fireInfo.vPath);\n"),
        ("botammo", r"^(static bool HasAmmo\(const AIWBM_AIWeapon & weapon_bute, [^\n]*\n\{\n)",
         lambda m: m.group(1)
         + "\t// @TAG: a Predator bot's spear gun without spears, or its disc while thrown, has no ammo\n"
         + "\tif( !BotMgr_WeaponUsable(hCharacter, weapon_bute.Name.c_str()) )\n\t\treturn false;\n\n"),
        ("botkeepideal",
         r"^\tif\( m_pIdealWeaponButes && !HasAmmo\(\*m_pIdealWeaponButes, \*m_pWeaponPosition, m_hObject, GetInjuredMovement\(\) \) \)\n",
         lambda m: "\t// @TAG: a Predator bot's disc comes back: keep its ideal weapon instead of dropping it for good\n"
                   "\tif( m_pIdealWeaponButes && !BotMgr_PredAmmoBot(m_hObject)\n"
                   "\t\t&& !HasAmmo(*m_pIdealWeaponButes, *m_pWeaponPosition, m_hObject, GetInjuredMovement() ) )\n"),
    ])
    print("PredDisk.cpp")
    patch("ObjectDLL/PredDisk.cpp", [
        ("include", r'^(#include "PredDisk.h"[^\n]*\n)',
         lambda m: m.group(1) + '#include "BotMgr.h" // @TAG\n'),
        ("botdisc", r"^(\t\t\tif\(pChar->IsDead\(\) \|\| )pChar->HasDiscAmmo\(\)\)\n",
         lambda m: "\t\t\t// @TAG: a Predator bot's stuck disc waits for its recall, as a player's (an AI's disc pool is infinite)\n"
                   + m.group(1) + "(!BotMgr_PredAmmoBot(m_hFiredFrom) && pChar->HasDiscAmmo()))\n"),
    ])

    # Hearing: in multiplayer the AI never heard anything of other characters' movement; players hear every
    # footstep, jump and landing (BotMgr_HearStep, bots.cfg 'hearsteps').
    print("Character.cpp (hearing)")
    patch("ObjectDLL/Character.cpp", [
        # CCharacter::HandleFootstepKey: footstep keys of players and AI reach here in multiplayer too, but only the
        # single-player AI was told
        ("hearsteps",
         r"^(\t\t\t\(\*iter\)->GetSenseMgr\(\)\.Footstep\(m_LastFootstepSoundInfo,this\);\n\t\t\}\n\t\}\n)",
         lambda m: m.group(1) + "\t// @TAG: in multiplayer the bots hear it, as the players' clients play it\n"
                   "\telse\n\t\tBotMgr_HearStep(this, BOTHEAR_STEP);\n"),
        # CCharacter::PlayExclamationSound: the single-player AI's voice lines ("What was that?" on hearing steps)
        ("botquiet",
         r"^(int CCharacter::PlayExclamationSound\(const char\* szSoundType\)\n\{\n)",
         lambda m: m.group(1) + "\t// @TAG: bots do not speak the single-player AI's lines (players have no such voice)\n"
                   "\tif( BotMgr_IsBot(m_hObject) )\n\t\treturn -1;\n\n"),
    ])
    print("PlayerObj.cpp (hearing)")
    relay = lambda cfx: (r"^(\t\t\t\tg_pLTServer->WriteToMessageByte\(hMessage, " + cfx +
                         r"\);\n\t\t\t\tg_pLTServer->EndMessage2\(hMessage, MESSAGE_NAGGLE\);\n)")
    patch("ObjectDLL/PlayerObj.cpp", [
        ("include", r'^(#include "PlayerObj.h"[^\n]*\n)',
         lambda m: m.group(1) + '#include "BotMgr.h" // @TAG\n'),
        # a player's client reports its jump, landing and first step; the server relays them to the other clients
        ("hearjump", relay("CFX_JUMP_SOUND"),
         lambda m: m.group(1) + "\t\t\t\tBotMgr_HearStep(this, BOTHEAR_JUMP); // @TAG: the bots hear it too\n"),
        ("hearland", relay("CFX_LAND_SOUND"),
         lambda m: m.group(1) + "\t\t\t\tBotMgr_HearStep(this, BOTHEAR_LAND); // @TAG: the bots hear it too\n"),
        ("hearfirststep", relay("CFX_FOOTSTEP_SOUND"),
         lambda m: m.group(1) + "\t\t\t\tBotMgr_HearStep(this, BOTHEAR_STEP); // @TAG: the bots hear it too\n"),
    ])
    # A person who hears something glances toward it and goes on playing; the single-player AI stood 2-4 s
    # "suspicious", walked there and searched 3-5 s (BotMgr_QuickInvestigate / GlanceStart / GlanceFollow).
    print("AIInvestigate.cpp (hearing)")
    patch("ObjectDLL/AIInvestigate.cpp", [
        ("include", r'^(#include "AIInvestigate.h"[^\n]*\n)',
         lambda m: m.group(1) + '#include "BotMgr.h" // @TAG\n'),
        ("investnoanim",
         r"^\tif\( pAttackSet && \(pAttackSet->m_fPlayChance > GetRandom\(0\.0f,1\.0f\)\)  \)\n",
         lambda m: "\t// @TAG: no investigate animation for a bot\n"
                   "\tif( pAttackSet && !BotMgr_QuickInvestigate(GetAIPtr()) && (pAttackSet->m_fPlayChance > GetRandom(0.0f,1.0f))  )\n"),
        ("investglance",
         r"^(\tm_tmrSuspicious\.Start\(fSuspiciousDur\);[^\n]*\n)",
         lambda m: m.group(1)
         + "\t// @TAG: a bot only glances toward it (the turn below, then a head turn's time)\n"
         + "\t{\n"
         + "\t\tCAISense * pGlanceSense = GetActiveSensePtr();\n"
         + "\t\tconst LTFLOAT fGlance = BotMgr_GlanceStart(GetAIPtr(), pGlanceSense ? pGlanceSense->GetStimulus().GetHOBJECT() : LTNULL,\n"
         + "\t\t\tpGlanceSense && pGlanceSense == m_pHearEnemyFootstep);\n"
         + "\t\tif( fGlance > 0.0f )\n"
         + "\t\t\tm_tmrSuspicious.Start(fGlance);\n"
         + "\t}\n"),
        ("investglanceend",
         r"^(void InvestigateGoal::UpdateSuspicious\(RootFSM \*\)\n\{\n)",
         lambda m: m.group(1)
         + "\t// @TAG: at the end of a bot's glance (and of its turn): go toward it if it is still coming closer (or is\n"
         + "\t// not a footstep), otherwise go back to what it was doing\n"
         + "\tif( BotMgr_QuickInvestigate(GetAIPtr()) )\n"
         + "\t{\n"
         + "\t\tconst AIActionTurnTo * pTurn = dynamic_cast<const AIActionTurnTo*>(GetAIPtr()->GetCurrentAction());\n"
         + "\t\tif( !m_tmrSuspicious.Stopped() || (pTurn && !pTurn->IsDone()) )\n"
         + "\t\t\treturn;\n"
         + "\t\tm_pSense = GetActiveSensePtr();\n"
         + "\t\tif( m_bTriggered || (m_pSense && BotMgr_GlanceFollow(GetAIPtr(), m_pSense == m_pHearEnemyFootstep)) )\n"
         + "\t\t\tm_RootFSM.AddMessage(Root::eStimulus);\n"
         + "\t\telse\n"
         + "\t\t\tm_RootFSM.AddMessage(Root::eFinished);\n"
         + "\t\treturn;\n"
         + "\t}\n\n"),
        ("investnosearch",
         r"^(void InvestigateGoal::StartSearch\(RootFSM \* fsm\)\n\{\n)",
         lambda m: m.group(1)
         + "\t// @TAG: a bot that got there does not stand 3-5 s looking around\n"
         + "\tif( BotMgr_QuickInvestigate(GetAIPtr()) )\n"
         + "\t{\n"
         + "\t\tm_RootFSM.AddMessage(Root::eFinished);\n"
         + "\t\treturn;\n"
         + "\t}\n\n"),
    ])

    print("AI.cpp (motion tracker)")
    patch("ObjectDLL/AI.cpp", [
        # CAI::CreateGoal: the Marine bots' reaction to their motion tracker (BotRoamGoal.h, BotMgr_TrackerBid)
        ("creategoaltracker",
         r'^(\telse if\( stricmp\(szGoalName, "BotRoam"\) == 0 \) // avp2bots:creategoal\n\t\tpGoal = new BotRoamGoal\(this, szGoalName\);\n)',
         lambda m: m.group(1) + '\telse if( stricmp(szGoalName, "BotTracker") == 0 ) // @TAG\n'
                   '\t\tpGoal = new BotTrackerGoal(this, szGoalName);\n'),
    ])

    # Ammunition like players (bots.cfg ammopickups): bots open the ammo boxes of their species and take spears
    # stuck in walls; BotMgr counts the ammo they use (spears, pulse-rifle grenades)
    print("AmmoBox.h / .cpp")
    patch("ObjectDLL/AmmoBox.h", [
        ("friend", r"^(class AmmoBox : public Prop\n\{\n\tpublic :\n)",
         lambda m: m.group(1) + "\t\tfriend struct BotPickupAccess; // @TAG: bots look up ammo boxes\n"),
    ])
    patch("ObjectDLL/AmmoBox.cpp", [
        ("include", r'^(#include "AmmoBox.h"[^\n]*\n)',
         lambda m: m.group(1) + '#include "BotMgr.h" // @TAG\n'),
        ("botammobox",
         r"^\t//see that we are being activated by a player\n\tif\(!IsPlayer\(hObj\)\)\treturn;\n\n"
         r"(\t// \[KLS\] 9/23/01 Make sure the player is alive\.\.\.\n"
         r"\tCPlayerObj\* pPlayer = \(CPlayerObj\*\)g_pLTServer->HandleToObject\(hObj\);\n"
         r"\tif \(!pPlayer \|\| pPlayer->GetState\(\) != PS_ALIVE\) return;\n\n\n"
         r"\tif\(IsAlien\(hObj\) \|\| IsExosuit\(hObj\)\) return;\n"
         r"\telse if\(IsHuman\(hObj\) && !m_bHumanOnly\) return;\n"
         r"\telse if\(IsPredator\(hObj\) && m_bHumanOnly\) return;\n)",
         lambda m: "\t// @TAG: a bot opens a box of its species like a player (BotMgr_CanOpenAmmoBox; BotMgr counts the ammo)\n"
                   "\tconst bool bBotOpens = !IsPlayer(hObj) && BotMgr_CanOpenAmmoBox(hObj, m_bHumanOnly ? true : false);\n"
                   "\t//see that we are being activated by a player\n\tif(!IsPlayer(hObj) && !bBotOpens)\treturn;\n\n"
                   "\tif(!bBotOpens)\n\t{\n" + m.group(1) + "\t}\n"),
        ("botammoboxtake",
         r"^(\t\t\tif\(!m_strPickupCommand\[i\]\.IsNull\(\)\)\n\t\t\t\{\n)(\t\t\t\t// Send the message to the character who touched the pickup\n)",
         lambda m: m.group(1)
         + "\t\t\t\t// @TAG: a bot's share is counted by BotMgr (the AI's own pools are infinite)\n"
         + "\t\t\t\tif(bBotOpens)\n\t\t\t\t{\n"
         + "\t\t\t\t\tBotMgr_AmmoBoxPickup(hObj, m_strPickupCommand[i].CStr());\n\t\t\t\t\tcontinue;\n\t\t\t\t}\n"
         + m.group(2)),
    ])
    print("PickupObject.cpp (ammo)")
    patch("ObjectDLL/PickupObject.cpp", [
        ("botammopickup", r"^(\t// If we're only allowed for players\.\.\. check that\n)",
         lambda m: "\t// @TAG: a bot takes ammo it counts (spears stuck in walls) as a player does: BotMgr credits it and the\n"
                   "\t// pickup goes as if the character had answered MID_PICKEDUP\n"
                   "\tif(!IsPlayer(hSender) && BotMgr_TakeAmmoPickup(hSender, m_puButes.m_szMessage))\n\t{\n"
                   "\t\tuint32 nAmt[4] = {0, 0, 0, 0};\n\t\tPickedUp(hSender, nAmt, LTFALSE);\n\t\treturn LTTRUE;\n\t}\n\n"
                   + m.group(1)),
    ])

    # Alien lifecycle: a live bot is never SetCharacter'd (in MP ResetCharacter rebuilds the attachments and the
    # AI's weapon pointer dangles: crash in HasAmmo); the hugged body is known to BotMgr from the attach on.
    print("Character.cpp (lifecycle)")
    patch("ObjectDLL/Character.cpp", [
        # CCharacter::UpdateChestbursterCycle: the grown chestburster becomes its host's adult
        ("botgrow",
         r"^(\t\t\t// Get a handle to the player object\n\t\t\tCPlayerObj \*pPObj = dynamic_cast<CPlayerObj\*>\(g_pLTServer->HandleToObject\(m_hObject\)\);\n)",
         lambda m: "\t\t\t// @TAG: a bot chestburster is not switched in place (in multiplayer SetCharacter rebuilds the\n"
                   "\t\t\t// attachments under the AI's weapon pointers): BotMgr spawns the adult (this set) at its place\n"
                   "\t\t\tif( BotMgr_ChestbursterGrown(m_hObject, (int)nNewCharacter) )\n"
                   "\t\t\t{\n"
                   "\t\t\t\tLTVector vGrowPos = m_cmMovement.GetPos();\n"
                   "\t\t\t\tg_pServerSoundMgr->PlaySoundFromPos(vGrowPos, (char*)\"Sounds\\\\Weapons\\\\Alien\\\\FaceHug\\\\chestburst.wav\", 1000.0f, SOUNDPRIORITY_MISC_HIGH);\n"
                   "\t\t\t\treturn;\n"
                   "\t\t\t}\n\n" + m.group(1)),
        # CCharacter::ProcessCommand "CHARACTER <set>" (level triggers): never on a bot
        ("botnochar",
         r"^(\telse if \(0 == _stricmp\(\*pTokens, TRIGGER_CHARACTER\)\)\n\t\{\n)\t\tif \(nArgs == 2\)\n",
         lambda m: m.group(1) + "\t\t// @TAG: a bot's character set is changed by BotMgr only (respawn in place)\n"
                   "\t\tif (nArgs == 2 && !BotMgr_IsBot(m_hObject))\n"),
    ])
    print("BodyProp.cpp (lifecycle)")
    patch("ObjectDLL/BodyProp.cpp", [
        # the facehugger is attached to the body: BotMgr keeps the body awake and knows the host for the fallback
        ("bothugattach",
         r"^(\t\t\t\t\t\tm_fFaceHugTime = g_pLTServer->GetTime\(\);\n)",
         lambda m: m.group(1) + "\t\t\t\t\t\tBotMgr_OnFacehugAttach(m_hFaceHugger, m_hObject, (int)m_nCharacterSet); // @TAG\n"),
    ])
    # Found 2026-11-12 on a live server: a human Marine killed a Marine bot in Deathmatch and every
    # client got the single-player "Mission failed!" screen. CCharacter::CheckMissionFailed sends SCM_MISSION_FAILED
    # to ALL clients when a player kills a non-player ally of his own class; stock multiplayer had no AI characters,
    # so the rule never ran there. In multiplayer it never applies (a bot is a deathmatch opponent).
    # A player's vector weapons (bullets, rail gun, Smart Gun, shotgun, pistols...) are hit-tested by the client in
    # multiplayer (CWeaponModel::ClientHitDetection): when the ray meets any character, the client sends
    # MID_WEAPON_HIT instead of the fire message, and the server applied it only to a player target
    # (CPlayerObj::ValidatePosition). Hits on bots were dropped: only explosions and misses of the client's node test
    # hurt them ("10 rail gun hits on a Predalien and he's fine"). Bots take them too, checked against the bot's
    # position (BotMgr_ValidateHit).
    print("PlayerObj.cpp (players' client-side hits on bots)")
    patch("ObjectDLL/PlayerObj.cpp", [
        ("botweaponhit",
         r"^\tif\(pPlayer && pPlayer->ValidatePosition\(vTargetPos, fTimeStamp\)\)\n",
         lambda m: "\t// @TAG: a hit the client claims on a bot counts as on a player (the server took players only)\n"
                   "\tif((pPlayer && pPlayer->ValidatePosition(vTargetPos, fTimeStamp)) ||\n"
                   "\t   (!pPlayer && BotMgr_ValidateHit(m_hObject, hTarget, vTargetPos, vFirePos)))\n"),
    ])

    print("Character.cpp (multiplayer: no single-player 'mission failed')")
    patch("ObjectDLL/Character.cpp", [
        ("botmissionfail",
         r"^(\t// Do nothing if the dying character is a player\n\tif \(IsPlayer\(m_hObject\)\)\n\t\treturn;\n)",
         lambda m: m.group(1) + "\n\t// @TAG: never in multiplayer, where the AI characters are the bots (their killer\n"
                   "\t// is a deathmatch opponent, not a player who shot his ally): every client showed \"Mission failed!\"\n"
                   "\tif (g_pGameServerShell->GetGameType().IsMultiplayer())\n"
                   "\t\treturn;\n"),
    ])


def server_ext():
    """Hooks of the server extension points (bots/ServerExt.h; no-ops in the public build): joins, leaves, renames, chat,
    world starts, deaths, and a spectator camera client that never spawns."""
    print("MultiplayerMgr.cpp (server extensions)")
    patch("ObjectDLL/MultiplayerMgr.cpp", [
        ("srvinclude", r'^(#include "MspClient.h" // avp2bots:mspinclude\n)',
         lambda m: m.group(1) + '#include "ServerExt.h" // @TAG\n'),
        ("srvjoin", r'^([ \t]*)(LTBOOL bRenamed = CreateUniqueClientName\(hClient\);\n)',
         lambda m: m.group(1) + m.group(2) + m.group(1)
                   + "ServerExt_ClientAdded(hClient, m_ClientData[i].m_nClientID, m_ClientData[i].m_Data.m_szName); // @TAG\n"),
        ("srvleave", r"^(\t// Otherwise, get rid of his ass! He's been drinking all the beer!\n)",
         lambda m: "\tServerExt_ClientRemoved(hClient, m_ClientData[nIndex].m_nClientID, m_ClientData[nIndex].m_Data.m_szName); // @TAG\n\n"
                   + m.group(1)),
        ("srvchat", r'^([ \t]*)(ServerAppMessage\(szString, nIndex\);\n)',
         lambda m: m.group(1) + m.group(2) + m.group(1)
                   + "ServerExt_Chat(m_ClientData[nIndex].m_nClientID, m_ClientData[nIndex].m_Data.m_szName, szString); // @TAG\n"),
        ("srvnameold", r'^([ \t]*)(memset\(m_ClientData\[nIndex\]\.m_Data\.m_szName, 0, MAX_MP_CLIENT_NAME_LENGTH\);\n)',
         lambda m: m.group(1) + "char szSrvOld[MAX_MP_CLIENT_NAME_LENGTH]; // @TAG: old name, for the NAME event\n"
                   + m.group(1) + "memcpy(szSrvOld, m_ClientData[nIndex].m_Data.m_szName, sizeof(szSrvOld));\n"
                   + m.group(1) + "szSrvOld[sizeof(szSrvOld) - 1] = 0;\n" + m.group(1) + m.group(2)),
        ("srvnamesan", r'^([ \t]*)(strncpy\(m_ClientData\[nIndex\]\.m_Data\.m_szName, szString, 21\);\n)',
         lambda m: m.group(1) + m.group(2) + m.group(1)
                   + "Msp_SanitizeName(m_ClientData[nIndex].m_Data.m_szName); // @TAG: a renamed player must not freeze AvP2Serv.exe either\n"),
        ("srvname", r'^([ \t]*)(CreateUniqueClientName\(m_ClientData\[nIndex\]\.m_hClient\);\n[ \t]*SetUpdateGameServ\(\);\n)',
         lambda m: m.group(1) + m.group(2) + m.group(1)
                   + "ServerExt_ClientRenamed(m_ClientData[nIndex].m_nClientID, szSrvOld, m_ClientData[nIndex].m_Data.m_szName); // @TAG\n"),
        ("srvcamspawn", r'^(\t// Get the handle to this client\n\tHCLIENT hClient = pPlayer->GetClient\(\);\n\n\tif\(!hClient\)\n\t\treturn LTTRUE;\n)',
         lambda m: m.group(1) + "\n\t// @TAG: the kill-clip camera client never spawns, it stays an observer (BotCamera.cpp)\n"
                   "\tif(ServerExt_IsCameraClient(hClient))\n\t{\n\t\tif(pPlayer->GetState() != PS_OBSERVING)\n"
                   "\t\t\tpPlayer->SetObservePoint_Random();\n\t\treturn LTFALSE;\n\t}\n"),
        ("srvworld", r'^(void MultiplayerMgr::ServerAppPostStartWorld\( \)\n\{\n)',
         lambda m: m.group(1) + "\tServerExt_WorldStarted(); // @TAG: MAP event, camera reset (before the serverapp check)\n\n"),
    ])
    print("GameServerShell.cpp (server extensions)")
    patch("ObjectDLL/GameServerShell.cpp", [
        ("srvinclude", r'^(#include "BotMgr.h" // avp2bots:include\n)',
         lambda m: m.group(1) + '#include "ServerExt.h" // @TAG\n'),
        ("srvdisconnect", r'^(void CGameServerShell::OnRemoveClient\(HCLIENT hClient\)\n\{\n)',
         lambda m: m.group(1) + "\tServerExt_ClientDisconnected(hClient); // @TAG: LEAVE (world exits also happen at every map change)\n\n"),
    ])
    print("Destructible.cpp (server extensions)")
    patch("ObjectDLL/Destructible.cpp", [
        ("srvinclude", r'^(#include "BotMgr.h" // avp2bots:include\n)',
         lambda m: m.group(1) + '#include "ServerExt.h" // @TAG\n'),
        ("srvdeath", r'^(void CDestructible::HandleDestruction\(HOBJECT hDamager\)\n\{\n)',
         lambda m: m.group(1) + "\t// @TAG: KILL event (characters only)\n"
                   "\tif(dynamic_cast<CCharacter*>(g_pLTServer->HandleToObject(m_hObject)))\n"
                   "\t\tServerExt_Death(m_hObject, hDamager, (int)m_eLastDamageType);\n\n"),
    ])


if __name__ == "__main__":
    main()
    server_ext()
