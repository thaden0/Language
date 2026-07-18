/* lv_proc.c — process natives (G-LANG-2, techdesign-spawn-llvm.md §4).
 * Thin LvValue marshaling over lv_plat_spawn/pidfd_open/reap/kill; ALL policy
 * lives in the plat floor + the prelude (Process/TcpStream in Resolver.cpp).
 * Mirrors src/RuntimeNatives.cpp:1841-1933 (the interpreter oracle).
 * Ownership pattern: lv_loop.c owns socket natives, lv_thread.c owns thread
 * natives, this TU owns process natives (D2). */
#include "lv_abi.h"
#include "lv_plat.h"
#include <stdlib.h>

/* lv_runtime.c's lv_st_val is file-static (by design); this is the same
 * two-word element store written inline (techdesign-spawn-llvm.md §4.2 note —
 * a 16-byte {tag,payload} write, nothing divergent to drift). */
static void lv_proc_st_val(int64_t payload, int64_t off, const LvValue* v) {
    int64_t* p = (int64_t*)((uint8_t*)(intptr_t)payload + off);
    p[0] = v->tag; p[1] = v->payload;
}

void lvrt_sysspawn(LvValue* out, const LvValue* path, const LvValue* args) {
    /* empty result = spawn failure (frozen: [] not None) */
    const char* cpath = (const char*)(intptr_t)(path->payload + 8);
    int64_t plen = *(const int64_t*)(intptr_t)path->payload;
    if (plen <= 0) { lvrt_arr_new(out, 0); return; }

    /* argv built BEFORE fork (D7): [path, args..., NULL]; element bytes are
     * NUL-terminated in the string rep (lv_abi.h §strings), so we alias the
     * LvValue strings directly — never copy. */
    int64_t n = 0;
    if (args->tag == LV_ARR) {
        n = *(const int64_t*)(intptr_t)args->payload;
        if (n < 0) n = 0;   /* dense/columnar marker bits — Array<string> is boxed */
    }
    char** argv = (char**)malloc((size_t)(n + 2) * sizeof(char*));
    if (!argv) { lvrt_arr_new(out, 0); return; }
    argv[0] = (char*)cpath;
    for (int64_t i = 0; i < n; i++) {
        /* boxed-array element i: payload word at +8 + 16*i + 8 (the
         * lvrt_systaskjoinall read idiom, lv_loop.c) */
        int64_t sp = *(const int64_t*)(intptr_t)(args->payload + 8 + 16 * i + 8);
        argv[i + 1] = (char*)(intptr_t)(sp + 8);
    }
    argv[n + 1] = NULL;

    int fds[3];
    int pid = lv_plat_spawn(cpath, argv, fds);
    free(argv);   /* parent-side, post-fork — the child never saw allocator state */
    if (pid <= 0) { lvrt_arr_new(out, 0); return; }

    lvrt_arr_new(out, 4);              /* rc 0; codegen retainDst()s (D1, sysArgs parity) */
    LvValue e; e.tag = LV_INT;
    e.payload = pid;    lv_proc_st_val(out->payload, 8 + 16 * 0, &e);
    e.payload = fds[0]; lv_proc_st_val(out->payload, 8 + 16 * 1, &e);
    e.payload = fds[1]; lv_proc_st_val(out->payload, 8 + 16 * 2, &e);
    e.payload = fds[2]; lv_proc_st_val(out->payload, 8 + 16 * 3, &e);
}

void lvrt_syspidfdopen(LvValue* out, const LvValue* pid) {
    out->tag = LV_INT; out->payload = lv_plat_pidfd_open((int)pid->payload);
}

void lvrt_sysreap(LvValue* out, const LvValue* pid) {
    out->tag = LV_INT; out->payload = lv_plat_reap((int)pid->payload);
}

void lvrt_syskill(LvValue* out, const LvValue* pid, const LvValue* sig) {
    out->tag = LV_INT; out->payload = lv_plat_kill((int)pid->payload, (int)sig->payload);
}
