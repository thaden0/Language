/* Track B — B-M3: the event loop (timer + fd-watch registries) and the
 * socket natives that ride it ("fd watches are the socket substrate" —
 * src/RuntimeLoop.hpp's own framing). Registries, due-time ordering, and
 * dispatch-until-idle semantics are ported from src/RuntimeLoop.cpp; socket
 * natives' ownership/behavior from src/X64Gen.cpp's genTcpConnect/Listen,
 * genRecv, genWatchAdd/Cancel, genTimerAdd/Cancel (ground truth) and
 * src/RuntimeNatives.cpp (the oracle's byte-identical behavior) — never
 * invented.
 *
 * No OS calls happen in this file — everything routes through lv_plat.h,
 * same discipline as lv_runtime.c (checked by the B-M1 acceptance grep,
 * now extended to cover this file too).
 */
#include "lv_abi.h"
#include "lv_plat.h"
#include "lv_thread.h"   /* Track 10: lvrt_loop_cancel_fd prototype (defined here) */
#include "lv_tls.h"      /* LA-2: wrap-in-place send/recv routing + §2.3 loop queries */
#include "lv_task.h"     /* LA-30 doc 2 §6: dispatch-on-task under the scheduler */

#include <string.h>   /* memcpy — raw in-place array loads (lv_invoke1's idiom) */
#include <stdlib.h>
#include <string.h>   /* memcpy — glibc pulls it in transitively, wasi-libc/
                       * mingw/clang-18-cross do not, and clang 18 makes the
                       * implicit declaration a hard error */

/* Runtime-internal bridge to lv_runtime.c's registered dispatch trampoline
 * (see lv_rt_dispatch_fn's definition there for why this isn't in
 * lv_abi.h: generated code never calls it, only our own runtime files do). */
extern LvDispatchFn lv_rt_dispatch_fn(void);

typedef struct Timer {
    int64_t id;
    int64_t dueNs;
    int64_t intervalMs;   /* 0 = one-shot */
    int64_t ticks;
    LvValue cb;
    int     active;
} Timer;

typedef struct Watch {
    int64_t id;
    int     fd;
    int     write;    /* 0: read-readiness; 1: write-readiness (Track 08 F5) */
    LvValue cb;
    int     active;
} Watch;

static LV_TLS Timer*  g_timers;
static LV_TLS int64_t g_timerCount, g_timerCap;
static LV_TLS Watch*  g_watches;
static LV_TLS int64_t g_watchCount, g_watchCap;
static LV_TLS int64_t g_nextId = 1;

/* invokes `clo(argVal)` through the registered dispatch trampoline — the
 * closure is passed as the callee's own first parameter (X64Gen's
 * genCallClosure forwarding order, src/X64Gen.cpp:1978-1998: the closure
 * value itself is pushed as the call's first logical argument, ahead of
 * the real args, so the callee's body can read its own captures via
 * lvrt_capture_get). fnIndex sits at the closure body's word0, identical
 * to an object's classId slot (§2.4: closures reuse the object layout
 * verbatim). Releases whatever the callback returns — event callbacks are
 * void in practice, but a stray +1 must not leak if one ever isn't. */
static void lv_invoke1(const LvValue* clo, LvValue argVal) {
    LvDispatchFn dispatch = lv_rt_dispatch_fn();
    if (!dispatch) return;
    int64_t fnIndex = *(const int64_t*)(const void*)(intptr_t)clo->payload;
    LvValue args[2]; args[0] = *clo; args[1] = argVal;
    LvValue ret; ret.tag = LV_VOID; ret.payload = 0;
    dispatch(fnIndex, &ret, args, 2);
    lvrt_release(&ret);
}

