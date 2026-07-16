#pragma once
#include "RuntimeValue.hpp"
#include <unordered_map>
#include <vector>
#include <string>
#include <memory>

// ---------------------------------------------------------------------------
//  Memory-safety verifier (§15).
//
//  It rides inside the IR interpreter, whose reference-counted `shared_ptr`
//  registers are EXACT ground truth for object lifetimes. Two jobs:
//
//   Phase A (oracle): track every heap allocation by (site, instance) and,
//     via `weak_ptr`, record the precise op at which it becomes unreachable —
//     "what must be freed, and when" — plus the live-set profile.
//
//   Phase B (shadow ARC): maintain a shadow reference count using the SAME
//     retain/release discipline the machine-code backend will emit, and assert
//     shadowRc == true reachability at every op boundary. A mismatch pinpoints
//     a missed retain (future use-after-free) or missed release (future leak)
//     in readable C++, before any machine code is written.
//
//  Object identity is the underlying pointer (shared_ptr::get()); the verifier
//  itself only ever holds `weak_ptr`, so it never perturbs the counts it reads.
// ---------------------------------------------------------------------------

struct MemVerifier {
    struct Tracked {
        long id = 0;
        int fn = 0, pc = 0;            // allocation site
        char kind = '?';               // o(bject) a(rray) m(ap) c(losure)
        std::weak_ptr<void> weak;      // ground-truth reachability (never counted)
        long shadowRc = 1;             // Phase B: the ARC's explicit count
        bool shadowFreed = false;      // Phase B
        bool gone = false;             // Phase A: weak has expired
        long deadAtOp = -1;            // Phase A: global op index of expiry
    };

    // Keyed by the underlying pointer so both engines agree on identity.
    std::unordered_map<const void*, Tracked> objs;
    long nextId = 1;
    long opClock = 0;                  // monotonic op counter (global schedule)
    long liveNow = 0, peakLive = 0;
    long totalAllocs = 0;
    bool shadow = false;               // Phase B enabled?
    std::vector<std::string> errors;   // invariant violations (Phase B)

    // --- identity ----------------------------------------------------------
    static const void* keyOf(const Value& v) {
        switch (v.kind) {
            case VKind::Object:  return v.obj.get();
            case VKind::Array:   return v.arr.get();
            case VKind::Map:     return v.map.get();
            case VKind::Closure: return v.closure.get();
            default:             return nullptr;
        }
    }
    static std::shared_ptr<void> spOf(const Value& v) {
        switch (v.kind) {
            case VKind::Object:  return v.obj;
            case VKind::Array:   return v.arr;
            case VKind::Map:     return v.map;
            case VKind::Closure: return v.closure;
            default:             return nullptr;
        }
    }
    static char kindOf(const Value& v) {
        switch (v.kind) {
            case VKind::Object:  return 'o';
            case VKind::Array:   return 'a';
            case VKind::Map:     return 'm';
            case VKind::Closure: return 'c';
            default:             return '?';
        }
    }

    // --- Phase A: census ---------------------------------------------------
    void onAlloc(const Value& v, int fn, int pc) {
        std::shared_ptr<void> sp = spOf(v);
        if (!sp) return;
        const void* k = sp.get();
        auto it = objs.find(k);
        if (it != objs.end() && !it->second.gone) return;   // already tracking this live cell
        Tracked t;
        t.id = nextId++; t.fn = fn; t.pc = pc; t.kind = kindOf(v);
        t.weak = std::weak_ptr<void>(sp);
        objs[k] = t;
        ++totalAllocs; ++liveNow;
        if (liveNow > peakLive) peakLive = liveNow;
    }

    // Detect cells that have just become unreachable (weak expired) and record
    // the op at which it happened. This is the free-schedule oracle.
    void sweep() {
        for (auto& [k, t] : objs) {
            if (t.gone) continue;
            if (t.weak.expired()) {
                t.gone = true; t.deadAtOp = opClock; --liveNow;
                if (shadow && !t.shadowFreed)
                    errors.push_back(siteStr(t) + " unreachable at op " +
                                     std::to_string(opClock) +
                                     " but the shadow ARC still holds it (refcount " +
                                     std::to_string(t.shadowRc) + ") -> LEAK");
            }
        }
    }

    static std::string siteStr(const Tracked& t) {
        return std::string("#") + std::to_string(t.id) + " " + t.kind + " @fn" +
               std::to_string(t.fn) + ":" + std::to_string(t.pc);
    }

    std::string report() const;
};
