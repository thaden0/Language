# Sonar DOM — Tech Design 01: Document, SonarApp, Selectors, DomNode

**Status:** design, pre-implementation. **Date:** 2026-07-15. **Track:** D01.
**Owns:** `sonar/src/dom/{document,node,selector,app}.lev`.
**Depends on:** T01 core (Component seams), T09 App, D-C contracts (anchor §5). Consumes D03's
`domSlot_`/`classes_`/`position_` additive fields (declared by D03; D01 reads them). Probes: D-P8, D-P10.
**Gates:** G-D1. **Difficulty:** M/L. **Risk:** MED — index-staleness discipline and the
narrowing-based uniform accessors are the concentrated risk.

Implements anchor contracts D-C2 (DomNode), D-C3 (selectors), D-C4 (Document), D-C9 (errors).

---

## 1. Design position

The component tree is the DOM; this track adds **addressing** (selectors), **identity** (ids, tags,
attrs — the meta side-table), and a **uniform hand** (DomNode) over it. Nothing here paints, lays
out, or dispatches — those stay landed machinery. The Document never owns components; it annotates
them. Everything is a cache over the tree, and the tree is always truth: a stale cache entry is
re-derived by walking, never trusted.

## 2. `Document` (`document.lev`)

```lev
class Document {
    new Document(App host);
    App host();

    // identity / meta (the side-table, §2.1)
    int __slotFor(IComponent c);            // assigns domSlot_ on first touch
    void __recordBuild(IComponent c, string tag, Array<string> attrNames, Array<string> attrValues);
    string tagOf(IComponent c);             // meta tag, else registry reverse-match (D-P8), else ""
    string attrOf(IComponent c, string name);       // "" when absent
    void __setAttrMeta(IComponent c, string name, string value);

    // id index (validated cache)
    void registerId(string id, IComponent c);       // throws SonarDomException on live duplicate
    IComponent? byId(string id);                    // validates attachment before returning (§2.2)

    // exposure + store (D05 consumes; declared here, spec'd there)
    void expose(string key, () => string getter);
    void set(string key, string value);  string get(string key);

    // sweep hook surface (D05)
    void __sweepBindings();
    bool __sweepDirty();

    // serializer (§5)
    string outerMarkup(IComponent root);
}
```

### 2.1 The meta side-table (storage discipline)

Per-node metadata is **parallel columns on the Document instance**, indexed by the additive
`Component.domSlot_` int (D03 declares the field; `-1` = never touched by DOM):

```
Array<IComponent> nodes_;        Array<bool>   alive_;
Array<string>     tags_;         Array<string> attrBlob_;     // "kvkv…" packed pairs
Array<ActionRegistry?> actions_; // D04 rows, lazily created
```

