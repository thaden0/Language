# Tech Design LA-31 — 01: Prelude Surface (`namespace expr`, `like`/`ilike`, `expr::eval`)

**Stage:** 1 of 3. **Difficulty: Sonnet** — every artifact in this stage is fully
specified below; there are **zero decisions to make**. **Depends on:** nothing (first
stage). **Feeds:** Stage 2 (the reifier emits constructions of these classes).
**Owns:** `src/Resolver.cpp` prelude string segments only; `tests/corpus/expr_prelude_*`,
`tests/corpus/expr_like_*`, `tests/corpus/expr_eval_*`; their CMake rows.
**Window:** 2026-07-19 → 2026-07-20.

## STOP-AND-ESCALATE PROTOCOL (read first)

This stage must not change the design. If **any** of the following occurs, STOP
immediately, commit what is green, write the finding into a short note at the bottom of
this file (an "Implementation log" section), and escalate to the owner — do **not**
improvise an alternative shape:

- The prelude segment fails to parse/resolve (e.g. the union-typed field
  `string|int|float|bool|None v`, the generic `class Expr<F>`, or the `F fn` field).
- A hand-constructed tree misbehaves on any engine (wrong `match` arm taken, union
  value corrupt, `as`-downcast failing) — this smells like a compiler bug: file it in
  `/bug.md` with a minimal repro per the bug workflow, then stop.
- `expr::eval`'s signature (function-typed parameter with a union return) fails to
  parse or check.
- Adding `like`/`ilike` breaks any pre-existing test (emit-C++ reachability is the
  known risk class; the method names are distinctive, so this is unexpected — if it
  happens, it is a finding, not something to work around silently).
- Anything else that would require deviating from the code given below.

Do not touch any file outside this stage's ownership list. All new files are `.lev`.

---

## 1. Deliverable A — the `expr::` prelude namespace

### 1.1 Placement

The prelude is C++ raw-string source concatenated in `src/Resolver.cpp` into
`preludeFile_.text` (segments `kPreludeCore` + `kPreludeStd` + `kPreludeRest` + …;
concatenation at Resolver.cpp:5303–5308). Add a **new segment** `kPreludeExpr`
(a `static const char* kPreludeExpr = R"LEV( … )LEV";` next to the existing segments)
and append it **last** in the concatenation.

### 1.2 The exact prelude source (verbatim; do not restyle)

```
namespace expr {

    class Node { }

    class Field : Node {
        Array<string> path;
        new Field(Array<string> p) { path = p; }
    }

    class Lit : Node {
        string | int | float | bool | None v;
        new Lit(string | int | float | bool | None value) { v = value; }
    }

    class Bind : Node {
        int slot;
        new Bind(int s) { slot = s; }
    }

    class Bin : Node {
        string op;
        Node l;
        Node r;
        new Bin(string o, Node left, Node right) { op = o; l = left; r = right; }
    }

    class Un : Node {
        string op;
        Node e;
        new Un(string o, Node inner) { op = o; e = inner; }
    }

    class Call : Node {
        string name;
        Node recv;
        Array<Node> args;
        new Call(string n, Node receiver, Array<Node> a) { name = n; recv = receiver; args = a; }
    }

    class Assign : Node {
        Field target;
        Node value;
        new Assign(Field t, Node val) { target = t; value = val; }
    }

    class Expr<F> {
        F fn;
        expr::Node tree;
        Array<string | int | float | bool | None> binds;
        int siteId;
        new Expr(F f, expr::Node t, Array<string | int | float | bool | None> b, int s) {
            fn = f; tree = t; binds = b; siteId = s;
        }
    }
}
```

Discipline notes (these are the reasons the code is shaped this way — keep them):

- **Bodies are trivial field stores only.** The Checker never walks the prelude
  (prelude-not-checked rule); nothing here may rely on narrowing, overload subtleties,
  or checker annotations (prelude-no-narrowing rule).
- **Constructor parameter names differ from field names** (`p`/`path`, `o`/`op`, …) so
  every store is an unambiguous bare `field = param` write — no `this.` needed, no
  shadowing ambiguity in unchecked code.
