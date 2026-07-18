# Atlantis Track 06 — ORM (v2, rebuilt)

**Status:** accepted design — owner rulings taken live in-session 2026-07-18. This doc
**supersedes and replaces the 2026-07-06 draft in full** (purged by owner directive: the
descriptor/`Cond` query machinery, the six-member `__orm*` seam sketch, and the
closure-array applier design are all dead; nothing from v1 is normative).
**Date:** 2026-07-18. **Gate:** AG-4 (2026-11-10), unchanged.
**Depends on:** Track 05 (C3 seam + `atlantis-mysql` driver — **landed 2026-07-17**),
Track 03 (serialization — **landed 2026-07-13**; its probe results and rule mechanics are
this doc's precedent), metaprogramming Phases 1–4 (**landed**, incl. member-selector
splice, named attribute args, Layer D, procedural macros), LA-18 `T::member`
(**landed**), `enum` (**landed**), **LA-31 expression reification**
(`designs/requests/request-expr-reification.md`, filed with this doc — the one new
language ask; owner builds per R9).
**Owns:** namespace `Atlantis::Orm` — its types, **and its attributes and rules** (C1 as
amended 2026-07-18: subsystem-owned metaprogramming; an entity file opts in via
`uses Atlantis::Orm;` and activates exactly the ORM rule set, nothing else).

---

## 0. Mission and the five rules

**Mission.** The data layer of Atlantis: entities are reference classes tracked by
compile-time-generated members, queried **in Leviathan** (not SQL), persisted through a
unit of work, read at bulk speed through dense struct projections. Zero runtime
reflection; every generated line visible under `--expand`; speed is the top-ranked
requirement, then intuitiveness, then compactness.

Five rules; every section below is a consequence of one of them:

1. **Leviathan is the query language.** Predicates, orderings, projections, includes,
   and bulk updates are ordinary Leviathan lambdas over the entity type, compile-checked
   by the ordinary checker. SQL is a *rendering target* behind the seam, never the
   surface. (Owner direction 2026-07-18: users should want Leviathan as the query
   language; no commitment to any future engine's internals is made here — §3.7.)
2. **Entities are `class : Model`; projections are `struct`.** Identity + tracking needs
   ARC'd reference objects; bulk reads want dense value structs (columnar when
   pure-scalar). This is R2 mapped onto the ABI, not a second persistence model.
3. **Everything generated, nothing reflected.** Per-entity members come from rules over
   `$for`; the mechanisms used are the ones Track 03 landed and corpus-proved, upgraded
   where splice capabilities have since landed.
4. **The unit of work is the only write path.** Passive field-write tracking, snapshot
   diff, one transaction per `save()`, dirty-columns-only UPDATE, multi-row INSERT.
5. **Relations load explicitly and in batch, or throw loudly.** N+1 is structurally
   unreachable, not linted.

**Non-goals (v1, each with its trigger):** a JOIN builder (relations are second-query +
stitch; exotic joins use the raw hatch; revisit only on demonstrated need); lazy loading
(never — explicitness is a feature); `@Transactional` via Layer D (`rewrites` has landed,
but `transact()` stays the one model until the owner asks — one model per concern);
schema-diff auto-migrations (scaffolder prints suggested DDL; humans own data-loss);
composite / non-`int` primary keys (single `@PrimaryKey int` v1); PostgreSQL (C3 seam is
ready; separate package when scheduled); **any LeviathanDB contract** (owner ruling: that
engine's shape is a post-stage-3 research outcome; §3.7 records what this design
deliberately keeps open for it).

**Error model:** C3's capability interfaces (`IDbException`, `IDuplicateKeyException`,
`IDeadlockException`, `IConnectionLostException`, `ITransientDbException`) are caught
driver-agnostically. This track adds, in `Atlantis::Orm`: `MappingException` (row ↔
entity shape mismatch — always names table, column, field, and the fix),
`NotLoadedException` (relation touched before loading — carries the fix in the message),
`EmptyResultException` (`first()` on zero rows), `OrmLogicException` (misuse: re-entrant
`save()`, nested `transact`, missing `uses`). All implement `IException`; none are HTTP
types (layering — controllers map them via C4 if they want to).

**DbValue.** As in C3: a cell is `string | int | float | bool | None`, written out at
every signature (no alias facility). Prose below says "DbValue" for readability; code
spells the union.

---

## 1. The entity surface

```
uses Atlantis::Orm;                       // opts THIS file into the ORM rules (C1-as-amended)

namespace App::Models {

    @Table("users")
    class User : Model, Timestamps, SoftDelete {

        @PrimaryKey @AutoIncrement
        int id;

        @NotNull
        string name;

        string email;                     // a plain field IS a column (§1.2)
        bool active = true;
        RelationType relation;            // enum column — stored by carrier int (§7)

        @Ignore
        int scoreCache;                   // explicitly not persisted

        @HasMany("App::Models::Post", fk: "userId")
        RelMany<Post> posts;              // relation — never a column (§5)

        @ManyToMany(through: "App::Models::Relationship", a: "user1", b: "user2")
        RelMany<User> friends;            // owner's self-join ruling honored (§5.4)
    }
}
```

Deltas from the owner's `designs/atlantis/example/Models/User.lev`, each ruled:

- **`uses Atlantis::Orm;`** not `uses Atlantis::Models;` — the amended C1's own worked
  example names `Atlantis::Orm` as the entity-file import; adopted. (If the owner later
  prefers the `Models` spelling, it is a namespace rename, not a design change.)
