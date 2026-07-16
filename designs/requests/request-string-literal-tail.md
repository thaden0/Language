# Summary: Remaining string-literal forms — `\u{...}` escape, raw strings, multiline strings

Track 01/04 landed most of the string-literal completeness pass recommended in
`designs/suggested-features.md` §3.3 — `\r` is documented, `\xNN` byte escapes work, hex/
binary numeric literals and digit separators shipped (§3.2), and string interpolation
landed (§3.1). Three specific literal forms from that same section did not: the `\u{...}`
Unicode escape, raw string literals, and triple-quote/multiline strings. This ticket tracks
the remainder now that the source document is being retired.

## Request Details

- **`\u{...}` Unicode escape** — insert an arbitrary Unicode scalar by codepoint
  (`"\u{1F600}"`), the natural sibling of the already-landed `\xNN` byte escape. Blocked-ish
  on the `char`/`chars()` scalar-vs-byte semantics settling first (`suggested-features.md`
  §3.3 flagged this dependency), which Track 03 has since landed (info.md §9, §19#9) — so
  the dependency this was waiting on is now resolved and the escape itself is unblocked.
- **Raw strings** — a literal form where backslash escapes do not apply, wanted for regex
  patterns, Windows-style paths, and embedded JSON/HTML in test fixtures. `` `...` `` is
  already taken by quasiquotes (info.md §16.5), so `suggested-features.md` suggested
  `r"..."` as the likely spelling — flagged here as a suggestion, not a decision, since
  syntax choices in this space need owner sign-off per the request-template's own guidance.
- **Triple-quote / multiline strings** (`"""..."""`) — answers the multiline-string gap
  templates and embedded-HTML/SQL/JSON code want badly; `suggested-features.md` noted this
  can double as the raw-string answer if triple-quote strings are also raw, or be a
  separate, escape-processing form — another open call for whoever designs this.

## Requested Specific Feature

No syntax is prescribed here beyond the candidates already named above (`\u{...}`,
`r"..."`, `"""..."""`) — each needs a lexer production and, for raw/multiline forms, a
decision on whether they interact with the already-landed `${...}` interpolation (does
`r"..."` suppress interpolation the way it suppresses escapes? do triple-quoted strings
still interpolate?).

## Known Warnings

- `\u{...}` should reuse whatever scalar-validity rules `char`/`chars()` already settled
  (invalid/surrogate codepoints, info.md §19#9's "invalid bytes → U+FFFD, never a throw"
  precedent) rather than inventing a second validation story.
- Raw strings and interpolation both touch the lexer's string-scanning path; sequence
  raw-string design after checking Track 01's interpolation implementation
  (`designs/complete/techdesign-01-literals-operators.md`) for a clean seam.

## Acceptance Criteria

1. `\u{...}` parses to the correct Unicode scalar, consistent with `char`'s scalar model,
   on every active engine.
2. A raw-string form exists (exact spelling is the owner's call) where backslash sequences
   are literal.
3. A multiline string form exists, with an explicit, documented answer to "does it
   interpolate."
4. `docs/reference.md` §1.4 (or wherever escapes are documented today) gets all three forms.

## Interim Fallback

Unicode codepoints outside the BMP-by-escape use `char::fromCode(n)` at the call site
instead of a string literal escape; raw/multiline content is built with ordinary escaped
strings and `+`-concatenation or `StringBuilder`. No code is blocked waiting on this.
