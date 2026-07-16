# Atlantis Track 03 — Serialization & DTOs

**Status:** draft for owner review. **Date:** 2026-07-06.
**Depends on:** Track 09 (`json::JsonValue` — THE interchange type per C7), metaprogramming
Phases 1–3 (typed attributes, rules, `$for`, `where`, anchors), Track 01 (C4 exception
types, consumed not owned), `designs/request-metaprog-attr-values.md` (LA-4/6 — designed
assuming it lands, R4).
**Owns:** `Atlantis::Json` (negotiation, policy helpers), plus — per C1 (all attributes and
rules live in namespace `Atlantis`) — the `Serializable`/`JsonIgnore` attributes, the
`@Serializable` rule set, the `ISerializable` facade interface, and the `__atl*`
splice-facing helper functions in `Atlantis` root (§5.3 explains why root).
**Conforms to:** overview rulings R1–R9 and contracts C1–C8, especially **C7** (member
`JsonValue toJson()` + labeled constructor `new FromJson(JsonValue v)`; JSON keys = field
names verbatim until LA-4) and **H-8/Track 09 §6#3** (serializers always build fresh trees).

---

## 0. Mission, scope, non-goals

**Mission.** Every DTO and entity that crosses the JSON boundary gets a compile-time-derived
`toJson()`/`FromJson` pair from one attribute — zero runtime reflection, all generated code
visible under `--expand`, errors flowing through the one error model (C4). Plus the content
negotiation seam Tracks 01/02 consume.

**In scope:**
1. The `@Serializable` rule set generating the C7 pair for value structs (request/response
   DTOs — the primary case) and reference classes (entities).
2. Field-type dispatch, optionals, `DateTime`, `@JsonIgnore`, nested serializable types,
   `Array<T>`/`Map<string,T>`; explicit v1 refusals with loud diagnostics.
3. Unknown-key / missing-key policies; the 400-vs-422 split; aliasing discipline.
4. Accept-header content negotiation; 415/406 stances.
5. DTO conventions for the blessed scaffold (`app/models/dtos/requests|responses`).

**Non-goals:** JSON parsing/rendering itself (Track 09 owns `json`); validation attribute
semantics (`@Required` etc. — Track 02; §8 defines only the JSON-side hookpoints);
`application/problem+json` rendering (Track 01's error path — we only *select* it);
views/HTML rendering (Track 09-views); binary bodies (LA-3); XML/msgpack (never in v1).

---

## 1. Ground truth: what the splice engine can and cannot do (verified against source)

This track's whole design hangs on the exact semantics of three substitution paths in
`src/Rules.cpp` (read 2026-07-06; treat as the authoritative reading until P-probes confirm
at runtime — framework tracks never edit these files, STOP (b)):

1. **Value-hole field fold** (`Rules.cpp:1477–1493`): `$x.field` where `x` is a binding
   with `hasVal` and an Object value — reads the object's field and **reifies it as a
   literal**. This is how `$f.name` / `$f.type` become string literals inside a `$for`
   element template (proven end-to-end by `tests/corpus/meta/rule_orm.ext`).
2. **Decl-hole member-name splice** (`Rules.cpp:1522–1534`): `this.$m` where `m` is a
   binding with `declStmt` — splices the decl's selector text as the member name (proven by
   `rule_forward_args`'s `this.$m($_args)`). **If the binding is value-only, this path
   errors: `'$f' is an attribute value, not a member name`** (line 1528–1530).
3. **Bare name-hole** (`Rules.cpp:1536–1557`): decl binding → name; primitive value →
   reified literal; Object value → error "splice a field".

And four match-engine facts:

- The `@Attr` clause of `match` is **optional** (`Parser.cpp:1167–1188`,
  `Rules.cpp:1105`): `match on field f in class C : ISerializable` parses and matches.
  Encloser kind `class` also matches `struct` ancestors (`Rules.cpp:1152–1155`), and the
  `: Type` constraint resolves through `implementsOrExtends`.
- A `$for` loop variable binds **value-only** (`Rules.cpp:1464–1467`: `Binding{val=item}`,
  no `declStmt`). A per-field rule's subject binds **decl-only** (`Rules.cpp:1137–1142`:
  `declStmt` + `selectorText`, no `val`).
- `meta::Class` exposes `name/bases/fields/methods` — **no `attrs`**
  (`Resolver.cpp:1234–1255`, `Rules.cpp:1011–1035`). A rule can test `C.hasBase("X")` in
  `where`, but **cannot see the enclosing class's own attributes**.
- `meta::Class.fields` walks the class's **own body only** (`Rules.cpp:1029–1033`) —
  inherited/mixin fields are invisible to `$for`.

**Consequences (the capability matrix this design is built on):**

