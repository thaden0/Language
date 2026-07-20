# Atlantis Track 01 — kernel test results

Executed 2026-07-12 with `build/leviathan` / `build/trident`. One shared `.expected`
per case, verified byte-identical across the **tree-walk oracle** (`trident run`),
the **IR interpreter** (`leviathan --plan … --ir`), and the **LLVM native binary**
(`leviathan --plan … --build-native`). (emit-C++ has no system/event-loop layer and
is not a server target, per C8; the frozen ELF backend is out of scope.)

Run from the repo root:

```
./build/trident run packages/atlantis/tests/corpus/kernel      # M1–M3 acceptance shape
./build/trident run packages/atlantis/tests/corpus/static      # M4 + SSE + facade
./build/trident run packages/atlantis/tests/corpus/loopback    # real traffic over a socket
./build/leviathan --run packages/atlantis/tests/probes/p1_function_values.lev
./build/leviathan --run packages/atlantis/tests/probes/p2_exceptions.lev
```

| case | what it proves | oracle | IR | LLVM |
|---|---|---|---|---|
| probe p1 function-values | C2 fold: lambda middleware in `Array`/field, `var` fn-locals, capturing return | green | green | green |
| probe p2 exceptions | C4 hierarchy: interface fields on exceptions, base-ctor, catch order, rethrow | green | green | green |
| corpus/kernel | Pipeline fold + Context + error→status mapping (404/400/422/413/500) + problem+json + BodyLimit + Health + Deadline + server-side-only 500 logging | green | green | green |
| corpus/static | P7 traversal canonicalization + serve/ETag(md5)/304/HEAD + `%2e%2e`/`%252e%252e` defense + SSE framing + Builder/App facade + IActionResponse | green | green | green |
| corpus/loopback | the `Server` (wrapping Track 09's hardened `HttpServer`) serving real traffic through the full pipeline with an in-process `HttpClient`; exception→status end-to-end; 500 internals never leak the wire | green | green | green |

**Milestone coverage** (design §12): M0 probes ✓, M1 pipeline/Context/Respond/seam/Server ✓,
M2 Log + AccessLog + ErrorMapper + problem+json ✓, M3 Health/BodyLimit/Deadline ✓,
M4 StaticFiles (canonicalization/types/ETag/304/HEAD; large-file streaming is
collect-then-send interim) ✓, M5 SSE/ChunkedBody surface ✓ (finite; unbounded push
awaits the streaming-response hook — `designs/requests/request-http-streaming-response.md`),
R10 Builder/App facade + IActionResponse ✓.

**Language findings filed** (bug.md): #37 qualified/nested namespace resolution (the
C1 namespace layout is expressed via nested-brace `namespace Atlantis { namespace Http
{ … } }` + `uses Atlantis::Http;` as the workaround), #38 struct positional
auto-construction, #39 typed lambda parameters.

---

# Atlantis Track 02 — routing & controllers test results

Executed 2026-07-13 with `build/leviathan` / `build/trident`, same three-engine discipline
as Track 01 above (oracle/IR/LLVM byte-identical against one shared `.expected`; emit-C++ and
the frozen ELF backend are out of scope for the same reasons).

```
./build/trident run packages/atlantis/tests/corpus/routing
```

| case | what it proves | oracle | IR | LLVM |
|---|---|---|---|---|
| corpus/routing | segment-trie precedence/backtracking (static > `:param` > `*wildcard`) + trailing-slash normalize + query-string strip + 404/405 (sorted `Allow`) + HEAD-via-GET; `Route<C>`/`Controller`/`App.AddRoute`/`useRouting`/`finalize` wiring (§0R); Era-A path/query/header/JSON-body binding helpers; explicit `Validation` (422 + field errors); `authDefault` in all three modes (`auth`/`noauth`/`explicit`, incl. the boot-error path); the `router.record(...)` escape hatch (§7); route-table introspection (Track 07 handoff shape) | green | green | green |

**Scope note (design §0R/§13):** this track ships **Era A only** — the explicit fluent
`Route`/`Controller`/`App.AddRoute` surface, which the design's own R10 revision made the
primary, "fully functional" path. The attribute-and-rule auto-wiring layer (§2 verb/prefix/
auth-marker rules, §5.1 attribute-driven validation-descriptor generation, the `@InjectRoutes()`
splice) is **not implemented** — it depends on metaprogramming asks not yet landed (LA-4
attribute-value reflection, LA-16 `$if`/identifier-splicing, LA-22 named splice), all already
filed in `designs/requests/request-metaprog-attr-values.md` and
`designs/requests/request-metaprog-splices.md` before this track began. The C5/routing
attribute *shapes* (`Get`/`Post`/.../`RoutePrefix`/`Required`/`MaxLen`/...) are declared (Layer
A, inert) per this track's ownership commitment, ready for the rule layer once those asks land.
P-probes P1–P10 (design §9) are therefore not run — they test the rule-based wiring path this
track doesn't attempt.

