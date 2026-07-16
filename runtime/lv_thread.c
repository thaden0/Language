/* Track 10 M3c — true OS threads on the LLVM/POSIX backend
 * (techdesign-threads-3 §5). WorkerCtx, the pthread trampoline, and the join/
 * reap seam natives (sysThreadStart/sysThreadResult). This file is where the
 * cooperative interpreter leg becomes a real pthread: the body runs on its own
 * thread against its OWN TLS heap/arena/loop (per-worker isolation, D1'), and
 * its result rebuilds back on the SPAWNER's thread through an eventfd join — so
 * the Worker object, its continuation, and the loop callbacks are only ever
 * touched by the thread that owns them (A-1's cross-thread resolve is
 * structurally absent).
 *
 * Threads are POSIX/LLVM-only in v1: a Windows build never spawns (LV_TLS is
 * non-TLS under _WIN32, so real threads there would share what must be isolated
 * — doc-3 §7), and the LLVM codegen rejects the thread/channel seam natives for
 * a Windows target at compile time. This whole translation unit is therefore
 * empty on _WIN32; the symbols simply do not exist there (unreferenced, since a
 * Windows program never lowers spawn/Channel). Unlike lv_runtime.c/lv_loop.c
 * this file is NOT under the platform-floor audit — real threads inherently need
 * the OS threading primitives, so it uses pthreads/eventfd directly. */
#ifndef _WIN32

#include "lv_abi.h"
#include "lv_thread.h"
#include "lv_task.h"     /* LA-30 doc 2 §7: per-worker scheduler bootstrap (S4) */

#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

/* the runtime's registered dispatch trampoline (see lv_rt_dispatch_fn — not in
 * lv_abi.h because generated code never calls it, only our own runtime does). */
extern LvDispatchFn lv_rt_dispatch_fn(void);

/* bug #35: the codegen-registered spawn-body global-Promise checker (see
 * lvrt_register_spawn_check in lv_runtime.c). Same private-accessor discipline
 * as lv_rt_dispatch_fn — NULL when no generated program registered one. */
extern LvSpawnCheckFn lv_rt_spawn_check_fn(void);

typedef struct LvWorkerCtx {
    pthread_t tid;
    int       joinFd;        /* eventfd, level-triggered; the join wakeup */
    uint8_t*  captureBuf;    /* flattened body closure (worker consumes) */
    uint8_t*  resultBuf;     /* flattened result-or-message (worker writes, joiner frees) */
    int32_t   failedFlag;    /* the body threw: resultBuf carries the message string */
    uint8_t*  heapBase;      /* worker region bases, for reap-time munmap (§5.3) */
    uint8_t*  arenaBase;
} LvWorkerCtx;

/* fd -> ctx registry: the join watch fires with only the eventfd, so
 * sysThreadResult recovers its ctx by fd. A worker may itself spawn (R-3), so a
 * mutex guards it; spawn/join are cold, the lock cost is irrelevant. */
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static LvWorkerCtx**   g_reg;
static int             g_reg_n, g_reg_cap;

static void lv_reg_add(LvWorkerCtx* c) {
    pthread_mutex_lock(&g_reg_lock);
    if (g_reg_n == g_reg_cap) {
        g_reg_cap = g_reg_cap ? g_reg_cap * 2 : 8;
        g_reg = realloc(g_reg, (size_t)g_reg_cap * sizeof *g_reg);
    }
    g_reg[g_reg_n++] = c;
    pthread_mutex_unlock(&g_reg_lock);
}
static LvWorkerCtx* lv_reg_take(int fd) {
    pthread_mutex_lock(&g_reg_lock);
    LvWorkerCtx* found = NULL;
    for (int i = 0; i < g_reg_n; i++)
        if (g_reg[i] && g_reg[i]->joinFd == fd) { found = g_reg[i]; g_reg[i] = g_reg[--g_reg_n]; break; }
    pthread_mutex_unlock(&g_reg_lock);
    return found;
}

/* Flatten the thrown exception's MESSAGE (v1 carrier, §3.2) into resultBuf. The
 * concrete-exception-object carrier is deferred (request-promise-rejection.md);
 * a message string always flattens, so this never itself fails to cross. */
