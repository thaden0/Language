# Track W — doc 6 of 6: rules bindgen, packaging & ship (W-M3 part 2, W-M4)

**Status:** PROPOSED. **Depends on:** doc 05 (the bridge the bindgen generates stubs for).
**HARD content:** none.

## 1. The bindgen — `@extern` via the rules engine (W-M3)

All dependencies landed (metaprog Phases 1–4 + F4, `info.md:1818-1821`). The in-tree model
is `@Test` auto-discovery (`techdesign-unit-test-library.md:174,187-196`): match an
attribute, `$for` over methods, inject generated decls `at namespace`. The DOM bindgen maps
1:1:

```
namespace Dom {
    attribute extern { string kind; }        // @extern("dom") on a declared interface class
    rule bindExtern {
        match @extern(e) on class C
        inject `$for m in C.methods
                  : Dom::__import($e.kind, "$C", "$m", ($_params) => this.$m($_args))`
        at namespace DomBindings
    }
}
```

Grounded constraints that shape the rule (all from the dossier §8.6, verified there):

- **`meta.*` reflects attribute *names* only, never arguments** (`Rules.cpp:1421-1424`) —
  the rule must *match* `@extern(e)` to read `e.kind`; never try to reflect it.
- `$for` over `C.methods`, `$m`/`$C` splices, `$_params`/`$_args` forwarding, and the
  `at namespace` generated-decl channel (`Rules.cpp:1818-1829`, flushed into
  `program.items`, pass-2-resolved like hand-written code) are the only primitives used.
- Def-site qualification (`techdesign-metaprog-phase3.md:530-567`) keeps `Dom::__import`
  references valid when spliced into user files.

Scope discipline: v1 generates **import declarations + marshaling stubs** for
handle-wrapper classes (methods whose params/returns are immediates, strings, or handle
wrappers). Anything fancier (callbacks in signatures beyond the event pattern, structs
by value) is hand-written in the `Dom` prelude surface until a real consumer demands
generation. Doc 05's hand-written v1 stubs are replaced by generated ones in this packet;
diff the generated output against the hand stubs as the packet's own differential.

## 2. Per-target stdlib consumption (W-M4, gated on the owner ruling)

Doc 00 §5 holds the stance: escalate at W-M1 start; do not improvise. When the ruling
lands, this packet consumes it:

- the wasm-relevant prelude surface (`Dom`, the fetch/WebSocket endpoints, anything
  browser-only) becomes a per-target segment (working name `kPreludeWasm`) included only
  for wasm builds — or a shipped file, per the ruling's shape;
- conversely nothing OS-only needs *excluding* for correctness (the doc-02 gate already
  handles it); per-target selection is a size/cleanliness win, not a correctness need —
  say so in the packet so the ruling isn't blocked on a false urgency.

If the ruling is still open at W-M3 complete: **STOP condition 2** — escalate again; W-M4's
demo may proceed on dev builds, but the track does not close.

## 3. Size & performance passes (W-M4, best-effort)

- Record `.wasm` sizes at O0/O2, gzipped, in the log from W-M1 on — no budget enforced in
  v1, but the curve must be visible.
- `wasm-opt -O2` post-pass: measure once; adopt only if free (no behavior diff on the
  corpus) and meaningful.
- The zero-copy `TypedArray` path and `externref` handles remain logged future
  optimizations (doc 05 §2/§3), not W-M4 work.

## 4. The Atlantis-client demo (the W-M4 gate)

A real, small Atlantis client page: fetch JSON from an endpoint (can be a local static
fixture), render a list into the DOM, handle clicks that mutate state and re-render, one
`WebSocket` echo if §6's WebSocket endpoint landed. Cross-link
`designs/atlantis/techdesign-09-views.md` as consumer. The demo lives under
`examples/wasm-client/` with the loader page and a README that states the JSPI browser
floor.

## 5. Definition of done (whole track)

- All W-M1–W-M4 gates green; wasm lane running its declared corpus subset in CI-adjacent
  scripting (`tests/run_wasm.sh`, `tests/run_wasm_dom.sh`), byte-identical to `--ir` on
  the portable subset.
- Zero edits to frozen X64/ELF; every **[HARD]** edit reviewed with four-lane differential
  evidence in its commit.
- Emscripten/Asyncify artifacts labeled bring-up and absent from any required lane.
- Docs swept (§6); track docs moved to `designs/complete/`; proposal's §14 implementation
  log updated with the final state.

## 6. Docs sweep (W-M4 close)

- `info.md`: new §20 — the wasm target: capability-subset framing (per-target, never a
  dialect), floor-as-imports, JSPI = the browser realization of §14's landed stackful
  tasks, the threads-leg deferral. §19: mark the packaging ruling consumed (or still-open
  status honestly); add the encoding ruling outcome (doc 05 §7).
- `docs/reference.md`: backend/target matrix row for `wasm32` (covered subset, gated set,
  diagnostics); the `@extern` surface.
- `docs/footguns.md`: the marshaler header-gate footgun (`payload-16` on literal
  strings/dense records) and the DOM↔closure cycle shape.
