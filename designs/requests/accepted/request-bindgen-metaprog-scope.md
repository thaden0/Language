# Request: Metaprogramming Scope for the DOM `@extern` Bindgen (Track W doc 06 §1)

**From:** Track W (wasm frontend) doc 06 investigation. **Date:** 2026-07-19.
**Priority:** P2 — blocks techdesign-06 §1 (generating the DOM bridge stubs); the
hand-written `Dom` prelude (doc 05, `Resolver.cpp` `kPreludeWasm`) is a working
substitute, so nothing is stopped in the meantime, just hand-maintained.

## 1. Background

techdesign-06 §1 proposed a `bindExtern` rule (attribute match + `$for` over
`C.methods` + `at namespace` injection) to generate the DOM handle-wrapper marshaling
stubs that doc 05 currently hand-writes. Investigated 2026-07-19 and found blocked by
two structural gaps in the metaprog rules engine, neither in the sanctioned P4 roadmap
(P4 design §9.2 deliberately bounds scope, "keep it bounded"):

1. **Declare-vs-generate collision.** To reflect a handle-wrapper's method signatures
   the rule must match the class that *declares* them; injecting same-named marshaling
   methods back into that same class collides in the member checker (verified live). A
   faithful bindgen needs either (a) signatures reflected off an *interface* the
   wrapper implements, with generated methods injected into the *implementing* class —
   i.e. inherited/interface **method** reflection (extends
   `request-metaprog-attr-values.md` item (E), which only covers **field** visibility,
   not method signatures via an interface) — plus (b) cross-class member injection
   (`at namespace` today only targets namespace-level generated decls, not "inject into
   class X while matching class Y").
2. **The target moved.** Doc 05 as-built (§9 item 1) replaced the
   one-wasm-import-per-method model with a single reflective `lv.dom_call(op, ...)`.
   There's no per-method import declaration left to generate — the real stub body needs
   an op-name string (`setAttr` → `"setAttribute"`) and a return-native tag (V/S/I by
   return type) that vary per method, i.e. conditional templating beyond what `$for`
   splicing expresses today.

## 2. The ask

One of:

- **(a) Metaprog scope expansion:** interface/inherited **method** reflection
  (parallel to item (E)'s field ask) + cross-class member injection
  (`inject ... into OtherClass`, or equivalent), sufficient to close gap 1. Gap 2
  additionally needs some conditional/templated string generation inside `$for` bodies
  (op-name and return-type-tag per reflected method) — scope and syntax for this isn't
  sketched anywhere yet and needs its own design pass, not just a bigger `$for`.
- **(b) DOM-seam redesign:** change doc 05's `lv.dom_call` marshaling shape so the
  per-method dispatch data (op string, return tag) is expressible as a plain
  attribute-argument reflection (e.g. `@extern(op: "setAttribute", returns: V)`) that
  today's `$for` + attribute-value reflection (already landed, item (A)) CAN generate
  from — sidestepping the need for interface reflection or cross-class injection
  entirely, at the cost of putting the op-string/return-tag directly on each
  hand-declared method's attribute instead of deriving it from the C++ bridge.

(b) is worth scoping first — it may be cheaper than (a) and doesn't touch the bounded
P4 metaprog surface at all. This ticket asks the owner/metaprog design track to pick a
direction; it is not itself a specific implementation ask.

## 3. Acceptance criteria (differs by path chosen)

- If (a): the `bindExtern` rule from techdesign-06 §1 generates stubs that diff-match
  the current hand-written `Dom` prelude stubs (doc 05), for the full DOM surface doc
  05 covers.
- If (b): the redesigned `@extern` attribute shape + rule generates the same stubs, and
  doc 05's hand-written stubs are deleted in favor of generated ones.
- Either way: full differential corpus green (`--run`/`--ir`/`--expand`), wasm lane
  included.

## 4. Interim fallback (already in place)

The hand-written `Dom` prelude surface (doc 05, `Resolver.cpp` `kPreludeWasm`) is the
as-built reality and is not blocked by this ticket — it works today. This request only
concerns replacing it with generated code; nothing is stopped in the meantime.

## 5. Not in scope

This ticket does not request the full P4 metaprog roadmap — only the specific
reflection/injection primitives (or attribute-shape redesign) needed to close
techdesign-06 §1's differential.