static void lv_worker_flatten_fail(LvWorkerCtx* ctx, const char* fallback) {
    LvValue exc; lvrt_thrown(&exc);
    LvValue msg;
    if (exc.tag == LV_OBJ) {
        lvrt_getfield(&msg, &exc, "message");
        if (msg.tag != LV_STR) lvrt_str_new(&msg, fallback, (int64_t)strlen(fallback));
    } else if (exc.tag == LV_STR) {
        msg = exc;
    } else {
        lvrt_str_new(&msg, fallback, (int64_t)strlen(fallback));
    }
    char err[160]; err[0] = 0; int64_t sz = 0;
    ctx->resultBuf = lv_thread_flatten(&msg, &sz, err, (int)sizeof err);
    ctx->failedFlag = 1;
}

/* The worker thread (§5.2). Bootstraps its own TLS runtime (NOT lv_rt_init —
 * F-i), rebuilds the body into its heap, runs it, drains its own loop, flattens
 * the result-or-exception into resultBuf, and signals the join eventfd. It does
 * NOT free its heap — reap munmaps it wholesale once the result is rebuilt out
 * (§5.3); everything the worker leaves behind is either flattened (region-
 * external) or reclaimed by that unmap. */
/* The body-run steps, shared verbatim by both legs below: rebuild the
 * flattened body into THIS heap, free the capture buffer, dispatch the
 * closure. Under LA-30 this is task 0 of the worker's scheduler; on the pump
 * leg it is a plain inline call on the OS stack — byte-identical to the
 * pre-LA-30 shape. (a = ctx, b = the ret out-slot; lv_task_fn signature.) */
static void lv_worker_body_task(void* a, void* b) {
    LvWorkerCtx* ctx = (LvWorkerCtx*)a;
    LvValue* ret = (LvValue*)b;
    LvValue body; lv_thread_rebuild(&body, ctx->captureBuf);
    lv_thread_free_buf(ctx->captureBuf);
    ctx->captureBuf = NULL;

    LvDispatchFn dispatch = lv_rt_dispatch_fn();
    if (dispatch && body.tag == LV_CLO) {
        int64_t fnIndex = *(const int64_t*)(const void*)(intptr_t)body.payload;
        LvValue args[1]; args[0] = body;         /* the closure is its own first arg */
        dispatch(fnIndex, ret, args, 1);
    }
}

static void* lv_worker_main(void* arg) {
    LvWorkerCtx* ctx = (LvWorkerCtx*)arg;
    lv_thread_ctx_init(&ctx->heapBase, &ctx->arenaBase);

    LvValue ret; ret.tag = LV_VOID; ret.payload = 0;
    if (lv_tasks_enabled()) {
        /* ⟦SENSITIVE-GATE S4⟧ (doc 2 §7, authored under the model gate) —
         * the worker scheduler bootstrap, symmetric with lv_entry.c's. Sched
         * TLS init rides lv_thread_ctx_init's contract ("initialize ONLY this
         * thread's TLS runtime state" — the scheduler struct is exactly such
         * state). The body runs as task 0; the scheduler replaces the R-3
         * drain below (its loop_step hook dispatches this worker's timers/
         * watches as tasks until quiescence). An uncaught body throw exits
         * the scheduler through the §8 gate with the flag intact and lands in
         * the existing lvrt_throwing() check below — the flatten_fail →
         * eventfd → sysThreadResult rethrow → prelude watch-catch → reject
         * seam is UNTOUCHED, so "never leaves the awaiter parked forever"
         * (threads risk #3) is preserved verbatim: the eventfd is always
         * signaled on every path out of this branch. A body parked forever
         * quiesces the scheduler → drain wake → parity: silent default value
         * (G2); M5: the C3 throw → this same reject seam (doc 2 §7). */
        lv_sched_init();
        lv_sched_hooks(lvrt_promise_state, lvrt_loop_step_tasks);
        lv_sched_throw_probe(lvrt_throwing);
        lv_sched_run(lv_worker_body_task, ctx, &ret);   /* body = task 0 of THIS thread */
        /* Pooled task stacks are TLS-reachable only — munmap them before this
         * thread dies or every completed worker leaks its stacks (doc 2 §7's
         * reap-time +0B rule; the pool-teardown note in lv_task.c). Before the
         * eventfd publish: after the signal the spawner may unmap our regions
         * at any moment, so nothing below the signal may run here anyway. */
        lv_sched_teardown();
        /* ⟦SENSITIVE-GATE S4 — end of fenced region⟧ */
    } else {
        lv_worker_body_task(ctx, &ret);   /* today's direct run, verbatim */
        /* R-3: a worker may register its own timers/watches; publish only when
         * the worker is genuinely done (doc-2 §4.2's drain order). */
        while (lvrt_loop_has_work()) lvrt_loop_step();
    }

    if (lvrt_throwing()) {
        lv_worker_flatten_fail(ctx, "worker failed");
    } else {
        char err[160]; err[0] = 0; int64_t sz = 0;
        ctx->resultBuf = lv_thread_flatten(&ret, &sz, err, (int)sizeof err);
        if (!ctx->resultBuf) {
            /* a non-flattenable RESULT cannot cross: carry the reason as a
             * failure message (same loud-and-local shape as a body throw). */
            LvValue m; lvrt_str_new(&m, err[0] ? err
                        : "worker result cannot cross a thread boundary (v1)",
                        (int64_t)strlen(err[0] ? err
                        : "worker result cannot cross a thread boundary (v1)"));
            char e2[160]; e2[0] = 0; int64_t sz2 = 0;
            ctx->resultBuf = lv_thread_flatten(&m, &sz2, e2, (int)sizeof e2);
            ctx->failedFlag = 1;
        }
    }

    /* Publish + wake. The worker's writes to ctx->resultBuf/failedFlag are
     * ordered BEFORE this write(2) (a syscall is a full barrier), and the
     * spawner's read(2) on the same eventfd is ordered before it reads them
     * (§5.3) — the kernel's eventfd write/read pair is the happens-before edge
     * for the whole handoff, so no ctx field needs its own atomic. (TSan does
     * not model eventfd's happens-before, so a TSan build flags/aborts here;
     * the handoff is nonetheless correct on real hardware — verified by a
     * 500-run stress sweep, 0 mismatches.) */
    uint64_t one = 1;
    ssize_t w = write(ctx->joinFd, &one, sizeof one);
    (void)w;
    return NULL;
}

