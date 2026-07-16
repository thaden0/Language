#pragma once
#include "Ir.hpp"
#include <set>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
//  Ownership / escape analysis (§15 validation).
//
//  For every allocation site in the IR (NewObject, NewArray, NewArraySized,
//  NewMap, MakeRange, MakeClosure), decide statically whether the allocation
//  can outlive its frame:
//
//    ScopeOwned — provably dies with the frame; the compiler could emit the
//                 free at scope exit (§15's compile-time cleanup, no runtime
//                 bookkeeping).
//    Escapes    — returned / thrown / stored in an object or collection /
//                 captured / passed somewhere unknown: needs the refcount tier.
//
//  Method: flow-insensitive alias sets per register, interprocedural
//  parameter summaries computed to fixpoint over static calls (param escapes?
//  param aliases the return value?), hand annotations for the native cores,
//  conservative escape for unknown targets. Soundness of ScopeOwned is what
//  the IR interpreter's verify mode checks empirically (weak-ptr liveness at
//  frame exit).
// ---------------------------------------------------------------------------

struct AllocSite {
    int fn = 0;
    int pc = 0;
    Op op;
    bool escapes = false;
    std::string reason;     // why it escapes ("" if scope-owned)
};

struct OwnershipInfo {
    std::vector<AllocSite> sites;
    // (fn, pc) of scope-owned allocation instructions, for the verifier
    std::set<std::pair<int, int>> scopeOwned;

    int total() const { return (int)sites.size(); }
    int owned() const { return (int)scopeOwned.size(); }
};

// Runs the analysis over a lowered module.
OwnershipInfo analyzeOwnership(const IrModule& mod);

// Human-readable report (per-function sites + module summary).
std::string ownershipReport(const IrModule& mod, const OwnershipInfo& info);