**Language findings filed** (bug.md), all found implementing this track: #40 nullable
narrowing blocks same-slot lazy-init assignment, #41 generic-constructor type-argument
inference doesn't propagate through a nested call argument or a fluent chain, #42 lambda-typed
free-function-return/local-variable declarations don't parse, #43 `uses` doesn't reach a
same-file namespace reopening (narrower #37 instance), #44 a two-`::`-hop qualified name fails
IR lowering, #45 a call-expression-as-callee chained call fails IR lowering, #46 a qualified
generic type argument doesn't type-match its bare spelling across a qualification boundary,
**#47 (P1) a `Map<K, Struct>` class field corrupts memory on the LLVM backend once it holds 3+
entries** — worked around throughout by indexing through a parallel `Array<Struct>` instead of
storing structs as map values.

---

# Atlantis Track 03 — serialization & DTOs test results

Executed 2026-07-13 with `build/leviathan` / `build/trident`, same three-engine discipline as
Track 01/02 above (oracle/IR/LLVM byte-identical against one shared `.expected`; emit-C++ and
the frozen ELF backend out of scope for the same reasons; JSON is now full-coverage on all four
active engines — bug.md #30 was fixed 2026-07-10, `docs/reference.md` §6.11 corrected).

```
./build/leviathan --run packages/atlantis/tests/probes/ser_p1a_value_hole_this_dollar_f.lev
./build/leviathan --run packages/atlantis/tests/probes/ser_p1b_perfield_ctor_splice_struct.lev
./build/leviathan --run packages/atlantis/tests/probes/ser_p2_ctor_append_ordering.lev
./build/leviathan --run packages/atlantis/tests/probes/ser_p3_optional_witness_overload.lev
./build/leviathan --run packages/atlantis/tests/probes/ser_p4_generic_body_overload_resolution.lev
./build/leviathan --run packages/atlantis/tests/probes/ser_p5_decl_hole_type_position.lev
./build/leviathan --run packages/atlantis/tests/probes/ser_p6_labeled_ctor_meta_visibility.lev
./build/leviathan --run packages/atlantis/tests/probes/ser_p7_struct_closure_staleness.lev
./build/leviathan --run packages/atlantis/tests/probes/ser_p8_no_base_list_anchor.lev
./build/trident run packages/atlantis/tests/corpus/serialization
./build/trident run packages/atlantis/tests/corpus/serialization_refusals   # EXPECTED TO FAIL — pins §5.2 diagnostics
```

| case | what it proves | oracle | IR | LLVM |
|---|---|---|---|---|
| P-1a `this.$f` in `$for` | Mechanism A (class-level `toJson` generation) is blocked exactly as the design's source reading predicted — `'$f' is an attribute value, not a member name` | FAIL (expected) | n/a (compile error, engine-independent) | n/a |
| P-1b per-field ctor splice, struct | Mechanism B (`this.$f` decl-hole into `bottom of C.constructor`) works, including on a **struct** | green | green | green |
| P-2 ctor-append ordering | R1 (class-level skeleton) → R2 (per-field) → R3 (seal) land in the SAME rule-injected ctor, in that order; R2 does not re-match R1's injected fields | green (`1011`) | green | green |
| P-3 `T`/`T?` witness overload ranking | required-vs-optional witness dispatch resolves correctly | green | green | green |
| P-4 generic body + overloaded call | **FAIL** — bug.md **#54**: resolves once at definition time, wrongly, differently per engine | oracle/IR agree (both wrong); LLVM disagrees with both | — | — |
| P-5 `$C` in type position + `$C::Label(...)` | nested-type decode dispatch (§4.3) works | green | green | green |
| P-6 labeled-ctor visibility in `meta::Class.methods` | `new FromJson(...)` is visible, named by its label | green | green | green |
| P-7 struct closure staleness | confirms structs snapshot `this` by value (kills closure-registry designs, §1) — oracle-only per the design's own "documentation-grade" bar; IR/LLVM rephrasings hit pre-existing bugs #42/#52, not new findings | green (`2,1`) | n/a | n/a |
| P-8 no base-list anchor | negative confirmation: no rule syntax adds a base/interface | FAIL-to-parse (expected) | n/a | n/a |
| corpus/serialization | struct DTO + entity class round-trip, `@JsonIgnore`, optional present/absent/null, nested DTO (required + optional), `Array<string>`, `DateTime`, unknown-keys-ignored, missing/wrong-type/non-integral/null-for-required/wrong-top-level-kind → `ValidationException`, §7 `parseAccept`/`negotiate`/`isJsonContent` fixtures | green | green | green |
| corpus/serialization_refusals | all five `__atlGuard` (§5.2) messages fire with the exact field-anchored text (bad Map key type, function-typed field, non-`T?` union field, reserved `__atl*` name, missing `: IJsonSerializable` pairing) | green (matches pinned diagnostic transcript) | green | green |

**Milestone coverage** (design §12): M0 probes ✓ (P-1a…P-8, all run and logged), M1 `Atlantis`
root helper library (`__atlEnc`/`__atlDec`/`__atlZipObj`/`__atlGuard`, `IJsonSerializable`,
`@Serializable`/`@JsonIgnore`) ✓, M2 rule set (keys + FromJson R1/R2/R3 + nested-type dispatch;
`toJson` generation NOT shipped — §3.2 escape hatch, see below) ✓, M3 policy hardening (unknown-
key ignore, missing/wrong-type/non-integral/null-for-required throw, 400-vs-422 split via
`__atlRequireObject`) ✓, M4 content negotiation (`parseAccept`/`negotiate`/`isJsonContent`) ✓.
**M5 (post-LA collapse) not attempted** — it rides LA-15/LA-6 landing, per the design's own
target ("M5 rides the owner's LA schedule").

**§3.2 escape hatch shipped as designed:** P-1a reproduced the exact predicted failure
(`this.$f` inside a `$for` array-literal element template), so `toJson()` is **hand-written**
on every `@Serializable` type in this track's corpus (the `IJsonSerializable` requirement makes
a forgotten `toJson()` a compile error, not silent non-generation, §2.1(c)). `__atlKeys()` and
`FromJson` generate automatically (Mechanism B, proven by P-1b/P-2). LA-15 (the unblock) was
**already filed** at P1 — item (A) of `designs/requests/request-metaprog-splices.md` —
independently converged from Tracks 02/03/04/06 before this track began; no new ticket needed.
LA-16 (`meta::Class.attrs`) was likewise already filed (item (F) of
`designs/requests/request-metaprog-attr-values.md`).

**New finding, not a bug — filed as a P2 ergonomics ask:** rule SUBJECT-kind matching
(`match @Attr on class C`) is exact and does **not** match `struct` declarations, unlike
ENCLOSER-kind matching (`in class C`, which deliberately also accepts `struct`/`interface`
ancestors). Every subject-position `@Serializable` rule therefore ships as an explicit
`...Class`/`...Struct` pair (10 rules, not 5) — mechanical, P-probed, zero runtime cost. Filed
as §4 of `designs/requests/request-metaprog-splices.md`.

**Side finding — stale documentation, corrected:** `docs/reference.md` §6.11 and several other
design docs (`designs/complete/techdesign-01-kernel.md`, `techdesign-09-web-foundations.md`,
and the not-yet-implemented `designs/sonar/techdesign-08-theming-di.md`) still cite bug.md #30
(`Map<K, recursive-class>` LLVM corruption) as blocking JSON on the LLVM backend. It was fixed
2026-07-10 (`designs/complete/techdesign-bug30-map-with-ownership.md`); this track's own corpus
(nested `Map<string, JsonValue>` objects, byte-identical oracle/IR/LLVM) re-confirms the fix.
`docs/reference.md` §6.11 corrected here; the Sonar theming doc's "avoid JSON, use in-language
TOML" premise is now stale and worth a look by that track's owner — out of this track's scope
to change.

**Language findings filed** (bug.md), found implementing this track: **#54 [P1]** a generic
function body's call to an overloaded free function resolves once at definition-check time
(not per-instantiation), and wrongly — oracle/IR agree on one wrong answer, LLVM disagrees with
both. Forced §5.1's monomorphic-overload fallback for `Array<T>`/`Map<string,string>`
encode/decode instead of a generic `__atlEnc<T>`/`__atlDec<T>`.

---

# Atlantis Track 04 — DI, config & app-bootstrap test results

Executed 2026-07-13 with `build/leviathan` / `build/trident`. One shared `.expected` per
case, verified byte-identical across the **tree-walk oracle** (`trident run`), the **IR
interpreter** (`leviathan --plan build/plan.lvplan --ir`), and the **LLVM native binary**
(`trident build … --out …` then run; the `[heap] …` line is a stderr diagnostic, compare
stdout only). (emit-C++ has no system layer and is not a server target, per C8; the frozen
ELF backend is out of scope.)

Run from the repo root (regenerate the plan per case before the IR leg):

```
./build/trident run   packages/atlantis/tests/corpus/config    # M1 config grammar + reads + section + dump
./build/trident run   packages/atlantis/tests/corpus/di        # M2/M4 lifetimes + @Injectable + nearest-wins shadow
./build/trident run   packages/atlantis/tests/corpus/app       # M3 command dispatch + exit codes
./build/trident plan  packages/atlantis/tests/corpus/<case> && ./build/leviathan --plan build/plan.lvplan --ir
./build/trident build packages/atlantis/tests/corpus/<case> --out /tmp/<case> && /tmp/<case>
```

| case | what it proves | oracle | IR | LLVM |
|---|---|---|---|---|
| corpus/config | §4.2 strict `.env` parser (comments/quoted+escape/raw/empty/trim), all-errors ConfigException (missing-`=`/invalid-key/duplicate/bad-escape/unterminated), §4.4 typed reads with reference-collector error accumulation, `require` throw, §5 prefix `section()`, §4.5 `dump()` secret redaction | green | green | green |
| corpus/di | §2.1 singleton (shared bind → 1,2), §2.2 transient (`=> Ctor()` → 1,1), chained factory bind (`=> Svc(inject IDb)`), §2.3 request-scoped (state via a per-request carrier param), §3.4 `@Injectable` validation (I-prefixed base passes), §7.4 nearest-wins block-scope shadow (the mock-install mechanism) | green | green | green |
| corpus/app | §6 `Atlantis::App::run(app, argv)` — default `serve`, app-defined commands with argv tails, `help` → 0, unknown → usage+1, handler non-zero exit propagates; P-7 fn-typed field in `Command` + `Array<Command>` + call-through exercised end-to-end | green | green | green |
| probe di-p1 (negative) | §3.4 `@Injectable` on a class with no interface base → named **compile** error at rule-eval time, program never runs (P-5) | green (errors as intended) | — | — |

**Milestone coverage** (design §10): M1 Config ✓, M2 composition-root lifetimes + P-8 nearest-wins
shadow (via the `=> Ctor()` form the design mandates) ✓, M3 App dispatcher + exit-code contract + P-7 ✓,
M4 `@Injectable` validation + P-5 ✓. M5 (generated-bindings successor) stays gated/unscheduled per §3R
(needs LA-22 `@InjectBindings();` splice).

**Coordinator note confirmed (design §12):** argv (`env::args()`) and exit codes (`env::exit`) landed
after the doc was written, so §6's "serve-only until argv lands" fallback is retired — `run` reaches every
command from day one. `run` still takes argv as a *parameter* (never reads the process), which is what lets
the corpus drive the full dispatcher on every engine.

**Design-vs-reality corrections (design §13 log):**
- **Error accumulation is a reference collector, not an `Array<string>` parameter.** The design's typed-read
  signatures (`readInt(key, fallback, Array<string> errs)`) can't accumulate under the language's value
  semantics — an Array parameter is a copy; `errs = errs.add(x)` rebinds the callee's local only (probe-proven:
  caller length stays 0). Shipped as `Atlantis::Config::Errors` (a reference class whose `add` mutates the one
  shared instance) — same intent, honest mechanism.