/* sysThreadStart (spawner thread, §5.1): flatten the captures, create the join
 * eventfd, register the ctx, pthread_create, return the eventfd. The prelude
 * registers the watch on THIS loop in the same expression sequence, before any
 * await runs; the eventfd is level-triggered, so a worker that finishes before
 * the watch registers still leaves the fd readable (lost-wakeup ordering closed
 * from both sides, doc-2 problem #3). A non-flattenable capture raises HERE
 * (spawner thread) — the -1 return is only the interpreters' no-true-threads
 * probe, never a reject path. */
void lvrt_systhreadstart(LvValue* out, const LvValue* body) {
    out->tag = LV_INT; out->payload = -1;
    /* bug #35 (reject route A): a Promise the body reaches through a bare GLOBAL
     * bypasses the capture flatten below — it is read from the shared lv_globals,
     * never flattened. Re-apply A-1's reject to those globals HERE, before the
     * worker thread starts, byte-identical to the captured-Promise reject. The
     * generated checker maps the body closure's IR fn index to the referenced
     * globals; the seam is optional (NULL when unregistered, e.g. selftest). */
    if (body->tag == LV_CLO) {
        LvSpawnCheckFn chk = lv_rt_spawn_check_fn();
        if (chk) {
            int64_t fnIndex = *(const int64_t*)(const void*)(intptr_t)body->payload;
            const char* m = chk(fnIndex);
            if (m) { lvrt_raise(m); return; }
        }
    }
    char err[160]; err[0] = 0; int64_t sz = 0;
    uint8_t* capBuf = lv_thread_flatten(body, &sz, err, (int)sizeof err);
    if (!capBuf) {
        lvrt_raise(err[0] ? err : "value cannot cross a thread boundary (v1)");
        return;
    }
    LvWorkerCtx* ctx = calloc(1, sizeof *ctx);
    ctx->captureBuf = capBuf;
    ctx->joinFd = eventfd(0, 0);              /* blocking, level-triggered */
    if (ctx->joinFd < 0) {
        lv_thread_free_buf(capBuf); free(ctx);
        lvrt_raise("worker: eventfd creation failed");
        return;
    }
    lv_reg_add(ctx);
    atomic_fetch_add(&lv_thread_spawns, 1);
    if (pthread_create(&ctx->tid, NULL, lv_worker_main, ctx) != 0) {
        lv_reg_take(ctx->joinFd);
        atomic_fetch_add(&lv_thread_spawns, -1);
        close(ctx->joinFd); lv_thread_free_buf(capBuf); free(ctx);
        lvrt_raise("worker: pthread_create failed");
        return;
    }
    out->payload = ctx->joinFd;
}

