# Tech Design LA-31 — 03: Verification & Landing (corpus, differential rows, docs, closure)

**Stage:** 3 of 3. **Difficulty: Sonnet** — every corpus row, dump format, diagnostic
substring, and doc section is specified below; there are **zero decisions to make**.
**Depends on:** Stages 1–2 landed. **Owns:** `tests/corpus/expr_reify_*`,
`tests/negative/expr_reify_*`, `tests/run_expr_reify_error.sh`, CMake rows,
`docs/reference.md` §expr, the M2 smoke, and the
completion file moves. **This stage touches NOTHING in `src/`** — a compiler defect
found here is filed in `known_bugs_1.md`/`known_bugs_2.md` with a minimal repro, never patched.
**Window:** 2026-07-23 → 2026-07-25.

## STOP-AND-ESCALATE PROTOCOL

STOP, commit green work, log the finding at the bottom of this file, and escalate if:

- Any differential row disagrees between the closure leg and the `eval` leg — that is
  either a reifier defect (Stage 2's owner takes it back) or an engine bug (file it);
  never "fix" a row's expectation to make it pass.
- Any positive form from the table in §2 fails to reify, or any negative form produces
  the wrong (or no) diagnostic — same routing.
- The twin byte-equivalence (§4) differs by anything other than nothing — do not
  hand-wave whitespace; a formatting mismatch is a finding about the printer.
- A corpus row cannot be written because the specified surface doesn't exist — that is
  a design/reality divergence and must go back to the owner, not be re-designed here.

## 1. Canonical dump format (normative — all corpus `.expected` files use exactly this)

Each reify corpus file defines `dump(expr::Node) -> string` (checked user code, copied
verbatim across files like `eval`) producing, recursively, with **no whitespace**:

- `Field(a.b)` — path joined with `.`
- `Lit(int:5)` · `Lit(str:A%)` · `Lit(bool:true)` · `Lit(float:1.5)` · `Lit(none)`
- `Bind(0)`
- `Bin(&&,<l>,<r>)` — the op spelled verbatim (`==`, `!=`, `<`, `<=`, `>`, `>=`,
  `&&`, `||`, `+`, `-`, `*`, `/`, `%`)
- `Un(!,<e>)` / `Un(-,<e>)`
- `Call(like,<recv>,[<a0>])` — args comma-joined in `[...]`
- `Assign(Field(name),<v>)`

And two trailer lines per site:
`binds=[int:18,str:x,none]` (slot order; same `type:value` coding; empty → `binds=[]`)
and `siteId=0`. Float values in dumps are restricted to halves (`1.5`, `2.0`) so all
engines print identically. A full test line is:

```
s0:Bin(&&,Field(active),Call(like,Field(name),[Lit(str:A%)]))
s0binds:binds=[]
s0site:siteId=0
```

## 2. Positive reify corpus (`tests/corpus/expr_reify_*.lev`, oracle+IR+LLVM)

One file per group; each site's dump + a `.fn` invocation on a fixture (both legs
always exercised). Forms (each is a row; ask §1.3 fully covered):

