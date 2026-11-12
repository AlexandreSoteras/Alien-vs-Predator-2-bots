// BotNav: waypoint graph loading and A* path finding for AvP2 server-side bots.
//
// The graph comes from navgen/navbuild.py (".nav" text files, format "AVP2NAV 1"):
//   n <id> <x> <y> <z>                       waypoint (floor position, Y up)
//   e <a> <b> <px> <py> <pz> <kind>          walk/ladder: a<->b; drop/jump: a->b; through portal point p
//   e <a> <b> <px> <py> <pz> ladder <fx> <fy> <fz> <tx> <ty> <tz>
//                                            ladder from a (foot) up to b (top): climb at p's x/z, stand at f
//                                            before climbing, step off at t (both floor positions)
//   e <a> <b> <wx> <wy> <wz> climb <fx> <fy> <fz> <tx> <ty> <tz> <nx> <nz>
//                                            Aliens only, a->b: from floor point f, climb the wall to w (on the
//                                            wall face, normal n), get onto the ledge at t
//   e <a> <b> <wx> <wy> <wz> climbdown <fx> <fy> <fz> <tx> <ty> <tz> <nx> <nz>
//                                            the reverse: from ledge point f onto the wall at w, down to floor t
//   badspawn <x> <y> <z> walkers|aliens|all  a start point bots should not use (cut off from the main area)
//   usedoor <name>                           a door the graph goes through (bots press Use on it)
//
// This file has no engine dependency so it can be unit-tested on its own.

#ifndef AVP2BOTS_BOTNAV_H
#define AVP2BOTS_BOTNAV_H

#include <vector>
#include <string>

namespace avp2bots {

struct Vec3
{
    float x, y, z;
    Vec3() : x(0), y(0), z(0) {}
    Vec3(float ax, float ay, float az) : x(ax), y(ay), z(az) {}
};

float Dist(const Vec3& a, const Vec3& b);
float DistXZ(const Vec3& a, const Vec3& b);

enum NavLinkKind { NAV_WALK = 0, NAV_DROP = 1, NAV_LADDER = 2, NAV_JUMP = 3, NAV_CLIMB = 4, NAV_CLIMBDOWN = 5 };

struct NavLink
{
    int to;
    Vec3 portal;     // point on the border between the two waypoint areas
    NavLinkKind kind;
    float cost;
    bool blocked;    // crosses a solid obstacle found at runtime (BlockBox)
    bool hasEnds;    // ladders and climbs: 'enter' and 'exit' are set
    Vec3 enter;      // floor point to stand on before climbing
    Vec3 exit;       // floor point to step off to at the other end
    Vec3 normal;     // climbs: the wall's normal (horizontal)
};

struct NavNode
{
    Vec3 pos;
    std::vector<NavLink> links;
};

// Route preference for FindPath (an alien keeping out of the open floor a watcher faces): walk links that end on
// the watcher's floor level inside its facing cone cost walkMul times more; wall climbs cost climbMul times.
struct PathBias
{
    Vec3 eye;        // the watcher's feet
    float fx, fz;    // its horizontal facing (unit vector)
    float cosHalf;   // cosine of the cone's half angle
    float range;     // the cone's length
    float floorDy;   // a waypoint is on the watcher's floor level within this height
    float walkMul;
    float climbMul;
    bool InCone(const Vec3& p) const;
};

class BotNav
{
public:
    BotNav();

    // Loads a .nav file. Returns false (and fills error) on failure; the graph is left empty.
    bool Load(const char* path, std::string* error);
    void Clear();

    bool IsLoaded() const { return !m_nodes.empty(); }
    const std::string& MapName() const { return m_map; }
    int NodeCount() const { return (int)m_nodes.size(); }
    int LinkCount() const;
    const NavNode& Node(int i) const { return m_nodes[i]; }

    // Closest waypoint to a world position, preferring ones at a similar height.
    // Returns -1 when the graph is empty.
    int Nearest(const Vec3& pos) const;

    // A* from node a to node b. On success fills 'points' with the positions to steer through
    // (portal points and waypoints, excluding the start node, ending at b) and returns true.
    // 'kinds' (optional) receives one entry per link taken; points come in (portal, node) pairs per link.
    // allowLadders: -1 = the UseLadders() setting, 0 = never (e.g. Aliens, who do not climb), 1 = always.
    // allowClimb: use wall climbs (Aliens).
    // 'links' (optional) receives a copy of each link taken (ladder and climb points).
    // bias (optional): see PathBias. LastExpanded() = waypoints the last search closed (cost diagnostics).
    bool FindPath(int a, int b, std::vector<Vec3>* points, std::vector<int>* nodes = 0,
                  std::vector<NavLinkKind>* kinds = 0, int allowLadders = -1,
                  std::vector<NavLink>* links = 0, bool allowClimb = false, const PathBias* bias = 0) const;
    int LastExpanded() const { return m_lastExpanded; }

    // How many waypoints a bot standing on 'from' can reach (links followed one way, as FindPath does),
    // counting up to 'limit'. A small count means an island the bot cannot leave (reached by a one-way drop).
    int ReachableCount(int from, int limit, int allowLadders = -1, bool allowClimb = false) const;

    // A node reachable from 'from' picked with the given random value in [0,1); -1 if none.
    int RandomReachable(int from, float rnd, int allowLadders = -1, bool allowClimb = false) const;

    // Ladder links are skipped by FindPath and RandomReachable unless enabled.
    void UseLadders(bool use);

    // Start points the generator found cut off from the main play area, for walkers (Marines, Predators)
    // or for climbers (Aliens).
    bool IsBadSpawn(const Vec3& pos, bool alien) const;

    // Doors the generator made passable (players open them with Use); bots open only these.
    bool IsUseDoor(const char* name) const;

    // Marks waypoints inside an axis-aligned box, and links whose path crosses it, as unusable
    // (solid model props are not in the map file the graph was built from). Returns the number of
    // waypoints blocked. ClearBlocks() undoes all of them.
    int BlockBox(const Vec3& mn, const Vec3& mx, int* linksBlocked = 0);
    void ClearBlocks();
    bool IsBlocked(int node) const { return node >= 0 && node < (int)m_blocked.size() && m_blocked[node] != 0; }

private:
    bool m_useLadders;
    mutable int m_lastExpanded;
    std::vector<char> m_blocked;
    struct BadSpawn { Vec3 pos; bool walkers, aliens; };
    std::vector<BadSpawn> m_badSpawns;
    std::vector<std::string> m_useDoors;
    std::string m_map;
    std::vector<NavNode> m_nodes;
    // reachability components (walk links only, both directions), used to avoid hopeless searches
    std::vector<int> m_component;         // walk links only
    std::vector<int> m_componentLadders;  // walk + ladder links
    std::vector<int> m_componentClimb;    // walk + climb links (counted both ways: Aliens drop back down)
    void BuildComponents();
    void BuildComponentSet(bool ladders, bool climbs, std::vector<int>& out) const;
};

} // namespace avp2bots

#endif