/* sysThreadResult (spawner thread, inside the join watch callback, §5.3): drain
 * the eventfd, rebuild the worker's result into THIS thread's heap (strictly
 * BEFORE the unmap — F-2), join and reap the thread, munmap its regions, close
 * the fd (F-ii's POLLNVAL auto-cancel retires the watch). A failed worker
 * rethrows its carried message as a RuntimeException; the prelude's watch
 * try/catch routes that to Worker.reject, and await rethrows it at the join. */
void lvrt_systhreadresult(LvValue* out, const LvValue* joinFd) {
    out->tag = LV_VOID; out->payload = 0;
    int fd = (int)joinFd->payload;
    LvWorkerCtx* ctx = lv_reg_take(fd);
    if (!ctx) return;                          /* already reaped / unknown fd */

    uint64_t drain;
    ssize_t r = read(fd, &drain, sizeof drain);
    (void)r;

    int failed = ctx->failedFlag;
    LvValue rebuilt; rebuilt.tag = LV_VOID; rebuilt.payload = 0;
    if (ctx->resultBuf) {
        lv_thread_rebuild(&rebuilt, ctx->resultBuf);   /* +1 into this heap */
        lv_thread_free_buf(ctx->resultBuf);
        ctx->resultBuf = NULL;
    }
    pthread_join(ctx->tid, NULL);
    lv_thread_ctx_unmap(ctx->heapBase, ctx->arenaBase);
    lvrt_loop_cancel_fd(fd);   /* retire our join watch BEFORE the fd is reused */
    close(fd);
    atomic_fetch_add(&lv_thread_reaps, 1);
    free(ctx);

    if (failed) {
        if (rebuilt.tag == LV_STR)
            lvrt_raise((const char*)(intptr_t)(rebuilt.payload + 8));
        else
            lvrt_raise("worker failed");
        return;
    }
    *out = rebuilt;
}

/* ========================================================================
 * Track 10 M3d — Channel<T> on the LLVM leg (techdesign-threads-3 §6).
 *
 * A process-global SPSC ring of flattened-message pointers plus two eventfds
 * (dataReady producer->consumer, spaceReady consumer->producer for block
 * backpressure). head/tail are C11 acquire/release atomics — the ONLY atomics on
 * any hot path (D1' keeps everything else non-atomic). The Channel<T> object
 * carries only the channel id (a scalar), so flattening a Channel handle across
 * a spawn boundary degenerates to copying the id: both rebuilt handles name the
 * SAME record and portal semantics fall out of plain field copy (§3.4).
 *
 * DEVIATIONS (v1, logged like the serial-engine block asymmetry): a parked
 * producer/consumer blocks its THREAD on the eventfd (a blocking read) rather
 * than pumping its loop's other work — the corpus's endpoints do nothing else
 * while parked, so this is unobservable there; the loop-integrated park is a v2
 * refinement. The channel record itself is not reclaimed on endpoint drop (the
 * §6 2-count tally is deferred) — a bounded per-channel leak, invisible to the
 * counted-heap meter and to M6 (which asserts message BUFFERS, not records). */

typedef struct LvChannel {
    int64_t          capacity;
    int64_t          policy;        /* 0 block, 1 drop, 2 error */
    _Atomic int      closed;
    uint8_t**        ring;          /* `capacity` slots of flattened-msg pointers */
    _Atomic int64_t  head;          /* consumer cursor (monotonic; slot = head % cap) */
    _Atomic int64_t  tail;          /* producer cursor (monotonic; slot = tail % cap) */
    int              dataReady;     /* eventfd: producer -> consumer wake */
    int              spaceReady;    /* eventfd: consumer -> producer wake (block policy) */
} LvChannel;

static pthread_mutex_t g_chan_lock = PTHREAD_MUTEX_INITIALIZER;
static LvChannel**     g_chans;
static int64_t         g_chan_n, g_chan_cap;   /* g_chans[id] -> channel */

static LvChannel* lv_chan_get(int64_t id) {
    pthread_mutex_lock(&g_chan_lock);
    LvChannel* c = (id >= 0 && id < g_chan_n) ? g_chans[id] : NULL;
    pthread_mutex_unlock(&g_chan_lock);
    return c;
}

/* sysChannelNew(capacity, policy) -> id (>= 0). The id IS the capability probe:
 * an interpreter returns -1 and runs the in-process queue instead (§3.4). */
