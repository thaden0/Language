/* Track W — the wasm leg of the loop registry (W-M2, doc 04 §2/§3.2).
 *
 * Replaces lv_loop.c IN THE WASM ARCHIVE ONLY (build-triple.sh's wasm32
 * source list) — the shared loop is never edited for wasm-only behavior, and
 * it cannot serve this target: its registries are file-static, its step
 * blocks in lv_plat_poll (meaningless on wasm's single thread, doc 03 §2),
 * and its due-time bookkeeping presumes the runtime owns the clock. Here the
 * HOST owns the clock and the loop (doc 04 §1's inversion):
 *
 *   - registering a timer imports lv.timer_start(id, delayMs); the host
 *     setTimeouts and, on fire, calls the promising-wrapped export
 *     lv_dispatch(id) below — each dispatch is its own suspendable
 *     activation (lv_task_wasm.c's lv_wasm_dispatch_run), so a callback
 *     that awaits suspends independently of every other (§3.2);
 *   - dispatch order for simultaneously-due work preserves registration
 *     order because the host fires strictly through its own event loop,
 *     which orders same-due setTimeouts by creation (§3.2; pinned by
 *     tests/corpus/wasm/order_timers.lev);
 *   - lvrt_loop_step — whose one live caller under tasks is lv_main's
 *     emitted tail drain (LlvmGen.cpp:693-716, pumped from inside task 0)
 *     — becomes "suspend until the host completes one dispatch"
 *     (lv_task_loop_step_wait), keeping the drain's shape and its
 *     throw-gate semantics with zero codegen edits (§4);
 *   - repeating timers re-arm from C after each fire (fixed-delay via the
 *     host's setTimeout vs native's fixed-rate dueNs += interval — the
 *     corpus pins tick shape/order, not wall-clock phase).
 *
 * Everything else lv_loop.c owns — the sysTask* natives over the substrate,
 * the socket natives over the floor — is carried here verbatim so the
 * archive keeps one definition per symbol; closure invocation goes through
 * lvrt_callclosure (hard-05's seam, whose contract note says "only the wasm
 * glue calls it in v1" — this file is that glue).
 *
 * FD watches register and cancel (id space shared with timers, as native)
 * but never arm anything on the host and never count as loop work: every fd
 * source is compile-time gated on this target (hard-03), so a live watch is
 * unreachable from user code; letting one hold the drain open would hang
 * every program for a capability that cannot exist here.
 */
#ifndef __wasm__
#error "lv_loop_wasm.c is the wasm32 leg of the loop registry (see lv_loop.c)"
#endif

#include "lv_abi.h"
#include "lv_plat.h"
#include "lv_task.h"
#include "lv_tls.h"     /* lv_tls_is/send/recv — the none provider on wasm */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* runtime-internal seams from lv_task_wasm.c (the lv_rt_dispatch_fn idiom:
 * cross-file externs between the runtime's own members, never lv_abi.h) */
extern void lv_wasm_dispatch_run(lv_task_fn fn, void* a, void* b);
extern int  lv_task_loop_step_wait(void);

/* host imports — timer arming (doc 04 §3.2) */
__attribute__((import_module("lv"), import_name("timer_start")))
extern void lv_import_timer_start(int64_t id, int64_t delayMs);
__attribute__((import_module("lv"), import_name("timer_cancel")))
extern void lv_import_timer_cancel(int64_t id);

typedef struct WTimer {
    int64_t id;
    int64_t intervalMs;   /* 0 = one-shot */
    int64_t ticks;
    LvValue cb;
    int     active;
} WTimer;

typedef struct WWatch {
    int64_t id;
    int     fd;
    int     write;
    LvValue cb;
    int     active;
} WWatch;

/* single-threaded target (doc 02 §3): plain statics, no LV_TLS needed */
static WTimer*  g_timers;
static int64_t  g_timerCount, g_timerCap;
static WWatch*  g_watches;
static int64_t  g_watchCount, g_watchCap;
static int64_t  g_nextId = 1;

/* invoke `clo(argVal)` / `clo()` through hard-05's C-callable seam; releases
 * whatever the callback returns (lv_loop.c lv_invoke1's discipline). */
static void lv_invoke_cb(const LvValue* clo, const LvValue* args, int nargs) {
    LvValue ret;
    lvrt_callclosure(&ret, clo, args, nargs);
    lvrt_release(&ret);
}

/* ================================ timers ================================= */

static int64_t lv_timer_find(int64_t id) {
    for (int64_t i = 0; i < g_timerCount; i++)
        if (g_timers[i].active && g_timers[i].id == id) return i;
    return -1;
}