| # | lambda body (target `Expr<(User)=>bool>` unless noted) | expected tree dump |
|---|---|---|
| 1 | `u.active` | `Field(active)` |
| 2 | `u.addr.city == "Oslo"` (chained field) | `Bin(==,Field(addr.city),Lit(str:Oslo))` |
| 3 | `u.role == RelationType::Friend` (enum) | `Bin(==,Field(role),Lit(int:<carrier>))` |
| 4 | `u.age >= 18 && u.age < 65` | `Bin(&&,Bin(>=,Field(age),Lit(int:18)),Bin(<,Field(age),Lit(int:65)))` |
| 5 | `u.age >= lo` (captured local) | `Bin(>=,Field(age),Bind(0))` + `binds=[int:18]` |
| 6 | `u.name == this.defaultName` (this capture) | `Bin(==,Field(name),Bind(0))` |
| 7 | `u.limit < cfg.maxItems` (capturedObj.field — R3) | `Bin(<,Field(limit),Bind(0))` |
| 8 | `u.nick == None` (`T?` field) | `Bin(==,Field(nick),Lit(none))` |
| 9 | `None != u.nick` (R7 — no reordering) | `Bin(!=,Lit(none),Field(nick))` |
| 10 | `!u.active` | `Un(!,Field(active))` |
| 11 | `u.score > -5` → unary on literal | `Bin(>,Field(score),Un(-,Lit(int:5)))` |
| 12 | `u.name.like("A%")` | `Call(like,Field(name),[Lit(str:A%)])` |
| 13 | `u.name.ilike(pat)` (captured pattern) | `Call(ilike,Field(name),[Bind(0)])` |
| 14 | `u.name.startsWith("A")` / `endsWith` / `contains` | the three `Call(...)` shapes |
| 15 | `tags.contains(u.tag)` (captured Array — doc 01 §3.1 ruling) | `Call(contains,Bind(0),[Field(tag)])` + `binds=[none]` |
| 16 | `u.a + u.b * u.c - u.d / u.e % u.f` (int fields; precedence mirrored) | nested `Bin` exactly as checked |
| 17 | `u.first + " " + u.last == full` (string `+`) | `Bin(==,Bin(+,Bin(+,Field(first),Lit(str: )),Field(last)),Bind(0))` |
| 18 | target `Expr<(User)=>int>`: `u.visits = u.visits + 1` (set shape, R10) | `Assign(Field(visits),Bin(+,Field(visits),Lit(int:1)))` |
| 19 | `lo == lo2` where both captured; also `lo` twice → one slot (R5 dedupe): body `u.age >= lo && u.age2 >= lo` | `binds=[int:18]`, both sides `Bind(0)` |
| 20 | multi-param `Expr<(User,User)=>bool>`: `(a, b) => a.age < b.age` (R11) | `Bin(<,Field(age),Field(age))` — dumps identical; `.fn` row proves the params bind distinctly |
| 21 | positions file (R12): same lambda via call-arg, local bind, `return`, field initializer — four sites | four identical trees, `siteId=0..3` in source order (R4) |

Note row 20: `Field` carries no parameter marker (the ask's taxonomy has none — path
only). Two same-named fields of different params dump identically; the `.fn`
invocation row (`a=30,b=40 → true; a=40,b=30 → false`) is what proves leg-1 binds
correctly, and consumers that need per-param roots in v1 declare single-param lambdas.
This is the ask's own taxonomy, restated here so nobody "fixes" it mid-corpus.