/* ==================== LA-30 doc 2 §6 — dispatch-on-task ====================
 * Under the scheduler, every closure the loop fires becomes a task; the loop
 * itself never runs language code. The flag below is set ONLY inside
 * lvrt_loop_step_tasks (the scheduler's hook): the raw lvrt_loop_step keeps
 * inline dispatch in BOTH modes, because lv_main's emitted tail drain
 * (LlvmGen.cpp:693-716) pumps it directly from inside task 0 — task-spawning
 * from there would run callbacks AFTER lv_main's uncaught/meter epilogue and
 * break M1's byte-identity bar. Batch note (§6): spawn order = readiness
 * order = FIFO runq order, and this loop never WAITS for the spawned tasks —
 * that would rebuild the pump one level up; the scheduler owns sequencing. */
static LV_TLS int g_dispatch_as_tasks;

typedef struct LvCbTask { LvValue cb; LvValue arg; } LvCbTask;

static void lv_run_closure_thunk(void* a, void* b) {
    (void)b;
    LvCbTask* t = (LvCbTask*)a;
    lv_invoke1(&t->cb, t->arg);
    lvrt_release(&t->cb);        /* the spawn-time retain below: task-owned ref */
    free(t);
}

/* One choke point for both fire sites (watch + timer). The spawn takes its own
 * +1 on the closure so a registry release (one-shot completion, sysUnwatch,
 * lvrt_loop_cancel_fd) between spawn and run cannot free a closure a queued
 * task still needs — the same lifetime rule the inline path's retain-across-
 * invoke enforces, stretched across the queue. */
static void lv_dispatch_cb(const LvValue* cb, LvValue argVal) {
    if (g_dispatch_as_tasks) {
        LvCbTask* t = malloc(sizeof *t);
        if (!t) { lv_invoke1(cb, argVal); return; }   /* degrade, never drop */
        t->cb = *cb; t->arg = argVal;
        lvrt_retain(cb);
        lv_task_spawn(lv_run_closure_thunk, t, NULL);
    } else {
        lv_invoke1(cb, argVal);
    }
}

int lvrt_loop_step_tasks(void) {
    if (!lvrt_loop_has_work()) return 0;
    g_dispatch_as_tasks = 1;
    lvrt_loop_step();
    g_dispatch_as_tasks = 0;
    return 1;
}

/* ==================== LA-30 B2 (doc 06 §4) — task natives ===================
 * The sysTask* floor over the doc-1 substrate extensions (lv_task.c): group
 * children as registered tasks, ids across the native boundary (never
 * handles), parks that deliver cancellation. This file owns them because it
 * already owns the closure->task bridge (lv_dispatch_cb above); the group
 * thunk below is that bridge's zero-arg twin. All results are scalars, written
 * to `out` BEFORE any raise (the lvrt_syschannelsend convention: the dk==2
 * wrap has released the old dest, so a throwing native must still leave a
 * valid immediate there). */

typedef struct LvGroupTask { LvValue cb; } LvGroupTask;

static void lv_run_group_thunk(void* a, void* b) {
    (void)b;
    LvGroupTask* t = (LvGroupTask*)a;
    LvDispatchFn dispatch = lv_rt_dispatch_fn();
    if (dispatch) {
        /* zero-arg closure call: the closure value is the callee's own first
         * parameter (lv_invoke1's forwarding order, minus the event arg). */
        int64_t fnIndex = *(const int64_t*)(const void*)(intptr_t)t->cb.payload;
        LvValue args[1]; args[0] = t->cb;
        LvValue ret; ret.tag = LV_VOID; ret.payload = 0;
        dispatch(fnIndex, &ret, args, 1);
        lvrt_release(&ret);
        /* an uncaught throw in the body leaves the pending-throw flag set: the
         * trampoline returns it to the scheduler, whose §8 gate stops dispatch
         * — program-uncaught, exactly C2's rule for loop callbacks. The
         * CancelledException absorption for group children lives in the
         * PRELUDE wrapper (TaskGroup.run), not here. */
    }
    lvrt_release(&t->cb);        /* the spawn-time retain below: task-owned ref */
    free(t);
}

/* sysTaskRun(closure) -> id. Spawn + register; the closure is retained across
 * the queue exactly like lv_dispatch_cb's (a cancel of the GROUP between spawn
 * and first run cannot free a closure a queued task still needs). */
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

