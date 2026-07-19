# Request: Ship the Stdlib Prelude as `.lev` Files (W-M4 unblock)

**From:** Track W (wasm frontend), doc 06 §2. **Date:** 2026-07-19.
**Priority:** P1 — blocks W-M4 (wasm packaging/shipping close).
**Owner ruling (2026-07-19):** *"#2 has always been the goal, and that should have been
well established by now."* Resolved: the stdlib ships as `.lev` source files, not as
in-binary per-target segment selection. This closes the open question tracked at
info.md §19 #18, `designs/wasm-frontend/techdesign-00-overview.md` §5,
`designs/wasm-frontend/techdesign-06-bindgen-and-ship.md` §2, proposal §398.

## 1. Current state (verified 2026-07-19)

`Resolver::parsePrelude()` (`src/Resolver.cpp:5448-5452`) concatenates six raw C++
string-literal segments unconditionally into every compiled program, regardless of
target:

```cpp
preludeFile_.text = std::string(kPreludeCore) + kPreludeStd +
                    kPreludeRest + kPreludeRegexCore + kPreludeRegexApi + kPreludeWeb +
                    kPreludeWasm;
```

Every non-wasm build compiles-and-discards the whole DOM/JS bridge surface
(`kPreludeWasm`, `src/Resolver.cpp:5313`); every wasm build carries the full
HTTP/socket/thread prelude it can never reach. No per-target selection exists;
segmentation (Track 09 §0) landed but file-shipping did not.

## 2. The ask

Replace the `kPrelude*` raw-string-literal segments compiled into the binary with real
`.lev` source files shipped alongside (or embedded as read-only resources in) the
compiler binary, loaded by a real `parsePrelude()` file-reading seam instead of string
concatenation. Per-target selection (which files load for which `--target`) is a detail
*within* this model, not an alternative to it — e.g. a wasm build loads
`core.lev + std.lev + rest.lev + web.lev + wasm.lev` while a native build skips
`wasm.lev`.

## 3. Why this and not per-target in-binary segments

Per-target segment selection (target-gated string concatenation, keeping everything as
compiled-in raw strings) was the alternative on the table and is explicitly rejected —
it doesn't change the compiler's distribution story (the prelude source stays opaque,
baked into the binary, unavailable for a user to read/vendor/patch) and doesn't address
prelude compile-time cost as more segments land (info.md §19 #18's stated trigger).
Shipping files is the architecturally-committed direction; per-target gating without
file-shipping is not an acceptable place to stop.

## 4. Acceptance criteria

1. `parsePrelude()` reads `.lev` files from a known install-relative path (or an
   embedded resource table) instead of concatenating `kPrelude*` C++ string constants.
2. Byte-identical prelude AST/lowering output to the current concatenation, on the full
   existing differential corpus, for the default (native) target.
3. A wasm build loads only the wasm-relevant file set (today's `kPreludeWasm` content,
   moved to `wasm.lev` or equivalent) plus the shared segments — Web/HTTP/socket/thread
   segments that don't apply to wasm are excluded at load time, not just dead-code-
   eliminated post-hoc.
4. Compiler distribution (install/build output) documented: where the `.lev` files live
   relative to the binary, how a non-standard install path is configured if at all.
5. `docs/reference.md` / `info.md` updated to describe the shipped-files model
   (supersedes the in-binary-concatenation description).

## 5. Scope note

This unblocks techdesign-06 §2 / W-M4 packaging close. It does **not** by itself unblock
techdesign-06 §1 (the DOM bindgen) — that is a separate, still-open metaprog scope
question, tracked in `designs/requests/request-bindgen-metaprog-scope.md`.

## 6. Interim fallback

None needed — this is a ship-readiness item, not a blocker on other Track W work.
W-M1–W-M3 already ride the existing in-binary concat per doc 00 §5's own scoping; only
W-M4's "shipping" gate needs this landed.
