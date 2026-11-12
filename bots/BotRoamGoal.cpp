#include "stdafx.h"
#include "BotRoamGoal.h"
#include "BotMgr.h"
#include "BotNav.h"
#include "AI.h"
#include "AIActionMisc.h"

using avp2bots::BotNav;
using avp2bots::Vec3;

BotRoamGoal::BotRoamGoal(CAI* pAI, std::string create_name)
    : Goal(pAI, create_name),
      m_Follow(pAI),
      m_fRetryTime(0.0f),
      m_fGiveUpTime(0.0f),
      m_bHasLastDest(false),
      m_fEndTime(-1000.0f)
{
}

LTFLOAT BotRoamGoal::GetBid()
{
    // a Stalker's run for spears in a fight outbids the attack (AttackSeeEnemy 60); feeding the attack on a
    // target out of sight (AttackOther 30)
    if (BotMgr_AmmoRunWanted(GetAIPtr()))
        return 65.0f;
    // ... and a facehugger with no victim keeps moving instead of the attack's 5-12 s wait (BotMgr_HugRoam)
    return (m_hFeed.GetHOBJECT() || BotMgr_FeedWanted(GetAIPtr()) || BotMgr_HugRoam(GetAIPtr())) ? 45.0f : 15.0f;
}

void BotRoamGoal::Load(HMESSAGEREAD hRead)
{
    Goal::Load(hRead);
    // multiplayer is never saved; nothing else to restore
}

void BotRoamGoal::Save(HMESSAGEWRITE hWrite)
{
    Goal::Save(hWrite);
}

void BotRoamGoal::Start()
{
    Goal::Start();
    m_Follow.Clear();
    GetAIPtr()->SetMovementStyle();
    GetAIPtr()->Run();
    GetAIPtr()->SetNextAction(CAI::ACTION_IDLE);
    m_fRetryTime = 0.0f;
    // back from a glance (at most 2 s away): go on to the same place, as a person would
    const LTFLOAT now = g_pLTServer->GetTime();
    if (BotMgr_HearSteps() && m_bHasLastDest && now - m_fEndTime < 2.0f && m_Follow.Set(m_vLastDest))
    {
        m_fGiveUpTime += now - m_fEndTime;
        return;
    }
    m_bHasLastDest = false;
    PickDestination();
}

void BotRoamGoal::End()
{
    Goal::End();
    m_fEndTime = g_pLTServer->GetTime();
    m_Follow.Clear();
    BotMgr_AmbushEnd(GetAIPtr(), "goal");  // an enemy seen from the wall (or another goal) ends a wall ambush
    if (m_hFeed.GetHOBJECT())
        BotMgr_FeedEnd(GetAIPtr());   // an enemy (or another goal) interrupted the feeding
    m_hFeed = (HOBJECT)LTNULL;
}

