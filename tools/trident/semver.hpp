#pragma once
#include "provider.hpp"
#include <string>

// -----------------------------------------------------------------------------
// tools/trident/semver.{hpp,cpp} — SemVer parse/compare (techdesign-package-
// manager.md §3.2/§5.1 P2.1a). SemVer *semantics* only (major = breaking) —
// no ranges, no caret/tilde constraints. Selection is entirely MVS's job
// (mvs.{hpp,cpp}); this file only parses a version string into the `Version`
// value (provider.hpp) and compares two of them.
// -----------------------------------------------------------------------------

// Parse a version string into `out`. Accepts an optional leading 'v'/'V' (a
// git tag is "vMAJOR.MINOR.PATCH", techdesign-package-manager.md §3.4/§3.5;
// a manifest's `version` field is conventionally written without it, e.g.
// `version = "1.2.0"` in the existing corpus fixtures) — both spellings parse
// to the same Version. Returns false and sets `err` on any malformed input.
bool parseSemVer(const std::string& text, Version& out, std::string& err);

// Render `v` as a bare "MAJOR.MINOR.PATCH" string — the manifest/lockfile
// spelling (techdesign-package-manager.md §3.4's `selected = "1.2.0"`).
std::string formatSemVer(const Version& v);

// Render `v` as a git tag "vMAJOR.MINOR.PATCH" — what vcs.cpp (P2.1c) asks
// `git` to check out.
std::string formatSemVerTag(const Version& v);

// Three-way compare: <0 if a<b, 0 if equal, >0 if a>b. Ordinary lexicographic
// (major, minor, patch) order.
int compareSemVer(const Version& a, const Version& b);