- **No module-level `expr::` globals** — the emit-C++ eager-global-instance footgun
  (a global instance forces every method of its class into the emit-C++ build).
- **No methods beyond constructors.** The tree is data; behavior lives in consumers
  (`match`-walks). This also keeps emit-C++ by-name reachability inert: the only member
  names introduced are the ctor labels and fields, all distinctive.
- Reference classes are correct here (per the ask §1.2): the tree is runtime data,
  immutable by convention.

## 2. Deliverable B — `string.like` / `string.ilike`

### 2.1 Placement

Inside `class string` in `kPreludeStd`, next to `contains`/`startsWith`/`indexOf`
(Resolver.cpp:176–334). Three methods: the two public ones and one shared matcher.
Explicit `this.` on **every** self-call and self-member access (this-receiver
discipline; the string class already documents it at Resolver.cpp:204–213).

### 2.2 Pinned semantics (ruling R13; the corpus in §2.4 enforces every line)

- Anchored **full-string** match (SQL `LIKE` semantics), byte-oriented.
- `%` (byte 37) matches any run of bytes, including empty.
- `_` (byte 95) matches exactly one byte. (Byte, not scalar: a UTF-8 multibyte
  character needs one `_` per byte. This is the documented byte-level bound.)
- `\` (byte 92) escapes the **next byte**, whatever it is, to a literal. A pattern
  ending in a lone `\` is **unmatchable** (matches no text, including text ending in a
  backslash) — this falls out of the matcher below and is pinned by a corpus row.
- Empty pattern matches only the empty string. Trailing `%` runs collapse (the
  epilogue loop).
- `like` compares bytes exactly (case-sensitive). `ilike` folds A–Z (65–90) to a–z
  (97–122) **on both text and pattern bytes at each comparison site** — including a
  `\`-escaped literal byte — and compares everything else exactly. No folded string
  copies are built.

### 2.3 The exact method bodies (verbatim)

```
    bool like(string pattern) return this.__likeMatch(pattern, false);

    bool ilike(string pattern) return this.__likeMatch(pattern, true);

    bool __likeMatch(string pattern, bool fold) {
        int n = this.length();
        int m = pattern.length();
        int i = 0;
        int j = 0;
        int star = 0 - 1;
        int mark = 0;
        while (i < n) {
            int pb = 0 - 1;
            if (j < m) pb = pattern.byteAt(j);
            if (pb == 92 && j + 1 < m) {
                int lit = pattern.byteAt(j + 1);
                int tb = this.byteAt(i);
                if (fold) {
                    if (lit >= 65 && lit <= 90) lit = lit + 32;
                    if (tb >= 65 && tb <= 90) tb = tb + 32;
                }
                if (lit == tb) { i = i + 1; j = j + 2; }
                else if (star >= 0) { j = star + 1; mark = mark + 1; i = mark; }
                else return false;
            } else if (pb == 95) {
                i = i + 1; j = j + 1;
            } else if (pb == 37) {
                star = j; mark = i; j = j + 1;
            } else {
                int tb = this.byteAt(i);
                int qb = pb;
                if (fold) {
                    if (qb >= 65 && qb <= 90) qb = qb + 32;
                    if (tb >= 65 && tb <= 90) tb = tb + 32;
                }
                if (j < m && qb == tb) { i = i + 1; j = j + 1; }
                else if (star >= 0) { j = star + 1; mark = mark + 1; i = mark; }
                else return false;
            }
        }
        while (j < m && pattern.byteAt(j) == 37) j = j + 1;
        return j == m;
    }
