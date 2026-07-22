# Gotchas

Shared landmine list for all agents. One line per entry. Read this before starting implementation or bug-hunting work; add an entry (in the same commit) whenever you discover a new trap that is not derivable from the code. See docs/policies.md § Gotchas.

- Never name a local variable or parameter `expr` — it breaks native method calls on the IR and LLVM backends (bug #105).
- Rebuild the compiler from HEAD before running a repro probe or filing a compiler bug — `build/leviathan` is frequently stale because parallel agents commit continuously, and stale binaries produce false bug reports.
- UTF-8 decoding logic exists as THREE synchronized copies (`utf8DecodeAt` in the checker/oracle path, `u8dec` in the IR runtime, `lv_utf8_decode_at` in the C runtime) — any change to one must be made to all three.
- When merging known_bugs_1.md/known_bugs_2.md across branches from before the branch-prefix numbering policy, the same bug may appear under different numbers on each side — cross-reference repro shape and dates before resolving; prefer the side with an actual code fix.
- `Http::View.render()` requires an installed `Views::Engine` and throws without one — demo/test code predating Track 09 may need `Views::Engine` installation added.
- Overloaded functions called from an UNCHECKED prelude body (prelude/*.lev) resolve by ARITY only, not by argument type — the checker stamps no target on prelude-body calls, so Lower.cpp/Eval.cpp pick the overload whose param count matches. Keep same-named prelude overload families arity-unique (e.g. the DOM marshal family uses a distinct `__child` name for its arity-3 DomNode overload rather than a fourth `__act`); two same-named same-arity prelude overloads are indistinguishable there and one will be silently mis-called.
