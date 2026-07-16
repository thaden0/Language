# Request: Per-Instantiation Overload Resolution in Generic Bodies (LA-18 demand-trigger extension)

**Design (to extend):** `designs/complete/techdesign-generic-static-members.md` (LA-18, **LANDED
2026-07-12**) — the demand-driven, above-IR monomorphization pass. This request asks to **widen its
`specializationRequired` trigger**, not to build new codegen machinery.

**From:** Atlantis Track 03 (serialization; the `__atlEnc`/`__atlDec` families) — also lands the
general fix for any framework/stdlib generic that dispatches an overload set on a type-parameter
value. **Date:** 2026-07-14. **Priority:** P1 — closes bug **#54** (`known_bugs_2.md`), currently the
only open entry there.

## 1. The ask

Inside a generic body, a call to an **overloaded free/namespace/static function** passing a
**type-parameter-typed value** as an argument must be overload-resolved **per concrete
instantiation**, exactly as a hand-written monomorphic body would be:

```lev
namespace Probe {
    interface ISerializable { string tag(); }
    string enc(int v) => "I" + v.toString();
    string enc(string v) => "S" + v;
    string enc(ISerializable v) => "O" + v.tag();

    Array<string> encArr<T>(Array<T> v) {
        Array<string> out = [];
        for (T item in v) { out = out.add(Probe::enc(item)); }   // resolve per concrete T
        return out;
    }
}
uses Probe;
class Thing : Probe::ISerializable { string tag() => "thing"; }

Array<int>    xs = [1, 2, 3];
Array<string> ss = ["a", "b"];
Array<Thing>  ts = [Thing()];
console.write(Probe::encArr(xs).joinToString(",") + "|" +
              Probe::encArr(ss).joinToString(",") + "|" +
              Probe::encArr(ts).joinToString(","));
```

