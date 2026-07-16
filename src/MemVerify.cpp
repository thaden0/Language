#include "MemVerify.hpp"
#include <map>
#include <sstream>

std::string MemVerifier::report() const {
    std::ostringstream out;
    // Per-site census: how many allocated, how many still reachable at exit.
    struct SiteStat { long allocated = 0, aliveAtExit = 0; char kind = '?'; };
    std::map<std::pair<int, int>, SiteStat> sites;
    long aliveAtExit = 0;
    for (const auto& [k, t] : objs) {
        auto& s = sites[{t.fn, t.pc}];
        ++s.allocated; s.kind = t.kind;
        bool alive = !t.weak.expired();
        if (alive) { ++s.aliveAtExit; ++aliveAtExit; }
    }

    out << "[mem] " << totalAllocs << " heap allocation(s), peak "
        << peakLive << " live concurrently, " << aliveAtExit
        << " reachable at exit (the root set).\n";

    // The reachability oracle: objects that die DURING the run are exactly what
    // a machine-code ARC must free; the rest are program-lifetime roots.
    long transient = totalAllocs - aliveAtExit;
    out << "[mem] " << transient << " allocation(s) become unreachable mid-run "
        << "(the ARC's free obligation); " << aliveAtExit
        << " persist to exit.\n";

    for (const auto& [site, s] : sites) {
        out << "[mem]   fn" << site.first << ":" << site.second << " (" << s.kind
            << ") x" << s.allocated;
        if (s.aliveAtExit) out << "  [" << s.aliveAtExit << " reachable at exit]";
        out << "\n";
    }

    if (shadow) {
        if (errors.empty())
            out << "[mem] shadow ARC: OK (refcount tracked true reachability at "
                   "every op; no leak, no premature free)\n";
        else {
            out << "[mem] shadow ARC: " << errors.size() << " violation(s):\n";
            for (const std::string& e : errors) out << "[mem]   " << e << "\n";
        }
    }
    return out.str();
}
