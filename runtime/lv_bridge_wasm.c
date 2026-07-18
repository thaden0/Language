/* Track W — the wasm JS/DOM bridge (W-M3, techdesign-05-dom-bridge.md;
 * hard-06-hostbridge-seam.md).
 *
 * The C half of the bridge doc 05 describes: the emitted-against seam
 * (lvrt_hostcall / lvrt_host_clo_reg / lvrt_hostecho, hard-06) plus the
 * closure-table ROOT (§5), the DOM-event trampoline export (§4), and the
 * marshaler-support exports the JS reflective marshaler (§3) calls back
 * through. Wasm-only — the native builds use lv_runtime.c's raising stubs
 * (DOM/JS is a wasm-*gained* capability). The JS half — the reflective
 * marshaler, the handle table, the DOM op dispatch — lives in lv_host.js;
 * this file only moves bytes and pointers across the `lv.dom_call` import and
 * invokes stored closures through hard-05's lvrt_callclosure.
 *
 * SHADOW-STACK NOTE (lv_task_wasm.c's discipline): lvrt_hostcall is a
 * SYNCHRONOUS call from generated code — it never suspends, so it needs no
 * __stack_pointer save/restore. The one suspension-capable path (a DOM handler
 * that awaits) runs inside lv_dom_dispatch, which routes through
 * lv_wasm_dispatch_run — the same per-activation pooled-stack wrapper timer
 * dispatch uses (rule 1), so an awaiting handler suspends on its OWN stack. A
 * JS marshaler call back INTO wasm (lvrt_str_new to build a string result) is
 * an ordinary nested call at the live sp; no suspension crosses it.
 */
#ifndef __wasm__
#error "lv_bridge_wasm.c is the wasm32 JS/DOM bridge (see lv_runtime.c stubs)"
#endif

#include "lv_abi.h"
#include "lv_task.h"   /* lv_task_fn — the dispatch-wrapper thunk signature */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* runtime-internal seam from lv_task_wasm.c: run one dispatched callback as its
 * own suspendable activation (the timer-dispatch analog — an awaiting DOM
 * handler must suspend on its own pooled stack, rule 1). */
extern void lv_wasm_dispatch_run(lv_task_fn fn, void* a, void* b);

/* host import (module "lv") — the ONE generic sync bridge call. lv_host.js
 * spells "dom_call" and dispatches on the op string; op/a/b cross as LvValue*
 * (LV_STR), h0/h1 as raw i32 handles, out receives the marshaled result. */
__attribute__((import_module("lv"), import_name("dom_call")))
extern void lv_import_dom_call(const LvValue* op, int32_t h0, int32_t h1,
                               const LvValue* a, const LvValue* b, LvValue* out);

/* ============================ op-name compare ============================= */
/* op is an LV_STR: { int64 len; bytes[len]; NUL }. Compare against a C literal. */
static int lv_op_is(const LvValue* op, const char* lit) {
    if (!op || op->tag != LV_STR) return 0;
    const uint8_t* p = (const uint8_t*)(intptr_t)op->payload;
    int64_t len; memcpy(&len, p, sizeof len);
    size_t ll = strlen(lit);
    return (int64_t)ll == len && memcmp(p + 8, lit, ll) == 0;
}

/* ===================== the closure-table ROOT (doc 05 §5) =================
 * Entries retained on register, released on removeEventListener / drop —
 * symmetric, audited in this one place. Free-list reuse; g_clo_live is the
 * leak pin's meter ("cloCount"). */
typedef struct { LvValue clo; int used; } LvCloEnt;
static LvCloEnt* g_clo;
static int g_clo_len, g_clo_cap, g_clo_live;

static int lv_clo_reg(const LvValue* cb) {
    int slot = -1;
    for (int i = 0; i < g_clo_len; i++)
        if (!g_clo[i].used) { slot = i; break; }
    if (slot < 0) {
        if (g_clo_len == g_clo_cap) {
            g_clo_cap = g_clo_cap ? g_clo_cap * 2 : 8;
            g_clo = realloc(g_clo, (size_t)g_clo_cap * sizeof *g_clo);
            if (!g_clo) return -1;
        }
        slot = g_clo_len++;
    }
    g_clo[slot].clo = *cb;
    g_clo[slot].used = 1;
    lvrt_retain(cb);                 /* the root's +1 (doc 05 §5) */
    g_clo_live++;
    return slot;
}
static void lv_clo_release(int idx) {
    if (idx < 0 || idx >= g_clo_len || !g_clo[idx].used) return;
    g_clo[idx].used = 0;
    lvrt_release(&g_clo[idx].clo);   /* symmetric release */
    g_clo_live--;
}

/* ===================== the seam (hard-06 C entries) ======================= */

void lvrt_host_clo_reg(LvValue* out, const LvValue* cb) {
    out->tag = LV_INT;
    out->payload = (cb && cb->tag == LV_CLO) ? lv_clo_reg(cb) : -1;
}