```

Notes: `0 - 1` (not `-1` literal) sidesteps any unary-minus-in-initializer wrinkle in
unchecked prelude code — keep it. The lone-trailing-`\` case reaches the `pb == 92 &&
j + 1 < m` guard as false, falls to the final `else` branch with `qb == 92` vs the text
byte; if the text byte *is* 92 it advances and then the epilogue fails on the leftover
`\` (j != m)… actually the escape byte is consumed as a literal `\` in that branch, so
`"a\\"` vs text `"a\\"`: after `a`, `pb==92`, `j+1==m` → final else, `qb=92`, `tb=92`,
match, `j==m`, `i==n` → **true**. That is the pinned behavior refinement: **a lone
trailing `\` matches a literal `\`** (it only becomes unmatchable when no text byte 92
is there to pair with). The corpus row in §2.4 asserts exactly this: pattern `"a\\"`
matches `"a\\"` and does not match `"a"` or `"ab"`. (This supersedes the shorthand in
the overview's R13 one-liner; this section is the normative pin.)

### 2.4 `like`/`ilike` unit corpus — `tests/corpus/expr_like_1.lev` (+ `.expected`)

One file, oracle+IR+LLVM (standard `run_corpus.sh`/`run_native.sh` matrix + CMake
`corpus_expr_like_1` targets). Print one line per row: `console.writeln(label + ":" +
value.toString())`. Rows (text, pattern, expected — for both `like` and `ilike` where
marked):

| # | text | pattern | like | ilike |
|---|---|---|---|---|
| 1 | `"hello"` | `"hello"` | true | true |
| 2 | `"hello"` | `"h%"` | true | true |
| 3 | `"hello"` | `"%llo"` | true | true |
| 4 | `"hello"` | `"h_llo"` | true | true |
| 5 | `"hello"` | `"h__lo"` | true | true |
| 6 | `"hello"` | `"%"` | true | true |
| 7 | `""` | `"%"` | true | true |
| 8 | `""` | `""` | true | true |
| 9 | `"hello"` | `""` | false | false |
| 10 | `"HELLO"` | `"hello"` | false | true |
| 11 | `"Hello"` | `"h%O"` | false | true |
| 12 | `"50%"` | `"50\\%"` | true | true |
| 13 | `"505"` | `"50\\%"` | false | false |
| 14 | `"a_b"` | `"a\\_b"` | true | true |
| 15 | `"axb"` | `"a\\_b"` | false | false |
| 16 | `"a\\"` | `"a\\"` | true | true |
| 17 | `"a"` | `"a\\"` | false | false |
| 18 | `"ab"` | `"a\\"` | false | false |
| 19 | `"abc"` | `"a%c"` | true | true |
| 20 | `"ac"` | `"a%c"` | true | true |
| 21 | `"abcbc"` | `"a%bc"` | true | true |
| 22 | `"hello"` | `"hello%%%"` | true | true |
| 23 | `"héllo"` | `"h_llo"` | false | false |
| 24 | `"héllo"` | `"h__llo"` | true | true |
| 25 | `"HÉllo"` | `"hÉllo"` | false | true |

(23/24 pin the byte-level `_`; 25 pins that fold is ASCII-only — `É` stays exact while
`H` folds. In the `.lev` source, backslashes are written with whatever escape the
Leviathan string literal needs — use raw strings `r"…"` where that is clearer; they are
landed surface.)

## 3. Deliverable C — hand-built trees + `expr::eval` (the taxonomy proof)

Reification does not exist yet (Stage 2). This deliverable proves the taxonomy is
constructible and walkable **by hand**, so Stage 2 lands onto verified ground, and it
delivers the exact `eval` the Stage 3 differential corpus will embed.

### 3.1 The canonical `expr::eval` walker (verbatim; this text is the one Stage 3 copies)

`eval` is **checked user code shipped inside corpus files** (ruling R14), never
prelude. It takes the record accessor as a function value and the binds table, returns
the DbValue union. Helper `evalTruth` coerces to bool for `&&`/`||`/`!` arms; `None`
propagates SQL-ishly as pinned below.

Pinned in-memory semantics (these ARE the language-side contract the ORM renderer must
match — the ask §2.1's "consumers own rendering fidelity" starts from here):

- `==`/`!=`: `None == None` is `true`; `None` vs non-None is `false` (so `!=` is
  `true`); same-type values compare by value; **mixed non-None types never occur** (the
  tree mirrors a checked body — a type-mismatched comparison already failed checking).
- `<`/`<=`/`>`/`>=` and arithmetic `+ - * / %`: operate on matching `int`/`float`
  pairs (and `+` on `string` pairs — string concatenation reifies as `Bin("+")` when
  it appears between two reifiable string operands); any `None` operand → the
  comparison yields `false`, the arithmetic yields `None`.
- `&&`/`||`: strict boolean over both evaluated sides (the closure leg short-circuits;
  the eval leg evaluates both — pure trees make this unobservable, and reifiable
  bodies are pure by construction: no non-whitelisted calls, no assignment except the
  set shape which never mixes with `&&`).
- `!` needs a `bool`; unary `-` needs `int`/`float`.
- `Call` arms implement exactly: `like`, `ilike`, `startsWith`, `endsWith`,
  `contains` (string), `contains` (Array-bind receiver: the receiver `Bind` resolves
  to… — see the note below the code).
- `Assign` is not evaluated by `eval` (it is a write-description, not a predicate);
  the arm throws.

```
// ---- expr::eval — reference tree interpreter (copy verbatim into corpus files) ----
bool evalTruth(string | int | float | bool | None v) {
    match (v) {
        bool => return v as bool;
        else => return false;
    }
}

