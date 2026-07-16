# Tech Design: Procedural Macros (comptime code → code)

**Status:** implemented. **Date:** 2026-07-12. **Feature ID:** F4 (Sonar prerequisite).
**Depends on:** metaprogramming Phases 1–4 (landed), LA-20 comptime `import()` (landed). **Unblocks:** Sonar T06 (the `sonar!` template layer — the framework's differentiator) + T11 (reactivity).
**Difficulty:** L. **Risk:** MED — genuinely new capability, but bounded: comptime + parser + one value kind; **no ABI change, no backend change**.

Frozen surface: anchor §4/F4. Ground truth: infodemp §5 (the three-pieces map; re-verify lines at implementation).

---

## 1. Motivation

Landed expression macros are fixed-template substitution: `expandMacroCall` (`Rules.cpp:737-827`) clones a parse-time-fixed quasiquote and splices holes — a macro **cannot branch on its argument, count children, or synthesize different structure**. `sonar!(\`<App>…\`)` must parse a tag grammar and emit variably-shaped construction code. The capability gap is exactly: *a macro whose body is comptime code that inspects a payload and returns constructed code.*

Example of the capability (input → output shapes differ structurally by input):
```lev
sonar!(`<Text/>`)                  // → (() => { var __sonar_0 = Text(); return __sonar_0; })()
sonar!(`<Box><Text/><Text/></Box>`) // → a 3-node construction block — different shape, same macro
```

## 2. Current state — three pieces exist, unconnected through comptime (verified map)

1. **Raw payload exists as bytes**: a backtick lexes to one `QuasiLiteral` token (`Lexer.cpp:99-111`); payload = `file_.text[span+1..end-1]`. But QuasiLiteral is NOT a primary expression — accepted only as a macro-decl body (`Parser.cpp:1244`) and rule action (`:1314`). `sonar!(\`…\`)` won't parse today.
2. **String→AST engine exists**: fragment parsers `parseStmtsFragment`/`parseMemberFragment`/`parseExprFragment`/`parseItemsFragment` (`Parser.cpp:1388-1552`) re-lex a span and run a sub-Parser — but are unreachable from comptime (`Eval.cpp` has no Parser handle; no parse-as-code native).
3. **Splice substrate solved**: `Binding::exprNode` splices arbitrary pre-built subtrees (`Rules.cpp:1914-1916`); a computed tree can replace the template clone at the `slot = std::move(expanded)` assignment (`:826`).

The hard boundary: `reify` (`Rules.cpp:514-614`) is the sole Value→AST bridge and emits literals only; `VKind` (`RuntimeValue.hpp:35`) has no code carrier. **The Ast value kind is the single largest missing piece.**

## 3. Design

### 3.1 Surface (frozen)

```lev
macro sonar(string payload) comptime {
    // ordinary comptime Leviathan code (hermetic: sys* denied, import() allowed, step-bounded)
    var out = [ "(() => {" ];
    // ... parse payload, append lines ...
    out = out.add("})()");
    return meta::parseExpr(out.joinToString("\n"));
}
var app = sonar!(`<App title="demo"/>`);        // quasiliteral arg = raw payload string
comptime string tpl = import("views/editor.sonar");
var app2 = sonar!(tpl);                          // any comptime-evaluable string
```

- **Declaration:** `macro name(string p) comptime <body>` — body-is-one-statement; returns `Ast`. v1: exactly one `string` parameter (multi-arg and `Ast`-typed params are designed-but-deferred, §8).
- **Call:** `name!(arg)`; the argument is comptime-EVALUATED to a string (contrast template macros, which bind args as unevaluated subtrees). A quasiliteral in macro-argument position is sugar for a raw string literal of its payload — the parser change is scoped to exactly that position (macro-call args), NOT general primary expressions (keeps the grammar door closed for future quasiquote uses).
- **`Ast`:** a new comptime value kind (VKind addition) + an opaque prelude `class Ast;`. Carries an owned parsed subtree + its fragment kind (expr | stmts). Opaque in v1: no traversal API (structured reflection is the deliberately-deferred can of worms); printing shows `Ast(expr)`.
- **`meta::parseExpr(string) -> Ast` / `meta::parseStmts(string) -> Ast`:** comptime-intercepted natives on the `import()` precedent (allowed at comptime; runtime bodies throw). They expose the existing fragment parsers over a runtime-built string (the parsers already take an arbitrary span; parsing a heap string is the only extension — lex into a temporary buffer registered with the diagnostics sink).
- **Splice:** `expandMacroCall` grows a comptime-body branch: evaluate the body on the comptime oracle with the arg bound as a VALUE; the returned Ast's tree is assigned into the call-site slot (the reify-sibling case: an Ast-valued result splices its carried tree directly). M22's single-splice hole guard doesn't apply to computed output; **M24 no-re-entry is KEPT**: macro output is not re-scanned — a `name!(...)` inside generated code is detected post-splice and is a compile error ("macro call in generated code"), never silently ignored.
- **Hygiene contract (v1, honest):** computed output cannot be alpha-renamed against a template, so **macro authors own generated-local uniqueness** via the `__<pkg>_`-prefix + counter convention (plain legal identifiers; `$`-names are compiler-internal). Def-site qualification passes that apply to rule-injected clones apply to the spliced tree identically (it goes through pass-2 resolve/check like injected code). A user local colliding with a generated name is the user's `__`-prefix violation; documented.
- **`--expand`:** the expansion is source-shaped and must round-trip; the macro body itself never appears in output (it ran at compile time).

### 3.2 Pass ordering (normative)

Comptime-macro expansion runs in the existing `macroExpansionEnabled_` second walk (`Rules.cpp:55-64`), at the same point template macros expand — the only difference is HOW the replacement tree is produced (oracle evaluation vs clone). The body observes NOTHING of the program (its string arg only — this is not reflection); its OUTPUT gets full pass-2 re-resolve/check and normal lowering, like rule-injected code. Rules fire on the augmented tree per the existing orchestration; a rule matching macro-generated declarations works (the T11 attribute-relay depends on this — state it as a supported composition and pin with a test).

### 3.3 Error taxonomy (normative)

| condition | behavior |
|---|---|
| payload parse error in `meta::parse*` | compile error at the MACRO CALL SITE + the fragment text with a caret at the offset (a dedicated renderer — the span-attribution soft spot, addressed head-on) |
| macro body throws | compile error: macro name + call site + the exception message |
| body returns non-Ast | compile error at the return (evaluator-enforced in v1; the body is comptime-evaluated, not statically checked — v2 integrates the checker) |
| step-budget exhaustion | existing comptime budget diagnostic + the macro name (`--comptime-budget` escape) |
| expr Ast spliced where stmts expected (or v.v.) | kind check at splice, call-site error |
| `name!()` in generated output | "macro call in generated code" (M24 ruling) |

### 3.4 v1.5 extension (designed now, built later): macro-emitted companion members

Sonar reactivity needs member-level effects. Two mechanisms analyzed: (a) `meta::injectMember(Ast)` from the macro body — requires an encloser handle at expansion time (new plumbing into the rules engine's anchor machinery); (b) **attribute-relay**: the macro's ordinary output carries attributes that landed Layer-B rules match (`member of` anchors) and expand. **(b) is the v1.5 recommendation** — zero new capability, composes with landed machinery; (a) recorded as v2. This choice is load-bearing for T11 and frozen there.

## 4. Implementation plan

| M | step | difficulty |
|---|---|---|
| M1 | `VKind::Ast` + owned-tree carrier + opaque prelude `class Ast` | M |
| M2 | `meta::parseExpr/parseStmts` comptime natives + the call-site error renderer (heap-string lex seam) | M |
| M3 | Parser deltas: `comptime`-bodied macro decl form; quasiliteral in macro-arg position | S/M |
| M4 | `expandMacroCall` comptime branch: arg evaluation, oracle run, Ast splice, kind check, generated-macro-call scan, M22 bypass | M/L |
| M5 | Diagnostics polish + `--expand` round-trip | S |
| M6 | Corpus | M |

M1+M2 are parallel-safe; M4 depends on both; M3 independent.

## 5. Edge cases & failure modes

Empty payload (macro decides — `meta::parseExpr("")` is a parse error surfaced per taxonomy); macro returning the SAME Ast twice (each splice must clone — Ast splices by clone, not move, so a memoizing macro is safe); a macro FUNCTION calling itself recursively inside the body (fine — bounded by the step budget; distinct from macro-invoking-macro, which is the banned re-entry — the distinction stated loudly); comptime string arg that isn't comptime-evaluable → existing comptime diagnostic; generated code containing `comptime` declarations (legal; folded in pass-2 like handwritten).

## 6. Potential issues & mitigations

1. **Span attribution for generated nodes** (the known soft spot): all spliced nodes carry the macro-call span so downstream type errors point at the template; payload parse errors get the §3.3 caret renderer. Downstream errors deep inside a big expansion are call-site-coarse in v1 — mitigation: emitters put each construct on its own line so `--expand` reading is fast; per-hole sub-span mapping is v2.
2. **Comptime budget vs large templates** — a 500-tag view through an O(n) emitter fits comfortably in ~100M steps; the macro author discipline (array-of-strings + joinToString, never O(n²) concat) is documented; budget-exceeded names the macro.
3. **Hermeticity** — the oracle already denies `sys*` at comptime and sanctions `import()` (LA-20, trident-hashed assets); no new holes. Determinism inherited.
4. **Grammar creep from the quasiliteral change** — scoped strictly to macro-call argument position; the parser delta is a single acceptance point, tested for non-leakage (quasiliteral anywhere else still errors).
5. **Fragment-parser reuse from Eval** introduces a Parser dependency into the comptime evaluator — keep it behind a narrow seam (a `ComptimeParseHook` function pointer installed by the driver) so Eval stays layer-clean.
6. **Rules × macros ordering regressions** — pin the orchestration with a corpus program combining a rule and a comptime macro whose output the rule matches (the T11 shape).

## 7. Testing plan

Corpus under `tests/corpus/meta/` (existing conventions; `@no-roundtrip` only where comptime prints): identity macro; branching macro (payload "a"/"b" → structurally different output — the capability proof); list-generating macro (N → N calls); parse-error position (caret offset pinned); body-throw; budget; generated-macro-call error; import()-fed payload with assets; rule-matches-generated-output (T11 shape); `--expand` round-trip of all green cases; one runtime program per engine lane (expansion is pre-lower ⇒ all four engines identical — pin it anyway).

## 8. Open questions

1. Multi-arg / `Ast`-typed macro params — deferred; surface sketched (`macro f(string a, string b) comptime`), no blocker, demand-gated.
2. `meta::quote` (quasiquote-with-holes builder as a nicer authoring surface than string assembly) — v2; string path is sufficient and `--expand`-verifiable.
3. Statically checking macro bodies (they're evaluator-enforced v1, like prelude leniency) — revisit with checker-integration appetite.

## 9. Implementation log

- 2026-07-12 — design written; not started.
- 2026-07-12 — M1–M6 landed: `VKind::Ast`, opaque prelude `Ast`, the
  `ComptimeParseHook` heap-string fragment seam, procedural declaration and raw
  macro-argument syntax, isolated/recursive comptime body evaluation, clone-on-
  splice with call-site restamping, M24 generated-call rejection, caret-bearing
  fragment diagnostics, `--expand` round-trip, and corpus/unit coverage. The
  runtime proof passes oracle, IR, emit-C++, and LLVM; frozen ELF is not a gate.