**Expected:** `I1,I2,I3|Sa,Sb|Othing`.
**Actual (bug #54):** the call resolves **once**, at the single definition-check, against the
first-declared applicable overload (`enc(int)`), and every instantiation is then read through that
one static parameter type — silently wrong (exit 0, no diagnostic) and **inconsistently** so per
engine:
- `--run` (oracle): `I1,I2,I3|I,I|I`
- `--ir`: `I1,I2,I3|I,I|I`
- `--build-native` (LLVM): `I1,I2,I3|Ia,Ib|I` (diverges from oracle/IR on the `string` case)

The `int` case reads correctly only by coincidence (first-declared overload = first array tried).

## 2. Why this is the robust fix, not a patch

This is the **free-function-overload variant** of the exact gap LA-18 already documents for
`T::member` (that doc's §7.6 and bug #54's own write-up call it "the same underlying gap"). Today
the `specializationRequired` demand fires only when a generic body **syntactically contains
`X::member`**. Bug #54's shape has no `::` on the type parameter — it is an ordinary overloaded call
with a `T`-typed argument — so the demand never fires, the body compiles once with the argument's
static type still the unresolved `T`, and overload resolution collapses.

Widening the trigger to **"a call to an overload set with at least one type-parameter-typed
argument"** routes #54 through the *already-landed* pipeline: specialization set keyed by
`(fn, type-tuple)`, one concrete body emitted per distinct tuple, dedup, transitive propagation, and
— because specialization lives **above the IR** — every engine (oracle, IR, emit-C++, LLVM, ELF)
consumes the concrete copies as ordinary function-table entries with **no new op**. That above-IR
property is what makes this differential-safe by construction: an interpreter-only patch would break
the oracle/IR-vs-native contract (the failure mode #3 demonstrated), whereas specialization produces
identical concrete bodies for all engines to lower.

This advances the language's core theses directly: it lets a framework author write **one** generic
adapter instead of per-type hand-written monomorphic overloads (redundancy reduction), and it makes
an erased, dynamically-dispatched language behave like a monomorphizing one at the call boundary
(low-level cost model, high-level ergonomics — info.md's stated aim).

## 3. Requested specific feature

- **Trigger (M1 checker):** in addition to the `X::member` demand, mark a generic callable
  `specializationRequired` when its body contains a call whose resolved callee is an **overload set**
  (2+ candidates distinguished by argument type) and at least one argument's static type is (or
  contains, e.g. `Array<T>`, `T?`) a **function-level** type parameter of the enclosing callable.
  Record the demand per call site (callee overload set + which argument positions carry the
  type-parameter).
- **Tuple collection:** reuse LA-18's existing collection on the **pass-2 (post-rule-injection)**
  tree (`inferConstruction`/`genericReturn`), so rule-generated call sites contribute tuples too.
- **Lower (M2):** in the copy with `T = Foo`, the overloaded call re-resolves through the **ordinary
  concrete path** with `T` pinned to `Foo` → one concrete `CallFn`. No runtime dispatch, no erasure
  problem. Dedup and transitive propagation are unchanged from LA-18.
- **No public syntax change.** This is resolution semantics inside an already-legal body; the source
  in §1 compiles today (wrongly). Nothing new for the user to approve.
- **Scope boundary — inherit LA-18's exactly:** function-level type parameters only. Class-level
  type parameters (§7.6), HKT parameters (§7.2), and instance methods under override dispatch (§7.7)
  stay the same v1 compile errors — the trigger must make the same declaring-scope distinction. #54's
  consumers (framework/stdlib free functions on concrete DTO/primitive instantiations) hit none of
  these, identical to Track 03/07.

## 4. Known warnings

- **Bug association:** closes **#54** (`known_bugs_2.md`), P1. `docs/footguns.md` line ~54 records the
  current workaround as a footgun — remove that row on landing.
- **Overload-set selection at collection time:** the checker must record the *unresolved* overload
  set at the demand site and re-run selection **inside each specialized copy**, not memoize the
  definition-site (wrong) pick. This is the crux of the fix — the current bug is precisely a premature
  memoized selection.
- **Interaction with existing overload scoring:** watch for entanglement with bug **#34** (lambda
  literal wrongly applicable to a string param in overload scoring). Re-resolution inside a copy runs
  the same scorer; add a corpus case mixing a lambda-typed and a type-parameter-typed overload to
  guard against a regression there.
- **Span attribution (§7.3, soft):** specialized copies are late-materialized; a runtime throw or
  secondary type error inside one must attribute to the **original source span**. Add a corpus case
  whose specialized body throws and assert the report points at the original line.
- **Termination (§7.1):** same finite `(fn × whole-program-types)` guarantee as LA-18; the depth
  bound + reification-fallback STOP already designed in applies unchanged.
- **`this`-receiver hazard (bug #53):** the repro uses a bare namespace-qualified call, not a bare
  sibling instance method, so it is unaffected — but corpus bodies added for this feature must keep
  using explicit `this.` for any sibling instance call.

## 5. Acceptance criteria

1. **Corpus (the §1 program):** `Probe::encArr` over `int`/`string`/`Thing` instantiations produces
   `I1,I2,I3|Sa,Sb|Othing` on **oracle, IR, and LLVM identically** (exit 0). Add the `--build-native`
   leg explicitly so the current LLVM `string`-case divergence is a hard-failing regression test.
2. **Overload dispatch reaches interface members:** the `ISerializable` instantiation calls
   `v.tag()` (currently never reached), proving the `enc(ISerializable)` overload is selected in that
   copy.
3. **Missing-overload instantiation = clean compile error** naming the use site and the concrete
   type (reuse LA-18 §4.3's two-span diagnostic), e.g. instantiating `encArr<SomeType>` where no
   `enc(SomeType)` and no applicable overload exists.
4. **Works for value structs AND reference classes** as `T` (parallels LA-18 acceptance #2).
5. **No regression to unspecialized generics:** a generic body with no type-parameter-typed overload
   call and no `X::member` still compiles exactly once (assert no code-size/behaviour change on the
   existing generic corpus).
6. **Rule-injected call sites** contribute tuples (a rule/`@`-generated body that calls the overload
   set on `T` specializes correctly), verifying pass-2 collection.

## 6. The complete-but-heavier alternative (recorded, so it is not re-litigated)

If a future consumer needs an instantiation shape this trigger extension **cannot enumerate**
(class-level type parameters under legal raw-widening — §7.6; a non-terminating `A → Box<A> → …` set
— §7.1; or overload dispatch across an override set — §7.7), the sanctioned correction is
**reification** (§4.4 of the LA-18 design): a runtime type descriptor per type parameter so
resolution happens at runtime against the actual type. It is the *only* route that covers every
erasure-gap shape, but it carries a calling-convention change + a descriptor value kind + static
v-tables **on all five engines** + per-call runtime cost — contradicting LA-18's "cost-identical,
whole-program" thesis — and partly un-does the deliberate erasure architecture (info.md). It is a
**separate, weeks-scale design**, not this request. No current consumer needs it; #54 and every known
consumer are fully served by the demand-trigger extension above. **Do not** implement unconditional
C++-style monomorphization as a middle path — it is more invasive than this extension yet still walls
out at §7.1/§7.6, i.e. worse on both axes.

## 7. Interim fallback (in use)

Hand-written monomorphic per-type overloads instead of one generic helper — the workaround #54 and
`docs/footguns.md` already record (Track 03's `__atlEnc`/`__atlDec` families ship this way,
`packages/atlantis/src/serialization/*`). Deletable on landing: once the trigger extends, the generic
helper can replace the per-type ladder with no source change beyond removing the duplicates.
