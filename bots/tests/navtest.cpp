// Standalone check of BotNav against a generated .nav file.
// Usage: navtest <file.nav> [x y z x y z ...]   (positions, typically start points; paths are tested between all pairs)
// Exit code 0 = every pair has a continuous path, 1 = failure.

#include "../BotNav.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace avp2bots;

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: navtest <file.nav> [x y z ...]\n");
        return 2;
    }
    BotNav nav;
    std::string err;
    if (!nav.Load(argv[1], &err))
    {
        std::printf("load failed: %s\n", err.c_str());
        return 1;
    }
    nav.UseLadders(true);  // as the server does by default
    std::printf("map %s: %d nodes, %d links\n", nav.MapName().c_str(), nav.NodeCount(), nav.LinkCount());

    std::vector<Vec3> pts;
    for (int i = 2; i + 2 < argc; i += 3)
        pts.push_back(Vec3((float)std::atof(argv[i]), (float)std::atof(argv[i + 1]), (float)std::atof(argv[i + 2])));
    if (pts.empty())
    {
        // default: first, middle and last node
        pts.push_back(nav.Node(0).pos);
        pts.push_back(nav.Node(nav.NodeCount() / 2).pos);
        pts.push_back(nav.Node(nav.NodeCount() - 1).pos);
    }

    int failures = 0, pairs = 0;
    for (size_t i = 0; i < pts.size(); ++i)
    {
        for (size_t j = 0; j < pts.size(); ++j)
        {
            if (i == j) continue;
            int a = nav.Nearest(pts[i]);
            int b = nav.Nearest(pts[j]);
            std::vector<Vec3> path;
            ++pairs;
            if (!nav.FindPath(a, b, &path))
            {
                std::printf("NO PATH %u -> %u (nodes %d -> %d)\n", (unsigned)i, (unsigned)j, a, b);
                ++failures;
                continue;
            }
            // continuity: consecutive steering points must be reasonably close (no teleports)
            Vec3 prev = nav.Node(a).pos;
            float len = 0, worst = 0;
            for (size_t k = 0; k < path.size(); ++k)
            {
                float d = Dist(prev, path[k]);
                len += d;
                if (d > worst) worst = d;
                prev = path[k];
            }
            std::printf("path %u -> %u: %u points, length %.0f, longest step %.0f\n",
                        (unsigned)i, (unsigned)j, (unsigned)path.size(), len, worst);
            if (worst > 400.0f)
            {
                std::printf("  suspicious step > 400 units\n");
                ++failures;
            }
        }
    }
    int r = nav.RandomReachable(0, 0.5f);
    std::printf("random reachable from 0: %d\n", r);
    std::printf("%s (%d/%d pairs ok)\n", failures ? "FAIL" : "PASS", pairs - failures, pairs);
    return failures ? 1 : 0;
}
