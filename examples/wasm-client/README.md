# wasm-client — the Atlantis-client demo (Track W, W-M4)

The W-M4 gate artifact of the WebAssembly front-end target
(`designs/wasm-frontend/techdesign-06-bindgen-and-ship.md` §4): a small,
real Leviathan program that runs **in the browser** — parse JSON, render a
DOM list, handle clicks that mutate state and re-render.

- **[`todo.lev`](todo.lev)** — the client. Parses a JSON task list (a local
  static fixture standing in for a `fetch` response — the fetch-as-a-stream
  endpoint is the one W-M3 remainder, doc 05 §9 #6), renders it as a `<ul>`,
  and installs a *complete next* click handler that mutates a row and
  re-renders a live `remaining:` status line.
- **[`todo.expected`](todo.expected)** — the pinned console trace.
- **[`index.html`](index.html)** — the browser loader. Mounts the program's
  `<ul>` into the live `document` and streams its console output into a `<pre>`.

## Run it headlessly (the CI-adjacent lane)

```sh
tests/run_wasm_dom.sh build/leviathan examples/wasm-client/todo.lev
```

`run_wasm_dom.sh` builds `todo.lev` for `wasm32`, runs it under Node's native
`WebAssembly` against the in-memory DOM stub (`tests/wasm_node_run.mjs`'s
`nodeDom`, the stand-in for a browser `document`), and diffs stdout against
`todo.expected`. It **skips cleanly** (does not fail) when the `wasm32` runtime
archive or the wasm linker is absent — those belong to the archive owner, not
this lane (build the archive with `runtime/build-triple.sh wasm32-wasi`, which
needs a `wasi-libc` sysroot; see `designs/wasm-frontend/techdesign-03-floor-wasm.md`).

## Run it in a real browser

```sh
leviathan --build-native examples/wasm-client/todo.wasm \
          --target wasm32-unknown-unknown examples/wasm-client/todo.lev
(cd examples/wasm-client && python3 -m http.server)
# open http://localhost:8000/  in Chrome >= 137
```

**Browser floor:** the DOM click handler awaits the dispatch trampoline's own
promising activation (doc 05 §4), so this needs **JSPI** — **Chrome ≥ 137**
(JSPI on by default), or Node ≥ 24 with `--experimental-wasm-jspi` for the
headless lane. The host fails loud naming the floor when JSPI is absent
(`runtime/lv_host.js`).

## What this exercises

- the DOM bridge marshaler + handle table + closure trampoline (doc 05);
- events-as-self-dispatch, deferred to their own turn (doc 05 §4 — the reason
  the handler lines print *after* the synchronous ones);
- the pure stdlib on wasm unchanged: `json::parse`, `String`, `Array`, the
  `Dom` surface — all the same source that runs on every other engine, modulo
  the documented capability subset (overview §3).

Cross-link: the consumer framework is
[`designs/atlantis/techdesign-09-views.md`](../../designs/atlantis/techdesign-09-views.md).
