# Request: Comptime Template Import (LA-20)

**From:** Atlantis Track 09 (views). **Date:** 2026-07-07.
**Priority:** P2 — post-AG-7 upgrade; v1 ships a boot-parsed runtime engine behind the
same `view()` surface either way. Filed now because it is the single ask that turns
template typos into compile errors, and it may inform how comptime hermeticity is scoped.

## 1. The ask

A narrowly-scoped exception to comptime's no-I/O rule: **build-input file inclusion**.

```
comptime string tpl = import("views/links/index.html");
```

- Resolved at compile time against the project root (manifest-relative, no absolute
  paths, no `..` escapes); the file's content becomes a comptime string constant.
- The imported file is a declared build input: its content hash participates in the build
  (same determinism class as a source file — this is NOT runtime I/O sneaking in; it is
  widening "the build's inputs" from *.lev to declared assets, the same thing `sources`
  globs already do for code).
- Hermeticity stays intact: no network, no clock, no writes, no reads outside the
  project tree; a missing file is a compile error.

## 2. What it unlocks (why it's worth an exception)

With template text available at comptime, the views engine's parser (ordinary language
code) runs at compile time: per-template generated `render(Model)` functions, dot-paths
checked against model struct fields via `meta.*` — **a template typo becomes a compile
error** — zero boot-time parse cost, `--expand`-visible render code, and the path to
context-aware autoescaping. The competitor Loom draft promised type-checked templates but
had no mechanism; this is the mechanism. Also plausibly useful beyond views: embedded SQL
files, fixtures, `openapi.json` goldens.

## 3. Acceptance

1. Corpus: import a text file → comptime string equals disk content; missing file /
   escape path → compile error; content change → recompile observes it (hash-keyed).
2. `--expand` shows code generated from imported content like any comptime fold.
3. Track 09 upgrade proof: one demo template compiled via import renders byte-identically
   to the runtime engine's output for the same model (the swap-in acceptance).

## 4. Interim fallback (v1 design of record)

Runtime engine: boot-time parse of views/ with fail-fast diagnostics, dev-mode mtime
reload, models crossing as JsonValue via C7 toJson(). Unchanged public surface
(`view(name, model)`), so the upgrade is invisible to app code.
