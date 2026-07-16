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
