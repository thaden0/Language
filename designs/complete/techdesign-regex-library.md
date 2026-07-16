# Track 10 — Regex Library (doc 2 of 2: the public surface)

**Status:** implemented (L1-L4). **Date:** 2026-07-11. **Depends on:** doc 1
(`designs/complete/techdesign-regex-engine.md` — the engine this surface fronts), named
arguments (landed), enums (landed; considered and rejected for options, §2.3).
**Source:** `designs/requests/request-regex.md` (LA-13); owner directive 2026-07-11: a
**full-featured, C#-shaped Regex library** (`Regex.Matches`, `Regex.Replace(str, src,
dest, RegexOptions)` + string-flag variants) showcasing the engine's performance.
**Owns (regions):** a NEW prelude segment `kPreludeRegexApi` in `src/Resolver.cpp`
(top-level public types + `namespace regex` convenience functions); surface corpus
`tests/corpus/regex/`; `docs/reference.md` regex chapter; `designs/requests/request-regex.md`
status line.
**Does NOT own:** engine internals (doc 1, `kPreludeRegexCore`) — one additive accessor
(`nameRecords`/`programNameData`) was added there, see §11; `class string` (no
edits — regex methods live on `Regex`, not on `string`, in v1).

---

## 0. Naming and casing ruling (asked directly by the owner)

The existing standard is unambiguous and this surface follows it:

- **Methods and functions are camelCase**, first letter lowercase: `matches`, not
  `Matches` (per `length`, `subStr`, `isEmpty`, `joinToString`, `indexOf`). C#'s
  PascalCase method names do not port; the *shape* of the API does.
- **Types are PascalCase**: `Regex`, `Match`, `Group`, `RegexOptions`, `RegexException`.
- **Namespaces are lowercase**: `regex` (per `std`, `env`, `json`).

**`Regex::matches(...)` — what it means here.** The language has no `static` members
yet (no `KwStatic` token; `request-generic-static-members.md` is open), so C#'s static
convenience methods land as **functions in the top-level `namespace regex`**:
`regex::isMatch(s, pattern)`, `regex::matches(s, pattern)`, `regex::replace(...)`. This
reads within two characters of the C# call and uses an existing general rule instead of
new machinery. If class-statics land later, `Regex::isMatch` can alias `regex::isMatch`
as a mechanical rider.

**The first-match method is `find`, not `match`.** Two reasons, either sufficient:
(a) `match` is a hard keyword (`KwMatch`) and is **not** in the parser's
contextual-member-name set (Parser.cpp `isContextualName`), so `re.match(s)` does not
parse today; (b) the principle — bare `match` is the pattern-dispatch control-flow
construct, and one word must not mean two things. C# `Match(input)` → `find(input)`.
The *type* `Match` is fine (capitalized, never in keyword position).

## 1. Public types (all top-level — qualified type names don't parse in type position
yet, info.md §12.6, so nothing public may live namespace-nested)

### 1.1 `class Regex` — the compiled pattern (reference semantics; share freely)

```
class Regex {
    // Construction — pattern-as-CODE: malformed pattern THROWS RegexException.
    new Regex(string pattern);
    new Regex(string pattern, RegexOptions options);
    new Regex(string pattern, string flags);          // "ims" string form
    new FromProgram(Array<int> program);              // comptime path (doc 1 §3)

    string        pattern();
    RegexOptions  options();
    int           groupCount();                       // capture groups, excl. group 0
    Array<string> groupNames();                       // declared (?<name>…) names

    bool          isMatch(string s);
    bool          isMatch(string s, int from);
    Match?        find(string s);                     // first match or None
    Match?        find(string s, int from);
    Array<Match>  matches(string s);                  // all non-overlapping matches
    int           count(string s);                    // number of matches; DFA-only,
                                                      //   never materializes a Match
    string        replace(string s, string replacement);              // replaces ALL
    string        replace(string s, string replacement, int count);   //   (C# parity)
    string        replace(string s, (Match) => string fn);            // evaluator
    string        replace(string s, (Match) => string fn, int count);
    Array<string> split(string s);
    Array<string> split(string s, int limit);

