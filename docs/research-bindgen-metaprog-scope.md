# Research: Metaprogramming Scope for the DOM `@extern` Bindgen

**Status:** RESEARCH — feeds a tech design, not one itself. **Date:** 2026-07-19.
**Answers:** `designs/requests/request-bindgen-metaprog-scope.md`.
**Sources grounded against (code, not prose, wherever the two disagree):**
`src/Rules.cpp` (2598 lines), `src/Rules.hpp`, `src/Resolver.cpp` (`kPreludeWasm`,
lines 5302–5423), `designs/wasm-frontend/techdesign-05-dom-bridge.md`,
`designs/complete/hard-06-hostbridge-seam.md`, `designs/wasm-frontend/techdesign-06-bindgen-and-ship.md`,
`designs/complete/techdesign-metaprog-phase4.md`, `designs/requests/request-metaprog-attr-values.md`,
`designs/requests/request-metaprog-splices.md`, `designs/complete/techdesign-unit-test-library.md`,
`tests/corpus/meta/rule_orm.ext`, `rule_attr_values.ext`, `rule_stmt_for.ext`.

## 0. What this document is for

The request ticket asks the metaprog owner to pick between two directions (or find a
third) to close techdesign-06 §1's differential. This document is the grounding for
that decision: the exact current behavior of the rule engine (verified against source,
not the design docs describing it — two of which are already stale in small but
load-bearing ways, flagged below), a full inventory of the generation target (the `Dom`
prelude surface), a precise mechanical account of both blocking gaps, and — new,
surfaced during this research, not present in the request ticket — a **third candidate
direction** that may sidestep both gaps using machinery that already shipped
(`rewrites`/`replace`/`$body`, Phase 4, landed 2026-07-10) but that neither the original
techdesign-06 §1 sketch nor the request ticket considered.

Nothing here decides the direction. §9 lays out three candidates with honest tradeoffs;
the design pass picks.

## 1. The ask, restated precisely