- **External entry points are reached bare via `uses`, never by deep qualification.** A doubly-nested free
  function (`Atlantis::Config::parseText`) does not resolve when spelled fully-qualified from outside the
  `Atlantis` tree — the IR backend cannot even lower the deep name. `uses Atlantis::Config;` + a bare call
  (`parseText(...)`, `Config`, `Errors`, `ConfigException`) resolves on every engine. Sibling-relative calls
  inside the tree (`Config::empty()` from `Atlantis::App`) work as they do elsewhere (`Http::jsonQuote`).
- **`@Injectable` validation uses the sentinel-guard idiom** (Track 03's `where … && false` + a throwing
  `meta::Class` guard), which supersedes design §3.4's "inject an unknown-return-type member" sketch (A16
  found bare-member injection is not reliably diagnosed). Same outcome — a legible compile error — sturdier path.
- **`Map()` needs a declared-target binding**, not a bare constructor argument, to infer K/V in a body (`Map<K,V> m = Map();`
  infers; `f(Map())` does not) — hence `Config::empty()` builds the map in a local. Flow narrowing does not carry
  past an early `return`/`continue`, so every optional guard uses `else` arms.

**Language findings filed** (bug.md), found implementing this track: **#55 [P2]** a bare named-function
reference stored in a function-typed field fails to resolve when dot-called (lambda-wrap works — the design's
own `Command`-handler idiom); **#56 [P2]** a block-scope `bind IFace => localVar;` (binding a local-variable
reference) yields an injection value whose method dispatch fails (the `=> Ctor()` factory form and top-level
`=> globalVar` singleton form both work — so the mandated test-shadow and singleton patterns are unaffected).
Repros kept at `packages/atlantis/tests/probes/di_p2_fnfield_named_ref.lev` and `di_p3_localvar_bind.lev`.

