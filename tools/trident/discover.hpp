#pragma once
#include <string>

// tools/trident/discover.{hpp,cpp} — two independent lookups:
//
// 1. A project's manifest file inside a directory (H-4): the fixed filename
//    `trident.toml` (Cargo-style — not name-parameterized). Returns "" if not
//    found.
//
// 2. The `leviathan` binary (§3.4): `--leviathan` flag -> `$LEVIATHAN` env ->
//    sibling of the running `trident` executable (via /proc/self/exe) ->
//    PATH. Mirrors how cargo finds rustc. Returns "" if all four probes miss;
//    `tried` collects what was probed, for the diagnostic.

std::string discoverManifestPath(const std::string& dir);

std::string findLeviathan(const std::string& flagOverride, std::string* tried);

// Resolve a user-given positional arg (or "" for cwd) to a manifest file: a
// path naming a file directly is used as-is; a directory (or the default,
// cwd) is searched via discoverManifestPath(). Shared by main.cpp's
// build/run/check/emit-llvm/plan dispatch and commands.cpp's
// add/remove/update/lock/fetch/why (P2.1e) — both need the identical
// "what manifest does this argument mean" resolution.
std::string resolveManifestArg(const std::string& arg);
