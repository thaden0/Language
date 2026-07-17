# Packet 04 — exterminate the silent-false fallbacks (Model: **Opus**)

Design owner ruling: "the status-quo behavior — bare `==` on an `(==)`-less
struct silently returning `false` — dies in M1 … and must not survive in any
form." With packets 02+03 landed, the per-engine `keyEquals` struct-`==`
fallbacks are dead code on every checked path. Remove them so the ONLY
struct relation is the generated/explicit method — and so any future hole
surfaces as a loud runtime error, never a silent `false`.

## The four kill sites

Remove ONLY the `isValue → keyEquals/keyEq/lvrt_keyeq` arm. The class
reference-identity arm **stays** in all four (classes without `(==)` remain
identity — design §5.2 ladder, checker branch `Checker.cpp:2060-2065`).

1. **Oracle** `Eval.cpp:1073-1082` (`combine`):
   ```cpp
   bool same = l.obj->cls && l.obj->cls->isValue ? keyEquals(l, r) : (...identity...);
   ```
   → keep the branch but for value classes fall through to the existing
   `throwRuntime("no operator '==' on '...'")` below (line 1083). i.e.:
   ```cpp
   if ((op == EqEq || op == BangEq) && l.obj->cls && !l.obj->cls->isValue) {
       bool same = r.kind == VKind::Object && l.obj == r.obj;
       return vbool(op == EqEq ? same : !same);
   }
   // value struct with no (==): unreachable from checked code (packet 03
   // gate); loud if ever reached.
   ```
2. **IR interp** `IrInterp.cpp:728-735` (`objectArith`): same transform;
   fall through to the existing `raise(...)` (line 736).
3. **emit-C++** `CGen.cpp:1560-1567` (the emitted `opm` tail): drop the
   `isValueClass(l.o->cls) ? keyEq(l,r) :` arm; keep
   `(r.k == 5 && l.o == r.o)` identity for non-value classes; value class →
   fall to the emitted raise/`V{}` tail — check what the emitted `opm`
   returns on no-match today (`return V{};`, line 1567) and instead emit a
   `raise("no operator '==' on struct")` so it's loud, matching engines 1/2.
4. **LLVM runtime** `lv_runtime.c:1326-1337` (`lvrt_opm`): drop the
   `lvrt_isvalueclass(classId) ? lvrt_keyeq(l,r) :` arm; keep payload
   identity for non-value classes; value class → route to the runtime's
   existing raise/trap helper (find how `lvrt_opm`'s no-method tail reports
   today and match the other engines' message text).

## What does NOT change

- `keyEquals` (`RuntimeValue.hpp:242`), CGen's emitted `keyEq`
  (`CGen.cpp:231`), `lvrt_keyeq` (`lv_runtime.c:2075`) all SURVIVE — they
  are the live Map-key comparators (`Map.at/has/with`, `idxget`, upsert).
  Only their *operator-fallback callers* die. (Their float legs change in
  packet 05.)
- The `!=`-derives-from-`==` arms above each kill site stay.
- X64Gen/NativeRuntime: untouched (frozen; never had the fallback).

## Message discipline

All four engines should produce the same runtime-error text for the
now-unreachable case; reuse the existing "no operator '==' on 'T'" shape
each engine already has for other missing operators. Byte-identical matters
only if a corpus file can reach it — none can (checker gate) — but keep
them aligned anyway.

## Edge to verify (from packet 02's notes)

Comptime (RuleEngine oracle) evaluates between resolver passes. A rule that
*generates a struct and compares two instances inside comptime of the same
run* would now hit the loud runtime error instead of silent field-wise
equality, because that struct's synthesis happens in pass 2. Grep the rules
corpus (`tests/corpus/meta/`, `tests/corpus/composition/`) for comptime
struct compares; run `corpus_meta_treewalk`/`_ir` and the metatests. If
anything trips: STOP, report — the fix direction is running synthesis
inside `RuleEngine`'s post-injection hook too, which needs a design call,
not an improvisation.

## Update the stale pin comment

`tests/corpus/composition/aggregates/green/struct_default_eq.lev` header
says "Reuses the same keyEquals recursion Map keys already use" — rewrite to
"synthesized field-wise `(==)` (designs/struct-equality/, §5.5); Map keys
keep their own keyEquals recursion." Output must not change.

## Acceptance

- Full `ctest` green; equality cluster + `struct_default_eq` byte-identical
  on all four lanes (the OUTPUT must not move — same values, new mechanism).
- `grep -n "keyEquals(l, r)" src/Eval.cpp src/IrInterp.cpp` → no operator
  fallback hits remain (Map-key call sites remain).
- Run harpoon's test suite if wired in ctest (assertEqual relies on struct
  `==`; grep `harpoon/tests` lanes) — must stay green.

Commit: `Struct equality packet 04 (M1): remove per-engine keyEquals ==
fallbacks; struct == is the method, loud otherwise`.
M1 IS NOW COMPLETE — pause and re-run the FULL suite before starting M2.
