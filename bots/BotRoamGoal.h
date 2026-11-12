// BotRoamGoal: an AI goal that wanders between random waypoints of the bot navigation graph.
// Its bid (15) sits above Patrol (10) and below AttackOther (30), Investigate (40) and
// AttackSeeEnemy (60), so a bot roams only while it has nothing better to do.
// Created with the AI command "AG BotRoam" (see CAI::CreateGoal).

#ifndef AVP2BOTS_BOTROAMGOAL_H
#define AVP2BOTS_BOTROAMGOAL_H

#include "Goal.h"
#include "AIStrategyFollowPath.h"

class BotRoamGoal : public Goal
{
public:
    BotRoamGoal(CAI* pAI, std::string create_name);

    virtual void Load(HMESSAGEREAD hRead);
    virtual void Save(HMESSAGEWRITE hWrite);

    // 45 while a hurt alien bot feeds on a body (or has one to go to): above what follows a fight (AttackOther 30
    // waiting for a new target, Investigate 40), below seeing an enemy (AttackSeeEnemy 60)
    virtual LTFLOAT GetBid();
    // 65: a run for spears in a fight (BotMgr_AmmoRunWanted) outbids the attack (AttackSeeEnemy 60)
    virtual LTFLOAT GetMaximumBid() { return 65.0f; }

    virtual void Start();
    virtual void End();
    virtual void Update();

    virtual CMusicMgr::EMood GetMusicMoodModifier() { return CMusicMgr::eMoodNone; }

#ifndef _FINAL
    virtual const char* GetDescription() const { return "BotRoam"; }
#endif

private:
    void PickDestination();

    CAIStrategyFollowPath m_Follow;
    LTFLOAT m_fRetryTime;
    LTFLOAT m_fGiveUpTime;
    LTSmartLink m_hFeed;   // an alien bot walking to, or feeding on, this body (BotMgr_FeedGoal)
    // the last random destination, to resume after a short interruption (a glance toward a sound, 'hearsteps')
    LTVector m_vLastDest;
    bool m_bHasLastDest;
    LTFLOAT m_fEndTime;
};

// BotTrackerGoal: a Marine bot's reaction to its motion tracker (BotMgr_TrackerBid, bid 35 while a contact
// comes closer in front, else 0): it turns toward the blip and holds there, facing it, while it keeps coming.
// Created with the AI command "AG BotTracker" (hook 'creategoaltracker').
class BotTrackerGoal : public Goal
{
public:
    BotTrackerGoal(CAI* pAI, std::string create_name);

    virtual void Load(HMESSAGEREAD hRead);
    virtual void Save(HMESSAGEWRITE hWrite);

    virtual LTFLOAT GetBid();
    virtual LTFLOAT GetMaximumBid() { return 35.0f; }

    virtual void Start();
    virtual void End();
    virtual void Update();

    virtual CMusicMgr::EMood GetMusicMoodModifier() { return CMusicMgr::eMoodNone; }
#ifndef _FINAL
    virtual const char* GetDescription() const { return "BotTracker"; }
#endif

private:
    void Face();
};

#endif