- **Individual decorators** (`@PrimaryKey @AutoIncrement`) are the shipping spelling;
  the example's grouped `@attr(PrimaryKey, AutoIncrement);` statement is LA-26 (splices
  ticket item F, parse-level sugar, not landed). The day it lands, both spellings are
  legal and mean the same thing; nothing here depends on it.
- **Relations wear `Rel<T>` / `RelMany<T>`** rather than bare `Array<User>` — ruled
  2026-07-18: a bare array cannot distinguish *unloaded* from *empty*, and silent-empty
  is the exact enterprise failure this ORM refuses to have (§5.1).
- **`@Table` is required.** No pluralization/derivation magic. A `Model`-derived class
  without `@Table` generates nothing — and the boot guard (§8) makes that loud.

### 1.1 Attribute vocabulary (all in `Atlantis::Orm`; supersedes the C5 data rows)

```
attribute Table       { string name; }            // v1: one path segment (§3.6)
attribute PrimaryKey  { }                         // exactly one per entity, int-typed (guarded §8)
attribute AutoIncrement { }                       // DB assigns; save() writes lastInsertId back
attribute NotNull     { }                         // schema constraint; boot-checked vs T? (§8)
attribute Ignore      { }                         // field is not a column
attribute HasMany     { string entity; string fk; }
attribute BelongsTo   { string entity; string fk = ""; }   // fk defaults to own <field>Id (§5.2)
attribute ManyToMany  { string through; string a; string b; }
attribute Row         { }                         // struct projection lane (§6.3)
```

Named attribute arguments are **landed** (`evalAttrArgs`, corpus `attr_named.lev`), so
`fk:`/`through:`/`a:`/`b:` are today's dialect, not an ask. Entity references are strings
in v1; the checked-member-reference upgrade is LA-27 and replaces strings mechanically
when it lands (the attributes gain `memberref` fields; no consumer code changes).

### 1.2 Fields are columns (ruled 2026-07-18)

Every field the entity's own body declares is a column **except**: `Rel<`/`RelMany<`-typed
fields, and `@Ignore` fields. Visibility is irrelevant (`private string passwordHash` is
a column — private-to-code and persisted are orthogonal). Column name = field name
verbatim (renames are LA-4's `@Column("snake_name")` upgrade; nothing in v1 depends on
it). A field whose type has no column mapping (§7) **fails the build** at the generated
converter call — overload resolution has no candidate — and the §8 sentinel rule
pre-empts the common cases with a friendlier message.

The polarity's failure mode is loud by construction: a forgotten `@Ignore` produces an
unknown-column error at **dev-boot schema validation** (§8) or, at worst, on the first
executed statement (generated SQL always lists columns explicitly — never `SELECT *`).
The reverse polarity's failure mode (forgotten `@Column` → field silently never
persists) is the one we refuse.

### 1.3 Mixins — the multiple-inheritance showcase, kept honest

`Timestamps` and `SoftDelete` are real stateful bases in `Atlantis::Orm`:

```
class Timestamps {
    DateTime createdAt = DateTime::Epoch();
    DateTime updatedAt = DateTime::Epoch();
}
class SoftDelete {
    DateTime? deletedAt = None;
    bool isDeleted() => this.deletedAt != None;
}
```

`meta::Class.fields` still reflects **own-body fields only** (LA-21 item E: designed,
pull-based, not landed), so mixin columns are invisible to the entity's `$for`. The
framework mixins therefore **hand-implement** a contributor trio —
`__mixCols() / __mixVals() / __mixApply(Row)` — and `Model`'s aggregators splice them in
with compile-time-known type probes (`match (this) { Timestamps => …; else => {} }`),
exactly the mechanism v1 sketched and Track 03's experience validates. Finite (two
mixins), hand-written once, zero reflection.