Rationale: no `Map<IComponent, …>` (class-keyed map on the hot path — unprobed), no
`Array<struct>` rows (#74 discipline), O(1) node→meta both ways. Attr pairs pack into one string
blob per node (attrs are read rarely — selectors `[attr=…]`, serializer, inspector — and written
once at build); ``/`` separators are rejected inside attribute values by the parser
(E-D2). Slots are recycled through a free-list when `alive_` entries are compacted (threshold: 25%
dead AND ≥64 dead — D-P10 pins flat churn).

### 2.2 Id index: validated cache, tree is truth

`Map<string, int> idIndex_` (id → slot). `byId` validates before returning: the slot must be
`alive_` AND the component's parent-walk must reach `host()` (or a live overlay root). A failed
validation evicts the entry and falls through to the full-tree walk (`findIdWalk`), re-indexing on a
hit. `registerId` on a **live** duplicate throws (`duplicate id 'x'` — loud, the Keymap-duplicate
precedent); a dead duplicate is evicted silently. Detach does NOT eagerly unregister (no detach hook
exists on plain components and D03's fields stay dumb) — staleness is absorbed by validation, which
is why validation is mandatory, not an optimization. Ids arrive from: the markup builder (D02), and
`DomNode.setId` (hand-built trees).

## 3. `SonarApp` (`app.lev` — DOM's, i.e. `sonar/src/dom/app.lev`)

```lev
class SonarApp : App {
    new SonarApp() {                 // App() singleton rule applies unchanged
        doc_ = Document(this);
        __setCurrentDocument(doc_);  // namespace free-fn setter — the R3 lowering rule
    }
    Document document() => doc_;
    void start() => this.run();      // alias; run() stays canonical

    // frame hook: sweep bindings, then the landed frame
    void renderFrame() {
        doc_.__sweepBindings();
        this.App::renderFrame();     // qualified base call (landed, #64-fixed)
    }

    // query sugar at the root
    DomNode query(string selector);      DomNodeList queryAll(string selector);
    DomNode? queryOrNone(string selector);
}
namespace Sonar { namespace Dom {
    Document? currentDocument_ = None;
    void __setCurrentDocument(Document d) { currentDocument_ = d; }
    Document document();                 // throws SonarDomException when no SonarApp exists
}}
```

`SonarApp.add` is inherited `Container.add` unchanged; Document learns about subtrees lazily
(queries walk) and eagerly only for markup-built nodes (the builder records meta at construction).
A plain `App` user never touches any of this — zero cost, zero behavior change.

## 4. Selector engine (`selector.lev`)

### 4.1 Compile

`compileSelector(string) -> Selector` — a `.chars()`/`.code()` scanner (no char literals in
call-arg position), producing (all **classes**, not structs — they carry arrays):

```lev
class SelPart { string tag; Array<string> ids; Array<string> classes;
                Array<string> attrNames; Array<string> attrValues; Array<bool> attrHasValue; }
class SelChain { Array<SelPart> parts; Array<bool> childCombinator; }   // parts.length()-1 combinators
class Selector { Array<SelChain> alternatives; }                        // comma groups
```

Parse errors throw `SonarDomException("selector 'a >> b': unexpected '>' at 4")` — offset caret,
E-D10. Grammar is anchor D-C3 verbatim; whitespace collapses; `>` binds tighter than space by
tokenization (no ambiguity — combinators are single tokens between compounds).

### 4.2 Match

`bool matchPart(SelPart p, IComponent c, Document doc)`:
- tag: `doc.tagOf(c) == p.tag` — meta tag when the node was markup-built; else the registry's
  reverse matchers (`Dom::registry().tagMatch(c)`, D02) narrow landed classes (`c is Button` …,
  probe D-P8); miss ⇒ no tag match (documented: tag selectors need meta or a registered matcher).
- id: `doc.attrOf(c, "id")`-equivalent via the id column; class: D03 `hasClass`; attr: the meta blob.

`queryAll(root, sel)`: one DFS over `__sonarChildren()` (visible and hidden both — selectors see the
whole tree; visibility is a paint fact), testing the **rightmost** part at each node, then verifying
leftward parts against the ancestor path (descendant: scan up; child: immediate parent only).
Document order, deduplicated. Fast path: a selector that is exactly one `#id` part goes through
`byId`. Cost: O(nodes × parts) worst case — trees are ~10²–10³ nodes; fine (inspector re-queries are
the heaviest consumer and are human-triggered).

`query` = first match or **throw** (`no match for '#file-menu'` + root description); `queryOrNone`
returns `DomNode?`.

## 5. `DomNode` / `DomNodeList` (`node.lev`)

`DomNode` wraps `(IComponent raw, Document doc)` — created fresh per query (no identity; the
COMPONENT is the identity, `same(other)` compares raws). Full surface per anchor D-C2. The
type-dispatched accessors are narrowing ladders (the one place the DOM knows concrete classes):

```lev
string value() {
    var c = raw_;
    if (c is TextArea)  return c.text();
    if (c is Input)     return c.value();
    if (c is CheckBox)  return c.checked ? "true" : "false";
    if (c is RadioGroup) return c.selected.toString();
    if (c is Text)      return c.text();          // value ≡ text for display leaves
    if (c is ContentBar) return c.left();
    throw SonarDomException("node <" + doc_.tagOf(c) + "> has no value");
}
DomNode value(string v) { … mirrored setters …; return this; }
```

`text()`/`text(string)` = the display-text channel (Text/Button label/MenuItem label/Modal title…);
`checked`, `enabled` (Focusable-family `enabled` fields + `setTabStop` coupling per T04 convention),
`show()/hide()` = remove/add the `hidden` semantic class (D03 owns the class⇄visible coupling),
`on(event, handler)`: event-name ladder → the landed token APIs (`"key"→onKey`, `"mouse"`,
`"paste"`, `"press"→Button.onPress`, `"select"`, `"change"`, `"submit"`, `"toggle"`, `"dismiss"`) —
unknown event throws E-D11 listing the node's events. `attr(name, value)` re-applies through the
D02 registry applier (live mutation — the inspector's edit path) and updates the meta blob.
`actions()` lazily creates the node's D04 registry row. `position()` returns D03's live object.

`DomNodeList`: `length()/at(i)/first()/forEach(fn)` + broadcast mutators (`addClass`, `removeClass`,
`show`, `hide`, `enabled(bool)`, `value(string)`) returning the list. Empty lists are legal
(broadcasts no-op); `at` out of range throws (array rule).

### 5.1 Serializer

`outerMarkup(root)`: meta-built nodes emit `<tag attr="v"…>` with recorded attrs (in application
order), synthesized `id`/`class` reflect CURRENT state (id column, live class list), children
recurse, text leaves emit their current text (bindings serialize as their `{{…}}` source — recorded
by D05 at registration). Hand-built nodes with no meta emit `<!-- unmapped: … -->` placeholders
(best-effort, documented — the serializer is a debugging/testing tool, not a persistence format).
Round-trip guarantee (D08 tests): parse(outerMarkup(parse(m))) is structurally identical for
meta-built trees.

## 6. Milestones

| M | contents | difficulty |
|---|---|---|
| M1 | Document core: slots/meta columns/compaction + id index with validation | M |
| M2 | selector compile + match + query/queryAll/queryOrNone + fast path | M |
| M3 | DomNode/DomNodeList full surface incl. narrowing ladders + on() | M |
| M4 | SonarApp (document/start/renderFrame hook/query sugar) + currentDocument global | S |
| M5 | serializer + round-trip harness with D02 | S |

## 7. Potential issues & mitigations

1. **Stale index vs. loud queries** — a query hitting a detached-but-indexed id must not return a
   zombie. Validation walk (§2.2) is mandatory on every index hit; the churn probe (D-P10) pins
   flatness; a dedicated test detaches an id'd subtree and asserts `queryOrNone` is None + the entry
   evicted.
2. **Narrowing ladders drift as components are added** — the ladder is data the registry also
   carries; D08's drift test constructs every registered tag and asserts `value`/`text` round-trips
   or throws per a table, so a new tag missing its ladder row fails CI.
3. **`domSlot_` collision with recycled slots** — a recycled slot's old component must not alias the
   new one: recycling stamps `nodes_[slot]` and the component's `domSlot_` together; validation
   compares identity (`nodes_[slot] == c`) before trusting any row.
4. **Query cost inside paint** — selectors are forbidden in paint paths (the Styleable.resolve cost
   note precedent); documented, and the inspector throttles its re-query to selection changes.
5. **Multi-mixin narrowing (D-P8)** — if `is` misbehaves on any landed multi-mixin leaf, tag
   selectors degrade to meta-only (fallback named in the probe register); id/class selectors are
   unaffected either way.

## 8. Testing plan

Table-driven selector corpus (compile errors incl. offsets; match/no-match pairs over a fixture tree
built both by markup and by hand); id duplicate/evict/revalidate scripts; DomNode accessor matrix
(every tag × value/text/checked/enabled — the drift table); on() ladder for every event name +
unknown-event throw; broadcast list ops; serializer round-trips + unmapped placeholders; 5k-node
churn (D-P10). All headless via SonarTest harness, differential oracle/IR/LLVM, emit-C++
compile-only.

## 9. Open questions

1. `:focus`/`:disabled` pseudo-classes — natural v1.1 sugar over Focusable state; deferred.
2. Scoped queries with combinator roots (`> menu` relative to the node) — v1.1.
3. Attribute meta for hand-built nodes (auto-capture on fluent setters) — would need setter
   instrumentation on landed classes; rejected v1 (meta is a markup-tier fact; `attr()` writes work
   on any node regardless).

## 10. Implementation log

- 2026-07-15 — design written; not started.
- 2026-07-15 — **D01 IMPLEMENTED (M1–M5) in `sonar_v2/`.** Per the owner directive to
  implement Sonar v2 in a "v2 directory," the whole framework was copied to `sonar_v2/`
  (package still `namespace Sonar`, version bumped 0.1.0 → 0.2.0) and the DOM layer added on
  top, so the landed, validated `sonar/` 0.1.0 package is untouched. Files:
  `sonar_v2/src/dom/{document,selector,node,app}.lev` (this track) + two **interim cross-track
  seams** replaced wholesale when their owners land: `dom/position.lev` (D-C5 `Position` +
  `PositionMethod`, D03) and `dom/actions.lev` (D-C7 `ActionRegistry`, D04 — local `fire`, no
  responder chain yet). The additive `Component` seams D01 *reads* (`domSlot_`, `classes_` +
  `hasClass/addClass/removeClass/toggleClass`, lazy-less `position_` accessor) were added to the
  v2 copy's `component.lev` (D03 owns them in the frozen anchor; provided here so D01 builds and
  tests standalone), plus `left()/center()/right()` getters on the v2 `contentbar.lev` for the
  value ladder. **Verified oracle/IR/LLVM byte-identical** (`sonar_v2/tests/dom/`, golden
  `dom.expected`); emit-C++ is the sanctioned async/native skip (App drags in `sysTaskCancel` —
  the standing T09 lane matrix, printed by `runtests.sh`). Probes exercised green: D-P8
  (`is`-narrowing IComponent→Button/Text/Input/CheckBox/RadioGroup/ContentBar in the ladders,
  all three run engines), D-P10 (5-node build/detach/`__reap`/rebuild recycles slots — capacity
  flat).

  **Findings / forced deviations (all footgun-compliant, none a compiler STOP):**
  - **Bare class-typed fields auto-construct (§3).** `App host_;` on Document and
    `Document doc_;` on SonarApp would auto-construct — the former trips `App()`'s single-app
    guard, the latter has no nullary ctor. Both are `T? = None` set in the ctor and narrowed on
    read (`host()`/`rootNode()`/`document()`). Not a bug; the auto-construction rule working as
    designed. Worth a footgun-doc row.
  - **No narrowing across early returns** (D00 §7) — the checker does not carry a `!= None`
    narrowing past an early `return`/`throw`; every optional read narrows inside an
    `if (x != None) { … }` block or a loop condition (which *do* narrow). Restructured
    `reachesRoot`, `matchChainAt`'s child combinator, and `queryStr` accordingly.
  - **Attr storage** uses cross-node parallel arrays (`attrSlot_/attrName_/attrValue_`) instead
    of the §2.1 packed ``/`` blob, to avoid embedding control-char (0x1f/0x1e) separators in
    string literals — functionally identical, footgun-free, still instance-scoped (#73). `id`
    and `class` are **not** stored as attrs: `id` lives in a per-slot `nodeId_` column (the
    authoritative value the revalidation walk compares) + the fast `idKey_/idSlot_` index;
    `class` routes into the live class list. Both are synthesized by the serializer (§5.1).
  - **Id fast index is parallel arrays** (`idKey_/idSlot_`), not `Map<string, int>` — dodges the
    `Map.without` LLVM gap (#18) and makes eviction a rebuild. `byId` validates a fast hit
    (alive + reaches root); a miss re-walks the tree comparing `nodeId_` and re-indexes.
  - **Reap-on-demand** (`__reap()`): with no detach hook on plain components (§2.2), staleness is
    collected by freeing every alive slot whose node no longer reaches the root, recycling the
    slot through `freeSlots_`. This keeps D-P10 churn flat without array remapping; the
    array-shrinking compaction of §2.1 is deferred (free-list reuse covers build/teardown churn).
  - **`DomNode.on(event, handler)`** takes a uniform `() => void`; the native event object is
    reached through the landed typed APIs when needed (payload-carrying `on` forms are v1.1).
    `TextArea` is omitted from the value/text ladders (D06 adds it).
  - **D02 dependencies absent** (this is D01): tag selectors on landed-class nodes rely on meta
    (registry reverse-match is D02, documented §4.2); `attr(name, value)` updates the meta blob
    only (the registry applier is D02). Both degrade exactly as the design's fallbacks specify.
  - **Toolchain:** `build/liblvrt.a` was stale (missing `lvrt_value_has_promise` /
    `lvrt_register_spawn_check` — LA-30 spawn/promise guard symbols); rebuilt via
    `cmake --build build --target lvrt`. Not a code change; noted for the next native run.
