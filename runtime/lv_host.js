// Track W — the wasm-browser JS host (W-M1 floor, doc 03 §3; W-M2 async,
// doc 04 §3).
//
// Small, dependency-free, and the ONLY place import names are spelled — the
// "lv" namespace lv_plat_wasm.c / lv_task_wasm.c / lv_loop_wasm.c call, and
// the wasi_snapshot_preview1 stubs below it. "One imports file, three hosts"
// (doc 03 §3): this module, lv_host_page.html (manual browser runs), and
// tests/wasm_node_run.mjs (the headless corpus lane) all build their host
// from makeHost().
//
// W-M2 (doc 04): the host OWNS the event loop — the native scheduler's job
// moves here (§1's inversion). Concretely:
//   - lv.park is a WebAssembly.Suspending import: the whole wasm activation
//     parks on a promise this host stores keyed by the task record (§3.1);
//   - timers are host-armed setTimeouts; each fire calls the
//     WebAssembly.promising-wrapped export lv_dispatch(id), its own
//     suspendable activation (§3.2) — same-due fires keep registration
//     order because setTimeout does;
//   - settlement is the turn-end scan (§3.3): after each turn back into JS,
//     poll every parked entry via the exported lv_park_poll; ready ->
//     resolve LV_PARK_SETTLED. O(parked) per turn; the settle-hook import
//     stays the documented, UNBUILT escape hatch;
//   - the loud drain (§3.4): when nothing is armed, queued, or in flight
//     and parked tasks remain, every parked entry resolves LV_PARK_DRAINED
//     — the runtime itself then raises the same "await: event loop drained
//     with promise unresolved" the native flip mode raises;
//   - run() settles when main's activation has settled AND the parked map,
//     timer set, and in-flight work are all empty (§3.4).
//
// JSPI availability (doc 04 §5): feature-detected. Without it, pure-compute
// programs still run (plain function imports, synchronous main); any async
// path fails loud with the message below naming the floor. Hosts of record:
// Chrome >= 137 (JSPI on by default) and Node >= 24 with
// --experimental-wasm-jspi (recorded in tests/run_wasm.sh).
//
// Why wasi_snapshot_preview1 is here at all, on a target that is explicitly
// NOT WASI: the runtime archive links wasi-libc for the C-level pieces
// lv_runtime.c always needed (malloc/mem*/str*/snprintf); musl's vsnprintf
// shares its implementation with real file-descriptor I/O, so linking
// snprintf pulls in fd_write/fd_close/fd_seek even though nothing here ever
// calls them. They return "bad file descriptor" if a future libc bump ever
// changes that.
const EBADF = 8;

const kNoJspi =
  'async on wasm needs WebAssembly JSPI (WebAssembly.Suspending): ' +
  'use Chrome >= 137, or Node >= 24 with --experimental-wasm-jspi';

// lv_task.h wake codes (must match runtime/lv_task.h)
const LV_PARK_SETTLED = 1;
const LV_PARK_DRAINED = 2;

export class LvExit extends Error {
  constructor(code) {
    super(`lv exit ${code}`);
    this.code = code;
  }
}

function defaultSink(fd, text) {
  (fd === 2 ? console.error : console.log)(text.replace(/\n$/, ''));
}

// A DOM backend over the ambient `document` (browsers). The headless corpus
// driver (tests/wasm_node_run.mjs) passes its own in-memory stub instead — the
// bridge code below is backend-agnostic (doc 05 §2: Leviathan never touches the
// opaque JS value, only the glue does).
export function browserDom() {
  return {
    body: () => document.body,
    createElement: (t) => document.createElement(t),
    createTextNode: (s) => document.createTextNode(s),
    getElementById: (id) => document.getElementById(id),
    setAttribute: (el, n, v) => el.setAttribute(n, v),
    getAttribute: (el, n) => el.getAttribute(n),
    setText: (el, t) => { el.textContent = t; },
    getText: (el) => el.textContent,
    appendChild: (p, c) => p.appendChild(c),
    addEventListener: (el, type, fn) => el.addEventListener(type, fn),
    removeEventListener: (el, type, fn) => el.removeEventListener(type, fn),
    dispatchEvent: (el, type) => el.dispatchEvent(new Event(type)),
    eventType: (e) => e.type,
    eventTargetValue: (e) => (e && e.target && 'value' in e.target) ? e.target.value : '',
  };
}