void BotRoamGoal::Update()
{
    Goal::Update();

    const LTFLOAT now = g_pLTServer->GetTime();

    // a chestburster hides until it grows (BotMgr decides where and when to run)
    {
        LTVector hide;
        const int cb = BotMgr_CbHide(GetAIPtr(), m_Follow.IsSet() && !m_Follow.IsDone() && now <= m_fGiveUpTime,
                                     m_Follow.IsDone(), &hide);
        if (cb != BOTCB_NONE)
        {
            GetAIPtr()->Run();
            if (cb == BOTCB_GO)
            {
                if (m_Follow.Set(hide))
                    m_fGiveUpTime = now + 20.0f;
                else
                    BotMgr_CbHideFailed(GetAIPtr());
            }
            else if (cb == BOTCB_STAY)
            {
                if (m_Follow.IsSet())
                    m_Follow.Clear();
                return;
            }
            if (m_Follow.IsSet() && !m_Follow.IsDone())
                m_Follow.Update();
            return;
        }
    }

    if (m_hFeed.GetHOBJECT())
    {
        // an alien feeding on a body: walk there, then stand and strike it (BotMgr_Feed decides)
        const bool walking = m_Follow.IsSet() && !m_Follow.IsDone() && now <= m_fGiveUpTime;
        const int state = BotMgr_Feed(GetAIPtr(), m_hFeed.GetHOBJECT(), !walking);
        if (state == BOTFEED_EAT)
        {
            if (m_Follow.IsSet())
                m_Follow.Clear();
            return;
        }
        if (state == BOTFEED_WALK && walking)
        {
            GetAIPtr()->Run();
            m_Follow.Update();
            return;
        }
        BotMgr_FeedEnd(GetAIPtr());
        m_hFeed = (HOBJECT)LTNULL;
        m_Follow.Clear();
        m_fRetryTime = 0.0f;
    }
    GetAIPtr()->Run();

    // a wall ambush: follow the path onto the wall, then hang there (BotMgr decides when it ends)
    const int amb = BotMgr_AmbushUpdate(GetAIPtr(), m_Follow.IsSet() && !m_Follow.IsDone() && now <= m_fGiveUpTime,
                                        m_Follow.IsDone());
    if (amb == BOTAMB_WAITING)
        return;
    if (amb == BOTAMB_GOING)
    {
        m_Follow.Update();
        return;
    }

    if (!m_Follow.IsSet() || m_Follow.IsDone() || now > m_fGiveUpTime)
    {
        if (now >= m_fRetryTime)
            PickDestination();
        return;
    }
    m_Follow.Update();
}

void BotRoamGoal::PickDestination()
{
    const LTFLOAT now = g_pLTServer->GetTime();
    m_fRetryTime = now + 1.0f;

    BotNav* nav = BotMgr_GetNav();
    if (!nav || !nav->IsLoaded())
        return;

    const LTVector& p = GetAIPtr()->GetPosition();
    const LTVector& dims = GetAIPtr()->GetDims();

    // a chestburster keeps away from the fight while it grows (cbhide)
    LTVector hide;
    if (BotMgr_HideDest(GetAIPtr(), &hide) && m_Follow.Set(hide))
    {
        m_fGiveUpTime = now + 10.0f + (hide - p).Mag() / 150.0f;
        return;
    }

    // a hurt alien with nothing to fight: feed on a body, as alien players heal
    LTVector body;
    HOBJECT hBody = LTNULL;
    if (BotMgr_FeedGoal(GetAIPtr(), &body, &hBody))
    {
        LTVector dest(body.x, body.y + dims.y, body.z);
        if (m_Follow.Set(dest))
        {
            m_hFeed = hBody;
            m_fGiveUpTime = now + 10.0f + (dest - p).Mag() / 150.0f;
            return;
        }
        BotMgr_FeedEnd(GetAIPtr(), true);
    }

    // an alien with nothing to do sometimes waits on a wall above a busy route, as alien players do
    LTVector wall;
    if (BotMgr_AmbushPick(GetAIPtr(), &wall))
    {
        if (m_Follow.Set(wall))
        {
            m_fGiveUpTime = now + 10.0f + (wall - p).Mag() / 150.0f;
            return;
        }
        BotMgr_AmbushEnd(GetAIPtr(), "nopath");
    }

    // hurt and nothing to fight: go for a health or armor pickup, as players do
    LTVector pickup;
    if (BotMgr_PickupGoal(GetAIPtr(), &pickup))
    {
        LTVector dest(pickup.x, pickup.y + dims.y, pickup.z);
        if (m_Follow.Set(dest))
        {
            if (BotMgr_Debug())
            {
                char who[64] = "";
                g_pLTServer->GetObjectName(GetAIPtr()->m_hObject, who, sizeof(who));
                BotMgr_Log("ROAM %s pickup at %.0f %.0f %.0f hp=%.0f", who, pickup.x, pickup.y, pickup.z,
                           GetAIPtr()->GetHitPoints());
            }
            m_fGiveUpTime = now + 10.0f + (dest - p).Mag() / 150.0f;
            return;
        }
    }
    // a run for spears in a fight with no way to a box: no random roaming instead, back to the fight
    if (BotMgr_AmmoRunWanted(GetAIPtr()))
    {
        BotMgr_AmmoRunCancel(GetAIPtr());
        return;
    }

    int from = nav->Nearest(Vec3(p.x, p.y - dims.y, p.z));
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        // Aliens do not climb ladders but climb walls: pick places they can reach that way
        const CharacterButes* cb = GetAIPtr()->GetButes();
        const bool alien = cb && cb->m_eCharacterClass == ALIEN;
        int to = nav->RandomReachable(from, GetRandom(0.0f, 1.0f), alien ? 0 : -1, alien && BotMgr_CanWallWalk(GetAIPtr()));
        if (to < 0)
            return;
        const Vec3& d = nav->Node(to).pos;
        LTVector dest(d.x, d.y + dims.y, d.z);
        LTBOOL ok = m_Follow.Set(dest);
        char who[64] = "";
        g_pLTServer->GetObjectName(GetAIPtr()->m_hObject, who, sizeof(who));
        if (BotMgr_Debug())
            BotMgr_Log("ROAM %s node %d -> %d (%.0f %.0f %.0f) set=%d isSet=%d done=%d", who,
                   from, to, dest.x, dest.y, dest.z, (int)ok, (int)m_Follow.IsSet(), (int)m_Follow.IsDone());
        if (ok)
        {
            m_vLastDest = dest;
            m_bHasLastDest = true;
            // generous time budget: straight distance at a slow walk, at least 10 s
            LTVector delta = dest - p;
            m_fGiveUpTime = now + 10.0f + delta.Mag() / 150.0f;
            return;
        }
    }
}

