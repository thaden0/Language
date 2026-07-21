// RuntimeCore — one shared implementation of the runtime semantics that the
// AST oracle (Evaluator) and the --run executor (IrInterp) used to duplicate.
// Filled by refactor_1 session 02 (techdesign-02-runtime-core-opus.md).
//
// Everything here is a free function over Value / Symbol / Stmt — no engine
// state, no virtuals, no new class. Anything that needs engine behavior (how a
// method is invoked, how a runtime exception is raised, how a job task is
// spawned) is taken as an explicit parameter/callback so dispatch, arithmetic,
// member access, and task-state transitions can never drift between the engines
// again (the bug.md #13 family).
#pragma once
#include "runtime/RuntimeValue.hpp"
#include "core/Symbols.hpp"
#include <functional>
#include <memory>
#include <string>

// --- (1) arity-aware method lookup -----------------------------------------
// bug.md #13: an overloaded same-class call left unresolved by the checker (it
// never walks prelude class bodies) reaches this by-name fallback. It must not
// pick the first name-match arity-blind — that silently ran the 1-arg indexOf
// for a 2-arg indexOf(sub, from) and hung. When argc is supplied, prefer the
// unique method whose parameter count matches; a tie (same arity, different
// param types) or no arity match keeps the legacy first-found choice; a single
// non-overloaded method is unaffected either way. The walk order over the
// (flattened, inheritance-inclusive) slot list is the bug.md #13 specification
// and must be preserved exactly.
const Stmt* rtFindMethod(Symbol* cls, const std::string& name, int argc = -1);

// --- (2) object arithmetic --------------------------------------------------
// Operator token -> the method name the checker (opMethodName) resolves it to.
// Kept in lockstep with the checker's authoritative mapping for the binary
// operators that reach object dispatch.
const char* rtOpSymbol(TokenKind op);

// Shared object-operator dispatch: the resolved-decl-else-lookup pattern, the
// (!=)->(==)-negate fallback, and the reference-identity rule for (==)/(!=) on a
// non-value class (a value struct is synthesized field-wise (==) at resolve
// time, §5.5, so it never reaches the fall-through). `resolved` is the checker-
// chosen operator method (may be null -> by-symbol lookup). The two engine-
// specific steps are callbacks:
//   invoke     — perform the engine's actual method call on (l, r)
//   raiseNoOp  — raise the engine's runtime exception for the given operator
//                symbol and return the value to yield (vvoid()).
Value rtObjectArith(TokenKind op, Symbol* cls, const Stmt* resolved,
                    const Value& l, const Value& r,
                    const std::function<Value(const Stmt*, const Value&, const Value&)>& invoke,
                    const std::function<Value(const std::string&)>& raiseNoOp);

// --- (3) raw member (slot/field/key) access --------------------------------
// "name" (+ optional source qualifier) -> the storage key, honoring distinct
// slots. Accessor (get/set method) resolution stays engine-side; this is the
// raw key computation only.
std::string rtKeyFor(Symbol* cls, const std::string& name, const std::string& source);

// Raw slot read/write over an object's fields, after accessor resolution has
// been declined by the caller: a weak slot round-trips through the weak table,
// everything else is a plain fields[] access.
Value rtRawFieldGet(const std::shared_ptr<Object>& obj, const std::string& storageKey);
void  rtRawFieldSet(const std::shared_ptr<Object>& obj, const std::string& storageKey,
                    const Value& v);

// --- (4) task / promise state transitions ----------------------------------
// One loop-dispatched job (callback + argument), heap-carried into its task.
// Shared by both engines' taskRunJob/taskLoopStep so the loop-drain protocol is
// defined once.
struct RtTaskJob {
    Value callback;
    Value argument;
};

// G15: poll a promise object's settled state as bool immediates only (bit 0 =
// ready, bit 1 = failed) — the exact reads the pump's ready-check made, ARC-
// silent (no Value copies of object-typed fields).
int rtPollPromise(const void* obj);

// One loop batch drained into tasks: every user callback becomes a task (spawn
// order = FIFO runq order = the pump's inline dispatch order). Returns 0 when a
// termination is stashed or the loop has no work (mirroring the pump drain's
// `while (!throwing_ && loop.hasWork())` gate), 1 after spawning a batch. `spawn`
// is the engine's taskRunJob thunk; each job is a heap RtTaskJob it consumes.
int rtTaskLoopStep(bool termStashed, void (*spawn)(void*, void*));

// G11: the throw flag never crosses a task boundary; first termination wins (the
// pump stopped all dispatch at the first throw, so later ones were unreachable).
// Stashes the terminating (throw or env.exit) state and clears the live flags.
void rtCaptureTermination(bool& throwing, bool& exiting, const Value& thrown,
                          bool& termStashed, Value& termThrown, bool& termExiting);
