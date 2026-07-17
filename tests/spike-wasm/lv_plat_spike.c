/* Track W — W-M0 de-risking spike (designs/wasm-frontend/techdesign-01-spike.md §3.2).
 *
 * THROWAWAY. This is spike-only garbage that satisfies the `lv_plat_*` floor
 * (runtime/lv_plat.h) plus the task-scheduler and TLS-provider symbols that the
 * real runtime (lv_task.c / lv_tls_none.c) would supply — neither of which the
 * spike compiles for wasm (lv_task.c needs arch-asm context switches; TLS needs
 * a provider). doc-03's lv_plat_wasm.c is the REAL floor and does NOT grow from
 * this file (spike §2).
 *
 * The spike corpus is pure computation + `print` only: no async/await, no
 * timers, no sockets, no files, no threads. So only a tiny floor slice runs —
 * write (print's sink), map/unmap (the allocator's backing), exit. Everything
 * else abort()s loudly if a corpus file ever reaches it (which would itself be
 * a spike finding, not a thing to paper over).
 *
 * Built against wasi-libc: malloc/write/clock/_exit are real WASI calls, so the
 * floor is a thin shim over libc rather than raw imports. lv_tasks_enabled()
 * returns 0, so lv_entry.c takes the pump-until-quiescent path (§3.2) and the
 * whole scheduler branch is dead — its symbols exist only to satisfy the link.
 */
#include "lv_abi.h"
#include "lv_plat.h"

#include <stdint.h>
#include <stdlib.h>   /* malloc, free, _Exit         */
#include <string.h>   /* memset                      */
#include <unistd.h>   /* write (WASI fd_write)       */
#include <time.h>     /* clock_gettime               */

/* ------- a loud stop for anything outside the pure-compute floor ------- */
extern void abort(void);
static void spike_unreached(const char* what) {
    /* one imported write, then trap — never returns */
    (void)write(2, "spike floor: unreached path: ", 29);
    if (what) (void)write(2, what, (int)strlen(what));
    (void)write(2, "\n", 1);
    abort();
}

/* =========================== B-M1 floor slice =========================== */

void* lv_plat_map(int64_t size) {
    /* The §15 allocator asks for a raw RW region; malloc over the WASI heap
     * (memory.grow underneath) is exactly that. Zeroed to match the POSIX
     * MAP_ANONYMOUS contract the runtime relies on. */
    if (size <= 0) return 0;
    void* p = malloc((size_t)size);
    if (p) memset(p, 0, (size_t)size);
    return p;
}
void lv_plat_unmap(void* base, int64_t size) { (void)size; free(base); }

int64_t lv_plat_write(int fd, const void* buf, int64_t n) {
    /* print/syswrite's sink — the one path that makes stdout observable. */
    return (int64_t)write(fd, buf, (size_t)n);
}
int64_t lv_plat_read(int fd, void* buf, int64_t n) {
    (void)fd; (void)buf; (void)n; return 0;   /* EOF — no stdin in the corpus */
}

int64_t lv_plat_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
int64_t lv_plat_now_realtime_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
void lv_plat_exit(int code) { _Exit(code); }

/* ===================== everything past the slice: abort ================= */
/* These have no place in a pure-compute + print spike; reaching one is a
 * finding. Signatures mirror runtime/lv_plat.h exactly.                    */
int     lv_plat_open(const char* p, int b, int m){ (void)p;(void)b;(void)m; spike_unreached("open"); return -1; }
void    lv_plat_close(int fd){ (void)fd; spike_unreached("close"); }
int     lv_plat_term_raw(int fd){ (void)fd; return -1; }        /* "not a tty" — harmless */
int     lv_plat_term_restore(int fd){ (void)fd; return 0; }
int     lv_plat_term_size(int fd,int*r,int*c){ (void)fd;(void)r;(void)c; return -1; }
int     lv_plat_term_israw(void){ return 0; }
int     lv_plat_signal_open(const int* s,int n){ (void)s;(void)n; return -1; }
int     lv_plat_signal_next(int fd){ (void)fd; return -1; }
void    lv_plat_signal_close(int fd){ (void)fd; }
int64_t lv_plat_stat_size(const char* p){ (void)p; return -1; }
int64_t lv_plat_stat_mtime(const char* p){ (void)p; return -1; }
int     lv_plat_stat_isdir(const char* p){ (void)p; return -1; }
int     lv_plat_mkdir(const char* p){ (void)p; spike_unreached("mkdir"); return -1; }
int     lv_plat_tcp_connect(const char* ip,int port){ (void)ip;(void)port; spike_unreached("tcp_connect"); return -1; }
int     lv_plat_tcp_listen(const char* ip,int port,int bk,int rp){ (void)ip;(void)port;(void)bk;(void)rp; spike_unreached("tcp_listen"); return -1; }
int     lv_plat_cpu_count(void){ return 1; }
int     lv_plat_accept(int fd){ (void)fd; spike_unreached("accept"); return -1; }
int64_t lv_plat_send(int fd,const void* b,int64_t n){ (void)fd;(void)b;(void)n; spike_unreached("send"); return -1; }
int64_t lv_plat_recv(int fd,void* b,int64_t n){ (void)fd;(void)b;(void)n; spike_unreached("recv"); return -1; }
int     lv_plat_sock_buffer(int fd,int64_t s,int64_t r){ (void)fd;(void)s;(void)r; return -1; }
void    lv_plat_set_nonblock(int fd){ (void)fd; }
int64_t lv_plat_random(void* b,int64_t n){ (void)b;(void)n; spike_unreached("random"); return -1; }
int     lv_plat_tcp_connect_nb(const char* ip,int port){ (void)ip;(void)port; spike_unreached("tcp_connect_nb"); return -1; }
int     lv_plat_connect_result(int fd){ (void)fd; spike_unreached("connect_result"); return -1; }
int     lv_plat_poll(LvPollFd* f,int n,int t){ (void)f;(void)n;(void)t; return -1; }