**User-defined persisted mixins are not supported in v1** — that is LA-21's landing
condition, and this doc pulls LA-21 (owner queue, P2→P1 at the owner's discretion). No
ctor-registration contraption ships in the interim: a user mixin's fields today simply
are not columns, and the §8 boot validator warns when it detects a `Model`-derived class
inheriting fields from a non-framework base ("field X inherited from Y is not persisted —
user persisted mixins gate on LA-21").

Field collisions between mixins keep the language's answer: `distinct` slots +
qualification; a collided **column name** is a boot error naming both sources.

`Model` itself:

```
class Model {
    private Array<string | int | float | bool | None> __snap = [];
    private bool __isNew = true;

    bool isNew() => this.__isNew;
    bool isDirty() { … }                    // __isNew || dirtyCols().length() > 0
    Array<string> dirtyCols() { … }         // element-wise dbEq vs __snap
    // Generated-member defaults: every __orm*() below has a throwing body:
    //   throw OrmLogicException("ORM members not generated for <type> — is the class
    //   @Table-attributed, and does its file `uses Atlantis::Orm;`?")
    // (the C1 silent-no-fire duty, §8). Derived overrides dispatch on the runtime
    // object — class-method dispatch landed 2026-07-11; probe P8 re-pins it.
}
```

---

## 2. Generated members (the seam)

One rule set, matching `@Table(t) on class C` (subject-kind is exact — entities are
always `class`, `@Row` targets are always `struct`, so no class/struct rule pairs are
needed; the Track 03 trap does not apply). Generated per entity:

```
Array<string>  __ormPath();     // ["users"] — target as PATH, not bare name (§3.6)
string         __ormPk();       // "id"
Array<string>  __ormCols();     // own @-eligible fields, declaration order
Array<string | int | float | bool | None> __ormVals();   // current values, same order
new FromRow(Atlantis::Data::Row r);                      // materialization (§2.2)
void           __ormSetPk(int v);                        // AutoIncrement write-back
```

plus `Model`-level aggregation (`allCols() = __ormCols() + mixin contributions`, same
for vals/apply) consumed by §3–§6.

### 2.1 The read side — landed splice shapes, no probes pending on capability

The **member-selector splice for `$for`-bound fields is landed** (reference.md §6.9:
a `$for`-bound `meta::Field` in member-selector position splices its name), which is
exactly what Track 03's P-1a lacked in July week 1. So the value-side generators are
single clean rules:

```
rule ormCols {
    match @Table(t) on class C
    inject `Array<string> __ormCols() =>
        [ $for f in C.fields.where((x) => !x.hasAttr("Ignore")
                                        && !x.type.startsWith("Rel<")
                                        && !x.type.startsWith("RelMany<")) : "$f.name" ];`
        at member of C
}
rule ormVals {
    match @Table(t) on class C
    inject `Array<string | int | float | bool | None> __ormVals() =>
        [ $for f in C.fields.where((x) => !x.hasAttr("Ignore")
                                        && !x.type.startsWith("Rel<")
                                        && !x.type.startsWith("RelMany<")) :
            Atlantis::Orm::toDb(this.$f) ];`
        at member of C
}
```

`"$f.name"` string reification and the `$for` filter shape are corpus-proven
(`rule_orm`); `this.$f` in read position is the landed splice; **P1 (§11) verifies the
whole template from inside nested `Atlantis::Orm`** — the amended C1 places rules in the
subsystem namespace, and Track 03 §5.3 documented an open resolver bug on
nested-namespace qualified paths in templates. If P1 trips that bug, the fallback is
Track 03's: splice-facing **helpers** (`toDb`/`fromDb`/`step` only) move to `Atlantis`
root as `__orm*` free functions (compatible with amended C1, which restricts *rules and
attributes*, not helper functions) and the bug is filed. Rules and attributes stay in
`Atlantis::Orm` either way.

### 2.2 The write side — `FromRow`, two byte-equivalent shapes, probe-selected

Materialization is a generated labeled constructor: `E e = E::FromRow(row);`. No
closures, no per-row allocation beyond the entity itself. Assignments are statements and
`$for` is still array-literal-only (LA-6: designed, pull-based, not landed), so the body
uses **assignment-as-expression inside an array literal** — or, if the assignment-target
splice isn't admitted, Track 03's proven ctor-append machinery. Public surface identical
in both shapes; probes P2/P3 select.

**Shape A (primary — needs `this.$f` as assignment *target* in a `$for` element, P3):**

```
rule ormFromRow {
    match @Table(t) on class C
    inject `new FromRow(Atlantis::Data::Row r) {
        Array<int> _fx = [ $for f in C.fields.where((x) => /* same filter as §2.1 */) :
            Atlantis::Orm::step(this.$f = Atlantis::Orm::fromDb(r.byName("$f.name"), this.$f)) ];
        this.__ormSeal();
    }` at member of C
}
// Orm::step(x) => 0 — evaluates the assignment for effect; inlines to nothing on LLVM.
```

**Shape B (fallback — Track 03 Mechanism B verbatim, P-1b/P-2-proven):** a skeleton rule
injects `new FromRow(Row r)` setting scratch fields `__ormSrc`/`__ormIdx`; one per-field
rule (`match on field f in class C : Model`, encloser-constraint through the resolved
base chain — P5 pins the base-class form) appends
`this.$f = Atlantis::Orm::fromDbAt(this.__ormSrc, this.__ormCols(), this.__ormIdx, this.$f);
this.__ormIdx = this.__ormIdx + 1;` at `bottom of C.constructor`; a seal rule (declared
after) appends `__ormSeal()`. Field order joins `__ormCols()` by runtime index — the
same filtered iterator, same order. In user-declared ctors the appended statements no-op
(`__ormSrc == None` → witness passthrough), the Track 03 discipline. Scratch fields and
index join are deleted the day LA-6 or the P3 splice admits Shape A — byte-equivalence
pinned by acceptance (M1).

`__ormSeal()`: `this.__snap = this.allVals(); this.__isNew = false;` — the snapshot is a
**COW alias, O(1)** (pure-value `Array`), immune to later field writes by value
semantics.

`__ormSetPk(int v)`: generated by a single-element `$for` over the `@PrimaryKey` field
(Shape A form) or a per-field member injection (P4); last-resort fallback is a
per-instance one-closure setter registered in the ctor (proven mechanics, one closure
per *entity*, pk-only). Probe-selected like FromRow; surface fixed.

### 2.3 Converters — the witness idiom (landed capabilities, no speculation)

```
namespace Atlantis::Orm {
    // toDb: one overload per mapped type (§7). fromDb: witness-typed overloads —
    // the field's current value pins the return type; overloads own all coercion.
    int      fromDb(string | int | float | bool | None v, int witness)      { … }
    int?     fromDb(…, int? witness)      { … }   // None ⇔ SQL NULL
    string   fromDb(…, string witness)    { … }   // + string?, float/float?, bool/bool?
    DateTime fromDb(…, DateTime witness)  { … }   // DATETIME text ↔ DateTime, §7
    // Enums ride the generic tail — LA-18 T::member, landed:
    T fromDb<T>(string | int | float | bool | None v, T witness)
        => T::fromCode(Atlantis::Orm::asInt(v)) ?? witness;
}
```

Concrete overloads win by specificity (Track 03 P-3 proved T/T? ranking); enum fields
fall to the generic tail, where `T::fromCode` monomorphizes per LA-18 (landed; and the
old #54 generic-overload hazard was **fixed 2026-07-15**). A non-enum unmappable field
type fails *here* at compile time, naming the instantiation — the §1.2 guarantee.
Coercions owned: text-protocol digit strings → int/float (strict), `0/1` → bool, NULL
into non-optional → `MappingException` naming table+column+fix.

---

## 3. Queries — Leviathan lambdas over a reified tree (LA-31)

### 3.1 The surface

```
Array<User> vips = await db.users
    .where((u) => u.active && u.name.like("A%"))
    .orderBy((u) => u.name)
    .take(50)
    .with((u) => u.posts)
    .all();

int n = await db.users.where((u) => u.lastSeen < cutoff)
                      .set((u) => u.active = false);       // bulk UPDATE, §6.1
```

`u` is typed `User` by ordinary inference. A typo'd member is an ordinary compile error.
`like` is a real method on `string` (LA-31 ships it in the prelude with pinned
semantics, §3.4), so `u.age.like(…)` is an ordinary type error — the v1 `TypedColumn`
inconsistency is not fixed but *unexpressible*.

### 3.2 The mechanism — `Expr<F>`, one lambda, two executions

LA-31 (`request-expr-reification.md`): a parameter typed `expr::Expr<(U) => bool>`
accepts a lambda literal; the compiler emits the ordinary **closure** *plus* generated
constructor calls building the body's **tree** (`expr::Node` prelude classes) *plus* a
**binds array** (each captured value's current value, in slot order) and a
**compile-time-unique `siteId`**. All of it is ordinary emitted code — visible under
`--expand`, zero runtime reflection, cost O(nodes) per call.

| leg | consumer |
|---|---|
| tree + binds → SQL + `?` params | the renderer (§3.5) → C3 driver — today |
| closure | in-memory repos for tests (DI-swapped); relation stitching; any future engine that executes Leviathan natively |

Captured values ride as binds — **user input cannot reach the SQL text, by
construction**. Literals in the lambda render inline (they are compile-time constants),
keeping each call site's SQL string stable → the driver's prepared-statement cache turns
steady-state execution into: bind params → binary-protocol execute. No string assembly
per request; `siteId` reserves a render-memo slot (designed; lands only if profiling
shows the µs-scale render matters).

### 3.3 The reifiable subset (compile-error outside it, at the site)

Field access on the lambda parameter (chained fields = a path, §3.6); literals; captured
locals/params (→ binds); `== != < <= > >=`; `&& || !`; `+ - * / %`; `== None` /
`!= None` on `T?` fields (→ `IS NULL` / `IS NOT NULL`); enum comparisons (carrier int);
whitelisted calls: `string.like / ilike / startsWith / endsWith / contains`,
`Array<T>.contains(x)` (→ `x IN (…)`, per-element binds; empty array → constant FALSE);
single assignment to a parameter field (`set()` bodies only). Anything else — method
calls off the whitelist, `await`, closures-in-closures, mutation in a `where` — is a
compile error naming the construct and pointing at the whitelist doc.

### 3.4 The consistency law (the differential-corpus discipline, applied to queries)

Every whitelisted operation has **two implementations that must agree**: the in-memory
method and the SQL rendering. The corpus runs each op both ways — closure leg against
fixture arrays, SQL leg against the driver — and diffs verdicts, the same
oracle-vs-LLVM discipline this repo already lives by. Pinned semantics v1:

- `like(pat)` — byte-exact wildcard match (`%`,`_`), **case-sensitive**; renders
  `col LIKE BINARY ?`. Deterministic on every engine, no collation dependence.
- `ilike(pat)` — ASCII-case-insensitive; renders `LOWER(col) LIKE LOWER(?)`. The ASCII
  guarantee is the contract; beyond ASCII, DB collation governs (documented bound,
  corpus covers ASCII).
- `startsWith/endsWith/contains` — render as `LIKE BINARY` with `%`-composed,
  wildcard-escaped bind (`\%`, `\_`); in-memory legs are the existing string methods.
  Byte-exact both legs.
- Comparisons on `DateTime` — both legs compare the same normalized value (§7).

### 3.5 `Query<E>` (immutable chain; exactly one SQL per terminal)

```
class Query<E : Model> {
    Query<E> where(expr::Expr<(E) => bool> p);        // multiple where = AND
    Query<E> orderBy(...key selector...);  Query<E> orderByDesc(...); // + thenBy pair
    Query<E> take(int n);   Query<E> skip(int n);
    Query<E> with(...member path...);                 // §5.3
    Query<E> noTrack();                               // §4.4
    Query<E> withDeleted();                           // §5.5
    Promise<Array<E>> all();      Promise<E> first();          // empty -> EmptyResultException
    Promise<E?> firstOrNone();    Promise<int> count();  Promise<bool> exists();
    Promise<int> set(expr::Expr<(E) => int> assignment);  Promise<int> delete();   // §6.1
    SqlAndParams toSql();                             // inspection, diff-stable
}
```

`all/first/firstOrNone` ⇒ one SELECT (LIMIT applied); `count` ⇒ `SELECT COUNT(*)`;
`exists` ⇒ `SELECT 1 … LIMIT 1`. The chain never round-trips early.

### 3.6 Target as path

The tree's FROM target is `__ormPath()` — `Array<string>` segments, not a bare name.
The MySQL renderer joins/quotes segments (`schema`.`table` when two). Rationale
recorded: the owner's storage direction is hierarchical
(`root.db1.group3….users`); a path costs nothing today and never hardcodes "table" as
the only addressable unit. v1 `@Table` carries one segment.

### 3.7 What stays open for the future engine (deliberately)

No plan format, no wire shape, no execution model is promised to LeviathanDB. What this
design keeps open, at zero cost: the tree and the closure are **both** first-class legs
(an engine that runs Leviathan natively can take the closure; one that wants plans can
take the tree); targets are paths; the renderer is one replaceable consumer of the tree
behind the seam. Models are typed **views** — nothing below `Repo` knows models exist
(the C3 seam is SQL+params+rows; `raw`/`rows<T>` are co-equal citizens underneath).

### 3.8 Before LA-31 lands (the M1 floor — no throwaway surface)

`find(pk)`, `all()` (unfiltered), the write path (§4), and the raw hatch (§6.2) ship
first and don't need the reifier. **No string/fragment/descriptor query surface is built
in the interim** — that was v1's mistake twice over. The lambda surface activates in M2
when LA-31 lands (owner-built; the same-week precedent: threads, TLS, readonly,
target::).

---

## 4. The unit of work

### 4.1 Context and repos

```
class Db {                                    // ONE per request (H-5); never shared
    new (Atlantis::Data::IDbPool pool);
    Repo<E> repo<E : Model>(() => E make);    // factory closure; P8 re-pins identity aliasing
    void add(Model e);      void remove(Model e);      void removeHard(Model e);
    Promise<void> save();
    Promise<T> transact<T>((Db) => Promise<T> body);
    Promise<void> load<E : Model>(Array<E> es, ...member path...);   // §5.3
    Promise<Atlantis::Data::ResultSet> raw(string sql, Array<string | int | float | bool | None> params);
    Promise<Array<T>> rows<T>(string sql, Array<string | int | float | bool | None> params);  // §6.3
}
// App side:
class AppDb : Db {
    Repo<User> users;  Repo<Post> posts;
    new (Atlantis::Data::IDbPool p) : Db(p) {
        this.users = this.repo<User>(() => User());
        this.posts = this.repo<Post>(() => Post());
    }
}
```

Request scope = the object you make per request (H-5): `AppDb` is constructed by the
composition root per request (or injected via the request `Context`), never a global.
Under workers (LA-1): **one pool per worker, one `Db` per request, connections never
cross workers** — fd-bound carriers cannot cross threads anyway; the design makes the
safe shape the only shape.

### 4.2 Identity map

Keyed `"<joined path>:<pk>"` per `Db`. `Repo` materialization consults it **before**
`FromRow`: a hit returns the existing instance and skips materialization entirely (the
map is also a speed feature); a fresh row's instance is registered after seal. Same PK ⇒
same instance within a context; in-memory dirty state is never clobbered by a re-read
(EF semantics, documented loudly).

### 4.3 `save()`

Order: **INSERTs, UPDATEs, DELETEs**; one transaction self-wrapped when more than one
statement is pending. Specifics:

- **INSERT** — pending-new grouped **per entity class → one multi-row
  `INSERT … VALUES (…),(…),…`** (single prepared statement, N×k params; MySQL's 16MB
  packet bounds it far above any sane unit of work). `@AutoIncrement`: MySQL guarantees
  consecutive ids for a multi-row insert under `innodb_autoinc_lock_mode ∈ {0,1}`;
  under mode 2 (interleaved) consecutiveness can break, so the pool's bootstrap query
  reads `@@innodb_autoinc_lock_mode` once — mode 2 downgrades that class's batch to
  per-row INSERTs, correct over fast, logged. Write-back via `__ormSetPk`, then seal.
  Parents-before-children is add-order (documented v1; topo-sort by `@BelongsTo` is a
  v1.1 line item).
- **UPDATE** — identity-map scan for `isDirty()`; SET lists **only** `dirtyCols()`;
  WHERE pk. Then re-seal.
- **DELETE** — `remove()` on a `SoftDelete` entity becomes
  `UPDATE … SET deletedAt = ?`; `removeHard()` is the real DELETE. Non-SoftDelete
  `remove()` deletes.
- **Timestamps discipline** — INSERT: `createdAt = updatedAt = now` before vals are
  taken; UPDATE: `updatedAt = now` *before* computing `dirtyCols()` (so a touch alone
  marks dirty). One `now` per `save()` (single clock read, uniform batch).
- **Re-entrancy** — `save()`/`transact()` set a `saving` flag; re-entrant use throws
  `OrmLogicException`. All statements of one `save()` run sequentially on **one**
  acquired connection, released on every exit edge (`using`-shaped) — the event-loop
  interleave hazard from v1's problems table, answered structurally.

### 4.4 `noTrack()`

Read-only lane: skips snapshot and identity-map registration (materialize → seal-less).
Listings that will never be saved pay zero tracking cost. Entities from a `noTrack`
query throw on `db.add`-less `save()` participation (they are simply never in the map).

### 4.5 `transact()`

`await db.transact((tx) => { … tx.users …; return r; });` — `tx` is a child `Db`
sharing the identity map, pinned to one connection with `BEGIN` open; commit on return,
`ROLLBACK` on any throw, connection released either way. Nested `transact` on a `tx`
throws (no savepoints v1). The C3 `ITransaction : IDisposable` seam underneath.

---

## 5. Relations — N+1 structurally unreachable

### 5.1 The wrappers (ruled 2026-07-18)

```
class Rel<T : Model>     { bool isLoaded();  T get();  T? orNone(); }
class RelMany<T : Model> { bool isLoaded();  Array<T> all();  int count(); }
```

Touching an unloaded relation throws `NotLoadedException` with the fix in the message:
`"relation 'posts' on User was not loaded — add .with((u) => u.posts) to the query, or
await db.load(users, (u) => u.posts)"`. Deterministic, fires in the first test that
touches the path; there is **no code path that silently issues N queries** and none
that silently reads empty.

### 5.2 Declarations

`@HasMany("App::Models::Post", fk: "userId")` on the parent; `@BelongsTo("App::Models::User")`
on the child rides its own FK column (`fk:` defaults to `<fieldName>Id` — a `Rel<User>
author` expects `int authorId`; boot-validated). Entity names are strings v1 (LA-27
upgrades to checked member references; attributes swap field types, consumers
unchanged).

### 5.3 Loading — always explicit, always batched

- `.with((u) => u.posts)` on a query — the include is a reified **member path** (the
  trivial `Expr` subset), so a typo'd relation is a compile error.
- `await db.load(users, (u) => u.posts)` — batch-load for a **collection you already
  have** (the place real codebases breed N+1). One query.
- Mechanics (both): collect parent pks → `SELECT … WHERE fk IN (?,…)` → group in
  memory → assign into each parent's wrapper. One extra query per relation, no JOIN
  row-explosion, identity map applies to the children.

Relations land at M3 (post-LA-31); **no string-named interim spelling ever ships** —
one spelling, compile-checked, forever.

### 5.4 Many-to-many (the owner's self-join ruling, honored with today's dialect)

`@ManyToMany(through: "App::Models::Relationship", a: "user1", b: "user2")` — loading
`u.friends` = one through-table query `WHERE user1 = ? OR user2 = ?`, projecting the
non-matching side per the owner's pipe ruling ("whichever one isn't self"); asymmetric
M2M names `a:` as the self side and `b:` as the projection. Same batch mechanics as
§5.3. String member names v1; LA-27's `self:`-pipe spelling replaces them verbatim when
it lands.

### 5.5 SoftDelete default scope

`E : SoftDelete` (known at rule time via `C.hasBase`) ⇒ every `Query<E>` appends
`deletedAt IS NULL` unless `.withDeleted()`. Explicit, greppable, opt-out; bulk ops
(§6.1) respect it too.

---

## 6. Bulk ops, the raw hatch, and the dense lane

### 6.1 Set-based writes (no load-modify-save loops)

`query.set((u) => u.active = false)` → one `UPDATE … SET … WHERE …` (assignment expr
from the reified subset; chain `.set()` for multiple columns);
`query.delete()` → one DELETE — or tombstone UPDATE under SoftDelete. Both return
affected-row counts. Neither touches the identity map (documented: bulk ops bypass
tracking — the enterprise-standard contract).

### 6.2 Raw SQL — the bazooka room stays open (ruled 2026-07-18)

`db.raw(sql, params)` → `ResultSet`; `repo.sql(sql, params)` → `Promise<Array<E>>`
materialized through the same `FromRow` path, result columns checked against
`allCols()` with a `MappingException` listing both sets on mismatch. Always
parameterized. Greppable (`grep "\.raw(\|\.sql("`). Demoted, never deleted.

### 6.3 `@Row` structs — the columnar fast lane (ruled 2026-07-18: v1)

```
@Row struct UserStat { int userId; int postCount; }

Array<UserStat> stats = await db.rows<UserStat>(
    "SELECT userId, COUNT(*) AS postCount FROM posts GROUP BY userId", []);
```

One rule (`match @Row on struct S` — struct-subject, no pairing issue) generates
`new FromRow(Row r)` binding fields by name via the same converters; `rows<T>` is one
generic method calling `T::FromRow(r)` — LA-18, landed. A pure-scalar `@Row` struct
(**int/float/bool/char only**) lands in a **columnar array**: tag-free 8-byte columns,
~4.5× field-scan, ~2× memory (the ABI does this automatically — §7.4 of the reference).
A `@Row` struct with a `string` field stays row-major dense — still unboxed, still no
tracking, still fast. No identity, no snapshot, no heap-per-row: the read-heavy lane
Leviathan uniquely offers. (Lambda-shaped projection `select(...)` over entities is a
designed v2 on the same reifier; raw SQL feeds the lane v1.)

---

## 7. Type mapping (single authority)

| Leviathan field | DbValue leg | MySQL DDL suggestion | notes |
|---|---|---|---|
| `int` | int | BIGINT | |
| `int?` | int \| None | BIGINT NULL | |
| `float` / `float?` | float | DOUBLE | |
| `bool` / `bool?` | bool (accepts `0/1`, `"0"/"1"`) | TINYINT(1) | |
| `string` / `string?` | string | VARCHAR(255) (suggested; human owns DDL) | byte-clean |
| `DateTime` / `DateTime?` | string `"YYYY-MM-DD hh:mm:ss[.ffffff]"` UTC | DATETIME(6) | normalized UTC both directions; comparisons consistent both legs |
| `enum E` / `E?` | int (carrier) | INT | `toDb: e.code()`; `fromDb`: generic tail `T::fromCode` (§2.3); unknown carrier in a row → `MappingException` |
| `Rel<T>` / `RelMany<T>` | — | — | never columns |
| anything else | — | — | compile error at the converter (§1.2); §8 sentinel pre-empts with a named message |

NULL into non-optional → `MappingException` (table, column, "make the field `T?` or add
NOT NULL"). `@NotNull string? x` → boot error (§8). Unsigned-BIGINT-overflow strings
from the driver into `int` → `MappingException` naming the column (v1 scopes ids to
signed 64).

---

## 8. Boot validation (dev mode) — one guard, three duties

`Db` construction (with `validate: true`, the dev default; prod default off, one flag)
runs, per registered repo:

1. **C1 silent-no-fire duty** (mandated by the amendment): instantiate via the factory,
   call `__ormPath()` — the `Model` throwing default converts "forgot
   `uses Atlantis::Orm` / forgot `@Table`" into a boot error naming the class and the
   fix.
2. **Schema drift**: `DESCRIBE <table>` vs `allCols()` — missing column ("field X has no
   column — @Ignore it or migrate"), extra NOT-NULL-no-default column ("column Y is not
   mapped and not defaultable"), `@NotNull` vs `T?` polarity mismatches, `@BelongsTo` FK
   field existence, collided mixin column names (§1.3), reserved-word column names
   (rename advice until LA-4 renames + driver quoting).
3. **Rule-time guards** (compile time, not boot — the Track 03 sentinel pattern: a
   `where`-clause helper that throws): exactly one `@PrimaryKey`, and it is `int`;
   `@AutoIncrement` only on the pk; `@Ignore` on a `Rel` field (redundant → error);
   unmappable field types by `$f.type` string for the friendly message.

---

## 9. The speed ledger (why each choice is fast — the contract M-gates assert)

| lever | mechanism | cost at steady state |
|---|---|---|
| statement reuse | stable per-call-site SQL (literals inline, captures bound) → driver stmt cache → **binary protocol** | bind + execute; zero string work (render is µs; siteId memo reserved) |
| materialization | generated `FromRow`, zero closures, identity-map short-circuit | one entity alloc + k converter calls |
| tracking | snapshot = COW alias (O(1)); diff only at `save()` | one vals-array build per saved entity |
| writes | dirty-cols-only UPDATE; **multi-row INSERT** per class; one transaction | minimal statements, minimal bytes |
| reads that skip it all | `noTrack()`; `@Row` structs → dense/columnar arrays | no map, no snap; column scans ~4.5× |
| relations | one `IN` query per relation, batched over collections | never N+1, never JOIN row-explosion |
| pool/driver | Track 05: prepared cache, pipelined FSM, `SO_REUSEPORT`-ready worker shape | landed |

---

## 10. Migrations (carried over from v1 — the one part that was right — condensed)

`migrations/*.lev` classes (`name()`, `parent()`, `Array<string> up()` — authored SQL,
up-only v1); explicit `migrations/index.lev` registry (scaffolder appends);
`./myapp migrate` (Track 04's app-hosted command layer): ensures
`schema_migrations(name, hash, appliedAt)`, applies pending in chain order, each in a
transaction (MySQL DDL auto-commits; the transaction protects bookkeeping — documented).
**Content-hash chain**: `hash = sha256(parentHash + name + up-SQL)`; two migrations
claiming one parent = hard error with the merge recipe; applied-hash mismatch = tamper
error. `./myapp migrate new <slug>` scaffolds with `parent()` = head and prints
suggested DDL from `allCols()` + a generated `__ormColTypes()` (§7 table). Suggested
only — the human owns the SQL.

---

## 11. P-probes (M0 — run before any feature work; failures with a metaprog-shaped cause → `/bug.md` or an LA, never a hack)

| # | Probe | Proves | Fallback |
|---|---|---|---|
| **P1** | Rules + attributes declared in `Atlantis::Orm`; template calls `Atlantis::Orm::toDb`; expand + run, 3 engines | Amended-C1 placement: nested-ns def-site qualification (the Track 03 §5.3 resolver-bug check) — **gates the whole subsystem-rules model, run first** | helpers to `Atlantis` root as `__orm*` (rules/attrs stay in `Orm`); file the bug |
| P2 | §2.1 `ormVals` template verbatim (read-side member-selector splice in `$for`) | the landed splice in our exact shape | none expected; regression → bug.md (reference.md documents it landed) |
| P3 | `this.$f = …` as assignment **target** in a `$for` element (Shape A) | FromRow Shape A | Shape B (proven, Track 03 P-1b/P-2) |
| P4 | per-field rule injecting a **member** (`__ormSetPk`) vs ctor-append only | setPk shape | single-element `$for` (P3); else per-instance pk-setter closure |
| P5 | encloser constraint `in class C : Model` (base **class**, not interface) | Shape B's match + mixin-aware rules | constraint via `C.hasBase("Model")` in `where` |
| P6 | generic-tail `fromDb<T>` + `T::fromCode` on an enum field; concrete overloads still win for scalars | enum columns; §1.2 compile-error guarantee | per-enum overloads generated at namespace anchor (Track 03 §4.3 pattern) |
| P7 | `$f.type` spellings for `Rel<Post>`/`RelMany<Post>`/`enum` fields in `where` filters | §2.1 filters, §8 guards | adjust string predicates to actual spellings |
| P8 | identity aliasing: `Array<E>` of class refs, mutate via map hit, observe via array; repo factory-closure generics | §4.2; `repo<E : Model>` typing | non-generic `Repo` + generated typed facades (v1 fallback, unchanged from old P4a) |

LA-31 acceptance carries its own probe set (differential corpus per whitelist op) — in
the ticket, not here.

## 12. Foreseeable problems

| Problem | Risk | Answer |
|---|---|---|
| P1 trips the nested-path resolver bug | Medium — bug was open at Track 03 time | Fallback is one-line helper relocation; bug filed; **rules/attributes stay subsystem-owned** either way |
| LA-31 slips | Medium | M1 floor is complete and useful without it (CRUD/UoW/raw/@Row); M2+ activate on landing; escalate priority per R4 — no interim query DSL will be built |
| Whitelist-op semantic drift (collation, Unicode) | Medium | §3.4 pins; differential corpus enforces; non-ASCII `ilike` bound documented |
| Multi-row INSERT id write-back under interleaved autoinc | Low | `@@innodb_autoinc_lock_mode` probe at pool bootstrap; mode 2 → per-row inserts for correctness (§4.3) |
| Mixin columns invisible (LA-21) | Accepted v1 bound | framework mixins hand-contribute; user mixins refused loudly with the LA named (§1.3) |
| Event-loop interleave during `save()` | High if ignored | one pinned connection + `saving` flag + per-request `Db` (§4.3) |
| Snapshot aliasing | None by construction | scalar `Array<DbValue>` values; COW |
| `FromRow` shape divergence A/B | None user-visible | byte-equivalence acceptance (M1) — same discipline Track 03 pinned |

## 13. Milestones (AG-4 unchanged: 2026-11-10)

| M | Scope ("done" =) | Target |
|---|---|---|
| **M0** | All §11 probes run + logged (P1 first); LA-31 ticket filed (done, with this doc); bug.md entries for surprises | 2026-07-25 |
| **M1** | `Model` + mixins + generated members (probe-selected shapes, byte-equivalence pinned); `Db`/`Repo`/identity map/`save()` (multi-row INSERT, dirty UPDATE, Timestamps/SoftDelete)/`transact()`/`noTrack`; `find`/`all`; `raw`/`sql`/`rows<T>`/`@Row`; §8 boot validation; green vs FakeDriver **and** the Track 05 loopback fake, oracle+IR+LLVM | 2026-08-21 |
| **M2** | LA-31 landed (owner) → `where/orderBy/take/skip/first/firstOrNone/count/exists/toSql`; renderer + binds; consistency corpus (closure-vs-SQL differential, every whitelist op); `set`/`delete` | 2026-09-25 |
| **M3** | Relations: `Rel`/`RelMany`, `.with`, `db.load`, M2M-through, SoftDelete scope + `withDeleted` | 2026-10-20 |
| **M4 = AG-4** | Migrations end-to-end; real MySQL 8 acceptance (Track 05 §7.2 job); demo-app entity corpus on oracle+IR+LLVM; speed ledger asserted (captured-SQL tests: statement counts, dirty-only SETs, one-SQL-per-terminal) | 2026-11-10 |

## 14. STOP conditions

- P1 fails **and** the root-helper fallback also fails → the amended-C1 placement model
  is broken program-wide: STOP, escalate to owner (affects Tracks 03/04 retrofits too).
- Any temptation toward runtime reflection, a second query surface (string/fragment/
  descriptor — v1's corpse stays buried), a second persistence model, or editing
  `src/**` → STOP per overview §0.4.
- LA-31 scope creep pressure (translating non-whitelisted constructs "just this once")
  → STOP; the whitelist grows only by ticket amendment with both legs + corpus.
- C3 shape changes needed → frozen-contract escalation.
- A probe failure contradicting a **landed** capability's documentation → `/bug.md`
  with repro, never a workaround.

## 15. Language-ask register notes (for the overview owner)

- **LA-31 (NEW, P0/AG-4):** expression reification — `expr::Expr<F>` + prelude
  `expr::Node` taxonomy + `string.like/ilike`. Ticket:
  `designs/requests/request-expr-reification.md` (filed 2026-07-18). Consumer: this
  track (M2+); deliberately language-owned, not ORM-owned.
- **LA-21 (E) pull:** inherited/mixin field visibility (`C.allFields`) — unlocks
  user-defined persisted mixins (§1.3). P2→P1 at owner's discretion.
- **LA-27:** unchanged (checked member refs in relation attributes — swap-in upgrade,
  §5.2/§5.4). **LA-4:** column renames — post-v1, plugs into `__ormCols()` only.
  **LA-6:** statement-`$for` — deletes Shape B's scratch machinery if P3 forces it;
  ergonomic, not blocking.
- **LA-17 ($ident) and LA-24 (comptime expression trees as previously scoped): dead for
  this track** — descriptor codegen is purged; LA-31 supersedes LA-24's intent.

## 16. Implementation log (append-only)

- 2026-07-18 — v2 authored, replacing the 2026-07-06 draft in full, per owner direction
  (purge + rebuild). Rulings recorded in-session: Leviathan-lambda query surface (LA-31
  filed); no LeviathanDB commitment (tree stands on today's merits; target-as-path);
  raw hatch retained; fields-are-columns default-IN + `@Ignore` + boot validation;
  `Rel`/`RelMany` wrappers; `@Row` columnar lane in v1; namespace `Atlantis::Orm` under
  amended C1 (attributes/rules subsystem-owned). Probes not yet run; implementation not
  started. M0's P1 (nested-namespace rule placement) flagged as the first action and a
  framework-wide concern.
