/* Track B — runtime core: allocator, ARC, strings, collections, objects,
 * dispatch, exceptions. See docs/techdesign-portable-backend-2.md §3
 * (B-M1). Every fact this file encodes as behavior (rc transitions, heap
 * layouts, COW rules, ownership contracts) was extracted from the frozen
 * src/X64Gen.{hpp,cpp} reference — never invented — per the doc's mandate;
 * deviations are called out inline and in the implementation log (§10).
 *
 * No OS calls happen in this file — everything routes through lv_plat.h
 * (checked by the B-M1 acceptance grep over mmap/munmap/open/read/write/
 * socket/poll/clock_gettime).
 */
#include "lv_abi.h"
#include "lv_plat.h"
#include "lv_thread.h"   /* Track 10: flatten/rebuild + transfer-counter contract */
#include "lv_tls.h"      /* LA-2: TLS/crypto provider seam (wrap-in-place routing) */
#include "lv_task.h"     /* LA-30 doc 2: lvrt_await's park seam (lv_task_park_on) */

#include <errno.h>    /* strtoll/strtod overflow detection (lvrt_str_toint/tofloat) */
#include <math.h>     /* fmod — float % (libm; the CMake target links m) */
#include <stdatomic.h> /* Track 10 §4.2: the process-global transfer counters */
#include <stdint.h>
#include <stdio.h>    /* snprintf only — pure formatting, no buffered IO (B-H4) */
#include <string.h>
#include <stdlib.h>

/* ========================================================================
 * Small pointer/field helpers. `payload` fields are always addresses
 * stored as int64_t (never dereferenced without going through these).
 * ==================================================================== */

#define P8(x)  ((uint8_t*)(intptr_t)(x))
#define I64(x) ((int64_t)(intptr_t)(x))

/* the 16-byte LvHeader sits immediately before every counted payload */
#define HDR(payload) ((int64_t*)(P8(payload) - 16))   /* [0]=rc [1]=meta */

static int64_t lv_ld_i64(int64_t payload, int64_t off) {
    return *(int64_t*)(P8(payload) + off);
}
static void lv_st_i64(int64_t payload, int64_t off, int64_t v) {
    *(int64_t*)(P8(payload) + off) = v;
}
static LvValue lv_ld_val(int64_t payload, int64_t off) {
    int64_t* p = (int64_t*)(P8(payload) + off);
    LvValue v; v.tag = p[0]; v.payload = p[1];
    return v;
}
static void lv_st_val(int64_t payload, int64_t off, const LvValue* v) {
    int64_t* p = (int64_t*)(P8(payload) + off);
    p[0] = v->tag; p[1] = v->payload;
}

static void lv_die(const char* msg) {
    lv_plat_term_restore(0);   /* §3.4: restore the terminal before the error + exit */
    lv_plat_write(2, msg, (int64_t)strlen(msg));
    lv_plat_exit(1);
}

/* ========================================================================
 * §3 step 2 — Allocator: regions, registry, size-class free lists, arena
 * save/restore, live/peak meters.
 *
 * B-M1 keeps one fixed-size mmap per tier (matches X64Gen's own single
 * 128 MiB heap/arena mmaps exactly — src/X64Gen.cpp:3778-3791). Growing
 * to multiple regions per tier is a straightforward future extension
 * (append to a region list) that isn't needed for correctness now; logged
 * as a known limitation rather than built speculatively.
 * ==================================================================== */

#define LV_HEAP_BYTES  (256 * 1024 * 1024)
#define LV_ARENA_BYTES (64 * 1024 * 1024)
#define LV_SIZE_CLASSES 28   /* block size 16 << c, c in [0,27]: 2^4 .. 2^31 */

/* the dense-array marker is bit 63 of the length field (§2.4). Shifting a
 * literal `1` into the sign bit of a signed int64 is undefined behavior
 * (caught by UBSan) — build it as unsigned, then reinterpret. Defined here (not
 * beside the array cores) because the Track 10 flatten engine above them reads
 * it too. */
#define LV_DENSE_BIT_U ((uint64_t)1 << 63)
#define LV_DENSE_BIT   ((int64_t)LV_DENSE_BIT_U)

/* columnar (struct-of-arrays) marker: bit 62 IN ADDITION to bit 63
 * (techdesign-columnar-arrays.md §4.1). A "not boxed" word (bit 63 set, i.e.
 * rawlen < 0) is COLUMNAR when bit 62 is also set, else DENSE (row-major). The
 * clean length masks off BOTH top bits. Row-major dense buffers keep bit 62
 * clear, so LV_ARR_CLEANLEN reduces to the old `& ~LV_DENSE_BIT` for them and
 * the existing dense paths stay bit-for-bit unchanged. */
#define LV_COL_BIT_U   ((uint64_t)1 << 62)
#define LV_COL_BIT     ((int64_t)LV_COL_BIT_U)
#define LV_ARR_TOPBITS_U (LV_DENSE_BIT_U | LV_COL_BIT_U)   /* 3ULL << 62 */
#define LV_ARR_CLEANLEN(w) ((int64_t)((uint64_t)(w) & ~LV_ARR_TOPBITS_U))
#define LV_IS_COLUMNAR(w)  (((w) < 0) && (((uint64_t)(w) & LV_COL_BIT_U) != 0))

static LV_TLS uint8_t* g_heap_base;
static LV_TLS uint8_t* g_heap_cursor;
static LV_TLS uint8_t* g_heap_end;
static LV_TLS uint8_t* g_arena_base;
LV_TLS uint8_t* lv_g_arena_cursor;
static LV_TLS uint8_t* g_arena_end;
static LV_TLS void*    g_freelist[LV_SIZE_CLASSES];
static LV_TLS int64_t  g_live_bytes;
static LV_TLS int64_t  g_peak_bytes;

static int lv_in_region(const void* p) {
    const uint8_t* u = (const uint8_t*)p;
    if (u >= g_heap_base && u < g_heap_end) return 1;
    if (u >= g_arena_base && u < g_arena_end) return 1;
    return 0;
}

/* rounds `n` up to its power-of-two class (16 << c); mirrors X64Gen's
 * genAlloc/genHfree rounding loop (src/X64Gen.cpp:96-102) exactly,
 * including the cap at class 27 for a garbage/huge size. */
static int lv_size_class(int64_t n, int64_t* blockSizeOut) {
    int64_t sz = 16; int c = 0;
    while (sz < n && c < LV_SIZE_CLASSES - 1) { sz <<= 1; c++; }
    *blockSizeOut = sz;
    return c;
}

/* Lazy self-initialization (ruled at the 2026-07-05 maintenance pass):
 * until Track A's A-M5 flips program entry to the runtime-owned main(),
 * LlvmGen emits its own main() and nothing calls lv_rt_init — the first
 * allocation would die on a NULL heap. The guard costs one predictable
 * branch per allocation and disappears as a concern once A-M5 lands
 * (lv_rt_init stays the explicit entry: argv capture needs it). */
static void lv_ensure_init(void) {
    if (!g_heap_base) lv_rt_init(0, NULL);
}

static void* lv_alloc_heap(int64_t reqSize) {
    lv_ensure_init();
    int64_t blockSize;
    int c = lv_size_class(reqSize, &blockSize);
    void* p = g_freelist[c];
    if (p) {
        g_freelist[c] = *(void**)p;
    } else {
        if (g_heap_cursor + blockSize > g_heap_end) lv_die("lvrt: heap exhausted\n");
        p = g_heap_cursor;
        g_heap_cursor += blockSize;
    }
    memset(p, 0, (size_t)blockSize);   /* callers rely on zeroed memory (mkobj slots, etc.) */
    g_live_bytes += blockSize;
    if (g_live_bytes > g_peak_bytes) g_peak_bytes = g_live_bytes;
    return p;
}

static void* lv_alloc_arena(int64_t reqSize) {
    lv_ensure_init();
    if (lv_g_arena_cursor + reqSize > g_arena_end) lv_die("lvrt: arena exhausted\n");
    void* p = lv_g_arena_cursor;
    lv_g_arena_cursor += reqSize;
    return p;
}

void* lvrt_halloc(int64_t size, int tier) {
    return tier == LV_TIER_ARENA ? lv_alloc_arena(size) : lv_alloc_heap(size);
}

/* the hfree equivalent: only heap-tier blocks are pooled; arena/garbage
 * pointers are silently ignored (matches genHfree's skipLo/skipHi guard) */
static void lv_free_raw(void* raw, int64_t reqSize) {
    if (!((uint8_t*)raw >= g_heap_base && (uint8_t*)raw < g_heap_end)) return;
    int64_t blockSize;
    int c = lv_size_class(reqSize, &blockSize);
    g_live_bytes -= blockSize;
    *(void**)raw = g_freelist[c];
    g_freelist[c] = raw;
    if (blockSize > (int64_t)sizeof(void*))
        memset((uint8_t*)raw + sizeof(void*), 0xFE, (size_t)(blockSize - (int64_t)sizeof(void*)));
}

/* allocates a fresh HEAP-tier block of `bodySize` bytes plus a 16-byte ARC
 * header, writes rc=0 (fresh/unowned) and meta=totalSize, returns the
 * payload pointer (raw+16) — the shared core of every heap-tier
 * constructor (object/array/map/closure/string). Mirrors X64Gen's
 * genHalloc (src/X64Gen.cpp:471-483). */
static int64_t lv_halloc_prefixed(int64_t bodySize) {
    int64_t total = bodySize + 16;
    uint8_t* raw = (uint8_t*)lv_alloc_heap(total);
    int64_t* hdr = (int64_t*)raw;
    hdr[0] = 0;       /* fresh/unowned */
    hdr[1] = total;
    return I64(raw + 16);
}

int64_t lvrt_arena_save(void) { lv_ensure_init(); return I64(lv_g_arena_cursor); }
void lvrt_arena_restore(int64_t mark) { lv_g_arena_cursor = P8(mark); }
int64_t lvrt_live_bytes(void) { return g_live_bytes; }
int64_t lvrt_peak_bytes(void) { return g_peak_bytes; }

/* ========================================================================
 * §3 step 3 — ARC: retain/release/recursive-free, debug trace hook.
 * ==================================================================== */

static LV_TLS int g_arc_trace = -1; /* -1 = not yet checked (LANG_ARC_TRACE) */

static void lv_trace(char op, int64_t payload, int64_t count) {
    if (g_arc_trace < 0) g_arc_trace = getenv("LANG_ARC_TRACE") ? 1 : 0;
    if (!g_arc_trace) return;
    int64_t rec[3]; rec[0] = (int64_t)(unsigned char)op; rec[1] = payload; rec[2] = count;
    lv_plat_write(2, rec, (int64_t)sizeof rec);
}

/* is this (tag, payload) a counted heap value at all? Gates BOTH retain and
 * release identically (extracted from genRetain/genRelease's shared
 * pre-header-read checks, src/X64Gen.cpp:557-627), generalized per doc-2
 * §2.5 to range-check every counted tag (4/5/6/7/9) via the region
 * registry, not just strings as X64Gen does — a defensive strengthening
 * the doc explicitly calls for. */
static int lv_is_counted(int64_t tag, int64_t payload) {
    if (tag < LV_STR || tag == LV_NONE) return 0;              /* scalars, none */
    /* LV_CHAR (10) is an immediate — falls through here (not in the list) and
     * returns 0, so retain/release no-op on it exactly like INT/BOOL. */
    if (tag != LV_STR && tag != LV_OBJ && tag != LV_ARR && tag != LV_MAP &&
        tag != LV_CLO && tag != LV_BLOCK && tag != LV_WEAK_PROXY)
        return 0;
    /* Region check MUST come before any dereference of `payload` — it is
     * the only thing standing between this function and a garbage/stack
     * pointer (caught by ASan: an earlier version read the tag-5 classId
     * before confirming the pointer was runtime-owned at all). Only pure
     * pointer-value comparisons happen before this line. */
    if (!lv_in_region(P8(payload))) return 0;                   /* literal or out-of-region */
    if (tag == LV_OBJ && lvrt_isvalueclass(lv_ld_i64(payload, 0)))
        return 0;                                               /* value struct / dense record */
    return 1;
}

static void lv_recursive_free(int64_t tag, int64_t payload);

/* F5: per-thread target -> proxy table. M1 found that header meta contains
 * unrounded requested sizes, so no low flag bit is available. The ratified
 * fallback probes this compact table only while it is non-empty. Removal is
 * swap-delete: no tombstones accumulate and component-scale tables stay tiny. */
typedef struct LvWeakEntry { int64_t target; int64_t proxy; } LvWeakEntry;
static LV_TLS LvWeakEntry* lv_weak_entries;
static LV_TLS int64_t lv_weak_count;
static LV_TLS int64_t lv_weak_cap;

static int64_t lv_weak_find(int64_t target) {
    for (int64_t i = 0; i < lv_weak_count; ++i)
        if (lv_weak_entries[i].target == target) return i;
    return -1;
}
static void lv_weak_remove_at(int64_t i) {
    if (i < 0 || i >= lv_weak_count) return;
    lv_weak_entries[i] = lv_weak_entries[--lv_weak_count];
    if (!lv_weak_count) {
        free(lv_weak_entries);
        lv_weak_entries = NULL;
        lv_weak_cap = 0;
    }
}
static void lv_weak_invalidate(int64_t target) {
    if (!lv_weak_count) return;
    int64_t i = lv_weak_find(target);
    if (i < 0) return;
    int64_t proxy = lv_weak_entries[i].proxy;
    LvValue none = {LV_NONE, 0};
    lv_st_val(proxy, 0, &none);             /* strictly before child walk/poison */
    lv_weak_remove_at(i);
}

void lvrt_retain(const LvValue* v) {
    if (!lv_is_counted(v->tag, v->payload)) return;
    int64_t* hdr = HDR(v->payload);
    if (hdr[0] < 0) return;   /* arena sentinel */
    hdr[0] += 1;
    lv_trace('R', v->payload, hdr[0]);
}

void lvrt_release(const LvValue* v) {
    if (!lv_is_counted(v->tag, v->payload)) return;
    int64_t* hdr = HDR(v->payload);
    if (hdr[0] <= 0) return;   /* arena sentinel OR fresh/unowned: no-op (extracted fact,
                                   not a bug — see lv_abi.h's LvHeader.rc note) */
    hdr[0] -= 1;
    lv_trace('r', v->payload, hdr[0]);
    if (hdr[0] == 0) lv_recursive_free(v->tag, v->payload);
}

void lvrt_vfree(const LvValue* v) {
    if (v->tag != LV_OBJ) return;
    int64_t classId = lv_ld_i64(v->payload, 0);
    if (!lvrt_isvalueclass(classId)) return;
    int64_t rc = HDR(v->payload)[0];
    if (rc != 0) return;   /* arena (-1) or, in principle, counted: never for a value struct */
    int64_t n = lvrt_fieldcount(classId);
    for (int64_t i = 0; i < n; i++) {
        LvValue field = lv_ld_val(v->payload, 16 + 16 * i);
        /* Release a reference-typed field's +1 (the matching drop for the retain
         * lvrt_copyval / the constructor's RawSet gave it) — no-op on immediates
         * and on nested value structs, which the recursive vfree below reclaims
         * instead (bug #49). Without this, every value struct with a counted
         * reference field leaked that field, and a struct-valued collection's
         * copies never balanced their shared referents' counts. */
        lvrt_release(&field);
        lvrt_vfree(&field);   /* self-recursive over nested value-struct fields */
    }
    lv_free_raw(P8(v->payload) - 16, HDR(v->payload)[1]);
}

static void lv_recursive_free(int64_t tag, int64_t payload) {
    if (tag != LV_WEAK_PROXY) lv_weak_invalidate(payload);
    if (tag == LV_WEAK_PROXY) {
        LvValue target = lv_ld_val(payload, 0);
        if (target.tag != LV_NONE) {
            int64_t i = lv_weak_find(target.payload);
            if (i >= 0 && lv_weak_entries[i].proxy == payload) lv_weak_remove_at(i);
        }
        lv_free_raw(P8(payload) - 16, HDR(payload)[1]);
    } else if (tag == LV_OBJ) {
        int64_t classId = lv_ld_i64(payload, 0);
        int64_t n = lvrt_fieldcount(classId);
        for (int64_t i = 0; i < n; i++) {
            LvValue field = lv_ld_val(payload, 16 + 16 * i);
            lvrt_release(&field);   /* frees a reference field whose rc hits 0 */
            lvrt_vfree(&field);     /* reclaims a value-struct field (release() no-ops on it) */
        }
        /* dyn-list fallback fields (§2.4's "raw-node fallback list", offset 8)
         * own their value exactly like a static slot — found 2026-07-06
         * hardening the throw/unwind path: this walk was missing, so any
         * object field reachable only via the dyn-list (e.g. a class
         * registered with fewer static slots than fields it's given, the
         * shape RuntimeException uses when the class table omits `message`)
         * leaked both its held value's refcount and the 32-byte node itself.
         * LV_CLO below already does this correctly for captureHead; objects
         * were the asymmetric case. */
        int64_t node = lv_ld_i64(payload, 8);
        while (node) {
            int64_t next = lv_ld_i64(node, 0);
            LvValue val = lv_ld_val(node, 16);
            lvrt_release(&val);
            lvrt_vfree(&val);
            lv_free_raw(P8(node), 32);
            node = next;
        }
        lv_free_raw(P8(payload) - 16, HDR(payload)[1]);
    } else if (tag == LV_CLO) {
        int64_t node = lv_ld_i64(payload, 8);   /* captureHead */
        while (node) {
            int64_t next = lv_ld_i64(node, 0);
            LvValue val = lv_ld_val(node, 16);
            lvrt_release(&val);
            lv_free_raw(P8(node), 32);          /* raw node, no ARC header */
            node = next;
        }
        lv_free_raw(P8(payload) - 16, HDR(payload)[1]);
    } else if (tag == LV_ARR) {
        int64_t rawlen = lv_ld_i64(payload, 0);
        if (rawlen < 0) {
            /* dense OR columnar: both hold only value-struct scalars, never
             * refcounted (§2.4, techdesign-columnar §6.2). Bit 62 need not be
             * distinguished — either way it's one buffer, no per-element walk. */
            lv_free_raw(P8(payload) - 16, HDR(payload)[1]);
        } else {
            int64_t capacity = HDR(payload)[1];   /* meta = capacity for boxed arrays */
            for (int64_t i = 0; i < rawlen; i++) {
                LvValue elem = lv_ld_val(payload, 8 + 16 * i);
                lvrt_release(&elem);
                lvrt_vfree(&elem);
            }
            lv_free_raw(P8(payload) - 16, 24 + 16 * capacity);
        }
    } else if (tag == LV_MAP) {
        int64_t len = lv_ld_i64(payload, 0);
        for (int64_t i = 0; i < len; i++) {
            LvValue key = lv_ld_val(payload, 8 + 32 * i);
            LvValue val = lv_ld_val(payload, 8 + 32 * i + 16);
            lvrt_release(&key);          /* keys can't be value structs; no vfree needed */
            lvrt_release(&val);
            lvrt_vfree(&val);
        }
        lv_free_raw(P8(payload) - 16, HDR(payload)[1]);
    } else if (tag == LV_BLOCK) {
        /* Track 03 §3: a slice (parentPtr!=0) aliases the root's shared bytes —
         * release the root (its rc, transitively, owns the buffer); never free
         * dataPtr here. A root (parentPtr==0) owns dataPtr — free it. Then free
         * this block's own 48-byte alloc either way. */
        int64_t parent = lv_ld_i64(payload, 0);
        if (parent) {
            LvValue root; root.tag = LV_BLOCK; root.payload = parent;
            lvrt_release(&root);
        } else {
            int64_t data = lv_ld_i64(payload, 24);
            lv_free_raw(P8(data) - 16, HDR(data)[1]);
        }
        lv_free_raw(P8(payload) - 16, HDR(payload)[1]);
    } else {
        /* string: no nested references */
        lv_free_raw(P8(payload) - 16, HDR(payload)[1]);
    }
}

/* ========================================================================
 * §2.6 — class registry, objects, dynamic dispatch.
 * ==================================================================== */

static const LvClassInfo* g_classes;
static int64_t g_nclasses;
static LvDispatchFn g_dispatch;

void lvrt_register(const LvClassInfo* classes, int64_t nclasses, LvDispatchFn dispatch) {
    g_classes = classes;
    g_nclasses = nclasses;
    g_dispatch = dispatch;
}

/* Runtime-internal bridge for lv_loop.c (a separate translation unit) to
 * invoke a stored closure's body — NOT part of lv_abi.h: generated code
 * never calls this, only our own runtime files do. g_dispatch itself stays
 * static/private; this is the one accessor, per B-H6's "isolate in one
 * function" spirit. */
LvDispatchFn lv_rt_dispatch_fn(void) { return g_dispatch; }

static const LvClassInfo* lv_find_class(int64_t classId) {
    for (int64_t i = 0; i < g_nclasses; i++)
        if (g_classes[i].classId == classId) return &g_classes[i];
    return NULL;
}

static const LvClassInfo* lv_find_class_by_name(const char* name) {
    for (int64_t i = 0; i < g_nclasses; i++)
        if (strcmp(g_classes[i].name, name) == 0) return &g_classes[i];
    return NULL;
}

