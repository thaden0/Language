# Request: Stdlib Regex (LA-13)

**Status 2026-07-11: superseded — both docs landed in full.** Engine M1-M5
(`designs/complete/techdesign-regex-engine.md`, linear-time NFA/DFA, DBMS
performance profile) and the C#-shaped public surface
(`designs/complete/techdesign-regex-library.md`: `Regex`/`Match`/`Group`/
`RegexOptions`/`RegexException` + `namespace regex` conveniences) are both
implemented and corpus-green on all four maintained lanes. Surface renames
vs this request: `test`→`isMatch`, `find` kept, `findAll`→`matches`,
`replaceAll`→`replace`; `Match` extended (`value`, `groups` of `Group`). The
§1 requirements below (linear time, zero natives, malformed→`None` on the
data path) are preserved verbatim in the designs. `@Pattern`/route-param
constraints (§2) are now unblocked for Atlantis to pick up.

**From:** Atlantis framework (Track 02 validation; also 08 log scrubbing, route
constraints). **Date:** 2026-07-06. **Priority:** P2 — Atlantis v1 ships without
`@Pattern`; this unlocks it plus route param constraints.

## 1. Requirement

A `namespace regex` in the stdlib, **in-language over the string toolkit** (zero natives —
Track 09's proven pattern), with the deliberately bounded engine class:

- **Linear-time NFA simulation (RE2 discipline), no backtracking** — a web framework
  cannot expose a ReDoS surface through its own validation layer. This is a hard
  requirement, not an implementation detail: catastrophic backtracking on user-supplied
  input is the exact silent-distant footgun §16 bans.
- Surface: `Regex? compile(string pattern)` (`None` on malformed — data errors are
  values); `bool Regex.test(string s)`; `Match? Regex.find(string s)` /
  `Array<Match> findAll` (`Match { int start; int len; Array<string?> groups; }`);
  `string Regex.replaceAll(string s, string replacement)` with `$1`-style group refs.
- Supported syntax (the RE2-safe subset): literals, `.`, classes `[a-z0-9_]` (+ negation,
  escapes `\d \w \s`), anchors `^ $`, alternation `|`, grouping `(…)` (capturing) /
  `(?:…)`, quantifiers `* + ? {m,n}` (greedy only in v1). Explicitly out: backreferences,
  lookaround (both incompatible with the linear-time guarantee).
- Byte-oriented v1 (matches the language's byte-clean string stance); codepoint classes
  can follow Track 03's `chars()`.

## 2. What Atlantis does with it

`@Pattern("...")` validation attribute (C5 — currently dropped from v1); route parameter
constraints (`/users/:id(\d+)`); input scrubbing in the logging layer.

## 3. Acceptance

1. Vector corpus: a published regex test set subset (match/no-match/group extents),
   including the classic pathological patterns (`(a+)+$` against `"aaaaaaaaaaaaaaaaaaaaX"`)
   completing in linear time — timed in the corpus, not eyeballed.
2. All five engines via ordinary lowering (zero natives), oracle==IR==LLVM.
3. Malformed patterns → `None`, never a throw (expected-outcome rule, info.md §12.6).

## 4. Interim fallback (already designed in)

No `@Pattern` in C5 v1; `@Email`/`@Url` validators are hand-written scanners over string
methods; route params constrain by type only (`int` parse).
