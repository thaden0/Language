#pragma once
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// tools/trident/vcs.{hpp,cpp} — invoking the system `git` (techdesign-
// package-manager.md §3.6/§5.3 P2.1c, H-4). trident forks/execs the `git`
// binary already on $PATH — it does not link libgit (mirrors
// `runLeviathan`'s fork/execv pattern, main.cpp, and the "no linked
// dependencies" stance elsewhere in the toolchain). `git` is a runtime
// prerequisite for VCS dependencies only; local-path deps need no git.
// -----------------------------------------------------------------------------

// Locate `git` on $PATH. Returns "" if not found — callers turn that into a
// clear diagnostic, never a crash (H-4).
std::string findGit();

// List a remote's tags (`git ls-remote --tags <remote>`). `remote` may be a
// URL or a local filesystem path, including a local bare repo (the offline
// test fixture, H-7 — no test may reach the real network). Annotated tags'
// dereferenced "^{}" duplicate lines are collapsed to the plain tag name.
bool gitListTags(const std::string& remote, std::vector<std::string>& out, std::string& err);

// Shallow-clone `remote` at `tag` (`git clone --depth 1 --branch <tag>`)
// into a fresh temp directory under `parentDir` (or /tmp if empty). Returns
// the checkout's absolute path in `checkoutDir` — the caller owns cleanup.
bool gitCloneTag(const std::string& remote, const std::string& tag, const std::string& parentDir,
                 std::string& checkoutDir, std::string& err);

// Publishing helpers (P2.3/GT5). All operate through `git -C <repo>` and
// never invoke a shell. `gitTagCommit` reports a missing tag with
// `exists == false`, not as an error, so publish can be safely retried.
bool gitRepoRoot(const std::string& path, std::string& root, std::string& err);
bool gitWorkingTreeClean(const std::string& repo, bool& clean, std::string& details,
                         std::string& err);
bool gitOriginUrl(const std::string& repo, std::string& remote, std::string& err);
bool gitHeadCommit(const std::string& repo, std::string& commit, std::string& err);
bool gitPathTracked(const std::string& repo, const std::string& relativePath,
                    bool& tracked, std::string& err);
bool gitTagCommit(const std::string& repo, const std::string& tag, bool& exists,
                  std::string& commit, std::string& err);
bool gitCreateTag(const std::string& repo, const std::string& tag,
                  const std::string& commit, std::string& err);
bool gitDeleteTag(const std::string& repo, const std::string& tag, std::string& err);

// Convert an origin URL into the manifest/module identity spelling used by
// Trident: https://github.com/u/r.git and git@github.com:u/r.git both become
// github.com/u/r; absolute/file remotes remain filesystem paths (the offline
// fixture case).
std::string modulePathFromRemote(const std::string& remote);