techdesign-06 §1 proposed generating the DOM handle-wrapper marshaling methods (`DomNode
setAttr`, `string attr`, etc. — currently hand-written in `kPreludeWasm`,
`Resolver.cpp:5350-5410`) via a `bindExtern` rule. Investigation (2026-07-19, recorded in
techdesign-06's own header) found it blocked by two independent structural facts, not one:

1. **Declare-vs-generate collision** (§5 below) — verified live against `Rules.cpp`'s
   `injectMember` (line 2515): a rule that matches a class *and* injects a same-named,
   same-typed member back into *that same class* trips the M33 collision check. A
   faithful generator needs the method's **signature** to live somewhere the rule can
   read without also being where it **writes** the generated body.
2. **The target moved** (§6 below): doc 05 shipped with a single reflective
   `std::sysHost{I,S,V}(op, h0, h1, a, b)` seam instead of the originally-assumed
   one-wasm-import-per-method model. There is no per-method import declaration left to
   generate — the real work is picking `op` (a string), the native tag (I/S/V, by return
   shape), and which of the four marshaling slots (`h0`/`h1`/`a`/`b`) each parameter
   fills — and this varies per method in a way plain `$for` splicing can't express.

## 2. RuleEngine primer (grounded, for whoever writes the design)

### 2.1 Pipeline

Four layers, in order, per `Rules.hpp:23-35`:

- **Layer A (attributes):** resolve `@Name(args)` against the declaring file's `uses`
  imports, type-check + evaluate the args to compile-time `Value`s
  (`RuleEngine::processAttrs`/`evalAttrArgs`).
- **Layer B (rules):** `match` a declaration shape, `inject` quasiquoted code at an
  anchor. The layer this ticket is about.
- **Layer C (comptime):** fold `comptime` vars/exprs/ifs via a tree-walk oracle.
- **Layer D (rewriters):** `rewrites body of <bind> { replace \`...\` }` — replaces
  (not adds beside) a matched method's body, `$body` splices the original back in.
  **Landed** 2026-07-10 (`techdesign-metaprog-phase4.md` §14 item 1) — see §2.7, this is
  the capability the request ticket and the original techdesign-06 sketch both overlook.

`RuleEngine::run()` (`Rules.cpp`) drives: fold comptime → resolve attrs → index decls →
match+expand rules (`runRules`, two passes: additive `inject`s first, then `replace`
rewriters, per Phase 4 §2.4's defined order) → optional `reentrant` fixpoint
(`runReentrantFixpoint`).

### 2.2 Core data structures

```cpp
// Rules.hpp — one resolved match binding. Unified on Value so the same
// binding serves template splices ($r.method) and `where`-clause names.
struct Binding {
    Value val;                 // attribute: its evaluated Object. decl: unset.
    bool hasVal = false;
    Stmt* declStmt = nullptr;  // decl identity — the ACTUAL class/method/field Stmt*
    Symbol* declSym = nullptr;
    std::string_view selectorText;   // member selector, for name holes
    const Expr* exprNode = nullptr;  // macro-arg subtree (Layer C only)
};
using Bindings = std::map<std::string_view, Binding>;
```

Every name a rule binds — the subject (`on class C` → `C`), each encloser
(`in class D` → `D`), the attribute value (`@Route(r)` → `r`) — is one `Binding` in one
flat map, keyed by the source name the rule author chose. **This is the mechanism that
makes "inject into class X while matching class Y" hard**: `boundClass(b, target)`
(`Rules.cpp:2427`) does a **lookup in this same map** —

```cpp
Stmt* RuleEngine::boundClass(const Bindings& b, std::string_view target) const {
    auto it = b.find(target);
    if (it == b.end() || !it->second.declStmt) return nullptr;
    Stmt* s = it->second.declStmt;
    return s->kind == StmtKind::Class ? s : nullptr;
}
```

— an injection target (`member of <target>`) must be a name **already bound by this
same match** (subject or an encloser). There is no query that returns "classes that
implement interface I" or "the class enclosing method m" unless the rule's own `match`
clause bound it. This is exactly gap 1's cross-class-injection half: nothing produces a
second class binding from a match on one class.

### 2.3 The `meta.*` object model (exact current schema)

Built lazily by `buildMetaValue` (`Rules.cpp:1378-1486`), cached per-`Stmt*`. This is the
full current shape — anything not listed here does not exist yet:

```
meta::Class {
    string name
    Array<string> bases          // STRINGS ONLY — base->canonical, not resolved
                                  // meta::Class objects. No structured base walk.
    Array<meta::Field>  fields    // decl->body only — OWN members, not inherited
    Array<meta::Method> methods   // decl->body only — OWN members, not inherited
}
meta::Method / meta::Field {      // every callable member -> Method, else -> Field
    string name
    Array<string>     attrs        // attribute NAMES only
    Array<meta::Attr>  attributes   // names + evaluated ARGS (item A, landed)
    // Method only:
    string returnType
    Array<meta::Param> params
    // Field only:
    string type
}
meta::Param { string name; string type }
meta::Attr  { string name; Array<meta::AttrArg> args }
meta::AttrArg { string s; int i; bool b; float f; bool present }  // typed union;
                                  // `present` distinguishes explicit from defaulted
```

Two facts fall directly out of `buildMetaValue`'s loop (`Rules.cpp:1402-1408`,
`for (const StmtPtr& m : decl->body)`):

- **`C.fields`/`C.methods` reflect own-body members only.** A mixin's or an interface's
  members are invisible to `$for m in C.methods` when the rule matches `C`. This is
  request-metaprog-attr-values.md item (E), *scoped to fields only* there — the DOM
  bindgen needs the **method** analogue, which nobody has asked for as its own item
  before this ticket.
- **`bases` is `Array<string>`**, not `Array<meta::Class>`. Even if `C.methods` grew an
  inherited variant, there is today no way to walk "what does base B declare" from a
  `meta::Class` value — you'd need `meta::Class.bases` to carry resolved objects (item H,
  "structured Type," `techdesign-metaprog-phase4.md` §9.1 — also deferred, unrelated to
  method reflection but load-bearing for any design that wants to say "reflect off my
  base's methods" symbolically rather than positionally).

### 2.4 Matching semantics — a footgun that bites an interface-based design directly

Subject-position kind matching (`match @Attr on class C`) is **exact**
(`declKindMatches`, `Rules.cpp:1317`, confirmed no leniency): `on class C` does **not**
fire on an `interface`, and vice versa. `interface` **is** a distinct, valid subject
kind-word (`declKindWord`, `Rules.cpp:1354-1366`, returns `"interface"`; indexing tags it
correctly, `Rules.cpp:1284`) — so `match @Extern(e) on interface I` is legal and correct
today, but `on class C` silently never fires on an interface `I` with no diagnostic
(`request-metaprog-splices.md` §4 addendum, confirmed live repro). **Any design that
attributes an interface must spell `on interface`, exactly** — this is not a gap to close,
just a sharp edge to get right in the rule the design pass writes.

Encloser-position matching (`in class D`) is deliberately lenient and *does* accept
`struct`/`interface` ancestors (`Rules.cpp:1562-1565`) — the asymmetry is intentional
and documented (`declKindMatches`'s own comment), not a bug, but it means subject- and
encloser-position kind words behave differently and a design that mixes them needs to
know which is which.

### 2.5 Injection anchors — what each can target

From `AnchorKind` (`Ast.hpp:293-302`) and `expand()`'s per-anchor branches
(`Rules.cpp:1710-1864`):

| Anchor | Target | Can cross to an unbound class? |
|---|---|---|
| `member of <target>` | `boundClass(b, target)` — must be a `Binding` in *this* match | **No** |
| `top`/`bottom of <target>.constructor` | same `boundClass` lookup | **No** |
| `top`/`bottom of body` | implicit — always the subject (`subjectOf(b)`) | **No** (subject only) |
| `marker "name"` | searched within the **subject's own** normalized body only (§9.4 of P4 — class-wide search is explicitly deferred, "add on demand") | **No** |
| `rewrites body of <bind>` (Layer D) | the named binding's own body, overwritten in place | **No** — but doesn't need to; see §9.3 |
| `at namespace N` | a **literal namespace name string**, not a binding at all — `expand()`'s `else` branch (`Rules.cpp:1855-1863`) builds a fresh `namespace N { ... }` `Stmt` from scratch and queues it in `namespaceInjections_`, flushed into `program.items` and pass-2-resolved like hand-written code | N/A — namespace-level only, never a class member |

**No anchor injects a class member into a class other than one already bound by the
firing match.** This is exactly what the request ticket calls "cross-class member
injection... `at namespace` today only targets namespace-level generated decls, not
'inject into class X while matching class Y'" — confirmed at the mechanism level, not
just as stated behavior.

### 2.6 `$for` — expression- and statement-position (both landed)

- **Expression-position** (`ExprKind::ForSplice`, Phase 3): iterates an array-valued
  comptime expression, clones its element template once per item
  (`cloneArrayElements`, `Rules.cpp`). `$for f in C.fields.where(...).map(...) : $f.name`
  — the ORM corpus shape (`tests/corpus/meta/rule_attr_values.ext`).
- **Statement-position** (`StmtKind::ForSplice`, item J, **landed** 2026-07-19 alongside
  item A — see `request-metaprog-attr-values.md`'s own status header): repeats whole
  statements (`cloneStmtInto`, the statement-list analogue). Confirmed working, corpus
  `tests/corpus/meta/rule_stmt_for.ext`:

  ```
  rule buildRow {
      match @Row(r) on class C
      inject `Array<string> toRow() {
          Array<string> out = [];
          $for f in C.fields.where((x) => x.hasAttr("Col"))
              : out = out.add($f.name + "=" + this.$f.toString());
          return out;
      }` at member of C
  }
  ```

**What `$for` fundamentally cannot do, by construction:** produce a *fixed-arity* call
with *per-item-conditional* argument placement. It expands to N clones of one template —
each clone is structurally identical up to hole substitution. It cannot decide "if this
method has a handle-typed parameter, put it in slot h1; otherwise leave h1 as 0" within
one generated call, because that's a per-iteration **branch**, not a repeated element.
This is the mechanical content of gap 2 (§6) — not a syntax deficiency, a structural one.
P4 §9.2 (the section the request ticket cites as "deliberately bounds scope") is about
statement-position `$for` specifically, and its exact bounding language is **"no `$if`/
`$while` follows it (that line is where the smell starts)"** — i.e., the owner explicitly
declined to add conditional/loop control flow *inside* templates as a general feature,
for the stated reason that it would turn quasiquote templates into "a full imperative
sublanguage." Gap 2 is asking to cross exactly that line, in miniature (one conditional
per method, not a general `$if`). **The design pass should treat this as "the bounded
line applies and a workaround is wanted," not "the line was never drawn here" — get the
distinction on record rather than re-deriving it.**

### 2.7 Layer D — `rewrites`/`replace`/`$body` (landed, unused by either proposed path)

Fully implemented, `techdesign-metaprog-phase4.md` §14 item 1, corpus green
(`rule_body_replace_arrow`, `rule_timed`, `rule_memoize`). Grammar:

```
rule NAME rewrites body of <bind> {
    match ...
    replace `... $body ...`     // $body splices the ORIGINAL body (statement- or
}                                // expression-position; exactly once, M32 otherwise)
```

Mechanically: resolves the target method **via the match's own bindings** (so the
subject itself, typically), requires it callable-with-a-body, clones the `replace`
template with `$body` bound to a clone of the original body, then **overwrites**
`subject->memberBody` in place (`Rules.cpp` `BodyReplace` branch, `1811-1854`). Two
properties matter for this ticket specifically:

- **It replaces an existing declaration's body — it does not add a new same-named
  member.** `injectMember`'s M33 collision check (§5 below) is never consulted; there is
  nothing to collide with, because no new `Stmt` is added to `cls->body`.
- **The subject can carry its own signature *and* its own per-method attribute data in
  one declaration** — nothing forces signature and generation data apart the way
  `member of` injection does. A rule matching `@extern(op:"setAttribute", returns: V) on
  method m` can `rewrites body of m` using `m`'s own already-declared parameter list.

§9.3 develops this into a third candidate direction not present in the request ticket.

### 2.8 Already-landed capability the request ticket doesn't credit

Two corrections to what the ticket (and the still-open-status headers of its companion
tickets) currently say:

- **Member-selector splice (`this.$f` / `t.$m()`) is landed**, not blocked.
  `request-metaprog-splices.md` item (A) says "Track 03's source read (Rules.cpp:1522–
  1534) says `$f` after `.` is currently treated as an attribute-value reification, not
  a member name — so the second `$for` fails today," and lists it as an open P1 ask.
  That line number now points at different code (the file has grown); the **current**
  `cloneExpr` (`Rules.cpp:2170-2201`) has a dedicated branch — "Decl-hole in member-name
  position: `this.$m` -> splice the selector" — that handles exactly this, including the
  `$for`-bound-meta-value case (a `meta::Method`/`meta::Field` object's `name` field
  spliced as the selector). Confirmed working live: `tests/corpus/meta/rule_stmt_for.ext`
  line 13 uses `this.$f.toString()` and its `.expected` output round-trips correctly.
  This must have landed as an implementation detail of item B (statement-position `$for`)
  — the `toRow()` generator needs it and the commit message (`e3e9432`) bundles "items
  A+B" together. **Any design for this ticket can rely on `this.$m(...)`/`t.$m(...)`
  forwarding calls without asking for anything new.**