    // DBMS batch forms (doc 1 §4): one warm engine across a column.
    Array<bool>   isMatchAll(Array<string> rows);
    Array<int>    countAll(Array<string> rows);
}
```

Overloads here are user-code-facing (checker-resolved — safe); per bug #13 the *bodies*
only ever self-call distinct internal names.

### 1.2 `struct Match` — one match (a value; returned only on success)

```
struct Match {
    int index;              // byte offset of the match
    int length;             // byte length
    string value;           // the matched text
    Array<Group> groups;    // groups[0] = whole match (C# convention), then 1..n
    Group  group(int i);            // OOB throws (RuntimeException, like Array.at)
    Group? group(string name);      // named group; None if no such name
}
```

No `success` field: absence is `None` (`find -> Match?`) — the language already has a
word for "no match" and it is not a flag on a hollow object. The name→index map rides
along as a pure-value `Map` handle (cheap copy), which is what makes `group(name)` work
on a detached value.

### 1.3 `struct Group`

```
struct Group {
    bool matched;           // false = group did not participate (C# Group.Success)
    int index;              // -1 when !matched
    int length;             // 0 when !matched
    string value;           // "" when !matched
}
```

An unparticipating group inside a successful match is a real state (alternation arms),
so `matched` stays — unlike `Match`, where the whole value is absent instead.

### 1.4 `struct RegexOptions` + string flags

```
struct RegexOptions {
    bool ignoreCase;   // ASCII case-insensitive (v1; Unicode folding is a Track 03 rider)
    bool multiline;    // ^ and $ also match at \n boundaries
    bool dotAll;       // . matches \n too   (C# calls this Singleline — renamed: the
}                      //  C# name is the classic confusion; dotAll says what it does)
```

Named arguments make this the readable form: `Regex("^err", RegexOptions(multiline:
true))`. The **string-flags variants** (owner-requested) accept `"i"`, `"m"`, `"s"` in
any order: `Regex(p, "im")`, `regex::replace(s, p, r, "i")`. Unknown flag characters
follow the pattern's own error rule at that call site (throw on the code path, `None`
on the `compile` path).

**Why not an `enum`:** the landed enum is a closed value set — duplicate carriers are
errors and combined values aren't members, so C#'s `[Flags]`-style `IgnoreCase |
Multiline` cannot be expressed. A bool-struct with named args is the honest equivalent.
Deliberately absent options: `Compiled` (everything is compiled; comptime is our
"Compiled"), `RightToLeft` (niche), `ECMAScript` (n/a), `IgnorePatternWhitespace`
(deferred, cheap to add later).

### 1.5 `class RegexException : Exception`

Carries `message` **and** `int offset` (byte position in the pattern):
`"unterminated class at offset 7"`. Thrown by pattern-as-code paths only (§2.1);
catchable by contract as any `IException`.

## 2. `namespace regex` — compile entry + C#-static-style conveniences

### 2.1 The error-path split (one rule, stated once)

- **Pattern as data** → `regex::compile(...) -> Regex?` — `None` on malformed, never a
  throw (LA-13 acceptance; expected-outcome rule §12.6). For patterns from config,
  routes, user input.
- **Pattern as code** (constructors, the convenience functions below, where the pattern
  is a literal at the call site) → **throw `RegexException`** — a malformed literal is a
  programmer error and fails loud (§16: no silent-distant failures). And a *constant*
  pattern compiled via comptime (doc 1 §3) fails **at build time**.

### 2.2 Functions

```
namespace regex {
    Regex? compile(string pattern);
    Regex? compile(string pattern, RegexOptions options);
    Regex? compile(string pattern, string flags);

    Array<int> compileProgram(string pattern, string flags);   // pure; comptime target
                                                               // (throws on malformed —
                                                               //  = build error in a fold)
    // Convenience (compile-per-call semantics, backed by the LRU cache below).
    bool          isMatch(string s, string pattern);
    bool          isMatch(string s, string pattern, RegexOptions o);
    bool          isMatch(string s, string pattern, string flags);
    Match?        find(string s, string pattern);                 // + options/flags forms
    Array<Match>  matches(string s, string pattern);              // + options/flags forms
    string        replace(string s, string pattern, string replacement);          // + o/f
    string        replace(string s, string pattern, (Match) => string fn);        // + o/f
    Array<string> split(string s, string pattern);                // + options/flags forms
    int           count(string s, string pattern);                // + options/flags forms

