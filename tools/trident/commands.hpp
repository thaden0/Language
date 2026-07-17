#pragma once
#include <string>

// -----------------------------------------------------------------------------
// tools/trident/commands.{hpp,cpp} — the P2.1e dependency-management CLI
// (techdesign-package-manager.md §5.5): `add`/`remove`/`update`/`lock`/
// `fetch`/`why`. Split from main.cpp once it would have grown past ~300
// lines (§5.5's own suggestion). Each edits the manifest and/or
// (re)writes trident.lock and populates the store; none of them invoke
// leviathan (only build/run/check/emit-llvm, still in main.cpp, do that).
// -----------------------------------------------------------------------------

// `trident add <path>[@version] [--as <name>] [--dev] [manifest-or-dir]`.
// Adds a new [[dep]] (or updates an existing one with the same `path`) in
// the manifest, then re-resolves and rewrites the lockfile (which also
// fetches every VCS dep into the store, resolveVcsDeps). An omitted
// `@version` resolves to the highest available tag (`GitProvider::versions`).
int cmdAdd(const std::string& manifestArg, const std::string& depSpec,
          const std::string& asName, bool dev);

// `trident remove <path> [manifest-or-dir]`. Removes the [[dep]] with that
// `path` and re-locks. Not finding the path is a loud error, not a silent
// no-op.
int cmdRemove(const std::string& manifestArg, const std::string& path);

// `trident update [<path>] [manifest-or-dir]`. Bumps the named dep (or every
// Vcs-kind dep, if `path` is empty) to its highest available tag and
// re-locks.
int cmdUpdate(const std::string& manifestArg, const std::string& path);

// `trident lock [manifest-or-dir]`. Re-resolves (MVS + fetch) and rewrites
// trident.lock from scratch — the explicit "make the lock match the
// manifest right now" command (§3.4).
int cmdLock(const std::string& manifestArg);

// `trident fetch [manifest-or-dir]`. Same underlying resolution as `lock`
// (every Vcs-kind dep materialized into the store) — the explicit
// "warm the cache" command.
int cmdFetch(const std::string& manifestArg);

// `trident why <path> [manifest-or-dir]`. Explains the selected version of
// `path` and which module(s) required it (root, and/or another selected
// module's own `requires`).
int cmdWhy(const std::string& manifestArg, const std::string& path);

// `trident audit [manifest-or-dir]` (techdesign-package-manager.md §6 P2.2,
// GT4). Requires an existing trident.lock (loud error naming `trident lock`
// otherwise — nothing to audit against). Re-resolves against that lock,
// which re-verifies every module's freshly fetched content against both the
// checksum DB and the lock's own pinned hash (resolveVcsDeps, resolve.cpp) —
// a mismatch is reported as a failed audit (exit 1) naming the module and
// both hashes; a clean run prints one "OK" line per module and exits 0.
int cmdAudit(const std::string& manifestArg);

// `trident vendor [manifest-or-dir]` (techdesign-package-manager.md §6 P2.2,
// GT4). Requires an existing, consistent trident.lock. Resolves it (a normal,
// possibly-networked fetch into $TRIDENT_HOME/store, verified exactly like
// any other resolution) and copies each selected module's store directory
// into `<manifest-dir>/vendor/<path>[@vN]/` — the layout `--vendor`
// (VendorProvider, resolve.cpp) reads back with zero network access.
int cmdVendor(const std::string& manifestArg);