/* =============== task scheduler (lv_task.c is not in the spike) ==========
 * lv_tasks_enabled() == 0 sends lv_entry.c down the pump-until-quiescent
 * path (spike §3.2), so lv_sched_* / the parks are dead — present only for
 * the link. lvrt_promise_state is a loop hook the drain never reaches
 * (has_work() is 0 with no timers/watches registered).                    */
int  lv_tasks_enabled(void){ return 0; }
void lv_sched_init(void){ spike_unreached("sched_init"); }
void lv_sched_hooks(void* a, void* b){ (void)a;(void)b; spike_unreached("sched_hooks"); }
void lv_sched_throw_probe(void* p){ (void)p; spike_unreached("sched_throw_probe"); }
void lv_sched_run(void* fn, void* ret, void* arg){ (void)fn;(void)ret;(void)arg; spike_unreached("sched_run"); }
int  lv_sched_main_completed(void){ return 1; }
/* NB: lvrt_promise_state is a REAL definition in lv_runtime.c — not stubbed. */

/* Signatures matched to lv_task.c's real prototypes so wasm-ld's per-function
 * type check stays quiet — these bodies are dead (no await/spawn in the spike). */
int  lv_task_park_on(void* a){ (void)a; spike_unreached("park_on"); return 0; }
int  lv_task_park_join(void* a, void* b){ (void)a;(void)b; spike_unreached("park_join"); return 0; }
int  lv_task_park_any2(void* a, void* b, void* c){ (void)a;(void)b;(void)c; spike_unreached("park_any2"); return 0; }
void lv_task_spawn(void* a, void* b, void* c){ (void)a;(void)b;(void)c; spike_unreached("task_spawn"); }
int64_t lv_task_spawn_registered(void* a, void* b, void* c){ (void)a;(void)b;(void)c; spike_unreached("task_spawn_registered"); return -1; }
int  lv_task_cancel(int64_t id){ (void)id; spike_unreached("task_cancel"); return 0; }
void lv_task_shield(int d){ (void)d; spike_unreached("task_shield"); }

/* ================= TLS provider (lv_tls_none.c not in the spike) =========
 * lv_runtime.c's socket/TLS natives reference these; no socket exists in a
 * pure-compute corpus, so nothing reaches them. The query trio returns "no"
 * defensively; actions abort.                                              */
int  lv_tls_is(int fd){ (void)fd; return 0; }
int  lv_tls_pending(int fd){ (void)fd; return 0; }
int  lv_tls_wants(int fd){ (void)fd; return 0; }
int  lv_tls_client_start(int fd, const char* h, const char* a, const char* ca, int v){ (void)fd;(void)h;(void)a;(void)ca;(void)v; spike_unreached("tls_client_start"); return -1; }
int  lv_tls_server_start(int fd, const char* c, const char* k, const char* a){ (void)fd;(void)c;(void)k;(void)a; spike_unreached("tls_server_start"); return -1; }
int  lv_tls_handshake(int fd){ (void)fd; spike_unreached("tls_handshake"); return -1; }
int64_t lv_tls_send(int fd, const void* b, int64_t n){ (void)fd;(void)b;(void)n; spike_unreached("tls_send"); return -1; }
int64_t lv_tls_recv(int fd, void* b, int64_t n){ (void)fd;(void)b;(void)n; spike_unreached("tls_recv"); return -1; }
int  lv_tls_close(int fd){ (void)fd; return 0; }
const char* lv_tls_error(int fd){ (void)fd; return ""; }
const char* lv_tls_alpn(int fd){ (void)fd; return ""; }
const char* lv_tls_version(int fd){ (void)fd; return ""; }
/* real signature: (fd, const void* in, int64_t n, int pad, void* out, int64_t cap) -> int64_t */
int64_t lv_rsa_encrypt(int fd, const void* in, int64_t n, int pad, void* out, int64_t cap){ (void)fd;(void)in;(void)n;(void)pad;(void)out;(void)cap; spike_unreached("rsa_encrypt"); return -1; }