    string escape(string literal);   // quote every metacharacter (C# Regex.Escape)
                                     // unescape: deferred (rare; subtle; add on demand)
}
```

**Pattern cache:** the convenience functions intern compiled patterns in an LRU cache
(`Map` + order array, capacity 16 — C# `Regex.CacheSize` precedent), so
`regex::isMatch(row, "\\d+")` in a loop compiles once. Namespace state is legal
(concretely-typed singleton, §6.6); single-threaded today; flagged as an LA-1
confinement rider alongside doc 1 §2's scratch note.

### 2.3 C# → Leviathan mapping (the porting table)

| C# | here | notes |
|---|---|---|
| `new Regex(p, opts)` | `Regex(p, opts)` | no `new` at call sites (§2 info.md) |
| `Regex.IsMatch(s, p)` | `regex::isMatch(s, p)` | statics → namespace fns |
| `r.IsMatch(s)` | `re.isMatch(s)` | |
| `r.Match(s)` | `re.find(s) -> Match?` | keyword + honesty ruling (§0) |
| `Match.Success == false` | `None` | absence is a value |
| `r.Matches(s)` | `re.matches(s)` | eager `Array<Match>` |
| `match.NextMatch()` | `re.find(s, m.index + max(1, m.length))` | no back-pointer on a value |
| `r.Replace(s, repl)` | `re.replace(s, repl)` | replaces all, both languages |
| `MatchEvaluator` | `(Match) => string` lambda | block-body lambdas landed |
| `r.Split(s, n)` | `re.split(s, n)` | |
| `Regex.Escape(s)` | `regex::escape(s)` | |
| `RegexOptions.Singleline` | `dotAll` | renamed (honest name) |
| `Group.Success/Index/Length/Value` | `matched/index/length/value` | |
| `RegexOptions.Compiled` | comptime `compileProgram` + `Regex::FromProgram` | build-time, stronger |

## 3. Replacement-string grammar (`replace`)

`$0`…`$99` (group by number; greedy two-digit read, longest valid group number wins),
`${name}` (named), `$$` (literal `$`), `$` before anything else / at end → literal `$`.
An unmatched group substitutes `""`. **A reference to a nonexistent group throws
`RegexException`** — replacement strings are code-authored; silent literal passthrough
(C#/JS behavior) is exactly the silent-distant footgun §16 bans. The replacement is
parsed once per `replace` call, before scanning.

## 4. Semantics contract (each row below is pinned by a corpus case)

1. **Leftmost-first, greedy-by-default** (Perl/C#/JS semantics, not POSIX-longest) —
   falls out of Pike VM thread priority (doc 1 §1.4-A).
2. **Empty-match advance:** a zero-length match at position i is reported, then the
   scan resumes at i+1 (prevents the classic infinite loop in `matches`/`replace`/
   `split`; matches C#/JS).
3. **`split` includes captured-group text** in the output when the separator pattern
   captures (C# behavior, kept for porting fidelity — documented loudly since it
   surprises people); `split(s, limit)`: at most `limit` pieces, remainder unsplit in
   the last piece (C# `count` semantics).
4. **`replace` replaces all by default**; the `count` overloads bound it (C# parity —
   note this differs from JS `String.replace`).
5. **Byte-oriented v1:** offsets/lengths are bytes; `.` matches one **byte**; a
   multi-byte UTF-8 scalar is not one `.` — stated in reference with the Track 03
   `chars()` cross-ref, and codepoint classes are the designed follow-up
   (`deferal-utf8-chars-string-ops.md` gains a regex line).
6. **`ignoreCase` is ASCII-only** in v1 (compile-time fold, doc 1 §1.2).

## 5. Worked examples (these go into reference.md and double as corpus seeds)

```
// Validation (anchored — the O(pattern)-per-row fast path)
Regex userRe = Regex("^[a-z0-9_]{3,16}$");
bool ok = userRe.isMatch(name);

// Extraction with named groups
Regex kvRe = Regex("(?<key>\\w+)=(?<val>[^;]*)");
Match? m = kvRe.find(line);
if (m != None) { console.writeln(m.group("key")?.value ?? ""); }

// Replace with group refs / with an evaluator
Regex dateRe = Regex("(\\d{4})-(\\d{2})-(\\d{2})");
string us    = dateRe.replace(s, "$2/$3/$1");
string masked = tokenRe.replace(log, (m) => "***" + m.value.subStr(m.length - 4, 4));