// --- BotTrackerGoal ---------------------------------------------------------------------------------

BotTrackerGoal::BotTrackerGoal(CAI* pAI, std::string create_name)
    : Goal(pAI, create_name)
{
}

LTFLOAT BotTrackerGoal::GetBid()
{
    return BotMgr_TrackerBid(GetAIPtr(), LTNULL);
}

void BotTrackerGoal::Load(HMESSAGEREAD hRead)
{
    Goal::Load(hRead);
}

void BotTrackerGoal::Save(HMESSAGEWRITE hWrite)
{
    Goal::Save(hWrite);
}

void BotTrackerGoal::Start()
{
    Goal::Start();
    GetAIPtr()->SetMovementStyle();
    GetAIPtr()->Run();
    GetAIPtr()->SetNextAction(CAI::ACTION_IDLE);   // stop: a player who sees a blip coming stops to face it
    Face();
}

void BotTrackerGoal::End()
{
    Goal::End();
}

void BotTrackerGoal::Update()
{
    Goal::Update();
    Face();
}

void BotTrackerGoal::Face()
{
    LTVector blip;
    if (BotMgr_TrackerBid(GetAIPtr(), &blip) <= 0.0f)
        return;
    const AIActionTurnTo* turning = dynamic_cast<const AIActionTurnTo*>(GetAIPtr()->GetCurrentAction());
    if (turning && !turning->IsDone())
        return;
    LTVector to = blip - GetAIPtr()->GetPosition();
    to.y = 0.0f;
    LTVector fwd = GetAIPtr()->GetForward();
    fwd.y = 0.0f;
    const float d = to.Mag(), f = fwd.Mag();
    if (d < 1.0f || f < 0.01f)
        return;
    // already facing it (within 10 degrees): stand and watch
    if (fwd.Dot(to) / (d * f) > 0.985f)
        return;
    if (AIActionTurnTo* turn = dynamic_cast<AIActionTurnTo*>(GetAIPtr()->SetNextAction(CAI::ACTION_TURNTO)))
        turn->Init(to);
}
