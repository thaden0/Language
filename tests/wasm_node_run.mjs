// Track W — the headless wasm corpus driver (W-M1, doc 02 §7 / doc 03 §3;
// async since W-M2, doc 04).
//
// Plain `wasmtime run --invoke main` cannot satisfy this target's imports:
// wasmtime's CLI only auto-provides WASI, and the floor's real surface is
// the "lv" module (doc 03 §1). Node's native V8 WebAssembly implementation
// plus runtime/lv_host.js's shared makeHost() is the headless host doc 03 §3
// calls for — "one imports file, three hosts" (this driver,
// lv_host_page.html, and a real browser all build from the same function).
//
// W-M2: the host owns the event loop (doc 04 §1) — run() resolves when
// main's activation settles AND the parked map / timer set are empty
// (§3.4). Async programs need JSPI: run under
// `node --experimental-wasm-jspi` (Node >= 24; see tests/run_wasm.sh).
// Pure-compute programs still pass without it.
//
// usage: node [--experimental-wasm-jspi] wasm_node_run.mjs <file.wasm>
// Writes fd1/fd2 verbatim to this process's stdout/stderr and exits with the
// program's exit code.
//
// LV_PROBE_FETCH=1 additionally drives doc 04 §6's composition probe: while
// the program's own activations park on timers, one extra activation
// (lv_probe_fetch) suspends on a GENUINE fetch of a data: URI, then again
// through the yield lane, and must come back with 42.
import { readFileSync } from 'node:fs';
import { makeHost } from '../runtime/lv_host.js';

const path = process.argv[2];
if (!path) {
  console.error('usage: node wasm_node_run.mjs <file.wasm>');
  process.exit(2);
}

// A minimal in-memory DOM (W-M3, doc 05 §8): the headless stand-in for a real
// browser `document`, so tests/run_wasm_dom.sh drives the DOM bridge without a
// headless browser on the box. Backend-shaped exactly like lv_host.js's
// browserDom() — the bridge code is backend-agnostic. Inert for non-DOM
// programs (dom_call is never invoked). dispatchEvent runs listeners
// synchronously, exactly like the platform (doc 05 §4's self-dispatch path).
function nodeDom() {
  const byId = new Map();
  const mkNode = (tag) => ({ __nodeTag: tag, attrs: {}, text: '', children: [], listeners: {} });
  return {
    body: () => (nodeDom.body ||= mkNode('body')),
    createElement: (t) => mkNode(t),
    createTextNode: (s) => { const n = mkNode('#text'); n.text = s; return n; },
    getElementById: (id) => byId.get(id) || null,
    setAttribute: (el, n, v) => { el.attrs[n] = v; if (n === 'id') byId.set(v, el); },
    getAttribute: (el, n) => (n in el.attrs ? el.attrs[n] : null),
    setText: (el, t) => { el.text = t; el.children = []; },
    getText: (el) => el.text,
    appendChild: (p, c) => { p.children.push(c); },
    addEventListener: (el, type, fn) => { (el.listeners[type] ||= []).push(fn); },
    removeEventListener: (el, type, fn) => {
      const a = el.listeners[type]; if (!a) return;
      const i = a.indexOf(fn); if (i >= 0) a.splice(i, 1);
    },
    dispatchEvent: (el, type) => {
      const a = el.listeners[type]; if (!a) return;
      const evt = { type, target: el };
      for (const fn of a.slice()) fn(evt);
    },
    eventType: (e) => e.type,
    eventTargetValue: (e) => (e && e.target && 'value' in e.target ? e.target.value : ''),
  };
}

const bytes = readFileSync(path);
let inst;
const sink = (fd, text) => (fd === 2 ? process.stderr : process.stdout).write(text);
const host = makeHost(() => inst, sink, nodeDom());
const { instance } = await WebAssembly.instantiate(bytes, host.imports);
inst = instance;

const wantProbe = process.env.LV_PROBE_FETCH === '1';
if (wantProbe && typeof WebAssembly.promising !== 'function') {
  console.error('probe_fetch: JSPI unavailable (need node --experimental-wasm-jspi)');
  process.exit(70);
}

// run() enters main synchronously up to its first suspension — main MUST be
// the first export entered (lv_task_wasm.c rule 4: task 0 owns the default
// shadow stack, laid down while __stack_pointer is still pristine). The probe
// starts after that, entering like any other activation.
const runPromise = host.run();
const probe = wantProbe
  ? WebAssembly.promising(instance.exports.lv_probe_fetch)()
  : null;

const code = await runPromise;
if (probe) {
  const got = await probe;
  if (got !== 42) {
    console.error(`probe_fetch: want 42, got ${got}`);
    process.exit(70);
  }
}
process.exit(code);
