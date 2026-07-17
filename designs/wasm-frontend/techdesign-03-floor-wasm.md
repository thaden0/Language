# Track W — doc 3 of 6: the floor retarget — `runtime/lv_plat_wasm.c` (W-M1, part 2)

**Status:** PROPOSED. **Depends on:** doc 02 (archive + link lane land together with this).
**HARD content:** none. This is a new `.c` file behind the enforced seam
(`lv_plat.h:1-4`: "a new target is a new lv_plat_*.c file, nothing else") plus a JS host
file. Zero compiler edits, zero `lv_runtime.c` edits.

## 1. Shape

One file, `runtime/lv_plat_wasm.c`, implementing the ~40 `lv_plat_*` symbols
(`lv_plat.h:22-142`) against **host imports**, plus the JS half, `runtime/lv_host.js` — a
small ES module that instantiates the wasm module and supplies the imports. POSIX is 499
lines, Win32 459; expect the same order, smaller (absent capabilities are one-line error
returns).

**Import module name: `lv`.** Every import the floor consumes is
`(import "lv" "<name>" ...)`; the JS host builds that object in one place. Keep the import
surface *smaller* than the floor surface — most floor functions are pure C over linear
memory (allocator bookkeeping, buffering) and import nothing.

## 2. The substitution table (normative)

| floor symbol | wasm-browser implementation | import? |
|---|---|---|
| `lv_plat_map/unmap` | bump region over `__builtin_wasm_memory_grow`; unmap = no-op (the §15 allocator owns reuse — ARC/arena/free-list are target-independent, dossier §6.4) | no |
| `lv_plat_write(fd,buf,n)` | import `lv.write(fd, ptr, len)` → decode UTF-8, fd1→`console.log` buffer (flush on `\n`), fd2→`console.error` | yes |
| `lv_plat_read` | v1: return 0 (EOF — no stdin in a browser); revisit if a demo needs it | no |
| `lv_plat_now_realtime_ms` | import `lv.now_ms()` → `Date.now()` | yes |
| `lv_plat_now_ns` | import `lv.now_ns()` → `performance.now()*1e6` (as f64, converted) | yes |
| `lv_plat_random(buf,n)` | import `lv.random(ptr, n)` → `crypto.getRandomValues` on a memory view | yes |
| `lv_plat_exit(code)` | import `lv.exit(code)` → host throws a sentinel / marks done; **must not return** — the C side loops after calling it | yes |
| `lv_plat_poll(fds,n,timeout)` | v1 (no async yet): timeout-only sleep is meaningless synchronously → return 0 immediately; doc 04 replaces the *callers'* park path with JSPI, after which poll only ever sees ready work. Keep `LvPollFd`'s 3 bits mapped onto host-promise readiness then. | doc 04 |
| `lv_plat_open/close/stat/mkdir/getdents…` | return the floor's error convention (absent capability); unreachable from user code anyway per the doc-02 gate | no |
| `lv_plat_tcp_*` | absent as raw sockets — same error convention; networking returns reshaped via fetch/WebSocket endpoints (doc 05 §6) | no |
| `lv_plat_term_*`, `signal_*` | absent — error convention | no |

Match `lv_plat.h`'s exact error-return conventions per symbol (read the header comments;
POSIX file is the reference for what callers expect). Never abort inside the floor for an
absent capability — the gate (doc 02 §5) owns loud failure; the floor stays a quiet
error-returner, mirroring how Win32 returns `-1` for POSIX-only holes.

## 3. The JS host (`runtime/lv_host.js`)

Small, dependency-free, and **the only place** import names are spelled:

```js
export async function instantiate(url, opts = {}) {
  const mem = () => inst.exports.memory;
  const imports = { lv: {
    write: (fd, p, n) => sink(fd, dec.decode(new Uint8Array(mem().buffer, p, n))),
    now_ms: () => Date.now(),
    now_ns: () => performance.now() * 1e6,
    random: (p, n) => crypto.getRandomValues(new Uint8Array(mem().buffer, p, n)),
    exit: c => { throw new LvExit(c); },
    // doc 04 adds: park, settle, timer; doc 05 adds the dom.* module
  }};
  const { instance: inst } = await WebAssembly.instantiateStreaming(fetch(url), imports);
  return inst;
}
```

Plus a 15-line `runtime/lv_host_page.html` loader for manual browser runs, and a node/wasmtime
shim so `tests/run_wasm.sh` (doc 02 §7) can drive the same imports headlessly — one imports
file, three hosts.

## 4. Memory & allocator notes

- The §15 allocator (ARC + per-frame arena + free-list) runs unmodified over the region
  `lv_plat_map` returns; growth via `memory.grow` is append-only, which fits the existing
  mmap-more model. No host call on the allocation fast path.
- `WebAssembly.Memory` default max may bite long demos: export the memory from the module
  (default) and set a generous `--max-memory` equivalent in the link flags if the first
  OOM shows up; note the chosen ceiling in the code.
- TLS: single-threaded v1 — the per-thread heap/arena state (`lv_thread.h`) collapses to
  the one instance; nothing to do beyond doc 02 §3's LocalExec pin.

## 5. Verification (W-M1 floor half — joint gate with doc 02)

- `tests/run_wasm.sh` over pure-compute + console + time/random clusters (time/random pins
  assert *shape*, not values, as the existing corpus does for nondeterminism).
- The same corpus green in a real browser once per milestone (manual page, W-M1 gate says
  "wasmtime **and** a browser loader").
- Absent-capability behavior pinned twice: compile-time (doc 02 §7 gated pin) and, for the
  trap-stub tier, one contrived pin that reaches `lvrt_unsupported` at runtime and asserts
  the message + nonzero exit.