// The host: imports + the run lifecycle around one instance.
// `getInstance` is a thunk (the instance exists only after instantiation);
// `sink(fd, text)` receives fd1/fd2 output (default console.log/error);
// `dom` is the DOM backend (default: the ambient `document` if present — a
// browser; else undefined, and DOM ops throw loud when reached, doc 05 §2).
export function makeHost(getInstance, sink = defaultSink,
                         dom = (typeof document !== 'undefined' ? browserDom() : undefined)) {
  const dec = new TextDecoder();
  const enc = new TextEncoder();
  const mem = () => getInstance().exports.memory;
  const jspi = typeof WebAssembly.Suspending === 'function';

  // --- async state (doc 04 §3) ---------------------------------------------
  const parked = new Map();      // task record ptr -> park resolver (park order)
  const timers = new Map();      // timer id (BigInt) -> setTimeout handle
  const stepWaiters = [];        // lv.step resolvers (lv_main's tail drain)
  let spawnsQueued = 0;          // lv.spawn microtasks not yet entered
  let pendingOps = 0;            // in-flight non-park suspensions (probe fetch)
  let active = 0;                // activations whose promises haven't settled
  let mainSettled = false;
  let exitCode = 0;
  let finished = false;
  let scanScheduled = false;
  let resolveExit, rejectExit;
  const exitPromise = new Promise((res, rej) => { resolveExit = res; rejectExit = rej; });

  const promisified = new Map(); // export fn -> promising-wrapped fn
  function promising(fn) {
    if (!jspi) return fn;        // degrade: sync call (pure-compute programs)
    let p = promisified.get(fn);
    if (!p) { p = WebAssembly.promising(fn); promisified.set(fn, p); }
    return p;
  }
  const suspending = (fn) =>
    jspi ? new WebAssembly.Suspending(fn)
         : () => { throw new Error(kNoJspi); };

  // Enter wasm through one export: its own activation. Tracks settlement,
  // routes the LvExit sentinel (env.exit / lvrt_unsupported / the throw
  // gate) to finishNow, and schedules the turn-end scan.
  function enter(name, ...args) {
    const fn = promising(getInstance().exports[name]);
    active++;
    let p;
    try { p = Promise.resolve(fn(...args)); } catch (e) { p = Promise.reject(e); }
    return p.then(
      (v) => { active--; schedule(); return v; },
      (e) => {
        active--;
        if (e instanceof LvExit) { finishNow(e.code); return e.code; }
        fail(e);
        throw e;
      });
  }

  function schedule() {
    if (scanScheduled || finished) return;
    scanScheduled = true;
    queueMicrotask(scan);
  }

  // §3.3 turn-end scan + §3.4 lifetime. Runs only when no wasm frame is
  // executing (microtasks run between turns), so lv_park_poll is safe.
  function scan() {
    scanScheduled = false;
    if (finished) return;
    const inst = getInstance();
    if (!inst) return;
    let woke = 0;
    for (const [rec, resolve] of parked) {
      if (inst.exports.lv_park_poll(rec)) {
        parked.delete(rec);
        resolve(LV_PARK_SETTLED);      // resumes in park order — C1 parity
        woke++;
      }
    }
    if (woke) { schedule(); return; }  // resumed work re-scans on settle
    const quiet = timers.size === 0 && spawnsQueued === 0 && pendingOps === 0;
    if (!quiet) return;
    if (stepWaiters.length) {          // tail drain waiting with nothing armed:
      for (const r of stepWaiters.splice(0)) r(0);   // let it re-check has_work
      return;
    }
    if (parked.size) {
      // §3.4 loud drain: nothing can ever settle these. Wake every parked
      // task DRAINED, in park order; the runtime raises the native flip
      // mode's own message at each await (C3 — loud, local, catchable).
      const entries = [...parked];
      parked.clear();
      for (const [, resolve] of entries) resolve(LV_PARK_DRAINED);
      return;
    }
    maybeFinish();
  }

  function maybeFinish() {
    if (!finished && mainSettled && active === 0 && parked.size === 0 &&
        timers.size === 0 && spawnsQueued === 0 && pendingOps === 0 &&
        stepWaiters.length === 0) {
      finished = true;
      resolveExit(exitCode);
    }
  }

  function finishNow(code) {
    if (finished) return;
    finished = true;
    for (const h of timers.values()) clearTimeout(h);
    timers.clear();
    resolveExit(code);
  }

  function fail(e) {
    if (finished) return;
    finished = true;
    for (const h of timers.values()) clearTimeout(h);
    timers.clear();
    rejectExit(e);
  }

  // --- W-M3 JS/DOM bridge (doc 05 §2-§6) -----------------------------------
  // The handle table (§2): opaque JS values held by integer index; index 0 is
  // the reserved null/document sentinel (a getElementById miss maps here). Free
  // list reuse on release; the wrappers keep the ABI plain-i64 and debuggable
  // (externref is a later zero-surface-change optimization).
  const heap = [null];
  const heapFree = [];
  function halloc(obj) {
    if (heapFree.length) { const i = heapFree.pop(); heap[i] = obj; return i; }
    heap.push(obj); return heap.length - 1;
  }
  function hfree(i) {
    if (i > 0 && i < heap.length && heap[i] !== null) { heap[i] = null; heapFree.push(i); }
  }

  const exports = () => getInstance().exports;
  const dvNew = () => new DataView(mem().buffer);

  // Read a NUL-terminated C string (an interned slot name) from linear memory.
  function readCStr(ptr) {
    if (!ptr) return null;
    const buf = new Uint8Array(mem().buffer);
    let end = ptr; while (buf[end] !== 0) end++;
    return dec.decode(buf.subarray(ptr, end));
  }
  // Read an LV_STR body { i64 len; bytes[len] } — key off len, NEVER a NUL,
  // and NEVER touch payload-16 (the marshaler footgun rule, doc 05 §3).
  function readStrAt(payload) {
    if (!payload) return '';
    const d = dvNew();
    const len = Number(d.getBigInt64(payload, true));
    return dec.decode(new Uint8Array(mem().buffer, payload + 8, len));
  }
  // Read an LV_STR value pointed at by an LvValue* (op/a/b args).
  function readStr(ptr) {
    if (!ptr) return '';
    const d = dvNew();
    const tag = Number(d.getBigInt64(ptr, true));
    if (tag !== 4) return '';
    return readStrAt(Number(d.getBigInt64(ptr + 8, true)));
  }

  // The reflective marshaler (§3, normative): one routine, switch on tag +
  // shape. Reads a value at an LvValue* and returns a JS value. Header-free by
  // construction — it never reads rc/meta, only tag/payload and the payload's
  // own body. Boxed arrays only in v1 (dense/columnar TypedArray views deferred,
  // §3 / dossier risk #7).
  function marshal(ptr) {
    const d = dvNew();
    const tag = Number(d.getBigInt64(ptr, true));
    switch (tag) {
      case 0:  return undefined;                                   // VOID
      case 8:  return null;                                        // NONE
      case 1:  return Number(d.getBigInt64(ptr + 8, true));        // INT
      case 3:  return d.getBigInt64(ptr + 8, true) !== 0n;         // BOOL
      case 10: return String.fromCodePoint(Number(d.getBigInt64(ptr + 8, true))); // CHAR
      case 2:  return d.getFloat64(ptr + 8, true);                 // FLOAT (payload bits as f64)
      case 4:  return readStrAt(Number(d.getBigInt64(ptr + 8, true)));   // STR
      case 5:  return marshalObj(Number(d.getBigInt64(ptr + 8, true)));  // OBJ
      case 6:  return marshalArr(Number(d.getBigInt64(ptr + 8, true)));  // ARR
      case 7:  return marshalMap(Number(d.getBigInt64(ptr + 8, true)));  // MAP
      case 9:  return { __closure: true };                         // CLO
      case 11: return { __block: true };                           // BLOCK
      default: return { __tag: tag };
    }
  }
  function fieldName(classId, i) {
    return readCStr(exports().lvrt_class_field_name(classId, BigInt(i)) >>> 0);
  }
  function className(classId) {
    return readCStr(exports().lvrt_class_name(classId) >>> 0);
  }
  function marshalObj(payload) {
    const d = dvNew();
    const classId = d.getBigInt64(payload, true);          // object: {classId, dyn, slots...}
    const nslots = Number(exports().lvrt_fieldcount(classId));
    // handle-wrapper short-circuit (§3): the bridge's own handle wrappers
    // (DomNode/DomEvent) ARE their JS value — identify them by class NAME, not
    // by an ambiguous "one int slot" shape (a user class `{int h;}` shares that
    // shape but is a plain object). Their sole slot is the int handle.
    const cls = className(classId);
    if (cls === 'DomNode' || cls === 'DomEvent') {
      const handle = Number(d.getBigInt64(payload + 16 + 8, true));  // slot 0 payload
      return heap[handle];
    }
    const o = {};
    for (let i = 0; i < nslots; i++) o[fieldName(classId, i)] = marshal(payload + 16 + 16 * i);
    return o;
  }
  function marshalArr(payload) {
    const d = dvNew();
    const word0 = d.getBigInt64(payload, true);            // boxed: {len; elems[]}
    if ((word0 & (3n << 62n)) !== 0n) return { __dense: true };   // v1: boxed only
    const len = Number(word0);
    const arr = [];
    for (let i = 0; i < len; i++) arr.push(marshal(payload + 8 + 16 * i));
    return arr;
  }
  function marshalMap(payload) {
    const d = dvNew();
    const len = Number(d.getBigInt64(payload, true));      // {len; LvPair entries[]}
    const o = {};
    for (let i = 0; i < len; i++) {
      const kp = payload + 8 + 32 * i;
      o[String(marshal(kp))] = marshal(kp + 16);
    }
    return o;
  }
  // Deterministic host-side rendering for the §8 round-trip pins.
  function render(v) {
    if (v === undefined) return 'void';
    if (v === null) return 'none';
    if (typeof v === 'boolean') return v ? 'true' : 'false';
    if (typeof v === 'number')
      return Number.isInteger(v) ? String(v) : v.toFixed(6);
    if (typeof v === 'string') return JSON.stringify(v);
    if (Array.isArray(v)) return '[' + v.map(render).join(', ') + ']';
    if (v && v.__closure) return '<closure>';
    if (v && v.__block) return '<block>';
    if (v && v.__dense) return '<dense-array>';
    if (v && v.__nodeTag) return '<' + v.__nodeTag + '>';       // stub DOM node
    if (typeof v === 'object')
      return '{' + Object.keys(v).map((k) => k + ': ' + render(v[k])).join(', ') + '}';
    return String(v);
  }

  // Write a result LvValue* (JS -> wasm; §3: strings through an exported
  // runtime constructor, never raw memory).
  function writeInt(outPtr, n) {
    const d = dvNew();
    d.setBigInt64(outPtr, 1n, true);
    d.setBigInt64(outPtr + 8, BigInt(n), true);
  }
  function writeVoid(outPtr) {
    const d = dvNew();
    d.setBigInt64(outPtr, 0n, true);
    d.setBigInt64(outPtr + 8, 0n, true);
  }
  function writeStr(outPtr, s) {
    const bytes = enc.encode(s == null ? '' : String(s));
    const scratch = exports().lv_host_scratch() >>> 0;
    const cap = exports().lv_host_scratch_size();
    if (bytes.length > cap)
      throw new Error(`host bridge: string result too large (${bytes.length} > ${cap} bytes)`);
    new Uint8Array(mem().buffer, scratch, bytes.length).set(bytes);
    exports().lvrt_str_new(outPtr, scratch, BigInt(bytes.length));   // fresh rc-0; codegen retains
  }

  // Installed DOM listeners, keyed for removeEventListener symmetry (§5).
  const listeners = new Map();
  const lkey = (nodeH, type, idx) => `${nodeH} ${type} ${idx}`;
  function installListener(nodeH, type, cloIdx) {
    const el = heap[nodeH];
    const fn = (evt) => {
      // Each event enters the trampoline as its OWN promising activation so a
      // handler may await (doc 05 §4). The event is a transient handle, freed
      // once the activation settles.
      const eh = halloc(evt);
      enter('lv_dom_dispatch', cloIdx, eh).then(() => hfree(eh), () => hfree(eh));
    };
    dom.addEventListener(el, type, fn);
    listeners.set(lkey(nodeH, type, cloIdx), fn);
  }
  function removeListener(nodeH, type, cloIdx) {
    const k = lkey(nodeH, type, cloIdx);
    const fn = listeners.get(k);
    if (fn) { dom.removeEventListener(heap[nodeH], type, fn); listeners.delete(k); }
  }

  // The one host import the bridge crosses (hard-06). op selects the operation;
  // JS reflects on the LvValue args and writes the marshaled result to `out`.
  function domCall(opPtr, h0, h1, aPtr, bPtr, outPtr) {
    const op = readStr(opPtr);
    if (op === 'echo') { writeStr(outPtr, render(marshal(aPtr))); return; }
    if (!dom) throw new Error(`host bridge: no DOM backend for op '${op}'`);
    const a = aPtr ? readStr(aPtr) : '';
    const b = bPtr ? readStr(bPtr) : '';
    switch (op) {
      case 'documentBody':    writeInt(outPtr, halloc(dom.body())); break;
      case 'createElement':   writeInt(outPtr, halloc(dom.createElement(a))); break;
      case 'createTextNode':  writeInt(outPtr, halloc(dom.createTextNode(a))); break;
      case 'getElementById': {
        const el = dom.getElementById(a);
        writeInt(outPtr, el ? halloc(el) : 0);
        break;
      }
      case 'setAttribute':    dom.setAttribute(heap[h0], a, b); writeVoid(outPtr); break;
      case 'getAttribute':    writeStr(outPtr, dom.getAttribute(heap[h0], a) ?? ''); break;
      case 'setText':         dom.setText(heap[h0], a); writeVoid(outPtr); break;
      case 'getText':         writeStr(outPtr, dom.getText(heap[h0]) ?? ''); break;
      case 'appendChild':     dom.appendChild(heap[h0], heap[h1]); writeVoid(outPtr); break;
      case 'addEventListener':    installListener(h0, a, h1); writeVoid(outPtr); break;
      case 'removeEventListener': removeListener(h0, a, h1); writeVoid(outPtr); break;
      case 'dispatchEvent': {
        // Self-dispatch (Leviathan calling dispatchEvent) is DEFERRED to its own
        // turn, not fired re-entrantly: firing synchronously would run the
        // handler while the caller's activation is still on the stack, and a
        // handler that COMPLETES there (returns or throws) would leak its
        // pending-throw flag / stack state into the caller (the native model
        // runs every dispatched callback in its OWN turn, doc 04 §3.2). A real
        // user event already fires in its own turn from the browser loop and
        // never reaches this op. pendingOps holds the loop open until it fires.
        const el = heap[h0], ty = a;
        pendingOps++;
        setTimeout(() => {
          try { dom.dispatchEvent(el, ty); }
          finally { pendingOps--; schedule(); }
        }, 0);
        writeVoid(outPtr);
        break;
      }
      case 'eventType':       writeStr(outPtr, dom.eventType(heap[h0])); break;
      case 'eventTargetValue':writeStr(outPtr, dom.eventTargetValue(heap[h0])); break;
      case 'release':         hfree(h0); writeVoid(outPtr); break;
      default: throw new Error(`host bridge: unknown DOM op '${op}'`);
    }
  }

  const imports = {
    lv: {
      // --- W-M1 floor (doc 03 §2) ---
      write: (fd, ptr, len) => {
        const bytes = new Uint8Array(mem().buffer, ptr, len);
        sink(fd, dec.decode(bytes));
        return len;
      },
      now_ms: () => Date.now(),
      now_ns: () => performance.now() * 1e6,
      random: (ptr, len) => {
        crypto.getRandomValues(new Uint8Array(mem().buffer, ptr, len));
      },
      exit: (code) => { throw new LvExit(code); },

      // --- W-M2 async (doc 04 §3) ---
      // park: the awaiting activation suspends until the scan (SETTLED), a
      // cancel (lv.wake), or the drain (DRAINED) resolves it. The scheduled
      // scan runs after this activation has fully suspended (microtasks run
      // between turns) — it is what detects an immediate drain when nothing
      // is armed at all (C3 with an empty loop).
      park: suspending((rec) => {
        schedule();
        return new Promise((resolve) => parked.set(rec, resolve));
      }),
      // wake: cancel delivery (B2) — resolve a parked task NOW with `code`.
      wake: (rec, code) => {
        const resolve = parked.get(rec);
        if (resolve) { parked.delete(rec); resolve(code); }
        schedule();
      },
      // spawn: queue one activation per task, FIFO, ahead of any timer fire
      // (microtasks outrank macrotasks — the runq-before-loop-step order).
      spawn: (rec) => {
        spawnsQueued++;
        queueMicrotask(() => {
          spawnsQueued--;
          if (!finished) enter('lv_task_run', rec);
        });
      },
      // yield: FIFO fairness — resume after already-queued microtasks.
      yield: suspending(() => new Promise((r) => queueMicrotask(() => r(0)))),
      // step: "one loop batch" for lv_main's emitted tail drain — suspend
      // until the next dispatch completes; nothing armed => return at once.
      step: suspending(() => {
        if (timers.size === 0) return 0;
        return new Promise((r) => stepWaiters.push(r));
      }),
      // timers (§3.2): host-armed; fire enters lv_dispatch(id) as its own
      // activation. id is i64 -> BigInt end to end.
      timer_start: (id, delayMs) => {
        const handle = setTimeout(() => {
          timers.delete(id);
          enter('lv_dispatch', id).then(() => {
            for (const r of stepWaiters.splice(0)) r(1);   // batch completed
            schedule();
          }, () => { for (const r of stepWaiters.splice(0)) r(1); });
        }, Number(delayMs));
        timers.set(id, handle);
      },
      timer_cancel: (id) => {
        const handle = timers.get(id);
        if (handle !== undefined) {
          clearTimeout(handle);
          timers.delete(id);
        }
        schedule();                    // may have gone quiet (drain / finish)
      },
      // doc 04 §6's composition probe: a GENUINE host promise chain (fetch of
      // a data: URI) behind a Suspending import. Driven only by the harness
      // (lv_probe_fetch export); a real fetch bridge is doc 05's.
      probe_fetch: suspending(async () => {
        pendingOps++;
        try {
          const resp = await fetch('data:text/plain,42');
          return parseInt(await resp.text(), 10) | 0;
        } finally {
          pendingOps--;
          schedule();
        }
      }),

      // --- W-M3 JS/DOM bridge (doc 05 §2-§6) ---
      // The one generic sync host call; op selects the operation. Synchronous
      // (never suspends) — a DOM handler that awaits does so inside the
      // lv_dom_dispatch trampoline, its own promising activation.
      dom_call: domCall,
    },
    // dead imports — see the file header.
    wasi_snapshot_preview1: {
      fd_write: () => EBADF,
      fd_close: () => EBADF,
      fd_seek: () => EBADF,
    },
  };

  // Program lifetime (§3.4): enter main as a promising export; settle when
  // main is done AND the loop is empty. env.exit / trap-stub / throw-gate
  // exits arrive as the LvExit sentinel from ANY activation and settle
  // immediately (native parity: the process exits now).
  function run() {
    enter('main', 0, 0).then(
      (code) => {
        mainSettled = true;
        exitCode = Number(code ?? 0) | 0;
        schedule();
      },
      () => {});                       // enter() already routed the failure
    return exitPromise;
  }

  return { imports, run };
}

// Browser entry point: fetch + stream-compile + instantiate. Returns
// { instance, host } — pass to run() below.
export async function instantiate(url, opts = {}) {
  let inst;
  const host = makeHost(() => inst, opts.sink || defaultSink);
  const { instance } = await WebAssembly.instantiateStreaming(fetch(url), host.imports);
  inst = instance;
  return { instance, host };
}

// Drive the program to completion; resolves with the exit code (§3.4).
export function run(handle) {
  return handle.host.run();
}
