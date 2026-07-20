# Request: Rule-Template Splice Extensions (LA-15/16/17/22)

**From:** Atlantis Tracks 02, 03, 04, 06 (independently converged). **Date:** 2026-07-07.
**Priority:** P1 — the first two items gate **AG-2 (Oct 5)** headline features (typed
handler signatures; single-rule serializers). Companion to
`request-metaprog-attr-values.md` (values) — this ticket is about **where a bound thing
may be spliced**, that one is about **what can be read**. All items are additive template
capabilities; no new layer, no reentrancy, hygiene rules unchanged.

## 1. The asks, in priority order

**(A) Member-selector splice — `this.$f`** (Track 03; blocks toJson "Mechanism A", ORM
appliers). A `$for`-bound field must be usable in member-access position:
```
inject `JsonValue toJson() => __atlZipObj(
    [ $for f in C.fields : $f.name ],
    [ $for f in C.fields : __atlEnc(this.$f) ]);` at member of C
```
Track 03's source read (Rules.cpp:1522–1534) says `$f` after `.` is currently treated as
an attribute-value reification, not a member name — so the second `$for` fails today.
Requested: a `$for`-bound (or match-bound) field/method binding in selector position
splices the member name.

**(B) Identifier/type-position splicing of comptime strings + `$if`** (Tracks 02, 04's γ;
blocks Era-B typed handler binding). Two halves:
- a bound param's `.name`/`.type` usable as an identifier and as a **type token**:
  `$p.type local_$idx = ...;` and `$p.type::FromJson(v)` (type + labeled-ctor position);
- a conditional splice `$if (comptime-pred) { template-frag } $else { frag }` inside
  templates, so one binding rule emits per-param-source variants (path vs query vs body)
  without one-rule-per-combination explosion. This is NOT statement-`$for` (LA-6, already
  ticketed) — it is branch selection at expansion time, the template stays a template.

**(C) Identifier synthesis — `$ident(...)`** (Track 06; blocks compile-checked column
descriptors). Build a declaration name from bound strings: `class $ident(C.name, "Cols")
{ ... }` → `class UserCols`. With bug.md #22 in mind: name-collision with anything in
scope must be a rule-stage ERROR (never a crash, never silent shadowing).

