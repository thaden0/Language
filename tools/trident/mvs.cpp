#include "mvs.hpp"
#include "semver.hpp"
#include <map>

bool dependencyToRequire(const Dependency& d, Require& out, std::string& err) {
    Version v;
    if (!parseSemVer(d.version, v, err)) {
        err = "dependency '" + d.path + "': " + err;
        return false;
    }
    // Identity major (techdesign-package-manager.md §0.5): "major <= 1 =>
    // key path" — majors 0 and 1 share ONE module identity (a 0.x package
    // that bumps to 1.x is not a breaking-identity change; only 2+ is).
    // `out.min` still carries the true, un-normalized version for accurate
    // MVS comparison/display — only ModuleId's bucketing major is clamped.
    int identityMajor = v.major <= 1 ? 1 : v.major;
    out.mod = ModuleId{d.path, identityMajor};
    out.min = v;
    out.as_ = d.as_;
    out.dev = d.dev;
    return true;
}

namespace {

// Expand one fetched module's own manifest into further Require edges — see
// mvs.hpp's comment on selectVersions for why every dep here is treated as
// Vcs unconditionally.
bool expandRequires(const ProjectManifest& pm, std::vector<Require>& out, std::string& err) {
    for (const Dependency& d : pm.deps) {
        Require r;
        if (!dependencyToRequire(d, r, err)) return false;
        out.push_back(r);
    }
    return true;
}

std::string renderSelected(const BuildListEntry& entry) {
    return entry.mod.path + "@" + formatSemVer(entry.selected);
}

}  // namespace

std::string findRequireCycle(const std::vector<BuildListEntry>& entries) {
    std::map<ModuleId, size_t> indexByModule;
    for (size_t i = 0; i < entries.size(); ++i) indexByModule.emplace(entries[i].mod, i);

    // Iterative three-color DFS. `pathPosition` lets a gray-target edge
    // render precisely the active suffix that closes, including the
    // repeated head, without recursion or a second graph walk.
    enum Color : unsigned char { White, Gray, Black };
    struct Frame {
        size_t entryIndex;
        size_t nextEdge;
    };

    std::vector<Color> color(entries.size(), White);
    std::vector<size_t> path;
    std::vector<size_t> pathPosition(entries.size(), entries.size());
    std::vector<Frame> stack;

    for (size_t start = 0; start < entries.size(); ++start) {
        if (color[start] != White) continue;

        color[start] = Gray;
        pathPosition[start] = path.size();
        path.push_back(start);
        stack.push_back(Frame{start, 0});

        while (!stack.empty()) {
            Frame& frame = stack.back();
            const std::vector<Require>& edges = entries[frame.entryIndex].requires_;
            if (frame.nextEdge == edges.size()) {
                color[frame.entryIndex] = Black;
                pathPosition[frame.entryIndex] = entries.size();
                path.pop_back();
                stack.pop_back();
                continue;
            }

            const Require& edge = edges[frame.nextEdge++];
            auto targetIt = indexByModule.find(edge.mod);
            if (targetIt == indexByModule.end()) continue;  // dangling lock edge: a leaf

            size_t target = targetIt->second;
            if (color[target] == White) {
                color[target] = Gray;
                pathPosition[target] = path.size();
                path.push_back(target);
                stack.push_back(Frame{target, 0});
                continue;
            }
            if (color[target] != Gray) continue;

            std::string cycle;
            for (size_t i = pathPosition[target]; i < path.size(); ++i) {
                if (!cycle.empty()) cycle += " -> ";
                cycle += renderSelected(entries[path[i]]);
            }
            cycle += " -> " + renderSelected(entries[target]);
            return cycle;
        }
    }
    return "";
}

MvsResult selectVersions(const std::vector<Require>& rootRequires, ModuleProvider& provider) {
    MvsResult result;
    std::map<ModuleId, BuildListEntry> entries;
    std::vector<Require> queue(rootRequires.begin(), rootRequires.end());

    // Defensive cap: MVS converges because a module's selected version only
    // ever increases and each increase triggers at most one refetch — a
    // provider that kept returning ever-higher versions would otherwise spin
    // forever. No real, finite manifest graph should ever approach this.
    const size_t kMaxSteps = 100000;
    size_t steps = 0;

    for (size_t qi = 0; qi < queue.size(); ++qi) {
        if (++steps > kMaxSteps) {
            result.err = "MVS did not converge (exceeded " + std::to_string(kMaxSteps) +
                         " steps) — check for a provider returning ever-increasing versions";
            return result;
        }
        const Require r = queue[qi];

        auto it = entries.find(r.mod);
        bool needFetch = (it == entries.end());
        if (needFetch) {
            BuildListEntry e;
            e.mod = r.mod;
            e.selected = r.min;
            entries.emplace(r.mod, std::move(e));
        } else if (compareSemVer(r.min, it->second.selected) > 0) {
            it->second.selected = r.min;
            needFetch = true;
        }

        if (needFetch) {
            BuildListEntry& e = entries.at(r.mod);
            ProjectManifest pm;
            std::string err;
            if (!provider.manifestOf(e.mod, e.selected, pm, err)) {
                result.err = "cannot resolve " + e.mod.path + "@" + formatSemVer(e.selected) +
                             ": " + err;
                return result;
            }
            std::vector<Require> subs;
            if (!expandRequires(pm, subs, err)) {
                result.err = "dependency '" + e.mod.path + "@" + formatSemVer(e.selected) +
                             "': " + err;
                return result;
            }
            e.requires_ = subs;
            for (const Require& sub : subs) queue.push_back(sub);
        }
    }

    result.buildList.reserve(entries.size());
    for (auto& [id, entry] : entries) result.buildList.push_back(std::move(entry));

    // Policy validation happens only after MVS has fully converged. It reads
    // the final graph but cannot perturb selection or short-circuit its walk.
    std::string cycle = findRequireCycle(result.buildList);
    if (!cycle.empty()) {
        result.err = "require cycle detected: " + cycle +
                     " — the external dependency graph must be acyclic; break the cycle by "
                     "removing one of these modules' requires on the other";
        return result;
    }

    result.ok = true;
    return result;
}