---

# Atlantis Track 06 — ORM results (2026-07-19)

Executed with `build/leviathan` / `build/trident` at head. Every corpus case
below is verified byte-identical across oracle / IR / LLVM via the new
generic runner:

```
./packages/atlantis/tests/runtests.sh          # all corpus dirs, 3 engines
./packages/atlantis-mysql/tests/runtests.sh    # incl. the new orm_loopback
```

## M0 probes (design §11)

| # | probe file | result |
|---|---|---|
| P1 | `orm_p1_nested_ns_rules.lev` | FAILED as the design feared (rules/attrs in a NESTED namespace never fired — three distinct failure spellings), filed as **#91**, then **fixed at source the same day (owner-directed)**: `Rules.cpp` now keys rule/attribute namespaces by full qualified path. Probe is the regression floor; amended-C1 placement (`Atlantis::Orm` owns its rules) ships as designed, **no fallback relocation needed** |
| P2 | `orm_p2_ormvals_read_splice.lev` | GREEN (3 engines) — `this.$f` read splice + `$for` filters. Correction to the design's §2.1 template: string reification is the BARE hole (`$f.name`), never `"$f.name"` (a quoted hole is a literal) |
| P3 | `orm_p3_assign_target_splice.lev` | GREEN (3 engines) — **Shape A holds**: `this.$f` as assignment target inside a `$for` element + assignment-as-expression. Shape B's scratch machinery was never built |
| P4 | `orm_p4_perfield_member_inject.lev` | GREEN — both the per-field member injection and the single-element-`$for` form |
| P5 | `orm_p5_encloser_base_class.lev` | GREEN — `in class C : Model` matches through a base CLASS |
| P6 | `orm_p6_generic_tail_fromdb_enum.lev` | GREEN — witness overload specificity + generic tail `T::fromCode` (LA-18), unknown carrier handling |
| P7 | `orm_p7_field_type_spellings.lev` | GREEN — `$f.type` spellings pinned: `int`, `int | None`, `DateTime`, bare enum name (`Kind`), `Rel<Post>`, `RelMany<Post>` |
| P8 | `orm_p8_identity_repo_generics.lev` | GREEN — identity aliasing through `Array<E>`/`Map<int,E>`; `E::Label` inside a generic METHOD and inside a lambda within it. **Finding:** explicit generic call args (`repo<User>(...)`) do not exist in the language — `E` rides inference (factory closures / witness parameters); `db.rows(sql, params, witness)` is the shipping spelling of the design's `rows<T>` |
| — | `miniorm/` | regression floor for **#92** (attribute class shadowing bare type names — found here, fixed at source same day) |