| Want | `$for` template (class-level rule) | per-field rule template |
|---|---|---|
| field name as string literal | YES (`$f.name`, proven) | NO (`$f.name` clones to a runtime `.name` access on the field — wrong; §11#4) |
| `this.<field>` member access | **P-probe #1a** — expected NO per path 2's exact diagnostic | YES (`this.$f`, path 2, proven cousin `this.$m`) |
| statements (assignments) | NO (`$for` is array-literal-only, LA-6) | YES, but only at ctor/body/marker anchors of the *subject* — for a field subject, only `bottom of C.constructor` (markers/bodies need a callable subject, M26/M27) |

One language fact that kills a whole family of designs: **structs are value types and
closures snapshot `this` by value** (reference.md:314–316; `Lower.cpp` MakeClosure
snapshots the receiver). A registry of getter/setter lambdas built at construction time
captures a *copy* of the struct — getters go stale, setters mutate a ghost. Closure
registries are sound for reference classes only; DTOs are structs (R2). Therefore **no
closure-registry mechanism can be the primary design.**

**Why reference classes are fine despite comptime non-reifiability (task-mandated
verification):** M28/§11 of phase 3 limits *reifying class values out of comptime
evaluation* (`comptime C x = f()` — permanently refused). This track never does that: the
only values crossing the comptime boundary are `meta::Field` **strings** (`$f.name`,
`$f.type` — `VKind::Str`, always reifiable) and decl selector texts. Everything generated
is ordinary **runtime** code compiled by pass 2. Reification limits do not bite; entities
get the identical rule set.

---

## 2. The generated contract and user-facing convention

### 2.1 What the user writes

```
uses Atlantis;                                   // opts the file into the rules (C1)

@Serializable
struct CreateUserRequest : ISerializable {       // DTO: value struct (R2)
    string email;
    string fullName;
    int? age;                                    // optional -> JSON null / absent OK
    @JsonIgnore string csrfShadow;               // never serialized, never bound
}

@Serializable
class User : DbTracking, Timestamps, ISerializable {   // entity: reference class (R2)
    int id;
    string email;
    DateTime createdAt;                          // iso8601 on the wire
}
```

Both markers are load-bearing in v1 and the pairing is enforced (§5.2):

- **`@Serializable`** (C5: `Serializable {}`) drives the class-level generation rules and
  is where future args land (naming policy, strict mode) once LA-7 named args exist.
- **`: ISerializable`** — `interface ISerializable { JsonValue toJson(); }` in `Atlantis`
  root. It is required because (a) per-field rules have no other way to gate on the
  enclosing class (`meta::Class` has no attrs — §1), (b) it gives nested-type dispatch one
  overload (`__atlEnc(ISerializable v) => v.toJson()`), and (c) it turns "file forgot
  `uses Atlantis`" into a crisp pass-2 error ("`User` does not implement `toJson`") instead
  of silent non-generation. Structs may implement interfaces (reference.md:316), so this
  works for DTOs. LA-16 (§14) would let the interface become optional later; it stays the
  contract for dispatch regardless.

### 2.2 What the rules generate (per C7)

| Member | Visibility | Semantics |
|---|---|---|
| `JsonValue toJson()` | public (the C7 surface) | fresh `JsonValue` object tree; keys = non-ignored field names in declaration order |
| `new FromJson(JsonValue v)` | public labeled ctor (C7) | binds fields from `v`; unknown keys ignored (§6.3); missing required key / wrong type → `ValidationException` (§6.4) |
| `Array<string> __atlKeys()` | generated, internal-by-convention | non-ignored field names, declaration order — the single point where LA-4 aliases plug in |
| `JsonValue? __atlSrc; int __atlIdx;` | generated fields, internal | FromJson plumbing (v1 interim only, deleted post-LA-6; §4.4) |

`__atl*` names are reserved: the guard rule (§5.2) rejects user fields starting `__atl`.

---

## 3. Mechanism A — `toJson` via `$for` (primary shape; gated on P-probe #1a)

The clean shape, one class-level rule, everything in array-literal position (legal today):

```
// namespace Atlantis (rules live in the root namespace, C1)
rule serializableToJson {
    match @Serializable on class C
    where C.hasBase("ISerializable")
          && C.methods.where((m) => m.name == "toJson").length() == 0   // §3.3 escape hatch
    inject `JsonValue toJson() {
        Array<string> ks = this.__atlKeys();
        Array<JsonValue> vs = [ $for f in C.fields.where((x) => !x.hasAttr("JsonIgnore"))
                                  : Atlantis::__atlEnc(this.$f) ];
        return Atlantis::__atlZipObj(ks, vs);
    }` at member of C
}

rule serializableKeys {
    match @Serializable on class C
    where C.hasBase("ISerializable")
    inject `Array<string> __atlKeys() =>
        [ $for f in C.fields.where((x) => !x.hasAttr("JsonIgnore")) : $f.name ];`
       at member of C
}
```

Two `$for`s over the **same filtered iterator** → same order → `__atlZipObj` pairs
`ks[i]`/`vs[i]` into a fresh `JsonValue::ofObject` (insertion-ordered map, Track 09 §1.1 —
key order = declaration order, stable for golden tests).

**The load-bearing unknown is exactly one atom: `this.$f`.** Everything else in this
template is proven (`$f.name` reification: `rule_orm`; `member of C`; `where` over
`meta.*`). Per §1 path 2, the source reading says `this.$f` with a value-bound `$f` fails
with *"'$f' is an attribute value, not a member name"*. **P-probe #1a runs it anyway**
(source reading is not a semantics proof, and the probe transcript is the artifact the
language ask cites).

### 3.1 If P-probe #1a passes

Mechanism A ships for structs and classes; `FromJson` still uses Mechanism B (assignments
are statements — `$for` cannot emit them regardless of #1a; §4). Done.

### 3.2 If P-probe #1a fails (expected)

There is **no sound today-shape for struct `toJson`** — the blocked primitive is
"per-field expressions evaluated at call time inside one member body", and:
per-field rules only reach ctor anchors (ctor-time evaluation = stale values); closure
registries capture struct copies (§1); member-name synthesis per field would collide or
needs decl-name holes that don't exist. This is not a workaround gap, it is a language
gap → per R4/R9, **file the ask, design assuming it lands**:

- **LA-15 (new, P1 for this track):** value-bound member-selector splice — when a
  member-name hole's binding is value-only and the value is a `meta::Field` object (or a
  plain string), splice its `name` as the selector. Surgical: the `Rules.cpp:1528` branch
  gains a value case (~6 lines). **Alternative that also closes it:** LA-6
  statement-position `$for` (already ticketed in `request-metaprog-attr-values.md` item B)
  — this track asks the owner to raise item B from P2 to **P1** and treats either landing
  as the unblock. §14 has the register-ready text.

**Interim until the ask lands** (weeks, per the owner's R4 posture — not months):
- Structs and classes: the **escape hatch** (§3.3) — hand-write `toJson()`; generation of
  `FromJson`/keys still applies. The demo app hand-writes its 2–3 DTO `toJson()`s
  temporarily; they are deleted the day the ask lands.
- We do **not** ship a classes-only closure registry: it is unsound for structs, unsound
  even for classes constructed via the implicit nullary path (registry statements live in
  *declared* ctors only), and would be a second serialization model (STOP (c)).

### 3.3 Escape hatch (also the collision policy)

The `where` clause `C.methods.where((m) => m.name == "toJson").length() == 0` suppresses
generation when the user hand-writes `toJson` — deliberate override, not an error (the
member-of conflict check would otherwise hard-error on name+type collision). Same guard on
`FromJson` generation (`C.methods` reflects ctors as `meta::Method`s — probe P-6 confirms
labeled-ctor visibility/naming in `meta.*`; if ctors aren't visible there, the guard drops
and a hand-written `FromJson` is simply a rule-conflict error with a clear message —
acceptable, logged).

---

## 4. Mechanism B — `FromJson` via per-field ctor-append (works today)

Assignments are statements; array-literal `$for` can never emit them (LA-6). But a labeled
constructor **is a constructor**, and `bottom of C.constructor` appends to every declared
ctor — and a per-field rule's decl-bound `$f` **can** splice `this.$f`. The design joins
"which key" (class-level `$for`, declaration order) to "which field" (per-field firings,
also declaration order — single rule, decls iterate in declaration order,
`Rules.cpp:1195`) **by runtime index**, because per-field templates cannot reify the field
name (§1 matrix).

### 4.1 The rule set (declaration order in framework source is load-bearing)

```
// R1 — skeleton: fields + FromJson shell (fires once per class)
rule serializableFromJson {
    match @Serializable on class C
    where C.hasBase("ISerializable")                     // + §3.3 guard for FromJson
    inject `JsonValue? __atlSrc;` at member of C
    inject `int __atlIdx;`        at member of C
    inject `new FromJson(JsonValue v) {
        Atlantis::__atlRequireObject(v);                 // wrong shape -> ValidationException
        this.__atlSrc = v;
        this.__atlIdx = 0;
    }` at member of C
}

// R2 — per-field apply (fires once per field, declaration order; ONE rule so order holds)
rule serializableApply {
    match on field f in class C : ISerializable          // attr-less match (§1)
    where !f.hasAttr("JsonIgnore")
    inject `this.$f = Atlantis::__atlDec(this.__atlSrc, this.__atlKeys(), this.__atlIdx, this.$f);
            this.__atlIdx = this.__atlIdx + 1;`
        at bottom of C.constructor
}

// R3 — epilogue: drop the parse tree so DTO copies don't drag it (declared after R2)
rule serializableSeal {
    match @Serializable on class C
    where C.hasBase("ISerializable")
    inject `this.__atlSrc = None;` at bottom of C.constructor
}
```

Execution order: R1's template body runs first (it *is* the ctor body), R2's appends land
after it in field order, R3's append lands last (ctor-anchor accumulation is rule-order —
techdesign-metaprogramming §5.6; probe P-2 pins it).

### 4.2 Why appending to ALL declared ctors is correct

R2/R3 statements reference only `this` and `Atlantis`-root helpers — they compile in any
ctor. In a user-declared entity ctor, `this.__atlSrc` is `None` (field default), and
`__atlDec(None, …, witness)` **returns the witness unchanged** — a no-op pass over already
constructed fields (they run at ctor *bottom*, after user code). The implicit nullary
construction path is guaranteed by the language independent of declared ctors (info.md §3,
techdesign-metaprogramming §5.6) and runs no appended statements at all — default-built
DTOs pay nothing. Cost of the no-op pass in user ctors: n small calls — an accepted,
logged interim deviation from cost-identity, deleted post-LA-6 (§4.4).

### 4.3 Witness dispatch — `__atlDec`

Return-type-directed extraction is impossible with overloads, so the field's **current
value is passed as a witness** and overloads dispatch on it, returning the same type:

```
// namespace Atlantis (root — §5.3)
int      __atlDec(JsonValue? src, Array<string> keys, int idx, int      witness) { … }
int?     __atlDec(JsonValue? src, Array<string> keys, int idx, int?     witness) { … }
string   __atlDec(…, string witness) { … }     // + float/float?/bool/bool?/string?
DateTime __atlDec(…, DateTime witness) { … }   // iso8601 parse, §6.2  (+ DateTime?)
JsonValue __atlDec(…, JsonValue witness) { … } // raw passthrough field
```

Shared body shape: `src == None` → return witness (no-op pass, §4.2); key = `keys[idx]`;
`src.atOrNone(key) == None` → **optional witness: `None`; required witness: throw
`ValidationException(key, "missing")`** (§6.4); kind mismatch → `ValidationException(key,
"expected <type>")`; int overloads reject non-integral numbers (§6.6).

**Nested serializable types** get per-type overloads injected by a fourth rule at a
namespace anchor (reopen semantics, phase 3 §8.3), using decl-hole type/name splices
(`$C` in type position and `$C::FromJson` — probe P-5):

```
rule serializableNested {
    match @Serializable on class C
    where C.hasBase("ISerializable")
    inject `$C __atlDec(JsonValue? src, Array<string> keys, int idx, $C witness) {
        if (src == None) { return witness; }
        JsonValue? item = src.atOrNone(keys[idx]);
        if (item == None) { Atlantis::__atlMissing(keys[idx]); }
        return $C::FromJson(item);
    }
    $C? __atlDec(JsonValue? src, Array<string> keys, int idx, $C? witness) { …null/absent -> None… }`
       at namespace Atlantis
}
```

Design rule for every template in this track: **templates name only `Atlantis`-root
helpers, `this`, and holes** — all real logic (throwing C4 exceptions, JSON kind checks)
lives in hand-written framework functions whose own file has proper imports. This keeps
def-site qualification single-level (§5.3) and makes templates trivially auditable under
`--expand`.

### 4.4 The post-LA collapse

When LA-6 (statement `$for`) — or LA-15 + LA-6 — lands, R1/R2/R3 and the
`__atlSrc`/`__atlIdx` fields are **deleted** and `FromJson` becomes one straight-line
template (direct per-field assignments with reified key strings, error accumulation per
§6.4). `toJson` likewise collapses to Mechanism A. The public surface (`toJson`,
`FromJson`, key order, policies) is **identical before and after** — the swap is invisible
to app code; acceptance M5 pins byte-equivalence to a hand-written twin (the `rule_orm`
discipline).

---

## 5. Type dispatch: overloads, not type-string interpretation

### 5.1 The decision

The task's naive shape — map `$f.type` strings to serializer expressions at rule time —
is not expressible today (one `$for` element template cannot vary per element; per-field
rules would need one rule per type family, which breaks §4.1's single-rule ordering).
It is also the *worse* design: string-matching canonical type spellings reimplements the
checker badly. **v1 dispatches by overload resolution on the field's static type** — the
language's own mechanism, with errors anchored at the offending field's generated code.
`$f.type` strings are used only where they are the right tool: the guard rule's refusal
diagnostics (§5.2).

The encode surface (all fresh trees, §6.5):

```
// namespace Atlantis (root)
JsonValue __atlEnc(int v);        JsonValue __atlEnc(int? v);      // null <- None
JsonValue __atlEnc(float v);      JsonValue __atlEnc(float? v);
JsonValue __atlEnc(bool v);       JsonValue __atlEnc(bool? v);
JsonValue __atlEnc(string v);     JsonValue __atlEnc(string? v);
JsonValue __atlEnc(DateTime v);   JsonValue __atlEnc(DateTime? v); // iso8601 string
JsonValue __atlEnc(JsonValue v);                                   // embed (§6.5 caveat)
JsonValue __atlEnc(ISerializable v) => v.toJson();                 // every nested type
JsonValue __atlEnc<T>(Array<T> v);        // ofArray, recursive __atlEnc per item — P-4
JsonValue __atlEnc<T>(Map<string, T> v);  // ofObject, recursive per value — P-4
JsonValue __atlZipObj(Array<string> ks, Array<JsonValue> vs);
```

Overload risks are probed, not assumed: `T` vs `T?` overload coexistence/ranking (P-3) and
recursive overload resolution inside generic bodies (P-4). P-3 failing is a STOP (§13) —
optional handling is contract-level. P-4 failing degrades v1 to monomorphic
`Array<int|float|bool|string>`/`Map<string,string>` overloads plus refusal of nested
containers (logged, LA'd) without changing any other section.

### 5.2 What v1 refuses, and the loud-diagnostic idiom

Refused field types (guard-checked as canonical strings): `Map<K,·>` with `K != string`
(JSON object keys are strings); function-typed fields (`(…) => …`); union-typed fields
other than `T?` (no discriminant convention in v1 — roadmap); HKT-typed fields; fields
named `__atl*`. Also refused pairings: `@Serializable` without `: ISerializable`.

Rules cannot emit custom diagnostics — but a `where` clause that **throws** surfaces as an
engine error carrying the message (M20 path, `Rules.cpp:1171–1186`: *"rule '…'
where-clause evaluation failed: <err>"*). v1 uses this as the diagnostic channel — a
**sentinel rule** whose predicate validates and never fires:

```
rule serializableRefuse {
    match @Serializable on class C
    where Atlantis::__atlGuard(C) && false      // __atlGuard: true, or throws with the
    inject `0;` at bottom of C.constructor      //   precise field/type/fix message
}
// e.g. thrown message: "@Serializable User: field 'tags' has type 'Map<int,string>' —
// JSON object keys must be strings; use Map<string,…> or @JsonIgnore."
```

`__atlGuard` is ordinary comptime code over `meta::Class` (hermetic, pure string checks).
What it *cannot* check — whether an unknown field type implements `ISerializable`
(comptime cannot resolve types by name) — falls through to pass-2 overload-resolution
failure on `__atlEnc`/`__atlDec`: still compile-time, still anchored at the field's
generated line under `--expand`, just less pretty. Documented in the user guide.

### 5.3 Why the splice-facing helpers live in `Atlantis` root

Def-site qualification (phase 3 §10) qualifies a template's free names against the rule's
**own** namespace — the rules live in `Atlantis` (C1), so `__atlEnc` resolving in
`Atlantis` root becomes `Atlantis::__atlEnc` in every injected clone: a **single-level**
qualified path. Nested paths (`Atlantis::Json::enc`) would both miss def-site
qualification and walk straight into the open nested-namespace qualified-path resolution
bug (bug.md; noted at `Resolver.cpp:1257ff`). So: `__atl*` helpers, `ISerializable`, and
the attributes sit in `Atlantis` root (facade types per C1); the curated *user-facing* API
(negotiation §7, policy docs) is `Atlantis::Json`. Users never type `__atl*` names.

---

## 6. Policies

1. **Optionals (`T?`).** Encode: `None` → JSON `null`. Decode: JSON `null` **or absent
   key** → `None`. Both directions symmetric; `null` for a *required* field is a
   `ValidationException` ("expected <type>, got null") — null and absent are equivalent
   only for optionals.
2. **DateTime ↔ iso8601.** Encode `d.iso8601()` (Track 09 F2: `Z`-normalized UTC). Decode
   `DateTime::parseIso8601` (accepts `Z`/`±hh:mm`, normalizes UTC); `None` result →
   `ValidationException(key, "expected iso8601 timestamp")`. Epoch-number timestamps are
   not accepted in v1 (one wire format; revisit on demand).
3. **Unknown keys on FromJson: IGNORED.** Justification: API evolution (a newer server
   adding response fields must not break older clients binding the same DTO shape);
   mainstream default (ASP.NET, serde, Jackson default); and mechanically free — v1 reads
   only known keys. A strict mode is a `@Serializable` arg once LA-7 named args land
   (roadmap, not v1).
4. **Missing keys.** Optional field → `None`. Required field → **throw
   `ValidationException`** (the C4 type: 422, carries field errors) naming the key.
   Justification: silently defaulting a missing required primitive to `0`/`""` masks
   client bugs — fail loud; and inventing a Json-local exception would be a second error
   channel (STOP (c)) — Track 03 throws C4's types directly (same package). v1 is
   fail-fast on the first bad field (the statement-per-field shape forces it); post-LA-6
   direct codegen upgrades to accumulate-all-field-errors in one 422 (logged as a designed
   improvement, not a behavior contract — Track 02's validation layer is the primary
   multi-error reporter).
   **400 vs 422 split:** malformed JSON *syntax* (`json::parse` → `None`) is the
   transport's fault → 400 (Track 02's binder maps it); well-formed JSON with the wrong
   *shape/types* → 422 `ValidationException` from `FromJson`. Wrong top-level kind (array
   where object expected) is shape → 422 via `__atlRequireObject`.
5. **Aliasing/depth discipline (H-8, Track 09 §6#3).** `toJson` always constructs a fresh
   tree (`ofObject`/`ofArray`/`of*` builders); serializers never hand out references to
   retained internals, and `FromJson` never retains `v` (R3 clears `__atlSrc`). One
   documented exception: a `JsonValue`-typed *field* is embedded as-is on encode (its
   subtree aliases the field) — documented with Track 09's own aliasing caveat; flips to
   `deepCopy()` when Track 09 adds it. Depth: generated code recurses only per nested
   *static* type (finite); runtime depth limits belong to `json::parse` (cap 128).
6. **Int precision.** JSON numbers are IEEE doubles (Track 09 §1.2): encode documents
   exactness only to ±2^53; decode of a non-integral number into `int` throws
   `ValidationException(key, "expected integer")` — checked in `__atlDec(…, int)`.
7. **`@JsonIgnore`** (C5): excluded from keys, `toJson`, and `FromJson` (field keeps its
   default) — pure `hasAttr` name checks, fully functional today (no LA-4 needed).
8. **Aliases (`@Json("alias")`)**: keys are field names verbatim until LA-4 lands (C7).
   The alias then plugs into exactly one place — the `__atlKeys()` `$for` element becomes
   `($f.attr("Json").argStr(0) ?? $f.name)` — and every index-join consumer (§4) is
   untouched. This is a deliberate property of keying everything off `__atlKeys()`.

---

## 7. Content negotiation (consumed by Tracks 01/02)

```
namespace Atlantis::Json {
    struct MediaRange { string type; string sub; float q; }        // parsed Accept entry
    Array<MediaRange> parseAccept(string header);                  // tolerant; no regex (LA-13)
    // Pick the best producible representation. None => caller answers 406.
    string? negotiate(string? acceptHeader, Array<string> producible);
    bool isJsonContent(HeaderMap h);   // Content-Type gate for FromJson binding (415)
}
```

- **Parsing:** split on `,`; each range splits on `;` for params; `q=` parsed strictly to
  float in `[0,1]` (malformed `q` → 1.0); malformed ranges are skipped; an absent or empty
  `Accept` behaves as `*/*` (RFC 9110). String ops only — no regex.
- **Selection:** filter producible types against ranges (exact > `type/*` > `*/*`), rank
  by q desc, then specificity desc, then **producible-array order** (server preference —
  callers list `application/json` first). `q=0` excludes.
- **The producible set is the caller's:** JSON endpoints pass
  `["application/json"]`; controllers with views (Track 09-views) pass
  `["text/html", "application/json"]` and dispatch on the result — this track selects,
  never renders HTML. `application/problem+json` is Track 01's error renderer; its
  error-path bodies deliberately ignore Accept (RFC 9457 §3 practice).
- **406 stance:** `negotiate(…) == None` → throw `HttpException(406, "not acceptable")`
  (C4 maps it); body is Track 01's problem+json regardless of Accept. Kept rare by
  wildcard handling — only an explicit exclusion earns a 406.
- **415 stance:** endpoints that JSON-bind a request DTO require
  `Content-Type: application/json` or any `+json` suffix type; `charset` param tolerated
  but must be utf-8 if present; a nonempty body with a missing or different Content-Type →
  throw `HttpException(415)`. GET/DELETE with no body skip the check. Enforced by Track
  02's binder calling `isJsonContent` — this track owns the predicate, not the middleware.

---

## 8. DTO conventions & the scaffold flow

Blessed layout (overview §4): `app/models/dtos/requests/*.lev`,
`app/models/dtos/responses/*.lev`; every DTO is a **value struct**, `@Serializable`,
`: ISerializable`, with Track 02 validation attributes stacked on the same fields:

```
@Serializable
struct CreateUserRequest : ISerializable {
    @Required @Email string email;
    @Required @MaxLen(80) string fullName;
    int? age;
}
```

**The request flow (JSON-side hookpoints are steps 2–3; Track 02 owns 1, 4, 5):**
1. Binder checks `isJsonContent` → else 415 (§7).
2. `json::parse(body)` → `None` ⇒ 400 bad request (§6.4 split).
3. `CreateUserRequest::FromJson(v)` — generated; throws 422 `ValidationException` on
   shape/type/missing-required errors.
4. Track 02 validation attributes run over the constructed DTO (`@Required` on a `string`
   distinguishes "present but empty"; `FromJson` only guarantees type-correct presence).
5. Handler receives the typed DTO; response DTO's `toJson()` renders via the negotiated
   type (§7); entities are mapped to response DTOs first — entities' mixin/inherited
   fields (invisible to `meta::Class.fields`, §1) is one more reason the entity→DTO
   mapping step is the blessed pattern, not serializing entities raw (§11#6).

---

## 9. vs Loom (R1)

Loom's draft promised `@Serialize` as a keyword-wish — pre-mature-metaprogramming, zero
mechanism, structs-only data model, no negotiation, no error-model integration. Atlantis
specifies the real thing, and is concretely better where it counts:

- **A mechanism that exists:** every splice in §3–§4 is either corpus-proven or carries a
  named P-probe with the exact engine line it depends on. The design degrades explicitly
  (escape hatch + P1 ask) instead of hand-waving.
- **Compile-time generated, greppable:** `--expand` shows the exact `toJson`/`FromJson`
  bodies per class; no runtime walk, no reflection tables, cost-identical to hand-written
  post-LA (deviations in v1 are enumerated and deleted, §4.4).
- **One error model:** binding failures are C4 `ValidationException`s → 422 with field
  errors; Loom had no story.
- **Entities vs DTOs (R2):** reference-class entities + value-struct DTOs each get the
  same derive surface; Loom's structs-only model couldn't express change-tracked entities
  at all.
- **Refusal over surprise:** unsupported shapes fail at expand/compile time with the
  offending field named (§5.2) — not at first request.

---

## 10. P-probes (run before any feature work; results → Implementation log)

Tiny `.lev` programs under `packages/atlantis/tests/probe/`, run via
`build/leviathan --expand` + `--run`/`--ir`/LLVM. Probes needing only the splice engine
run **now** (no Track 09 dependency — dummy types stand in for JsonValue).

| # | Probe | Expected (per source reading) | Gates |
|---|---|---|---|
| **P-1a** | `this.$f` inside a `$for` element template (Mechanism A) | **FAIL**: "'$f' is an attribute value, not a member name" (Rules.cpp:1528–1530) | §3 path choice; fail ⇒ file LA-15 + escalate LA-6 to P1, ship §3.2 interim |
| **P-1b** | per-field rule (attr-less, `in class C : IFace`) splicing `this.$f` into `bottom of C.constructor`, incl. on a **struct** | PASS (decl-hole path; `this.$m` cousin proven; mutating-in-ctor legal) | Mechanism B. **P-1a AND P-1b both failing = STOP** (§13) |
| **P-2** | ctor-append ordering: R1-template-body → R2 per-field (field order) → R3, all into the same ctor; AND R2 seeing R1's rule-injected `new FromJson` ctor | PASS (rule-order accumulation; expansion reads the class body live) | §4.1/§4.2; FromJson-visibility fail ⇒ fallback: user declares the one-line `new FromJson(JsonValue v) { }` shell, rules fill it (logged, not a STOP) |
| **P-3** | overload coexistence + ranking of `f(int)` vs `f(int?)`, args of both types | UNKNOWN — checker behavior | optional handling (§6.1). Fail = STOP |
| **P-4** | generic `__atlEnc<T>(Array<T>)` body calling overloaded `__atlEnc(item)`, instantiated at `int`/`string`/nested-serializable | UNKNOWN — per-instantiation vs definition-checked generics | container support; fail ⇒ monomorphic fallback (§5.1), logged |
| **P-5** | `$C` decl-hole in type position (param/return) and `$C::FromJson(x)` call, injected `at namespace Atlantis` | plausible (decl-hole-in-expression is spec'd; type-position + labeled-ctor path unproven) | nested decode (§4.3); fail ⇒ v1 refuses nested serializable on decode only (guard message), LA note |
| **P-6** | labeled ctors' visibility in `meta::Class.methods` (name spelling) | UNKNOWN | §3.3 FromJson escape-hatch guard only |
| **P-7** | struct closure capturing `this` — assign in lambda, observe original | copy semantics (stale) — confirming §1's kill of registries | documentation-grade certainty |
| **P-8** | negative confirmation: no rule syntax adds a base/interface to a matched type (nearest spellings fail to parse) | FAIL-to-parse (no anchor targets a base list — §16#2) | §16 fallback stands; documents the gap for the implies-base ask |

Any probe failing with a metaprog-shaped cause that contradicts the phase-3 doc → `/bug.md`
with repro + proposed ruling (H-2 discipline), never hacked around.

---

## 11. Foreseeable problems

| # | Problem | Strategy |
|---|---|---|
| 1 | **P-1a fails** (expected) → no generated struct `toJson` at launch | §3.2: escape hatch + LA-15/LA-6-escalation filed same day with the probe transcript; owner's R4 posture makes this a short gap; M2 acceptance explicitly splits pre/post-ask |
| 2 | **Interface-without-attribute** (`: ISerializable`, no `@Serializable`): R2 fires, R1 didn't → cryptic "unknown member `__atlSrc`" at pass 2 | Documented pairing rule + user-guide error index mapping the exact message to the fix; LA-16 (`meta::Class.attrs`, §14) upgrades this to a sentinel-rule diagnostic |
| 3 | **Attr-less R2 over-matching** — matches every field of every `ISerializable` class in `uses Atlantis` files | That set is exactly the opt-in set (interface = consent, §2.1); `where` filters ignores; perf is per-compile comptime, measured under H-7 discipline |
| 4 | **`$f.name` in per-field templates silently wrong** (clones to a runtime `.name` access on the field's value) | Hard authoring rule: per-field templates use `$f` ONLY as `this.$f`; review checklist item; keys always flow from `__atlKeys()` |
| 5 | **Ctor no-op pass cost in user-declared entity ctors** (n `__atlDec(None,…)` calls) + `__atlSrc`/`__atlIdx` field pollution | Accepted, enumerated interim (§4.2/§4.4); deleted wholesale post-LA-6; M5 pins the byte-equivalence end state |
| 6 | **Inherited/mixin fields invisible** to `meta::Class.fields` — entity `Timestamps.createdAt` won't serialize | Blessed pattern: entities map to flat response DTOs (§8); shared P2 ask with Track 06 (`meta` inherited-field reflection, §14); guard cannot detect the omission — user-guide warning |
| 7 | **JsonValue-typed field aliasing** on encode (embedded subtree shares nodes) | Documented per Track 09 §6#3; flips to `deepCopy()` when Track 09 grows it (15-line rider recorded there) |
| 8 | **Nested-namespace qualified-path bug** biting spliced helper calls | Structural avoidance: all splice-facing symbols in `Atlantis` root, single-level paths only (§5.3); templates never name nested namespaces |
| 9 | **Overload gaps** (P-3/P-4/P-5 partial failures) | Each has a designed degradation (§5.1, §4.3) that shrinks v1 scope loudly (guard messages) instead of misbehaving |
| 10 | **Key-order instability** breaking golden tests if a future engine change reorders firings | Order is pinned twice: `__atlKeys()` (declaration order via `$for`) is the single source; corpus test asserts exact rendered key order |

---

## 12. Milestones & acceptance

| M | Deliverable | Accept |
|---|---|---|
| M0 | Probe suite P-1a…P-7 run + logged; LA-15 ticket filed if P-1a fails (expected); bug.md entries for any spec contradictions | transcript in Implementation log; asks registered in overview §2 (coordination with overview owner) |
| M1 | `Atlantis` root helper library (`__atlEnc`/`__atlDec`/`__atlZipObj`/guards) + `ISerializable` + attributes — pure library, no rules; unit corpus incl. every §6 policy | green on oracle + IR + LLVM (C8); needs Track 09 M4 (json) + M2 (DateTime) — target start 2026-08-24 |
| M2 | Rule set (R-keys, R-toJson per P-1a outcome, R1/R2/R3, R-nested, R-refuse); corpus: struct DTO + entity class round-trip, `@JsonIgnore`, optionals, DateTime, nested DTO, refusal diagnostics | `--expand` shows generated members; round-trip corpus green on 3 engines; if P-1a failed: FromJson-side green + hand-written-toJson corpus, toJson generation lands within days of LA landing |
| M3 | Policy hardening: unknown-key, missing-key, 400/422 split, int-integral check, `ValidationException` field payloads (needs Track 01's C4 types in-tree) | negative-case corpus (each policy has a throwing test); no second error type introduced |
| M4 | Content negotiation (`parseAccept`/`negotiate`/`isJsonContent`) + 406/415 helpers | RFC-shaped fixture table (wildcards, q-ranking, malformed headers); consumed by Track 01/02 stubs |
| M5 | Post-LA collapse (when LA-6/LA-15 land): direct codegen, interim machinery deleted; LA-4 alias hook | generated output **byte-identical to a hand-written twin** (rule_orm discipline); alias corpus once LA-4 lands |

Target window: M0 now (no deps); M1–M4 2026-08-24 → 2026-09-25 (inside AG-1→AG-2 window;
Track 02 binding consumes M2+M4 for AG-2 on 2026-10-05). M5 rides the owner's LA schedule.

---

## 13. STOP conditions

- **P-1a AND P-1b both fail** → the design has no field-splice mechanism at all: STOP,
  commit probes, file the statement-splicing LA, escalate — do not improvise a third
  mechanism (per the track brief and §0.4).
- **P-3 fails** (T vs T? overloads can't coexist or rank) → optional handling — a C7-level
  contract behavior — has no clean shape: STOP for an owner/checker ruling (it is likely a
  checker gap, not a design choice).
- Any pressure toward runtime type inspection, a Json-local exception hierarchy, a second
  serialization surface, or editing `src/**` → STOP per overview §0.4 (a)(b)(c).
- C7 surface changes (renaming `toJson`/`FromJson`, key policy) → escalation event, not an
  edit (§0.4 (e)).

---

## 14. Language asks (proposed for the overview §2 register — coordination note)

This doc cannot edit `techdesign-00-overview.md` (single-file ownership); the overview
owner should register:

- **LA-15 (new, P1 if P-1a fails):** value-bound member-selector splice — `this.$f` where
  `$f` is `$for`-bound to a `meta::Field` (splice its `name` as the selector).
  Consumer: Track 03 `toJson` (Mechanism A). Pointer: `Rules.cpp:1522–1534`. Fallback:
  §3.2. Alternative superset: LA-6.
- **LA-6 escalation:** raise `request-metaprog-attr-values.md` item (B)
  (statement-position `$for`) from P2 to **P1** — it is the permanent fix for both
  directions (§4.4), not an ergonomic nicety.
- **LA-16 (new, P2):** `meta::Class` gains `Array<string> attrs` + `hasAttr` (parity with
  Field/Method; `buildMetaValue` already walks the decl's `attrs` for members —
  `Rules.cpp:1048–1052`). Consumer: §11#2 diagnostics; eventually lets per-field rules
  gate on `@Serializable` directly.
- **Shared with Track 06 (P2):** inherited-field reflection (`meta::Class.allFields` or a
  `bases`-walk surface) — mixin fields for both serialization (§11#6) and ORM columns.

---

## 15. Implementation log (append-only)

- 2026-07-06 — design authored. Source-verified readings: splice paths
  (`Rules.cpp:1477–1557`), attr-less match + encloser constraint (`Rules.cpp:1093–1188`,
  `Parser.cpp:1167–1217`), `meta.*` materialization (`Rules.cpp:1005–1089`,
  `Resolver.cpp:1234–1255`), struct/interface + value semantics (reference.md:314–320).
  P-probes not yet run.
- 2026-07-07 — §16 addendum added on Tracks 02/09 coordination request (marker interface
  `IJsonSerializable`; rule-added bases ruled out by anchor inventory; manual declaration
  chosen as fallback and shown to be load-bearing). P-probe P-8 added.
- 2026-07-13 — **implemented, M0–M4 landed** (M5 deferred, rides LA-15/LA-6 per this doc's
  own target). Full detail in `packages/atlantis/tests/RESULTS.md` ("Atlantis Track 03");
  summary below.

  **M0 — probes.** All eight (P-1a…P-8) run and logged.
  - **P-1a FAILED exactly as predicted** (§3.2 path taken): `this.$f` inside a `$for`
    element template errors `'$f' is an attribute value, not a member name`, confirming
    the source reading verbatim.
  - **P-1b, P-2, P-3, P-5, P-6 all PASSED** on oracle/IR/LLVM: per-field ctor splicing
    works on structs; R1→R2→R3 ctor-append ordering is exactly as designed (R2 lands
    inside R1's rule-injected `FromJson`, and does NOT re-match R1's own injected
    fields — traced end-to-end, `packages/atlantis/tests/probes/ser_p2_...`); `T`/`T?`
    witness-overload ranking is correct; `$C` decl-hole in type position + `$C::Label(...)`
    works; labeled ctors ARE visible in `meta::Class.methods`, named by their label.
  - **P-4 FAILED** — not anticipated as a hard failure by §10's "UNKNOWN" marking, but
    it degrades exactly per §5.1's designed fallback. A generic body's call to an
    overloaded free function resolves once at definition-check time, not
    per-instantiation, and does so *differently, wrongly* per engine (oracle/IR agree on
    one wrong answer; LLVM disagrees with both). Filed **bug.md #54 [P1]**. Consequence:
    `__atlEnc<T>`/`__atlDec<T>` over `Array<T>`/`Map<string,T>` are NOT generic — §5.1's
    monomorphic per-type-overload fallback ships instead (`Array<int|float|bool|string>`,
    `Map<string,string>`), exactly as this doc's own contingency describes.
  - **P-7 PASSED on oracle** (`2,1` — confirms struct closures snapshot fields by value,
    killing closure-registry designs per §1). IR/LLVM rephrasings of the same probe hit
    two *pre-existing, already-filed* bugs (#42, #52) unrelated to this finding — the
    design's own "documentation-grade certainty" bar for P-7 doesn't require 3-engine
    parity, so this is not a new blocker.
  - **P-8 FAILED-TO-PARSE as expected** (negative confirmation): no anchor spelling
    reaches a class's base list.
  - **No STOP.** P-1a failing alone (P-1b passed) and P-3 passing both clear §13's STOP
    gates.

  **New finding (not a bug, filed as a P2 ergonomics ask, §4 of
  `designs/requests/request-metaprog-splices.md`):** rule SUBJECT-kind matching
  (`match @Attr on class C`) is **exact** — it does not match a `struct` declaration,
  unlike ENCLOSER-kind matching (`in class C`), which is deliberately lenient and also
  accepts `struct`/`interface` ancestors. This was invisible to this doc's own §1
  source-reading (which examined the splice paths and the encloser leniency, but not
  this asymmetry). Since §2.1's contract requires `@Serializable` to generate
  identically for both struct DTOs and class entities, every SUBJECT-position class-
  level rule (`serializableGuard`, `serializableKeys`, `serializableFromJsonSkeleton`,
  `serializableSeal`, `serializableNested`) ships as an explicit `...Class`/`...Struct`
  pair — ten rules, not five, mechanical and P-probed. `serializableApply` (R2, the one
  field-subject/encloser-position rule) needed no duplication — it already covers both
  via the existing leniency, proven by P-1b.

  **M1 — helper library shipped in full**
  (`packages/atlantis/src/json/serializable.lev`): `@Serializable`/`@JsonIgnore`,
  `IJsonSerializable` (§16's renamed spelling, used throughout — the body text's
  "ISerializable" spelling in §§2–14 above was never implemented, superseded as that
  addendum already says), `__atlGuard` (§5.2, all five listed refusals: bad `Map` key
  type, function-typed field, non-`T?` union field, reserved `__atl*` name, missing
  `: IJsonSerializable` pairing — HKT-typed-field detection alone was skipped, falling
  through to §5.2's own documented pass-2-overload-failure fallback), `__atlZipObj`,
  the full `__atlEnc`/`__atlDec` overload sets for
  `int`/`float`/`bool`/`string`/`DateTime` (+ `T?` twins), `JsonValue` passthrough, and
  the P-4 monomorphic fallback for `Array<int|float|bool|string>`/`Map<string,string>`.
  One addition beyond the original §5.1 sketch: a per-nested-class `__atlEnc($C?
  witness)` overload (§4.3's nested rule) — `$C?` is not itself assignable to
  `IJsonSerializable`, so the required-only `__atlEnc(IJsonSerializable v)` overload
  does not cover optional nested fields; discovered running the corpus, fixed the same
  session.

  **M2 — rule set shipped**, with the class/struct duplication above; `toJson()`
  generation NOT shipped (§3.2 escape hatch — every corpus type hand-writes it). R1
  guarded so a hand-written `FromJson` overrides generation (P-6-confirmed labeled-ctor
  visibility makes the guard real, not speculative per §3.3's contingency text).

  **M3 — policy hardening**, all of §6 exercised by the corpus: optional
  null-or-absent → `None`; `DateTime` ↔ iso8601; unknown keys ignored; missing required
  key throws naming the field; `@JsonIgnore` fully functional; non-integral `int`
  rejected; wrong top-level kind (array-for-object) throws via `__atlRequireObject`
  (the 400-vs-422 split itself is Track 02's binder's job — this track only proves
  `FromJson`'s own throwing contract). §6.4's exact wording ("expected `<type>`, got
  null") was simplified to the same generic "expected `<type>`" the ordinary
  kind-mismatch path already uses for a null-into-required-field — same status/type,
  cosmetically different message text; not chased further.

  **M4 — content negotiation shipped in full**
  (`packages/atlantis/src/json/negotiation.lev`, `namespace Atlantis::Json`):
  `parseAccept` (comma/semicolon split, strict `q=` parse with malformed→1.0, malformed
  ranges skipped, absent/empty→`*/*`), `negotiate` (most-specific-matching-range per
  producible type, then q desc, specificity desc, producible-array-order tiebreak,
  `q=0` excludes), `isJsonContent` (`application/json` or `+json` suffix, charset
  tolerated but must be utf-8 if present). `controller.lev`'s existing inline 415 check
  (Track 02, pre-dating this track) was left as-is — out of this track's file-ownership
  scope to rewire; `isJsonContent` ships ready for that consumption per §7's contract.

  **Side finding — stale documentation, corrected:** `docs/reference.md` §6.11 (and
  several other design docs, out of this track's scope to fix) still cited bug.md #30
  (a `Map<K, recursive-class>` LLVM corruption) as blocking JSON on LLVM. It was fixed
  2026-07-10 (`designs/complete/techdesign-bug30-map-with-ownership.md`); this track's
  own corpus (nested `Map<string, JsonValue>` objects, byte-identical oracle/IR/LLVM)
  re-confirms the fix. `docs/reference.md` §6.11 corrected as part of this track's doc
  duty. `designs/sonar/techdesign-08-theming-di.md`'s "avoid JSON, use in-language TOML"
  premise rests on the same now-stale claim — flagged for that track's owner, not
  changed here.

  **Test corpus:** `packages/atlantis/tests/probes/ser_p{1a,1b,2,3,4,5,6,7,8}_*.lev` (+
  `.expected`, oracle/IR three-way where applicable); `packages/atlantis/tests/corpus/
  serialization/` (struct DTO + entity round-trip, `@JsonIgnore`, optional
  present/absent/null, nested DTO required+optional, `DateTime`, `Array<string>`,
  every §6 policy, §7 negotiation/content-type fixtures — byte-identical
  oracle/IR/LLVM); `packages/atlantis/tests/corpus/serialization_refusals/` (all five
  `__atlGuard` messages, pinned as a deterministic compile-failure transcript).

  **M5 not attempted** — correctly rides the owner's LA schedule (LA-15/LA-6, both
  already filed pre-existing this track). The public surface (`toJson`, `FromJson`,
  key order, policies) is designed to be identical before and after, per §4.4; nothing
  in this track's shipped code needs to change shape when the collapse lands, only the
  rule bodies collapse and the escape-hatch requirement on `toJson()` lifts.

## 16. Addendum (2026-07-07) — marker interface stamping (Tracks 02/09 coordination)

Tracks 02 and 09 (landed) each need serialized types addressable **by contract**: Track 09
types `view(name, model)` against it, Track 02 keys binding/validation off it. Request:
`@Serializable` should *stamp* `interface IJsonSerializable { JsonValue toJson(); }` onto
the matched type. Ruling:

1. **Naming adopted:** the marker is **`IJsonSerializable`** (coordinator's spelling).
   This supersedes the `ISerializable` spelling used in §§2–14 above — a pure rename, one
   symbol, applied throughout at implementation time (interface decl, `hasBase` strings,
   encloser constraints, the `__atlEnc(IJsonSerializable)` overload). Lives in `Atlantis`
   root per §5.3. Interfaces allocate nothing; requiring one is free at runtime.
2. **Can a rule add a base? No — by construction, not by probe gap.** The rule layer's
   entire mutation surface is the anchor set (`member of`, ctor top/bottom, body
   top/bottom, `marker`, `namespace` — `Parser.cpp:1220ff`, the five `expand()` branches);
   no anchor targets a class's base list, and no fragment kind produces one. There is no
   syntax to probe. **P-probe P-8** is therefore a negative-confirmation probe only (the
   nearest legal spellings fail to parse), pinned so the gap is documented, not assumed.
3. **Fallback chosen: users declare `: IJsonSerializable` manually** — which is what
   §2.1 already requires, because the declared base is **load-bearing for the mechanism
   itself**, not just for addressability: per-field rule R2 gates on the encloser
   constraint `in class C : IJsonSerializable`, evaluated during rule *matching*. This is
   also why the alternative fallback (structural satisfaction by the generated `toJson`
   member alone) is rejected: conformance is nominal (`implementsOrExtends` walks declared
   bases, `Rules.cpp:1158–1160`), an undeclared interface types nothing for Tracks 02/09,
   and a rule-stamped base would have to be visible to the meta stage's own matching to
   drive R2 — rules-feeding-rule-matching is the fixpoint the metaprog design forbids
   everywhere (phase 3 §9's one-direction pipeline).
4. **Metaprog ask recorded, deliberately P2:** "attribute implies base" (a rule clause
   like `implements IJsonSerializable`, applied at expansion, visible to **pass 2 only**).
   With base-stamping pass-2-only (the only shape consistent with the one-direction
   pipeline), R2's gate would have to move off the encloser constraint — feasible post
   LA-16 (`meta::Class.attrs`, §14) by gating on the attribute instead. So the ergonomic
   end-state is: LA-16 + implies-base together remove the manual declaration; either alone
   does not. Registered as a joint note under LA-16 rather than a new P1 ask — the manual
   `: IJsonSerializable` costs one token and fails loud when forgotten (§2.1(c), §11#2).
5. **Consumer guarantees to Tracks 02/09:** every `@Serializable` type declares
   `IJsonSerializable` (guard-enforced pairing, §5.2), so `IJsonSerializable` is safe to
   use in their signatures today: `view(string name, IJsonSerializable model)`,
   binder constraints, and heterogeneous `Array<IJsonSerializable>` all type against the
   interface; `FromJson` stays per-type (label ctors aren't interface members — decode
   entry points take the concrete DTO type, which Track 02's binder already knows
   statically from the handler signature).