int32_t lvrt_isvalueclass(int64_t classId) {
    const LvClassInfo* c = lv_find_class(classId);
    return c ? c->isValue : 0;
}

int64_t lvrt_fieldcount(int64_t classId) {
    const LvClassInfo* c = lv_find_class(classId);
    return c ? c->nslots : 0;
}

/* pointer-first, content-fallback comparison (ruled 2026-07-05 — see
 * lv_abi.h's key-comparison note). X64Gen compares pure pointers because
 * one compiler emits both sides through one intern cache; a program-
 * agnostic runtime can't assume codegen deduped its key globals against
 * the class table's slotNames, so a missed dedup must degrade to a slow
 * lookup, never a silent field miss. */
static int lv_key_eq(const char* a, const char* b) {
    return a == b || strcmp(a, b) == 0;
}

int64_t lvrt_fieldindex(int64_t classId, const char* key) {
    const LvClassInfo* c = lv_find_class(classId);
    if (!c) return -1;
    for (int64_t i = 0; i < c->nslots; i++)
        if (lv_key_eq(c->slotNames[i], key)) return i;
    return -1;
}

static void* lv_alloc_packed(int64_t word0, int64_t nslots) {
    int64_t payload = lv_halloc_prefixed(16 + 16 * nslots);
    lv_st_i64(payload, 0, word0);
    lv_st_i64(payload, 8, 0);   /* dyn / captureHead list, empty */
    return P8(payload);
}

void lvrt_obj_new(LvValue* out, int64_t classId) {
    int64_t n = lvrt_fieldcount(classId);
    void* p = lv_alloc_packed(classId, n);
    out->tag = LV_OBJ;
    out->payload = I64(p);
}

void lvrt_closure_new(LvValue* out, int64_t fnIndex) {
    void* p = lv_alloc_packed(fnIndex, 0);
    out->tag = LV_CLO;
    out->payload = I64(p);
}

/* dyn-list walk shared by object fields and closure captures — both use
 * the identical node shape (§2.4). */
static int lv_dyn_get(int64_t headPayload, int64_t headOffset, const char* key, LvValue* out) {
    int64_t node = lv_ld_i64(headPayload, headOffset);
    while (node) {
        if (lv_key_eq((const char*)(intptr_t)lv_ld_i64(node, 8), key)) {
            *out = lv_ld_val(node, 16);
            return 1;
        }
        node = lv_ld_i64(node, 0);
    }
    return 0;
}
static void lv_dyn_set(int64_t headPayload, int64_t headOffset, const char* key, const LvValue* val) {
    int64_t* head = (int64_t*)(P8(headPayload) + headOffset);
    int64_t node = *head;
    while (node) {
        if (lv_key_eq((const char*)(intptr_t)lv_ld_i64(node, 8), key)) {
            lv_st_val(node, 16, val);
            return;
        }
        node = lv_ld_i64(node, 0);
    }
    int64_t* nn = (int64_t*)lvrt_halloc(32, LV_TIER_HEAP);   /* raw, no ARC header */
    nn[0] = *head; nn[1] = I64(key); nn[2] = val->tag; nn[3] = val->payload;
    *head = I64(nn);
}

void lvrt_getfield(LvValue* out, const LvValue* obj, const char* key) {
    int64_t classId = lv_ld_i64(obj->payload, 0);
    int64_t idx = lvrt_fieldindex(classId, key);
    if (idx >= 0) { *out = lv_ld_val(obj->payload, 16 + 16 * idx); return; }
    if (!lv_dyn_get(obj->payload, 8, key, out)) { out->tag = LV_VOID; out->payload = 0; }
}

void lvrt_setfield(const LvValue* obj, const char* key, const LvValue* val) {
    int64_t classId = lv_ld_i64(obj->payload, 0);
    int64_t idx = lvrt_fieldindex(classId, key);
    if (idx >= 0) { lv_st_val(obj->payload, 16 + 16 * idx, val); return; }
    lv_dyn_set(obj->payload, 8, key, val);
}

void lvrt_weak_load(LvValue* out, const LvValue* slot) {
    if (slot->tag != LV_WEAK_PROXY) { out->tag = LV_NONE; out->payload = 0; return; }
    *out = lv_ld_val(slot->payload, 0);
    if (out->tag == LV_NONE) return;
    lvrt_retain(out);                         /* a live read is an ordinary +1 value */
}

void lvrt_weak_store(LvValue* slot, const LvValue* val) {
    LvValue old = *slot;
    slot->tag = LV_NONE; slot->payload = 0;
    lvrt_release(&old);
    if (val->tag == LV_NONE || val->tag == LV_VOID || !val->payload) return;
    int64_t i = lv_weak_find(val->payload);
    int64_t proxy;
    if (i >= 0) proxy = lv_weak_entries[i].proxy;
    else {
        proxy = lv_halloc_prefixed(16);
        lv_st_val(proxy, 0, val);              /* deliberately NOT retained */
        if (lv_weak_count == lv_weak_cap) {
            int64_t cap = lv_weak_cap ? lv_weak_cap * 2 : 16;
            LvWeakEntry* p = (LvWeakEntry*)realloc(lv_weak_entries,
                                                    (size_t)cap * sizeof(LvWeakEntry));
            if (!p) lv_die("lvrt: weak table allocation failed\n");
            lv_weak_entries = p; lv_weak_cap = cap;
        }
        lv_weak_entries[lv_weak_count++] = (LvWeakEntry){val->payload, proxy};
    }
    slot->tag = LV_WEAK_PROXY; slot->payload = proxy;
    lvrt_retain(slot);                         /* the weak slot owns the proxy */
}

void lvrt_weak_getfield(LvValue* out, const LvValue* obj, const char* key) {
    int64_t idx = lvrt_fieldindex(lv_ld_i64(obj->payload, 0), key);
    if (idx < 0) { out->tag = LV_NONE; out->payload = 0; return; }
    LvValue slot = lv_ld_val(obj->payload, 16 + 16 * idx);
    lvrt_weak_load(out, &slot);
}

void lvrt_weak_setfield(const LvValue* obj, const char* key, const LvValue* val) {
    int64_t idx = lvrt_fieldindex(lv_ld_i64(obj->payload, 0), key);
    if (idx < 0) return;                       /* declared-slot feature only */
    LvValue* slot = (LvValue*)(void*)(P8(obj->payload) + 16 + 16 * idx);
    lvrt_weak_store(slot, val);
}

void lvrt_capture_get(LvValue* out, const LvValue* clo, const char* key) {
    if (!lv_dyn_get(clo->payload, 8, key, out)) { out->tag = LV_VOID; out->payload = 0; }
}
void lvrt_capture_set(const LvValue* clo, const char* key, const LvValue* val) {
    lv_dyn_set(clo->payload, 8, key, val);
    lvrt_retain(val);   /* the capture node owns the value (X64Gen CaptureVar, src/X64Gen.cpp:3473-3479) */
}

void lvrt_copyval(LvValue* out, const LvValue* in) {
    if (in->tag != LV_OBJ || !lvrt_isvalueclass(lv_ld_i64(in->payload, 0))) {
        *out = *in;
        return;
    }
    int64_t classId = lv_ld_i64(in->payload, 0);
    LvValue dst; lvrt_obj_new(&dst, classId);
    int64_t n = lvrt_fieldcount(classId);
    for (int64_t i = 0; i < n; i++) {
        LvValue srcField = lv_ld_val(in->payload, 16 + 16 * i);
        LvValue dstField; lvrt_copyval(&dstField, &srcField);
        /* A value struct has VALUE semantics (each copy is independent), but a
         * reference-typed field (string/array/map/object/closure) is SHARED by
         * reference: the copy must independently own its +1 so the two copies
         * cannot free the shared referent out from under each other (bug #49 —
         * a struct-valued Map field grew a copy whose string/array field was
         * only borrowed, so a later free of one copy read the other's dangling
         * referent). retain no-ops on immediates and on nested value structs
         * (which lvrt_copyval already deep-copied above), so it fires only on a
         * genuine counted reference. lvrt_vfree performs the matching release. */
        lvrt_retain(&dstField);
        lv_st_val(dst.payload, 16 + 16 * i, &dstField);
    }
    *out = dst;
}

/* ========================================================================
 * Track 10 M3b — the C flatten/rebuild engine (techdesign-threads-3 §4).
 *
 * One engine serves every crossing (spawn capture, join result, worker
 * exception, channel message). Flatten walks the source value graph into a
 * self-contained relocatable buffer whose internal references are all
 * BUFFER-RELATIVE OFFSETS (so the buffer survives being handed to another
 * thread and its own realloc); rebuild is the inverse walk, allocating every
 * node through the RECEIVING thread's lvrt_halloc so each rebuilt node is a
 * first-class counted value in that thread's region (§4.3). A seen-map keyed
 * on source-payload identity makes shared substructure and cycles flatten once
 * and rebuild shared (a diamond in -> a diamond out; the walk terminates on
 * cycles). The per-tag layout mirrors lv_recursive_free's walk exactly
 * (lv_runtime.c above) — that function is the layout oracle, this is its
 * non-destructive twin. Values that cannot cross in v1 (§6) abort the flatten
 * with an error whose text is BYTE-IDENTICAL to the interpreters' lvThreadCopy,
 * so the differential holds on error paths too.
 *
 * Buffer format (§4.1):
 *   header:  int64 totalBytes | int64 rootTag | int64 rootPayloadOrOffset
 *   record*  (each): int64 tag | int64 size | payload (per-tag, refs as offsets)
 * A "flattened LvValue" (FV) is 16 bytes {tag, x}: for an immediate x is the
 * raw payload; for a heap node x is the buffer offset of that node's record.
 * ==================================================================== */

/* §4.2: process-global transfer counters (diagnostics off the hot path).
 * transfers_out = malloc'd transfer buffers not yet freed (must return to 0);
 * spawns/reaps are wired in lv_thread.c (M3c). C11 atomics so they are correct
 * once real worker threads increment them concurrently. */
atomic_llong lv_thread_transfers_out = 0;
atomic_llong lv_thread_spawns = 0;
atomic_llong lv_thread_reaps = 0;

/* header words {totalBytes, rootTag, rootPayloadOrOffset} + one reserved word so
 * the first record starts 16-byte aligned (records are 16-byte aligned, §4.1, so
 * every int64/FV read within one is naturally aligned — UBSan-clean). */
#define LV_FLAT_HEADER 32

typedef struct LvFlatBuf {
    uint8_t* data;
    int64_t  size;      /* bytes used */
    int64_t  cap;       /* bytes allocated */
    int      err;       /* set on a non-flattenable node */
    char     errmsg[160];
} LvFlatBuf;

/* seen-map: source payload -> record offset. Open-addressing hash so an
 * adversarial wide graph (M6) stays near-linear, not O(n^2). */
typedef struct LvFlatSeen {
    int64_t* srcs;      /* source payloads (0 = empty slot) */
    int64_t* offs;      /* record offsets */
    int64_t  cap, n;
} LvFlatSeen;

static void lv_seen_init(LvFlatSeen* s) {
    s->cap = 64; s->n = 0;
    s->srcs = calloc((size_t)s->cap, sizeof *s->srcs);
    s->offs = calloc((size_t)s->cap, sizeof *s->offs);
}
static void lv_seen_free(LvFlatSeen* s) { free(s->srcs); free(s->offs); }

static int lv_seen_get(LvFlatSeen* s, int64_t src, int64_t* offOut) {
    uint64_t h = (uint64_t)src * 1099511628211ull;
    for (int64_t i = 0; i < s->cap; i++) {
        int64_t idx = (int64_t)((h + (uint64_t)i) & (uint64_t)(s->cap - 1));
        if (s->srcs[idx] == 0) return 0;
        if (s->srcs[idx] == src) { *offOut = s->offs[idx]; return 1; }
    }
    return 0;
}
static void lv_seen_put(LvFlatSeen* s, int64_t src, int64_t off) {
    if ((s->n + 1) * 4 >= s->cap * 3) {   /* grow at 75% load */
        int64_t oc = s->cap; int64_t* os = s->srcs; int64_t* oo = s->offs;
        s->cap *= 2;
        s->srcs = calloc((size_t)s->cap, sizeof *s->srcs);
        s->offs = calloc((size_t)s->cap, sizeof *s->offs);
        s->n = 0;
        for (int64_t i = 0; i < oc; i++) if (os[i]) lv_seen_put(s, os[i], oo[i]);
        free(os); free(oo);
    }
    uint64_t h = (uint64_t)src * 1099511628211ull;
    for (int64_t i = 0; i < s->cap; i++) {
        int64_t idx = (int64_t)((h + (uint64_t)i) & (uint64_t)(s->cap - 1));
        if (s->srcs[idx] == 0) { s->srcs[idx] = src; s->offs[idx] = off; s->n++; return; }
    }
}

static void lv_flat_ensure(LvFlatBuf* b, int64_t need) {
    if (b->size + need <= b->cap) return;
    int64_t nc = b->cap ? b->cap : 256;
    while (nc < b->size + need) nc *= 2;
    b->data = realloc(b->data, (size_t)nc);
    b->cap = nc;
}
static int64_t lv_flat_alloc(LvFlatBuf* b, int64_t bytes) {   /* returns record offset */
    bytes = (bytes + 15) & ~(int64_t)15;    /* 16-byte align each record (§4.1) */
    lv_flat_ensure(b, bytes);
    int64_t off = b->size;
    memset(b->data + off, 0, (size_t)bytes);
    b->size += bytes;
    return off;
}
static void lv_flat_put_fv(LvFlatBuf* b, int64_t off, LvValue fv) {
    int64_t* p = (int64_t*)(void*)(b->data + off);
    p[0] = fv.tag; p[1] = fv.payload;
}

static void lv_flat_fail(LvFlatBuf* b, const char* msg) {
    if (!b->err) { b->err = 1; snprintf(b->errmsg, sizeof b->errmsg, "%s", msg); }
}
static void lv_flat_fail_type(LvFlatBuf* b, int64_t classId, const char* suffix) {
    if (b->err) return;
    const LvClassInfo* c = lv_find_class(classId);
    b->err = 1;
    snprintf(b->errmsg, sizeof b->errmsg,
             "value of type %s cannot cross a thread boundary (v1)%s",
             c ? c->name : "?", suffix);
}

/* is classId derived from (or equal to) the named built-in class? */
static int lv_class_is(int64_t classId, const char* name) {
    const LvClassInfo* target = lv_find_class_by_name(name);
    return target ? lvrt_issub(classId, target->classId) : 0;
}

/* An FV: {tag, x} where x is an immediate payload (immediate tags) or a record
 * offset (heap tags). Returns an FV; sets b->err on a non-flattenable node. */
static LvValue lv_flatten(LvFlatBuf* b, LvFlatSeen* seen, const LvValue* v, int isRoot) {
    LvValue fv; fv.tag = v->tag; fv.payload = v->payload;
    if (b->err) return fv;
    switch (v->tag) {
        case LV_VOID: case LV_INT: case LV_FLOAT: case LV_BOOL:
        case LV_NONE: case LV_CHAR:
            return fv;                               /* immediate: value rides inline */
        case LV_WEAK_PROXY:
            fv.tag = LV_NONE; fv.payload = 0;         /* weak slots do not cross copies */
            return fv;
        case LV_BLOCK:
            lv_flat_fail(b, "a Block cannot cross a thread boundary (v1)");
            return fv;
        case LV_STR: {
            int64_t off;
            if (lv_seen_get(seen, v->payload, &off)) { fv.payload = off; return fv; }
            int64_t len = lv_ld_i64(v->payload, 0);
            int64_t recOff = lv_flat_alloc(b, 16 + 8 + len);
            lv_seen_put(seen, v->payload, recOff);
            int64_t* h = (int64_t*)(void*)(b->data + recOff);
            h[0] = LV_STR; h[1] = 16 + 8 + len; h[2] = len;
            memcpy(b->data + recOff + 24, P8(v->payload) + 8, (size_t)len);
            fv.payload = recOff;
            return fv;
        }
        case LV_OBJ: {
            int64_t classId = lv_ld_i64(v->payload, 0);
            if (lv_class_is(classId, "Promise")) {   /* A-1: Worker/Promise handle */
                lv_flat_fail_type(b, classId, "; pass a Channel");
                return fv;
            }
            if (lv_class_is(classId, "TcpStream")   || lv_class_is(classId, "TcpListener") ||
                lv_class_is(classId, "Timer")        || lv_class_is(classId, "Process") ||
                lv_class_is(classId, "TcpConnector")) {
                lv_flat_fail_type(b, classId, "");    /* fd-/loop-bound carrier (§6) */
                return fv;
            }
            /* bug #81 (SU-1): an InStream is loop-bound only when it carries a
             * producer teardown (hasDispose == true — the signal::on
             * subscription; its onDispose reaches a live signalfd + loop watch).
             * A plain in-memory InStream (hasDispose == false) must still cross,
             * so this is a per-object field test, not a name-keyed reject. */
            if (lv_class_is(classId, "InStream")) {
                LvValue hd; lvrt_getfield(&hd, v, "hasDispose");
                if (hd.tag == LV_BOOL && hd.payload) {
                    lv_flat_fail_type(b, classId, "");
                    return fv;
                }
            }
            int64_t off;
            if (lv_seen_get(seen, v->payload, &off)) { fv.payload = off; return fv; }
            int64_t nslots = lvrt_fieldcount(classId);
            int64_t nDyn = 0;
            for (int64_t node = lv_ld_i64(v->payload, 8); node; node = lv_ld_i64(node, 0)) nDyn++;
            int64_t recBytes = 16 + 8 + 8 + nslots * 16 + 8 + nDyn * 24;
            int64_t recOff = lv_flat_alloc(b, recBytes);
            lv_seen_put(seen, v->payload, recOff);
            { int64_t* h = (int64_t*)(void*)(b->data + recOff);
              h[0] = LV_OBJ; h[1] = recBytes; h[2] = classId; h[3] = nslots; }
            /* static slots */
            for (int64_t i = 0; i < nslots; i++) {
                LvValue slot = lv_ld_val(v->payload, 16 + 16 * i);
                LvValue sf = lv_flatten(b, seen, &slot, 0);
                if (b->err) return fv;
                lv_flat_put_fv(b, recOff + 32 + i * 16, sf);
            }
            /* dyn-list: (interned keyPtr, value FV) pairs — the key char* is a
             * process-global interned pointer (code/rodata, valid on any thread),
             * so it crosses verbatim exactly like a closure fn index (§4.1). */
            int64_t dynBase = recOff + 32 + nslots * 16;
            *(int64_t*)(void*)(b->data + dynBase) = nDyn;
            int64_t di = 0;
            for (int64_t node = lv_ld_i64(v->payload, 8); node; node = lv_ld_i64(node, 0)) {
                int64_t keyPtr = lv_ld_i64(node, 8);
                LvValue dval = lv_ld_val(node, 16);
                LvValue df = lv_flatten(b, seen, &dval, 0);
                if (b->err) return fv;
                int64_t pairOff = dynBase + 8 + di * 24;
                *(int64_t*)(void*)(b->data + pairOff) = keyPtr;
                lv_flat_put_fv(b, pairOff + 8, df);
                di++;
            }
            fv.payload = recOff;
            return fv;
        }
        case LV_ARR: {
            int64_t off;
            if (lv_seen_get(seen, v->payload, &off)) { fv.payload = off; return fv; }
            int64_t rawlen = lv_ld_i64(v->payload, 0);
            if (LV_IS_COLUMNAR(rawlen)) {             /* columnar: scalars-only blob (§6.4) */
                int64_t cleanLen = LV_ARR_CLEANLEN(rawlen);
                int64_t classId = lv_ld_i64(v->payload, 8);
                int64_t dataBytes = lvrt_fieldcount(classId) * 8 * cleanLen;
                int64_t recOff = lv_flat_alloc(b, 16 + 8 + 8 + dataBytes);
                lv_seen_put(seen, v->payload, recOff);
                int64_t* h = (int64_t*)(void*)(b->data + recOff);
                h[0] = LV_ARR; h[1] = 16 + 8 + 8 + dataBytes;
                h[2] = rawlen; h[3] = classId;        /* h[2] carries BOTH top bits */
                memcpy(b->data + recOff + 32, P8(v->payload) + 16, (size_t)dataBytes);
                fv.payload = recOff;
                return fv;
            }
            if (rawlen < 0) {                         /* dense: value-struct records, no refs */
                int64_t cleanLen = rawlen & ~LV_DENSE_BIT;
                int64_t recBytesElem = lv_ld_i64(v->payload, 8);
                int64_t dataBytes = cleanLen * recBytesElem;
                int64_t recOff = lv_flat_alloc(b, 16 + 8 + 8 + dataBytes);
                lv_seen_put(seen, v->payload, recOff);
                int64_t* h = (int64_t*)(void*)(b->data + recOff);
                h[0] = LV_ARR; h[1] = 16 + 8 + 8 + dataBytes;
                h[2] = cleanLen | LV_DENSE_BIT; h[3] = recBytesElem;
                memcpy(b->data + recOff + 32, P8(v->payload) + 16, (size_t)dataBytes);
                fv.payload = recOff;
                return fv;
            }
            int64_t recBytes = 16 + 8 + rawlen * 16;
            int64_t recOff = lv_flat_alloc(b, recBytes);
            lv_seen_put(seen, v->payload, recOff);
            { int64_t* h = (int64_t*)(void*)(b->data + recOff);
              h[0] = LV_ARR; h[1] = recBytes; h[2] = rawlen; }
            for (int64_t i = 0; i < rawlen; i++) {
                LvValue el = lv_ld_val(v->payload, 8 + 16 * i);
                LvValue ef = lv_flatten(b, seen, &el, 0);
                if (b->err) return fv;
                lv_flat_put_fv(b, recOff + 24 + i * 16, ef);
            }
            fv.payload = recOff;
            return fv;
        }
        case LV_MAP: {
            int64_t off;
            if (lv_seen_get(seen, v->payload, &off)) { fv.payload = off; return fv; }
            int64_t len = lv_ld_i64(v->payload, 0);
            int64_t recBytes = 16 + 8 + len * 32;
            int64_t recOff = lv_flat_alloc(b, recBytes);
            lv_seen_put(seen, v->payload, recOff);
            { int64_t* h = (int64_t*)(void*)(b->data + recOff);
              h[0] = LV_MAP; h[1] = recBytes; h[2] = len; }
            for (int64_t i = 0; i < len; i++) {
                LvValue k = lv_ld_val(v->payload, 8 + 32 * i);
                LvValue val = lv_ld_val(v->payload, 8 + 32 * i + 16);
                LvValue kf = lv_flatten(b, seen, &k, 0);
                if (b->err) return fv;
                LvValue vf = lv_flatten(b, seen, &val, 0);
                if (b->err) return fv;
                lv_flat_put_fv(b, recOff + 24 + i * 32, kf);
                lv_flat_put_fv(b, recOff + 24 + i * 32 + 16, vf);
            }
            fv.payload = recOff;
            return fv;
        }
        case LV_CLO: {
            if (!isRoot) {                            /* only the spawn body may cross (§6) */
                lv_flat_fail(b, "a closure cannot cross a thread boundary (v1)");
                return fv;
            }
            int64_t off;
            if (lv_seen_get(seen, v->payload, &off)) { fv.payload = off; return fv; }
            int64_t fnIndex = lv_ld_i64(v->payload, 0);
            int64_t nCap = 0;
            for (int64_t node = lv_ld_i64(v->payload, 8); node; node = lv_ld_i64(node, 0)) nCap++;
            int64_t recBytes = 16 + 8 + 8 + nCap * 24;
            int64_t recOff = lv_flat_alloc(b, recBytes);
            lv_seen_put(seen, v->payload, recOff);
            { int64_t* h = (int64_t*)(void*)(b->data + recOff);
              h[0] = LV_CLO; h[1] = recBytes; h[2] = fnIndex; h[3] = nCap; }
            int64_t ci = 0;
            for (int64_t node = lv_ld_i64(v->payload, 8); node; node = lv_ld_i64(node, 0)) {
                int64_t keyPtr = lv_ld_i64(node, 8);
                LvValue cval = lv_ld_val(node, 16);
                LvValue cf = lv_flatten(b, seen, &cval, 0);
                if (b->err) return fv;
                int64_t pairOff = recOff + 32 + ci * 24;
                *(int64_t*)(void*)(b->data + pairOff) = keyPtr;
                lv_flat_put_fv(b, pairOff + 8, cf);
                ci++;
            }
            fv.payload = recOff;
            return fv;
        }
        default:
            lv_flat_fail(b, "value cannot cross a thread boundary (v1)");
            return fv;
    }
}

