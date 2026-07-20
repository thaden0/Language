/* lv_pty.c — pty natives (G-LANG-2 terminal half, designs/pty/ 02 §2).
 * Thin LvValue marshaling over lv_plat_pty_spawn/resize; ALL policy lives in
 * the plat floor + the prelude (Pty/TcpStream in Resolver.cpp). Mirrors the
 * sysPtySpawn oracle. Ownership pattern: lv_loop.c sockets, lv_thread.c
 * threads, lv_proc.c processes, THIS TU ptys (D-P7). */
#include "lv_abi.h"
#include "lv_plat.h"
#include <stdlib.h>

/* the sanctioned inline 16-byte element store (lv_proc.c precedent, R7) */
static void lv_pty_st_val(int64_t payload, int64_t off, const LvValue* v) {
    int64_t* p = (int64_t*)((uint8_t*)(intptr_t)payload + off);
    p[0] = v->tag; p[1] = v->payload;
}

void lvrt_sysptyspawn(LvValue* out, const LvValue* path, const LvValue* args,
                      const LvValue* rows, const LvValue* cols, const LvValue* flags) {
    /* empty result = spawn failure (frozen: [] not None) */
    const char* cpath = (const char*)(intptr_t)(path->payload + 8);
    int64_t plen = *(const int64_t*)(intptr_t)path->payload;
    if (plen <= 0 || rows->payload <= 0 || cols->payload <= 0) {
        lvrt_arr_new(out, 0); return;
    }
    /* argv built BEFORE fork (D-P2): [path, args..., NULL]; string bytes are
     * NUL-terminated in the rep — alias, never copy (lv_proc.c idiom). */
    int64_t n = 0;
    if (args->tag == LV_ARR) {
        n = *(const int64_t*)(intptr_t)args->payload;
        if (n < 0) n = 0;      /* dense/columnar marker — Array<string> is boxed */
    }
    char** argv = (char**)malloc((size_t)(n + 2) * sizeof(char*));
    if (!argv) { lvrt_arr_new(out, 0); return; }
    argv[0] = (char*)cpath;
    for (int64_t i = 0; i < n; i++) {
        int64_t sp = *(const int64_t*)(intptr_t)(args->payload + 8 + 16 * i + 8);
        argv[i + 1] = (char*)(intptr_t)(sp + 8);
    }
    argv[n + 1] = NULL;

    int master = -1;
    int pid = lv_plat_pty_spawn(cpath, argv, (int)rows->payload,
                                (int)cols->payload, (int)flags->payload, &master);
    free(argv);                /* parent-side, post-fork */
    if (pid <= 0) { lvrt_arr_new(out, 0); return; }

    lvrt_arr_new(out, 2);      /* rc 0; codegen retainDst()s (sysSpawn parity) */
    LvValue e; e.tag = LV_INT;
    e.payload = pid;    lv_pty_st_val(out->payload, 8 + 16 * 0, &e);
    e.payload = master; lv_pty_st_val(out->payload, 8 + 16 * 1, &e);
}

void lvrt_sysptyresize(LvValue* out, const LvValue* master,
                       const LvValue* rows, const LvValue* cols) {
    out->tag = LV_INT;
    out->payload = lv_plat_pty_resize((int)master->payload,
                                      (int)rows->payload, (int)cols->payload);
}