// One-shot convenience (cached), and the data path
if (regex::isMatch(input, "^\\d+$")) { ... }
Regex? r = regex::compile(userSuppliedPattern);     // None on malformed — no throw
if (r != None) { rows = rows.where((row) => r.isMatch(row)); }

// Build-time compilation (constant pattern ⇒ malformed = compile error)
comptime Array<int> EMAIL_P = regex::compileProgram("^[\\w.+-]+@[\\w-]+\\.\\w+$", "");
Regex emailRe = Regex::FromProgram(EMAIL_P);
Array<bool> hits = emailRe.isMatchAll(column);      // batch: one warm engine per column
```

(Raw strings — `suggested-features.md` §3.3 — would remove the `\\` doubling; noted
there as regex's DX dependency, not a blocker.)

## 6. Corpus & acceptance

`tests/corpus/regex/` — five lanes including ELF (zero natives; `strings_ext`
precedent). Pins: every §4 row, every §3 grammar case, every §1 signature exercised,
`RegexException` offsets, `None`-vs-throw split per §2.1, the checker test that an
un-narrowed `find()` result is a compile error in user code, and the LA-13 acceptance
trio (vector subset, pathological timing — owned by doc 1 M3 — and malformed→`None` on
the `compile` path). Differential rule: all five engines byte-identical output.

## 7. Milestones (interleaved with doc 1's; same track)

| M | deliverable | dates |
|---|---|---|
| L1 | `Regex`/`Match`/`Group` types + `isMatch`/`find`/`matches` over engine M1; ctor throw path + `RegexException` | Jul 14–16 |
| L2 | `RegexOptions` + flags strings; `replace` (all 4 forms, §3 grammar); `split`; §4 semantics pinned | Jul 17–19 |
| L3 | `namespace regex` conveniences + LRU cache + `escape`; `compile -> Regex?` data path | Jul 20–22 |
| L4 | `count`/batch forms, `FromProgram` + comptime example, full reference chapter, request-regex.md closed out, Atlantis handoff note (LA-13: `@Pattern`, route constraints, log scrubbing unblocked) | Jul 23–27 |

## 8. Deferred (named so they aren't re-litigated ad hoc)

Raw string literals (general feature, separate); `(~)` match operator (blocked on bug
#12, and wants an owner taste ruling); regex patterns in `match` arms (earn it later);
`Seq<Match>` lazy adapter over the iterator protocol; Unicode classes/folding (Track 03
riders); `unescape`; `IgnorePatternWhitespace`; scalar-indexed offsets; columnar/Block
scanning + `where` fusion (the DBMS endgame — arrives with dense string columns).

## 9. STOP conditions

- Any surface item that turns out to want a native or a parser change (incl. any
  temptation to add `match` to `isContextualName` — that is an owner call, not a
  track decision).
- A §4 semantics row that cannot be made identical across all five engines.
- The §2.1 throw-vs-None split proving unimplementable as specced (e.g. constructor
  throw fails P4) — escalate with the probe data; do not silently swap in a sentinel.

## 10. Reference-doc duty

`docs/reference.md`: new §6.4.x "Regex" (types, method tables, flags, replacement
grammar, §4 semantics rows, byte-orientation warning, comptime recipe, one-paragraph
engine overview from doc 1). `info.md` §11: one sentence adding regex to the data
toolkit with the linear-time guarantee. `designs/request-regex.md`: status line →
superseded by these two docs (surface renames: `test`→`isMatch`, `findAll`→`matches`,
`replaceAll`→`replace`; `Match` struct extended with `value`/`groups` of `Group`).

## 11. Implementation log

**2026-07-11 — STOP before landing: blocking LLVM engine bug in `Array<Struct>.at(i)`.**

Before writing `kPreludeRegexApi`, the constructor/overload/exception/namespace-
state mechanisms this surface depends on were probed in isolation against all
four required lanes (treewalk, IR, emit-cpp, LLVM) to de-risk the unusual
shapes doc 2 calls for:

- Struct constructors with named/defaulted args (`RegexOptions(multiline:
  true)`) — works on all four lanes.
- Namespace-scoped mutable state (a `Map`/`Array` LRU cache living directly in
  `namespace regex`, per §2.2) — works on all four lanes.
- `try`/`catch` catching a `RuntimeException` thrown by the engine
  (`regex::compileProgram`) and re-throwing a new exception type carrying an
  extra field (`RegexException`'s `offset`), or swallowing it into `None` —
  works on all four lanes.
- `T?` narrowing, `?.`, and `??` over a **struct**-typed optional (`Group?`),
  when done inside a function body — works on all four lanes identically to
  class-typed optionals. (Narrowing at bare top-level script scope does not
  work on ANY type, struct or class — a pre-existing, general limitation
  unrelated to this track; every corpus/example needs its logic inside a
  function, matching the rest of the existing corpus's own convention.)

The next probe — a `struct` with an `Array<OtherStruct>` field, read back via
`.at(i)` from a method (the exact shape `Match.groups: Array<Group>` /
`Match.group(int i)` needs) — surfaced a genuine, reproducible LLVM backend
bug, isolated down to a minimal, regex-free repro with no classes or methods
at all:

```
struct Grp { bool matched = false; string value = ""; }
void run() {
    Grp g0; g0.matched = true; g0.value = "whole";
    Array<Grp> groups = [g0];
    console.writeln(groups.length());   // 1 on every engine
    Grp r = groups.at(0);               // LLVM only: bogus OOB throw
}
run();
```

`--run`/`--ir`/`--emit-cpp` all print `1` then read `r` fine; `--build-native`
(LLVM) throws `Uncaught RuntimeException: index 0 out of bounds (length
-9223372036854775807)` on a **valid** index. `for (Grp g in groups)` over the
identical array iterates correctly on LLVM — only the named `.at(i)` method is
affected. Filed as **bug.md #33 [P0]**, with the root cause pinned to
`src/LlvmGen.cpp`'s `.at()` lowering for tag-6 receivers (~line 1507–1530):
it reads the dense-value-struct-array length header unmasked and never
branches on its sign bit the way `Op::IterLen`/`Op::IterAt` (~line 2600–2665,
same file) already do for the exact same dense/boxed distinction.

**Why this is a STOP, not a workaround-and-continue.** §1.2/§1.3 of this doc
specify `Match`/`Group` as top-level structs, with `Match.groups: Array<Group>`
and `Match group(int i)` doing indexed lookup, and `Regex.matches(s) ->
Array<Match>`. Both are structs stored in arrays — exactly the broken shape.
This is not an internal-implementation choice this track made and could route
around: it is the literal public contract in §1.1 (`Match group(int i)`,
`Array<Match> matches(string s)`), and ordinary user code calling `.at()` on
either returned array hits the same bug regardless of how the surface's own
methods are written internally. LLVM is one of the four required maintained
lanes (doc 1 §8's gate), so this blocks the captures half of the surface
(`find`, `matches`, `replace` with an evaluator lambda, `split` with capturing
separators — everything downstream of `Match`/`Group`) at the language level,
not at the design level. Per the STOP protocol (Sonnet: stop and escalate,
never improvise; see doc 1 §9's own M1 precedent), work stops here rather than
inventing a workaround (e.g. reshaping `Match`/`Group` to avoid struct arrays)
that would contradict the owner-directed C#-shaped public surface this doc
specifies.

**What is NOT blocked** (does not touch a struct-typed array): `RegexOptions`
(a struct, but never stored in an array), `RegexException`, `Regex.isMatch`,
`Regex.count`, `Regex.groupCount`, `Regex.isMatchAll`/`countAll` (already
`Array<bool>`/`Array<int>`, primitive element types), and the
`compile`/pattern-cache plumbing. Landing only that slice was considered and
rejected for this session: it is a small, oddly-shaped fragment of the C#-style
surface the owner asked for (no `find`/`matches`/`replace`/`split`, i.e. no
capture access at all), and per the standing "never revert validated work"
rule, a partial land now would need to be revisited/extended once bug #33 is
fixed rather than being a clean increment. Nothing has been written to
`src/Resolver.cpp` for this track yet — the STOP fired before any code
landed, per the probe-first approach above.

**Next step:** owner ruling on bug #33 (fix `LlvmGen.cpp`'s `.at()` lowering
to mirror `IterAt`'s dense/boxed branch), then resume this doc's
implementation from scratch with the probes above as the validated starting
point.

**2026-07-11 — bug #33 fixed; L1-L4 landed in full.**

- Confirmed the fix (`e4189b6`, masks the dense-array marker bit and adds the
  missing dense/boxed branch to `.at()`, mirroring `Op::IterAt`) with the
  exact repros from the STOP entry above, on all four maintained lanes, before
  resuming.
- `kPreludeRegexApi` added to `src/Resolver.cpp` (a new prelude segment after
  `kPreludeRegexCore`, concatenated in `parsePrelude`): the five top-level
  types (§1) and the `namespace regex` conveniences + LRU pattern cache (§2)
  as specced, with every internal call fully qualified and every method body
  delegating to a distinctly-named helper (never a same-type overloaded
  self-call) per the bug #13 discipline.
- One additive, non-breaking extension to the engine boundary was needed and
  made: `RegexCoreVm.nameRecords()` / `regex::programNameData(program)` in
  `kPreludeRegexCore`, exposing the already-parsed named-group table (raw
  `(groupIndex, nameLength, nameBytes...)` records) so `Regex.groupNames()`
  and each `Match`'s name→index lookup don't have to re-parse pattern text.
  No existing engine behavior changed.
- **Two more bugs found and worked around during implementation** (neither
  blocked landing; both are pre-existing, general compiler bugs, not
  regex-specific):
  - A qualified write to a namespace-scoped variable from within its own
    namespace (`regex::apiCache = m;`) fails to lower on `--ir`/`--emit-cpp`/
    `--native-obj` (breaks the WHOLE prelude, not just regex — every program
    failed with `IR: not yet lowerable: name 'regex'`) even though the
    equivalent BARE assignment (`apiCache = m;`) lowers fine. Fixed by using
    bare assignment for the two namespace-scoped cache fields; qualified
    *reads* are unaffected and kept for the file's usual qualify-everything
    discipline. Not filed as a numbered bug.md entry (caught and fixed before
    it reached a corpus lane; documented here and inline at the call site
    instead, since the workaround is a one-line, permanent, correct fix, not
    a standing hazard other tracks would rediscover) — see the comment at
    `apiCachedProgram` in `src/Resolver.cpp`.
  - **bug.md #34 [P1]:** a lambda-literal call argument is scored as
    applicable to a `string` (or any non-function) overload parameter, so
    `Regex.replace`'s `(string, string)`/`(string, (Match)=>string)` sibling
    overloads silently picked the WRONG one (the closure got passed where a
    string was expected) whichever was declared second. Root cause in
    `Checker::pickInjecting`/`Checker::pickOverload`'s applicability scoring
    (src/Checker.cpp) — full detail in the bug.md entry. Worked around by
    declaring every lambda-typed `replace` overload (on `Regex` and in
    `namespace regex`) BEFORE its string-typed sibling, so declaration-order
    tie-break lands on the correct one; the underlying scorer bug is
    unfixed and will bite the next unrelated overload set shaped this way.
- `tests/corpus/regex/` (9 programs: basics, matches_iteration, replace,
  split, options_flags, exceptions, batch, comptime, namespace_conveniences)
  plus `tests/negative/regex_unnarrowed_find.lev` (+
  `tests/run_regex_unnarrowed_find_error.sh`) for the §6 un-narrowed-`find()`
  acceptance item. All corpus-green, byte-identical, on treewalk/IR/emit-cpp/
  LLVM (`ctest -R regex`: 11/11 passed). Differential ground truth is the
  tree-walk oracle's captured output.
- Found along the way, not a blocker: Leviathan's own string-interpolation
  syntax (`"...${expr}..."`) collides with this doc's `${name}` replacement
  grammar — a replacement template with a named-group ref must be spelled
  `"\${name}"` in source (the existing `\${` escape, reference.md §3.2) to
  deliver the literal three characters to the regex engine. Documented in
  reference.md §6.4.6 and pinned by `tests/corpus/regex/replace.lev`.
- `docs/reference.md` §6.4.6 (new) documents the full public surface: type
  tables, options/flags, the replacement grammar (with the interpolation
  gotcha above), the §4 semantics rows, the byte-orientation note, and the
  comptime recipe; §6.4.4's status line and the top "Last Updated" section
  were updated. `info.md` §11/§18/Last-Updated all updated to "landed in
  full". `designs/requests/request-regex.md`'s status line marked
  superseded.
- All L1-L4 milestones (§7) are done. This doc moves to `designs/complete/`
  in the same change.
