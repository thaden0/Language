/* Track 10 — runtime-internal thread interface (techdesign-threads-3 §4/§5).
 *
 * NOT part of lv_abi.h: generated code never calls these — only the runtime's
 * own files do (lv_runtime.c defines the flatten/rebuild engine and counters;
 * lv_thread.c, M3c, defines the pthread spawn/trampoline/join/reap and calls
 * flatten/rebuild across the boundary). Same discipline as lv_rt_dispatch_fn.
 */
#ifndef LV_THREAD_H
#define LV_THREAD_H

#include "lv_abi.h"
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The C flatten/rebuild engine (lv_runtime.c). flatten returns a fresh,
 * self-contained, thread-portable malloc buffer (accounted in transfers_out) or
 * NULL with *errOut set on a non-flattenable node (§6). rebuild walks a buffer
 * into the CALLING thread's heap, returning the root at +1 (the transfer
 * contract). free_buf releases a buffer and decrements the counter. */
uint8_t* lv_thread_flatten(const LvValue* v, int64_t* sizeOut, char* errOut, int errCap);
void     lv_thread_rebuild(LvValue* out, const uint8_t* buf);
void     lv_thread_free_buf(uint8_t* buf);

/* §4.2 process-global transfer counters (diagnostics; C11 atomics). */
extern atomic_llong lv_thread_transfers_out;   /* buffers malloc'd, not yet freed */
extern atomic_llong lv_thread_spawns;
extern atomic_llong lv_thread_reaps;

/* §5.4: initialize ONLY this thread's TLS runtime state (allocator heap+arena,
 * freelist, accounting, throw state) — never the process-globals (argv, class
 * table). A worker bootstrap calls this, NOT lv_rt_init (F-i: lv_rt_init clobbers
 * g_argc/g_argv). Records the mmap'd region bases for reap-time munmap. */
void lv_thread_ctx_init(uint8_t** heapBaseOut, uint8_t** arenaBaseOut);
/* munmap a reaped worker's heap+arena regions (the exact sizes lv_thread_ctx_init
 * mapped). Called on the spawner thread AFTER the result is rebuilt out (§5.3). */
void lv_thread_ctx_unmap(uint8_t* heapBase, uint8_t* arenaBase);

/* Cancel every watch on `fd` (lv_loop.c) — sysThreadResult retires its own join
 * watch before closing+reusing the eventfd, so a stale watch never fires on the
 * next worker's reused fd number (§5.3; POLLNVAL/F-ii alone loses that race). */
void lvrt_loop_cancel_fd(int fd);

#ifdef __cplusplus
}
#endif

#endif /* LV_THREAD_H */