## Corpus (M1–M4)

| case | what it proves | oracle | IR | LLVM |
|---|---|---|---|---|
| corpus/orm | M1: generated members (path/pk/cols/vals/colTypes/notNull/entity/autoPk), Shape-A FromRow + mixin apply + seal, identity map (same pk ⇒ same instance, no re-query), multi-row INSERT + consecutive-id write-back + `@@innodb_autoinc_lock_mode` probe, dirty-cols-only UPDATE, SoftDelete tombstone remove, noTrack lane, raw/repo.sql (column-set check), `@Row` dense lane via `rows(sql, ps, witness)`, transact commit/rollback/nested-guard, §8 boot validation vs DESCRIBE, converter refusals (NULL-into-non-optional, unknown enum carrier), Orphan throwing defaults | green | green | green |
| corpus/orm_query | M2: LA-31 lambda queries — where/whereIn/orderBy(+Desc/thenBy)/take/skip/first/firstOrNone/count/exists/toSql, literals inline + captures as binds (stable SQL text), `== None` → IS NULL, composed wildcard-escaped LIKE binds, IN from Array.contains, `set()` (constant + computed), `delete()` (hard + SoftDelete tombstone + scope + withDeleted), **and the §3.4 differential: 10 predicates run on both legs (closure vs embedded `expr::eval` over tree+binds) — all verdicts agree** | green | green | green |
| corpus/orm_rel | M3: Rel/RelMany (unloaded touch throws with the fix; loaded-empty orNone), `.with((u) => u.posts)` = parent SELECT + ONE child IN-query, `db.load(collection, path)` batch @BelongsTo, @ManyToMany-through with the owner's self-join pipe projection, identity of loaded targets, empty-collection no-op | green | green | green |
| corpus/orm_migrate | M4: chain-ordered apply (stateful fake bookkeeping table), second run applies 0, content-hash chain (`sha256(parentHash+name+upSQL)`) tamper error, two-children fork error with merge recipe, scaffolder (suggested DDL from generated members + head-parented source) | green | green | green |
| atlantis-mysql/tests/orm_loopback | M1 acceptance's second leg: the ORM over the REAL Track 05 driver (Pool + prepared statements + binary rows) against an in-language wire-protocol fake server on a loopback socket — find→FromRow, identity map short-circuit (2 PREPAREs/2 EXECUTEs total for two finds + one save), INSERT lastInsertId write-back | green | green | green |