**Hand-written-twin byte-equivalence** (ask acceptance #1): for rows 1, 4, 12, 15, 18,
a sibling `expr_reify_twin_*.lev` hand-authors the exact
`expr::Expr<F>(<lambda>, <tree>, [<binds>], <siteId>)` construction; assert
`--expand` output of the reified file's site == the twin file's site **byte-identical**
(extract the construction statement lines with the harness's grep; the twin is written
to match the printer's formatting).

## 3. Negative corpus (`tests/negative/expr_reify_*.lev` + `tests/run_expr_reify_error.sh`)

Harness: the `run_regex_comptime_error.sh` pattern — compile, assert non-zero exit,
assert the substring. One file per row (Stage 2's catalog §6 is the source of truth
for text):

| file suffix | body | asserted substring |
|---|---|---|
| `call` | `u.name.toUpper()` | `non-whitelisted call 'toUpper'` **and** `string.like/1` (the generated list line — proves R6 wiring) |
| `await` | `await p` | `cannot reify await` |
| `nested` | `u.tags.contains((t) => t == "x")`-shaped inner lambda | `nested lambda` |
| `block` | `(u) => { return u.active; }` | `block body` |
| `assign_pos` | bool-target body `(u.visits = 1) == 1` | `assignment outside a set-shaped lambda` |
| `capmut` | set-target `cnt = cnt + 1` (captured root) | `mutation of a capture` |
| `is` | `u.pet is Dog` | `'is' expression` |
| `match` | `match (u.role) …` as expression | `'match' expression` |
| `interp` | `` `${u.name}!` == "x" `` (whatever the interpolation syntax spells) | `string interpolation` |
| `index` | `u.tags[0] == "x"` | `indexing` |
| `ternary` | conditional expression form | `conditional expression` |
| `captype` | captured object used as an operand: `u.owner == boss` (class-typed) | `capture of a non-value type` |
| `value` | `var f = (u) => u.active; take(f)` where `take(expr::Expr<…>)` | `only a lambda literal can be reified` |
| `ambig` | both-form overload pair called with a literal | `ambiguous lambda argument` |

## 4. Differential corpus (`tests/corpus/expr_diff_*.lev` — closure leg vs `eval` leg)

Each file embeds the verbatim `eval` + `dump` blocks (doc 01 §3.1). For each
whitelisted op and each `Bin`/`Un` op: build the reified `Expr`, run `.fn(fixture)`
and `evalNode(e.tree, get, e.binds, arrayBind0)` over the **same fixtures**, print
`opName:fnResult:evalResult` — the `.expected` shows them equal; the run on all three
engines shows the engines equal. Fixtures include: both branches of every comparison,
`None` field values against `==`/`!=`/`<` (pinning the doc-01 None semantics), the
full `like`/`ilike` pin rows 10–18 from doc 01 §2.4 re-run through reified trees.

**The two R5 snapshot rows** (ask §2.2 + the clarifying row):

- `snap_value`: capture `int lo = 18`; build the `Expr`; set `lo = 99`; assert **both**
  legs still behave as `18` (fn: closure captured the value; eval: `binds[0]==18`).
- `snap_ref`: capture `Array<string> tags`; build `Expr` for `tags.contains(u.tag)`;
  **mutate the array's contents** after construction; assert **both** legs see the
  mutation equally (fn: live reference; eval: the harness's `arrayBind0` IS the same
  reference). This is the row that pins snapshot = reference-snapshot, not deep copy.

## 5. `--expand` visibility + round-trip (ask §2.4)

- Assert (grep) that `--expand` of `expr_reify_smoke_1.lev` contains
  `expr::Expr<(User)=>bool>(` and `expr::Bin(` — the construction is visible.
- Run the round-trip harness over the expr corpus files: `--expand` → compile+`--run`
  the output → byte-identical stdout. (Enum-using row 3 is exempt from the whole-file
  round-trip per pre-existing bug #69; its exemption is noted inline in the harness
  with the bug number.)

## 6. Track 06 M2 activation smoke (`tests/corpus/expr_orm_smoke_1.lev`)

A minimal in-file `Query<E>`-shaped consumer (no dependency on the Atlantis package —
this is the language-side proof): a class with
`where(expr::Expr<(E)=>bool>) -> Query<E>` and `set(expr::Expr<(E)=>int>)` methods
that store and `match`-walk the trees, driven with the ask's own example shapes.
Asserts: lambdas convert in fluent-call position, the walk renders a stable string per
site, `siteId` distinguishes two `.where` sites. Zero changes to any ORM M1 code are
needed by construction (nothing here touches `designs/atlantis` code); Track 06 M2
consumes the landed surface as-is.

## 7. Documentation

- **`docs/reference.md`** — new §expr (place per the maintainers' numbering next to
  `regex::`/`json::`): the `expr::` taxonomy (verbatim class list), the conversion
  rule (lambda literal in `Expr<F>` positions: param/bind/return/field-init), the
  reifiable subset table + the generated-diagnostic note, the E1/E2/E3 catalog, the
  R5 snapshot contract (both rows spelled out), the Array-capture `None`-slot rule,
  `siteId` determinism, `like`/`ilike` full semantics (doc 01 §2.2–2.3 pins including
  the trailing-`\` refinement), and the R1 note that `--expand` is now post-check with
  `--ast-after-rules` as the pre-check view.
- **`docs/reference.md` §expr** — document these behavioral facts alongside the rest
  of the section (there is no separate footgun registry — `docs/footguns.md` was
  retired 2026-07-19): (a) `--expand` now runs the Checker — ill-typed programs fail
  it; use `--ast-after-rules` to debug ill-typed expansions; (b) an Array (or any
  non-DbValue) capture's binds slot holds `None` — consumers keep their own
  reference; (c) `Field` paths carry no parameter marker — multi-param reified
  lambdas with same-named fields need consumer care.
- If any new *compiler defect* was discovered during Stages 1–3 (as opposed to the
  above intentional behavior), it gets a `known_bugs_1.md`/`known_bugs_2.md` entry
  (each stage's implementation log is the source).

## 8. Stage-3 acceptance = the whole-feature acceptance (doc 00 §6)

1. §2 positive corpus + twins green (oracle+IR+LLVM, byte-identical).
2. §3 negative corpus green (every substring hits).
3. §4 differential + snapshot rows green.
4. §5 visibility + round-trip green; full pre-existing ctest matrix green
   (emit-C++ lanes included).
5. §6 ORM smoke green.
6. §7 docs landed.

## 9. Completion protocol (execute, in order — then this design set is CLOSED)

1. Clean rebuild; run the full matrix one final time.
2. `git mv designs/expr-reification designs/complete/expr-reification`
3. `git mv designs/requests/request-expr-reification.md designs/requests/accepted/`
4. Commit with a message noting **LA-31 landed; Atlantis Track 06 M2 unblocked**.
5. Push to master from your own worktree (`git push origin <branch>:master`).
   Nothing untracked, nothing uncommitted.

## 10. Implementation log

(Append findings and completion note here.)
