# Tech Design 03 — Named Splice Anchor: `@InjectRoutes();` / `at splice Name` (E)

**Part of** `designs/metaprog-splices/` (see `-overview-opus.md`). **Complexity:** opus
(a new anchor kind + a program-global splice-site index; the largest single new mechanism in
this design). **Grounding commit:** `cc071c3`, spiked live. Orthogonal to files 01/02 (anchor
side, not hole side) — can land in parallel with 02.

Covers **(E)** (request §1E, ruling R10, promoted to P1/AG-2, supersedes the withdrawn D):
a statement `@Name(args);` inside a function body is a **user-placed, named anchor**; rules
declare `inject … at splice Name`, and every statement they produce lands at that point,
**in the surrounding lexical scope**.

---

## 1. The ask and why the existing anchor is not enough

Request's two example sites:
```
void addRoutes(Atlantis::App app) {
    @InjectRoutes();                 // ← attribute-declared routes land here
    app.AddRoute(Route('/', HomeController::Index).requestType(RequestType::GET));
}
void AddBindings() {
    @InjectBindings();               // ← @Injectable-generated binds land here
    bind IUserService => UserService();
}
```
The value (request's own three reasons): (1) the insertion point is **explicit, visible,
user-chosen**; (2) spliced `bind`/`AddRoute` statements **inherit the splice site's lexical
scope**, dissolving the `@Injectable` scoping problem by construction — no marker files, no
`collect()` indirection; (3) duplicate/collision checking happens at the splice site like
hand-written code.

**The `Marker` anchor is 80% of this — but subject-implicit, which is the wrong 20%.** The
`@anchor("name")` statement + `at marker "name"` already exist and already land statements at
a statement-position slot **in surrounding lexical scope** (`findMarkerSlot` walks the
function body recursively, `Rules.cpp:2502-2520`; the landed `tests/corpus/meta/rule_marker.ext`
injects into a method body at `@anchor("audit")`). What it cannot do is **cross-declaration**:

```
$ build/leviathan --run spikeE.ext
    # rule: match @Injectable on class C  →  inject `... $C.name ...` at marker "binds"
    # the @anchor("binds") lives in a DIFFERENT function, addBindings()
spikeE.ext:5:9: error: marker "binds" not found in 'UserService'
```

`Marker` resolves against `subjectOf(b)` (`Rules.cpp:1806-1808`) — the matched declaration's
own body. For (E) the rule matches `@Injectable class UserService` but must inject into
`addBindings()`'s body. The marker must be a **program-global site**, not a subject-local
one. That is the entire delta.

---

## 2. Design: `at splice Name` targets a program-global, attribute-named site

Three pieces: the **site** (`@Name();` statement), the **anchor** (`at splice Name`), and the
**index** that connects them across declarations.

### 2.1 The site — `@Name();` as a statement-position splice marker

Reuse the existing `@anchor("name")` parse machinery (`Parser.cpp:1000-1020`), which already
produces a `StmtKind::Empty` carrying a marker name in statement position. Add the R10
spelling: a statement `@Name();` where `Name` resolves to a **declared `attribute`** produces
the same `StmtKind::Empty` marker node, with `name = "Name"` (the attribute's simple name)
and a flag `isSpliceSite = true`.

- `@Name()` **must** name a declared `attribute` (request: "the splice attribute is an
  ordinary declared attribute (`attribute InjectRoutes {}`), so the `uses`-scoping wall
  applies unchanged"). If `Name` is not a declared attribute → **M43**.
- It is **statement-position only**. `@Name` in declaration position stays an ordinary
  decorator (that is F's / today's meaning); the trailing `()` **and** `;` in statement
  position is what distinguishes a splice site from a decoration. This disambiguation is
  purely syntactic and local (Parser.cpp already forks on `@` at statement start,
  `Parser.cpp:1000`).

Because the site is a declared attribute, **`uses`-scoping is automatic**: a rule injecting
`at splice InjectRoutes` fires against a file's sites only if that file imports the
attribute's namespace — the same wall every rule/attribute already lives behind
(`Rules.cpp:1531-1533`), no new scoping concept.

### 2.2 The anchor — `at splice Name`

Add `AnchorKind::SpliceSite` to the enum (`Ast.hpp:296-311`) and a parse arm in
`parseRuleAction` (`Parser.cpp:1656`, beside the existing `marker` arm):
```
} else if (at(Identifier) && cur().text == "splice") {
    advance();
    if (at(Identifier)) { out.target = cur().text; advance(); }   // the attribute name
    else error("a splice-site name after 'splice'");
    out.anchor = AnchorKind::SpliceSite;
}
```
Template is a **statement fragment** (`parseStmtsFragment`, the existing default) — the
spliced statements inherit the site's scope.

### 2.3 The index — the one genuinely new mechanism

Before expansion, build a **program-global splice-site index**: a map
`spliceName → [ { ownerDecl, bodyVec, idx } ]` by scanning every function/method body for
`isSpliceSite` marker nodes. This is `findMarkerSlot` (`Rules.cpp:2502-2520`) lifted from
"search one subject's body" to "index all bodies once," keyed by attribute name. Populate it
in the same walk that indexes declarations (`indexDecls`, `Rules.cpp:1290-1327`) so it costs
one extra pass over already-visited bodies.

`expand`'s `SpliceSite` arm (a sibling of the `Marker` arm at `Rules.cpp:1806`):
1. Look up `act.target` in the index.
2. **Zero sites → M42** (`at splice InjectRoutes` but no `@InjectRoutes();` anywhere). Unless
   — request: "a splice with no *firing rules* expands to nothing (no dangling warning — it
   is an intentional extension point)." Note the asymmetry: a **site with no rules** is
   silent (intentional extension point); a **rule with no site** is M42 (the rule asked to
   inject somewhere that does not exist). These are the two directions and they differ by
   design.
3. **More than one site → M42-multi**, *unless the rule opts into multi-site* (request: "two
   splices of the same name in one program should be a rule-stage error unless the rule opts
   into multi-site"). Spelling: `at splice Name multi` — injects into **every** site (the R10
   fan-out case, if a program legitimately has two `@InjectRoutes()` points). Default single
   keeps the common case honest.
4. Clone the template per matched site and insert **after** the marker node (reusing the
   `Marker` arm's `markerInsertCount_` accumulation, `Rules.cpp:1828-1833`, so multiple rules
   stacking on one site accumulate in deterministic rule order — already solved for markers).

The marker node itself **stays in the tree** (like `@anchor`), so `--expand` shows the site
and the injected statements around it — the request's "explicit, visible" property survives
into the artifact.

### 2.4 Lexical scope inheritance — free, by placement

Because the statements are inserted into `addRoutes`'s own `bodyVec`, they resolve names in
`addRoutes`'s scope: `app` (its parameter), `bind`'s in-scope binder, local imports — all
visible, with **zero** special plumbing. This is exactly the request's reason (2): the
`@Injectable` scoping problem "dissolves by construction." A spliced `bind IUserService =>
UserService()` sees the surrounding `AddBindings()` binder scope like a hand-written line, so
duplicate-bind and name-collision checks fire at the site like ordinary code (reason 3) —
these are the resolver/checker's existing job on the post-splice tree, not new logic here.

### 2.5 Hygiene

Spliced statements go through the ordinary `cloneStmt` hygiene (template-locals renamed,
free names def-site-qualified — `Rules.cpp:2255-2304`), identical to every other anchor. The
one deliberate exception the request implies: names meant to bind **at the site** (the
generated `bind`'s target, `app.AddRoute`'s `app`) are **free names that stay bare** and
resolve at the injection site — which is precisely the existing "bare name resolves at
injection site" channel (`Rules.cpp:2263-2268`). So `app` in the template resolves to
`addRoutes`'s parameter, not a rule-namespace symbol. No new hygiene rule.

---

## 3. Worked expansion — R10 route registration

```
namespace Atlantis { attribute InjectRoutes { } }
namespace App::Web {
    attribute Route { string path; string verb; }
    rule emitRoutes {
        match @Route(r) on method m in class C
        inject `app.AddRoute(Route($r.path, $C::$m).requestType($r.verb));`
               at splice InjectRoutes
    }
}
namespace App::Controllers {
    class HomeController { @Route("/", "GET") void Index() { ... } }
}
namespace App::Routes {
    uses Atlantis; uses App::Web;
    void addRoutes(Atlantis::App app) {
        @InjectRoutes();                       // ← splice site
        app.AddRoute(Route("/health", HealthController::Ok).requestType("GET"));  // hand-written
    }
}
```
`--expand` of `addRoutes`:
```
void addRoutes(Atlantis::App app) {
    @InjectRoutes();
    app.AddRoute(Route("/", App::Controllers::HomeController::Index).requestType("GET"));  // spliced
    app.AddRoute(Route("/health", HealthController::Ok).requestType("GET"));               // hand-written
}
```
Every `@Route`-annotated method in the program contributes one `AddRoute` at the single
`@InjectRoutes()` site, in `addRoutes`'s scope (`app` is its parameter). The `@Injectable`
DI case is identical with `bind` statements at `@InjectBindings()`.

---

## 4. Diagnostics (this file's rows)

| Code | Trigger | Message shape |
|---|---|---|
| **M42** | `at splice Name`, zero sites; or ≥2 sites without `multi` | `rule '<r>' injects at splice 'Name' but no '@Name()' splice site exists` / `… but 2 '@Name()' sites exist at <a>, <b> — add 'multi' to fan out or keep one site` |
| **M43** | `@Name()` splice statement where `Name` is not a declared `attribute` (or used in decl position) | `'@Name()' is not a declared attribute — a splice site must name 'attribute Name { }'` |
| (silent, by design) | a `@Name()` site with **no** firing rules | nothing — intentional extension point (request) |

M-codes reserved across the design in the overview (files 01/02 own M37–M41).

---

## 5. File-level change map

| # | Change | File(s) | Size |
|---|---|---|---|
| 1 | `AnchorKind::SpliceSite` | `src/Ast.hpp:296-311` | 1 line |
| 2 | `@Name();` statement parse → `Empty` marker w/ `isSpliceSite`, name=`Name`; **M43** for non-attribute / decl-position misuse | `src/Parser.cpp:1000-1020` | ~15 lines |
| 3 | `at splice Name [multi]` anchor parse | `src/Parser.cpp:1656` (beside `marker`) | ~10 lines |
| 4 | Program-global splice-site index (name → sites), built in `indexDecls` walk | `src/Rules.cpp:1290-1327` + a small member on `RuleEngine` | ~30 lines |
| 5 | `expand` `SpliceSite` arm: lookup, **M42** (0/≥2), per-site clone+insert reusing `markerInsertCount_` | `src/Rules.cpp:1806-1833` (sibling of `Marker`) | ~35 lines |
| 6 | `--rules` report: list splice-site firings | `src/Rules.cpp:2642-2651` | ~5 lines |
| 7 | Corpus + reference.md | `tests/corpus/meta/`, `docs/reference.md:2184-2189` (anchor list) | test/doc |

The `Marker` machinery (`findMarkerSlot`, `markerInsertCount_`, the marker-stays-in-tree
property, statement-fragment parsing) is **reused wholesale**; the only new code is the
**global index** (step 4) and swapping subject-local lookup for index lookup (step 5).

---

## 6. Alternatives / out of scope

- **Keep the withdrawn D (cross-decl marker file / `collect()`).** Rejected by the request
  itself (superseded by E, owner-blessed spelling). Not designed.
- **Make `@anchor("string")` cross-decl too** (string-named global markers) instead of
  attribute-named. Rejected: loses the `uses`-scoping wall and the typo-safety of a declared
  name — a mistyped `@anchor("bindz")` is a silent no-op, a mistyped `@Bindz()` is M43. The
  attribute spelling is why R10 chose it.
- **Auto-create the site if absent.** Rejected — hides where generated code lands, defeating
  reason (1); M42 makes the missing site loud instead.
- **`at splice` in class-body / namespace position.** Out of scope for v1 — the request's
  cases are both function-body statement sites. The index could later key non-statement sites
  the same way; no requester now.
- **Ordering across multiple contributing rules.** Handled by the existing deterministic
  rule order (`orderRules`, source-offset stable, `Rules.cpp:1334-1339`) + `markerInsertCount_`
  accumulation — the same guarantee `at marker` already gives; no new ordering policy.

---

## 7. Testing & acceptance

Corpus pairs, green on `--run`/`--ir`/`--expand`:
- `rule_splice_routes{,_twin}.ext` — the §3 route case: N `@Route` methods → N `AddRoute`s at
  one `@InjectRoutes()`, byte-identical to a hand-written `addRoutes`.
- `rule_splice_binds{,_twin}.ext` — `@Injectable` → `bind` statements at `@InjectBindings()`,
  proving lexical-scope inheritance (the spliced `bind` sees the surrounding binder scope).
- `rule_splice_empty.ext` — a `@InjectRoutes()` site with no firing rule expands to just the
  marker (no error, no injected statements) — the intentional-extension-point acceptance.
- `rule_splice_missing.ext` (red) — `at splice X` with no site → **M42**.
- `rule_splice_dup.ext` (red) — two `@InjectRoutes()` sites, non-`multi` rule → **M42-multi**;
  `rule_splice_multi.ext` (green) — same two sites with `at splice … multi` fanning out.
- `rule_splice_notattr.ext` (red) — `@Undeclared()` site → **M43**.

**Definition of done:** corpus green; full meta suite regression-clean; `reference.md` anchor
list gains `splice Name [multi]` and the `@Name();` site spelling; Track 04 replaces
`config/ioc.lev` hand-wiring with `@InjectBindings()` and Track 02/R10 wires routes via
`@InjectRoutes()`.