## Findings / deviations from the design text (each logged in techdesign-06-orm.md §16)

- **Compiler bugs found:** #91 (nested-ns rules; FIXED), #92 (attribute class
  shadowing bare types; FIXED), #93 (punctuation-only template string literals
  corrupted; OPEN, worked around via `ctx()`/`ctxRow()` helpers), #94
  (field-closure dot-call silent no-op on LLVM; OPEN, worked around via
  local-copy-then-call throughout `db.lev`), #95 (pre-existing routing corpus
  LLVM segfault, unrelated to this track — verified against clean `src/`).
- **Real MySQL 8 acceptance** (M4/AG-4's live job): environment-gated, same
  posture as Track 05's landing (a MySQL server is present on this host but no
  test credentials exist; the Docker job in techdesign-05 §7.2 stands ready).
  The FakeDriver + loopback-fake legs cover the seam; the live job runs when
  credentials/Docker are available.
- `transact()` passes the SAME `Db` (pinned to one connection) to the body
  rather than a child object — a typed child (`tx.users`) can't be constructed
  generically; identity-map sharing and nested-guard semantics are exactly
  §4.5's.
- Timestamps discipline: `updatedAt = now` is applied to entities that are
  already otherwise dirty (then dirtyCols() recomputed) — the literal
  "before computing dirtyCols()" reading would mark EVERY tracked Timestamps
  entity dirty on every save and defeat the dirty-only lever.
