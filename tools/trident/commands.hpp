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