- **`distinct` (§4.3 escape, Phase 4)** lets two same-name+same-type members coexist in
  one class when one side is marked `distinct` — not directly useful for "replace the
  hand-written stub with a generated one" (you want one method, not two), but relevant if
  a transitional design wants generated and hand-written versions to coexist temporarily
  for a diff-based acceptance check (the ticket's own acceptance criterion: "diff-match
  the current hand-written stubs").

## 3. The generation target — full `Dom` prelude inventory

Source: `Resolver.cpp:5313-5423` (`kPreludeWasm`), cross-checked against
`hard-06-hostbridge-seam.md`'s native table. Every marshaled call funnels through one of
three natives, arity 5, fixed shape `(op, h0, h1, a, b)`:

```cpp
int    std::sysHostI(string op, int h0, int h1, string a, string b);
string std::sysHostS(string op, int h0, int h1, string a, string b);
void   std::sysHostV(string op, int h0, int h1, string a, string b);
int    std::sysHostCloReg((int) => void cb);   // closure registration, separate seam
string std::sysHostEcho<T>(T v);               // pins only, not a real op
```

Full method-by-method marshaling shape (this table is the ground truth the generated
code must reproduce byte-for-byte to satisfy the ticket's acceptance criterion):

| Declaring type | Method | Params | op string | tag | h0 | h1 | a | b | Shape notes |
|---|---|---|---|---|---|---|---|---|---|
| `Dom` (ns) | `body()` | — | `"documentBody"` | I | `0` | `0` | `""` | `""` | wraps result in `DomNode(...)` |
| `Dom` | `create(string t)` | 1 str | `"createElement"` | I | `0` | `0` | `t` | `""` | wraps `DomNode(...)` |
| `Dom` | `textNode(string s)` | 1 str | `"createTextNode"` | I | `0` | `0` | `s` | `""` | wraps `DomNode(...)` |
| `Dom` | `byId(string id)` | 1 str | `"getElementById"` | I | `0` | `0` | `id` | `""` | wraps `DomNode(...)`; miss ⇒ handle 0 |
| `Dom` | `listenerCount()` | — | `"cloCount"` | I | `0` | `0` | `""` | `""` | leak-pin meter |
| `DomNode` | `setAttr(string name, string value)` | 2 str | `"setAttribute"` | V | `h` | `0` | `name` | `value` | **returns `this`** (fluent — not part of the marshal call) |
| `DomNode` | `attr(string name)` | 1 str | `"getAttribute"` | S | `h` | `0` | `name` | `""` | |
| `DomNode` | `setText(string t)` | 1 str | `"setText"` | V | `h` | `0` | `t` | `""` | returns `this` |
| `DomNode` | `text()` | — | `"getText"` | S | `h` | `0` | `""` | `""` | |
| `DomNode` | `append(DomNode child)` | 1 handle | `"appendChild"` | V | `h` | `child.h` | `""` | `""` | **only method using h1 for a plain param** — the handle-typed param maps to h1, not a/b |
| `DomNode` | `dispatch(string type)` | 1 str | `"dispatchEvent"` | V | `h` | `0` | `type` | `""` | |
| `DomNode` | `release()` | — | `"release"` | V | `h` | `0` | `""` | `""` | |
| `DomNode` | `click()` | — | *(no native call — pure Leviathan)* | — | — | — | — | — | `this.dispatch("click")`; **excluded from generation, it's a convenience wrapper over another generated method** |
| `DomNode` | `exists()` | — | *(no native call)* | — | — | — | — | — | `h != 0`; pure Leviathan, not marshaled |
| `DomNode` | `on`/`off`/`events` | closures/strings | `addEventListener`/`removeEventListener` + `sysHostCloReg` | V (+ separate clo seam) | `h` | `idx` | `type` | `""` | **excluded from v1 bindgen scope by techdesign-06 §1's own scope discipline** ("methods whose params/returns are immediates, strings, or handle wrappers" — callbacks in signatures beyond the event pattern stay hand-written) |
| `DomEvent` | `type()` | — | `"eventType"` | S | `h` | `0` | `""` | `""` | |
| `DomEvent` | `targetValue()` | — | `"eventTargetValue"` | S | `h` | `0` | `""` | `""` | |

**The observable pattern**, if a design wants a positional convention instead of
per-param attribute data: params fill `a`, then `b`, in declaration order **if string-
typed**; a single handle-typed (`DomNode`) param fills `h1`; a zero-param method leaves
`a`/`b`/`h1` at their defaults. This convention covers **100%** of the in-scope surface
above (13 of 17 methods marshal; the other 4 are pure-Leviathan or explicitly
out-of-v1-scope). It is a real, generalizable rule, not a coincidence of a small sample —
but it is exactly the kind of "conditional templating" `$for` can't express directly
(§2.6), because building one call requires *picking* which param (if any) is the handle
one and treating the rest as an ordered string list, not repeating a template per param.

**Return-shape → native tag mapping**, from the same table: `void`-and-`DomNode`-
returning (fluent) methods → `sysHostV`; `string`-returning → `sysHostS`;
`int`/handle-constructing → `sysHostI`. Three-way, not per-return-type-string
computation — small enough to hand as three separate rule instances filtered by
`.where(m.returnType == "...")` rather than needing a single templated dispatch (see
§9.2/§9.3).

## 4. Gap 1 in depth — the declare-vs-generate collision

`injectMember` (`Rules.cpp:2515-2550`) is the M33 confluence check. Verbatim collision
logic:

```cpp
for (const StmtPtr& m : cls->body) {
    if (m->kind != StmtKind::Member) continue;
    if (m->name != member->name) continue;
    if (typeName(m.get()) != typeName(member.get())) continue;  // resolution by type
    if (member->distinct || m->distinct) continue;               // §4.3 escape
    // ... M33 error: "rule '...' injects a member '...' that collides with an
    // existing member of the same type in class '...'"
}
```

If a rule `match on class C` where `C` is `DomNode` (which already hand-declares
`setAttr`), and the rule's `inject ... at member of C` produces a new `DomNode
setAttr(string, string)`, this loop finds the pre-existing hand-written `setAttr` in
`cls->body`, same name + same syntactic return type, and errors — **this is the
"verified live" collision the request ticket reports.** It fires regardless of whether
the pre-existing method is hand-written or itself rule-injected (the `injectedMemberBy_`
map only changes the error's wording, naming both rules when the collision is
rule-vs-rule).

There is no way around this **while matching and injecting the same class** short of:
deleting the hand-written declaration first (not something the rule engine does — it
only adds), or marking one `distinct` (which keeps *both* methods, not what "replace the
hand-written stub with a generated one" wants), or using Layer D to *replace* rather than
add (§2.7 — this is why §9.3 below is a real candidate).

The techdesign-06 §1 original sketch avoided this collision entirely, but only because
it generated a **namespace-level registration call** (`Dom::__import($e.kind, "$C", "$m",
...)`, injected `at namespace DomBindings`) that assumed the real per-method body already
existed hand-written and just needed a registration entry — i.e., it was never actually
generating the marshaling body, only wiring an import table. That model matches the
*old* one-wasm-import-per-method DOM seam, not the shipped `lv.dom_call` reflective seam
— which is gap 2, and which is why the original sketch's rule, even if it worked
mechanically, wouldn't produce what doc 05 as-built actually needs.

## 5. Gap 2 in depth — the target moved

Doc 05 as-built (§9 item 1, `techdesign-05-dom-bridge.md:136-144`) replaced the
per-method-import model the original bindgen sketch assumed with the single reflective
`lv.dom_call`/`std::sysHost{I,S,V}` seam (§3 of this document has the full inventory).
Generating a **faithful** stub body today means synthesizing, per method:

1. **The op string** (`"setAttr"` → `"setAttribute"`) — not derivable from the method
   name by any general rule visible in the table (`setAttr`→`setAttribute`,
   `attr`→`getAttribute`, `text`→`getText`/`setText`, `append`→`appendChild`,
   `dispatch`→`dispatchEvent` — a getter/setter-prefix convention exists but isn't
   uniform enough to auto-derive reliably, e.g. `release`→`"release"` doesn't take a
   prefix at all). This needs either explicit per-method data (an attribute argument) or
   hand-authored derivation logic outside the rule engine's reach.
2. **The native tag** (I/S/V) — derivable from the method's declared return type, a
   simple 3-way `.where()` filter (§3's closing paragraph) — **not** the hard part.
3. **The argument-slot mapping** (which param → `h1` vs `a` vs `b`) — derivable from a
   positional convention (§3), but expressing "if there's a handle-typed param, put it in
   h1, otherwise leave it 0, then fill a/b from the remaining string params in order" as
   ONE generated call is the conditional/branching problem `$for` structurally cannot
   express (§2.6) — this is gap 2's real content, more than #1 or #2.

None of this needs interface reflection or cross-class injection — gap 2 is **orthogonal**
to gap 1. A design that solves gap 1 (however) still needs an answer for gap 2, and a
design that solves gap 2 alone (path (b), below) still needs *some* mechanism to place
the generated body correctly relative to the pre-existing declaration (which re-invokes
gap 1's territory unless it picks Layer D specifically — see §9.3).

## 6. What's still genuinely open (accurate as of this research)

From `request-metaprog-attr-values.md`'s own status header (2026-07-19): **(A) and (B)
landed**; **(C) named attribute args, (D) Layer-D rewrites/$body [now ALSO landed per
Phase 4, this status line is stale], (E) inherited/mixin field visibility, (F)
`meta::Class.attrs`, (G) memberref attribute values** remain open. From
`request-metaprog-splices.md`: **(A) member-selector splice is landed** (§2.8, this
document — the ticket's own status header is stale on this point); **(B) identifier/
type-position splicing of comptime strings + `$if`**, **(C) identifier synthesis
(`$ident(...)`)**, **(E) named splice invocation (`@Name();`)**, **(F) statement-position
grouped attributes** remain open.

Of these, the ones load-bearing for this ticket specifically:

- **(E), extended to methods.** Neither open item (E) (fields) nor anything else asks for
  inherited/interface **method** reflection — this is a genuinely new ask this ticket
  introduces, parallel in shape to (E) but not covered by it. `buildMetaValue`'s
  `decl->body`-only loop (§2.3) would need a base-walking variant (`C.allMethods`, or a
  flag) that also needs (H)-style structured `bases` (`meta::Class.bases` as resolved
  objects, not strings) to walk symbolically rather than by pre-resolved `Symbol*`
  (which `implementsOrExtends`, `Rules.cpp:1339-1346`, already has server-side — the
  gap is surfacing it to `meta.*`, not computing it).
- **(B) identifier-position splicing of comptime strings.** Needed only if a design wants
  to synthesize `std::sysHost$tag(...)` dynamically from a reflected string rather than
  filtering into 2-3 concrete rule instances by return type (§3's closing note says the
  3-way filter avoids needing this — cheaper than asking for (B)).
- **(G) memberref attribute args.** Not needed for this ticket's core ask — op-string and
  return-tag are plain strings/enums, not references to other declarations. Worth noting
  only so the design pass doesn't conflate the two tickets' scope.

## 7. Candidate directions

### 7.1 Path (a) — interface/inherited method reflection + cross-class injection

The request ticket's own option. Concretely needs:

1. `meta::Class.bases : Array<meta::Class>` (resolved, not strings) **or** an equivalent
   accessor — extends item H.
2. `C.methods` (or a new `C.allMethods`) unions in inherited/interface method
   signatures — extends item E from fields to methods.
3. A new anchor or `boundClass` extension that can inject into a class **not** bound by
   the current match — e.g. `member of $C.implementers` (a reverse query nothing
   currently supports) or restructuring the match to bind both the interface and each
   implementer simultaneously (no existing grammar shape does this; `match ... on class C
   : IFace` binds one class per firing, one firing per matching class, not "for the
   interface, for each implementer").

This is the largest lift of the three — two `meta.*` surface extensions plus a genuinely
new injection primitive with no existing partial implementation to build from. It also
still needs gap 2 solved independently (§5) — interface reflection tells you *that* a
method exists and its signature, not what op-string/native-tag/slot-mapping to generate
for it, so path (a) *by itself* only closes gap 1; it still needs either per-method
attribute data (converging with path (b)'s idea) or a derivation convention layered on
top.

### 7.2 Path (b) — DOM-seam redesign: push op/return-tag onto the attribute

The request ticket's other option: `@extern(op: "setAttribute", returns: V)` on each
hand-declared method, generating from `$for` + landed attribute-value reflection (item A)
alone — no interface reflection, no cross-class injection. This is cheaper (item A is
already landed) but:

- **Still needs gap 1 solved for placement.** If the annotated methods are hand-declared
  *with real signatures* in `DomNode`, and the rule also injects a generated body into
  `DomNode`, that's the exact same class matched/injected collision as §4 — **path (b)
  does not automatically avoid gap 1**, contrary to how the request ticket's phrasing
  ("sidestepping the need for interface reflection or cross-class injection entirely")
  could be read. It avoids gap 1's *interface-reflection* half by keeping the signature
  declaration and the generation data on the same declaration — but it still needs an
  injection mechanism that doesn't collide with that same declaration. Two sub-options:
  - (b-i) Keep the annotated methods as **bodyless abstract declarations** (in an
    interface `C` implements, or directly on `C` if the language allows a signature-only
    member) and generate the real definition elsewhere — this reduces to path (a)'s
    placement problem again.
  - (b-ii) Use **Layer D** (`rewrites body of`) instead of `member of` injection — see
    §7.3, this is the actual clean way to make path (b) work without touching gap 1 at
    all.
- **Still needs the arg-slot mapping problem (§5 item 3) solved**, either via a
  positional convention (derivable without new primitives, per §3) or by pushing even
  more data onto the attribute (`@extern(op:"setAttribute", returns:V, h1:"none",
  a:"name", b:"value")` — naming params by string, checkable only loosely, a step away
  from the "typo'd column should be a compile error" spirit the owner has stated
  elsewhere, `request-metaprog-attr-values.md` §5 (G)'s ruling on member references).

### 7.3 Path (c) — NEW, surfaced by this research: Layer D rewrite of hand-declared stubs

Not in the request ticket. Combine path (b)'s attribute-data idea with **`rewrites body
of`** (§2.7) instead of `member of` injection:

```
namespace Dom {
    attribute extern { string op; NativeTag returns; }   // "returns" as an enum/string

    rule bindExternV rewrites body of m {
        match @extern(e) on method m
        where e.returns == "V"
        replace `std::sysHostV("$e.op", h, /* h1 */ 0, /* a */ $p0, /* b */ $p1);`
    }
    // ... bindExternS, bindExternI siblings, filtered by e.returns
}
```

with the DOM classes keeping their **real signatures, hand-declared, bodies now
generated**:

```
class DomNode {
    int h;
    @extern(op: "setAttribute", returns: "V")
    DomNode setAttr(string name, string value) { /* body replaced by rule */ return this; }
    ...
}
```

**Why this avoids both gaps as stated:**

- **Gap 1 doesn't apply.** `rewrites body of m` overwrites `m`'s existing body in place —
  it never adds a new `Stmt` to `cls->body`, so `injectMember`'s M33 collision check
  (§4) is never consulted. No interface split needed; the signature and the generation
  data live on the exact same declaration, in the exact same class, which is fine for
  Layer D (unlike `member of`).
- **Gap 2's op-string/tag problem is solved by attribute data + a 3-way filter**, same as
  path (b) — no identifier-splicing ask needed (§6).
- **Gap 2's arg-slot problem remains** — this path does **not** solve it for free. The
  sketch above hand-waves `$p0`/`$p1` because Layer D's `replace` template still can't
  conditionally pick "the handle param if present, else the first two string params" any
  more than `member of` injection could (§2.6's limitation is about `$for`/template
  structure generally, not the `member of` anchor specifically). This path needs the
  **same** positional-convention-or-explicit-slot-data resolution as path (b) — it only
  removes the placement problem, not the argument-marshaling problem.

**What this path costs that the other two don't need to weigh:** every DOM method needs
a hand-declared signature **and** an `@extern` attribute (methods aren't fully generated
from an interface — they're semi-generated, body-only). This is arguably *closer* to what
"generate the marshaling stubs" means in the original ask (the signature is still
author-controlled, only the marshaling body is mechanical) and sidesteps the biggest
open-ended risk in path (a) (a brand-new cross-class injection primitive with no partial
implementation and real design questions about how a reverse "find implementers" query
composes with the existing one-`Bindings`-map-per-firing model, §2.2). It costs: the
`@extern` attribute duplicates information already implicit in doc 05's hand-written
bodies (the op string), so it's not zero-authoring the way a from-scratch interface-only
generation would be — every method still needs one attribute line, hand-written, forever
(same cost path (b) already accepts).

### 7.4 Comparison

| | Gap 1 | Gap 2 (tag) | Gap 2 (slots) | New primitives needed | Reuses landed-only machinery? |
|---|---|---|---|---|---|
| (a) interface reflection + cross-class inject | Solved (if built) | Not solved | Not solved | 2 `meta.*` extensions + 1 brand-new injection primitive | No |
| (b) attribute-driven, `member of` | **Not actually solved** (placement still collides unless paired with (c)'s mechanism) | Solved | Not solved | 0 (if paired with (c)); else same primitive gap as (a) | Item A only |
| (c) attribute-driven, `rewrites body of` | Solved (Layer D sidesteps it) | Solved | Not solved | 0 | Item A + Layer D, both landed today |

**Gap 2's argument-slot problem is common to all three paths and unsolved by any of
them as sketched** — it needs its own resolution regardless of which placement strategy
wins: either a positional convention (cheap, covers 100% of the current 13-method
surface per §3, but is an implicit convention nothing enforces if a future DOM method
breaks the pattern — e.g. two handle-typed params) or explicit per-param slot attributes
(more verbose, self-documenting, no silent-breakage risk). This choice is orthogonal to
the placement choice above and should be made independently in the design pass.

## 8. Prior art — the closest working precedent

`harpoon`'s `@Test` auto-discovery (`techdesign-unit-test-library.md` §5.2, **landed**)
is the nearest existing rule to this ticket's shape — attribute match + `$for` over
`C.methods` + filtered by `.where()` + `at namespace`:

```
rule discoverTests {
    match on class C
    where C.methods.any((x) => x.hasAttr("Test")) && !C.hasBase("IDisposable")
    inject `bool $C = harpoon::__seed("$C",
        [ $for m in C.methods.where((x) => x.hasAttr("Test") && x.returnType == "void"
                                          && x.arity() == 0 && !x.hasAttr("Skip"))
            : harpoon::__entry("$m", () => { $C t = $C(); t.$m(); }) ],
        ...);`
    at namespace HarpoonReg
}
```

Note this rule **never injects into the matched class** — it only reads `C`'s shape and
writes a namespace-level registry that *calls back into* `C` via `t.$m()` (member-
selector splice, §2.8). This is structurally identical to techdesign-06 §1's original
(now-superseded) sketch, and identical in spirit to why it doesn't have gap 1: it was
never trying to *replace* a method body, only *reference* one. The DOM bindgen's problem
is harder precisely because "generate the marshaling stubs" means replacing bodies, not
referencing them — `@Test` discovery is a good syntax precedent but not a solved-problem
precedent for this ticket.

## 9. Acceptance / verification (restated from the request, for the design doc's use)

- Path (a): generated stubs diff-match doc 05's hand-written `Dom` prelude, full surface.
- Path (b) or (c): the redesigned `@extern` shape + rule generates the same stubs; doc
  05's hand-written stub **bodies** are deleted in favor of generated ones (signatures
  may remain hand-declared under (c)).
- Either way: full differential corpus green (`--run`/`--ir`/`--expand`), wasm lane
  included (`tests/run_wasm_dom.sh`'s five pins: `dom_hello`, `dom_marshal`,
  `dom_strings`, `dom_leak`, `dom_throw`, §3 of `techdesign-05-dom-bridge.md`).
- Not in scope, restated: this ticket does not request the full P4 tail (§9.1-9.8 of
  `techdesign-metaprog-phase4.md`) — only the specific primitives (or attribute-shape
  redesign) that close this differential. Path (c) above notably needs **zero** new
  primitives, which — if the design pass agrees with this research's read — resolves
  the ticket without touching the P4 tail at all.

## 10. Open questions for the design pass

1. **Does path (c) actually work end-to-end**, including the transitional period where
   `DomNode`'s hand-written bodies are deleted and replaced with `@extern`-attributed
   signature-only declarations? (This research reasons from the mechanism; it has not
   built or compiled the rule.) Concretely: does a method whose body is *only* ever
   filled by a `rewrites` rule parse/resolve cleanly with an empty or placeholder body
   before the rule stage runs (pass 1), or does the language require a body at parse
   time that Layer D then discards? Needs a spike, not more reading.
2. **Positional convention vs. explicit slot attributes for gap 2's arg-slot problem**
   (§7.4's closing paragraph) — pick one; both are viable, this document intentionally
   doesn't decide.
3. **`returns` as a 3-way filter vs. identifier-splicing** — the 3-rule-filter approach
   (§3, §7.3) needs no new engine capability; confirm the design pass is comfortable with
   3 near-duplicate rule bodies (`bindExternV`/`S`/`I`) rather than requesting item (B) of
   `request-metaprog-splices.md` (identifier-position string splicing) to collapse them
   to one. Given (B) is unbuilt and this ticket's own framing prefers the cheaper path,
   the filter approach is the default recommendation of this research, not a requirement.
4. **If path (a) is chosen anyway** (e.g. because the design pass wants
   fully-interface-generated methods with zero hand-declared signatures, a stronger
   "faithful bindgen" than (c) provides): the cross-class injection primitive needs its
   own mini-design — at minimum, decide whether it's a new anchor spelling
   (`member of <expr>` where `<expr>` can name something outside `Bindings`) or a new
   match shape (bind interface + implementer in one firing). Out of scope for this
   research to resolve; flagged so the design doc doesn't treat it as a two-line change.

## 11. Source map

| Topic | File:line |
|---|---|
| `injectMember` / M33 collision | `src/Rules.cpp:2515-2550` |
| `boundClass` | `src/Rules.cpp:2427-2432` |
| `AnchorKind` enum | `src/Ast.hpp:293-302` |
| `expand()` per-anchor dispatch | `src/Rules.cpp:1710-1864` |
| `buildMetaValue` (meta.* schema) | `src/Rules.cpp:1378-1486` |
| `tryMatch` (subject/encloser/where) | `src/Rules.cpp:1502-1598` |
| `declKindMatches` (subject exactness) | `src/Rules.cpp:1317-1326` |
| `implementsOrExtends` | `src/Rules.cpp:1339-1346` |
| member-selector splice (`this.$m`) | `src/Rules.cpp:2170-2201` |
| Layer D `BodyReplace` expansion | `src/Rules.cpp:1811-1854` |
| `Bindings`/`Binding` struct | `src/Rules.hpp:187-201` |
| `kPreludeWasm` (`Dom` surface) | `src/Resolver.cpp:5313-5423` |
| DOM native seam (`sysHost*`) | `designs/complete/hard-06-hostbridge-seam.md` |
| Statement-`$for` bounding quote | `designs/complete/techdesign-metaprog-phase4.md` §9.2 |
| Layer D landed status | `designs/complete/techdesign-metaprog-phase4.md` §14 item 1 |
| `@Test` discovery precedent | `designs/complete/techdesign-unit-test-library.md` §5.2 |
| Working corpus examples | `tests/corpus/meta/rule_{orm,attr_values,stmt_for}.ext` |
