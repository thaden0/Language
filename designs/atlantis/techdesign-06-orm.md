# Atlantis Track 06 — ORM

**Status:** draft for owner review. **Date:** 2026-07-06.
**Depends on:** Track 05 (contract C3 — developed against a `FakeDriver` until AG-3),
Track 03 (C7 `@Serializable`), `designs/request-metaprog-attr-values.md` (LA-4 — column
renames are designed-in against that ticket; interim fallback = field-name-derived columns).
**Owns:** namespace `Atlantis::Orm`; the C5 data attributes `@Table`, `@Column`, `@Id`,
`@HasMany`, `@BelongsTo` and their rules (which live in namespace `Atlantis` per C1).

---

## 0. Mission, scope, non-goals

**Mission.** The data layer that makes R2 real: entities are reference classes composed
from *stateful* multiple-inheritance mixins, change-tracked by compile-time-generated
members (H-6, zero runtime reflection), queried through compile-checked column descriptors,
persisted through a unit-of-work context, and migrated by app-hosted commands (R3). Every
byte of "magic" is greppable via `--expand`.

**Scope (v1, gates AG-4 2026-11-10):**
1. `DbTracking` + `Timestamps` + `SoftDelete` mixins — the MI showcase (§1).
2. Generated per-entity tracking members via `$for` over `@Column` fields (§2).
3. `Db` context: unit of work, identity map, `save()` batching, explicit `transact()` (§3).
4. Query builder over typed column descriptors — one SQL per terminal op; raw-SQL escape
   hatch with `.toSql()` inspection (§4).
5. Relations `@HasMany`/`@BelongsTo` with explicit `.include(...)`; N+1 impossible by
   default (§5).
6. Migrations: `.lev` migration classes, `./myapp migrate`, content-hash chain (§6).
7. DTO↔entity mapping stance (§7).

**Non-goals (v1):** lambda-to-SQL (no expression trees in the language — see final-note ask
"comptime expression trees"); lazy loading (never — explicit loading is a feature); JOINs in
the builder (relations = second query + stitch); `@Transactional` (Layer-D rewrites, LA-5);
schema *diffing* auto-migrations (v1 migrations are authored SQL; the scaffolder prints
suggested DDL); non-`int` primary keys; PostgreSQL (driver seam C3 is ready for it).

**vs Loom (R1), the one-line verdict.** Loom's data layer (proposal §7) got the research
right — explicit `include` kills N+1, compile-checked fields kill string lookups, raw SQL is
non-negotiable, hash-chained migrations kill branch conflicts. Atlantis **keeps all four
wins** and beats Loom where its struct-only model is structurally weak: value structs have
no identity, so Loom cannot dirty-track (every UPDATE is a full-row write and a lost-update
hazard) and cannot compose persisted behavior (structs are final — no `Timestamps`, no
`SoftDelete`; every model re-declares the fields and every save site re-implements the
discipline). Loom's `Users.where((u) => u.active)` also silently assumed lambda-to-SQL
translation the language does not have; Atlantis achieves the same compile-checking through
generated descriptors that exist *today* (§4).

---

## 1. THE SHOWCASE (R2): entities as multiply-inherited, stateful mixins

This is the section the framework leads with. The entity model *is* the multiple-inheritance
pitch:

```
uses Atlantis;

@Table("users")
class User : DbTracking, Timestamps, SoftDelete {
    @Column @Id int    id;
    @Column     string name;
    @Column     string email;
    @Column     bool   active = true;

    @HasMany("Post") RelMany<Post> posts;
}
```

Three bases. Each one carries **real state and real behavior** — not markers, not
interfaces, not code generation:

```
namespace Atlantis::Orm {
    class DbTracking {                       // identity + change tracking (§2)
        private Array<DbValue> __snap = [];  // last-persisted column values
        private bool __isNew = true;
        bool isNew() => this.__isNew;
        bool isDirty() { … }                 // field-wise compare vs __snap (§2.3)
        Array<string> dirtyColumns() { … }
        // + the generated-member seam: __ormCols()/__ormVals()/… (§2.1)
    }
    class Timestamps {                       // persisted audit columns + touch discipline
        @Column DateTime createdAt = DateTime::Epoch();
        @Column DateTime updatedAt = DateTime::Epoch();
        void touch(DateTime now) { this.updatedAt = now; }   // ctx.save() calls this (§3.4)
    }
    class SoftDelete {                       // persisted tombstone + default-scope filtering
        @Column DateTime? deletedAt = None;
        bool isDeleted() => this.deletedAt != None;
        // repositories add `deletedAt IS NULL` to every query by default (§4.5)
    }
}
```