/* sysTaskCancel(id) -> 1 marked / 0 unknown-or-done (§8 idempotence). Safe in
 * both modes: under the pump the registry is empty, so it reads "done". */
void lvrt_systaskcancel(LvValue* out, const LvValue* id) {
    out->tag = LV_INT;
    out->payload = lv_task_cancel((uint64_t)id->payload);
}

/* sysTaskShield(delta) -> 0. The §8 shield-mask counter; a no-op from the
 * scheduler context or under the pump (lv_task_shield guards). */
void lvrt_systaskshield(LvValue* out, const LvValue* delta) {
    out->tag = LV_INT; out->payload = 0;
    lv_task_shield((int)delta->payload);
}

/* sysTaskJoinAll(Array<int> ids) -> count. Parks until every id is done; the
 * id buffer is copied out of the language array onto THIS task's C frame
 * (malloc'd), so the substrate borrows per G12 from a frame that owns it. */
void lvrt_systaskjoinall(LvValue* out, const LvValue* arr) {
    out->tag = LV_INT; out->payload = 0;
    if (!lv_tasks_enabled()) {
        lvrt_raise("TaskGroup requires tasks (running under LANG_PUMP=1)");
        return;
    }
    if (arr->tag != LV_ARR) return;
    /* raw in-place loads (lv_invoke1's idiom — lv_ld_i64 is lv_runtime-static):
     * rawlen at +0; elements are {tag,payload} pairs from +8, payload at +8. */
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

/* sysAwaitAny2(a, b) -> 0/1 (which settled first). The multi-park consumer
 * (awaitTimeout's {work, timer}). Reads NO value fields (G15/S2: the value
 * read and failed-rethrow stay in the ordinary await the prelude issues after
 * this returns) — this native only learns WHICH promise settled. */
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

/* ========================================================================
 * Timers — due-time ordering, one-shot vs repeating, registry ownership.
 * ==================================================================== */

static int64_t lv_loop_add_timer(int64_t delayMs, int64_t intervalMs, const LvValue* cb) {
    if (g_timerCount == g_timerCap) {
        g_timerCap = g_timerCap ? g_timerCap * 2 : 8;
        g_timers = realloc(g_timers, (size_t)g_timerCap * sizeof *g_timers);
    }
    Timer* t = &g_timers[g_timerCount++];
    t->id = g_nextId++;
    t->dueNs = lv_plat_now_ns() + delayMs * 1000000;
    t->intervalMs = intervalMs;
    t->ticks = 0;
    t->cb = *cb;
    t->active = 1;
    lvrt_retain(cb);   /* registry owns the callback until cancel/one-shot completion */
    return t->id;
}

static void lv_loop_cancel_timer(int64_t id) {
    for (int64_t i = 0; i < g_timerCount; i++) {
        if (g_timers[i].active && g_timers[i].id == id) {
            g_timers[i].active = 0;
            lvrt_release(&g_timers[i].cb);
            return;
        }
    }
}

void lvrt_systimerstart(LvValue* out, const LvValue* delayMs,
                        const LvValue* intervalMs, const LvValue* cb) {
    out->tag = LV_INT;
    out->payload = lv_loop_add_timer(delayMs->payload, intervalMs->payload, cb);
}

void lvrt_systimercancel(const LvValue* id) {
    lv_loop_cancel_timer(id->payload);
}

/* ========================================================================
 * FD watches — read-readiness, registry ownership.
 * ==================================================================== */

static int64_t lv_loop_add_watch(int fd, const LvValue* cb, int isWrite) {
    /* Reuse a retired slot before growing — Track 10 churns one join watch per
     * worker, so without reuse g_watches would grow unboundedly under the M6
     * spawn/join sweep (the slots are malloc, not the counted heap, but a real
     * leak all the same). An inactive slot has already released its cb. */
    int64_t slot = -1;
    for (int64_t i = 0; i < g_watchCount; i++)
        if (!g_watches[i].active) { slot = i; break; }
    if (slot < 0) {
        if (g_watchCount == g_watchCap) {
            g_watchCap = g_watchCap ? g_watchCap * 2 : 8;
            g_watches = realloc(g_watches, (size_t)g_watchCap * sizeof *g_watches);
        }
        slot = g_watchCount++;
    }
    Watch* w = &g_watches[slot];
    w->id = g_nextId++;
    w->fd = fd;
    w->write = isWrite;
    w->cb = *cb;
    w->active = 1;
    lvrt_retain(cb);   /* mirrors timer_add (X64Gen genWatchAdd) */
    return w->id;
}

static void lv_loop_cancel_watch(int64_t id) {
    for (int64_t i = 0; i < g_watchCount; i++) {
        if (g_watches[i].active && g_watches[i].id == id) {
            g_watches[i].active = 0;
            lvrt_release(&g_watches[i].cb);
            return;
        }
    }
}

/* Track 10 §5.3: cancel EVERY watch on `fd` immediately, before its eventfd
 * number is closed and reused by the next spawn. Relying on POLLNVAL (F-ii) is
 * not enough here — a reaped worker's join fd is reused by the next eventfd()
 * before the next poll, so the stale watch would fire on the NEW worker's fd and
 * reg_take the wrong ctx. Safe to call from inside the watch's own callback:
 * lvrt_loop_step retains the cb across its invoke, so releasing the registry's
 * ref here cannot free a closure whose body is still running. */
void lvrt_loop_cancel_fd(int fd) {
    for (int64_t i = 0; i < g_watchCount; i++)
        if (g_watches[i].active && g_watches[i].fd == fd) {
            g_watches[i].active = 0;
            lvrt_release(&g_watches[i].cb);
        }
}

void lvrt_syswatch(LvValue* out, const LvValue* fd, const LvValue* cb) {
    out->tag = LV_INT;
    out->payload = lv_loop_add_watch((int)fd->payload, cb, 0);
}

/* Track 08 F5: write-readiness watch — same registry/id space/cancel path;
 * when none is registered the poll set is bit-identical to the read-only
 * form (the design's problem-#2 "provably unaffected" guarantee). */
void lvrt_syswatchwrite(LvValue* out, const LvValue* fd, const LvValue* cb) {
    out->tag = LV_INT;
    out->payload = lv_loop_add_watch((int)fd->payload, cb, 1);
}

void lvrt_sysunwatch(const LvValue* id) {
    lv_loop_cancel_watch(id->payload);
}

/* ========================================================================
 * The loop itself — has_work / step, exactly what Await's codegen pumps
 * (mirrors X64Gen's inline has_work()/loop_step() calls, src/X64Gen.cpp:
 * 3573-3593: no other entry points are needed for `await` to work).
 * ==================================================================== */

int32_t lvrt_loop_has_work(void) {
    for (int64_t i = 0; i < g_timerCount; i++) if (g_timers[i].active) return 1;
    for (int64_t i = 0; i < g_watchCount; i++) if (g_watches[i].active) return 1;
    return 0;
}

/* One batch: poll active watches up to the earliest timer's deadline (block
 * indefinitely if only watches are pending, don't poll at all if there are
 * no watches), then fire every ready watch and every due timer — watches
 * first, then timers in (due, id) order, matching src/RuntimeLoop.cpp's
 * nextBatch exactly. Ready ids are snapshotted before firing anything:
 * callbacks may add/cancel timers or watches mid-batch (src/RuntimeLoop.cpp's
 * own "callbacks may add/cancel watches" note), and firing into a registry
 * that's being resized under us would read stale/freed memory. */
void lvrt_loop_step(void) {
    int64_t activeWatchN = 0;
    for (int64_t i = 0; i < g_watchCount; i++) if (g_watches[i].active) activeWatchN++;

    int haveTimers = 0;
    int64_t earliestDue = 0;
    for (int64_t i = 0; i < g_timerCount; i++) {
        if (!g_timers[i].active) continue;
        if (!haveTimers || g_timers[i].dueNs < earliestDue) earliestDue = g_timers[i].dueNs;
        haveTimers = 1;
    }
    int timeoutMs = -1;
    if (haveTimers) {
        int64_t remainNs = earliestDue - lv_plat_now_ns();
        /* ceiling, not truncating, division: a sub-millisecond remainder
         * must round UP to at least 1ms. Truncating (as in a naive port of
         * src/RuntimeLoop.cpp's duration_cast<milliseconds>) collapses any
         * "due in 0 < x < 1ms" into timeoutMs=0, which the no-watches branch
         * below reads as "already due" and skips sleeping — the timer isn't
         * actually due yet, so step() returns having done nothing, and a
         * caller polling in a tight loop (no per-call syscall overhead to
         * accidentally eat the remainder, unlike the oracle's interpreter
         * loop) never converges. Caught by the B-M3 selftest's repeating-
         * timer case hanging short of 3 ticks. */
        timeoutMs = remainNs <= 0 ? 0 : (int)((remainNs + 999999) / 1000000);
    }

    /* LA-2 §2.3 #2 (TLS buffered-plaintext stall): SSL_read can drain a whole
     * TLS record into OpenSSL's buffer while pump() takes only one chunk — the
     * socket then polls idle with plaintext undelivered. Any read watch whose
     * session has buffered plaintext forces a non-blocking poll pass (timeout 0) and
     * fires regardless of the poll result. lv_tls_pending is always 0 with no
     * live session, so timeoutMs is untouched — the §2.3 bit-identity guarantee. */
    int anyPending = 0;
    for (int64_t i = 0; i < g_watchCount; i++)
        if (g_watches[i].active && !g_watches[i].write && lv_tls_pending(g_watches[i].fd)) {
            anyPending = 1; break;
        }
    if (anyPending) timeoutMs = 0;

    if (activeWatchN > 0) {
        LvPollFd* pfds = malloc((size_t)activeWatchN * sizeof *pfds);
        int64_t* owner = malloc((size_t)activeWatchN * sizeof *owner);   /* pfds[k] -> watch id */
        int16_t* want = malloc((size_t)activeWatchN * sizeof *want);   /* per-slot readiness kind */
        int8_t*  pend = malloc((size_t)activeWatchN * sizeof *pend);   /* per-slot pending flag */
        int64_t n = 0;
        for (int64_t i = 0; i < g_watchCount; i++) {
            if (!g_watches[i].active) continue;
            pfds[n].fd = g_watches[i].fd;
            /* §2.3 #1 (want-direction inversion): base direction is the watch's
             * own; if this fd's TLS session currently needs the OTHER direction
             * (a read needing the fd writable, or vice versa, mid-handshake or on
             * a TLS 1.3 KeyUpdate), also poll for — and fire on — that direction.
             * lv_tls_wants is 0 for any plaintext fd, so the mask is bit-identical
             * to the pre-TLS `write ? LV_POLLOUT : LV_POLLIN`. */
            int16_t ev = g_watches[i].write ? LV_POLLOUT : LV_POLLIN;
            int wnt = lv_tls_wants(g_watches[i].fd);
            if (wnt == 1) ev |= LV_POLLIN;
            else if (wnt == 2) ev |= LV_POLLOUT;
            pfds[n].events = ev;
            pfds[n].revents = 0;
            want[n] = ev;
            pend[n] = (!g_watches[i].write && lv_tls_pending(g_watches[i].fd)) ? 1 : 0;
            owner[n] = g_watches[i].id;
            n++;
        }
        int ready = lv_plat_poll(pfds, (int)n, timeoutMs);
        if (ready > 0 || anyPending) {
            /* LV_POLLNVAL = the fd was closed under the watch. It counts as
             * a wake (the callback observes the dead fd via its recv), and
             * the watch is then retired — a dead descriptor can never become
             * readable again, and leaving it registered makes every poll
             * pass return instantly with an event nobody consumes: a silent
             * busy spin (RuntimeLoop.cpp had the identical gap; see
             * lv_plat.h's LV_POLLNVAL note). Fire first, cancel after —
             * lv_loop_cancel_watch releases the registry's callback ref,
             * and releasing before the invoke is the same use-after-free
             * the one-shot timer path already taught us about. */
            /* sized by n (not `ready`): the §2.3 pending scan can mark watches
             * ready that poll didn't report, so up to every watch may fire. */
            int64_t* readyIds = malloc((size_t)n * sizeof *readyIds);
            int64_t* deadIds  = malloc((size_t)n * sizeof *deadIds);
            int64_t rc = 0, dc = 0;
            for (int64_t k = 0; k < n; k++) {
                /* Fire when ANY watched direction (base or want-augmented) is
                 * ready — LV_POLLIN for read watches, LV_POLLOUT for write
                 * watches, plus whichever the TLS session wanted; the plat layer
                 * folds ERR/HUP into both so error wakes reach either. §2.3 #2:
                 * a pending-plaintext read watch fires even if poll saw nothing. */
                if ((pfds[k].revents & want[k]) || pend[k]) readyIds[rc++] = owner[k];
                if (pfds[k].revents & LV_POLLNVAL) deadIds[dc++] = owner[k];
            }
            for (int64_t k = 0; k < rc; k++) {
                for (int64_t i = 0; i < g_watchCount; i++) {
                    if (g_watches[i].active && g_watches[i].id == readyIds[k]) {
                        LvValue fdVal; fdVal.tag = LV_INT; fdVal.payload = g_watches[i].fd;
                        /* Retain the cb across its invoke so a callback that
                         * cancels its OWN watch (Track 10's join watch via
                         * lvrt_loop_cancel_fd, or a user sysUnwatch(self)) can
                         * release the registry's ref without freeing the closure
                         * whose body is still executing — this local ref keeps it
                         * alive until the body returns. */
                        LvValue cb = g_watches[i].cb;
                        lvrt_retain(&cb);
                        lv_dispatch_cb(&cb, fdVal);   /* LA-30 §6: task under sched */
                        lvrt_release(&cb);
                        break;
                    }
                }
            }
            for (int64_t k = 0; k < dc; k++) lv_loop_cancel_watch(deadIds[k]);
            free(readyIds); free(deadIds);
        }
        free(pfds); free(owner); free(want); free(pend);
    } else if (timeoutMs > 0) {
        lv_plat_poll(NULL, 0, timeoutMs);   /* no watches: poll(nfds=0) as a portable sleep */
    }

    /* due timers, oldest-due-first (ties broken by id, i.e. registration
     * order — matches src/RuntimeLoop.cpp's stable_sort on (due, id)). */
    int64_t nowNs = lv_plat_now_ns();
    int64_t dueCount = 0;
    for (int64_t i = 0; i < g_timerCount; i++)
        if (g_timers[i].active && g_timers[i].dueNs <= nowNs) dueCount++;
    if (dueCount == 0) return;

    int64_t* due = malloc((size_t)dueCount * sizeof *due);
    int64_t n = 0;
    for (int64_t i = 0; i < g_timerCount; i++)
        if (g_timers[i].active && g_timers[i].dueNs <= nowNs) due[n++] = i;
    for (int64_t a = 0; a < n; a++)              /* small-n insertion sort by (dueNs, id) */
        for (int64_t b = a; b > 0; b--) {
            Timer* x = &g_timers[due[b]]; Timer* y = &g_timers[due[b - 1]];
            int swap = x->dueNs < y->dueNs || (x->dueNs == y->dueNs && x->id < y->id);
            if (!swap) break;
            int64_t tmp = due[b]; due[b] = due[b - 1]; due[b - 1] = tmp;
        }

    for (int64_t k = 0; k < n; k++) {
        int64_t i = due[k];
        if (!g_timers[i].active) continue;   /* a prior callback in this batch canceled it */
        g_timers[i].ticks++;
        LvValue tickVal; tickVal.tag = LV_INT; tickVal.payload = g_timers[i].ticks;
        LvValue cb = g_timers[i].cb;          /* snapshot: firing may realloc g_timers */
        int64_t interval = g_timers[i].intervalMs;
        if (interval > 0) {
            g_timers[i].dueNs += interval * 1000000;
            lv_dispatch_cb(&cb, tickVal);      /* registry keeps its ref: invoke straight away */
        } else {
            /* one-shot: deactivate BEFORE invoking (a re-entrant cancel of
             * this same id from inside the callback must be a harmless
             * no-op, not a double-release) but release the registry's ref
             * only AFTER invoking — releasing first would free `cb` while
             * lv_invoke1 still needs to read its fnIndex (caught by the
             * B-M3 selftest: the callback landed on freed, poisoned memory
             * and silently no-op'd instead of firing). */
            g_timers[i].active = 0;
            lv_dispatch_cb(&cb, tickVal);      /* spawn path takes its own +1 first */
            lvrt_release(&cb);
        }
    }
    free(due);
}

/* ========================================================================
 * Sockets — the fd-watch substrate. Behavior/ownership from
 * src/RuntimeNatives.cpp and X64Gen's genTcpConnect/Listen, genSend, genRecv.
 * ==================================================================== */

void lvrt_systcpconnect(LvValue* out, const LvValue* host, const LvValue* port) {
    const char* ip = (const char*)(const void*)(intptr_t)(host->payload + 8);
    out->tag = LV_INT;
    out->payload = lv_plat_tcp_connect(ip, (int)port->payload);
}

/* Track 08 F5: the connect-timeout floor (behavior from RuntimeNatives.cpp's
 * sysTcpConnectNb/sysConnectResult — the oracle; never invented). */
void lvrt_systcpconnectnb(LvValue* out, const LvValue* host, const LvValue* port) {
    const char* ip = (const char*)(const void*)(intptr_t)(host->payload + 8);
    out->tag = LV_INT;
    out->payload = lv_plat_tcp_connect_nb(ip, (int)port->payload);
}

void lvrt_sysconnectresult(LvValue* out, const LvValue* fd) {
    out->tag = LV_INT;
    out->payload = lv_plat_connect_result((int)fd->payload);
}

/* binds 127.0.0.1 (lv_plat_tcp_listen treats a NULL/empty ip as loopback),
 * backlog 16 — RuntimeNatives.cpp's sysTcpListen(port) parity. */
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
    /* LA-2 §2.1: a TLS session on this fd routes through SSL_write (same -1/-2
     * contract); a plaintext fd takes the untouched path. */
    if (lv_tls_is(fdv)) { out->payload = lv_tls_send(fdv, bytes, len); return; }
    out->payload = lv_plat_send(fdv, bytes, len);
}

/* three-way narrowing (RuntimeNatives.cpp's sysRecv comment): None = peer
 * closed (EOF), a fresh "" = nothing available now (EAGAIN), else a fresh
 * string of the received bytes. */
void lvrt_sysrecv(LvValue* out, const LvValue* fd, const LvValue* max) {
    int64_t m = max->payload > 0 ? max->payload : 4096;
    char buf[m];
    int fdv = (int)fd->payload;
    /* LA-2 §2.1: SSL_read mapped onto the three-state contract — data -> data,
     * would-block -> "", clean close_notify / fatal / truncation -> None. */
    if (lv_tls_is(fdv)) {
        int64_t r = lv_tls_recv(fdv, buf, m);
        if (r > 0) { lvrt_str_new(out, buf, r); return; }
        if (r == -1) { lvrt_str_new(out, "", 0); return; }   /* would-block */
        out->tag = LV_NONE; out->payload = 0; return;         /* clean-EOF / fatal */
    }
    int64_t n = lv_plat_recv(fdv, buf, m);
    if (n == 0) { out->tag = LV_NONE; out->payload = 0; return; }
    if (n < 0) { lvrt_str_new(out, "", 0); return; }
    lvrt_str_new(out, buf, n);
}