void lvrt_systimerstart(LvValue* out, const LvValue* delayMs,
                        const LvValue* intervalMs, const LvValue* cb) {
    if (g_timerCount == g_timerCap) {
        g_timerCap = g_timerCap ? g_timerCap * 2 : 8;
        g_timers = realloc(g_timers, (size_t)g_timerCap * sizeof *g_timers);
    }
    WTimer* t = &g_timers[g_timerCount++];
    t->id = g_nextId++;
    t->intervalMs = intervalMs->payload;
    t->ticks = 0;
    t->cb = *cb;
    t->active = 1;
    lvrt_retain(cb);   /* registry owns the callback until cancel/one-shot done */
    lv_import_timer_start(t->id, delayMs->payload);
    out->tag = LV_INT;
    out->payload = t->id;
}

void lvrt_systimercancel(const LvValue* id) {
    int64_t i = lv_timer_find(id->payload);
    if (i < 0) return;
    g_timers[i].active = 0;
    lvrt_release(&g_timers[i].cb);
    lv_import_timer_cancel(id->payload);
}

/* One timer fire, run inside its own activation (lv_dispatch below). The id
 * crosses lv_task_fn's two void* args as a split int64 — the thunk signature
 * has no room for a heap ctx without allocating at an arbitrary shadow-stack
 * position, which the wrapper discipline forbids. */
static void lv_fire_timer(void* a, void* b) {
    int64_t id = (int64_t)(uint32_t)(intptr_t)a
               | ((int64_t)(int32_t)(intptr_t)b << 32);
    int64_t i = lv_timer_find(id);
    if (i < 0) return;                 /* cancelled between fire and dispatch */
    WTimer* t = &g_timers[i];
    t->ticks++;
    LvValue tick; tick.tag = LV_INT; tick.payload = t->ticks;
    LvValue cb = t->cb;                /* snapshot: the callback may realloc
                                          g_timers (add) or cancel this id   */
    lvrt_retain(&cb);                  /* across the invoke: a self-cancel
                                          releases the registry's ref while
                                          the body still runs (lv_loop.c's
                                          watch-path lesson)                 */
    int64_t interval = t->intervalMs;
    if (interval <= 0) {
        t->active = 0;                 /* pre-invoke: re-entrant cancel of this
                                          id must be a harmless no-op        */
        lv_invoke_cb(&cb, &tick, 1);
        lvrt_release(&cb);             /* the registry's ref (deactivated)    */
    } else {
        lv_invoke_cb(&cb, &tick, 1);
        int64_t j = lv_timer_find(id); /* re-find: invoke may have moved/
                                          cancelled the slot                 */
        if (j >= 0)
            lv_import_timer_start(id, g_timers[j].intervalMs);   /* re-arm */
    }
    lvrt_release(&cb);                 /* our invoke-spanning ref */
}

__attribute__((export_name("lv_dispatch")))
void lv_dispatch(int64_t id) {
    lv_wasm_dispatch_run(lv_fire_timer,
                         (void*)(intptr_t)(int32_t)(id & 0xffffffff),
                         (void*)(intptr_t)(int32_t)(id >> 32));
}

/* =============================== fd watches ===============================
 * Registry-only on wasm (see the file header): same id space, same
 * retain/release ownership, no host arming, excluded from has_work. */

static int64_t lv_watch_add(int fd, const LvValue* cb, int isWrite) {
    int64_t slot = -1;                       /* reuse a retired slot (lv_loop.c) */
    for (int64_t i = 0; i < g_watchCount; i++)
        if (!g_watches[i].active) { slot = i; break; }
    if (slot < 0) {
        if (g_watchCount == g_watchCap) {
            g_watchCap = g_watchCap ? g_watchCap * 2 : 8;
            g_watches = realloc(g_watches, (size_t)g_watchCap * sizeof *g_watches);
        }
        slot = g_watchCount++;
    }
    WWatch* w = &g_watches[slot];
    w->id = g_nextId++;
    w->fd = fd;
    w->write = isWrite;
    w->cb = *cb;
    w->active = 1;
    lvrt_retain(cb);
    return w->id;
}

void lvrt_syswatch(LvValue* out, const LvValue* fd, const LvValue* cb) {
    out->tag = LV_INT;
    out->payload = lv_watch_add((int)fd->payload, cb, 0);
}

void lvrt_syswatchwrite(LvValue* out, const LvValue* fd, const LvValue* cb) {
    out->tag = LV_INT;
    out->payload = lv_watch_add((int)fd->payload, cb, 1);
}