**Why this is trivial here and painful everywhere else.** In C# and Java, interfaces cannot
carry fields — so "every entity has `createdAt`/`updatedAt`/`deletedAt` and the machinery
that maintains them" has exactly three industrial answers, all bad: copy the fields into
every entity (Java/JPA `@MappedSuperclass` — and you get *one* superclass, so timestamps OR
soft-delete OR tenancy, pick your single slot); generate the code (C# source generators,
Lombok — a build-time shadow language); or move the state out of the object into the
framework (EF Core shadow properties + runtime change-tracker proxies — runtime reflection,
the exact thing Atlantis forbids). Rails gets composability via `method_missing` and gives
up static analyzability entirely. In Leviathan the answer is: **write a base class with
fields, inherit three of them.** Mixins compose because multiple inheritance is real; state
lives where the behavior lives; `grep` finds it; `--expand` shows the rest.

**Field collisions are a language feature, not a hazard.** If a user mixin collides with a
framework mixin — say an `Auditable` mixin that also declares `updatedAt` — the language's
`distinct` modifier keeps separate per-source slots, reachable by qualification
(reference §4.3): a bare read of a collided member is a compile error until qualified, so
the collision is *loud at compile time* and resolved explicitly:

```
class Auditable { public distinct DateTime updatedAt = DateTime::Epoch(); … }
class Invoice : DbTracking, Timestamps, Auditable {
    void onSave(DateTime now) {
        this.Timestamps::updatedAt = now;    // the persisted column
        this.Auditable::updatedAt  = now;    // the audit trail's own slot
    }
}
```

Default (no `distinct`): later base wins — also deterministic. Either way there is no
diamond ambiguity at runtime and no MRO folklore: the program says which slot it means.
Column mapping under `distinct` collisions is P-probe **P1** (§8); v1 rule: a collided
column name is a **boot-time schema error** naming both sources (rename one column when
LA-4 lands; before LA-4, rename one field).

---

## 2. Change tracking with ZERO runtime reflection (H-6)

### 2.1 The generated-member seam (exact surface)

`DbTracking` declares six low-level members with throwing default bodies; the Atlantis rules
**override** them per entity with generated code. Everything else (`snapshot`, `isDirty`,
`dirtyColumns`, `toInsert`, `toUpdate`, `applyRow`) is *hand-written once* in `DbTracking`
on top of these six — keeping the generated surface minimal (H-2 discipline):

```
// Declared on DbTracking; each default body throws LogicException(
//   "ORM members not generated for ${...}: is the entity @Table-attributed and does its file `uses Atlantis;`?")
string           __ormTable();      // "users"                        (from $t.name)
string           __ormIdCol();      // "id"                           (the @Id field's name)
Array<string>    __ormCols();       // ["id","name","email","active"] (own @Column fields, decl order)
Array<DbValue>   __ormVals();       // current values, same order
Array<(Row) => void> __ormAppliers();   // per-column setters: row -> this.field
void             __ormSetId(int v); // lastInsertId write-back after INSERT
```

### 2.2 Rule mechanics

One class-level rule per member, anchored `at member of C`, matching `@Table(t) on class C`
— the **proven pattern from `tests/corpus/meta/rule_orm.ext`** (landed corpus test:
`$for f in C.fields.where((x) => x.hasAttr("Column"))` inside an array literal, columns
named by field name until LA-4 lands, then `$f.attr("Column").argStr(0) ?? $f.name`).
Sketches in today's dialect (`$for` is array-literal-position only — metaprog P3 §5):

```
rule ormCols {
    match @Table(t) on class C
    inject `Array<string> __ormCols() =>
        [ $for f in C.fields.where((x) => x.hasAttr("Column")) : "$f.name" ];`
        at member of C
}
rule ormVals {
    match @Table(t) on class C
    inject `Array<DbValue> __ormVals() =>
        [ $for f in C.fields.where((x) => x.hasAttr("Column")) : Orm::toDb(this.$f.name) ];`
        at member of C
}
rule ormAppliers {
    match @Table(t) on class C
    inject `Array<(Row) => void> __ormAppliers() =>
        [ $for f in C.fields.where((x) => x.hasAttr("Column")) :
            (Row r) => { this.$f.name = Orm::fromDb(r.byName("$f.name"), this.$f.name); } ];`
        at member of C
}
```

