/* Track W — the wasm-browser platform floor (W-M1, doc 03).
 *
 * ONE new file implementing the `lv_plat.h` interface against host imports
 * ("the ONLY place the runtime touches an OS", lv_plat.h:1-4 — for wasm, the
 * "OS" is whatever JS host instantiates the module: a browser page via
 * lv_host.js, or the headless Node driver tests/wasm_node_run.mjs reuses it
 * from). Every import lives in ONE module, `"lv"` (techdesign-03-floor-wasm.md
 * §1) — lv_host.js is the only place those names are spelled.
 *
 * Absent-capability functions return the documented lv_plat.h error
 * convention and NEVER abort here: the compile-time capability gate
 * (hard-03-capability-gate.md) owns loud failure for a *reachable* gated
 * native; an *unreachable* prelude body that still calls one of these gets
 * redirected to `lvrt_unsupported` by that same gate, never to a real floor
 * call. This file stays a quiet error-returner, mirroring how lv_plat_win32.c
 * returns -1 for POSIX-only holes.
 *
 * `getenv` is defined here (not left to a libc): the runtime archive is built
 * against wasi-libc for its non-syscall pieces (malloc, the mem family, the
 * str family, snprintf — doc 02 §6's "the bulk is lv_plat_wasm.c" turned out
 * to also mean "the bulk is not reinventing musl"), but wasi-libc's real
 * `getenv` pulls in the WASI environ syscalls, and lv_runtime.c/lv_task.c
 * call `getenv` unconditionally
 * on hot paths (LANG_ARC_TRACE, LANG_THREAD_STATS, LANG_TASK_STATS, ...) —
 * every wasm build would otherwise import `wasi_snapshot_preview1.environ_*`
 * for a feature that means nothing in a browser. This definition, linked
 * ahead of the archive's wasi-libc members, satisfies the symbol without
 * ever pulling that machinery in. Always "unset" — there is no process
 * environment on this target.
 */
#include "lv_abi.h"
#include "lv_plat.h"

#include <stdint.h>
#include <string.h>

/* ========================== the region allocator ===========================
 * doc 03 §4: the §15 allocator (ARC + arena + free-list) runs unmodified over
 * whatever region this returns; growth is append-only, the "mmap-more" model
 * it already expects. unmap is a no-op — the allocator owns reuse, never the
 * floor.
 *
 * Page-granular grow from the CURRENT memory end, not a private bump cursor
 * (the W-M1 shape): the archive links wasi-libc, whose dlmalloc grows this
 * same linear memory through sbrk — also memory.grow, also from the end. The
 * W-M1 cursor tracked its own g_bump from __heap_base, so once BOTH
 * allocators had grown the memory it could hand out a window that overlapped
 * a block sbrk had already given malloc (doc 04 makes malloc traffic real:
 * pooled task stacks, task records). Two growers that each take
 * [end, end + pages) are disjoint by construction, whatever the interleave;
 * mmap on the POSIX floor is page-granular too, so callers see the same
 * rounding they always did. Fresh pages are spec-zeroed — no memset. */
void* lv_plat_map(int64_t size) {
    if (size <= 0) return 0;
    uintptr_t pages = ((uintptr_t)size + 65535u) / 65536u;
    uintptr_t prev = __builtin_wasm_memory_grow(0, pages);
    if (prev == (uintptr_t)-1) return 0;
    return (void*)(prev * 65536u);
}
void lv_plat_unmap(void* base, int64_t size) { (void)base; (void)size; }

/* ================================ imports =================================
 * The declared subset lv_host.js supplies (techdesign-03-floor-wasm.md §3).
 * Kept smaller than the floor surface on purpose — most of lv_plat.h below
 * is either pure C (map/unmap) or an absent-capability stub; only real
 * browser-host interaction crosses the import boundary. */
__attribute__((import_module("lv"), import_name("write")))
extern int32_t lv_import_write(int32_t fd, const void* ptr, int32_t len);
__attribute__((import_module("lv"), import_name("now_ms")))
extern double lv_import_now_ms(void);
__attribute__((import_module("lv"), import_name("now_ns")))
extern double lv_import_now_ns(void);
__attribute__((import_module("lv"), import_name("random")))
extern void lv_import_random(void* ptr, int32_t len);
__attribute__((import_module("lv"), import_name("exit")))
extern void lv_import_exit(int32_t code);

/* ---- B-M1 slice ---- */

