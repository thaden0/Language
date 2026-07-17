# Packet 03 — the loud comparability gate (Model: **Opus**)

Design §5.1: a `==`/`!=` on a value struct whose fields aren't all
comparable is a **compile error naming the first non-comparable field** —
the silent path becomes structurally impossible at check time.

## Where

`Checker::typeOfBinary`, the user-class branch `Checker.cpp:2046-2067`.
Today a value struct with no `(==)` overload falls through the
reference-identity branch (gated `!lt.sym->isValue`, line 2062) to
`return unknown()` (line 2066).

Insert, after the overload attempts (line 2059) and BEFORE the
reference-identity branch:

```cpp
// Design §5.1: value-struct ==/!= with no (==) — synthesized or explicit —
// is the comparability gate firing. Name the first bad field, loudly.
if (lt.sym && lt.sym->isValue &&
    (e->op == TokenKind::EqEq || e->op == TokenKind::BangEq)) {
    for (const StructEqSynth& s : program_->structEqSynths)
        if (!s.synthesized && s.structName == lt.sym->name)
            return error(e->span, "struct '" + std::string(lt.sym->name) +
                "' has no '(==)': field '" + s.badField + "' (" +
                s.badKindNote + ") is not comparable — define an explicit "
                "'bool (==)(" + std::string(lt.sym->name) + " other)' to opt in");
}
```

Exact message shape per design §5.1. `program_` is already on the Checker
(`Checker.hpp:75`).

## Guard rails

- Fire ONLY for `==`/`!=` on **value** structs. Reference classes keep the
  identity branch verbatim. Other operators keep their current behavior
  (out of scope).
- If the struct is a value struct, has no `(==)`, and has NO
  `structEqSynths` entry at all (shouldn't happen once packet 02 runs on
  every user struct — but prelude/edge types may slip through), fall through
  to the existing `unknown()` — do not invent a second error path. Add a
  checker unit test only for the covered case.
- Both operand orders: the branch keys off `lt` (left type). `5 == p` with a
  struct on the right and primitive on the left takes the primitive branch
  (line 2036-2044) and stays a type-mismatch story — unchanged, fine.
- Do not touch the `(!=)`-derives-from-`(==)` logic (lines 2049-2052).

## Tests

Append to `tests/test_checker.cpp` (note: file has unrelated in-flight
edits — append only, commit only your hunks):

```cpp
ERROR_HAS("struct Job { Array<int> xs; } void main(){ Job a; Job b; bool c = a == b; } main();",
          "field 'xs'");
ERROR_HAS(<function-value field variant>, "not comparable");
ERROR_HAS(<Map field variant>, "define an explicit");
```
Adjust source snippets to real syntax (check neighboring tests for the
harness's program shape). Also one POSITIVE check: a comparable struct
`==` still type-checks (no error) — use the file's existing "no error"
helper if present, else `checkErrorContains(...) == false`.

Also land the design's red-corpus equivalent as a checker test rather than
a corpus file (compile errors don't run engines; the composition red lane
is for wrong-OUTPUT bugs, not diagnostics).

## Warnings

- The d68f1e8 merge removed the #77 silent-false *entry* from trackers but
  the RUNTIME silent-false still exists until packet 04. After this packet,
  non-comparable struct `==` errors at compile time; comparable structs use
  the packet-02 method; the runtime fallback is now fully dead code on
  checked paths. That's the intended half-state.
- `structEqSynths` name matching: struct names may collide across
  namespaces. If `Symbol*` is available in the metadata (packet 02 records
  `structName` only), prefer upgrading packet 02's record to carry the
  `const Stmt*` decl pointer and match on `lt.sym->decl` — coordinate; do
  not ship a name-collision false-positive. Check how `enumDesugars`
  handles this (it matches on name too — mirror whatever it does, and if
  it's name-only, keep parity and note it).

## Acceptance

`checkertests` green (new tests included); full `ctest` green; four-lane
equality cluster unchanged. Commit:
`Struct equality packet 03 (M1): loud comparability gate at ==/!= use sites`.