/* ========================================================================
 * bug #35 — spawn-body global Promise guard (reject route A).
 *
 * A std::spawn body may reference a Promise-derived object through a bare
 * GLOBAL (rather than a captured local). lv_flatten above only walks the body
 * closure's capture list, so a global reference never reaches its A-1 reject —
 * on LLVM the worker reads lv_globals in the shared address space and silently
 * drains. lvrt_value_has_promise re-applies the SAME reject to a global's
 * CURRENT value, read-only (no copy), reusing lv_flatten's container recursion
 * and cycle-safe seen-map.
 *
 * Scoped to Promise-derived objects reached through object static/dyn fields,
 * array elements, and map entries — the #35 shape. Channel portals are
 * relocatable (shared) and never rejected. Closures / Blocks / fd-carriers in
 * a global are OUT OF SCOPE: unlike a captured value they are read from the
 * shared address space, not flattened, so they are not the #35 defect (and
 * this matches the interpreters' depth — nested lambdas of the body are
 * walked, a called top-level function's body is not). Returns a thread-local
 * message string on the first hit (byte-identical to the capture reject), else
 * NULL. ================================================================= */
static LV_TLS char g_promise_scan_msg[160];
static const char* lv_scan_promise(const LvValue* v, LvFlatSeen* seen) {
    switch (v->tag) {
        case LV_OBJ: {
            int64_t classId = lv_ld_i64(v->payload, 0);
            if (lv_class_is(classId, "Channel")) return NULL;   /* portal: relocatable */
            if (lv_class_is(classId, "Promise")) {              /* A-1: Worker/Promise handle */
                const LvClassInfo* c = lv_find_class(classId);
                snprintf(g_promise_scan_msg, sizeof g_promise_scan_msg,
                         "value of type %s cannot cross a thread boundary (v1); pass a Channel",
                         c ? c->name : "?");
                return g_promise_scan_msg;
            }
            int64_t off;
            if (lv_seen_get(seen, v->payload, &off)) return NULL;
            lv_seen_put(seen, v->payload, 1);
            int64_t nslots = lvrt_fieldcount(classId);
            for (int64_t i = 0; i < nslots; i++) {
                LvValue slot = lv_ld_val(v->payload, 16 + 16 * i);
                const char* r = lv_scan_promise(&slot, seen);
                if (r) return r;
            }
            for (int64_t node = lv_ld_i64(v->payload, 8); node; node = lv_ld_i64(node, 0)) {
                LvValue dval = lv_ld_val(node, 16);
                const char* r = lv_scan_promise(&dval, seen);
                if (r) return r;
            }
            return NULL;
        }
        case LV_ARR: {
            int64_t off;
            if (lv_seen_get(seen, v->payload, &off)) return NULL;
            lv_seen_put(seen, v->payload, 1);
            int64_t rawlen = lv_ld_i64(v->payload, 0);
            if (rawlen < 0) return NULL;   /* dense/columnar: scalar structs, no refs */
            for (int64_t i = 0; i < rawlen; i++) {
                LvValue el = lv_ld_val(v->payload, 8 + 16 * i);
                const char* r = lv_scan_promise(&el, seen);
                if (r) return r;
            }
            return NULL;
        }
        case LV_MAP: {
            int64_t off;
            if (lv_seen_get(seen, v->payload, &off)) return NULL;
            lv_seen_put(seen, v->payload, 1);
            int64_t len = lv_ld_i64(v->payload, 0);
            for (int64_t i = 0; i < len; i++) {
                LvValue k   = lv_ld_val(v->payload, 8 + 32 * i);
                LvValue val = lv_ld_val(v->payload, 8 + 32 * i + 16);
                const char* r = lv_scan_promise(&k, seen);
                if (r) return r;
                r = lv_scan_promise(&val, seen);
                if (r) return r;
            }
            return NULL;
        }
        default:
            return NULL;   /* immediates, strings, closures, blocks: out of scope */
    }
}
const char* lvrt_value_has_promise(const LvValue* v) {
    if (!v || (v->tag != LV_OBJ && v->tag != LV_ARR && v->tag != LV_MAP)) return NULL;
    LvFlatSeen seen; lv_seen_init(&seen);
    const char* r = lv_scan_promise(v, &seen);
    lv_seen_free(&seen);
    return r;
}

/* bug #35: the codegen-emitted spawn-body global-Promise checker
 * (LlvmGen's lv_spawn_global_check) registers here so lv_thread.c's spawn path
 * can consult it BEFORE flattening. Weak/optional: NULL until a generated
 * program registers one (runtime_selftest never does, so the check no-ops
 * there). Same private-accessor discipline as lv_rt_dispatch_fn. */
static LvSpawnCheckFn g_spawn_check;
void lvrt_register_spawn_check(LvSpawnCheckFn fn) { g_spawn_check = fn; }
LvSpawnCheckFn lv_rt_spawn_check_fn(void) { return g_spawn_check; }

/* rebuild-seen: record offset -> already-rebuilt node (tag, payload). */
typedef struct LvRebSeen { int64_t* offs; LvValue* vals; int64_t cap, n; } LvRebSeen;
static void lv_reb_init(LvRebSeen* s) {
    s->cap = 64; s->n = 0;
    s->offs = calloc((size_t)s->cap, sizeof *s->offs);
    s->vals = calloc((size_t)s->cap, sizeof *s->vals);
    for (int64_t i = 0; i < s->cap; i++) s->offs[i] = -1;
}
static void lv_reb_free(LvRebSeen* s) { free(s->offs); free(s->vals); }
static int lv_reb_get(LvRebSeen* s, int64_t off, LvValue* out) {
    uint64_t h = (uint64_t)off * 1099511628211ull;
    for (int64_t i = 0; i < s->cap; i++) {
        int64_t idx = (int64_t)((h + (uint64_t)i) & (uint64_t)(s->cap - 1));
        if (s->offs[idx] == -1) return 0;
        if (s->offs[idx] == off) { *out = s->vals[idx]; return 1; }
    }
    return 0;
}
static void lv_reb_put(LvRebSeen* s, int64_t off, LvValue v) {
    if ((s->n + 1) * 4 >= s->cap * 3) {
        int64_t oc = s->cap; int64_t* oo = s->offs; LvValue* ov = s->vals;
        s->cap *= 2;
        s->offs = calloc((size_t)s->cap, sizeof *s->offs);
        s->vals = calloc((size_t)s->cap, sizeof *s->vals);
        for (int64_t i = 0; i < s->cap; i++) s->offs[i] = -1;
        s->n = 0;
        for (int64_t i = 0; i < oc; i++) if (oo[i] != -1) lv_reb_put(s, oo[i], ov[i]);
        free(oo); free(ov);
    }
    uint64_t h = (uint64_t)off * 1099511628211ull;
    for (int64_t i = 0; i < s->cap; i++) {
        int64_t idx = (int64_t)((h + (uint64_t)i) & (uint64_t)(s->cap - 1));
        if (s->offs[idx] == -1) { s->offs[idx] = off; s->vals[idx] = v; s->n++; return; }
    }
}

/* rebuild an FV into the CALLING thread's heap. Immediates ride inline; a heap
 * FV names a record offset. Each node is created at rc=0 and the STORING parent
 * retains it (so a node's rc equals its reference count); the top-level caller
 * retains the root once for the transfer's +1 (§4.3). */
static LvValue lv_rebuild(const uint8_t* buf, LvRebSeen* seen, LvValue fv);

static void lv_rebuild_store(const uint8_t* buf, LvRebSeen* seen,
                             LvValue childFv, int64_t dstPayload, int64_t dstOff) {
    LvValue child = lv_rebuild(buf, seen, childFv);
    lv_st_val(dstPayload, dstOff, &child);
    lvrt_retain(&child);   /* the parent slot owns its value */
}

static LvValue lv_rebuild(const uint8_t* buf, LvRebSeen* seen, LvValue fv) {
    switch (fv.tag) {
        case LV_VOID: case LV_INT: case LV_FLOAT: case LV_BOOL:
        case LV_NONE: case LV_CHAR:
            return fv;                               /* immediate */
        default: break;
    }
    int64_t recOff = fv.payload;
    LvValue cached;
    if (lv_reb_get(seen, recOff, &cached)) return cached;   /* shared/cyclic: one node */
    const int64_t* h = (const int64_t*)(const void*)(buf + recOff);
    int64_t tag = h[0];
    LvValue out; out.tag = LV_VOID; out.payload = 0;
    switch (tag) {
        case LV_STR: {
            int64_t len = h[2];
            lvrt_str_new(&out, (const char*)(buf + recOff + 24), len);
            lv_reb_put(seen, recOff, out);
            return out;
        }
        case LV_OBJ: {
            int64_t classId = h[2], nslots = h[3];
            lvrt_obj_new(&out, classId);
            lv_reb_put(seen, recOff, out);           /* register BEFORE children (cycles) */
            for (int64_t i = 0; i < nslots; i++) {
                LvValue sf; const int64_t* p = (const int64_t*)(const void*)(buf + recOff + 32 + i * 16);
                sf.tag = p[0]; sf.payload = p[1];
                lv_rebuild_store(buf, seen, sf, out.payload, 16 + 16 * i);
            }
            int64_t dynBase = recOff + 32 + nslots * 16;
            int64_t nDyn = *(const int64_t*)(const void*)(buf + dynBase);
            for (int64_t di = 0; di < nDyn; di++) {
                int64_t pairOff = dynBase + 8 + di * 24;
                int64_t keyPtr = *(const int64_t*)(const void*)(buf + pairOff);
                LvValue df; const int64_t* p = (const int64_t*)(const void*)(buf + pairOff + 8);
                df.tag = p[0]; df.payload = p[1];
                LvValue dv = lv_rebuild(buf, seen, df);
                lvrt_setfield(&out, (const char*)(intptr_t)keyPtr, &dv);
                lvrt_retain(&dv);                    /* the dyn node owns its value */
            }
            return out;
        }
        case LV_ARR: {
            int64_t rawlen = h[2];
            if (LV_IS_COLUMNAR(rawlen)) {             /* columnar: scalars-only blob (§6.4) */
                int64_t cleanLen = LV_ARR_CLEANLEN(rawlen);
                int64_t classId = h[3];
                int64_t dataBytes = lvrt_fieldcount(classId) * 8 * cleanLen;
                int64_t payload = lv_halloc_prefixed(16 + dataBytes);
                lv_st_i64(payload, 0, rawlen);        /* both top bits ride through */
                lv_st_i64(payload, 8, classId);
                memcpy(P8(payload) + 16, buf + recOff + 32, (size_t)dataBytes);
                out.tag = LV_ARR; out.payload = payload;
                lv_reb_put(seen, recOff, out);
                return out;
            }
            if (rawlen < 0) {                         /* dense */
                int64_t cleanLen = rawlen & ~LV_DENSE_BIT;
                int64_t recBytesElem = h[3];
                int64_t dataBytes = cleanLen * recBytesElem;
                int64_t payload = lv_halloc_prefixed(16 + dataBytes);
                lv_st_i64(payload, 0, cleanLen | LV_DENSE_BIT);
                lv_st_i64(payload, 8, recBytesElem);
                memcpy(P8(payload) + 16, buf + recOff + 32, (size_t)dataBytes);
                out.tag = LV_ARR; out.payload = payload;
                lv_reb_put(seen, recOff, out);
                return out;
            }
            lvrt_arr_new(&out, rawlen);
            lv_reb_put(seen, recOff, out);
            for (int64_t i = 0; i < rawlen; i++) {
                LvValue ef; const int64_t* p = (const int64_t*)(const void*)(buf + recOff + 24 + i * 16);
                ef.tag = p[0]; ef.payload = p[1];
                lv_rebuild_store(buf, seen, ef, out.payload, 8 + 16 * i);
            }
            return out;
        }
        case LV_MAP: {
            int64_t len = h[2];
            lvrt_map_new(&out, len);
            lv_reb_put(seen, recOff, out);
            for (int64_t i = 0; i < len; i++) {
                const int64_t* pk = (const int64_t*)(const void*)(buf + recOff + 24 + i * 32);
                const int64_t* pv = (const int64_t*)(const void*)(buf + recOff + 24 + i * 32 + 16);
                LvValue kf; kf.tag = pk[0]; kf.payload = pk[1];
                LvValue vf; vf.tag = pv[0]; vf.payload = pv[1];
                lv_rebuild_store(buf, seen, kf, out.payload, 8 + 32 * i);
                lv_rebuild_store(buf, seen, vf, out.payload, 8 + 32 * i + 16);
            }
            return out;
        }
        case LV_CLO: {
            int64_t fnIndex = h[2], nCap = h[3];
            lvrt_closure_new(&out, fnIndex);
            lv_reb_put(seen, recOff, out);
            for (int64_t ci = 0; ci < nCap; ci++) {
                int64_t pairOff = recOff + 32 + ci * 24;
                int64_t keyPtr = *(const int64_t*)(const void*)(buf + pairOff);
                LvValue cf; const int64_t* p = (const int64_t*)(const void*)(buf + pairOff + 8);
                cf.tag = p[0]; cf.payload = p[1];
                LvValue cv = lv_rebuild(buf, seen, cf);
                lvrt_capture_set(&out, (const char*)(intptr_t)keyPtr, &cv);   /* retains cv */
            }
            return out;
        }
        default:
            return out;                              /* unreachable: flatten rejects first */
    }
}

/* Flatten `v` into a fresh malloc buffer (accounted). NULL + *err on a
 * non-flattenable node. The buffer is self-contained and thread-portable. */
uint8_t* lv_thread_flatten(const LvValue* v, int64_t* sizeOut, char* errOut, int errCap) {
    LvFlatBuf b; b.data = NULL; b.size = LV_FLAT_HEADER; b.cap = 0; b.err = 0; b.errmsg[0] = 0;
    lv_flat_ensure(&b, LV_FLAT_HEADER);
    memset(b.data, 0, LV_FLAT_HEADER);
    LvFlatSeen seen; lv_seen_init(&seen);
    LvValue rootFv = lv_flatten(&b, &seen, v, /*isRoot=*/1);
    lv_seen_free(&seen);
    if (b.err) {
        if (errOut && errCap > 0) snprintf(errOut, (size_t)errCap, "%s", b.errmsg);
        free(b.data);
        return NULL;
    }
    int64_t* hdr = (int64_t*)(void*)b.data;
    hdr[0] = b.size;                     /* totalBytes */
    hdr[1] = rootFv.tag;
    hdr[2] = rootFv.payload;             /* record offset or immediate */
    if (sizeOut) *sizeOut = b.size;
    atomic_fetch_add(&lv_thread_transfers_out, 1);
    return b.data;
}

/* Rebuild the root value out of a flatten buffer into the calling thread's heap.
 * The root is returned at +1 (the transfer contract). */
void lv_thread_rebuild(LvValue* out, const uint8_t* buf) {
    const int64_t* hdr = (const int64_t*)(const void*)buf;
    LvValue rootFv; rootFv.tag = hdr[1]; rootFv.payload = hdr[2];
    LvRebSeen seen; lv_reb_init(&seen);
    *out = lv_rebuild(buf, &seen, rootFv);
    lv_reb_free(&seen);
    lvrt_retain(out);                    /* transfer: the receiver owns the root at +1 */
}

void lv_thread_free_buf(uint8_t* buf) {
    if (!buf) return;
    free(buf);
    atomic_fetch_add(&lv_thread_transfers_out, -1);
}

/* §4.2: the env-gated [threads] stats line, emitted at exit right after the
 * [heap] meter (LlvmGen's lv_main). LANG_THREAD_STATS=1 -> one stderr line the
 * M6 harness parses; stdout is untouched, so corpus differentials are
 * unaffected. On a thread-free program (or Windows, where the counters never
 * move) it reads spawns=0 reaps=0 transfer-outstanding=0. */
void lvrt_thread_stats_report(void) {
    if (!getenv("LANG_THREAD_STATS")) return;
    char buf[128];
    long long sp = atomic_load(&lv_thread_spawns);
    long long rp = atomic_load(&lv_thread_reaps);
    long long out = atomic_load(&lv_thread_transfers_out);
    int n = snprintf(buf, sizeof buf,
                     "[threads] spawns=%lld reaps=%lld transfer-outstanding=%lld\n",
                     sp, rp, out);
    if (n > 0) lv_plat_write(2, buf, n);
}

/* The single-thread transfer used by the interpreters' LLVM twin and by any
 * same-thread crossing: flatten, rebuild into this heap, free. On a non-
 * flattenable node it raises a catchable RuntimeException whose text matches
 * the interpreters' lvThreadCopy byte-for-byte (§4.1). */
void lvrt_systhreadtransfer(LvValue* out, const LvValue* v) {
    char err[160]; err[0] = 0;
    int64_t sz = 0;
    uint8_t* buf = lv_thread_flatten(v, &sz, err, (int)sizeof err);
    if (!buf) { lvrt_raise(err[0] ? err : "value cannot cross a thread boundary (v1)");
                out->tag = LV_VOID; out->payload = 0; return; }
    lv_thread_rebuild(out, buf);
    lv_thread_free_buf(buf);
}

int32_t lvrt_issub(int64_t a, int64_t b) {
    if (a == b) return 1;
    const LvClassInfo* c = lv_find_class(a);
    if (!c) return 0;
    for (int64_t i = 0; i < c->nSubtypeIds; i++)
        if (c->subtypeIds[i] == b) return 1;
    return 0;
}

/* Table-driven member lookup — name compared by content (strcmp), entry
 * kind discriminated by methodKinds (LV_M_*). X64Gen builds four SEPARATE
 * per-program dispatch switches (genGetm getters, genSetm setters, genOpm
 * operators, genCallM methods — src/X64Gen.cpp:771-895, 1919-1974); the
 * kind column is how one shared table reproduces that split (a field read
 * must never dispatch a plain method). NULL methodKinds == all LV_M_METHOD. */
