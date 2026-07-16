# Request: Metaprogramming Phase-4 Subset — Attribute-Value Reflection First (LA-4/5/6/7)

**From:** Atlantis framework (Tracks 02, 03, 06). **Date:** 2026-07-06.
**Priority:** P1 — item (A) below is wanted by AG-4 (ORM, 2026-11-10); the rest are
ergonomics that can trail. These are all already-designed Phase-4 items
(`designs/complete/techdesign-metaprog-phase4.md`); this ticket is a **prioritized subset + ordering**
request, not new design.

## 1. The ask, in priority order

**(A) Attribute-value reflection in `meta.*`** (P4 item I) — the one that matters.
Today a rule reads its *matched* attribute's args (`$r.path`), but `$for f in C.fields`
iteration sees other attributes' **names only** (`f.hasAttr("Column")`). Consequence: a
class-level rule generating a schema/serializer **cannot read `@Column("full_name")` /
`@Json("alias")` argument values**, so per-field renames are impossible — the ORM corpus
example was forced to name columns by field name for exactly this reason (P3 §5).

Requested surface (P4-I's own sketch is fine):
```
class meta::Attr  { string name; /* typed arg access, e.g. */ string argStr(int i); int argInt(int i); }
// meta::Field/meta::Method gain:
Array<meta::Attr> attributes;   meta::Attr? attr(string name);
```
so a template can write
`$for f in C.fields.where((x) => x.hasAttr("Column")) : ($f.attr("Column").argStr(0) ?? $f.name)`
(or the equivalent shape the P4 design prefers — exact spelling is the metaprog owner's).

Real-world forcing function: **enterprise databases are snake_case, Leviathan fields are
camelCase** — without (A), Atlantis's ORM cannot map `full_name`/`created_at` columns, and
its JSON layer cannot interop with existing APIs' key names. This is the difference between
"works on greenfield demos" and "works against the database you already have."

**(B) Statement-position `$for`** (P4 item J) — P2. Serializer/schema codegen currently
contorts through array-literal-only iteration; statement-position `$for` makes generated
`toJson`/`applyRow` members direct instead of build-an-array-then-interpret.

**(C) Named attribute args** (P4 item M) — P2. `@Column(name: "full_name", nullable: true)`
readability once the language itself grows named args.

**(D) Layer D `rewrites`/`$body`** — P2, explicitly **last**. Unlocks `@Transactional`/
`@Timed`/`@Cached` wrappers. Atlantis's middleware model covers the HTTP-layer cases, so
this is a should-have, not a blocker; listed so its priority relative to (A) is on record.

## 2. Acceptance (for (A), the priority item)

1. Extend the existing ORM corpus pair (`tests/corpus/meta/rule_orm.ext` + twin):
   `@Column("full_name")` flows into the generated schema; byte-identical to hand-written.
2. A field with a bare `@Column` (defaulted arg) falls back cleanly (`argStr(0)` on a
   defaulted/absent arg is well-defined — `None` or the default, P4 owner's call).
3. `--expand` shows the reified values; hygiene/provenance unchanged.

## 3. Interim fallback (already designed in)

Column/JSON names derive from field names; where a rename is unavoidable, the
per-attribute-match constructor-registration pattern (each `match @Column(c) on field f`
CAN read `$c.name`) registers name maps at construction time — correct but clumsier and
per-instance. Atlantis flips to (A) the day it lands.

## 4. Addendum 2026-07-07 (LA-21, from Tracks 03/06 findings) — two `meta.*` riders

- **(E) Inherited/mixin field visibility.** `C.fields` reflects **own-body fields only**
  — a mixin's columns (`Timestamps.createdAt` on `class User : DbTracking, Timestamps`)
  are invisible to the entity's `$for` iteration. Ask: either `C.fields` includes
  inherited fields (flagged), or a separate `C.allFields`. Without it, framework mixins
  hand-implement their column contributions (done, works) but **user-defined mixins
  can't contribute ORM columns or serialized fields** — a real hole in the R2 showcase.
  Priority alongside (A): AG-4.
- **(F) `meta::Class.attrs`** (class-level attribute names, mirroring `Field.attrs`) —
  small symmetry gap; lets a rule's `where` see the subject class's other markers.
  P2. **Priority raise on this whole ticket:** (A) is now wanted by **AG-2/AG-4 (Oct)**
  per Tracks 02/03 — see overview §2.

## 5. Addendum 2 — 2026-07-07 (LA-27, from ruling R10 / the owner's example app)

**(G) Named attribute args + MEMBER REFERENCES + pipes as attribute argument values.**
The owner's example (`designs/atlantis/example/Models/User.lev`) writes:

```
@hasManyBelongsToMany(App::Relationship, self: App::Relationship::user1 | App::Relationship::user2);
Array<User> friends;
```

Three ingredients, in need order: (i) **named args** (`self:` — item C of this ticket /
P4 item M, priority now P1/AG-4 because this relation shape depends on it); (ii) a
**member reference as an attribute argument value** (`App::Relationship::user1` naming a
field of another class, checkable at rule time — a typo'd column should be a compile
error, which is the whole point over string names); (iii) a **pipe of two references**
where `self:` marks both as self-match candidates. The owner's semantic ruling is recorded
in the example's own comment and adopted verbatim by Track 06: *"self marks both sides of
the pipe as 'match against the owning instance.' Since a pipe only ever has two members,
and both are now marked as self-candidates, the return column falls out mechanically —
whichever one isn't self — without needing a separate symmetric:true toggle or a
where-style clause. The keyword carries the semantic; the pipe stays doing only
join-matching, not projection."* Attribute fields today are int/float/bool/string only —
this asks for a `memberref` attribute-field type (or equivalent), reified to rules as
(declaring-type, member-name) pairs.
