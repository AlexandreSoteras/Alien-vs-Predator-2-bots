#include "BotNav.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <queue>
#include <functional>
#include <algorithm>

namespace avp2bots {

float Dist(const Vec3& a, const Vec3& b)
{
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float DistXZ(const Vec3& a, const Vec3& b)
{
    float dx = a.x - b.x, dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

BotNav::BotNav() : m_useLadders(false), m_lastExpanded(0) {}

bool PathBias::InCone(const Vec3& p) const
{
    const float dx = p.x - eye.x, dz = p.z - eye.z;
    const float d = sqrtf(dx * dx + dz * dz);
    if (d < 1.0f || d > range || fabsf(p.y - eye.y) > floorDy)
        return false;
    return (dx * fx + dz * fz) >= cosHalf * d;
}

void BotNav::Clear()
{
    m_map.clear();
    m_nodes.clear();
    m_blocked.clear();
    m_badSpawns.clear();
    m_useDoors.clear();
    m_component.clear();
    m_componentLadders.clear();
    m_componentClimb.clear();
}

int BotNav::LinkCount() const
{
    int n = 0;
    for (size_t i = 0; i < m_nodes.size(); ++i)
        n += (int)m_nodes[i].links.size();
    return n;
}

static void AddLink(NavNode& from, int to, const Vec3& portal, NavLinkKind kind, const Vec3& toPos)
{
    NavLink l;
    l.to = to;
    l.portal = portal;
    l.kind = kind;
    l.blocked = false;
    l.hasEnds = false;
    // cost = path length through the portal; drops are cheap to take but slightly penalised
    // so bots prefer walking routes of similar length
    l.cost = Dist(from.pos, portal) + Dist(portal, toPos);
    if (kind == NAV_DROP)
        l.cost *= 1.25f;
    else if (kind == NAV_JUMP)
        l.cost *= 1.5f;  // jumps can fail: prefer walking routes of similar length
    else if (kind == NAV_CLIMB || kind == NAV_CLIMBDOWN)
        l.cost *= 2.0f;  // climbing is slower than running (and than dropping)
    from.links.push_back(l);
}

bool BotNav::Load(const char* path, std::string* error)
{
    Clear();
    FILE* f = std::fopen(path, "r");
    if (!f)
    {
        if (error) *error = std::string("cannot open ") + path;
        return false;
    }

    char line[512];
    bool header = false;
    int lineNo = 0;
    while (std::fgets(line, sizeof(line), f))
    {
        ++lineNo;
        if (!header)
        {
            if (std::strncmp(line, "AVP2NAV 1", 9) != 0)
            {
                if (error) *error = "not an AVP2NAV 1 file";
                std::fclose(f);
                Clear();
                return false;
            }
            header = true;
            continue;
        }
        if (line[0] == 'n' && line[1] == ' ')
        {
            int id;
            float x, y, z;
            if (std::sscanf(line + 2, "%d %f %f %f", &id, &x, &y, &z) != 4 || id != (int)m_nodes.size())
            {
                if (error) { char b[64]; std::sprintf(b, "bad node line %d", lineNo); *error = b; }
                std::fclose(f);
                Clear();
                return false;
            }
            NavNode n;
            n.pos = Vec3(x, y, z);
            m_nodes.push_back(n);
        }
        else if (line[0] == 'e' && line[1] == ' ')
        {
            int a, b;
            float px, py, pz;
            char kind[16] = {0};
            float fx, fy, fz, tx, ty, tz, nx = 0, nz = 0;
            const int fields = std::sscanf(line + 2, "%d %d %f %f %f %15s %f %f %f %f %f %f %f %f", &a, &b, &px, &py,
                                           &pz, kind, &fx, &fy, &fz, &tx, &ty, &tz, &nx, &nz);
            if (fields < 6 ||
                a < 0 || b < 0 || a >= (int)m_nodes.size() || b >= (int)m_nodes.size())
            {
                if (error) { char buf[64]; std::sprintf(buf, "bad edge line %d", lineNo); *error = buf; }
                std::fclose(f);
                Clear();
                return false;
            }
            Vec3 portal(px, py, pz);
            if (std::strcmp(kind, "drop") == 0)
            {
                AddLink(m_nodes[a], b, portal, NAV_DROP, m_nodes[b].pos);
            }
            else if (std::strcmp(kind, "jump") == 0)
            {
                AddLink(m_nodes[a], b, portal, NAV_JUMP, m_nodes[b].pos);
            }
            else if (std::strcmp(kind, "climb") == 0 || std::strcmp(kind, "climbdown") == 0)
            {
                if (fields != 14)
                    continue;  // malformed: skip the link, keep the graph
                AddLink(m_nodes[a], b, portal, kind[5] ? NAV_CLIMBDOWN : NAV_CLIMB, m_nodes[b].pos);
                NavLink& up = m_nodes[a].links.back();
                up.hasEnds = true;
                up.enter = Vec3(fx, fy, fz);
                up.exit = Vec3(tx, ty, tz);
                up.normal = Vec3(nx, 0.0f, nz);
            }
            else if (std::strcmp(kind, "ladder") == 0)
            {
                AddLink(m_nodes[a], b, portal, NAV_LADDER, m_nodes[b].pos);
                AddLink(m_nodes[b], a, portal, NAV_LADDER, m_nodes[a].pos);
                if (fields == 12)
                {
                    NavLink& up = m_nodes[a].links.back();
                    NavLink& down = m_nodes[b].links.back();
                    up.hasEnds = down.hasEnds = true;
                    up.enter = down.exit = Vec3(fx, fy, fz);
                    up.exit = down.enter = Vec3(tx, ty, tz);
                }
            }
            else
            {
                AddLink(m_nodes[a], b, portal, NAV_WALK, m_nodes[b].pos);
                AddLink(m_nodes[b], a, portal, NAV_WALK, m_nodes[a].pos);
            }
        }
        else if (std::strncmp(line, "usedoor ", 8) == 0)
        {
            char name[128] = {0};
            if (std::sscanf(line + 8, "%127s", name) == 1)
                m_useDoors.push_back(name);
        }
        else if (std::strncmp(line, "badspawn ", 9) == 0)
        {
            float x, y, z;
            char who[16] = {0};
            if (std::sscanf(line + 9, "%f %f %f %15s", &x, &y, &z, who) == 4)
            {
                BadSpawn b;
                b.pos = Vec3(x, y, z);
                b.walkers = std::strcmp(who, "aliens") != 0;
                b.aliens = std::strcmp(who, "walkers") != 0;
                m_badSpawns.push_back(b);
            }
        }
        else if (std::strncmp(line, "map ", 4) == 0)
        {
            char name[128] = {0};
            if (std::sscanf(line + 4, "%127s", name) == 1)
                m_map = name;
        }
        // other lines (params, counts) are informative only
    }
    std::fclose(f);

    if (m_nodes.empty())
    {
        if (error) *error = "no nodes";
        return false;
    }
    m_blocked.assign(m_nodes.size(), 0);
    BuildComponents();
    return true;
}

bool BotNav::IsUseDoor(const char* name) const
{
    for (size_t i = 0; i < m_useDoors.size(); ++i)
    {
        const char* a = m_useDoors[i].c_str();
        const char* b = name;
        while (*a && *b && (*a | 32) == (*b | 32))
            ++a, ++b;
        if (!*a && !*b)
            return true;
    }
    return false;
}

bool BotNav::IsBadSpawn(const Vec3& pos, bool alien) const
{
    for (size_t i = 0; i < m_badSpawns.size(); ++i)
        if ((alien ? m_badSpawns[i].aliens : m_badSpawns[i].walkers) && Dist(pos, m_badSpawns[i].pos) < 32.0f)
            return true;
    return false;
}

void BotNav::UseLadders(bool use)
{
    m_useLadders = use;
}

void BotNav::BuildComponents()
{
    BuildComponentSet(false, false, m_component);
    BuildComponentSet(true, false, m_componentLadders);
    BuildComponentSet(false, true, m_componentClimb);
}

void BotNav::BuildComponentSet(bool ladders, bool climbs, std::vector<int>& out) const
{
    out.assign(m_nodes.size(), -1);
    // climbs go up only; Aliens come back down by dropping, so count them both ways for grouping
    std::vector<std::vector<int> > back;
    if (climbs)
    {
        back.resize(m_nodes.size());
        for (size_t k = 0; k < m_nodes.size(); ++k)
            for (size_t i = 0; i < m_nodes[k].links.size(); ++i)
                if (m_nodes[k].links[i].kind == NAV_CLIMB || m_nodes[k].links[i].kind == NAV_CLIMBDOWN)
                    back[m_nodes[k].links[i].to].push_back((int)k);
    }
    int comp = 0;
    std::vector<int> stack;
    for (size_t s = 0; s < m_nodes.size(); ++s)
    {
        if (out[s] >= 0)
            continue;
        stack.push_back((int)s);
        out[s] = comp;
        while (!stack.empty())
        {
            int k = stack.back();
            stack.pop_back();
            const std::vector<NavLink>& ls = m_nodes[k].links;
            for (size_t i = 0; i < ls.size(); ++i)
            {
                bool twoWay = ls[i].kind == NAV_WALK || (ladders && ls[i].kind == NAV_LADDER) ||
                              (climbs && (ls[i].kind == NAV_CLIMB || ls[i].kind == NAV_CLIMBDOWN));
                if (twoWay && out[ls[i].to] < 0)
                {
                    out[ls[i].to] = comp;
                    stack.push_back(ls[i].to);
                }
            }
            if (climbs)
                for (size_t i = 0; i < back[k].size(); ++i)
                    if (out[back[k][i]] < 0)
                    {
                        out[back[k][i]] = comp;
                        stack.push_back(back[k][i]);
                    }
        }
        ++comp;
    }
}

int BotNav::Nearest(const Vec3& pos) const
{
    int best = -1;
    float bestScore = 0;
    for (size_t i = 0; i < m_nodes.size(); ++i)
    {
        if (IsBlocked((int)i))
            continue;
        const Vec3& p = m_nodes[i].pos;
        // vertical distance weighs more: a node on the floor below a catwalk is not "near". Below the feet,
        // up to 48 units is a ramp or a step; past that, a lower floor. (The old 150-unit allowance put a bot on
        // a landing of Suk-Stronghold on the floor 128 units below, 3 units away horizontally, instead of the
        // landing's own node 64 units away: its paths started downstairs and it pushed against the railing.)
        float dy = pos.y - p.y;
        float score = DistXZ(pos, p) + (dy < 0 ? -dy * 3.0f : (dy > 48.0f ? (dy - 48.0f) * 1.5f : 0.0f));
        if (best < 0 || score < bestScore)
        {
            best = (int)i;
            bestScore = score;
        }
    }
    return best;
}

bool BotNav::FindPath(int a, int b, std::vector<Vec3>* points, std::vector<int>* nodes,
                      std::vector<NavLinkKind>* kinds, int allowLadders, std::vector<NavLink>* links,
                      bool allowClimb, const PathBias* bias) const
{
    m_lastExpanded = 0;
    const bool ladders = allowLadders < 0 ? m_useLadders : allowLadders != 0;
    if (points) points->clear();
    if (nodes) nodes->clear();
    if (kinds) kinds->clear();
    if (links) links->clear();
    int n = (int)m_nodes.size();
    if (a < 0 || b < 0 || a >= n || b >= n)
        return false;
    if (a == b)
    {
        return true;
    }

    std::vector<float> g(n, -1.0f);
    std::vector<int> parent(n, -1);
    std::vector<int> parentLink(n, -1);
    std::vector<char> closed(n, 0);
    typedef std::pair<float, int> QItem;  // (f = g + h, node)
    std::priority_queue<QItem, std::vector<QItem>, std::greater<QItem> > open;

    const Vec3& goal = m_nodes[b].pos;
    g[a] = 0.0f;
    open.push(QItem(Dist(m_nodes[a].pos, goal), a));
    while (!open.empty())
    {
        int k = open.top().second;
        open.pop();
        if (closed[k])
            continue;
        closed[k] = 1;
        ++m_lastExpanded;
        if (k == b)
            break;
        const std::vector<NavLink>& ls = m_nodes[k].links;
        for (size_t i = 0; i < ls.size(); ++i)
        {
            int t = ls[i].to;
            if (closed[t] || ls[i].blocked || (t != b && IsBlocked(t)) || (ls[i].kind == NAV_LADDER && !ladders) ||
                ((ls[i].kind == NAV_CLIMB || ls[i].kind == NAV_CLIMBDOWN) && !allowClimb))
                continue;
            float c = ls[i].cost;
            if (bias)
            {
                if (ls[i].kind == NAV_CLIMB || ls[i].kind == NAV_CLIMBDOWN)
                    c *= bias->climbMul;
                else if (ls[i].kind == NAV_WALK && bias->InCone(m_nodes[t].pos))
                    c *= bias->walkMul;
            }
            float ng = g[k] + c;
            if (g[t] < 0.0f || ng < g[t])
            {
                g[t] = ng;
                parent[t] = k;
                parentLink[t] = (int)i;
                open.push(QItem(ng + Dist(m_nodes[t].pos, goal), t));
            }
        }
    }
    if (!closed[b])
        return false;

    std::vector<int> rev;
    for (int k = b; k != a; k = parent[k])
        rev.push_back(k);
    for (int i = (int)rev.size() - 1; i >= 0; --i)
    {
        int k = rev[i];
        const NavLink& l = m_nodes[parent[k]].links[parentLink[k]];
        if (points)
        {
            points->push_back(l.portal);
            points->push_back(m_nodes[k].pos);
        }
        if (nodes)
            nodes->push_back(k);
        if (kinds)
            kinds->push_back(l.kind);
        if (links)
            links->push_back(l);
    }
    return true;
}

int BotNav::ReachableCount(int from, int limit, int allowLadders, bool allowClimb) const
{
    const int n = (int)m_nodes.size();
    if (from < 0 || from >= n)
        return 0;
    const bool ladders = allowLadders < 0 ? m_useLadders : allowLadders != 0;
    std::vector<char> seen(n, 0);
    std::vector<int> queue(1, from);
    seen[from] = 1;
    for (size_t q = 0; q < queue.size() && (int)queue.size() < limit; ++q)
    {
        const std::vector<NavLink>& ls = m_nodes[queue[q]].links;
        for (size_t i = 0; i < ls.size() && (int)queue.size() < limit; ++i)
        {
            const int t = ls[i].to;
            if (seen[t] || ls[i].blocked || IsBlocked(t) || (ls[i].kind == NAV_LADDER && !ladders) ||
                ((ls[i].kind == NAV_CLIMB || ls[i].kind == NAV_CLIMBDOWN) && !allowClimb))
                continue;
            seen[t] = 1;
            queue.push_back(t);
        }
    }
    return (int)queue.size();
}

int BotNav::RandomReachable(int from, float rnd, int allowLadders, bool allowClimb) const
{
    if (from < 0 || from >= (int)m_nodes.size())
        return -1;
    const bool ladders = allowLadders < 0 ? m_useLadders : allowLadders != 0;
    const std::vector<int>& comps = allowClimb ? m_componentClimb : ladders ? m_componentLadders : m_component;
    int comp = comps[from];
    int count = 0;
    for (size_t i = 0; i < comps.size(); ++i)
        if (comps[i] == comp && (int)i != from && !IsBlocked((int)i))
            ++count;
    if (count == 0)
        return -1;
    int pick = (int)(rnd * count);
    if (pick >= count) pick = count - 1;
    for (size_t i = 0; i < comps.size(); ++i)
    {
        if (comps[i] == comp && (int)i != from && !IsBlocked((int)i))
        {
            if (pick == 0)
                return (int)i;
            --pick;
        }
    }
    return -1;
}

} // namespace avp2bots

namespace avp2bots {

static bool InBox(const Vec3& p, const Vec3& mn, const Vec3& mx)
{
    return p.x >= mn.x && p.x <= mx.x && p.z >= mn.z && p.z <= mx.z && p.y >= mn.y && p.y <= mx.y;
}

int BotNav::BlockBox(const Vec3& mn, const Vec3& mx, int* linksBlocked)
{
    if (m_blocked.size() != m_nodes.size())
        m_blocked.assign(m_nodes.size(), 0);
    int count = 0;
    for (size_t i = 0; i < m_nodes.size(); ++i)
    {
        if (!m_blocked[i] && InBox(m_nodes[i].pos, mn, mx))
        {
            m_blocked[i] = 1;
            ++count;
        }
        std::vector<NavLink>& ls = m_nodes[i].links;
        for (size_t k = 0; k < ls.size(); ++k)
        {
            if (ls[k].blocked)
                continue;
            // sample the bent segment node -> portal -> target every 8 units at most (a long segment
            // sampled 9 times could step over a small box)
            const Vec3& a = m_nodes[i].pos;
            const Vec3& p = ls[k].portal;
            const Vec3& b = m_nodes[ls[k].to].pos;
            const float len = Dist(a, p) > Dist(p, b) ? Dist(a, p) : Dist(p, b);
            const int steps = len > 64.0f ? (int)(len / 8.0f) + 1 : 8;
            for (int s = 0; s <= steps && !ls[k].blocked; ++s)
            {
                float t = s / (float)steps;
                Vec3 q1(a.x + (p.x - a.x) * t, a.y + (p.y - a.y) * t, a.z + (p.z - a.z) * t);
                Vec3 q2(p.x + (b.x - p.x) * t, p.y + (b.y - p.y) * t, p.z + (b.z - p.z) * t);
                if (InBox(q1, mn, mx) || InBox(q2, mn, mx))
                {
                    ls[k].blocked = true;
                    if (linksBlocked)
                        ++*linksBlocked;
                }
            }
        }
    }
    return count;
}

void BotNav::ClearBlocks()
{
    m_blocked.assign(m_nodes.size(), 0);
    for (size_t i = 0; i < m_nodes.size(); ++i)
        for (size_t k = 0; k < m_nodes[i].links.size(); ++k)
            m_nodes[i].links[k].blocked = false;
}

} // namespace avp2bots