int64_t lv_plat_write(int fd, const void* buf, int64_t n) {
    return (int64_t)lv_import_write(fd, buf, (int32_t)n);
}
int64_t lv_plat_read(int fd, void* buf, int64_t n) {
    /* doc 03 §2: no stdin in a browser — always EOF. */
    (void)fd; (void)buf; (void)n;
    return 0;
}
int64_t lv_plat_now_ns(void) {
    /* performance.now() is fractional milliseconds; convert to ns in the
     * host (double math) rather than truncating here. */
    return (int64_t)lv_import_now_ns();
}
int64_t lv_plat_now_realtime_ms(void) {
    return (int64_t)lv_import_now_ms();
}
void lv_plat_exit(int code) {
    /* lv.exit never returns on the host side (it throws a sentinel the
     * driver catches); __builtin_trap is the unreachable backstop if a host
     * ever violates that contract, matching lv_plat.h's "void, never returns
     * in practice" callers assume. */
    lv_import_exit(code);
    __builtin_trap();
}

/* ---- B-M3 slice: absent capabilities (doc 00 §3 "Lost" row) --------------
 * Filesystem, process, raw sockets, tty, signals: all gated at compile time
 * for code reachable from user programs (hard-03); these bodies exist only
 * so prelude wrappers that ARE built for wasm (but never called for a
 * reachable path) still link. Error convention matches lv_plat_win32.c's
 * POSIX-only holes. */
int lv_plat_open(const char* path, int bits, int mode) { (void)path; (void)bits; (void)mode; return -1; }
void lv_plat_close(int fd) { (void)fd; }
int lv_plat_term_raw(int fd) { (void)fd; return -1; }
int lv_plat_term_restore(int fd) { (void)fd; return 0; }
int lv_plat_term_size(int fd, int* rows, int* cols) { (void)fd; (void)rows; (void)cols; return -1; }
int lv_plat_term_israw(void) { return 0; }
int lv_plat_signal_open(const int* sigs, int n) { (void)sigs; (void)n; return -1; }
int lv_plat_signal_next(int fd) { (void)fd; return -1; }
void lv_plat_signal_close(int fd) { (void)fd; }
int64_t lv_plat_stat_size(const char* path) { (void)path; return -1; }
int64_t lv_plat_stat_mtime(const char* path) { (void)path; return -1; }
int lv_plat_stat_isdir(const char* path) { (void)path; return -1; }
int lv_plat_mkdir(const char* path) { (void)path; return -1; }
int lv_plat_remove(const char* path) { (void)path; return -1; }
int lv_plat_rename(const char* from, const char* to) { (void)from; (void)to; return -1; }
int lv_plat_listdir(const char* path, LvDirEntries* out) {
    (void)path;
    if (out) { out->names = NULL; out->count = 0; }
    return -1;
}
void lv_plat_listdir_free(LvDirEntries* entries) {
    if (entries) { entries->names = NULL; entries->count = 0; }
}
int lv_plat_tcp_connect(const char* ip, int port) { (void)ip; (void)port; return -1; }
int lv_plat_tcp_listen(const char* ip, int port, int backlog, int reuse_port) {
    (void)ip; (void)port; (void)backlog; (void)reuse_port; return -1;
}
int lv_plat_cpu_count(void) { return 1; }   /* single-threaded v1 (doc 02 §3) */
int lv_plat_accept(int fd) { (void)fd; return -1; }
int64_t lv_plat_send(int fd, const void* buf, int64_t n) { (void)fd; (void)buf; (void)n; return -2; }
int64_t lv_plat_recv(int fd, void* buf, int64_t n) { (void)fd; (void)buf; (void)n; return -1; }
int lv_plat_sock_buffer(int fd, int64_t send_bytes, int64_t recv_bytes) {
    (void)fd; (void)send_bytes; (void)recv_bytes; return -1;
}
void lv_plat_set_nonblock(int fd) { (void)fd; }

int64_t lv_plat_random(void* buf, int64_t n) {
    lv_import_random(buf, (int32_t)n);
    return n;
}

int lv_plat_tcp_connect_nb(const char* ip, int port) { (void)ip; (void)port; return -1; }
int lv_plat_connect_result(int fd) { (void)fd; return -1; }

int lv_plat_spawn(const char* path, char* const argv[], int fds[3]) {
    (void)path; (void)argv; (void)fds; return -1;
}
int lv_plat_pidfd_open(int pid) { (void)pid; return -1; }
int lv_plat_reap(int pid) { (void)pid; return -1; }
int lv_plat_kill(int pid, int sig) { (void)pid; (void)sig; return -1; }

int lv_plat_poll(LvPollFd* fds, int nfds, int timeout_ms) {
    /* doc 03 §2: v1 has no async yet (no sockets/timers reach here without
     * being gated first), so a synchronous "nothing ready" is the only
     * correct answer — never blocks on wasm's single thread. */
    (void)fds; (void)nfds; (void)timeout_ms;
    return 0;
}

/* Always "unset" — see the file header. */
char* getenv(const char* name) { (void)name; return 0; }