void lvrt_sysunwatch(const LvValue* id) {
    for (int64_t i = 0; i < g_watchCount; i++)
        if (g_watches[i].active && g_watches[i].id == id->payload) {
            g_watches[i].active = 0;
            lvrt_release(&g_watches[i].cb);
            return;
        }
}

void lvrt_loop_cancel_fd(int fd) {
    for (int64_t i = 0; i < g_watchCount; i++)
        if (g_watches[i].active && g_watches[i].fd == fd) {
            g_watches[i].active = 0;
            lvrt_release(&g_watches[i].cb);
        }
}

/* ========================= the loop entry points ========================== */

int32_t lvrt_loop_has_work(void) {
    /* active timers only — watches can never fire here (file header) */
    for (int64_t i = 0; i < g_timerCount; i++)
        if (g_timers[i].active) return 1;
    return 0;
}

void lvrt_loop_step(void) {
    /* The host owns firing (§3.2); "one batch" = wait until the host has run
     * one dispatch activation to completion (or has nothing armed). The one
     * live caller under tasks is lv_main's emitted tail drain; the parked
     * paths never come here (they suspend inside lv_task_park_on). */
    if (!lvrt_loop_has_work()) return;
    (void)lv_task_loop_step_wait();
}

int lvrt_loop_step_tasks(void) {
    /* The native scheduler's hook. No carrier loop exists on this target, so
     * it is never called; defined for the lv_sched_hooks installation in
     * lv_entry.c and kept semantically honest should one ever appear. */
    if (!lvrt_loop_has_work()) return 0;
    lvrt_loop_step();
    return 1;
}

/* ==================== task natives (lv_loop.c, verbatim) ==================
 * The sysTask* floor over the substrate — group children as registered
 * tasks, ids across the native boundary, parks that deliver cancellation.
 * Not gated on wasm (hard-03's table note: "the sysTask* engine floor —
 * async is doc 04's, not gated"), so these are live here, riding
 * lv_task_wasm.c's realization of spawn/cancel/park. */

typedef struct LvGroupTask { LvValue cb; } LvGroupTask;

static void lv_run_group_thunk(void* a, void* b) {
    (void)b;
    LvGroupTask* t = (LvGroupTask*)a;
    lv_invoke_cb(&t->cb, NULL, 0);     /* zero-arg closure call (hard-05 seam) */
    lvrt_release(&t->cb);              /* the spawn-time retain below */
    free(t);
}

void lvrt_systaskrun(LvValue* out, const LvValue* clo) {
    out->tag = LV_INT; out->payload = 0;
    if (!lv_tasks_enabled()) {
        lvrt_raise("TaskGroup requires tasks (running under LANG_PUMP=1)");
        return;
    }
    LvGroupTask* t = malloc(sizeof *t);
    if (!t) { lvrt_raise("task alloc failed"); return; }
    t->cb = *clo;
    lvrt_retain(clo);
    out->payload = (int64_t)lv_task_spawn_registered(lv_run_group_thunk, t, NULL);
}

void lvrt_systaskcancel(LvValue* out, const LvValue* id) {
    out->tag = LV_INT;
    out->payload = lv_task_cancel((uint64_t)id->payload);
}

void lvrt_systaskshield(LvValue* out, const LvValue* delta) {
    out->tag = LV_INT; out->payload = 0;
    lv_task_shield((int)delta->payload);
}

void lvrt_systaskjoinall(LvValue* out, const LvValue* arr) {
    out->tag = LV_INT; out->payload = 0;
    if (!lv_tasks_enabled()) {
        lvrt_raise("TaskGroup requires tasks (running under LANG_PUMP=1)");
        return;
    }
    if (arr->tag != LV_ARR) return;
    const uint8_t* base = (const uint8_t*)(intptr_t)arr->payload;
    int64_t rawlen; memcpy(&rawlen, base, sizeof rawlen);
    if (rawlen < 0) return;                     /* dense arrays never carry ids */
    out->payload = rawlen;
    if (rawlen == 0) return;
    uint64_t* ids = malloc((size_t)rawlen * sizeof *ids);
    if (!ids) { lvrt_raise("task join alloc failed"); return; }
    for (int64_t i = 0; i < rawlen; i++)
        memcpy(&ids[i], base + 8 + 16 * i + 8, sizeof ids[0]);   /* element payload */
    int r = lv_task_park_join(ids, (int)rawlen);
    free(ids);
    if (r == LV_PARK_DRAINED)
        lvrt_raise("TaskGroup.close: event loop drained with tasks unjoined");
    else if (r == LV_PARK_CANCELLED)
        lvrt_raise_cls("CancelledException", "task cancelled");
}