static int lv_member_lookup(int64_t classId, const char* name, int8_t kind, int64_t* fnIndexOut) {
    const LvClassInfo* c = lv_find_class(classId);
    if (!c || !g_dispatch) return 0;   /* no table or no trampoline: fall to fields */
    for (int64_t i = 0; i < c->nMethods; i++) {
        int8_t k = c->methodKinds ? c->methodKinds[i] : (int8_t)LV_M_METHOD;
        if (k == kind && strcmp(c->methodNames[i], name) == 0) {
            *fnIndexOut = c->methodFnIndex[i];
            return 1;
        }
    }
    return 0;
}

#define LV_MAX_DISPATCH_ARGS 64

/* returns +1 (transfer), uniformly — see lv_abi.h's dispatch-ownership
 * ruling. Field path retains before returning; getter dispatch passes its
 * call result's +1 through untouched. */
void lvrt_getm(LvValue* out, const LvValue* recv, const char* name) {
    if (recv->tag == LV_OBJ) {
        int64_t classId = lv_ld_i64(recv->payload, 0);
        int64_t fnIndex;
        if (lv_member_lookup(classId, name, LV_M_GET, &fnIndex)) {
            LvValue args[1]; args[0] = *recv;
            g_dispatch(fnIndex, out, args, 1);   /* already +1 */
            return;
        }
        lvrt_getfield(out, recv, name);
        lvrt_retain(out);                        /* borrowed slot read -> +1 */
        return;
    }
    out->tag = LV_VOID; out->payload = 0;
}

/* field-write path does the slot ARC internally: release-old-then-retain-new
 * (§B-H1 order — the IR read protocol protects self-assignment). Setter
 * dispatch does no slot ARC; the setter body owns its stores. */
void lvrt_setm(const LvValue* recv, const char* name, const LvValue* val) {
    if (recv->tag == LV_OBJ) {
        int64_t classId = lv_ld_i64(recv->payload, 0);
        int64_t fnIndex;
        if (lv_member_lookup(classId, name, LV_M_SET, &fnIndex)) {
            LvValue args[2]; args[0] = *recv; args[1] = *val;
            LvValue ret;
            g_dispatch(fnIndex, &ret, args, 2);
            return;
        }
        LvValue old; lvrt_getfield(&old, recv, name);
        lvrt_release(&old);
        lvrt_setfield(recv, name, val);
        lvrt_retain(val);
    }
}

void lvrt_opm(LvValue* out, int64_t opcode, const LvValue* l, const LvValue* r) {
    static const char* kOpNames[] = { "+", "==", "!=", "-", "*", "/", "%",
                                       "<", ">", "<=", ">=", "&", "|", "<<", ">>",
                                       "^" };
    if (l->tag == LV_OBJ && opcode >= 0 && opcode < 16) {
        int64_t classId = lv_ld_i64(l->payload, 0);
        int64_t fnIndex;
        if (lv_member_lookup(classId, kOpNames[opcode], LV_M_OP, &fnIndex)) {
            LvValue args[2]; args[0] = *l; args[1] = *r;
            g_dispatch(fnIndex, out, args, 2);
            return;
        }
        if (opcode == LV_OP_NE && lv_member_lookup(classId, "==", LV_M_OP, &fnIndex)) {
            LvValue args[2]; args[0] = *l; args[1] = *r;
            LvValue eq; g_dispatch(fnIndex, &eq, args, 2);
            out->tag = LV_BOOL; out->payload = eq.payload ? 0 : 1;
            return;
        }
        if (opcode == LV_OP_EQ || opcode == LV_OP_NE) {
            /* a class with no (==) is reference identity (design §5.2). A value
             * struct gets a synthesized field-wise (==) at resolve time
             * (designs/struct-equality/, §5.5) and so never reaches here from
             * checked code; raise rather than answer silently if it ever does. */
            if (!lvrt_isvalueclass(classId)) {
                int same = r->tag == LV_OBJ && l->payload == r->payload;
                out->tag = LV_BOOL;
                out->payload = opcode == LV_OP_EQ ? same : !same;
                return;
            }
            const LvClassInfo* c = lv_find_class(classId);
            char buf[128];
            snprintf(buf, sizeof buf, "no operator '%s' on '%s'",
                     kOpNames[opcode], c && c->name ? c->name : "object");
            lvrt_raise(buf);
            out->tag = LV_VOID; out->payload = 0;
            return;
        }
    }
    out->tag = LV_VOID; out->payload = 0;
}

void lvrt_callm(LvValue* out, const LvValue* recv, const char* name, LvValue* args, int64_t argc) {
    if (recv->tag == LV_OBJ) {
        int64_t classId = lv_ld_i64(recv->payload, 0);
        int64_t fnIndex;
        if (lv_member_lookup(classId, name, LV_M_METHOD, &fnIndex)) {
            if (argc + 1 > LV_MAX_DISPATCH_ARGS) lv_die("lvrt: too many call args\n");
            LvValue full[LV_MAX_DISPATCH_ARGS];
            full[0] = *recv;
            for (int64_t i = 0; i < argc; i++) full[i + 1] = args[i];
            g_dispatch(fnIndex, out, full, argc + 1);
            return;
        }
    }
    /* native fallback: wired at B-M3 (§2.7) */
    out->tag = LV_VOID; out->payload = 0;
}

/* ========================================================================
 * §3 step 4 — Strings: descriptor ops, consume-unowned discipline.
 * ==================================================================== */

void lvrt_str_new(LvValue* out, const char* bytes, int64_t len) {
    int64_t payload = lv_halloc_prefixed(8 + len + 1);
    lv_st_i64(payload, 0, len);
    memcpy(P8(payload) + 8, bytes, (size_t)len);
    P8(payload)[8 + len] = 0;
    out->tag = LV_STR; out->payload = payload;
}

/* base-10, optional '-' sign — mirrors X64Gen::genIntToStr exactly
 * (src/X64Gen.cpp:132-170). */
void lvrt_int_to_str(LvValue* out, int64_t v) {
    char buf[24];
    int neg = v < 0;
    uint64_t u = neg ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    int i = (int)sizeof buf;
    buf[--i] = 0;   /* not part of the descriptor; just a scratch terminator */
    do { buf[--i] = (char)('0' + (u % 10)); u /= 10; } while (u);
    if (neg) buf[--i] = '-';
    lvrt_str_new(out, buf + i, (int64_t)(sizeof buf - 1 - i));
}

/* frees the string in `v` iff it is an UNOWNED (rc==0) heap block — the
 * §15 "dropped temporary" test (X64Gen::emitStrTempFree, src/X64Gen.cpp:
 * 209-217). The region check runs first: literals have no header at all. */
static void lv_str_temp_free(const LvValue* v) {
    if (!lv_in_region(P8(v->payload))) return;
    if (HDR(v->payload)[0] != 0) return;   /* owned or arena: not a dead temp */
    lv_free_raw(P8(v->payload) - 16, HDR(v->payload)[1]);
}

void lvrt_str_concat(LvValue* out, const LvValue* a, const LvValue* b) {
    int64_t lenA = lv_ld_i64(a->payload, 0), lenB = lv_ld_i64(b->payload, 0);
    int64_t payload = lv_halloc_prefixed(8 + lenA + lenB + 1);
    lv_st_i64(payload, 0, lenA + lenB);
    memcpy(P8(payload) + 8, P8(a->payload) + 8, (size_t)lenA);
    memcpy(P8(payload) + 8 + lenA, P8(b->payload) + 8, (size_t)lenB);
    P8(payload)[8 + lenA + lenB] = 0;
    /* consume unowned operands (dropped temporaries); guard B==A (same
     * temp fed twice) so it is only freed once */
    lv_str_temp_free(a);
    if (b->payload != a->payload) lv_str_temp_free(b);
    out->tag = LV_STR; out->payload = payload;
}

int32_t lvrt_str_eq(const LvValue* a, const LvValue* b) {
    int64_t lenA = lv_ld_i64(a->payload, 0), lenB = lv_ld_i64(b->payload, 0);
    if (lenA != lenB) return 0;
    if (lenA == 0) return 1;
    return memcmp(P8(a->payload) + 8, P8(b->payload) + 8, (size_t)lenA) == 0;
}

/* clamped [start, start+n), matches X64Gen::genStrSubStr exactly
 * (src/X64Gen.cpp:2729-2750). */
void lvrt_str_substr(LvValue* out, const LvValue* s, int64_t start, int64_t n) {
    int64_t slen = lv_ld_i64(s->payload, 0);
    if (start < 0 || start > slen) { lvrt_str_new(out, "", 0); return; }
    int64_t avail = slen - start;
    int64_t take = n < avail ? n : avail;
    if (take < 0) take = 0;
    lvrt_str_new(out, (const char*)(P8(s->payload) + 8 + start), take);
}

/* naive search; empty needle -> 0, not found -> -1 (X64Gen::genStrIndexOf,
 * src/X64Gen.cpp:2699-2726). */
int64_t lvrt_str_indexof(const LvValue* s, const LvValue* needle) {
    int64_t slen = lv_ld_i64(s->payload, 0), nlen = lv_ld_i64(needle->payload, 0);
    if (nlen == 0) return 0;
    const char* sb = (const char*)(P8(s->payload) + 8);
    const char* nb = (const char*)(P8(needle->payload) + 8);
    for (int64_t i = 0; i + nlen <= slen; i++)
        if (memcmp(sb + i, nb, (size_t)nlen) == 0) return i;
    return -1;
}

/* Track 04 M2 DEVIATION from the frozen X64Gen reference (this file's own
 * mandate, header comment): X64Gen::genStrToInt (src/X64Gen.cpp:2753-2776)
 * is the pre-Track-04 atoll-style parse (skip leading space, digits until
 * the first non-digit, garbage -> 0) and is FROZEN there unchanged — ELF
 * keeps that old behavior forever (designs/techdesign-04-stdlib-strings.md
 * §STOP: X64Gen is never extended). This runtime's toInt/toFloat instead
 * implement the NEW strict full-string parse (int? toInt() / float?
 * toFloat(), reference §6.1): optional leading '-', digits only, no
 * surrounding space; anything else, or overflow, -> None (tag LV_NONE). */
void lvrt_str_toint(LvValue* out, const LvValue* s) {
    int64_t len = lv_ld_i64(s->payload, 0);
    const char* b = (const char*)(P8(s->payload) + 8);
    int64_t i = (len > 0 && b[0] == '-') ? 1 : 0;
    if (len == 0 || i == len) { out->tag = LV_NONE; out->payload = 0; return; }
    for (int64_t j = i; j < len; j++)
        if (b[j] < '0' || b[j] > '9') { out->tag = LV_NONE; out->payload = 0; return; }
    errno = 0;
    char* end = NULL;
    long long v = strtoll(b, &end, 10);
    if (errno == ERANGE || end != b + len) { out->tag = LV_NONE; out->payload = 0; return; }
    out->tag = LV_INT; out->payload = (int64_t)v;
}

/* Same strictness shape as lvrt_str_toint above (no surrounding space,
 * strtod full-consumption, finite result only — rejects "inf"/"nan" text). */
void lvrt_str_tofloat(LvValue* out, const LvValue* s) {
    int64_t len = lv_ld_i64(s->payload, 0);
    const char* b = (const char*)(P8(s->payload) + 8);
    if (len == 0 || b[0] == ' ' || b[0] == '\t' || b[0] == '\n' || b[0] == '\r' ||
        b[len - 1] == ' ' || b[len - 1] == '\t' || b[len - 1] == '\n' || b[len - 1] == '\r') {
        out->tag = LV_NONE; out->payload = 0; return;
    }
    errno = 0;
    char* end = NULL;
    double v = strtod(b, &end);
    if (end != b + len || !isfinite(v)) { out->tag = LV_NONE; out->payload = 0; return; }
    out->tag = LV_FLOAT; memcpy(&out->payload, &v, sizeof v);
}

/* Track 04 M3: byte value 0..255 at index i; OOB raises (matches lvrt_arr's
 * OOB discipline via lvrt_raise_oob), never returns None. */
void lvrt_str_byteat(LvValue* out, const LvValue* s, int64_t i) {
    int64_t len = lv_ld_i64(s->payload, 0);
    if (i < 0 || i >= len) { lvrt_raise_oob(i, len); out->tag = LV_VOID; out->payload = 0; return; }
    const unsigned char* b = (const unsigned char*)(P8(s->payload) + 8);
    out->tag = LV_INT; out->payload = (int64_t)b[i];
}

/* The free-function fallback for a static-side native (no `static` keyword
 * exists — designs/techdesign-04-stdlib-strings.md problem #6). 1-byte
 * string from a byte value; out of range raises, matching lvrt_str_byteat. */
void lvrt_str_frombyte(LvValue* out, int64_t b) {
    if (b < 0 || b > 255) {
        char buf[64];
        snprintf(buf, sizeof buf, "byte value %lld out of range 0..255", (long long)b);
        lvrt_raise(buf);
        out->tag = LV_VOID; out->payload = 0;
        return;
    }
    char c = (char)(unsigned char)b;
    lvrt_str_new(out, &c, 1);
}

void lvrt_str_trim(LvValue* out, const LvValue* s) {
    int64_t len = lv_ld_i64(s->payload, 0);
    const char* b = (const char*)(P8(s->payload) + 8);
    int64_t a = 0;
    while (a < len && (b[a] == ' ' || b[a] == '\t' || b[a] == '\n' || b[a] == '\r')) a++;
    if (a == len) { lvrt_str_new(out, "", 0); return; }
    int64_t e = len - 1;
    while (e > a && (b[e] == ' ' || b[e] == '\t' || b[e] == '\n' || b[e] == '\r')) e--;
    lvrt_str_substr(out, s, a, e - a + 1);
}

void lvrt_str_case(LvValue* out, const LvValue* s, int32_t toLower) {
    int64_t len = lv_ld_i64(s->payload, 0);
    int64_t payload = lv_halloc_prefixed(8 + len + 1);
    lv_st_i64(payload, 0, len);
    const unsigned char* src = (const unsigned char*)(P8(s->payload) + 8);
    unsigned char* dst = P8(payload) + 8;
    for (int64_t i = 0; i < len; i++) {
        unsigned char c = src[i];
        if (toLower) { if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32); }
        else         { if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32); }
        dst[i] = c;
    }
    dst[len] = 0;
    out->tag = LV_STR; out->payload = payload;
}

/* ========================================================================
 * Track 03 §1 — char (LV_CHAR): UTF-8 en/decode. Byte-for-byte mirrors the
 * oracle's utf8Encode / utf8DecodeAt (src/RuntimeValue.hpp:107,132) so the
 * LLVM lane diffs identically against --run/--ir/emit-C++.
 * ==================================================================== */