**(D) Cross-decl marker anchor** (Track 04's β) — **SUPERSEDED 2026-07-07 by (E) below**,
which solves the same problem with a user-facing spelling the owner has already blessed
in `designs/atlantis/example/` (ruling R10). Kept for context only.

**(E) Named splice invocation — `@InjectRoutes();` as a statement** (R10; priority ⇒
**P1, AG-2**, promoted above (C)/(D)). A statement of the form `@Name(args);` inside a
function body is a **user-placed, named anchor**: rules declare they inject `at splice
Name`, and every statement they produce lands at that point, **in the surrounding lexical
scope**. The example's two uses:

```
namespace App::Routes {
    public void addRoutes(Atlantis::App app) {
        @InjectRoutes();                          // ← attribute-declared routes land here
        app.AddRoute(Atlantis::Routing::Route('/', App::Controllers::HomeController::Index)
            .requestType(RequestType::GET));
    }
}
namespace App {
    void AddBindings() {
        @InjectBindings();                        // ← @Injectable-generated binds land here
        bind IUserService => UserService();
    }
}
```

Why this shape is right: (1) it makes generated code's insertion point **explicit, visible,
and user-chosen** — explicitness moved up a level, the same argument as §16.5's rule-site
legibility, now applied to the anchor too; (2) spliced `bind` statements inherit the
splice site's lexical scope, which dissolves the @Injectable scoping problem (Track 04 §1)
by construction — no marker files, no collect() indirection; (3) duplicate-bind and
name-collision checking happens at the splice site like hand-written code. Semantics
notes: the splice attribute is an ordinary declared attribute (`attribute InjectRoutes {}`
in `Atlantis`), so the `uses`-scoping wall applies unchanged; two splices of the same name
in one program should be a rule-stage error unless the rule opts into multi-site; a splice
with no firing rules expands to nothing (no dangling warning — it is an intentional
extension point).

**(F) Statement-position grouped attributes** (R10, LA-26, P2). The example's entity
spelling: `@attr(PrimaryKey, AutoIncrement);` as a statement preceding a field, where the
bare identifiers reference declared attributes — sugar for `@PrimaryKey @AutoIncrement`
decorating the next declaration. Pure parse-level desugar; the interim spelling
(individual decorators) works today and stays legal.

## 2. Acceptance

1. Each item extends the existing meta corpus with an example + hand-written twin
   (byte-identical output, the rule_routes/rule_orm discipline).
2. `--expand` renders all splices; hygiene: spliced identifiers resolve at use-site for
   member selectors (A) and at def-site for everything else, per the existing model.
3. (C): collision → rule-stage error naming both sites.
4. Atlantis flips: Track 03 M5 deletes Mechanism B with byte-equivalence proof; Track 02
   Era-B activates; Track 06 replaces boot-validated strings with typed descriptors.

## 3. Interim fallbacks (all designed, all working, all deletable)

(A) per-field-match rules + runtime key index (Track 03 §Mechanism B); (B) Era-A uniform
`(Context) => HttpResponse` signatures + Controller helpers (Track 02); (C) boot-validated
`col("name")` strings (Track 06); (D) hand-written `config/ioc.lev` (Track 04's v1 primary
regardless).

## 4. Addendum 2026-07-13 (from Track 03) — subject-kind `class`/`struct` symmetry (P2, not blocking)

**Finding.** Rule SUBJECT-kind matching (`match @Attr on class C`) is exact —
`declKindMatches` (`Rules.cpp:1317`) requires `di.kindWord == kind` with no
leniency — while ENCLOSER-kind matching (`in class C`) is deliberately lenient
and also accepts `struct`/`interface` ancestors (`Rules.cpp:1152-1155`,
comment: "Encloser kind `class` also matches `struct` ancestors"). So `match
@Serializable on class C` silently does **not** fire on `@Serializable struct
S { ... }` — no diagnostic, the rule just never matches. Confirmed with a
minimal standalone repro (identical template; an `on class C` rule is silent
on a struct subject, an `on struct C` twin fires correctly on the same
declaration). This is not a bug — `reference.md` already lists `class` and
`struct` as independent subject kind-words alongside `field`/`method`/
`constructor`, so the exactness is the documented default, consistent with
every other subject kind. It is, however, an easy trap: any design whose
contract must cover **both** value structs and reference classes uniformly
(exactly Track 03's C7 contract, `@Serializable` on both DTOs and entities)
needs a full class/struct PAIR of every subject-position rule, with no
compiler help to catch a missing twin.

**Ask (P2, ergonomics — not a blocker; Track 03 shipped the workaround).**
Either (a) extend the SAME leniency subject-position matching already has
at encloser position (`on class C` also matches `struct`, `on struct C`
stays exact for authors who want to be struct-only), or (b) add a unifying
subject kind-word (`on type C`) meaning "class or struct or interface,"
mirroring the encloser clause's existing behavior. Whichever spelling: this
would collapse Track 03's five class/struct rule pairs (ten rules) into
five, and removes a silent-no-fire trap for every future track with the same
"works on both struct DTOs and class entities" shape (ORM entities, DI
registration, anything under the C7-shaped contract family).

**Interim (shipped, Track 03):** every subject-position `@Serializable`
rule (`serializableGuard`, `serializableKeys`, `serializableFromJsonSkeleton`,
`serializableSeal`, `serializableNested`) ships as an explicit `...Class`/
`...Struct` pair — mechanical, verified safe (both twins P-probed), zero
runtime cost (`packages/atlantis/src/json/serializable.lev`).