void lvrt_sysawaitany2(LvValue* out, const LvValue* a, const LvValue* b) {
    out->tag = LV_INT; out->payload = 0;
    if (a->tag != LV_OBJ || b->tag != LV_OBJ) return;
    const void* ka = (const void*)(intptr_t)a->payload;
    const void* kb = (const void*)(intptr_t)b->payload;
    /* ready -> proceed without yielding (doc 5 C4), first-declared-wins */
    if (lvrt_promise_state(ka)) return;
    if (lvrt_promise_state(kb)) { out->payload = 1; return; }
    if (!lv_tasks_enabled()) {
        lvrt_raise("awaitTimeout requires tasks (running under LANG_PUMP=1)");
        return;
    }
    int which = 0;
    int r = lv_task_park_any2(ka, kb, &which);
    if (r == LV_PARK_DRAINED)
        lvrt_raise("await: event loop drained with promise unresolved");
    else if (r == LV_PARK_CANCELLED)
        lvrt_raise_cls("CancelledException", "task cancelled");
    else
        out->payload = which;
}

/* ================== socket natives (lv_loop.c, verbatim) ==================
 * Thin wrappers over the floor; every lv_plat_tcp / send / recv on this
 * target is the absent-capability error convention (doc 03 §2), and every
 * reachable caller is compile-time gated (hard-03) — carried so the archive
 * links. */

void lvrt_systcpconnect(LvValue* out, const LvValue* host, const LvValue* port) {
    const char* ip = (const char*)(const void*)(intptr_t)(host->payload + 8);
    out->tag = LV_INT;
    out->payload = lv_plat_tcp_connect(ip, (int)port->payload);
}

void lvrt_systcpconnectnb(LvValue* out, const LvValue* host, const LvValue* port) {
    const char* ip = (const char*)(const void*)(intptr_t)(host->payload + 8);
    out->tag = LV_INT;
    out->payload = lv_plat_tcp_connect_nb(ip, (int)port->payload);
}

void lvrt_sysconnectresult(LvValue* out, const LvValue* fd) {
    out->tag = LV_INT;
    out->payload = lv_plat_connect_result((int)fd->payload);
}

void lvrt_systcplisten(LvValue* out, const LvValue* port) {
    out->tag = LV_INT;
    out->payload = lv_plat_tcp_listen(NULL, (int)port->payload, 16, 0);
}

void lvrt_systcplisten_reuse(LvValue* out, const LvValue* port, const LvValue* reusePort) {
    out->tag = LV_INT;
    out->payload = lv_plat_tcp_listen(NULL, (int)port->payload, 16,
                                      reusePort->payload != 0);
}

void lvrt_syscpucount(LvValue* out) {
    out->tag = LV_INT;
    out->payload = lv_plat_cpu_count();
}

void lvrt_syssocketbuffer(LvValue* out, const LvValue* fd,
                          const LvValue* sendBytes, const LvValue* recvBytes) {
    out->tag = LV_INT;
    out->payload = lv_plat_sock_buffer((int)fd->payload,
                                       sendBytes->payload, recvBytes->payload);
}

void lvrt_sysaccept(LvValue* out, const LvValue* fd) {
    out->tag = LV_INT;
    out->payload = lv_plat_accept((int)fd->payload);
}

void lvrt_syssend(LvValue* out, const LvValue* fd, const LvValue* s) {
    const void* bytes = (const void*)(intptr_t)(s->payload + 8);
    int64_t len = *(const int64_t*)(const void*)(intptr_t)s->payload;
    out->tag = LV_INT;
    int fdv = (int)fd->payload;
    if (lv_tls_is(fdv)) { out->payload = lv_tls_send(fdv, bytes, len); return; }
    out->payload = lv_plat_send(fdv, bytes, len);
}

void lvrt_sysrecv(LvValue* out, const LvValue* fd, const LvValue* max) {
    int64_t m = max->payload > 0 ? max->payload : 4096;
    char buf[m];
    int fdv = (int)fd->payload;
    if (lv_tls_is(fdv)) {
        int64_t r = lv_tls_recv(fdv, buf, m);
        if (r > 0) { lvrt_str_new(out, buf, r); return; }
        if (r == -1) { lvrt_str_new(out, "", 0); return; }
        out->tag = LV_NONE; out->payload = 0; return;
    }
    int64_t n = lv_plat_recv(fdv, buf, m);
    if (n == 0) { out->tag = LV_NONE; out->payload = 0; return; }
    if (n < 0) { lvrt_str_new(out, "", 0); return; }
    lvrt_str_new(out, buf, n);
}