**Type dispatch without type splicing.** The element template is uniform across fields, so
per-field-type conversion cannot branch in the template. Instead `Atlantis::Orm` ships
generic converters and lets *inference at the splice site* pick the type — the **witness
trick**: `T fromDb<T>(DbValue v, T witness)` narrows the `DbValue` union via `match` and
returns `T`; the second argument (`this.$f.name`, the field's current value) exists only to
pin `T`. `toDb<T>(T v)` injects into the union. No overloading assumptions, no
`r.get<$f.type>` type-name splices. This is P-probe **P2a**; the injected-member-overrides-
base-method assumption is **P2b** (§8).

**Statement-position `$for` (LA-6)** would let `applyRow` be generated directly instead of
via the appliers-closure array; when it lands, `__ormAppliers()` collapses into a generated
`__ormApply(Row r)` with zero closure allocation. Designed-in, not blocked-on.

### 2.3 The hand-written half (DbTracking, no codegen)

```
void snapshot()             { this.__snap = this.__ormVals(); this.__isNew = false; }  // Arrays are COW values — O(1) alias, safe
bool isDirty()              => this.__isNew || this.dirtyColumns().length() > 0;
Array<string> dirtyColumns() {
    Array<string> cols = this.__ormCols();  Array<DbValue> now = this.__ormVals();
    Array<string> outp = [];
    for (int i = 0; i < cols.length(); i = i + 1)
        { if (!Orm::dbEq(now[i], this.__snap[i])) { outp = outp.push(cols[i]); } }
    return outp;
}
void applyRow(Row r) { … run each __ormAppliers() entry …; this.snapshot(); }
// toInsert(): (cols minus @Id when new, vals to match); toUpdate(): dirtyColumns() + vals + id — §3.3 consumes these
```

### 2.4 Mixin-declared columns

`meta` field iteration sees the matched class's **own** body only — `Timestamps`' columns
are invisible to `User`'s `$for`. Layered answer:

1. **Framework mixins need no rules at all.** `Timestamps`/`SoftDelete` are Atlantis source;
   they hand-implement a tiny contributor surface (`__mixCols()/__mixVals()/__mixAppliers()`
   on each mixin), and `DbTracking`'s aggregators probe `this` with `match`-by-type
   (`match (this) { Timestamps => …append… ; else => {} }`) — compile-time-known dispatch,
   no reflection. Finite, hand-composed, fast.
2. **User-defined persisted mixins** (v1 fallback pattern): a mixin class extends
   `Orm::EntityMixin`; the per-field rule
   `match @Column(c) on field f in class M : EntityMixin` injects
   `this.__regContrib("$f.name", () => Orm::toDb(this.$f.name), (Row r) => { … });`
   `at bottom of M.constructor` — the documented **per-field-match constructor-registration
   pattern** (overview LA-4 fallback; each match reads its own attribute's args, so
   `@Column("full_name")` renames work here *today*). Constructor chaining runs each mixin's
   ctor, so registrations accumulate in base order (P-probe **P2c**). Costs one closure pair
   per field per instance — correct now, upgraded to generated members when identifier
   synthesis (§4.2 ask) or statement-`$for` lands.

`DbTracking` aggregates: `allCols() = __ormCols() + mixin contributions` (same for vals /
appliers). §3–§6 consume only `allCols()`-level accessors.

---

## 3. Sessions, unit of work, repositories

Data-mapper-leaning per the research Loom compiled (models don't `save()` themselves) — but
with **tracked entities**, which is the part Loom's structs couldn't do.

### 3.1 The `Db` context

```
namespace Atlantis::Orm {
    class Db {                                   // one per request (H-5: request scope = objects you make per request)
        private IDbPool pool;                    // C3; bound in composition root
        private Map<string, DbTracking> identity = Map<string, DbTracking>();  // "users:17" -> instance
        private Array<DbTracking> pendingNew = [];
        private Array<DbTracking> pendingDelete = [];
        new (IDbPool pool) { this.pool = pool; }
        Repo<E> repo<E : DbTracking>(() => E factory) => Repo<E>(this, factory);
        Promise<void> save();                                    // §3.3
        Promise<T> transact<T>((Db) => Promise<T> body);         // §3.5
    }
}
// App side (examples/atlantis-demo):
class AppDb : Db {
    Repo<User> users;  Repo<Post> posts;
    new (IDbPool p) : Db(p) { this.users = this.repo<User>(() => User()); this.posts = this.repo<Post>(() => Post()); }
}
```

The generic-method + factory-closure shape is P-probe **P4a** (method-level generics with a
base constraint). Fallback if constraints/methods disappoint: non-generic `Repo` over
`DbTracking` + a thin generated typed facade per entity (same rule family as §4.2).

### 3.2 Repositories + identity map

`Repo<E>` materializes: `E e = this.factory(); e.applyRow(row);` — then consults the
context's identity map keyed `"${table}:${id}"`. **Same PK ⇒ same instance within a
context**: if the key is present, the *existing* instance is returned and the fresh row is
discarded (in-memory dirty state is never clobbered by a re-read — EF semantics, documented
loudly). `Array<E>` results hold references (entities are reference classes; pure-value
arrays copy the *array*, not the objects — P-probe **P4b** pins this).

The flagship flow from the brief:

```
u = User(); u.name = "Ada";  ctx.users.add(u);   // pendingNew
User v = await ctx.users.get(17);                 // identity-mapped
v.email = "new@x.com";                            // just a field write — tracking is passive
await ctx.save();                                 // 1 INSERT + 1 UPDATE (email only), batched
```

### 3.3 `save()` — batching from tracking state

Order: **INSERTs (add order — parents before children is the app's responsibility v1,
documented), UPDATEs (identity-map scan for `isDirty()`), DELETEs.** Each entity yields at
most one statement; UPDATE sets *only* `dirtyColumns()` (`UPDATE users SET email = ? WHERE
id = ?`) — minimal writes, the anti-lost-update win over full-row struct saves. After
INSERT: `e.__ormSetId(res.lastInsertId); e.snapshot();` after UPDATE: `e.snapshot()`.
If more than one statement is pending, `save()` self-wraps in a transaction. All statements
use `?` params via C3 — **always** (string-concatenated SQL from values is a design
violation; §4.6's escape hatch still parameterizes).

### 3.4 Timestamps / SoftDelete discipline

`save()` probes each pending entity by type: `Timestamps` ⇒ on INSERT set
`createdAt = updatedAt = now`; on UPDATE `touch(now)` *before* computing `dirtyColumns()`.
`SoftDelete` ⇒ `ctx.users.remove(u)` becomes `UPDATE … SET deletedAt = ?` instead of
`DELETE`; `removeHard(u)` is the explicit real delete.

### 3.5 Transactions

Explicit wrap, v1: `await ctx.transact((tx) => { … use tx.users …; return r; });` — `tx` is
a child context pinned to one acquired connection with `BEGIN` open; commit on normal
return, `ROLLBACK` on any thrown exception, connection released either way (`using`-shaped
internally). Nested `transact` on a `tx` throws v1 (no savepoints). `@Transactional` is
noted as future Layer-D (LA-5) — middleware/explicit-wrap covers v1 per the overview.

---

## 4. Query builder — compile-checked WITHOUT lambda-to-SQL

There are no expression trees in the language; Loom's `where((u) => u.active)` had no
translation mechanism. Atlantis keeps the headline win — **a typo'd column is a compile
error** — via *generated typed column descriptors*. ("Comptime expression trees" is flagged
as a possible future ask; nothing here designs on it.)

### 4.1 Descriptor types (`Atlantis::Orm`)

```
class TypedColumn<T> {
    string col;                                  // column name
    new (string col) { this.col = col; }
    Cond eq(T v)  => Cond("${this.col} = ?",  [Orm::toDb(v)]);
    Cond ne(T v)  => Cond("${this.col} <> ?", [Orm::toDb(v)]);
    Cond gt(T v)  / lt(T v) / ge(T v) / le(T v)  // same shape
    Cond in_(Array<T> vs)   // "col IN (?,?,…)" — placeholder per element; empty array ⇒ constant FALSE cond
    Cond isNull() => Cond("${this.col} IS NULL", []);   Cond isNotNull();
    Cond like(string pattern)   // present on all T v1; driver rejects non-text at execute — revisit with a TextColumn subtype
    OrderKey asc() / desc();
}
class Cond {          // composition tree; renders parenthesized SQL + ordered params
    string sql;  Array<DbValue> params;
    Cond and(Cond o) => Cond("(${this.sql}) AND (${o.sql})", this.params.concat(o.params));
    Cond or(Cond o)  => …;   Cond not() => …;
}
class Query<E> {      // immutable builder; every method returns a new Query (COW values underneath)
    Query<E> where(Cond c);           // multiple where = AND
    Query<E> orderBy(OrderKey k);  Query<E> take(int n);  Query<E> skip(int n);
    Query<E> include(string relation);       // §5 — boot-validated name v1, descriptor later
    Query<E> withDeleted();                  // §4.5
    Promise<Array<E>> all();   Promise<E> first();          // first() throws NotFoundException-shaped Orm error on empty
    Promise<E?> firstOrNone(); Promise<int> count(); Promise<bool> exists();
    SqlAndParams toSql();                    // inspection — Drizzle lesson, kept from Loom
}
```

**Exactly one SQL per terminal op** (`all`/`first`/`firstOrNone` ⇒ one SELECT with
LIMIT applied; `count` ⇒ `SELECT COUNT(*)`; `exists` ⇒ `SELECT 1 … LIMIT 1`). The chain
never round-trips early.

### 4.2 Generated descriptors (`cols`)

Target spelling (assuming the ask below lands — R4):

```
Array<User> vips = await ctx.users
    .where(UserCols.active.eq(true).and(UserCols.name.like("A%")))
    .orderBy(UserCols.name.asc()).take(50).all();
// typo: UserCols.naem  → compile error. Wrong type: .active.eq("yes") → compile error.
```

Mechanism: a rule on `match @Table(t) on class C` injects an **items fragment at the
`namespace` anchor** (metaprog P3 §8.3 — whole classes may be injected at namespace scope):
a companion class per entity whose fields are `TypedColumn<$f.type>`-typed and named
`$f.name` (field-name splices in member position are the proven part), plus one shared
instance the app reaches by a stable name. The **one missing capability is synthesizing the
companion's declared name** (`UserCols` from `C.name + "Cols"`): templates splice *values
into expressions*, not derived *identifiers into declaration-name position*. That is a new
Language Ask — **template identifier synthesis** (e.g. a `$ident(C.name, "Cols")` hole; or
the smaller variant, letting a string binding stand in declaration-name position) — to be
registered in overview §2 by the coordinator; P-probe **P3** first, since `--expand` may
already accept more than the docs promise.

**Interim fallback (works today, no new features):** `Orm::col<T>(string name)` string
factories, with every column name in a `Query` **validated at boot** against the entity's
generated `allCols()` when the app's `Db` subclass is constructed with `validateQueries`
(dev default on) — a typo is a *startup* error with the entity's real column list in the
message, not a 3 a.m. 500. Weaker than compile-time, still strictly ahead of
Django/Eloquent, and the flip to descriptors is mechanical when the ask lands.

### 4.3 SQL generation

`?` placeholders ALWAYS (C3); params ride alongside in order. Identifiers are emitted bare
v1 (column names derive from Leviathan identifiers — reserved-word collisions go through
the problems table); the driver seam owns quoting when LA-4 renames arrive with arbitrary
names. Rendering is deterministic ⇒ `toSql()` output is diff-stable for tests and logging.

### 4.4 Raw-SQL escape hatch (never trapped above SQL)

`ctx.users.sql("SELECT * FROM users WHERE email LIKE ? AND active = ?", [p1, p2])` →
`Promise<Array<User>>`: rows are materialized via the same `applyRow` path; result columns
are checked by name against `allCols()` and a missing/extra mismatch throws a
`MappingException` **listing both column sets**. `Db.raw(sql, params)` returns bare
`ResultSet` for truly shapeless queries. Both parameterized — no string-built SQL, ever.

### 4.5 SoftDelete default scope

If `E : SoftDelete` (known at rule time — `C.hasBase("SoftDelete")` gates a rule variant
that overrides generated `bool __ormSoftDeletes() => true;`), every `Query<E>` appends
`deletedAt IS NULL` unless `.withDeleted()` was called. Explicit, greppable, opt-out.

---

## 5. Relations — N+1 impossible by default

```
@HasMany("Post")   RelMany<Post> posts;     // on User; convention FK: post.userId
@BelongsTo("User") Rel<User>     author;    // on Post; rides the @Column int userId FK field
```

`Rel<T>` / `RelMany<T>` are load-state wrappers (`loaded: bool` + payload). Loading is
**always explicit**: `.include("posts")` on the query, or `await ctx.load(u, "posts")` for
one instance. Accessing an unloaded relation (`u.posts.items()`) throws
`LogicException("relation 'posts' on User was not loaded — add .include(\"posts\") to the query, or `await ctx.load(user, \"posts\")`")`
— loud, with the fix in the message (v1; a comptime lint is future work). This keeps Loom's
"N+1 hard to hit" research win with an honest v1 mechanism: Loom promised a compile-time
warning it had no way to implement (lambda analysis); Atlantis's runtime error is
deterministic, fires in the first test that touches the path, and never silently issues N
queries — the failure mode is a thrown exception, not a slow production query storm.

**Implementation: second query + in-memory stitch, never a JOIN** (v1): load parents; collect
ids; `SELECT … FROM posts WHERE userId IN (?,…)`; group by FK in memory; assign into each
parent's `RelMany`. One extra query per include, no row-explosion, identity map applies to
the stitched children too. Relation names in `include` are boot-validated against a
generated `__ormRelations()` list (same rule family as §2.1, iterating
`hasAttr("HasMany") || hasAttr("BelongsTo")` fields; the target entity name is the
attribute's *argument*, readable per-match — no LA-4 needed).

---

## 5R. R10 alignment (2026-07-07) — `Model` base, example attribute vocabulary, M2M self-joins

Overview ruling R10: the owner's `designs/atlantis/example/Models/` is the canonical
entity surface. Three alignments; §1–§5's mechanics (generated members, UoW, descriptors,
include) are unchanged underneath.

**(a) The base is named `Model`.** `class User : Model` (example) — `Model` IS this doc's
`DbTracking` renamed (same snapshot/dirty/identity machinery; the name `DbTracking`
survives only as the §2 machinery's internal description). The showcase composes on top,
unchanged: `class User : Model, Timestamps, SoftDelete` — mixins remain opt-in bases with
real state, `distinct` still arbitrates collisions (§1 stands as written, one rename).

**(b) Attribute vocabulary (C5 updated).** Field markers from the example:
`@PrimaryKey` (retires `@Id`), `@AutoIncrement` (new: DB-assigned key — `save()` reads it
back from `ExecResult.lastInsertId`), `@NotNull` (schema constraint + boot-checked against
`T?` optionality — a `@NotNull string? x` is a boot error). The example's grouped
statement form `@attr(PrimaryKey, AutoIncrement);` is LA-26 (splices ticket item F);
**interim spelling is individual decorators**: `@PrimaryKey @AutoIncrement int id;`.
Entity enums (`RelationType`) require Track 03's `enum` (STOP-blocked — overview §2 flag);
interim: int-backed with named comptime consts, mapped to INT columns.

**(c) Many-to-many through-entity relations — `@hasManyBelongsToMany` (gated on LA-27).**
The example defines a self-join M2M:

```
@hasManyBelongsToMany(App::Relationship, self: App::Relationship::user1 | App::Relationship::user2);
Array<User> friends;
```

Owner's semantic ruling (User.lev comment, adopted verbatim): *"`self:` marks both sides
of the pipe as 'match against the owning instance.' Since a pipe only ever has two
members, and both are now marked as self-candidates, the return column falls out
mechanically — whichever one isn't self — without needing a separate `symmetric: true`
toggle or a where-style clause to spell it out. The keyword carries the semantic; the
pipe stays doing only join-matching, not projection."* Semantics: loading `u.friends`
= rows of the through entity where `user1 == u OR user2 == u`, projecting the
non-matching side; asymmetric M2M (the common case) names one `self:` member and one
projection falls out the same way. Loading stays `.include`-explicit (§5's N+1 rule);
implementation = one through-table query + stitch, same shape as §5. GATES: named args +
member-refs in attribute args (LA-27, `request-metaprog-attr-values.md` addendum 2).
Until then: M2M via explicit repository methods over the through entity (works today,
no magic).

---

## 6. Migrations (R3: app-hosted)

- `migrations/` directory of `.lev` classes: `class M0001_CreateUsers : Orm::Migration {`
  `string name() => "0001_create_users"; string parent() => ""; Array<string> up() => [ "CREATE TABLE …" ]; }`
  (v1 is up-only, authored SQL — predictable, nothing hidden; `down()` is future).
- **Discovery without reflection:** `migrations/index.lev` exports
  `Array<Migration> all() => [ M0001_CreateUsers(), M0002_AddPosts() ];` — the scaffolder
  appends to it. (A rule-generated registration table at a `namespace` anchor is a later
  sugar; explicit index ships first.)
- `./myapp migrate` (dispatched by Track 04's app-hosted command layer, R3): ensures
  `schema_migrations(name, hash, appliedAt)`; applies pending in chain order, each inside a
  transaction (note: MySQL DDL auto-commits — the transaction protects the bookkeeping row,
  documented).
- **Content-hash chain (Loom §7.3's win, kept):** `hash = sha256(parentHash + name + joined up() SQL)`
  (Track 09 digest). Boot/migrate verifies one linear chain: **two migrations claiming the
  same parent = hard error** — "divergent migration heads M0007a/M0007b: merge branches,
  then `./myapp migrate merge` to scaffold a successor claiming the surviving head" — the
  Django-branch-pain killer. An *applied* hash mismatching its file = tamper error.
- `./myapp migrate new add_posts` scaffolds the next class with `parent()` = current head
  and **prints suggested `CREATE TABLE` DDL derived from the generated entity schema** —
  sourced from `allCols()` plus a generated `__ormColTypes()` (`$f.type` strings) mapped
  `int→BIGINT, string→VARCHAR(255), bool→TINYINT(1), float→DOUBLE, DateTime→DATETIME(6),`
  `T?→NULL-able`. Suggested only — the human owns the SQL (data-loss review stays human v1).

---

## 7. DTO ↔ entity mapping ("model casts") — honest v1

A generated `toDto<T>()` is **not real**: generics cannot iterate `T`'s fields (reference
classes are non-reifiable at comptime — permanently, per the overview), and cross-type field
matching would need one type's rule to read *another* type's meta, which the `meta.*`
surface deliberately does not expose. So, v1:

- Entities may be `@Serializable` (Track 03, C7) for direct JSON exposure in small apps;
  Atlantis marks all framework mixin/tracking fields `@JsonIgnore` so snapshots and
  tombstones never leak into payloads.
- The blessed enterprise shape: value-struct DTOs (R2) with **explicit mapping** — a labeled
  constructor `UserDto::FromEntity(User u)` next to the DTO. One line per field, compile-
  checked, zero magic; the demo app ships the pattern.
- Future: if cross-type meta ever lands, a `@MapsFrom("User")` derive-rule is the upgrade
  path; not designed on.

---

## 8. P-probes (run against `build/leviathan --expand` BEFORE feature work; failures with a metaprog-shaped cause → `/bug.md` or an LA, never a hack)

| # | Probe | Proves | Fallback if it fails |
|---|---|---|---|
| P1 | `class A { distinct DateTime t; } class B { DateTime t; } class C : A, B` + `@Column` on both; inspect slots + generated cols | Mixin field collision → `distinct` slots; collided-column detection at boot | Rule `where` rejects collided names with a fix-it (rename field) |
| P2a | `rule_orm.ext` extended: `$for` element `Orm::toDb(this.$f.name)` + witness-generic `fromDb(r.byName("$f.name"), this.$f.name)` in a lambda element | Value/applier generation + generic inference from witness arg | Per-field ctor-registration for values/appliers too (§2.4.2 pattern everywhere) |
| P2b | Base class method `__ormCols()` throwing; rule injects same-signature member on derived; call via base-typed reference | Injected member overrides base method (dynamic dispatch) | Route through interface `IOrmGenerated` implemented by injection; or registration pattern |
| P2c | Two-mixin chain, each ctor appends to an inherited Array field; assert order | Ctor chaining order for registration contributions | Explicit `initMixins()` call generated at entity ctor bottom |
| P3 | Rule injects `class Fixed { TypedColumn<int> id; }` items-fragment at `namespace` anchor; then attempt name-from-binding | Descriptor companion codegen; scopes the identifier-synthesis ask precisely | §4.2 interim: `Orm::col<T>` + boot validation |
| P4a/b | Generic method `repo<E : DbTracking>` + factory closure; `Array<E>` aliasing (mutate via one ref, observe via the array) | Context/repo typing; reference identity inside pure-value containers | Non-generic `Repo` + generated typed facades; identity map keeps canonical refs if arrays copy |

## 9. Foreseeable problems

| Problem | Risk | Mitigation |
|---|---|---|
| **Identity map vs pure-value collections** — if class references inside `Array`/`Map` ever deep-copied, "same PK ⇒ same instance" breaks silently | High if P4b fails | P4b probe first; identity map stores the canonical ref and `Repo` always returns map hits, so even copy-surprises converge |
| **Event-loop await discipline in `save()`** — sequential awaited statements on one connection; a second handler touching the same ctx/conn mid-save interleaves the transaction | High | One `Db` per request (H-5), never a shared global ctx; `save()`/`transact()` pin one pooled connection for their whole span and release on every exit edge; `Db` sets a `saving` flag and throws on re-entrant use |
| **TEXT/NULL edge mapping** — text-protocol cells arrive as strings; `DbValue::None` into a non-optional field; `0`/`1` vs bool | Medium | `fromDb` witness converters own coercion (strict `toInt()` parse, `"0"/"1"`→bool); NULL into non-optional ⇒ `MappingException` naming table+column+fix ("make the field `T?` or add NOT NULL"); FakeDriver fixtures cover every cell-kind × field-type cell |
| `lastInsertId` write-back for non-`@Id`d or composite keys | Low (v1 scopes to single `int` `@Id`) | Rule errors at compile time if `@Table` class lacks exactly one `@Id int` |
| FK ordering in `save()` (child INSERT before parent) | Medium | v1: add-order + docs; hash-checked in the demo corpus; topo-sort by `@BelongsTo` is a v1.1 line item |
| Reserved-word column names (`order`, `key`) emitted bare | Medium | Boot validation rejects a known reserved list with rename advice until driver-side quoting lands with LA-4 renames |
| Snapshot aliasing under COW | Low | `__snap` holds `Array<DbValue>` of *scalars* — value semantics make the snapshot immune by construction |

## 10. Milestones & acceptance (aligned to AG-4, 2026-11-10)

| M | Scope ("done" =) | Target |
|---|---|---|
| M1 | All §8 probes run + logged; `DbTracking`+mixins+generated members land; CRUD round-trip vs `FakeDriver`; dirty UPDATE touches only changed columns (asserted on captured SQL) | 2026-09-25 |
| M2 | Query builder: descriptors (or boot-validated interim), one-SQL-per-terminal asserted via `toSql()`; raw-SQL hatch + mismatch errors; SoftDelete default scope | 2026-10-15 |
| M3 | `Db` context: identity map, `save()` batching+transaction, `transact()`; relations with `.include` stitch + loud unloaded error; Timestamps discipline | 2026-10-29 |
| M4 (=AG-4) | Migrations end-to-end (`./myapp migrate`, `migrate new`, hash chain, divergence error); everything green against real MySQL 8 via Track 05 (AG-3); demo-app entity corpus runs on oracle + IR + LLVM | 2026-11-10 |

## 11. STOP conditions (per overview §0.4 — log, commit WIP, escalate; never improvise)

- Any §8 probe fails on a load-bearing mechanism (P2a/P2b especially) — that's an LA or a
  `/bug.md`, not a workaround sprint.
- Any temptation toward runtime reflection (a), a second query/persistence model (c), or
  editing compiler/toolchain (b) — including "just one helper in `src/`".
- C3/C5/C7 shape changes needed (e.g. a second `@Table` field for the companion name) —
  frozen-contract escalation, owner decides.
- Lambda-to-SQL "just for simple predicates" — no. The descriptor path or the escape hatch.
- LA-4 slips past 2026-10-15 with a customer-shaped need for snake_case columns — escalate
  priority rather than inventing a rename side-channel.

## 12. Implementation log (append-only)

- 2026-07-06 — Track 06 design authored (this doc). Probes not yet run; implementation not
  started. New ask surfaced for the register: **template identifier synthesis** (§4.2);
  future-flagged: **comptime expression trees** (§4 preamble), statement-`$for` already
  tracked as LA-6.
- 2026-07-07 (R10) — §5R added: base renamed `DbTracking` → `Model` (machinery unchanged,
  mixin showcase intact); attribute vocabulary aligned to the example (`@PrimaryKey`/
  `@AutoIncrement`/`@NotNull`, `@Id` retired, grouped `@attr()` = LA-26 with individual
  decorators interim); `@hasManyBelongsToMany` M2M with the owner-ruled `self:` pipe
  semantics designed in, gated on LA-27; entity enums flagged on Track 03's STOP.