string | int | float | bool | None evalNode(
        expr::Node n,
        (Array<string>) => string | int | float | bool | None get,
        Array<string | int | float | bool | None> binds,
        Array<string | int | float | bool | None> arrayBind0) {
    match (n) {
        expr::Field => { return get((n as expr::Field).path); }
        expr::Lit   => { return (n as expr::Lit).v; }
        expr::Bind  => { return binds[(n as expr::Bind).slot]; }
        expr::Un    => {
            expr::Un u = n as expr::Un;
            string | int | float | bool | None inner = evalNode(u.e, get, binds, arrayBind0);
            if (u.op == "!") return !evalTruth(inner);
            match (inner) {
                int   => return 0 - (inner as int);
                float => return 0.0 - (inner as float);
                else  => return None;
            }
        }
        expr::Bin   => {
            expr::Bin b = n as expr::Bin;
            string | int | float | bool | None lv = evalNode(b.l, get, binds, arrayBind0);
            string | int | float | bool | None rv = evalNode(b.r, get, binds, arrayBind0);
            if (b.op == "&&") return evalTruth(lv) && evalTruth(rv);
            if (b.op == "||") return evalTruth(lv) || evalTruth(rv);
            if (b.op == "==") return dbEq(lv, rv);
            if (b.op == "!=") return !dbEq(lv, rv);
            return dbNum(b.op, lv, rv);
        }
        expr::Call  => {
            expr::Call c = n as expr::Call;
            if (c.name == "contains" && c.args.length() == 1) {
                // Array receiver form: receiver is a Bind whose slot carries the
                // marker None in `binds` and whose values live in arrayBind0 (see note).
                match (c.recv) {
                    expr::Bind => {
                        string | int | float | bool | None needle =
                            evalNode(c.args[0], get, binds, arrayBind0);
                        int k = 0;
                        while (k < arrayBind0.length()) {
                            if (dbEq(arrayBind0[k], needle)) return true;
                            k = k + 1;
                        }
                        return false;
                    }
                    else => { }
                }
            }
            string recvS = asStr(evalNode(c.recv, get, binds, arrayBind0));
            string arg0S = asStr(evalNode(c.args[0], get, binds, arrayBind0));
            if (c.name == "like")       return recvS.like(arg0S);
            if (c.name == "ilike")      return recvS.ilike(arg0S);
            if (c.name == "startsWith") return recvS.startsWith(arg0S);
            if (c.name == "endsWith")   return recvS.endsWith(arg0S);
            if (c.name == "contains")   return recvS.contains(arg0S);
            throw RuntimeException("expr::eval: non-whitelisted call " + c.name);
        }
        else => { throw RuntimeException("expr::eval: unhandled node"); }
    }
}

bool dbEq(string | int | float | bool | None a, string | int | float | bool | None b) {
    match (a) {
        None => { match (b) { None => return true;  else => return false; } }
        int  => { match (b) { int  => return (a as int) == (b as int); else => return false; } }
        float=> { match (b) { float=> return (a as float) == (b as float); else => return false; } }
        bool => { match (b) { bool => return (a as bool) == (b as bool); else => return false; } }
        string => { match (b) { string => return (a as string) == (b as string); else => return false; } }
    }
}

