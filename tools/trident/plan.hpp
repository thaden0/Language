#pragma once
#include "resolve.hpp"
#include <string>

// tools/trident/plan.{hpp,cpp} — the build-plan WRITER (techdesign-toolchain.md
// §3.3). Serializes a ResolvedProject into the frozen text format leviathan's
// src/BuildPlan.cpp reads. Deliberately independent of the compiler's own
// BuildPlan.hpp C++ types — the two applications share only the frozen text
// contract and the levsyntax lexer, never a struct definition; that is the
// whole point of the split this design exists for.

struct WritePlanOptions {
    std::string out;
    std::string mode;      // "build-native" | "build" | "emit-llvm" | "run" | "check"
    std::string target;    // "" = host triple
    int optLevel = 2;
};

// Write `rp` (already resolved: sources, moduleDeps, classified entry) to
// `planPath` in the §3.3 grammar. Returns false (and prints to stderr) on any
// I/O error.
bool writeBuildPlan(const std::string& planPath, const ResolvedProject& rp,
                    const WritePlanOptions& opts);