- `Array<T>.contains` in a predicate needs `.whereIn(p, contents)` — LA-31
  R17 marks Array captures with `None` in `binds` ("the consumer keeps its own
  reference"); the ORM cannot reach the captured array any other way.
- Migration bookkeeping INSERT rides plain execution (no wrapping tx): MySQL
  DDL auto-commits around it and the bookkeeping is a single atomic statement.
- Boot validation is `await db.validate()` (dev flag = calling it at boot):
  repos register in the AppDb constructor AFTER `Db`'s own constructor runs,
  and constructors cannot await.

---

# Atlantis Track 07 — MCP/OpenAPI results (2026-07-19)

Executed with `build/leviathan`/`build/trident` rebuilt at head (commit `9709700` and
later — see the compiler-findings note below; the pre-rebuild binary gave false-positive
failures on unrelated code). All four milestones green on oracle/IR/LLVM:

```
./packages/atlantis/tests/runtests.sh
```

## M0 probes (design §7, run before any M2 feature work)

| # | probe file | result |
|---|---|---|
| P1 | `mcp_p1_generic_fromjson.lev` | GREEN (3 engines) — `A::FromJson(v)` inside a generic body (LA-18) works once `A` is pinned; **inferring `A` from a `(A) => R` argument (typed lambda OR bound method reference) does NOT work** without an explicit turbofish (LA-32) — a real finding, not metaprog-shaped, that reshapes M2 (see below) |
| P2 | `mcp_p2_empty_ctor.lev` | GREEN (3 engines) — rule-injected `new Empty() { }` + zero-default struct fields (int/string/bool all zero-value) |
| P3 | `mcp_p3_content_overloads.lev` | GREEN (3 engines) — `content(string)` beats `content<R>(R)`; duck-typed `.toJson()` generic fallback; `content(await promise)` unifies |
| P4 | `mcp_p4_for_over_params.lev` | GREEN (3 engines) — `$for p in m.params : F($p.name, $p.type)` (BARE holes — quoted holes are literals, corrected from this doc's own quoted sketches, same finding as ORM's P2) |
| P5 | `mcp_p5_splice_spellings.lev` | GREEN (3 engines) — `$m.name`/`$C.name`/`$p.type` bare-hole splicing; nested-namespace struct canonical spelling pinned as `Outer::Inner` (fully qualified, `::`-joined) |
| P6 | `mcp_p6_two_rules_stack.lev` | **FAILED, as real** — a rule cannot match an attribute declared in a DIFFERENT namespace than the rule itself (confirmed independent of nesting depth/direction via a minimal sibling-namespace repro). Filed **known_bugs_2.md #96** [P1]. Workaround applied: `schema.lev`'s rule stays in flat `Atlantis`, matching `@Serializable`'s real (un-migrated) home |
| P7 | `mcp_p7_multi_tool_accumulate.lev` | GREEN (3 engines) — three `@ItemProbe`-attributed methods, ctor-bottom injections accumulate in declaration order into one array field |

Two EXTRA probes run beyond the design's own table, both load-bearing for M2's actual
shape (not written up as separate `#N` items, but see the doc's §11 log): explicit
turbofish into a rule-generated splice position (`callWithParsed::<$p.type, string>(...)`)
fails to PARSE; `$p.type::FromJson(v)` (call-target position) parses but fails at RUNTIME
("cannot resolve call target"). Together with P1, these three confirm LA-16
(identifier/type-position splicing) is not landed — not a new bug, an already-filed ask.

## Corpus (M1–M4)

| case | what it proves | oracle | IR | LLVM |
|---|---|---|---|---|
| corpus/mcp_core | M1: JSON-RPC core over hand-registered (`addRaw`) tools — initialize/ping/tools-list/tools-call round trip, unknown-tool/unknown-method errors, the full §2.1 HTTP table (POST valid, notification→202, unparseable→-32700, batch→-32600, GET/DELETE→405, bad/good `MCP-Protocol-Version`→400/200), boot-time duplicate-name validation, `addRaw` duplicate rejection. Found+worked around known_bugs #94 (function-typed-FIELD dot-call silent no-op on LLVM) at `t.run(args)` | green | green | green |
| corpus/mcp_tool | M2: the real (fallback-ladder-adapted) round trip — `@Tool` generates `ToolMeta` metadata with zero hand code; `registerRun` hand-wires the typed closure naming `AddArgs` statically; `tools/list`/`tools/call` wire format matches this doc's §3.6 example exactly; `guard()` maps a `ValidationException` (missing field, wrong type) to `isError:true` text; boot warning for an `@Tool` method declared but never `registerRun`'d | green | green | green |
| corpus/openapi | M3: `SchemaRegistry` miss (boot error naming the exact fix line) + hit; `OpenApiDoc.build` over a hand-built `Array<RouteRec>` fixture — `openapi: "3.1.0"`, every route appears exactly once, `@Summary` joins into an op whose `RouteRec.summary` was `""`, `authMode` 1(+scheme)/2 → `security` entry/`[]`, every op's `default` response `$ref`s `components/schemas/Problem` (and it resolves), registered nested schemas present; **design risk #1 schema-drift check**: the same `CreateUser` DTO's schema through an MCP tool's `inputSchema` and an OpenAPI route's `requestBody` schema render byte-identical (both are literally the same `schemaJson()` call); `/llms.txt` output; `openApiCommand` factory construction (not executed — writes a real file) | green | green | green |
| corpus/client | M4: `ApiClient`/`ApiException` against a REAL in-process server (one event loop, Track 01 loopback's own pattern) — 2xx `getJson`/`postJson`, a `problem+json` 404 mapped to a typed `ApiException` (status + parsed detail), and the demo `UsersClient` (design §6.1's exact convention) round-tripping a typed `UserDto` through `await`. Timeout→exception not exercised (`HttpClient` has no timeout knob yet, reference.md §6.6.6) | green | green | green |

## Findings / deviations from the design text (each logged in detail in techdesign-07-mcp-openapi.md §11)

- **known_bugs_2.md #96 [P1] filed**: a rule cannot match an attribute declared in a
  different namespace than the rule itself (P6 above) — affects any future subsystem
  whose rule needs to match another subsystem's attribute, exactly the shape C1's own
  multi-consumer tiebreak anticipates as normal. Workaround: co-locate.
- **M2's `mcpTool` rule generates metadata only**, not a working tool — LA-16 (type-
  position splicing) gates the design's full "attribute only, zero hand code" promise for
  the typed dispatch closure specifically; `ToolSource.registerRun` + a boot-time
  unwired-tool warning is the shipped fallback (design's own §3.5 rung 3).
- **Compiler-build hygiene**: `build/leviathan` was stale by ~20 minutes relative to a
  same-day commit fixing bugs #91/#92; every symptom that looked like a #91 regression
  (including ORM's own regression-floor probe) disappeared after a plain rebuild. No
  compiler bug filed for that — it never existed on the current source.
- `schema.lev`'s rule placement (flat `Atlantis`, not `Atlantis::OpenApi`) and `@Summary`'s
  free-function shape (not a `Controller` base addition) are both cross-track-footprint-
  minimizing choices, not compiler-forced — see §11 for the full reasoning.
- `RouteRec.bodySchema`/`.summary` land as the LAST two constructor params (additive,
  positional — no named args in the language yet); every existing call site (Track 02's
  `AddRoute`, Track 02's own `routing.lev` corpus) updated to pass `None, ""`.

---

# Atlantis Track 08 — Auth & Security results (2026-07-19)

Core subsystem implemented in `packages/atlantis/src/auth/*.lev`
(principal/crypto/cookie/pbkdf2/strategy/session/bearer/guard/csrf/cors/headers/
ratelimit/audit) + additive `App.useAuthentication()/useAuthorization()` in
`kernel/facade.lev`. Full itemized deviation list lives in
`designs/complete/techdesign-08-auth-security.md` §15 — this is the test-result summary.

```
./packages/atlantis/tests/runtests.sh
```

| case | result |
|---|---|
| corpus/auth | **oracle: green. IR: green, byte-identical to oracle (`auth.expected`). LLVM: FAILS** — see below, not a Track 08 regression |

Full run across every corpus dir (2026-07-19, freshly rebuilt `leviathan`/`trident`):
every OTHER case (app, client, config, di, kernel, loopback, mcp_core, mcp_tool, openapi,
orm, orm_migrate, orm_query, orm_rel, serialization, static) still passes oracle+IR+LLVM
unchanged — Track 08's additions (a new `src/auth/*.lev` glob entry, two additive methods
on `App`, and removing the Track-01 `Principal` seed it explicitly said to replace) do not
regress anything else. Two OTHER pre-existing, unrelated failures were observed in the
same run and are NOT Track 08's: `routing` (LLVM only) and `serialization_refusals`
(negative-case text mismatch) — confirmed via `git status` that no file under
`src/json/` or `src/routing/` was touched this session.

**LLVM finding — `known_bugs_1.md` #97 filed, likely the same family as pre-existing
#95.** Bisected a genuine, minimal, independently-reproducible LLVM defect: a function
taking a class-typed parameter (e.g. `Http::Context`) that ALSO returns from inside a
`for` loop over `Array<Struct>` corrupts a LATER, unrelated heap-touching call. Fixed at
the one site this track hit (`cookie.lev`'s `cookieValue`, converted to an accumulator
pattern). Fixing it did NOT make the corpus LLVM-clean, though — the crash just moves to
a different `Router`/`Context` construction point depending on unrelated prior heap
activity (confirmed by reordering test sections: the crash site changes, never whether it
crashes). Directly re-ran the untouched, pre-existing `packages/atlantis/tests/
corpus/routing` corpus (Track 02, zero Track 08 code involved) and confirmed it ALSO
segfaults on LLVM within its first two lines of output — matching `known_bugs_1.md` #95's
own description ("atlantis routing corpus segfaults on LLVM, pre-existing at 2026-07-19
master") almost exactly. Conclusion: this is a systemic, already-tracked, out-of-scope
defect broad enough that essentially any non-trivial Atlantis program constructing
`Router`/`Context` objects is currently affected on LLVM — not something Track 08
introduced or can fix from within `.lev` source. Oracle+IR are the correctness reference
until the compiler-side fix lands.

**Other findings / deviations (full detail in the design's §15 log):**
- R-4 `sysRandomBytes` → landed as `sysRandom` before this track started (crypto ticket,
  already resolved, `Auth::randomBytes` is a direct pass-through).
- base64Url padding shim (P2) never needed — `encoding::base64Url{En,De}code` already
  emit/accept unpadded.
- `@Auth`/`@NoAuth` attributes still don't exist (blocked on LA-16/LA-22, an open/
  un-accepted request) — guard enforcement derives its GuardSpec from Track 02's landed
  `RouteRec.authMode`/`authRole` + `Router.authModeOf`/`authRoleOf` instead, zero schema
  changes; `.auth("policy:name")` already threads the `policy:` prefix through the
  existing `authRole` string.
- PBKDF2 corpus proves the algorithm/format/upgrade-path self-consistently at a tiny
  iteration count (4-8), not against RFC 7914 §11 vectors — `digest::hmacSha256` is an
  IN-LANGUAGE implementation, so real production iteration counts cost real wall-clock on
  the tree-walk oracle (~130ms/HMAC-call measured) and would blow any test timeout. No
  PBKDF2 calibration numbers exist yet for the 210k default.