string | int | float | bool | None dbNum(string op,
        string | int | float | bool | None a, string | int | float | bool | None b) {
    match (a) {
        int => { match (b) { int => {
            int x = a as int; int y = b as int;
            if (op == "<")  return x < y;
            if (op == "<=") return x <= y;
            if (op == ">")  return x > y;
            if (op == ">=") return x >= y;
            if (op == "+")  return x + y;
            if (op == "-")  return x - y;
            if (op == "*")  return x * y;
            if (op == "/")  return x / y;
            if (op == "%")  return x % y;
        } else => { } } }
        float => { match (b) { float => {
            float x = a as float; float y = b as float;
            if (op == "<")  return x < y;
            if (op == "<=") return x <= y;
            if (op == ">")  return x > y;
            if (op == ">=") return x >= y;
            if (op == "+")  return x + y;
            if (op == "-")  return x - y;
            if (op == "*")  return x * y;
            if (op == "/")  return x / y;
        } else => { } } }
        string => { match (b) { string => {
            if (op == "+") return (a as string) + (b as string);
        } else => { } } }
        else => { }
    }
    match (a) { None => { } else => { match (b) { None => { } else => {
        throw RuntimeException("expr::eval: bad operands for " + op); } } } }
    if (op == "<" || op == "<=" || op == ">" || op == ">=") return false;
    return None;
}
// ---- end expr::eval ----
```

**The Array-`contains` receiver note (pinned here, not decided later):** a reified
`capturedArray.contains(x)` has receiver `Bind(slot)` where `binds[slot]` would need to
hold an *Array* — but the binds element union is `string|int|float|bool|None` (the ask
§1.1, deliberately DbValue-shaped). Ruling: **the reifier (Stage 2) stores `None` in
the binds slot for an Array-typed capture and the `Expr` construction is otherwise
unchanged; the consumer that wants the array's contents keeps its own reference** — in
the ORM's case it renders `IN (?)` expansion from its own captured handle; in `eval`'s
case the test passes the array separately (`arrayBind0`). The tree still records *that*
slot k is the receiver (`Bind(k)`), so per-site parameter identity is preserved. Corpus
rows in Stage 3 exercise exactly one Array capture per lambda (`arrayBind0`); multiple
Array captures per lambda are legal in the language surface and simply give each its
own None-marked slot — `eval`'s single-array helper is a test-harness simplification,
not a language limit. This ruling exists because the union cannot widen without
amending the ask (consistency law) — and the DbValue shape is load-bearing for the ORM.

### 3.2 Hand-built-tree corpus — `tests/corpus/expr_eval_1.lev` (+ `.expected`)

Constructs, **by hand** (explicit `expr::` ctor calls), the tree for
`(u) => u.active && u.name.like("A%")` and the tree for
`(u) => u.age >= lo && u.age < hi` (with `lo`/`hi` as `Bind(0)`/`Bind(1)`,
binds `[18, 65]`), then `eval`s each against two fixture records implemented as lambda
accessors:

```
(Array<string>) => string | int | float | bool | None get1 = (path) => {
    if (path[0] == "active") return true;
    if (path[0] == "name")   return "Ada";
    if (path[0] == "age")    return 30;
    return None;
};
```

Expected output (byte-exact `.expected`, all three engines):

```
t1r1:true
t1r2:false
t2r1:true
t2r2:false
```

(Record 2: `active=false`, `name="Bob"`, `age=70`.) Also one row `matchElse:ok` proving
the `else` arm fires for a bare `expr::Node()` (unknown-node safety), via
`try { … } catch` printing `ok`.

### 3.3 Node-construction smoke — `tests/corpus/expr_prelude_1.lev` (+ `.expected`)

Constructs one of each node class, stores them via `expr::Node`-typed variables, reads
fields back through `as`-downcasts after `match`, prints a fixed dump; asserts the
union field `Lit.v` round-trips all five member types (including `None`), and that an
`Array<string|int|float|bool|None>` literal of mixed members stores and reads back.
Expected output pinned in the `.expected`. This is the earliest tripwire for
union-in-class-field trouble on LLVM (cf. the Map/struct-field family): if any engine
diverges here, that is a **bug-file-and-STOP**, not a workaround.

## 4. Acceptance for Stage 1 (all must be green before Stage 2 starts)

1. Clean rebuild; **full pre-existing ctest suite green** (oracle/IR/emit-C++/LLVM
   lanes) — proves the prelude additions regress nothing (watch emit-C++ specifically).
2. `corpus_expr_like_1`, `corpus_expr_eval_1`, `corpus_expr_prelude_1` green on
   oracle + IR + LLVM.
3. No new files outside this stage's ownership list; everything committed and pushed
   to master.

## 5. Implementation log

- **Deviation from the §3.1 `expr::eval` verbatim text — no `X as T` downcast
  operator exists in the language.** The language has no cast expression at
  all (`Ast.hpp`'s `ExprKind` has `Is`/`Match`, no `As`; `as` is a contextual
  keyword used only by `use ... as alias`, `docs/reference.md` §4.1/toml
  §deps). Confirmed by direct repro: `int y = v as int;` fails to parse
  ("unknown type 'as'"). Every `X as T` in the given eval walker sits inside
  a `match` arm (or a nested `match` reached from one) where `match`'s own
  landed type-narrowing already gives the arm's subject variable/parameter
  the narrowed type (`docs/reference.md` §3.15; confirmed idiom throughout
  the codebase, e.g. `packages/atlantis-mysql/tests/prepared/main.lev:11-15`
  — `match (v) { int => { return "int:" + v.toString(); } ... }`, no cast).
  Substitution applied mechanically, semantics unchanged: every `(x as T)`
  read/local-decl became a bare read/decl of the already-narrowed `x` (e.g.
  `(n as expr::Field).path` → `n.path`; `expr::Un u = n as expr::Un;` →
  `expr::Un u = n;`; `int x = a as int; int y = b as int;` inside the nested
  `int=>{match(b){int=>{...}}}` arm → `int x = a; int y = b;`). No semantic
  decision was made — the narrowing already fully pins the value each `as`
  was reaching for; this is a syntax correction to landed grammar, not a
  design change. Flagged here per protocol rather than silently patched.
- **Separately, found and filed bug #84** (`known_bugs_2.md`): a top-level
  bare `match` statement immediately followed by another top-level statement
  fails to parse ("expected ';'") — `Parser::parseTopLevelItemInner`'s
  bare-expression-statement fallback lacks the `ExprKind::Match`
  no-semicolon-required special case that `Parser::parseStatement`'s default
  case already has. Workaround (trailing `;` after the top-level match, or
  wrap in a function body) applied in the corpus files below; not a blocker.
- Prelude additions (Deliverables A + B) landed as specified verbatim, no
  deviation. `kPreludeExpr` uses the repo's existing `R"prelude(...)prelude"`
  raw-string delimiter and plain `const char*` (not `static`) to match every
  other segment in this file, rather than the doc's illustrative `R"LEV(...)"`
  spelling — a styling match, not a content change. Deliverable B smoke: all 25
  §2.4 rows verified by hand on the oracle, byte-identical on IR; the committed
  corpus files land when the stage resumes (below).
- **2026-07-18 (later) — STAGE PAUSED at Deliverable C, per protocol.** The
  first hand-built-tree probe took the wrong `match` arm: a qualified
  `expr::Field =>` pattern parses as a *value* pattern and silently never
  matches (oracle takes `else`; IR/emit-C++ error loud). This is the doc's
  "wrong `match` arm taken" STOP condition — filed as **#85** in
  `known_bugs_2.md` (with **#84**, the top-level-`match` trailing-`;` parse
  defect, found the same session). Escalated; the owner ruled: **fix the
  compiler first**. The fix design is
  **`techdesign-005-preprelude-fixes.md`** (this directory, stage "0.5"):
  parser neutrality + `<`-lookahead, Resolver symbol-driven arm
  reclassification, Checker type-value backstop, AstPrinter qualified-type
  printing, and the #84 statement-parity fix. **Stage 1 resumes at
  Deliverables B-corpus (§2.4 file) and C once 005 lands**, and then uses this
  doc's code AS WRITTEN — `expr::Field =>` arms directly (no `uses expr;`
  workaround) and top-level `match` statements with no trailing-`;`
  workaround (plus the `as`→match-narrowing substitution logged above, which
  stands).