void lvrt_hostcall(LvValue* out, const LvValue* op, const LvValue* h0,
                   const LvValue* h1, const LvValue* a, const LvValue* b) {
    out->tag = LV_VOID; out->payload = 0;
    int32_t h0v = (int32_t)(h0 && h0->tag == LV_INT ? h0->payload : 0);
    int32_t h1v = (int32_t)(h1 && h1->tag == LV_INT ? h1->payload : 0);
    /* closure-table ops handled C-side (the root lives here, §5) */
    if (lv_op_is(op, "cloCount")) {
        out->tag = LV_INT; out->payload = g_clo_live;
        return;
    }
    /* removeEventListener: detach host-side AND release the root entry h1 */
    if (lv_op_is(op, "removeEventListener")) {
        lv_import_dom_call(op, h0v, h1v, a, b, out);
        lv_clo_release(h1v);
        return;
    }
    /* everything else (create/get/set/append/addEventListener/release) is the
     * host's — JS reads op/a/b + the handles and writes the marshaled result */
    lv_import_dom_call(op, h0v, h1v, a, b, out);
}

/* The reflective round-trip probe (doc 05 §8): marshal `v` to JS and return a
 * host-side rendering string. Reuses the dom_call seam with op "echo"; JS runs
 * the full §3 marshaler over the single value pointer passed as `a` (for every
 * other op `a` is an LV_STR; for "echo" it is the value of any tag). The op
 * string is a static LV_STR body { int64 len; "echo" } in the data segment —
 * literal-string shape (no header), which the JS op reader keys off len for. */
static const struct { int64_t len; char s[8]; }
    g_echo_body = { 4, { 'e', 'c', 'h', 'o', 0, 0, 0, 0 } };
void lvrt_hostecho(LvValue* out, const LvValue* v) {
    out->tag = LV_VOID; out->payload = 0;
    LvValue op; op.tag = LV_STR; op.payload = (int64_t)(intptr_t)&g_echo_body;
    lv_import_dom_call(&op, 0, 0, v, (const LvValue*)0, out);
}

/* ===================== the DOM-event trampoline (doc 05 §4) ===============
 * JS installs a real DOM listener that calls lv_dom_dispatch(cloIdx, evt); the
 * stored closure runs as its OWN activation (lv_wasm_dispatch_run) so a handler
 * that awaits a timer suspends independently. The event crosses as a bare i32
 * handle — the Dom prelude wraps it in a DomEvent, so no C-side object
 * construction is needed (doc 05 §4: captures already ride captureHead). */
static void lv_dom_fire(void* a, void* b) {
    int idx = (int)(intptr_t)a;
    int evt = (int)(intptr_t)b;
    if (idx < 0 || idx >= g_clo_len || !g_clo[idx].used) return;
    LvValue arg; arg.tag = LV_INT; arg.payload = evt;
    LvValue ret;
    lvrt_callclosure(&ret, &g_clo[idx].clo, &arg, 1);  /* hard-05 seam */
    lvrt_release(&ret);
    /* a throw inside the handler surfaces via the pending-throw flag; the
     * dispatch wrapper's throw gate (lv_task_wasm.c) owns it, exactly like a
     * timer callback that throws (C2 parity). */
}
__attribute__((export_name("lv_dom_dispatch")))
void lv_dom_dispatch(int32_t cloIdx, int32_t evtHandle) {
    lv_wasm_dispatch_run(lv_dom_fire, (void*)(intptr_t)cloIdx,
                         (void*)(intptr_t)evtHandle);
}

/* ================= marshaler-support exports (doc 05 §3) ==================
 * The JS reflective marshaler reads LvValues straight from linear memory, but
 * ALLOCATION (a fresh runtime string for a string result / a JS->wasm string)
 * and CLASS METADATA must go through the runtime — "never raw memory" (§3).
 * These thin export wrappers surface the existing runtime entry points on the
 * wasm export table (the lv_park_poll host-accessor discipline); they are
 * pulled into the link exactly when a program references lvrt_hostcall (i.e.
 * uses the Dom surface), so non-DOM wasm programs carry none of them. */

/* A fixed bounce buffer: JS writes UTF-8 here (<= 64 KiB) then calls the
 * exported lvrt_str_new to copy it into a fresh runtime string. 64 KiB covers
 * every DOM attribute / text value the v1 corpus builds; larger values are a
 * future grow (logged in doc 05 §3 as-built). */
static _Alignas(8) char g_host_scratch[65536];
__attribute__((export_name("lv_host_scratch")))
void* lv_host_scratch_ptr(void) { return g_host_scratch; }
__attribute__((export_name("lv_host_scratch_size")))
int32_t lv_host_scratch_size(void) { return (int32_t)sizeof g_host_scratch; }

__attribute__((export_name("lvrt_str_new")))
void lv_ex_str_new(LvValue* out, const char* bytes, int64_t len) {
    lvrt_str_new(out, bytes, len);
}
__attribute__((export_name("lvrt_fieldcount")))
int64_t lv_ex_fieldcount(int64_t classId) { return lvrt_fieldcount(classId); }
__attribute__((export_name("lvrt_class_field_name")))
const char* lv_ex_class_field_name(int64_t classId, int64_t i) {
    return lvrt_class_field_name(classId, i);
}