void lvrt_syschannelnew(LvValue* out, const LvValue* capacity, const LvValue* policy) {
    LvChannel* c = calloc(1, sizeof *c);
    c->capacity = capacity->payload > 0 ? capacity->payload : 1;
    c->policy = policy->payload;
    c->ring = calloc((size_t)c->capacity, sizeof *c->ring);
    c->dataReady = eventfd(0, 0);
    c->spaceReady = eventfd(0, 0);
    pthread_mutex_lock(&g_chan_lock);
    if (g_chan_n == g_chan_cap) {
        g_chan_cap = g_chan_cap ? g_chan_cap * 2 : 8;
        g_chans = realloc(g_chans, (size_t)g_chan_cap * sizeof *g_chans);
    }
    int64_t id = g_chan_n++;
    g_chans[id] = c;
    pthread_mutex_unlock(&g_chan_lock);
    out->tag = LV_INT; out->payload = id;
}

/* sysChannelSend(id, value): flatten the value, enqueue the buffer. On a full
 * ring: block = park on spaceReady; drop = discard; error = throw (§6). */
void lvrt_syschannelsend(LvValue* out, const LvValue* id, const LvValue* value) {
    out->tag = LV_INT; out->payload = 0;
    LvChannel* c = lv_chan_get(id->payload);
    if (!c) return;
    if (atomic_load_explicit(&c->closed, memory_order_acquire)) {
        lvrt_raise("send on a closed channel");
        return;
    }
    char err[160]; err[0] = 0; int64_t sz = 0;
    uint8_t* buf = lv_thread_flatten(value, &sz, err, (int)sizeof err);
    if (!buf) { lvrt_raise(err[0] ? err : "value cannot cross a thread boundary (v1)"); return; }
    for (;;) {
        int64_t head = atomic_load_explicit(&c->head, memory_order_acquire);
        int64_t tail = atomic_load_explicit(&c->tail, memory_order_relaxed);
        if (tail - head < c->capacity) {                  /* space: enqueue */
            c->ring[tail % c->capacity] = buf;
            atomic_store_explicit(&c->tail, tail + 1, memory_order_release);
            uint64_t one = 1; ssize_t w = write(c->dataReady, &one, sizeof one); (void)w;
            return;
        }
        /* full */
        if (c->policy == 1) { lv_thread_free_buf(buf); return; }               /* drop */
        if (c->policy == 2) { lv_thread_free_buf(buf); lvrt_raise("channel overflow"); return; }  /* error */
        uint64_t drain; ssize_t r = read(c->spaceReady, &drain, sizeof drain); (void)r; /* block: park */
    }
}

/* sysChannelReceive(id) -> T?: dequeue+rebuild the next message into this heap, or
 * None once the channel is closed AND drained (F-8). Parks on dataReady when the
 * ring is empty and open. */
void lvrt_syschannelreceive(LvValue* out, const LvValue* id) {
    out->tag = LV_NONE; out->payload = 0;
    LvChannel* c = lv_chan_get(id->payload);
    if (!c) return;
    for (;;) {
        int64_t head = atomic_load_explicit(&c->head, memory_order_relaxed);
        int64_t tail = atomic_load_explicit(&c->tail, memory_order_acquire);
        if (head < tail) {                                /* item available */
            uint8_t* buf = c->ring[head % c->capacity];
            atomic_store_explicit(&c->head, head + 1, memory_order_release);
            uint64_t one = 1; ssize_t w = write(c->spaceReady, &one, sizeof one); (void)w;
            lv_thread_rebuild(out, buf);                  /* +1 into this heap */
            lv_thread_free_buf(buf);
            return;
        }
        if (atomic_load_explicit(&c->closed, memory_order_acquire)) {
            out->tag = LV_NONE; out->payload = 0;         /* closed and drained */
            return;
        }
        uint64_t drain; ssize_t r = read(c->dataReady, &drain, sizeof drain); (void)r;  /* park */
    }
}

/* sysChannelClose(id): mark closed, wake a parked consumer so it observes it. */
void lvrt_syschannelclose(LvValue* out, const LvValue* id) {
    out->tag = LV_INT; out->payload = 0;
    LvChannel* c = lv_chan_get(id->payload);
    if (!c) return;
    atomic_store_explicit(&c->closed, 1, memory_order_release);
    uint64_t one = 1; ssize_t w = write(c->dataReady, &one, sizeof one); (void)w;
}

#endif /* !_WIN32 */
