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

}  // namespace

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
    result.ok = true;
    return result;
}