/* encode one scalar into up to 4 bytes; returns the byte count */
static int lv_utf8_encode(int64_t cp, char out[4]) {
    unsigned long c = (unsigned long)cp;
    if (c <= 0x7F) { out[0] = (char)c; return 1; }
    if (c <= 0x7FF) {
        out[0] = (char)(0xC0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c <= 0xFFFF) {
        out[0] = (char)(0xE0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (c >> 18));
    out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

/* decode the scalar starting at byte `i`; sets *plen to its byte length and
 * *pboundary=0 iff byte i is a continuation byte. Invalid/truncated -> U+FFFD
 * with len=1 (replacement policy — data is never a crash). */
static int64_t lv_utf8_decode_at(const unsigned char* s, int64_t slen, int64_t i,
                                 int64_t* plen, int* pboundary) {
    *plen = 1; *pboundary = 1;
    if (i >= slen) return 0xFFFD;
    unsigned char c = s[i];
    if (c < 0x80) return c;
    if ((c & 0xC0) == 0x80) { *pboundary = 0; return 0xFFFD; }   /* continuation byte */
    int need; int64_t cp;
    if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; }
    else return 0xFFFD;                                          /* invalid lead byte */
    for (int k = 1; k <= need; k++) {
        if (i + k >= slen || (s[i + k] & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (s[i + k] & 0x3F);
    }
    *plen = need + 1;
    return cp;
}

void lvrt_char_to_string(LvValue* out, int64_t scalar) {
    char buf[4];
    int n = lv_utf8_encode(scalar, buf);
    lvrt_str_new(out, buf, n);
}

/* string.at(i): decode the scalar STARTING at byte offset i (C1, O(1)). OOB or
 * a mid-sequence offset throws (matches RuntimeNatives.cpp string "at"). */
void lvrt_str_at(LvValue* out, const LvValue* s, int64_t i) {
    int64_t slen = lv_ld_i64(s->payload, 0);
    if (i < 0 || i >= slen) {
        lvrt_raise_oob(i, slen); out->tag = LV_VOID; out->payload = 0; return;
    }
    const unsigned char* b = (const unsigned char*)(P8(s->payload) + 8);
    int64_t len; int boundary;
    int64_t cp = lv_utf8_decode_at(b, slen, i, &len, &boundary);
    if (!boundary) {
        char buf[64];
        snprintf(buf, sizeof buf, "byte offset %lld is not a scalar boundary",
                 (long long)i);
        lvrt_raise(buf); out->tag = LV_VOID; out->payload = 0; return;
    }
    out->tag = LV_CHAR; out->payload = cp;
}

/* string.chars(): full UTF-8 decode to a fresh Array<char>; invalid bytes ->
 * U+FFFD, never a throw. Elements are LV_CHAR immediates (no per-element ARC).
 * Two passes: count scalars, then fill (lvrt_arr_new pre-sizes exactly). */
void lvrt_str_chars(LvValue* out, const LvValue* s) {
    int64_t slen = lv_ld_i64(s->payload, 0);
    const unsigned char* b = (const unsigned char*)(P8(s->payload) + 8);
    int64_t n = 0;
    for (int64_t i = 0; i < slen; ) {
        int64_t len; int boundary;
        (void)lv_utf8_decode_at(b, slen, i, &len, &boundary);
        n++; i += len;
    }
    lvrt_arr_new(out, n);
    int64_t idx = 0;
    for (int64_t i = 0; i < slen; ) {
        int64_t len; int boundary;
        int64_t cp = lv_utf8_decode_at(b, slen, i, &len, &boundary);
        LvValue ch; ch.tag = LV_CHAR; ch.payload = cp;
        lv_st_val(out->payload, 8 + 16 * idx, &ch);
        idx++; i += len;
    }
}

/* ========================================================================
 * Track 03 §3 — Block (LV_BLOCK): fixed-length mutable byte buffer (C4).
 * Body {parentPtr@0, off@8, len@16, dataPtr@24}; see lv_abi.h §2.4. Every
 * accessor bounds-checks and raises (loud); writes mutate the shared buffer
 * in place, so a slice's write is visible through its parent (aliasing).
 * ==================================================================== */

/* the byte address of view element `i` (caller has already bounds-checked) */
static uint8_t* lv_block_at(int64_t payload, int64_t i) {
    return P8(lv_ld_i64(payload, 24)) + lv_ld_i64(payload, 8) + i;
}

static int64_t lv_block_alloc(int64_t parentPtr, int64_t off, int64_t len, int64_t dataPtr) {
    int64_t payload = lv_halloc_prefixed(32);
    lv_st_i64(payload, 0, parentPtr);
    lv_st_i64(payload, 8, off);
    lv_st_i64(payload, 16, len);
    lv_st_i64(payload, 24, dataPtr);
    return payload;
}

void lvrt_block_new(LvValue* out, int64_t size) {
    if (size < 0) size = 0;
    int64_t data = lv_halloc_prefixed(size);   /* owned buffer; header rc unused */
    if (size) memset(P8(data), 0, (size_t)size);
    out->tag = LV_BLOCK; out->payload = lv_block_alloc(0, 0, size, data);
}

void lvrt_block_fromstr(LvValue* out, const LvValue* s) {
    int64_t len = lv_ld_i64(s->payload, 0);
    int64_t data = lv_halloc_prefixed(len);
    if (len) memcpy(P8(data), P8(s->payload) + 8, (size_t)len);
    out->tag = LV_BLOCK; out->payload = lv_block_alloc(0, 0, len, data);
}

void lvrt_block_byteat(LvValue* out, const LvValue* b, int64_t i) {
    int64_t len = lv_ld_i64(b->payload, 16);
    if (i < 0 || i >= len) { lvrt_raise_oob(i, len); out->tag = LV_VOID; out->payload = 0; return; }
    out->tag = LV_INT; out->payload = (int64_t)(*lv_block_at(b->payload, i));
}

void lvrt_block_setbyte(const LvValue* b, int64_t i, int64_t value) {
    int64_t len = lv_ld_i64(b->payload, 16);
    if (i < 0 || i >= len) { lvrt_raise_oob(i, len); return; }
    if (value < 0 || value > 255) {
        char buf[64];
        snprintf(buf, sizeof buf, "byte value %lld out of range 0..255", (long long)value);
        lvrt_raise(buf); return;
    }
    *lv_block_at(b->payload, i) = (uint8_t)value;
}

void lvrt_block_slice(LvValue* out, const LvValue* b, int64_t off, int64_t len) {
    int64_t blen = lv_ld_i64(b->payload, 16);
    if (off < 0 || len < 0 || off + len > blen) {
        lvrt_raise_oob(off, blen); out->tag = LV_VOID; out->payload = 0; return;
    }
    int64_t bp = b->payload;
    int64_t parent = lv_ld_i64(bp, 0);
    int64_t root = parent ? parent : bp;   /* parentPtr is always the ROOT */
    int64_t payload = lv_block_alloc(root, lv_ld_i64(bp, 8) + off, len, lv_ld_i64(bp, 24));
    LvValue rootv; rootv.tag = LV_BLOCK; rootv.payload = root;
    lvrt_retain(&rootv);   /* the slice keeps the root (and its bytes) alive */
    out->tag = LV_BLOCK; out->payload = payload;
}

void lvrt_block_tostring(LvValue* out, const LvValue* b, int64_t off, int64_t len) {
    int64_t blen = lv_ld_i64(b->payload, 16);
    if (off < 0 || len < 0 || off + len > blen) {
        lvrt_raise_oob(off, blen); out->tag = LV_VOID; out->payload = 0; return;
    }
    lvrt_str_new(out, (const char*)lv_block_at(b->payload, off), len);
}

void lvrt_block_int32at(LvValue* out, const LvValue* b, int64_t i) {
    int64_t len = lv_ld_i64(b->payload, 16);
    if (i < 0 || i + 4 > len) { lvrt_raise_oob(i, len); out->tag = LV_VOID; out->payload = 0; return; }
    const uint8_t* d = lv_block_at(b->payload, i);
    uint32_t u = 0;
    for (int k = 0; k < 4; k++) u |= (uint32_t)d[k] << (8 * k);   /* little-endian */
    out->tag = LV_INT; out->payload = (int64_t)(int32_t)u;        /* sign-extended */
}

void lvrt_block_setint32(const LvValue* b, int64_t i, int64_t value) {
    int64_t len = lv_ld_i64(b->payload, 16);
    if (i < 0 || i + 4 > len) { lvrt_raise_oob(i, len); return; }
    uint8_t* d = lv_block_at(b->payload, i);
    uint32_t u = (uint32_t)value;
    for (int k = 0; k < 4; k++) d[k] = (uint8_t)(u >> (8 * k));
}

void lvrt_block_int64at(LvValue* out, const LvValue* b, int64_t i) {
    int64_t len = lv_ld_i64(b->payload, 16);
    if (i < 0 || i + 8 > len) { lvrt_raise_oob(i, len); out->tag = LV_VOID; out->payload = 0; return; }
    const uint8_t* d = lv_block_at(b->payload, i);
    uint64_t u = 0;
    for (int k = 0; k < 8; k++) u |= (uint64_t)d[k] << (8 * k);   /* little-endian */
    out->tag = LV_INT; out->payload = (int64_t)u;
}

void lvrt_block_setint64(const LvValue* b, int64_t i, int64_t value) {
    int64_t len = lv_ld_i64(b->payload, 16);
    if (i < 0 || i + 8 > len) { lvrt_raise_oob(i, len); return; }
    uint8_t* d = lv_block_at(b->payload, i);
    uint64_t u = (uint64_t)value;
    for (int k = 0; k < 8; k++) d[k] = (uint8_t)(u >> (8 * k));
}

void lvrt_block_fill(const LvValue* b, int64_t off, int64_t len, int64_t value) {
    int64_t blen = lv_ld_i64(b->payload, 16);
    if (off < 0 || len < 0 || off > blen || len > blen - off) {
        lvrt_raise_oob(off, blen); return;
    }
    if (value < 0 || value > 255) {
        char buf[64];
        snprintf(buf, sizeof buf, "byte value %lld out of range 0..255", (long long)value);
        lvrt_raise(buf); return;
    }
    if (len) memset(lv_block_at(b->payload, off), (int)value, (size_t)len);
}

void lvrt_block_blit(const LvValue* dst, int64_t dstOff, const LvValue* src,
                     int64_t srcOff, int64_t len) {
    int64_t dlen = lv_ld_i64(dst->payload, 16);
    int64_t slen = lv_ld_i64(src->payload, 16);
    if (dstOff < 0 || len < 0 || dstOff > dlen || len > dlen - dstOff) {
        lvrt_raise_oob(dstOff, dlen); return;
    }
    if (srcOff < 0 || srcOff > slen || len > slen - srcOff) {
        lvrt_raise_oob(srcOff, slen); return;
    }
    if (len) memmove(lv_block_at(dst->payload, dstOff), lv_block_at(src->payload, srcOff),
                     (size_t)len);
}

void lvrt_block_equals(LvValue* out, const LvValue* b, const LvValue* other) {
    int64_t len = lv_ld_i64(b->payload, 16);
    int64_t olen = lv_ld_i64(other->payload, 16);
    int equal = len == olen && (len == 0 || memcmp(lv_block_at(b->payload, 0),
                                                   lv_block_at(other->payload, 0),
                                                   (size_t)len) == 0);
    out->tag = LV_BOOL; out->payload = equal;
}

void lvrt_block_mismatch(LvValue* out, const LvValue* b, const LvValue* other, int64_t from) {
    int64_t len = lv_ld_i64(b->payload, 16);
    int64_t olen = lv_ld_i64(other->payload, 16);
    if (len != olen) {
        lvrt_raise("Block.mismatch requires equal lengths");
        out->tag = LV_VOID; out->payload = 0; return;
    }
    if (from < 0 || from > len) {
        lvrt_raise_oob(from, len); out->tag = LV_VOID; out->payload = 0; return;
    }
    const uint8_t* a = len ? lv_block_at(b->payload, from) : NULL;
    const uint8_t* c = len ? lv_block_at(other->payload, from) : NULL;
    int64_t i = from;
    while (i + 64 <= len && memcmp(a + (i - from), c + (i - from), 64) == 0) i += 64;
    while (i < len && a[i - from] == c[i - from]) i++;
    out->tag = LV_INT; out->payload = i == len ? -1 : i;
}

/* ========================================================================
 * §3 step 5 — Arrays/maps: cores + idxget/idxset with rc==1 COW.
 * ==================================================================== */

void lvrt_arr_new(LvValue* out, int64_t len) {
    int64_t payload = lv_halloc_prefixed(8 + 16 * len);
    HDR(payload)[1] = len;   /* meta = capacity for boxed arrays (§2.4) */
    lv_st_i64(payload, 0, len);
    out->tag = LV_ARR; out->payload = payload;
}

void lvrt_map_new(LvValue* out, int64_t len) {
    int64_t payload = lv_halloc_prefixed(8 + 32 * len);
    lv_st_i64(payload, 0, len);
    out->tag = LV_MAP; out->payload = payload;
}

static void lv_arr_go_dense(LvValue* out, const LvValue* val) {
    int64_t classId = lv_ld_i64(val->payload, 0);
    int64_t f = lvrt_fieldcount(classId);
    int64_t recBytes = 16 + 16 * f;
    int64_t payload = lv_halloc_prefixed(16 + recBytes);
    lv_st_i64(payload, 0, (int64_t)((uint64_t)1 | LV_DENSE_BIT_U));
    lv_st_i64(payload, 8, recBytes);
    memcpy(P8(payload) + 16, P8(val->payload), (size_t)recBytes);
    HDR(payload)[0] = 1;   /* §15: append returns owned (+1), a transfer contract */
    out->tag = LV_ARR; out->payload = payload;
}

/* dense append always copies+grows — matches X64Gen::genArrAppend's DENSE
 * APPEND branch exactly (src/X64Gen.cpp:977-992), which likewise never
 * frees the old buffer. This is a KNOWN leak edge in the frozen ELF
 * backend (see the declared dense_index_set.ext XFAIL, doc-2 §6) —
 * replicated deliberately for differential parity, not fixed here. */
static void lv_arr_dense_append(LvValue* out, const LvValue* arr, const LvValue* val) {
    int64_t cleanLen = lv_ld_i64(arr->payload, 0) & ~LV_DENSE_BIT;
    int64_t recBytes = lv_ld_i64(arr->payload, 8);
    int64_t payload = lv_halloc_prefixed(16 + (cleanLen + 1) * recBytes);
    lv_st_i64(payload, 0, (int64_t)((uint64_t)(cleanLen + 1) | LV_DENSE_BIT_U));
    lv_st_i64(payload, 8, recBytes);
    memcpy(P8(payload) + 16, P8(arr->payload) + 16, (size_t)(cleanLen * recBytes));
    memcpy(P8(payload) + 16 + cleanLen * recBytes, P8(val->payload), (size_t)recBytes);
    HDR(payload)[0] = 1;
    out->tag = LV_ARR; out->payload = payload;
}

/* ======================= columnar (SoA) array core =======================
 * techdesign-columnar-arrays.md §4-§6. A columnar buffer holds only immediate
 * scalars (eligibility guarantees no heap refs), so every operation here is
 * pure byte movement — no ARC walks. Column k of an element lives at
 * columnBase(k) + 8*i; the byte offset (relative to the payload base, past the
 * 16-byte {lenWithBits, classId} header) is computed once here so the stride-8
 * constant lives in exactly one place in this artifact (§4.3). */
static int64_t lv_col_field_off(int64_t cleanLen, int64_t k, int64_t i) {
    return 16 + k * 8 * cleanLen + 8 * i;   /* payload-relative byte offset */
}

/* scatter val's (standalone record) field payloads across element i's columns. */
static void lv_col_scatter(int64_t payload, int64_t cleanLen, int64_t f,
                           int64_t i, const LvValue* val) {
    for (int64_t k = 0; k < f; k++) {
        int64_t fieldPay = lv_ld_i64(val->payload, 16 + 16 * k + 8);   /* slot k payload half */
        lv_st_i64(payload, lv_col_field_off(cleanLen, k, i), fieldPay);
    }
}

/* gather element i into a FRESH standalone value-struct record (rc=1). Tags are
 * synthesized from the emitted descriptor (§4.3/§5.2); the buffer stores payload
 * words only. */
static void lv_col_gather(LvValue* out, const LvValue* base, int64_t cleanLen,
                          int64_t classId, int64_t f, int64_t i) {
    int64_t rec = lv_halloc_prefixed(16 + 16 * f);
    lv_st_i64(rec, 0, classId);
    lv_st_i64(rec, 8, 0);                  /* dyn list empty */
    for (int64_t k = 0; k < f; k++) {
        LvValue fv;
        fv.tag = lv_col_typecode(classId, k);
        fv.payload = lv_ld_i64(base->payload, lv_col_field_off(cleanLen, k, i));
        lv_st_val(rec, 16 + 16 * k, &fv);
    }
    /* rc stays 0 (lv_halloc_prefixed default): a gathered element is a FRESH
     * standalone value-struct copy under the SAME convention as lvrt_copyval —
     * unowned until a consumer retains it, reclaimed by lvrt_vfree at the
     * consuming site (VFree only frees rc==0). This supersedes the design's
     * §5.2 "rc=1" wording: rc=1 would make VFree no-op and leak (Part 2 §6). */
    out->tag = LV_OBJ; out->payload = rec;
}

/* first-struct flip to columnar (mirrors lv_arr_go_dense). cleanLen == 1. */
static void lv_arr_go_columnar(LvValue* out, const LvValue* val) {
    int64_t classId = lv_ld_i64(val->payload, 0);
    int64_t f = lvrt_fieldcount(classId);
    int64_t payload = lv_halloc_prefixed(16 + f * 8 * 1);
    lv_st_i64(payload, 0, (int64_t)((uint64_t)1 | LV_ARR_TOPBITS_U));
    lv_st_i64(payload, 8, classId);
    lv_col_scatter(payload, 1, f, 0, val);
    HDR(payload)[0] = 1;                    /* §15: append returns owned (+1) */
    out->tag = LV_ARR; out->payload = payload;
}

/* columnar append: copy-and-grow, per-column region copy (column bases move
 * with the length). No capacity slack — parity with dense-row (§5.1/§6.3). The
 * old buffer is not freed here, matching lv_arr_dense_append's declared wart. */
static void lv_arr_columnar_append(LvValue* out, const LvValue* arr, const LvValue* val) {
    int64_t word0 = lv_ld_i64(arr->payload, 0);
    int64_t cleanLen = LV_ARR_CLEANLEN(word0);
    int64_t classId = lv_ld_i64(arr->payload, 8);
    int64_t f = lvrt_fieldcount(classId);
    int64_t nlen = cleanLen + 1;
    int64_t payload = lv_halloc_prefixed(16 + f * 8 * nlen);
    lv_st_i64(payload, 0, (int64_t)((uint64_t)nlen | LV_ARR_TOPBITS_U));
    lv_st_i64(payload, 8, classId);
    for (int64_t k = 0; k < f; k++)         /* copy each column into the wider layout */
        memcpy(P8(payload) + lv_col_field_off(nlen, k, 0),
               P8(arr->payload) + lv_col_field_off(cleanLen, k, 0),
               (size_t)(8 * cleanLen));
    lv_col_scatter(payload, nlen, f, cleanLen, val);   /* the new element */
    HDR(payload)[0] = 1;
    out->tag = LV_ARR; out->payload = payload;
}

/* Array(n, fill) — one-shot O(n) sized construction (techdesign-columnar §5.1).
 * Replaces the codegen append loop (which is O(n^2) copy-and-grow for the dense
 * forms and leaks intermediate buffers) with a single allocation, producing the
 * SAME array the append loop would (row-major dense records / boxed elements /
 * columnar columns), just built in O(n) and leak-free. Returns owned (+1),
 * matching the NewArraySized dk==2 transfer convention. */
void lvrt_arr_fill(LvValue* out, int64_t n, const LvValue* fill) {
    if (n <= 0) { lvrt_arr_new(out, 0); HDR(out->payload)[0] = 1; return; }
    if (fill->tag == LV_OBJ && lvrt_isvalueclass(lv_ld_i64(fill->payload, 0))) {
        int64_t classId = lv_ld_i64(fill->payload, 0);
        int64_t f = lvrt_fieldcount(classId);
        if (lv_cfg_columnar && lv_col_eligible(classId)) {   /* columnar columns */
            int64_t payload = lv_halloc_prefixed(16 + f * 8 * n);
            lv_st_i64(payload, 0, (int64_t)((uint64_t)n | LV_ARR_TOPBITS_U));
            lv_st_i64(payload, 8, classId);
            for (int64_t k = 0; k < f; k++) {
                int64_t fp = lv_ld_i64(fill->payload, 16 + 16 * k + 8);   /* slot k payload */
                for (int64_t i = 0; i < n; i++)
                    lv_st_i64(payload, lv_col_field_off(n, k, i), fp);
            }
            HDR(payload)[0] = 1;
            out->tag = LV_ARR; out->payload = payload;
            return;
        }
        int64_t recBytes = 16 + 16 * f;                       /* row-major dense records */
        int64_t payload = lv_halloc_prefixed(16 + n * recBytes);
        lv_st_i64(payload, 0, (int64_t)((uint64_t)n | LV_DENSE_BIT_U));
        lv_st_i64(payload, 8, recBytes);
        for (int64_t i = 0; i < n; i++)
            memcpy(P8(payload) + 16 + i * recBytes, P8(fill->payload), (size_t)recBytes);
        HDR(payload)[0] = 1;
        out->tag = LV_ARR; out->payload = payload;
        return;
    }
    /* boxed: n copies of the fill (scalar or reference); references are retained */
    LvValue arr; lvrt_arr_new(&arr, n);
    for (int64_t i = 0; i < n; i++) {
        lv_st_val(arr.payload, 8 + 16 * i, fill);
        lvrt_retain(fill);
    }
    HDR(arr.payload)[0] = 1;
    *out = arr;
}

/* bug #66: does a value struct hold any HEAP-reference field (a nested struct,
 * string, array, map, closure, block)? Such a struct cannot live in a DENSE
 * array: the dense record memcpy shallow-copies the field's pointer and the
 * dense free path never walks elements, so the referent dangles the instant the
 * source struct is freed (or a later allocation reuses its slot). These must go
 * BOXED with a deep copy instead. A field's heap-ness is read from its stored
 * LvValue tag in the source record (offset 16 + 16*k). */
static int lv_struct_has_heap_field(int64_t payload) {
    int64_t classId = lv_ld_i64(payload, 0);
    int64_t f = lvrt_fieldcount(classId);
    for (int64_t k = 0; k < f; k++) {
        int64_t tag = lv_ld_i64(payload, 16 + 16 * k);
        if (tag == LV_STR || tag == LV_OBJ || tag == LV_ARR || tag == LV_MAP ||
            tag == LV_CLO || tag == LV_BLOCK || tag == LV_WEAK_PROXY)
            return 1;
    }
    return 0;
}

/* Store `val` into an array slot with array-owned semantics — a value struct is
 * deep-copied (value semantics, §9) so the array owns an independent copy whose
 * nested referents cannot be freed out from under it; any other value is
 * retained. Mirrors lv_map_store_val exactly (bug #49/#66). */
static void lv_arr_store_owned(int64_t payload, int64_t off, const LvValue* val) {
    if (val->tag == LV_OBJ && lvrt_isvalueclass(lv_ld_i64(val->payload, 0))) {
        LvValue cp; lvrt_copyval(&cp, val);
        lv_st_val(payload, off, &cp);
    } else {
        lv_st_val(payload, off, val);
        lvrt_retain(val);
    }
}

static void lv_arr_boxed_append(LvValue* out, const LvValue* arr, const LvValue* val) {
    int64_t len = lv_ld_i64(arr->payload, 0);
    int64_t rc = HDR(arr->payload)[0];
    int64_t capacity = HDR(arr->payload)[1];
    if (rc == 1 && len < capacity) {
        /* bug #66: value struct -> deep copy (array owns it); ref -> retain. */
        lv_arr_store_owned(arr->payload, 8 + 16 * len, val);
        lv_st_i64(arr->payload, 0, len + 1);
        *out = *arr;
        return;
    }
    if (rc == 1) {
        /* grow: geometric (max(4, 2*cap)), transfer old elements' refs,
         * reclaim the old buffer (its refs transfer, not double-owned) */
        int64_t newcap = capacity * 2; if (newcap < 4) newcap = 4;
        int64_t payload = lv_halloc_prefixed(8 + 16 * newcap);
        HDR(payload)[1] = newcap;
        lv_st_i64(payload, 0, len + 1);
        memcpy(P8(payload) + 8, P8(arr->payload) + 8, (size_t)(16 * len));
        lv_arr_store_owned(payload, 8 + 16 * len, val);   /* bug #66: own the new element */
        lv_free_raw(P8(arr->payload) - 16, 24 + 16 * capacity);
        HDR(payload)[0] = 1;
        out->tag = LV_ARR; out->payload = payload;
        return;
    }
    /* shared (rc != 1): pure-value copy — every element gets an OWNED copy in
     * the fresh buffer (a value struct is deep-copied so the two buffers cannot
     * vfree the same struct; a reference is retained), the old buffer stays live
     * with its own counted refs (bug #66). */
    LvValue fresh; lvrt_arr_new(&fresh, len + 1);
    for (int64_t i = 0; i < len; i++) {
        LvValue elem = lv_ld_val(arr->payload, 8 + 16 * i);
        lv_arr_store_owned(fresh.payload, 8 + 16 * i, &elem);
    }
    lv_arr_store_owned(fresh.payload, 8 + 16 * len, val);
    HDR(fresh.payload)[0] = 1;
    *out = fresh;
}

void lvrt_arr_append(LvValue* out, const LvValue* arr, const LvValue* val) {
    int64_t rawlen = lv_ld_i64(arr->payload, 0);
    if (rawlen < 0) {   /* already dense OR columnar */
        if (LV_IS_COLUMNAR(rawlen)) lv_arr_columnar_append(out, arr, val);
        else                        lv_arr_dense_append(out, arr, val);
        return;
    }
    if (rawlen == 0 && val->tag == LV_OBJ && lvrt_isvalueclass(lv_ld_i64(val->payload, 0))) {
        /* first value-struct append: flip. bug #66: a struct with a heap-
         * reference field (nested struct / string / array / …) is NOT dense-
         * safe — the dense record shallow-copies the pointer and the dense free
         * path never walks it — so it goes BOXED (deep-copied, ARC-managed)
         * instead. Only a FLAT struct (all-immediate fields, whose memcpy is a
         * full self-contained copy) takes the dense/columnar fast path. */
        int64_t classId = lv_ld_i64(val->payload, 0);
        if (lv_struct_has_heap_field(val->payload))      lv_arr_boxed_append(out, arr, val);
        else if (lv_cfg_columnar && lv_col_eligible(classId)) lv_arr_go_columnar(out, val);
        else                                             lv_arr_go_dense(out, val);
        return;
    }
    lv_arr_boxed_append(out, arr, val);
}

/* Track 04 M4: sum lengths, one allocation, memcpy each part -- O(total).
 * Boxed-array layout only (strings never go dense, §2.4); elements are read
 * as strings unconditionally (see lv_abi.h's declaration comment). */
void lvrt_arr_concatall(LvValue* out, const LvValue* arr) {
    int64_t len = lv_ld_i64(arr->payload, 0);
    int64_t total = 0;
    for (int64_t i = 0; i < len; i++) {
        LvValue elem = lv_ld_val(arr->payload, 8 + 16 * i);
        total += lv_ld_i64(elem.payload, 0);
    }
    int64_t payload = lv_halloc_prefixed(8 + total + 1);
    lv_st_i64(payload, 0, total);
    unsigned char* dst = P8(payload) + 8;
    int64_t off = 0;
    for (int64_t i = 0; i < len; i++) {
        LvValue elem = lv_ld_val(arr->payload, 8 + 16 * i);
        int64_t elen = lv_ld_i64(elem.payload, 0);
        memcpy(dst + off, P8(elem.payload) + 8, (size_t)elen);
        off += elen;
    }
    dst[total] = 0;
    out->tag = LV_STR; out->payload = payload;
}

/* struct-equality design §3: THE one canon for this engine (hash-consistency
 * law §3.3 — lvrt_keyeq's LV_FLOAT leg below, lvrt_canoneq, and any future
 * canon_hash all normalize through THIS symbol). Operates on the raw bit
 * pattern (LV_FLOAT stores it in `payload`, §2.3) in pure integer/branchless
 * form (§3.2): no FPU compare ever, no double materialized. NaN -> canonical
 * qNaN; ±0.0 -> 0; else raw bits. */
static uint64_t lv_canon_bits(uint64_t b) {
    uint64_t is_nan  = ((b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull)
                     & ((b & 0x000FFFFFFFFFFFFFull) != 0);
    uint64_t is_zero = (b << 1) == 0;
    return is_nan ? 0x7FF8000000000000ull : (is_zero ? 0 : b);
}

/* The canonical float relation the synthesized struct `(==)` compares float
 * fields through (design §3, §8). Args are the two floats' raw bit payloads. */
int32_t lvrt_canoneq(int64_t abits, int64_t bbits) {
    return lv_canon_bits((uint64_t)abits) == lv_canon_bits((uint64_t)bbits) ? 1 : 0;
}

/* Track 05 C3: Map key equality — primitives by value; structs field-wise
 * recursive (a struct IS its fields, §9); classes by identity. Mirrors
 * keyEquals in RuntimeValue.hpp and CGen.cpp's keyEq. */
int32_t lvrt_keyeq(const LvValue* a, const LvValue* b) {
    if (a->tag != b->tag) return 0;
    if (a->tag == LV_STR) return lvrt_str_eq(a, b);
    /* struct-equality design §5 pin (float_map_key_nan): Map keys are canonical
     * (§4). The old bit-compare below was already divergent from the
     * interpreters on ±0.0 keys (and treated NaN payloads as distinct keys);
     * routing LV_FLOAT through the canon fixes both. */
    if (a->tag == LV_FLOAT) return lvrt_canoneq(a->payload, b->payload);
    if (a->tag == LV_OBJ) {
        int64_t clsA = lv_ld_i64(a->payload, 0);
        int64_t clsB = lv_ld_i64(b->payload, 0);
        if (clsA != clsB || !lvrt_isvalueclass(clsA)) return a->payload == b->payload;
        int64_t n = lvrt_fieldcount(clsA);
        for (int64_t i = 0; i < n; i++) {
            LvValue fa = lv_ld_val(a->payload, 16 + 16 * i);
            LvValue fb = lv_ld_val(b->payload, 16 + 16 * i);
            if (!lvrt_keyeq(&fa, &fb)) return 0;
        }
        return 1;
    }
    return a->payload == b->payload;
}

void lvrt_idxget(LvValue* out, const LvValue* base, const LvValue* idx) {
    if (base->tag == LV_ARR) {
        int64_t rawlen = lv_ld_i64(base->payload, 0);
        if (LV_IS_COLUMNAR(rawlen)) {   /* columnar: gather a fresh standalone record (§5.2) */
            int64_t cleanLen = LV_ARR_CLEANLEN(rawlen);
            int64_t i = idx->payload;
            if (i < 0 || i >= cleanLen) { lvrt_raise_oob(i, cleanLen); out->tag = LV_VOID; out->payload = 0; return; }
            int64_t classId = lv_ld_i64(base->payload, 8);
            lv_col_gather(out, base, cleanLen, classId, lvrt_fieldcount(classId), i);
            return;
        }
        if (rawlen < 0) {   /* dense */
            int64_t cleanLen = rawlen & ~LV_DENSE_BIT;
            int64_t i = idx->payload;
            if (i < 0 || i >= cleanLen) { lvrt_raise_oob(i, cleanLen); out->tag = LV_VOID; out->payload = 0; return; }
            int64_t recBytes = lv_ld_i64(base->payload, 8);
            out->tag = LV_OBJ; out->payload = I64(P8(base->payload) + 16 + i * recBytes);
            return;
        }
        int64_t i = idx->payload;
        if (i < 0 || i >= rawlen) { lvrt_raise_oob(i, rawlen); out->tag = LV_VOID; out->payload = 0; return; }
        *out = lv_ld_val(base->payload, 8 + 16 * i);
        return;
    }
    if (base->tag == LV_MAP) {
        int64_t len = lv_ld_i64(base->payload, 0);
        for (int64_t i = 0; i < len; i++) {
            LvValue k = lv_ld_val(base->payload, 8 + 32 * i);
            if (lvrt_keyeq(&k, idx)) { *out = lv_ld_val(base->payload, 8 + 32 * i + 16); return; }
        }
        out->tag = LV_VOID; out->payload = 0;
        return;
    }
    out->tag = LV_VOID; out->payload = 0;
}

/* Store `val` into a map value slot with map-owned semantics: a value struct is
 * deep-copied (value semantics, §9) so each map owns its own; any other value is
 * retained. The slot must not already hold a live owned reference (the caller
 * reclaims the prior value first). See bug.md #49. */
static void lv_map_store_val(int64_t payload, int64_t off, const LvValue* val) {
    if (val->tag == LV_OBJ && lvrt_isvalueclass(lv_ld_i64(val->payload, 0))) {
        LvValue cp; lvrt_copyval(&cp, val);
        lv_st_val(payload, off, &cp);
    } else {
        lv_st_val(payload, off, val);
        lvrt_retain(val);
    }
}

static void lv_map_upsert(LvValue* out, const LvValue* base, const LvValue* idx, const LvValue* val) {
    int64_t oldlen = lv_ld_i64(base->payload, 0);
    int64_t foundIdx = -1;
    for (int64_t i = 0; i < oldlen; i++) {
        LvValue k = lv_ld_val(base->payload, 8 + 32 * i);
        if (lvrt_keyeq(&k, idx)) { foundIdx = i; break; }
    }
    int64_t rc = HDR(base->payload)[0];
    if (rc == 1 && foundIdx >= 0) {
        LvValue old = lv_ld_val(base->payload, 8 + 32 * foundIdx + 16);
        lvrt_release(&old);
        lvrt_vfree(&old);   /* a value-struct old value is reclaimed here (release no-ops it) */
        lv_map_store_val(base->payload, 8 + 32 * foundIdx + 16, val);
        *out = *base;
        return;
    }
    int64_t newlen = foundIdx >= 0 ? oldlen : oldlen + 1;
    LvValue fresh; lvrt_map_new(&fresh, newlen);
    memcpy(P8(fresh.payload) + 8, P8(base->payload) + 8, (size_t)(32 * oldlen));
    int64_t target = foundIdx >= 0 ? foundIdx : oldlen;
    int64_t* e = (int64_t*)(P8(fresh.payload) + 8 + 32 * target);
    e[0] = idx->tag; e[1] = idx->payload; e[2] = val->tag; e[3] = val->payload;
    for (int64_t i = 0; i < newlen; i++) {
        LvValue k = lv_ld_val(fresh.payload, 8 + 32 * i);
        LvValue v = lv_ld_val(fresh.payload, 8 + 32 * i + 16);
        lvrt_retain(&k);
        /* A value-struct value has VALUE semantics (§9): each map owns an
         * independent copy. The memcpy above aliased the source map's struct
         * pointer into `fresh`; without a deep copy both maps vfree the same
         * struct on free -> double-free (bug.md #49). retain/release no-op on
         * value structs, so refcount sharing cannot express this ownership. */
        lv_map_store_val(fresh.payload, 8 + 32 * i + 16, &v);
    }
    *out = fresh;
}

/* map.with — the CallDyn (+1 transfer) convention (bug.md #30), mirroring
 * lvrt_arr_append's stamped contract (§15). lv_map_upsert is written for the
 * IndexStore convention (dk==1: codegen retains the result on top), so its
 * FRESH-copy path leaves the new map at rc 0 — correct there, but WRONG under
 * CallDyn (dk==2), where nobody adds the +1 and the map is freed at the first
 * cross-frame return (the filed poison/segfault). Here every path crosses the
 * boundary OWNED: the in-place COW path returns base's SAME payload, whose
 * consumed +1 rides through untouched; a fresh copy (rc 0) is retained to +1.
 * The consumed receiver's fate stays with the CALL SITE (LlvmGen's `with` row,
 * which alone knows in.b) — never freed here. See
 * designs/complete/techdesign-bug30-map-with-ownership.md §4. */
void lvrt_map_with(LvValue* out, const LvValue* base,
                   const LvValue* idx, const LvValue* val) {
    lv_map_upsert(out, base, idx, val);
    if (out->payload != base->payload) lvrt_retain(out);   /* fresh: 0 -> 1 */
}

void lvrt_idxset(LvValue* out, const LvValue* base, const LvValue* idx, const LvValue* val) {
    if (base->tag == LV_ARR) {
        int64_t rawlen = lv_ld_i64(base->payload, 0);
        if (LV_IS_COLUMNAR(rawlen)) {   /* columnar: whole-buffer COW, scatter one element (§6.1) */
            int64_t cleanLen = LV_ARR_CLEANLEN(rawlen);
            int64_t i = idx->payload;
            int64_t classId = lv_ld_i64(base->payload, 8);
            int64_t f = lvrt_fieldcount(classId);
            int64_t rc = HDR(base->payload)[0];
            if (rc == 1) {              /* uniquely owned: scatter in place */
                if (i >= 0 && i < cleanLen) lv_col_scatter(base->payload, cleanLen, f, i, val);
                *out = *base;
                return;
            }
            /* shared: copy the whole column region, scatter into the copy */
            int64_t colBytes = f * 8 * cleanLen;
            int64_t payload = lv_halloc_prefixed(16 + colBytes);
            lv_st_i64(payload, 0, rawlen); lv_st_i64(payload, 8, classId);
            memcpy(P8(payload) + 16, P8(base->payload) + 16, (size_t)colBytes);
            if (i >= 0 && i < cleanLen) lv_col_scatter(payload, cleanLen, f, i, val);
            out->tag = LV_ARR; out->payload = payload;
            return;
        }
        if (rawlen < 0) {   /* dense: value-struct records, no element refs */
            int64_t cleanLen = rawlen & ~LV_DENSE_BIT;
            int64_t i = idx->payload;
            int64_t recBytes = lv_ld_i64(base->payload, 8);
            int64_t rc = HDR(base->payload)[0];
            if (rc == 1) {
                if (i >= 0 && i < cleanLen)
                    memcpy(P8(base->payload) + 16 + i * recBytes, P8(val->payload), (size_t)recBytes);
                *out = *base;
                return;
            }
            int64_t payload = lv_halloc_prefixed(16 + cleanLen * recBytes);
            lv_st_i64(payload, 0, rawlen); lv_st_i64(payload, 8, recBytes);
            memcpy(P8(payload) + 16, P8(base->payload) + 16, (size_t)(cleanLen * recBytes));
            if (i >= 0 && i < cleanLen)
                memcpy(P8(payload) + 16 + i * recBytes, P8(val->payload), (size_t)recBytes);
            out->tag = LV_ARR; out->payload = payload;
            return;
        }
        int64_t rc = HDR(base->payload)[0];
        int64_t i = idx->payload;
        if (rc == 1) {
            if (i >= 0 && i < rawlen) {
                LvValue old = lv_ld_val(base->payload, 8 + 16 * i);
                lvrt_release(&old);
                lv_st_val(base->payload, 8 + 16 * i, val);
                lvrt_retain(val);
            }
            *out = *base;
            return;
        }
        LvValue fresh; lvrt_arr_new(&fresh, rawlen);
        memcpy(P8(fresh.payload) + 8, P8(base->payload) + 8, (size_t)(16 * rawlen));
        if (i >= 0 && i < rawlen) lv_st_val(fresh.payload, 8 + 16 * i, val);
        for (int64_t k = 0; k < rawlen; k++) {
            LvValue elem = lv_ld_val(fresh.payload, 8 + 16 * k);
            lvrt_retain(&elem);
        }
        out->tag = LV_ARR; out->payload = fresh.payload;
        return;
    }
    if (base->tag == LV_MAP) { lv_map_upsert(out, base, idx, val); return; }
    *out = *base;   /* unknown base: return as-is (matches X64Gen::genIdxSet fallback) */
}

/* ========================================================================
 * Scalar core, stringify, print, minimal IO — the entry points the LLVM
 * backend's scalar core emits (ratified into lv_abi.h at the 2026-07-05
 * maintenance pass; they predate B-M1 as the NativeRuntime/stub contract).
 * lvrt_to_string is the ts_build analogue: byte-matched to valueToString
 * (src/RuntimeValue.hpp:75-114), composed via concat's consume-unowned
 * discipline so builder chains stay flat.
 * ==================================================================== */

int32_t lvrt_truth(const LvValue* v) {
    return v->tag == LV_BOOL ? (int32_t)(v->payload != 0) : 0;
}

void lvrt_not(LvValue* out, const LvValue* a) {
    out->tag = LV_BOOL;
    out->payload = !lvrt_truth(a);
}

void lvrt_neg(LvValue* out, const LvValue* a) {
    if (a->tag == LV_FLOAT) {
        double d; memcpy(&d, &a->payload, 8);
        d = -d;
        out->tag = LV_FLOAT; memcpy(&out->payload, &d, 8);
        return;
    }
    out->tag = LV_INT; out->payload = -a->payload;
}

static double lv_as_double(const LvValue* v) {
    if (v->tag == LV_FLOAT) { double d; memcpy(&d, &v->payload, 8); return d; }
    return (double)v->payload;
}

void lvrt_to_string(LvValue* out, const LvValue* v);

/* helper: append a C-literal piece to a growing rc-0 accumulator */
static void lv_ts_append_lit(LvValue* acc, const char* lit) {
    LvValue piece; lvrt_str_new(&piece, lit, (int64_t)strlen(lit));
    LvValue next; lvrt_str_concat(&next, acc, &piece);   /* consumes both rc-0 temps */
    *acc = next;
}
static void lv_ts_append_val(LvValue* acc, const LvValue* v) {
    LvValue piece; lvrt_to_string(&piece, v);
    LvValue next; lvrt_str_concat(&next, acc, &piece);   /* consumes rc-0; borrows owned */
    *acc = next;
}

void lvrt_to_string(LvValue* out, const LvValue* v) {
    switch (v->tag) {
        case LV_INT: lvrt_int_to_str(out, v->payload); return;
        case LV_BOOL:
            if (v->payload) lvrt_str_new(out, "true", 4);
            else lvrt_str_new(out, "false", 5);
            return;
        case LV_FLOAT: {
            /* valueToString uses std::to_string(double) == printf "%f" */
            char buf[64];
            double d; memcpy(&d, &v->payload, 8);
            int n = snprintf(buf, sizeof buf, "%f", d);
            lvrt_str_new(out, buf, n > 0 ? (int64_t)n : 0);
            return;
        }
        case LV_STR: *out = *v; return;   /* pass-through self (ts_build parity) */
        case LV_CHAR: lvrt_char_to_string(out, v->payload); return;   /* UTF-8 encode */
        case LV_BLOCK: {                  /* "Block(len=N)" (valueToString parity) */
            char buf[40];
            int n = snprintf(buf, sizeof buf, "Block(len=%lld)",
                             (long long)lv_ld_i64(v->payload, 16));
            lvrt_str_new(out, buf, n > 0 ? (int64_t)n : 0);
            return;
        }
        case LV_NONE: lvrt_str_new(out, "None", 4); return;
        case LV_OBJ: {
            const LvClassInfo* c = lv_find_class(lv_ld_i64(v->payload, 0));
            if (c && strcmp(c->name, "Range") == 0) {   /* "start..end" */
                LvValue s, e;
                lvrt_getfield(&s, v, "start");
                lvrt_getfield(&e, v, "end");
                LvValue acc; lvrt_int_to_str(&acc, s.payload);
                lv_ts_append_lit(&acc, "..");
                LvValue es; lvrt_int_to_str(&es, e.payload);
                LvValue next; lvrt_str_concat(&next, &acc, &es);
                *out = next;
                return;
            }
            lvrt_str_new(out, "<object>", 8);
            return;
        }
        case LV_CLO: lvrt_str_new(out, "<closure>", 9); return;
        case LV_ARR: {
            LvValue acc; lvrt_str_new(&acc, "[", 1);
            int64_t rawlen = lv_ld_i64(v->payload, 0);
            if (rawlen < 0) {   /* dense: inline value-struct records */
                int64_t cleanLen = rawlen & ~LV_DENSE_BIT;
                int64_t recBytes = lv_ld_i64(v->payload, 8);
                for (int64_t i = 0; i < cleanLen; i++) {
                    if (i) lv_ts_append_lit(&acc, ", ");
                    LvValue elem;
                    elem.tag = LV_OBJ;
                    elem.payload = I64(P8(v->payload) + 16 + i * recBytes);
                    lv_ts_append_val(&acc, &elem);
                }
            } else {
                for (int64_t i = 0; i < rawlen; i++) {
                    if (i) lv_ts_append_lit(&acc, ", ");
                    LvValue elem = lv_ld_val(v->payload, 8 + 16 * i);
                    lv_ts_append_val(&acc, &elem);
                }
            }
            lv_ts_append_lit(&acc, "]");
            *out = acc;
            return;
        }
        case LV_MAP: {
            LvValue acc; lvrt_str_new(&acc, "{", 1);
            int64_t len = lv_ld_i64(v->payload, 0);
            for (int64_t i = 0; i < len; i++) {
                if (i) lv_ts_append_lit(&acc, ", ");
                LvValue k = lv_ld_val(v->payload, 8 + 32 * i);
                LvValue val = lv_ld_val(v->payload, 8 + 32 * i + 16);
                lv_ts_append_val(&acc, &k);
                lv_ts_append_lit(&acc, ": ");
                lv_ts_append_val(&acc, &val);
            }
            lv_ts_append_lit(&acc, "}");
            *out = acc;
            return;
        }
        default: lvrt_str_new(out, "", 0); return;   /* void */
    }
}

/* op numbering = LV_OP_* (X64Gen::opCode). See lv_abi.h for the per-row
 * type rules; the string ==/!= mixed-type rule (mixed -> unequal, never
 * stringify-and-compare) follows the oracle, deliberately NOT the A-M2
 * stub (which stringified both sides — "1" == 1 would wrongly be true). */
void lvrt_arith(int32_t op, LvValue* out, const LvValue* l, const LvValue* r) {
    /* bug.md #4: None compares by TAG, never payload — checked first, same as
     * the interpreters' arithPrim (RuntimeValue.hpp). Every other row below
     * (the LV_STR check included) falls through to the plain `l->payload ==
     * r->payload` int64 comparison for LV_OP_EQ/NE when neither operand is a
     * string/float, so `int? a = 0; a != None` compared PAYLOAD (0 == 0) and
     * read as None even though the tags (LV_INT vs LV_NONE) differ. */
    if (l->tag == LV_NONE || r->tag == LV_NONE) {
        if (op == LV_OP_EQ) { out->tag = LV_BOOL; out->payload = (l->tag == r->tag); return; }
        if (op == LV_OP_NE) { out->tag = LV_BOOL; out->payload = (l->tag != r->tag); return; }
        out->tag = LV_VOID; out->payload = 0; return;
    }
    if (l->tag == LV_STR || r->tag == LV_STR) {
        if (op == LV_OP_ADD) {
            LvValue ls, rs;
            lvrt_to_string(&ls, l);
            lvrt_to_string(&rs, r);
            lvrt_str_concat(out, &ls, &rs);   /* consumes unowned temps */
            return;
        }
        int bothStr = (l->tag == LV_STR && r->tag == LV_STR);
        if (op == LV_OP_EQ || op == LV_OP_NE) {
            int eq = bothStr ? lvrt_str_eq(l, r) : 0;
            out->tag = LV_BOOL;
            out->payload = (op == LV_OP_EQ) ? eq : !eq;
            return;
        }
        if (bothStr && op >= LV_OP_LT && op <= LV_OP_GE) {   /* lexicographic */
            int64_t la = lv_ld_i64(l->payload, 0), lb = lv_ld_i64(r->payload, 0);
            int64_t n = la < lb ? la : lb;
            int c = memcmp(P8(l->payload) + 8, P8(r->payload) + 8, (size_t)n);
            if (c == 0) c = (la < lb) ? -1 : (la > lb ? 1 : 0);
            out->tag = LV_BOOL;
            out->payload = (op == LV_OP_LT) ? c < 0 : (op == LV_OP_GT) ? c > 0
                         : (op == LV_OP_LE) ? c <= 0 : c >= 0;
            return;
        }
        out->tag = LV_VOID; out->payload = 0;
        return;
    }
    if (l->tag == LV_FLOAT || r->tag == LV_FLOAT) {
        double a = lv_as_double(l), b = lv_as_double(r);
        double d;
        switch (op) {
            case LV_OP_ADD: d = a + b; break;
            case LV_OP_SUB: d = a - b; break;
            case LV_OP_MUL: d = a * b; break;
            /* bug.md #12: IEEE semantics (inf/nan), matching --run/--ir/
             * emit-C++/ELF — no divide-by-zero override. */
            case LV_OP_DIV: d = a / b; break;
            case LV_OP_MOD: d = fmod(a, b); break;
            case LV_OP_EQ: out->tag = LV_BOOL; out->payload = a == b; return;
            case LV_OP_NE: out->tag = LV_BOOL; out->payload = a != b; return;
            case LV_OP_LT: out->tag = LV_BOOL; out->payload = a < b;  return;
            case LV_OP_GT: out->tag = LV_BOOL; out->payload = a > b;  return;
            case LV_OP_LE: out->tag = LV_BOOL; out->payload = a <= b; return;
            case LV_OP_GE: out->tag = LV_BOOL; out->payload = a >= b; return;
            default: out->tag = LV_VOID; out->payload = 0; return;
        }
        out->tag = LV_FLOAT; memcpy(&out->payload, &d, 8);
        return;
    }
    int64_t a = l->payload, b = r->payload;
    switch (op) {
        case LV_OP_ADD: out->tag = LV_INT; out->payload = a + b; return;
        case LV_OP_EQ:  out->tag = LV_BOOL; out->payload = a == b; return;
        case LV_OP_NE:  out->tag = LV_BOOL; out->payload = a != b; return;
        case LV_OP_SUB: out->tag = LV_INT; out->payload = a - b; return;
        case LV_OP_MUL: out->tag = LV_INT; out->payload = a * b; return;
        /* DIV/MOD by zero raise a catchable RuntimeException (§3.7, bug.md #10),
         * same shape as the shift range check below. LLVM inlines the int-int
         * fast path (LlvmGen's ar.int.divbad) so this fallback is not normally
         * reached, but it is kept at the ratified semantics for parity. Floats
         * keep IEEE inf/nan (bug.md #12). */
        case LV_OP_DIV:
            if (b == 0) { lvrt_raise("division by zero");
                          out->tag = LV_VOID; out->payload = 0; return; }
            out->tag = LV_INT; out->payload = a / b; return;
        case LV_OP_MOD:
            if (b == 0) { lvrt_raise("modulo by zero");
                          out->tag = LV_VOID; out->payload = 0; return; }
            out->tag = LV_INT; out->payload = a % b; return;
        case LV_OP_LT:  out->tag = LV_BOOL; out->payload = a < b;  return;
        case LV_OP_GT:  out->tag = LV_BOOL; out->payload = a > b;  return;
        case LV_OP_LE:  out->tag = LV_BOOL; out->payload = a <= b; return;
        case LV_OP_GE:  out->tag = LV_BOOL; out->payload = a >= b; return;
        case LV_OP_AND: out->tag = LV_INT; out->payload = a & b; return;
        case LV_OP_OR:  out->tag = LV_INT; out->payload = a | b; return;
        /* Shift counts outside 0..63 raise a catchable RuntimeException (Track
         * 01 F1 ruling — arithPrim/X64Gen genAr parity), never x86's silent
         * mask-to-6-bits. Unreachable from LLVM-emitted code today (the int-int
         * inline fast path range-checks before this fallback is callable); kept
         * at the ratified semantics so no future caller inherits the mask. */
        case LV_OP_SHL:
            if ((uint64_t)b > 63) { lvrt_raise("shift count out of range");
                                    out->tag = LV_VOID; out->payload = 0; return; }
            out->tag = LV_INT; out->payload = (int64_t)((uint64_t)a << b); return;
        case LV_OP_SHR:
            if ((uint64_t)b > 63) { lvrt_raise("shift count out of range");
                                    out->tag = LV_VOID; out->payload = 0; return; }
            out->tag = LV_INT; out->payload = a >> b; return;
        case LV_OP_XOR: out->tag = LV_INT; out->payload = a ^ b; return;
        default: out->tag = LV_VOID; out->payload = 0; return;
    }
}

/* print consumes an unowned rc-0 string operand — the dropped-temporary
 * discipline (matches X64Gen's print path + emitStrTempFree): the rendered
 * temp always dies here; an owned (rc>=1) pass-through string is untouched. */
void lvrt_print_val(const LvValue* v) {
    LvValue s; lvrt_to_string(&s, v);
    lv_plat_write(1, P8(s.payload) + 8, lv_ld_i64(s.payload, 0));
    lv_str_temp_free(&s);
}

void lvrt_print_nl(void) {
    lv_plat_write(1, "\n", 1);
}

/* sysWrite BORROWS its string arg (§2.7 native row); non-string args are
 * stringified through a consumed temp. Returns LV_INT byte count. */
void lvrt_syswrite(LvValue* out, const LvValue* fd, const LvValue* s) {
    int64_t n;
    if (s->tag == LV_STR) {
        n = lv_ld_i64(s->payload, 0);
        lv_plat_write((int)fd->payload, P8(s->payload) + 8, n);
    } else {
        LvValue t; lvrt_to_string(&t, s);
        n = lv_ld_i64(t.payload, 0);
        lv_plat_write((int)fd->payload, P8(t.payload) + 8, n);
        lv_str_temp_free(&t);
    }
    out->tag = LV_INT; out->payload = n;
}

void lvrt_readline(LvValue* out, const LvValue* fd) {
    char buf[4096];
    int64_t len = 0;
    while (len < (int64_t)sizeof buf) {
        char c;
        int64_t got = lv_plat_read((int)fd->payload, &c, 1);
        if (got <= 0 || c == '\n') break;
        buf[len++] = c;
    }
    lvrt_str_new(out, buf, len);
}

/* ========================================================================
 * §2.7 B-M3 — file natives (sysOpen/sysClose/sysStat/sysRead). Behavior
 * extracted from src/RuntimeNatives.cpp; string payloads are already NUL-
 * terminated (§2.4's C-string-interop deviation) so no cstr copy is needed.
 * ==================================================================== */

void lvrt_sysopen(LvValue* out, const LvValue* path, const LvValue* flagBits) {
    const char* cpath = (const char*)(P8(path->payload) + 8);
    out->tag = LV_INT;
    out->payload = lv_plat_open(cpath, (int)flagBits->payload, 0644);
}

void lvrt_sysclose(const LvValue* fd) {
    /* LA-2 §10 #8: tear down the TLS session (if any) FIRST — one best-effort
     * close_notify + free, table slot cleared — before the descriptor is
     * released below, so an fd-number reused by the next connection can never
     * inherit a stale session. No-op on a plaintext fd. This native is the
     * single funnel TcpStream's close routes through. */
    lv_tls_close((int)fd->payload);
    lv_plat_close((int)fd->payload);
}

/* Track 08 F2 (C6): wall-clock epoch milliseconds. DateTime.now() is its one
 * consumer today (Track 09). Result crosses back as LvValue* (boundary rule). */
void lvrt_sysnow(LvValue* out) {
    out->tag = LV_INT;
    out->payload = lv_plat_now_realtime_ms();
}

/* Track 08 F2: CLOCK_MONOTONIC milliseconds — durations/deadlines (the F5
 * connect-timeout flows measure elapsed with it). Rides the floor's existing
 * monotonic lv_plat_now_ns; no new plat surface. */
void lvrt_sysmonotonic(LvValue* out) {
    out->tag = LV_INT;
    out->payload = lv_plat_now_ns() / 1000000;
}

/* field: 0=exists(0/1) 1=size 2=mtime 3=isDir; -1 for size/mtime/isDir of a
 * missing path (RuntimeNatives.cpp parity — this floor only exposes
 * stat_size, so mtime isn't available yet; -1 until a Block-backed stat
 * struct lands, matching the oracle's own "interim shape" comment). */
void lvrt_sysstat(LvValue* out, const LvValue* path, const LvValue* field) {
    const char* cpath = (const char*)(P8(path->payload) + 8);
    int64_t size = lv_plat_stat_size(cpath);
    out->tag = LV_INT;
    /* field 0=exists(0/1) 1=size 2=mtime 3=isDir; -1 for a missing path's
     * size/mtime/isDir (byte-parity with RuntimeNatives.cpp's sysStat,
     * src/RuntimeNatives.cpp :1312-1326 — field 2 returns real st_mtime
     * epoch seconds, field 3 is the request-stat-isdir.md S_ISDIR probe). */
    if (field->payload == 0) out->payload = size >= 0 ? 1 : 0;
    else if (field->payload == 1) out->payload = size;
    else if (field->payload == 2) out->payload = lv_plat_stat_mtime(cpath);
    else if (field->payload == 3) out->payload = lv_plat_stat_isdir(cpath);
    else out->payload = -1;
}

/* Track 08 F3 — sysMkdir on the compiled backends. Scalar-in (path),
 * scalar-out (0 ok / -1 fail), the sysStat/sysOpen shape; byte-parity with the
 * oracle's RuntimeNatives.cpp sysMkdir. */
void lvrt_sysmkdir(LvValue* out, const LvValue* path) {
    const char* cpath = (const char*)(P8(path->payload) + 8);
    out->tag = LV_INT;
    out->payload = lv_plat_mkdir(cpath);
}

void lvrt_sysread(LvValue* out, const LvValue* fd, const LvValue* max) {
    int64_t m = max->payload;
    if (m <= 0) { lvrt_str_new(out, "", 0); return; }
    char buf[m];   /* VLA: freed on return, no arena growth from repeated large reads */
    int64_t n = lv_plat_read((int)fd->payload, buf, m);
    lvrt_str_new(out, buf, n > 0 ? n : 0);
}

/* Track 03 M4 — zero-copy Block I/O overloads (lv_abi.h §6.6.5). The Block is
 * BORROWED (no ARC transfer). Read/recv fill it from offset 0; write/send take
 * an explicit [off, off+len) window, bounds-checked against the VIEW length and
 * raised loud on overflow (like the byte accessors). No interpreter sink: a
 * native binary's fd 1/2 is captured directly. lv_block_at()/lv_plat_* are all
 * in scope here, so all four live together rather than split with the string
 * forms (send/recv strings are in lv_loop.c). */
void lvrt_syswrite_block(LvValue* out, const LvValue* fd, const LvValue* b,
                         const LvValue* off, const LvValue* len) {
    int64_t blen = lv_ld_i64(b->payload, 16);
    int64_t o = off->payload, l = len->payload;
    if (o < 0 || l < 0 || o + l > blen) { lvrt_raise_oob(o, blen); out->tag = LV_VOID; out->payload = 0; return; }
    out->tag = LV_INT;
    out->payload = lv_plat_write((int)fd->payload, lv_block_at(b->payload, o), l);
}

void lvrt_sysread_block(LvValue* out, const LvValue* fd, const LvValue* b, const LvValue* max) {
    int64_t blen = lv_ld_i64(b->payload, 16);
    int64_t m = max->payload;
    if (m < 0) m = 0;
    if (m > blen) m = blen;
    int64_t n = m > 0 ? lv_plat_read((int)fd->payload, lv_block_at(b->payload, 0), m) : 0;
    out->tag = LV_INT; out->payload = n > 0 ? n : 0;
}

void lvrt_syssend_block(LvValue* out, const LvValue* fd, const LvValue* b,
                        const LvValue* off, const LvValue* len) {
    int64_t blen = lv_ld_i64(b->payload, 16);
    int64_t o = off->payload, l = len->payload;
    if (o < 0 || l < 0 || o + l > blen) { lvrt_raise_oob(o, blen); out->tag = LV_VOID; out->payload = 0; return; }
    out->tag = LV_INT;
    int fdv = (int)fd->payload;
    if (lv_tls_is(fdv)) { out->payload = lv_tls_send(fdv, lv_block_at(b->payload, o), l); return; }
    out->payload = lv_plat_send(fdv, lv_block_at(b->payload, o), l);
}

/* three-state like the string form: None = peer closed; 0 = nothing now; n>0 bytes. */
void lvrt_sysrecv_block(LvValue* out, const LvValue* fd, const LvValue* b, const LvValue* max) {
    int64_t blen = lv_ld_i64(b->payload, 16);
    int64_t m = max->payload;
    if (m < 0) m = 0;
    if (m > blen) m = blen;
    int fdv = (int)fd->payload;
    if (lv_tls_is(fdv)) {
        int64_t r = m > 0 ? lv_tls_recv(fdv, lv_block_at(b->payload, 0), m) : 0;
        if (r > 0) { out->tag = LV_INT; out->payload = r; return; }       /* data */
        if (r == -1) { out->tag = LV_INT; out->payload = 0; return; }     /* would-block -> 0 */
        out->tag = LV_NONE; out->payload = 0; return;                     /* clean-EOF / fatal */
    }
    int64_t n = m > 0 ? lv_plat_recv(fdv, lv_block_at(b->payload, 0), m) : 0;
    if (n == 0 && m > 0) { out->tag = LV_NONE; out->payload = 0; return; }
    out->tag = LV_INT; out->payload = n > 0 ? n : 0;
}

/* ========================================================================
 * LA-2 (techdesign-tls-crypto.md §5.2) — TLS + crypto ABI shims over the
 * lv_tls_* / lv_rsa_encrypt provider seam + lv_plat_random. String args are
 * BORROWED (null-terminated in the runtime heap at payload+8); string returns
 * are FRESH (+1, lvrt_str_new — LlvmGen retainDst mirrors sysRecv's string).
 * ==================================================================== */
static const char* lv_cstr(const LvValue* v) {
    return (const char*)(const void*)(intptr_t)(v->payload + 8);
}
void lvrt_systlsconnect(LvValue* out, const LvValue* fd, const LvValue* host,
                        const LvValue* alpn, const LvValue* caFile, const LvValue* verify) {
    out->tag = LV_INT;
    out->payload = lv_tls_client_start((int)fd->payload, lv_cstr(host), lv_cstr(alpn),
                                       lv_cstr(caFile), (int)verify->payload);
}
void lvrt_systlsaccept(LvValue* out, const LvValue* fd, const LvValue* cert,
                       const LvValue* key, const LvValue* alpn) {
    out->tag = LV_INT;
    out->payload = lv_tls_server_start((int)fd->payload, lv_cstr(cert), lv_cstr(key), lv_cstr(alpn));
}
void lvrt_systlshandshake(LvValue* out, const LvValue* fd) {
    out->tag = LV_INT; out->payload = lv_tls_handshake((int)fd->payload);
}
void lvrt_systlserror(LvValue* out, const LvValue* fd) {
    const char* e = lv_tls_error((int)fd->payload);
    lvrt_str_new(out, e, (int64_t)strlen(e));
}
void lvrt_systlsalpn(LvValue* out, const LvValue* fd) {
    const char* a = lv_tls_alpn((int)fd->payload);
    lvrt_str_new(out, a, (int64_t)strlen(a));
}
void lvrt_systlsversion(LvValue* out, const LvValue* fd) {
    out->tag = LV_INT; out->payload = lv_tls_version((int)fd->payload);
}
/* §7: RSA public-key encrypt. `bytes` is BINARY (password ⊕ scramble — may hold
 * NULs), so read it length-prefixed, not as a C string. Fresh string | LV_NONE. */
void lvrt_sysrsaencrypt(LvValue* out, const LvValue* pem, const LvValue* bytes, const LvValue* pad) {
    const char* pemP = lv_cstr(pem);
    int64_t pemLen = *(const int64_t*)(const void*)(intptr_t)pem->payload;
    const void* inP = (const void*)(const void*)(intptr_t)(bytes->payload + 8);
    int64_t inLen = *(const int64_t*)(const void*)(intptr_t)bytes->payload;
    int padi = strcmp(lv_cstr(pad), "pkcs1") == 0 ? 1 : 0;
    unsigned char buf[1024];   /* RSA-4096 ciphertext = 512 bytes; 1024 is ample */
    int64_t n = lv_rsa_encrypt(pemP, pemLen, inP, inLen, buf, (int64_t)sizeof buf, padi);
    if (n < 0) { out->tag = LV_NONE; out->payload = 0; return; }
    lvrt_str_new(out, (const char*)buf, n);
}
/* §8: sysRandom(n) — n crypto-grade random bytes in a string. n<=0 -> "";
 * n>1MB -> the exact RuntimeException the oracle raises. Byte-parity with
 * RuntimeNatives.cpp's sysRandom (getrandom via lv_plat_random). */
void lvrt_sysrandom(LvValue* out, const LvValue* n) {
    int64_t k = n->payload;
    if (k <= 0) { lvrt_str_new(out, "", 0); return; }
    if (k > (1LL << 20)) {
        char msg[96];
        snprintf(msg, sizeof msg,
                 "sysRandom: requested %lld bytes exceeds the 1MB sanity bound", (long long)k);
        lvrt_raise(msg); out->tag = LV_VOID; out->payload = 0; return;
    }
    char buf[k];   /* VLA: k <= 1MB, freed on return */
    if (lv_plat_random(buf, k) != k) { lvrt_raise("sysRandom: getrandom failed"); out->tag = LV_VOID; out->payload = 0; return; }
    lvrt_str_new(out, buf, k);
}

/* bug #68: env::get's `sysEnv` floor native on the COMPILED backends — the
 * interpreters had it (RuntimeNatives.cpp) but LlvmGen's Op::Native switch did
 * not, so a NON-inlined `env::get(...)` call (a package helper the optimizer
 * chose not to inline) hit "native floor function 'sysEnv'". A string payload
 * is NUL-terminated (lv_abi.h string layout: {len, bytes[len], nul}), so its
 * bytes at offset 8 can be handed straight to getenv. None == variable unset
 * (distinct from set-but-empty, the three-state fact env::get returns). */
void lvrt_sysenv(LvValue* out, const LvValue* key) {
    const char* k = (const char*)(P8(key->payload) + 8);
    const char* v = getenv(k);
    if (v) lvrt_str_new(out, v, (int64_t)strlen(v));
    else { out->tag = LV_NONE; out->payload = 0; }
}

/* terminal raw mode (designs/terminal-raw-mode.md §3.2). Thin wrappers over the
 * platform floor, returning the int result in the dest (0 ok / -1 fail), the
 * sysStat/sysOpen shape. Scalar-in, scalar-out. */
void lvrt_systermraw(LvValue* out, const LvValue* fd) {
    out->tag = LV_INT;
    out->payload = (int64_t)lv_plat_term_raw((int)fd->payload);
}
void lvrt_systermrestore(LvValue* out, const LvValue* fd) {
    out->tag = LV_INT;
    out->payload = (int64_t)lv_plat_term_restore((int)fd->payload);
}

/* Terminal size + raw-state query (designs/techdesign-terminal-floor.md §2).
 * sysWinSize(fd, field): field 0 = rows, 1 = cols; -1 on failure or an
 * unknown field — the sysStat field-indexed shape (avoids a multi-return
 * native). sysTermIsRaw(fd): 1 if raw mode is active, else 0 (fd advisory —
 * the floor tracks the one raw fd), consumed only by term::size()'s
 * cursor-report fallback guard. */
void lvrt_syswinsize(LvValue* out, const LvValue* fd, const LvValue* field) {
    out->tag = LV_INT;
    int rows = 0, cols = 0;
    if (lv_plat_term_size((int)fd->payload, &rows, &cols) != 0) { out->payload = -1; return; }
    long long f = field->payload;
    out->payload = f == 0 ? rows : (f == 1 ? cols : -1);
}
void lvrt_systermisraw(LvValue* out, const LvValue* fd) {
    (void)fd;
    out->tag = LV_BOOL;
    out->payload = lv_plat_term_israw() ? 1 : 0;
}

/* Signals as streams (designs/techdesign-terminal-floor.md §3). The natives
 * are thin over the platform floor; the language builds the stream on top via
 * sysWatch. sysSignalOpen takes a boxed Array<int> of signal numbers (ints are
 * never dense, so only the boxed layout can occur, §2.4): len at offset 0,
 * element i's int payload at 16 + i*16. Returns the readable fd, or -1. */
void lvrt_syssignalopen(LvValue* out, const LvValue* arr) {
    out->tag = LV_INT;
    if (arr->tag != LV_ARR) { out->payload = -1; return; }
    int64_t len = lv_ld_i64(arr->payload, 0);
    if (len < 0) { out->payload = -1; return; }        /* dense bit set => not ints */
    int sigs[64];
    if (len > 64) len = 64;
    for (int64_t i = 0; i < len; i++)
        sigs[i] = (int)lv_ld_i64(arr->payload, 16 + i * 16);
    out->payload = (int64_t)lv_plat_signal_open(sigs, (int)len);
}
void lvrt_syssignalnext(LvValue* out, const LvValue* fd) {
    out->tag = LV_INT;
    out->payload = (int64_t)lv_plat_signal_next((int)fd->payload);
}
void lvrt_syssignalclose(LvValue* out, const LvValue* fd) {
    lv_plat_signal_close((int)fd->payload);
    out->tag = LV_INT;
    out->payload = 0;
}

/* Restore the terminal iff a program left it in raw mode — the guaranteed
 * restore-on-exit (designs/terminal-raw-mode.md §3.4). Called from EVERY exit
 * path (normal entry return, uncaught reporter, lv_die), so a TUI that dies in
 * raw mode never orphans the user's shell. No-op when raw was never entered
 * (the floor's g_raw_active guard). A single tcsetattr — async-signal-safe. */
void lvrt_term_shutdown(void) {
    lv_plat_term_restore(0);   /* fd advisory; the floor restores the saved fd */
}

/* argv, captured verbatim from the process entry (lv_entry.c -> lv_rt_init).
 * These are the OS's own char* pointers; the runtime never frees them (they
 * outlive it) and copies bytes out lazily in lvrt_sysargs. On the transitional
 * lazy-init path (lv_ensure_init -> lv_rt_init(0, NULL)) they stay 0/NULL and
 * sysArgs degrades to a single "" element (designs/argv.md §5.1, §9). */
static int    g_argc = 0;
static char** g_argv = 0;

/* sysArgs(): the process argument vector as a fresh boxed Array<string>,
 * argv[0] first (designs/argv.md §4/§5.1). Materialized on demand — nothing is
 * eager. Length is always >= 1: with no captured argv it is [""], never empty,
 * so callers never guard a zero-length array. Bytes are copied verbatim via
 * lvrt_str_new (strings are byte strings — no UTF-8 decode, non-UTF-8 argv
 * round-trips; §9). Each element is owned by the array buffer at +1 (store then
 * retain, exactly lv_arr_boxed_append's discipline); the returned array itself
 * is fresh/unowned (rc 0) — the caller/codegen retains it (+1 transfer). */
void lvrt_sysargs(LvValue* out) {
    int64_t n = (g_argv && g_argc > 0) ? (int64_t)g_argc : 1;
    lvrt_arr_new(out, n);                       /* rc 0, length n, slots void */
    for (int64_t i = 0; i < n; i++) {
        const char* a = (g_argv && i < g_argc && g_argv[i]) ? g_argv[i] : "";
        LvValue s; lvrt_str_new(&s, a, (int64_t)strlen(a));   /* rc 0 fresh */
        lv_st_val(out->payload, 8 + 16 * i, &s);
        lvrt_retain(&s);                          /* buffer owns element: rc 0->1 */
    }
}

/* ========================================================================
 * §2.8 — Exception state.
 * ==================================================================== */

LV_TLS int32_t lv_g_throwing;
static LV_TLS LvValue g_thrown;

void lvrt_throw_set(const LvValue* v) {
    lvrt_retain(v);
    lv_g_throwing = 1;
    g_thrown = *v;
}
int32_t lvrt_throwing(void) { return lv_g_throwing; }
void lvrt_thrown(LvValue* out) { *out = g_thrown; }
void lvrt_catch_bind(LvValue* bindSlot) {
    lv_g_throwing = 0;
    lvrt_release(bindSlot);
    *bindSlot = g_thrown;
    g_thrown.tag = LV_VOID; g_thrown.payload = 0;
}

const char* const lv_key_message = "message";

/* LA-30 B2 (doc 06 §2): raise a NAMED prelude exception class — the same
 * name-row lookup lvrt_raise always did, parameterized so the cancellation
 * carrier (CancelledException) rides the identical machinery as
 * RuntimeException. The class row must be registered (LlvmGen's idChain
 * pre-seed); a missing row falls back to RuntimeException's so a catchable
 * exception always flies (belt-and-braces, mirrors the rex==NULL guard). */
void lvrt_raise_cls(const char* clsName, const char* msg) {
    const LvClassInfo* rex = lv_find_class_by_name(clsName);
    if (!rex) rex = lv_find_class_by_name("RuntimeException");
    LvValue exc;
    if (rex) lvrt_obj_new(&exc, rex->classId);
    else { exc.tag = LV_OBJ; exc.payload = 0; }   /* no class table registered (unit-level selftest) */
    LvValue msgVal; lvrt_str_new(&msgVal, msg, (int64_t)strlen(msg));
    if (rex) {
        lvrt_setfield(&exc, lv_key_message, &msgVal);
        lvrt_retain(&msgVal);   /* raw setfield bypasses ARC; the field owns the message */
    }
    lvrt_throw_set(&exc);
}

void lvrt_raise(const char* msg) {
    lvrt_raise_cls("RuntimeException", msg);
}

/* Track 10 §3.3: raise a RuntimeException from an lv STRING value — the Await
 * rethrow of a failed Worker's failMessage. lv strings are NUL-terminated
 * (lvrt_str_new), so the bytes at payload+8 are a valid C string. */
void lvrt_raise_str(const LvValue* s) {
    if (s && s->tag == LV_STR) lvrt_raise((const char*)(intptr_t)(s->payload + 8));
    else lvrt_raise("worker failed");
}

void lvrt_raise_oob(int64_t idx, int64_t len) {
    char buf[96];
    LvValue is; lvrt_int_to_str(&is, idx);
    LvValue ls; lvrt_int_to_str(&ls, len);
    int64_t ilen = lv_ld_i64(is.payload, 0), llen = lv_ld_i64(ls.payload, 0);
    int64_t n = 0;
    memcpy(buf + n, "index ", 6); n += 6;
    memcpy(buf + n, P8(is.payload) + 8, (size_t)ilen); n += ilen;
    memcpy(buf + n, " out of bounds (length ", 23); n += 23;
    memcpy(buf + n, P8(ls.payload) + 8, (size_t)llen); n += llen;
    buf[n++] = ')';
    buf[n] = 0;
    lvrt_raise(buf);
}

/* ========================================================================
 * LA-30 — true-suspension await (designs/suspension/techdesign-02-llvm-leg.md
 * §2/§3). ⟦SENSITIVE-GATE S2⟧ authored under the model gate (overview §12).
 *
 * Op::Await's codegen emits ONE call — lvrt_await(dst, promise) — inside the
 * generic dk==1 wrapDest (LlvmGen.cpp:1172): the wrap stashes dst's old value
 * before the call and, on the non-throwing path, releases the old and RETAINS
 * the new after it. THE RETAIN DISCIPLINE OF THIS FILE IS THEREFORE:
 *
 *   1. every value written to *dst is BORROWED (a raw copy-out; never retain
 *      here — the wrap's retain is the register's count, exactly the
 *      "dk==1 wrap retains the borrowed read-out" contract the inline pump's
 *      readBB carried at LlvmGen.cpp:2820);
 *   2. on any path that ends in a throw dispatch (failed-Worker rethrow, the
 *      M5 C3 raise), *dst is left UNTOUCHED: emitThrowCheck branches away
 *      BEFORE the wrap's release/retain tail, so the register still owns its
 *      OLD value and the unwind's releaseAllRegs must find it there. The
 *      inline pump's failThrow BB wrote only arcScratch for the same reason;
 *      writing void here would leak the old ref. (This deliberately tightens
 *      the design sketch's `lv_set_void(dst)` on those two paths — the sketch
 *      mirrored Eval's vvoid() convention, which has no register file.)
 *
 * HISTORY (doc 2 §1): an lvrt_await runtime call existed once and was removed
 * by the maintenance-pass-2 §8 correction in favor of the inline pump BB
 * graph. That ruling was correct FOR PUMPING (a pump is a loop over two ABI
 * calls with no semantic payload). Parking is different in kind — it must run
 * on the scheduler's stack discipline (lv_task.c) and cannot be expressed as
 * inline BBs. Do not "restore" the inline form at a future maintenance pass.
 * ==================================================================== */

/* Poll hook (G15): bit0 = ready, bit1 = failed. Same internal getfield the
 * pump's aw.cond BB used, restricted to the two bool immediates; lvrt_getfield
 * is a raw copy-out, so nothing on this path touches a refcount. The payload-
 * nonzero test (not tag==LV_BOOL) is pump parity: aw.cond compared payload
 * only, and a plain Promise's missing `failed` reads as void/0 — falsy. */
int lvrt_promise_state(const void* obj) {
    LvValue p; p.tag = LV_OBJ; p.payload = I64(obj);
    LvValue f; int st = 0;
    lvrt_getfield(&f, &p, "ready");
    if (f.payload != 0) st |= 1;
    lvrt_getfield(&f, &p, "failed");
    if (f.payload != 0) st |= 2;
    return st;
}

/* M5 flip (doc 5 §5, S5): parity keys off the same flag as the model itself —
 * tasks mode (the default) runs C3 live (a drained await raises, loud and
 * catchable); LANG_PUMP=1 keeps the old silent default-value readout verbatim
 * (G2). M6 deletes parity, the pump, and this function together; until then
 * the `drained_wakes` stat audits the interim (it now counts C3 throws). */
static int lv_parity_mode(void) { return !lv_tasks_enabled(); }

/* LANG_TASKS=0 fallback: a C transcription of the inline pump BB graph this
 * call replaced (aw.cond -> aw.chkthrow -> aw.chkwork -> aw.step), same two
 * ABI calls, same exit order. Behavior — not code — is what M1's double-mode
 * corpus run pins (doc 2 §4.2). */
static int lv_pump_until_ready(const LvValue* p) {
    const void* obj = (const void*)(intptr_t)p->payload;
    for (;;) {
        if (lvrt_promise_state(obj) & 1) return LV_PARK_SETTLED;
        if (lv_g_throwing) return LV_PARK_SETTLED;   /* pump chkThrow exit: the caller
                                                        falls to the field reads and the
                                                        op's throwCheck dispatches — never
                                                        the C3 raise (a throw is already
                                                        in flight) */
        if (!lvrt_loop_has_work()) return LV_PARK_DRAINED;   /* G2 */
        lvrt_loop_step();
    }
}

void lvrt_await(LvValue* dst, const LvValue* p) {
    if (p->tag != LV_OBJ) { *dst = *p; return; }   /* aw.pass parity: await non-
                                                      promise = identity (borrowed
                                                      copy; the wrap retains) */
    if (lv_g_throwing) {                /* Eval.cpp:1102 parity. Unreachable from
                                           codegen (the previous op's throwCheck
                                           dispatched), and rule 2 above: dst
                                           stays untouched on a throwing return. */
        return;
    }
    const void* obj = (const void*)(intptr_t)p->payload;
    if (!(lvrt_promise_state(obj) & 1)) {
        int r = lv_tasks_enabled()
              ? lv_task_park_on(obj)                 /* park: doc 1 §7. The G11 assert
                                                        holds — the early-out above
                                                        proves the flag clear here. */
              : lv_pump_until_ready(p);              /* LANG_TASKS=0: old path      */
        if (r == LV_PARK_DRAINED && !lv_parity_mode()) {
            /* C3 (doc 5, M5-gated): a drain wake arrives with `ready` still
             * false — fabricating the default value is the disease None exists
             * to cure. Loud, local, catchable. dst untouched (rule 2). */
            lvrt_raise("await: event loop drained with promise unresolved");
            return;
        }
        if (r == LV_PARK_CANCELLED) {
            /* B2 (doc 06 §2): cancellation is an exception delivered at park
             * points, and only there — the same rethrow-point machinery as the
             * Worker-failed and C3 paths. Only the tasks-mode park can produce
             * this (the pump never parks). dst untouched (rule 2). */
            lvrt_raise_cls("CancelledException", "task cancelled");
            return;
        }
        /* parity mode falls through to the field reads, silent — G2. The
         * re-read below is a STATE read, not trust in the wake: a drain wake
         * arrives with ready still false, and that is the point. */
    }
    if (lvrt_promise_state(obj) & 2) {               /* failed BEFORE value — the
                                                        chkFail ordering: a rejected
                                                        Worker must never yield its
                                                        default `value` */
        LvValue msg; lvrt_getfield(&msg, p, "failMessage");   /* borrowed */
        lvrt_raise_str(&msg);        /* copies the bytes; Track 10 §3.3 parity */
        return;                      /* dst untouched (rule 2) */
    }
    lvrt_getfield(dst, p, "value");  /* BORROWED read-out — rule 1: the dk==1
                                        wrap's retain is the register's count;
                                        the +0B churn corpus is the proof */
}

/* Track W hard-03 (designs/wasm-frontend/hard-03-capability-gate.md, tier 2):
 * the wasm capability-gate trap. A gated native inside a prelude body that is
 * emitted anyway (the whole prelude lowers into every module) but is NOT
 * reachable from user code compiles to a call here instead of the native.
 * Built for ALL targets so the archive stays one source — a no-op until
 * called, and only wasm codegen ever emits the call. Never returns. */
void lvrt_unsupported(const char* what) {
    static const char kSuffix[] = ": not on the wasm-browser target\n";
    lv_plat_write(2, what, (int64_t)strlen(what));
    lv_plat_write(2, kSuffix, (int64_t)sizeof kSuffix - 1);
    lv_plat_exit(134);
}

/* Track W hard-05 (designs/wasm-frontend/hard-05-callclosure-seam.md): the
 * C-callable closure seam — the contract lives on the lv_abi.h declaration.
 * Mirrors generated CallValue code (LlvmGen.cpp case Op::CallValue): the
 * closure value is the callee's args[0], fnIndex is the closure body's
 * word0, argc counts the closure. Built for ALL targets (one archive
 * source); only the wasm glue calls it in v1. */
void lvrt_callclosure(LvValue* out, const LvValue* clo,
                      const LvValue* args, int nargs) {
    out->tag = LV_VOID; out->payload = 0;
    if (!g_dispatch || clo->tag != LV_CLO || nargs < 0) return;
    int64_t fnIndex = *(const int64_t*)(const void*)(intptr_t)clo->payload;
    LvValue small[8];              /* fast path; heap only for wide calls */
    LvValue* window = small;
    if (nargs + 1 > (int)(sizeof small / sizeof small[0])) {
        window = (LvValue*)malloc(((size_t)nargs + 1) * sizeof(LvValue));
        if (!window) return;
    }
    window[0] = *clo;              /* borrowed bitwise copies, CallValue's */
    for (int i = 0; i < nargs; ++i) window[i + 1] = args[i];
    g_dispatch(fnIndex, out, window, (int64_t)nargs + 1);
    if (window != small) free(window);
}

/* Track W hard-06 (designs/wasm-frontend/hard-06-hostbridge-seam.md): the
 * JS/DOM host-bridge accessors + native stubs. lvrt_class_field_name and
 * lvrt_class_name are the host-facing readers the JS marshaler pairs with
 * lvrt_fieldcount to identify a class and walk its slots (doc 05 §3); real on
 * every target (reads the registered class table), meaningful only when a JS
 * host reads them through the wasm export. On wasm the bridge ENTRY points
 * (lvrt_hostcall/host_clo_reg/hostecho) live in lv_bridge_wasm.c; here they are
 * native stubs that raise, since DOM/JS is a wasm-only capability (a program
 * that reaches one on a native build gets a loud, catchable RuntimeException —
 * the existing corpus never does). */
const char* lvrt_class_field_name(int64_t classId, int64_t i) {
    const LvClassInfo* c = lv_find_class(classId);
    if (!c || i < 0 || i >= c->nslots || !c->slotNames) return NULL;
    return c->slotNames[i];
}
const char* lvrt_class_name(int64_t classId) {
    const LvClassInfo* c = lv_find_class(classId);
    return c ? c->name : NULL;
}

#ifndef __wasm__
static void lv_host_native_only(LvValue* out) {
    if (out) { out->tag = LV_VOID; out->payload = 0; }
    lvrt_raise("host bridge: available on the wasm-browser target only");
}
void lvrt_hostcall(LvValue* out, const LvValue* op, const LvValue* h0,
                   const LvValue* h1, const LvValue* a, const LvValue* b) {
    (void)op; (void)h0; (void)h1; (void)a; (void)b; lv_host_native_only(out);
}
void lvrt_host_clo_reg(LvValue* out, const LvValue* cb) {
    (void)cb; lv_host_native_only(out);
}
void lvrt_hostecho(LvValue* out, const LvValue* v) {
    (void)v; lv_host_native_only(out);
}
#endif /* !__wasm__ */

void lvrt_uncaught(void) {
    if (!lv_g_throwing) return;
    /* gap (b) (designs/exit-codes.md §5): an uncaught exception exits 1 — a
     * crashed program must not report success to a shell / `set -e` / CI. Set
     * BEFORE printing (lv_entry.c returns lv_rt_exit_code after this call). */
    lv_rt_set_exit_code(1);
    lvrt_term_shutdown();   /* §3.4: restore BEFORE the traceback lands (sane terminal) */
    const char* clsName = "value";
    LvValue msgVal; msgVal.tag = LV_VOID; msgVal.payload = 0;
    if (g_thrown.tag == LV_OBJ) {
        int64_t classId = lv_ld_i64(g_thrown.payload, 0);
        const LvClassInfo* c = lv_find_class(classId);
        clsName = c ? c->name : "object";
        lvrt_getfield(&msgVal, &g_thrown, lv_key_message);
    }
    char buf[512];
    int64_t n = 0;
    memcpy(buf + n, "Uncaught ", 9); n += 9;
    size_t cl = strlen(clsName); memcpy(buf + n, clsName, cl); n += (int64_t)cl;
    if (msgVal.tag == LV_STR) {
        int64_t mlen = lv_ld_i64(msgVal.payload, 0);
        if (mlen > 0) {
            memcpy(buf + n, ": ", 2); n += 2;
            if (mlen > (int64_t)sizeof(buf) - n - 2) mlen = (int64_t)sizeof(buf) - n - 2;
            memcpy(buf + n, P8(msgVal.payload) + 8, (size_t)mlen); n += mlen;
        }
    }
    buf[n++] = '\n';
    lv_plat_write(1, buf, n);
}

/* ========================================================================
 * §2.9 — process entry (init only; main() wiring lands at B-M2/A-M5).
 * ==================================================================== */

void lv_rt_init(int argc, char** argv) {
    /* Track 10 F-i (belt-and-braces): guard the argv capture so no future lazy-
     * init path (lv_ensure_init -> lv_rt_init(0, NULL)) can clobber the process-
     * global argv to NULL and silently degrade sysArgs for the whole process. */
    if (argv) { g_argc = argc; g_argv = argv; }   /* capture argv (designs/argv.md §1/§5.1) */
    g_heap_base = g_heap_cursor = (uint8_t*)lv_plat_map(LV_HEAP_BYTES);
    if (!g_heap_base) lv_die("lvrt: heap mmap failed\n");
    g_heap_end = g_heap_base + LV_HEAP_BYTES;
    g_arena_base = lv_g_arena_cursor = (uint8_t*)lv_plat_map(LV_ARENA_BYTES);
    if (!g_arena_base) lv_die("lvrt: arena mmap failed\n");
    g_arena_end = g_arena_base + LV_ARENA_BYTES;
    memset(g_freelist, 0, sizeof g_freelist);
    g_live_bytes = 0; g_peak_bytes = 0;
    lv_g_throwing = 0; g_thrown.tag = LV_VOID; g_thrown.payload = 0;
}

/* Track 10 §5.4 — a worker thread bootstraps its OWN hot state through THIS,
 * never lv_rt_init: it maps a fresh heap+arena and zeroes the TLS allocator/
 * throw state, but leaves every process-global (argv, class table, dispatch)
 * alone (F-i). Records the mmap'd region bases so reap can munmap them (§5.3). */
void lv_thread_ctx_init(uint8_t** heapBaseOut, uint8_t** arenaBaseOut) {
    g_heap_base = g_heap_cursor = (uint8_t*)lv_plat_map(LV_HEAP_BYTES);
    if (!g_heap_base) lv_die("lvrt: worker heap mmap failed\n");
    g_heap_end = g_heap_base + LV_HEAP_BYTES;
    g_arena_base = lv_g_arena_cursor = (uint8_t*)lv_plat_map(LV_ARENA_BYTES);
    if (!g_arena_base) lv_die("lvrt: worker arena mmap failed\n");
    g_arena_end = g_arena_base + LV_ARENA_BYTES;
    memset(g_freelist, 0, sizeof g_freelist);
    g_live_bytes = 0; g_peak_bytes = 0;
    lv_g_throwing = 0; g_thrown.tag = LV_VOID; g_thrown.payload = 0;
    if (heapBaseOut) *heapBaseOut = g_heap_base;
    if (arenaBaseOut) *arenaBaseOut = g_arena_base;
}

/* §5.3: the spawner thread unmaps a reaped worker's regions wholesale — nothing
 * outside a worker heap ever points into it (D1'), so the unmap is
 * unconditionally safe once the result has been rebuilt out. */
void lv_thread_ctx_unmap(uint8_t* heapBase, uint8_t* arenaBase) {
    if (heapBase) lv_plat_unmap(heapBase, LV_HEAP_BYTES);
    if (arenaBase) lv_plat_unmap(arenaBase, LV_ARENA_BYTES);
}

/* Process exit code (designs/exit-codes.md §3.1). A settable global backs
 * lv_rt_exit_code — lv_entry.c already RETURNS it (runtime/lv_entry.c), so
 * set-and-complete works for compiled programs the moment the global is
 * threaded, no entry change. Unix status is 8-bit, so every write is masked to
 * 0..255 (§5) — env.exit(257) -> 1, env.exit(-1) -> 255 — and every engine
 * agrees because the mask lives here and in lvrt_sysexit. */
static int g_exit_code = 0;
void lv_rt_set_exit_code(int code) { g_exit_code = code & 0xFF; }
int  lv_rt_exit_code(void)         { return g_exit_code; }

/* env.setExitCode(n) (designs/exit-codes.md §3.2): record the code for normal
 * completion, then let execution finish (the loop drains, cleanups run). The
 * arg crosses as an LvValue* (the boundary rule) carrying the int in payload. */
void lvrt_syssetexitcode(const LvValue* code) {
    lv_rt_set_exit_code((int)(code->payload & 0xFF));
}

/* env.exit(n) (designs/exit-codes.md §3.1): terminate NOW, abandoning pending
 * loop work. Routes through the shared exit epilogue — restore the terminal if
 * raw (terminal-raw-mode.md §3.4) so a TUI that exits from an error path never
 * orphans the shell — then hard-exit via the platform floor. Never returns. */
void lvrt_sysexit(const LvValue* code) {
    lvrt_term_shutdown();                          /* restore cooked mode if raw */
    lv_plat_exit((int)(code->payload & 0xFF));      /* 8-bit; never returns */
}
