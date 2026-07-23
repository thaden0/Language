/* Track B — standalone C selftest for the runtime core (B-M1 §3
 * acceptance). No LLVM needed: this exercises lv_runtime.c directly,
 * registering a small fake class table and a fake dispatch trampoline
 * exactly as docs/techdesign-portable-backend-2.md §3 step 6 asks for.
 *
 * Every check aborts the process (exit 1) with a message on failure, so
 * this doubles as a ctest target.
 */
#include "lv_abi.h"
#include "lv_plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

/* whitebox helpers — selftest is explicitly allowed to peek at the layout
 * facts it's verifying (see doc-2 §3.5); not part of the public ABI. */
static int64_t lv_test_len(int64_t payload) { return *(int64_t*)(intptr_t)payload; }
static int64_t lv_test_rc(int64_t payload) {
    return ((int64_t*)((uint8_t*)(intptr_t)payload - 16))[0];
}
static const char* lv_test_bytes(int64_t strPayload) {
    return (const char*)(intptr_t)(strPayload + 8);
}
static int lv_test_arr_has_string(const LvValue* arr, const char* wanted) {
    if (!arr || arr->tag != LV_ARR) return 0;
    int64_t n = lv_test_len(arr->payload);
    if (n < 0) return 0;   /* directory listings are always boxed arrays */
    size_t wantedLen = strlen(wanted);
    for (int64_t i = 0; i < n; i++) {
        LvValue elem;
        memcpy(&elem, (const void*)(intptr_t)(arr->payload + 8 + 16 * i),
               sizeof elem);
        if (elem.tag == LV_STR &&
            lv_test_len(elem.payload) == (int64_t)wantedLen &&
            memcmp(lv_test_bytes(elem.payload), wanted, wantedLen) == 0)
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * A tiny fake program: classes + a dispatch trampoline, registered exactly
 * the way Track A's generated code would via lvrt_register (§2.6).
 * ---------------------------------------------------------------------- */

static const char* kX = "x";
static const char* kY = "y";
static const char* kA = "a";
static const char* kB = "b";
static const char* kSum = "sum";
static const char* kSelf = "self";

enum { CLS_POINT = 1, CLS_PAIR_VALUE = 2, CLS_RUNTIME_EXCEPTION = 3, CLS_BASE = 4,
       CLS_DERIVED = 5, CLS_COL = 6 };

static const char* kPointSlots[2];
static const char* kPairSlots[2];
static const char* kColSlots[3];   /* CLS_COL: three int fields p,q,r (columnar-eligible) */
static const char* kColP = "p";
static const char* kColQ = "q";
static const char* kColR = "r";

/* Columnar descriptor contract (techdesign-columnar §4.3/§4.4). Generated code
 * emits these; the selftest hand-provides them, running the runtime columnar
 * core with --columnar semantically ON. Only CLS_COL is eligible — CLS_PAIR_VALUE
 * stays row-major dense (it is used by the dense path in test_arrays_maps), which
 * also proves the eligibility gate routes an ineligible value struct to dense. */
const int32_t lv_cfg_columnar = 1;
int32_t lv_col_eligible(int64_t classId) { return classId == CLS_COL ? 1 : 0; }
int32_t lv_col_typecode(int64_t classId, int64_t k) {
    (void)k;
    return classId == CLS_COL ? LV_INT : 0;   /* all three fields are int */
}
static const int64_t kDerivedSubtypes[] = { CLS_DERIVED, CLS_BASE };
/* Point members: sum (plain method, fn 100), magnitude (GETTER, fn 101),
 * "+" (operator, fn 102) — one entry of each kind so the LV_M_* dispatch
 * split is actually exercised (getm must not dispatch `sum`; callm must
 * not dispatch `magnitude`). */
static const char* kPointMethods[] = { "sum", "magnitude", "+" };
static const int64_t kPointMethodFn[] = { 100, 101, 102 };
static const int8_t kPointMethodKinds[] = { LV_M_METHOD, LV_M_GET, LV_M_OP };

static LvClassInfo g_classes[6];

/* fnIndex 100 = Point.sum(self) -> self.x + self.y
 * fnIndex 101 = Point.magnitude getter -> self.x*self.x + self.y*self.y
 * fnIndex 102 = Point + Point -> new Point(lx+rx, ly+ry), returned +1
 * fnIndex 200 = B-M3 loop-callback recorder (see below) */
static int64_t g_loopFireCount;
static int64_t g_loopLastArg;

static void fake_dispatch(int64_t fnIndex, LvValue* ret, LvValue* args, int64_t argc) {
    (void)argc;
    if (fnIndex == 100 || fnIndex == 101) {
        LvValue xv, yv;
        lvrt_getfield(&xv, &args[0], kX);
        lvrt_getfield(&yv, &args[0], kY);
        ret->tag = LV_INT;
        ret->payload = fnIndex == 100 ? xv.payload + yv.payload
                                       : xv.payload * xv.payload + yv.payload * yv.payload;
        return;
    }
    if (fnIndex == 102) {
        LvValue lx, ly, rx, ry;
        lvrt_getfield(&lx, &args[0], kX); lvrt_getfield(&ly, &args[0], kY);
        lvrt_getfield(&rx, &args[1], kX); lvrt_getfield(&ry, &args[1], kY);
        lvrt_obj_new(ret, CLS_POINT);
        LvValue nx = { LV_INT, lx.payload + rx.payload };
        LvValue ny = { LV_INT, ly.payload + ry.payload };
        lvrt_setfield(ret, kX, &nx);
        lvrt_setfield(ret, kY, &ny);
        lvrt_retain(ret);   /* call results transfer at +1 */
        return;
    }
    if (fnIndex == 200) {
        /* B-M3: the event loop's callback recorder — args[0] is the closure
         * itself (X64Gen's genCallClosure forwarding order), args[1] is the
         * tick count (timers) or fd (watches). */
        (void)argc;
        g_loopFireCount++;
        g_loopLastArg = args[1].payload;
        ret->tag = LV_VOID; ret->payload = 0;
        return;
    }
    ret->tag = LV_VOID; ret->payload = 0;
}

static void setup_classes(void) {
    kPointSlots[0] = kX; kPointSlots[1] = kY;
    kPairSlots[0] = kA; kPairSlots[1] = kB;
    kColSlots[0] = kColP; kColSlots[1] = kColQ; kColSlots[2] = kColR;

    g_classes[0] = (LvClassInfo){
        .name = "Point", .classId = CLS_POINT, .nslots = 2, .isValue = 0,
        .slotNames = kPointSlots, .subtypeIds = NULL, .nSubtypeIds = 0,
        .methodNames = kPointMethods, .methodFnIndex = kPointMethodFn,
        .methodKinds = kPointMethodKinds, .nMethods = 3,
    };
    g_classes[1] = (LvClassInfo){
        .name = "Pair", .classId = CLS_PAIR_VALUE, .nslots = 2, .isValue = 1,
        .slotNames = kPairSlots, .subtypeIds = NULL, .nSubtypeIds = 0,
        .methodNames = NULL, .methodFnIndex = NULL, .nMethods = 0,
    };
    g_classes[2] = (LvClassInfo){
        .name = "RuntimeException", .classId = CLS_RUNTIME_EXCEPTION, .nslots = 0, .isValue = 0,
        .slotNames = NULL, .subtypeIds = NULL, .nSubtypeIds = 0,
        .methodNames = NULL, .methodFnIndex = NULL, .nMethods = 0,
    };
    g_classes[3] = (LvClassInfo){
        .name = "Base", .classId = CLS_BASE, .nslots = 0, .isValue = 0,
        .slotNames = NULL, .subtypeIds = NULL, .nSubtypeIds = 0,
        .methodNames = NULL, .methodFnIndex = NULL, .nMethods = 0,
    };
    g_classes[4] = (LvClassInfo){
        .name = "Derived", .classId = CLS_DERIVED, .nslots = 0, .isValue = 0,
        .slotNames = NULL, .subtypeIds = kDerivedSubtypes, .nSubtypeIds = 2,
        .methodNames = NULL, .methodFnIndex = NULL, .nMethods = 0,
    };
    g_classes[5] = (LvClassInfo){
        .name = "Col", .classId = CLS_COL, .nslots = 3, .isValue = 1,
        .slotNames = kColSlots, .subtypeIds = NULL, .nSubtypeIds = 0,
        .methodNames = NULL, .methodFnIndex = NULL, .nMethods = 0,
    };
    lvrt_register(g_classes, 6, fake_dispatch);
}

/* A-M6 hardening (2026-07-06): builds an ARC-prefixed value-struct directly
 * in the arena tier via the raw lvrt_halloc(size+16, LV_TIER_ARENA) contract
 * (§2.5) — the exact calling convention Track A's escape analysis is about
 * to emit for scope-local CopyVal allocations, exercised here before any
 * codegen reaches it. lvrt_obj_new is heap-tier only, so this is hand-rolled;
 * fields are then filled through the normal lvrt_setfield API. */
static void arena_obj_new(LvValue* out, int64_t classId, int64_t nslots) {
    int64_t bodySize = 16 + 16 * nslots;
    uint8_t* raw = (uint8_t*)lvrt_halloc(bodySize + 16, LV_TIER_ARENA);
    int64_t* hdr = (int64_t*)raw;
    hdr[0] = -1;      /* arena sentinel (§B-H1) */
    hdr[1] = bodySize + 16;
    int64_t* body = (int64_t*)(raw + 16);
    body[0] = classId;
    body[1] = 0;      /* dyn head, unused for value classes */
    out->tag = LV_OBJ;
    out->payload = (int64_t)(intptr_t)(raw + 16);
}

/* ------------------------------------------------------------------------ */

static void test_allocator(void) {
    /* arena bump + save/restore bulk free */
    int64_t mark = lvrt_arena_save();
    void* a1 = lvrt_halloc(128, LV_TIER_ARENA);
    void* a2 = lvrt_halloc(256, LV_TIER_ARENA);
    CHECK(a1 != NULL && a2 != NULL && a2 == (uint8_t*)a1 + 128);
    lvrt_arena_restore(mark);
    void* a3 = lvrt_halloc(64, LV_TIER_ARENA);
    CHECK(a3 == a1);   /* cursor rewound exactly to the mark */

    /* range-check: retain/release on a pointer that is NOT runtime-owned
     * (a stack address, or a static literal that looks string-shaped) must
     * be a silent no-op — never a crash, never touching live-bytes. */
    int64_t liveBefore = lvrt_live_bytes();

    int stackInt = 42;
    LvValue bogusObj; bogusObj.tag = LV_OBJ; bogusObj.payload = (int64_t)(intptr_t)&stackInt;
    lvrt_retain(&bogusObj);
    lvrt_release(&bogusObj);
    CHECK(lvrt_live_bytes() == liveBefore);

    static const char kLit[16] = "static, no hdr";
    LvValue litStr; litStr.tag = LV_STR; litStr.payload = (int64_t)(intptr_t)kLit;
    lvrt_retain(&litStr);
    lvrt_release(&litStr);
    CHECK(lvrt_live_bytes() == liveBefore);

    /* alloc/free/reuse via the heap size-class allocator: a string built
     * and immediately consumed (rc==0, dropped by concat) returns its
     * block to the free list, so the next same-size allocation reuses it. */
    LvValue s1; lvrt_str_new(&s1, "abcdefgh", 8);   /* 8+8+1=17 -> class 32 */
    void* firstBlock = (void*)((uint8_t*)(intptr_t)s1.payload - 16);
    LvValue empty; lvrt_str_new(&empty, "", 0);
    LvValue merged; lvrt_str_concat(&merged, &s1, &empty);   /* consumes s1 (rc==0) */
    LvValue s2; lvrt_str_new(&s2, "ijklmnop", 8);
    void* secondBlock = (void*)((uint8_t*)(intptr_t)s2.payload - 16);
    CHECK(firstBlock == secondBlock);   /* recycled from the free list */
    lvrt_release(&merged);
    lvrt_release(&s2);

    /* bug #95: a compiler cleanup can retain a stale alias to a standalone
     * value struct after its owning aggregate has already reclaimed it.  The
     * payload remains inside the heap region, so the old ARC gate read the
     * poisoned class id, mistook the value struct for a counted object, and
     * decremented its free-list link.  The next two same-class allocations
     * then followed that corrupted pointer and crashed later in unrelated
     * code.  Freed-payload release/vfree must both be idempotent no-ops. */
    LvValue va, vb;
    lvrt_obj_new(&va, CLS_PAIR_VALUE);
    lvrt_obj_new(&vb, CLS_PAIR_VALUE);
    void* vaBlock = (void*)((uint8_t*)(intptr_t)va.payload - 16);
    void* vbBlock = (void*)((uint8_t*)(intptr_t)vb.payload - 16);
    lvrt_vfree(&va);
    lvrt_vfree(&vb);                    /* free-list head: vb -> va */
    lvrt_release(&vb);                  /* stale compiler-cleanup alias */
    lvrt_vfree(&vb);                    /* stale explicit value free */

    LvValue vc, vd;
    lvrt_obj_new(&vc, CLS_PAIR_VALUE);
    lvrt_obj_new(&vd, CLS_PAIR_VALUE);
    CHECK((void*)((uint8_t*)(intptr_t)vc.payload - 16) == vbBlock);
    CHECK((void*)((uint8_t*)(intptr_t)vd.payload - 16) == vaBlock);
    lvrt_vfree(&vc);
    lvrt_vfree(&vd);

    fprintf(stderr, "OK: allocator\n");
}

static void test_arc_object_graph(void) {
    int64_t baseline = lvrt_live_bytes();

    LvValue p; lvrt_obj_new(&p, CLS_POINT);
    LvValue x; x.tag = LV_INT; x.payload = 3;
    LvValue y; y.tag = LV_INT; y.payload = 4;
    lvrt_setfield(&p, kX, &x);
    lvrt_setfield(&p, kY, &y);
    LvValue sum; lvrt_callm(&sum, &p, kSum, NULL, 0);
    CHECK(sum.tag == LV_INT && sum.payload == 7);

    /* array[1] holding p: bind (retain) first, then mutate in place at
     * rc==1 — the standard "bind, then mutate" usage pattern. */
    LvValue arr; lvrt_arr_new(&arr, 1);
    lvrt_retain(&arr);
    LvValue idx0; idx0.tag = LV_INT; idx0.payload = 0;
    LvValue arr2; lvrt_idxset(&arr2, &arr, &idx0, &p);
    CHECK(arr2.payload == arr.payload);

    /* map{"pt": p}: maps always copy on insert (no spare capacity), so —
     * mirroring the "IndexStore op-hook" convention extracted from
     * X64Gen's genIdxSet comments — the caller releases the OLD map right
     * after rebinding to the new one, exactly like a variable reassign. */
    LvValue m; lvrt_map_new(&m, 0);
    lvrt_retain(&m);
    LvValue key; lvrt_str_new(&key, "pt", 2);
    LvValue m2; lvrt_idxset(&m2, &m, &key, &p);
    lvrt_release(&m);
    lvrt_retain(&m2);

    /* a closure capturing a second object */
    LvValue p2; lvrt_obj_new(&p2, CLS_POINT);
    LvValue clo; lvrt_closure_new(&clo, 42);
    lvrt_retain(&clo);
    lvrt_capture_set(&clo, kSelf, &p2);
    LvValue capped; lvrt_capture_get(&capped, &clo, kSelf);
    CHECK(capped.payload == p2.payload);

    /* tear the whole graph down */
    lvrt_release(&arr2);   /* frees the array; releases p (2 owners -> 1) */
    lvrt_release(&m2);     /* frees the map; releases key (freed) and p (1 -> 0, freed) */
    lvrt_release(&clo);    /* frees the closure; releases p2 (freed) */

    CHECK(lvrt_live_bytes() == baseline);
    fprintf(stderr, "OK: ARC object graph (live bytes returned to baseline)\n");
}

static void test_strings(void) {
    /* NOTE on this test's shape: every lvrt_str_* constructor here returns
     * a FRESH (rc==0) value, and lvrt_release() is a documented no-op at
     * rc<=0 (§2.4 — an unaliased fresh value is only ever reclaimed by
     * whatever later retains it, or — for strings specifically — by the
     * concat-consume path below). A real caller would either retain these
     * before release, or allocate them arena-tier and bulk-free on scope
     * exit; this test isn't doing either, so it does NOT assert an overall
     * return to baseline the way test_arc_object_graph does. What IS
     * checked here, precisely, is the one thing B-M1 step 4 asks for: that
     * concat's consume-unowned discipline actually frees its rc==0
     * operands rather than leaking them. */
    LvValue a; lvrt_str_new(&a, "hello ", 6);      /* body 6+9=15 -> total 31 -> class 32 */
    LvValue b; lvrt_str_new(&b, "world", 5);       /* body 5+9=14 -> total 30 -> class 32 */
    void* aBlock = (void*)((uint8_t*)(intptr_t)a.payload - 16);
    void* bBlock = (void*)((uint8_t*)(intptr_t)b.payload - 16);
    LvValue c; lvrt_str_concat(&c, &a, &b);   /* consumes unowned a, b */
    CHECK(lv_test_len(c.payload) == 11);
    CHECK(memcmp(lv_test_bytes(c.payload), "hello world", 11) == 0);
    /* a's and b's (class-32) blocks must be back on the free list, not
     * still live: two fresh same-class allocations recycle exactly them. */
    LvValue probe1; lvrt_str_new(&probe1, "abcdefg", 7);    /* body 7+9=16 -> total 32 -> class 32 */
    LvValue probe2; lvrt_str_new(&probe2, "gfedcba", 7);
    void* p1Block = (void*)((uint8_t*)(intptr_t)probe1.payload - 16);
    void* p2Block = (void*)((uint8_t*)(intptr_t)probe2.payload - 16);
    CHECK((p1Block == aBlock || p1Block == bBlock) && (p2Block == aBlock || p2Block == bBlock));
    CHECK(p1Block != p2Block);

    CHECK(lvrt_str_eq(&c, &c));
    LvValue notEq; lvrt_str_new(&notEq, "hello world!", 12);
    CHECK(!lvrt_str_eq(&c, &notEq));
    lvrt_release(&notEq);

    LvValue sub; lvrt_str_substr(&sub, &c, 6, 5);
    CHECK(lv_test_len(sub.payload) == 5 && memcmp(lv_test_bytes(sub.payload), "world", 5) == 0);

    LvValue needle; lvrt_str_new(&needle, "world", 5);
    CHECK(lvrt_str_indexof(&c, &needle) == 6);
    CHECK(lvrt_str_indexof(&c, &c) == 0);
    lvrt_release(&sub);
    lvrt_release(&needle);

    LvValue trimSrc; lvrt_str_new(&trimSrc, "  hi \n", 6);
    LvValue trimmed; lvrt_str_trim(&trimmed, &trimSrc);
    CHECK(lv_test_len(trimmed.payload) == 2 && memcmp(lv_test_bytes(trimmed.payload), "hi", 2) == 0);
    lvrt_release(&trimSrc);
    lvrt_release(&trimmed);

    LvValue upper; lvrt_str_case(&upper, &c, 0);
    CHECK(memcmp(lv_test_bytes(upper.payload), "HELLO WORLD", 11) == 0);
    lvrt_release(&upper);

    LvValue n; lvrt_int_to_str(&n, -1234);
    CHECK(lv_test_len(n.payload) == 5 && memcmp(lv_test_bytes(n.payload), "-1234", 5) == 0);
    LvValue parsed; lvrt_str_toint(&parsed, &n);
    CHECK(parsed.tag == LV_INT && parsed.payload == -1234);
    lvrt_release(&n);

    /* Track 04 M2: strict full-string parse -- garbage/partial -> None, not 0. */
    LvValue garbage; lvrt_str_new(&garbage, "12ab", 4);
    LvValue garbageParsed; lvrt_str_toint(&garbageParsed, &garbage);
    CHECK(garbageParsed.tag == LV_NONE);
    lvrt_release(&garbage);

    LvValue fstr; lvrt_str_new(&fstr, "3.5", 3);
    LvValue fparsed; lvrt_str_tofloat(&fparsed, &fstr);
    CHECK(fparsed.tag == LV_FLOAT);
    double fv; memcpy(&fv, &fparsed.payload, 8);
    CHECK(fv > 3.49 && fv < 3.51);
    lvrt_release(&fstr);

    LvValue zero; lvrt_int_to_str(&zero, 0);
    CHECK(lv_test_len(zero.payload) == 1 && lv_test_bytes(zero.payload)[0] == '0');
    lvrt_release(&zero);

    lvrt_release(&c);   /* no-op: c is rc==0, never aliased — see the note above */
    fprintf(stderr, "OK: strings\n");
}

static void test_arrays_maps(void) {
    /* boxed array COW: force rc==1 (in-place path) */
    LvValue arr; lvrt_arr_new(&arr, 3);
    lvrt_retain(&arr);
    LvValue v0; v0.tag = LV_INT; v0.payload = 10;
    LvValue v1; v1.tag = LV_INT; v1.payload = 20;
    LvValue idx0; idx0.tag = LV_INT; idx0.payload = 0;
    LvValue idx1; idx1.tag = LV_INT; idx1.payload = 1;
    LvValue after0; lvrt_idxset(&after0, &arr, &idx0, &v0);
    CHECK(after0.payload == arr.payload);
    LvValue after1; lvrt_idxset(&after1, &after0, &idx1, &v1);
    CHECK(after1.payload == arr.payload);

    LvValue got0; lvrt_idxget(&got0, &after1, &idx0);
    CHECK(got0.tag == LV_INT && got0.payload == 10);

    /* shared (rc>1): idxset must copy, leaving the original untouched */
    lvrt_retain(&after1);   /* rc 1 -> 2: genuinely aliased now */
    LvValue v99; v99.tag = LV_INT; v99.payload = 99;
    LvValue copied; lvrt_idxset(&copied, &after1, &idx0, &v99);
    CHECK(copied.payload != after1.payload);
    LvValue stillOld; lvrt_idxget(&stillOld, &after1, &idx0);
    CHECK(stillOld.tag == LV_INT && stillOld.payload == 10);
    LvValue nowNew; lvrt_idxget(&nowNew, &copied, &idx0);
    CHECK(nowNew.tag == LV_INT && nowNew.payload == 99);

    lvrt_release(&copied);
    lvrt_release(&after1);
    lvrt_release(&after1);   /* undo both retains (initial bind + alias) */

    /* dense value-struct array: appending a Pair (value class) goes dense */
    LvValue empty; lvrt_arr_new(&empty, 0);
    LvValue pair; lvrt_obj_new(&pair, CLS_PAIR_VALUE);
    LvValue av; av.tag = LV_INT; av.payload = 1;
    LvValue bv; bv.tag = LV_INT; bv.payload = 2;
    lvrt_setfield(&pair, kA, &av);
    lvrt_setfield(&pair, kB, &bv);
    LvValue dense; lvrt_arr_append(&dense, &empty, &pair);
    CHECK(lv_test_len(dense.payload) < 0);   /* dense marker bit set */
    LvValue elem; lvrt_idxget(&elem, &dense, &idx0);
    CHECK(elem.tag == LV_OBJ);
    LvValue elemA; lvrt_getfield(&elemA, &elem, kA);
    CHECK(elemA.tag == LV_INT && elemA.payload == 1);
    lvrt_vfree(&pair);       /* the standalone copy fed into append is dead */
    lvrt_release(&dense);    /* dense buffers have no per-element refs to release */

    /* boxed append growth: repeated .add() on a bound (rc==1) array grows
     * geometrically and stays byte-correct */
    LvValue acc; lvrt_arr_new(&acc, 0);
    lvrt_retain(&acc);
    for (int64_t i = 0; i < 10; i++) {
        LvValue item; item.tag = LV_INT; item.payload = i;
        LvValue next; lvrt_arr_append(&next, &acc, &item);
        if (next.payload != acc.payload) lvrt_release(&acc);   /* old buffer superseded */
        acc = next;
    }
    CHECK(lv_test_len(acc.payload) == 10);
    for (int64_t i = 0; i < 10; i++) {
        LvValue ii; ii.tag = LV_INT; ii.payload = i;
        LvValue got; lvrt_idxget(&got, &acc, &ii);
        CHECK(got.tag == LV_INT && got.payload == i);
    }
    lvrt_release(&acc);

    /* map: insert, update in place at rc==1, lookup */
    LvValue m; lvrt_map_new(&m, 0);
    lvrt_retain(&m);
    LvValue k1; lvrt_str_new(&k1, "one", 3);
    LvValue mv1; mv1.tag = LV_INT; mv1.payload = 1;
    LvValue m1; lvrt_idxset(&m1, &m, &k1, &mv1);
    CHECK(m1.payload != m.payload);   /* absent key -> always a fresh copy */
    lvrt_release(&m);

    lvrt_retain(&m1);
    LvValue mv1b; mv1b.tag = LV_INT; mv1b.payload = 111;
    LvValue m1updated; lvrt_idxset(&m1updated, &m1, &k1, &mv1b);
    CHECK(m1updated.payload == m1.payload);   /* existing key, rc==1 -> in place */
    LvValue got; lvrt_idxget(&got, &m1updated, &k1);
    CHECK(got.tag == LV_INT && got.payload == 111);
    lvrt_release(&m1updated);

    fprintf(stderr, "OK: arrays/maps (COW + dense)\n");
}

/* known_bugs_2.md worktree-agent-a7c3e630889a1bf22-107: lvrt_idxset_move — idxset with the VALUE operand
 * CONSUMED (§15 destination ownership). The dense/columnar/map paths copy the
 * record in and must vfree the dead standalone copy (the old ~recBytes/store
 * churn leak); the boxed in-range path transfers ownership to the slot (the
 * array's free path vfrees it) and must NOT free it; an OOB store consumes
 * nothing and must vfree. Every leg asserts live-bytes returns to baseline. */
static void test_idxset_move(void) {
    int64_t baseline = lvrt_live_bytes();
    LvValue idx0; idx0.tag = LV_INT; idx0.payload = 0;

    /* dense: record bytes memcpy'd in -> the standalone operand is freed */
    LvValue empty; lvrt_arr_new(&empty, 0);
    lvrt_retain(&empty);
    LvValue seed; lvrt_obj_new(&seed, CLS_PAIR_VALUE);
    LvValue av; av.tag = LV_INT; av.payload = 1;
    LvValue bv; bv.tag = LV_INT; bv.payload = 2;
    lvrt_setfield(&seed, kA, &av);
    lvrt_setfield(&seed, kB, &bv);
    LvValue dense; lvrt_arr_append(&dense, &empty, &seed);   /* rc 1, dense */
    lvrt_vfree(&seed);
    lvrt_release(&empty);
    CHECK(lv_test_len(dense.payload) < 0);
    LvValue q; lvrt_obj_new(&q, CLS_PAIR_VALUE);             /* fresh rc-0 copy */
    LvValue qa; qa.tag = LV_INT; qa.payload = 77;
    lvrt_setfield(&q, kA, &qa);
    LvValue after; lvrt_idxset_move(&after, &dense, &idx0, &q);   /* consumes q */
    CHECK(after.payload == dense.payload);                   /* rc==1: in place */
    LvValue elem; lvrt_idxget(&elem, &after, &idx0);
    LvValue elemA; lvrt_getfield(&elemA, &elem, kA);
    CHECK(elemA.tag == LV_INT && elemA.payload == 77);
    lvrt_release(&dense);
    CHECK(lvrt_live_bytes() == baseline);   /* q reclaimed at the store, not leaked */

    /* boxed in-range: the slot takes the pointer itself — ownership moves to
     * the array (its free path vfrees the element); no premature free */
    LvValue boxed; lvrt_arr_new(&boxed, 1);
    lvrt_retain(&boxed);
    LvValue q2; lvrt_obj_new(&q2, CLS_PAIR_VALUE);
    lvrt_setfield(&q2, kA, &av);
    int64_t q2pay = q2.payload;
    LvValue after2; lvrt_idxset_move(&after2, &boxed, &idx0, &q2);
    CHECK(after2.payload == boxed.payload);
    LvValue elem2; lvrt_idxget(&elem2, &after2, &idx0);
    CHECK(elem2.tag == LV_OBJ && elem2.payload == q2pay);    /* aliased, not copied */

    /* boxed OOB: nothing stored -> the operand is dead and reclaimed */
    LvValue q3; lvrt_obj_new(&q3, CLS_PAIR_VALUE);
    LvValue idx5; idx5.tag = LV_INT; idx5.payload = 5;
    LvValue after3; lvrt_idxset_move(&after3, &boxed, &idx5, &q3);
    CHECK(after3.payload == boxed.payload);
    lvrt_release(&boxed);                                    /* vfrees the q2 element */
    CHECK(lvrt_live_bytes() == baseline);

    /* map: upsert deep-copies the struct value -> the operand is reclaimed */
    LvValue m; lvrt_map_new(&m, 0);
    lvrt_retain(&m);
    LvValue key; lvrt_str_new(&key, "pt", 2);
    LvValue q4; lvrt_obj_new(&q4, CLS_PAIR_VALUE);
    lvrt_setfield(&q4, kB, &bv);
    LvValue m2; lvrt_idxset_move(&m2, &m, &key, &q4);        /* fresh map, consumes q4 */
    CHECK(m2.payload != m.payload);
    lvrt_retain(&m2);
    lvrt_release(&m);
    LvValue got; lvrt_idxget(&got, &m2, &key);
    CHECK(got.tag == LV_OBJ);
    LvValue gotB; lvrt_getfield(&gotB, &got, kB);
    CHECK(gotB.tag == LV_INT && gotB.payload == 2);
    lvrt_release(&m2);
    CHECK(lvrt_live_bytes() == baseline);

    fprintf(stderr, "OK: idxset_move (consumed value operand, worktree-agent-a7c3e630889a1bf22-107)\n");
}

/* techdesign-columnar-arrays.md §4-§6, plan §2.1/§3.3: the runtime columnar
 * core, driven entirely from C with no LLVM. Builds an Array<Col> (Col = 3 int
 * fields, the only lv_col_eligible class), and exercises: flip, append×N,
 * gather (fresh rc=1 record, tag synthesis), idxset in-place vs whole-buffer
 * COW, bounds throw, free (no leak), flatten→rebuild round-trip. Also asserts
 * the eligibility gate keeps an INELIGIBLE value struct (CLS_PAIR_VALUE) on the
 * row-major dense layout even with --columnar on. */
static LvValue col_make(int64_t p, int64_t q, int64_t r) {
    LvValue o; lvrt_obj_new(&o, CLS_COL);
    LvValue pv = {LV_INT, p}, qv = {LV_INT, q}, rv = {LV_INT, r};
    lvrt_setfield(&o, kColP, &pv);
    lvrt_setfield(&o, kColQ, &qv);
    lvrt_setfield(&o, kColR, &rv);
    return o;
}
static void col_check_elem(const LvValue* arr, int64_t i, int64_t p, int64_t q, int64_t r) {
    LvValue ii = {LV_INT, i};
    LvValue e; lvrt_idxget(&e, arr, &ii);
    CHECK(e.tag == LV_OBJ);
    CHECK(lv_test_rc(e.payload) == 0);            /* gather yields a FRESH unowned copy (copyval convention) */
    LvValue fp, fq, fr;
    lvrt_getfield(&fp, &e, kColP); lvrt_getfield(&fq, &e, kColQ); lvrt_getfield(&fr, &e, kColR);
    CHECK(fp.tag == LV_INT && fp.payload == p);
    CHECK(fq.tag == LV_INT && fq.payload == q);
    CHECK(fr.tag == LV_INT && fr.payload == r);
    lvrt_vfree(&e);                                /* free the gathered standalone copy */
}

static void test_columnar(void) {
    int64_t baseline = lvrt_live_bytes();

    /* --- flip: first value-struct append with an eligible class goes columnar --- */
    LvValue empty; lvrt_arr_new(&empty, 0);
    LvValue e0 = col_make(10, 11, 12);
    LvValue a1; lvrt_arr_append(&a1, &empty, &e0);
    lvrt_vfree(&e0);                               /* the standalone operand is dead */
    int64_t w0 = lv_test_len(a1.payload);
    CHECK(w0 < 0);                                 /* not boxed */
    CHECK(((uint64_t)w0 & ((uint64_t)1 << 62)) != 0);   /* columnar marker bit */
    CHECK((int64_t)((uint64_t)w0 & ~((uint64_t)3 << 62)) == 1);   /* cleanLen == 1 */
    CHECK(*((int64_t*)(intptr_t)(a1.payload + 8)) == CLS_COL);    /* word1 == classId */
    col_check_elem(&a1, 0, 10, 11, 12);

    /* --- append to 4 elements (copy-and-grow each time) --- */
    LvValue acc = a1;                              /* a1 is rc=1 (append stamps +1) */
    for (int64_t i = 1; i < 4; i++) {
        LvValue ei = col_make(10 + 10 * i, 11 + 10 * i, 12 + 10 * i);
        LvValue next; lvrt_arr_append(&next, &acc, &ei);
        lvrt_vfree(&ei);
        if (next.payload != acc.payload) lvrt_release(&acc);
        acc = next;
    }
    CHECK((int64_t)((uint64_t)lv_test_len(acc.payload) & ~((uint64_t)3 << 62)) == 4);
    for (int64_t i = 0; i < 4; i++)
        col_check_elem(&acc, i, 10 + 10 * i, 11 + 10 * i, 12 + 10 * i);

    /* --- idxset in place (rc==1): scatter one element across the columns --- */
    LvValue idx2 = {LV_INT, 2};
    LvValue repl = col_make(777, 888, 999);
    LvValue afterSet; lvrt_idxset(&afterSet, &acc, &idx2, &repl);
    CHECK(afterSet.payload == acc.payload);        /* mutated in place */
    acc = afterSet;
    col_check_elem(&acc, 2, 777, 888, 999);
    col_check_elem(&acc, 1, 20, 21, 22);           /* neighbours untouched */
    lvrt_vfree(&repl);

    /* --- idxset COW (rc>1): shared array copies; original untouched --- */
    lvrt_retain(&acc);                             /* rc 1 -> 2 (genuinely aliased) */
    LvValue idx0 = {LV_INT, 0};
    LvValue repl0 = col_make(-1, -2, -3);
    LvValue copied; lvrt_idxset(&copied, &acc, &idx0, &repl0);
    lvrt_retain(&copied);                          /* own the COW result (IndexStore dk==1 convention) */
    CHECK(copied.payload != acc.payload);          /* a fresh buffer */
    col_check_elem(&copied, 0, -1, -2, -3);        /* copy has the new value */
    col_check_elem(&acc, 0, 10, 11, 12);           /* original still original */
    lvrt_vfree(&repl0);
    lvrt_release(&copied);                          /* free the copy (rc 1->0) */
    lvrt_release(&acc);                            /* undo the alias retain */

    /* --- flatten -> rebuild round-trip (thread transfer, §6.4) --- */
    LvValue rebuilt; lvrt_systhreadtransfer(&rebuilt, &acc);
    int64_t rw0 = lv_test_len(rebuilt.payload);
    CHECK(rw0 < 0 && ((uint64_t)rw0 & ((uint64_t)1 << 62)) != 0);   /* still columnar */
    CHECK((int64_t)((uint64_t)rw0 & ~((uint64_t)3 << 62)) == 4);
    for (int64_t i = 0; i < 4; i++) {
        int64_t p = (i == 0) ? 10 : 10 + 10 * i;
        int64_t q = (i == 0) ? 11 : 11 + 10 * i;
        int64_t r = (i == 0) ? 12 : 12 + 10 * i;
        if (i == 2) { p = 777; q = 888; r = 999; }
        col_check_elem(&rebuilt, i, p, q, r);
    }
    lvrt_release(&rebuilt);
    lvrt_release(&acc);                            /* free the columnar buffer */

    /* --- eligibility gate: an INELIGIBLE value struct stays row-major dense --- */
    LvValue empty2; lvrt_arr_new(&empty2, 0);
    LvValue pair; lvrt_obj_new(&pair, CLS_PAIR_VALUE);   /* isValue, but NOT lv_col_eligible */
    LvValue av = {LV_INT, 5}, bv = {LV_INT, 6};
    lvrt_setfield(&pair, kA, &av); lvrt_setfield(&pair, kB, &bv);
    LvValue dpz; lvrt_arr_append(&dpz, &empty2, &pair);
    int64_t dw0 = lv_test_len(dpz.payload);
    CHECK(dw0 < 0);                                       /* dense/not-boxed */
    CHECK(((uint64_t)dw0 & ((uint64_t)1 << 62)) == 0);    /* NOT columnar — dense */
    LvValue di0 = {LV_INT, 0};
    LvValue de; lvrt_idxget(&de, &dpz, &di0);             /* dense idxget = record pointer */
    LvValue dea; lvrt_getfield(&dea, &de, kA);
    CHECK(dea.tag == LV_INT && dea.payload == 5);
    lvrt_vfree(&pair);
    lvrt_release(&dpz);

    /* the boxed empties that seeded each flip are rc=0 (arr_new) — reclaim them
     * (retain 0->1 so release 1->0 actually frees) so the baseline is exact. */
    lvrt_retain(&empty);  lvrt_release(&empty);
    lvrt_retain(&empty2); lvrt_release(&empty2);

    /* no columnar allocation outlives the test — the columnar core is leak-flat.
     * (The bounds-check below is deliberately AFTER this line: raising an OOB
     * exception leaks its message string by ~128B, a PRE-EXISTING runtime edge
     * unrelated to columnar — verified with lv_cfg_columnar=0 — so it must not
     * be charged against columnar flatness.) */
    CHECK(lvrt_live_bytes() == baseline);

    /* --- bounds check: out-of-range gather raises the same OOB exception --- */
    LvValue bc0; lvrt_arr_new(&bc0, 0);
    LvValue be = col_make(1, 2, 3);
    LvValue bc; lvrt_arr_append(&bc, &bc0, &be); lvrt_vfree(&be);
    CHECK(!lvrt_throwing());
    LvValue oob = {LV_INT, 99};
    LvValue dead; lvrt_idxget(&dead, &bc, &oob);
    CHECK(lvrt_throwing());
    LvValue bind = {LV_VOID, 0}; lvrt_catch_bind(&bind);
    CHECK(!lvrt_throwing());
    lvrt_release(&bind);
    lvrt_release(&bc);
    lvrt_retain(&bc0); lvrt_release(&bc0);

    fprintf(stderr, "OK: columnar Array<struct> (flip/append/gather/COW/bounds/transfer/gate)\n");
}

/* bug.md #30: lvrt_map_with must return an OWNED (+1) map on EVERY path so a
 * with-produced map survives a cross-frame return (the poison/segfault the bug
 * filed — a fresh copy left at rc 0 is freed by the retain/release pair at the
 * return boundary). This mirrors lvrt_arr_append's stamped +1 transfer contract
 * and is the runtime half of designs/complete/techdesign-bug30-map-with-ownership.md §4
 * (the consumed-receiver release is codegen's half, in LlvmGen's `with` row). */
static void test_map_with(void) {
    int64_t before = lvrt_live_bytes();

    /* (a) fresh insert into an empty base -> a NEW map at rc==1; base untouched */
    LvValue base; lvrt_map_new(&base, 0);
    lvrt_retain(&base);                       /* a live receiver at rc 1 */
    LvValue ka; lvrt_str_new(&ka, "a", 1);
    LvValue va = { LV_INT, 1 };
    LvValue m1; lvrt_map_with(&m1, &base, &ka, &va);
    CHECK(m1.payload != base.payload);        /* absent key -> fresh copy */
    CHECK(lv_test_rc(m1.payload) == 1);       /* THE FIX: result crosses OWNED (+1) */
    CHECK(lv_test_rc(base.payload) == 1);     /* base's own rc untouched */
    CHECK(lv_test_rc(ka.payload) == 1);       /* the key is owned by exactly the map */

    /* (b) update an existing key on the uniquely-owned result -> in place, SAME
     *     payload, rc unchanged (no spurious extra retain from the wrapper) */
    LvValue vb = { LV_INT, 99 };
    LvValue m2; lvrt_map_with(&m2, &m1, &ka, &vb);
    CHECK(m2.payload == m1.payload);          /* rc==1 + found -> in place */
    CHECK(lv_test_rc(m2.payload) == 1);       /* rc unchanged (m1/m2 alias one block) */
    LvValue got; lvrt_idxget(&got, &m2, &ka);
    CHECK(got.tag == LV_INT && got.payload == 99);

    /* (c) a second fresh insert -> 2-entry map at +1; "a" is now owned by both
     *     the old block and the fresh one (rc 2), "bb" by the fresh one only */
    LvValue kb; lvrt_str_new(&kb, "bb", 2);
    LvValue vc = { LV_INT, 7 };
    LvValue m3; lvrt_map_with(&m3, &m2, &kb, &vc);
    CHECK(m3.payload != m2.payload);          /* absent key -> fresh copy */
    CHECK(lv_test_rc(m3.payload) == 1);
    CHECK(lv_test_len(m3.payload) == 2);
    CHECK(lv_test_rc(ka.payload) == 2);       /* "a" owned by the old block AND m3 */
    CHECK(lv_test_rc(kb.payload) == 1);       /* "bb" owned by m3 only */

    /* teardown proves entries are owned at EXACTLY +1 per map (no over/under
     * count): releasing each map once cascades to free its entry strings, and
     * live-bytes returns to the pre-test baseline. m1 and m2 alias one block —
     * release it once (via m2), never twice. */
    int64_t kaPay = ka.payload;
    lvrt_release(&base);                       /* empty map; owns nothing */
    lvrt_release(&m2);                          /* frees the old 1-entry block; "a" 2 -> 1 */
    CHECK(lv_test_rc(kaPay) == 1);
    lvrt_release(&m3);                          /* frees the 2-entry block; "a" 1 -> 0, "bb" 1 -> 0 */

    CHECK(lvrt_live_bytes() == before);        /* no leak: the full round-trip balances */

    fprintf(stderr, "OK: map.with (bug #30: +1 transfer, in-place COW, no leak)\n");
}

static void test_dispatch_and_issub(void) {
    CHECK(lvrt_issub(CLS_DERIVED, CLS_DERIVED));
    CHECK(lvrt_issub(CLS_DERIVED, CLS_BASE));
    CHECK(!lvrt_issub(CLS_BASE, CLS_DERIVED));
    CHECK(!lvrt_issub(CLS_POINT, CLS_BASE));

    LvValue p; lvrt_obj_new(&p, CLS_POINT);
    lvrt_retain(&p);
    LvValue x = { LV_INT, 3 }, y = { LV_INT, 4 };
    lvrt_setfield(&p, kX, &x);
    lvrt_setfield(&p, kY, &y);

    /* kind split: callm dispatches methods only, getm getters only */
    LvValue sum; lvrt_callm(&sum, &p, kSum, NULL, 0);
    CHECK(sum.tag == LV_INT && sum.payload == 7);
    LvValue mag; lvrt_getm(&mag, &p, "magnitude");
    CHECK(mag.tag == LV_INT && mag.payload == 25);
    LvValue viaGetm; lvrt_getm(&viaGetm, &p, kSum);      /* `sum` is NOT a getter */
    CHECK(viaGetm.tag == LV_VOID);                        /* falls to (missing) field */
    LvValue viaCallm; lvrt_callm(&viaCallm, &p, "magnitude", NULL, 0);   /* getter is NOT a method */
    CHECK(viaCallm.tag == LV_VOID);

    /* getm on a plain field returns +1 (transfer): rc must bump */
    LvValue q; lvrt_obj_new(&q, CLS_POINT);
    LvValue qv = { LV_OBJ, q.payload };
    lvrt_setfield(&p, kX, &qv);      /* raw store: p.x = q (no ARC — test scaffolding) */
    lvrt_retain(&qv);                 /* the slot's own ref */
    int64_t rcBefore = lv_test_rc(q.payload);
    LvValue gotQ; lvrt_getm(&gotQ, &p, kX);
    CHECK(gotQ.tag == LV_OBJ && gotQ.payload == q.payload);
    CHECK(lv_test_rc(q.payload) == rcBefore + 1);         /* transfer retained */
    lvrt_release(&gotQ);

    /* setm does release-old/retain-new internally: overwriting p.x with an
     * int must release q (rc back down); q then dies with p's teardown...
     * but q's slot ref was its only one after we drop ours. */
    LvValue five = { LV_INT, 5 };
    lvrt_setm(&p, kX, &five);        /* releases old q ref (rc 1 -> 0 -> freed) */
    LvValue nowInt; lvrt_getfield(&nowInt, &p, kX);
    CHECK(nowInt.tag == LV_INT && nowInt.payload == 5);

    /* opm: Point + Point via the LV_M_OP entry; result is a fresh Point +1 */
    LvValue a; lvrt_obj_new(&a, CLS_POINT);
    lvrt_retain(&a);
    LvValue ax = { LV_INT, 1 }, ay = { LV_INT, 2 };
    lvrt_setfield(&a, kX, &ax); lvrt_setfield(&a, kY, &ay);
    LvValue opSum; lvrt_opm(&opSum, LV_OP_ADD, &p, &a);
    CHECK(opSum.tag == LV_OBJ);
    LvValue osx; lvrt_getfield(&osx, &opSum, kX);
    CHECK(osx.tag == LV_INT && osx.payload == 5 + 1);
    CHECK(lv_test_rc(opSum.payload) == 1);                /* returned owned (+1) */
    /* derived !=: no "==" declared, so EQ/NE fall to reference identity
     * (documented reference-class identity equality) — p and a are distinct
     * objects, so != is true */
    LvValue neq; lvrt_opm(&neq, LV_OP_NE, &p, &a);
    CHECK(neq.tag == LV_BOOL && neq.payload == 1);
    lvrt_release(&opSum);
    lvrt_release(&a);
    lvrt_release(&p);

    /* key content-fallback: a heap-built key equal in CONTENT to "x" must
     * hit the slot even though the pointer differs */
    LvValue h; lvrt_obj_new(&h, CLS_POINT);
    lvrt_retain(&h);
    LvValue hv = { LV_INT, 77 };
    char keyBuf[2] = { 'x', 0 };     /* stack copy: pointer != kX, content == */
    lvrt_setfield(&h, keyBuf, &hv);
    LvValue hGot; lvrt_getfield(&hGot, &h, kX);
    CHECK(hGot.tag == LV_INT && hGot.payload == 77);
    lvrt_release(&h);

    fprintf(stderr, "OK: dispatch tables (issub, kind split, getm +1, setm ARC, opm, key fallback)\n");
}

static void test_scalar_core(void) {
    /* truth / not / neg */
    LvValue t = { LV_BOOL, 1 }, f = { LV_BOOL, 0 }, i9 = { LV_INT, 9 };
    CHECK(lvrt_truth(&t) == 1 && lvrt_truth(&f) == 0 && lvrt_truth(&i9) == 0);
    LvValue notT; lvrt_not(&notT, &t);
    CHECK(notT.tag == LV_BOOL && notT.payload == 0);
    LvValue negI; lvrt_neg(&negI, &i9);
    CHECK(negI.tag == LV_INT && negI.payload == -9);

    /* int arith across the op table */
    LvValue a = { LV_INT, 17 }, b = { LV_INT, 5 }, r;
    lvrt_arith(LV_OP_ADD, &r, &a, &b); CHECK(r.tag == LV_INT && r.payload == 22);
    lvrt_arith(LV_OP_SUB, &r, &a, &b); CHECK(r.payload == 12);
    lvrt_arith(LV_OP_MUL, &r, &a, &b); CHECK(r.payload == 85);
    lvrt_arith(LV_OP_DIV, &r, &a, &b); CHECK(r.payload == 3);
    lvrt_arith(LV_OP_MOD, &r, &a, &b); CHECK(r.payload == 2);
    lvrt_arith(LV_OP_LT, &r, &a, &b);  CHECK(r.tag == LV_BOOL && r.payload == 0);
    lvrt_arith(LV_OP_GE, &r, &a, &b);  CHECK(r.tag == LV_BOOL && r.payload == 1);
    lvrt_arith(LV_OP_SHL, &r, &b, &(LvValue){ LV_INT, 2 }); CHECK(r.payload == 20);

    /* float arith: double bit patterns in the payload */
    double d15 = 1.5, d05 = 0.5;
    LvValue fl = { LV_FLOAT, 0 }, fr = { LV_FLOAT, 0 };
    memcpy(&fl.payload, &d15, 8); memcpy(&fr.payload, &d05, 8);
    lvrt_arith(LV_OP_ADD, &r, &fl, &fr);
    double dOut; memcpy(&dOut, &r.payload, 8);
    CHECK(r.tag == LV_FLOAT && dOut == 2.0);
    lvrt_arith(LV_OP_LT, &r, &fr, &fl);
    CHECK(r.tag == LV_BOOL && r.payload == 1);

    /* string arith: + concatenates (consuming rc-0 temps), == is content,
     * mixed-type == is FALSE (oracle rule, not the stub's stringify-both) */
    LvValue s1; lvrt_str_new(&s1, "ab", 2);
    LvValue s2; lvrt_str_new(&s2, "cd", 2);
    lvrt_arith(LV_OP_ADD, &r, &s1, &s2);
    CHECK(r.tag == LV_STR && lv_test_len(r.payload) == 4);
    CHECK(memcmp(lv_test_bytes(r.payload), "abcd", 4) == 0);
    LvValue sNum; lvrt_str_new(&sNum, "1", 1);
    LvValue one = { LV_INT, 1 };
    lvrt_arith(LV_OP_EQ, &r, &sNum, &one);
    CHECK(r.tag == LV_BOOL && r.payload == 0);   /* "1" == 1 is false */
    LvValue sAlpha; lvrt_str_new(&sAlpha, "abc", 3);
    LvValue sBeta;  lvrt_str_new(&sBeta,  "abd", 3);
    lvrt_arith(LV_OP_LT, &r, &sAlpha, &sBeta);
    CHECK(r.tag == LV_BOOL && r.payload == 1);   /* lexicographic */

    /* int + string mixed concat: stringifies the int through a consumed temp */
    LvValue n7 = { LV_INT, 7 };
    LvValue sX; lvrt_str_new(&sX, "x=", 2);
    lvrt_arith(LV_OP_ADD, &r, &sX, &n7);
    CHECK(r.tag == LV_STR && lv_test_len(r.payload) == 3);
    CHECK(memcmp(lv_test_bytes(r.payload), "x=7", 3) == 0);

    fprintf(stderr, "OK: scalar core (truth/not/neg/arith int+float+string)\n");
}

static void test_to_string(void) {
    LvValue r;

    LvValue i = { LV_INT, -42 };
    lvrt_to_string(&r, &i);
    CHECK(lv_test_len(r.payload) == 3 && memcmp(lv_test_bytes(r.payload), "-42", 3) == 0);

    LvValue bt = { LV_BOOL, 1 };
    lvrt_to_string(&r, &bt);
    CHECK(memcmp(lv_test_bytes(r.payload), "true", 4) == 0);

    LvValue none = { LV_NONE, 0 };
    lvrt_to_string(&r, &none);
    CHECK(memcmp(lv_test_bytes(r.payload), "None", 4) == 0);

    double d = 1.5;
    LvValue fv = { LV_FLOAT, 0 }; memcpy(&fv.payload, &d, 8);
    lvrt_to_string(&r, &fv);      /* std::to_string(1.5) == "1.500000" */
    CHECK(lv_test_len(r.payload) == 8 && memcmp(lv_test_bytes(r.payload), "1.500000", 8) == 0);

    /* string passes through as the SAME value (ts_build self rule) */
    LvValue s; lvrt_str_new(&s, "hi", 2);
    lvrt_to_string(&r, &s);
    CHECK(r.payload == s.payload);
    lvrt_release(&s);   /* rc 0, no-op; consumed below via print if ever */

    /* array: "[1, 2, 3]" — bind (retain) the array, store, stringify */
    LvValue arr; lvrt_arr_new(&arr, 3);
    lvrt_retain(&arr);
    for (int64_t k = 0; k < 3; k++) {
        LvValue idx = { LV_INT, k }, val = { LV_INT, k + 1 }, outv;
        lvrt_idxset(&outv, &arr, &idx, &val);
    }
    lvrt_to_string(&r, &arr);
    CHECK(lv_test_len(r.payload) == 9 && memcmp(lv_test_bytes(r.payload), "[1, 2, 3]", 9) == 0);

    /* map: "{a: 1}" with a string key */
    LvValue m; lvrt_map_new(&m, 0);
    lvrt_retain(&m);
    LvValue mk; lvrt_str_new(&mk, "a", 1);
    LvValue mv = { LV_INT, 1 };
    LvValue m2; lvrt_idxset(&m2, &m, &mk, &mv);
    lvrt_release(&m);
    lvrt_retain(&m2);
    lvrt_to_string(&r, &m2);
    CHECK(lv_test_len(r.payload) == 6 && memcmp(lv_test_bytes(r.payload), "{a: 1}", 6) == 0);
    lvrt_release(&m2);

    /* object without a Range class: "<object>" */
    LvValue p; lvrt_obj_new(&p, CLS_POINT);
    lvrt_retain(&p);
    lvrt_to_string(&r, &p);
    CHECK(memcmp(lv_test_bytes(r.payload), "<object>", 8) == 0);
    lvrt_release(&p);
    lvrt_release(&arr);

    fprintf(stderr, "OK: to_string (valueToString parity: scalars, array, map, object)\n");
}

static void test_exceptions(void) {
    CHECK(!lvrt_throwing());
    lvrt_raise_oob(5, 3);
    CHECK(lvrt_throwing());
    LvValue thrown; lvrt_thrown(&thrown);
    CHECK(thrown.tag == LV_OBJ);
    LvValue msg; lvrt_getfield(&msg, &thrown, lv_key_message);
    CHECK(msg.tag == LV_STR);
    CHECK(lv_test_len(msg.payload) == (int64_t)strlen("index 5 out of bounds (length 3)"));
    CHECK(memcmp(lv_test_bytes(msg.payload), "index 5 out of bounds (length 3)",
                 (size_t)lv_test_len(msg.payload)) == 0);

    fprintf(stderr, "  uncaught report follows (visual check): ");
    lvrt_uncaught();

    LvValue bind; bind.tag = LV_VOID; bind.payload = 0;
    lvrt_catch_bind(&bind);
    CHECK(!lvrt_throwing());
    CHECK(bind.tag == LV_OBJ);
    lvrt_release(&bind);

    fprintf(stderr, "OK: exceptions\n");
    (void)lv_test_rc;
}

/* Process exit codes (designs/exit-codes.md §3.1/§5). Runs right after
 * test_exceptions, which calls lvrt_uncaught — so the gap-(b) fix is observable
 * here: the uncaught reporter set the process exit code to 1. */
static void test_exit_code(void) {
    /* gap (b): uncaught (reported just above) => exit 1, not silent 0. */
    CHECK(lv_rt_exit_code() == 1);
    /* set-and-complete: the settable global backs lv_rt_exit_code. */
    lv_rt_set_exit_code(7);
    CHECK(lv_rt_exit_code() == 7);
    /* 8-bit truncation (§5): Unix status is code & 0xFF. */
    lv_rt_set_exit_code(257);
    CHECK(lv_rt_exit_code() == 1);
    lv_rt_set_exit_code(-1);
    CHECK(lv_rt_exit_code() == 255);
    lv_rt_set_exit_code(0);            /* leave a clean 0 for the harness epilogue */
    CHECK(lv_rt_exit_code() == 0);
    fprintf(stderr, "OK: exit codes\n");
}

/* ------------------------------------------------------------------------
 * A-M6 hardening (2026-07-06): the arena tier (§2.5) has only ever been
 * exercised by test_allocator's standalone bump/restore check above — never
 * combined with nested frames, live heap traffic, an in-flight exception, or
 * an escaping value-struct copy. Track A's A-M6 is about to wire
 * lvrt_arena_save/restore into every LLVM frame prologue/return path
 * (doc-1 §9), so this is that combination exercised ahead of them.
 * ---------------------------------------------------------------------- */

static void test_arena_hardening(void) {
    /* ---- nested save/restore marks (recursive-call shape) ---- */
    int64_t markOuter = lvrt_arena_save();
    void* o1 = lvrt_halloc(64, LV_TIER_ARENA);
    int64_t markInner = lvrt_arena_save();
    void* i1 = lvrt_halloc(128, LV_TIER_ARENA);
    void* i2 = lvrt_halloc(32, LV_TIER_ARENA);
    CHECK(i2 == (uint8_t*)i1 + 128);
    lvrt_arena_restore(markInner);            /* inner frame returns */
    void* o2 = lvrt_halloc(48, LV_TIER_ARENA);
    CHECK(o2 == i1);                           /* cursor rewound exactly to markInner */
    lvrt_arena_restore(markOuter);             /* outer frame returns */
    void* o3 = lvrt_halloc(16, LV_TIER_ARENA);
    CHECK(o3 == o1);                           /* cursor rewound exactly to markOuter */

    /* repeated save/restore at the same nesting depth (loop-body shape):
     * the mark must be identical every iteration -- no drift across N
     * reentries, the pattern a for-loop's per-iteration frame produces. */
    int64_t m0 = lvrt_arena_save();
    for (int depth = 0; depth < 4; depth++) {
        int64_t m = lvrt_arena_save();
        CHECK(m == m0);
        lvrt_halloc(16 * (depth + 1), LV_TIER_ARENA);
        lvrt_arena_restore(m);
    }
    CHECK(lvrt_arena_save() == m0);

    /* ---- arena-then-heap interleaving ---- */
    /* arena bump/restore must be fully invisible to the heap allocator: the
     * live-bytes meter only counts heap-tier traffic (§2.5), and the two
     * regions must never drift into each other across repeated cycles. */
    int64_t heapBaseline = lvrt_live_bytes();
    int64_t loopMark = -1;
    for (int i = 0; i < 50; i++) {
        int64_t m = lvrt_arena_save();
        if (loopMark < 0) loopMark = m; else CHECK(m == loopMark);
        lvrt_halloc(64, LV_TIER_ARENA);                 /* scope-local arena temp */
        LvValue s; lvrt_str_new(&s, "churn", 5);        /* heap-tier temp, same frame */
        lvrt_retain(&s);
        CHECK(s.tag == LV_STR && lv_test_len(s.payload) == 5);
        lvrt_release(&s);
        lvrt_arena_restore(m);
    }
    CHECK(lvrt_live_bytes() == heapBaseline);

    /* ---- restore under a pending throw ---- */
    /* the unwind path calls arena_restore on every frame it passes through
     * on the way to a catch (doc-1 §9's "restore on every return/unwind
     * path"); it must neither disturb g_throwing/g_thrown (heap-tier, a
     * different region entirely) nor leave anything the catch handler needs
     * unfreed. This is also a full-teardown regression check: it caught a
     * real leak (logged in doc-2 §10) where an object's dyn-list fallback
     * field -- the shape RuntimeException.message uses in this fake table --
     * was never released or freed by lv_recursive_free. */
    CHECK(!lvrt_throwing());
    int64_t excBaseline = lvrt_live_bytes();
    int64_t markThrow = lvrt_arena_save();
    lvrt_halloc(96, LV_TIER_ARENA);                     /* an arena temp live at throw time */
    lvrt_raise("arena unwind probe");
    CHECK(lvrt_throwing());
    lvrt_arena_restore(markThrow);                      /* the unwinding frame's exit restore */
    CHECK(lvrt_throwing());                             /* untouched by the arena restore */
    LvValue thrown; lvrt_thrown(&thrown);
    CHECK(thrown.tag == LV_OBJ);
    LvValue msg; lvrt_getfield(&msg, &thrown, lv_key_message);
    CHECK(msg.tag == LV_STR);
    CHECK(lv_test_len(msg.payload) == (int64_t)strlen("arena unwind probe"));
    CHECK(memcmp(lv_test_bytes(msg.payload), "arena unwind probe",
                 strlen("arena unwind probe")) == 0);
    LvValue bind; bind.tag = LV_VOID; bind.payload = 0;
    lvrt_catch_bind(&bind);                             /* caught by an outer frame */
    CHECK(!lvrt_throwing());
    lvrt_release(&bind);
    CHECK(lvrt_live_bytes() == excBaseline);            /* full teardown: message + dyn node + object */

    /* ---- value-struct copies inside a restored region (0xFE poison) ---- */
    /* lvrt_copyval must deep-copy a value struct to the heap tier regardless
     * of where the source lives -- including recursively for a nested
     * value-struct field -- so that once the arena frame returns (and the
     * source bytes are reused/poisoned), the escaped copy is unaffected.
     * A mis-arena'd escaping struct here would be a UAF (doc-1 §9's own
     * warning); this proves the existing copyval discipline already makes
     * that safe. */
    int64_t before4 = lvrt_live_bytes();
    int64_t markVal = lvrt_arena_save();

    LvValue inner; arena_obj_new(&inner, CLS_PAIR_VALUE, 2);
    LvValue ia = { LV_INT, 11 }, ib = { LV_INT, 22 };
    lvrt_setfield(&inner, kA, &ia);
    lvrt_setfield(&inner, kB, &ib);

    LvValue outerV; arena_obj_new(&outerV, CLS_PAIR_VALUE, 2);
    LvValue oa = { LV_INT, 33 };
    lvrt_setfield(&outerV, kA, &oa);
    lvrt_setfield(&outerV, kB, &inner);      /* nested value struct, arena-resident */

    CHECK(lv_test_rc(inner.payload) == -1);
    CHECK(lv_test_rc(outerV.payload) == -1);

    LvValue heapCopy; lvrt_copyval(&heapCopy, &outerV);
    CHECK(heapCopy.payload != outerV.payload);
    CHECK(lv_test_rc(heapCopy.payload) == 0);            /* fresh heap alloc, not arena */

    LvValue copiedA, copiedInner;
    lvrt_getfield(&copiedA, &heapCopy, kA);
    lvrt_getfield(&copiedInner, &heapCopy, kB);
    CHECK(copiedA.tag == LV_INT && copiedA.payload == 33);
    CHECK(copiedInner.tag == LV_OBJ && copiedInner.payload != inner.payload);
    CHECK(lv_test_rc(copiedInner.payload) == 0);         /* nested field is its own heap copy too */

    lvrt_arena_restore(markVal);                         /* the frame returns */
    /* poison the reclaimed bytes exactly like lv_free_raw does for a heap
     * free (§3 step 2) -- proves the escaped copy holds independent memory,
     * not a pointer back into the arena the next allocation will overwrite. */
    memset((void*)(intptr_t)inner.payload, 0xFE, 16 + 16 * 2);
    memset((void*)(intptr_t)outerV.payload, 0xFE, 16 + 16 * 2);

    LvValue copiedA2, copiedInner2;
    lvrt_getfield(&copiedA2, &heapCopy, kA);
    lvrt_getfield(&copiedInner2, &heapCopy, kB);
    CHECK(copiedA2.payload == 33);
    LvValue ia2, ib2;
    lvrt_getfield(&ia2, &copiedInner2, kA);
    lvrt_getfield(&ib2, &copiedInner2, kB);
    CHECK(ia2.payload == 11 && ib2.payload == 22);        /* unaffected by the poison */

    lvrt_vfree(&heapCopy);                                /* dead standalone copy, rc==0 */
    CHECK(lvrt_live_bytes() == before4);

    fprintf(stderr,
            "OK: arena hardening (nested marks, arena/heap interleave, "
            "throw-unwind teardown, poisoned-region value copies)\n");
}

/* ------------------------------------------------------------------------
 * B-M3: event loop (timers + fd watches) and the sys* natives.
 * ---------------------------------------------------------------------- */

static void test_event_loop(void) {
    int64_t mark = lvrt_live_bytes();

    /* one-shot timer: fires exactly once with tick=1, then self-retires
     * (registry releases its retained ref — no explicit release here, per
     * §2.7's "registry owns the callback... released on cancel/last fire"). */
    g_loopFireCount = 0; g_loopLastArg = -1;
    LvValue cb1; lvrt_closure_new(&cb1, 200);
    LvValue delay0 = { LV_INT, 0 }, interval0 = { LV_INT, 0 };
    LvValue timerId1;
    lvrt_systimerstart(&timerId1, &delay0, &interval0, &cb1);
    CHECK(lvrt_loop_has_work());
    for (int i = 0; i < 1000 && lvrt_loop_has_work(); i++) lvrt_loop_step();
    CHECK(!lvrt_loop_has_work());
    CHECK(g_loopFireCount == 1);
    CHECK(g_loopLastArg == 1);
    CHECK(lvrt_live_bytes() == mark);   /* one-shot cleaned up after itself */

    /* repeating timer: cancel mid-flight, confirm the registry's ref is
     * dropped (no leak) and no further ticks land after cancel. */
    g_loopFireCount = 0; g_loopLastArg = -1;
    LvValue cb2; lvrt_closure_new(&cb2, 200);
    LvValue delay1 = { LV_INT, 0 }, interval1 = { LV_INT, 2 };
    LvValue timerId2;
    lvrt_systimerstart(&timerId2, &delay1, &interval1, &cb2);
    for (int i = 0; i < 1000 && g_loopFireCount < 3; i++) lvrt_loop_step();
    CHECK(g_loopFireCount >= 3);
    int64_t ticksAtCancel = g_loopFireCount;
    lvrt_systimercancel(&timerId2);
    CHECK(!lvrt_loop_has_work());
    CHECK(lvrt_live_bytes() == mark);
    for (int i = 0; i < 5; i++) lvrt_loop_step();      /* no-op: nothing registered */
    CHECK(g_loopFireCount == ticksAtCancel);            /* cancel truly stopped it */

    /* cancelling an already-fired one-shot / unknown id is a documented
     * no-op (§2.7) — must not crash or double-release. */
    lvrt_systimercancel(&timerId1);

    /* fd watch: a pipe write must wake it; watches persist after firing
     * (unlike one-shot timers) until explicitly unwatched. */
    int pfd[2];
    CHECK(pipe(pfd) == 0);
    g_loopFireCount = 0; g_loopLastArg = -1;
    LvValue cb3; lvrt_closure_new(&cb3, 200);
    LvValue fdVal = { LV_INT, pfd[0] };
    LvValue watchId;
    lvrt_syswatch(&watchId, &fdVal, &cb3);
    CHECK(lvrt_loop_has_work());
    CHECK(write(pfd[1], "x", 1) == 1);
    for (int i = 0; i < 1000 && g_loopFireCount == 0; i++) lvrt_loop_step();
    CHECK(g_loopFireCount == 1);
    CHECK(g_loopLastArg == pfd[0]);
    CHECK(lvrt_loop_has_work());        /* watch is still active post-fire */
    lvrt_sysunwatch(&watchId);
    CHECK(!lvrt_loop_has_work());
    CHECK(lvrt_live_bytes() == mark);
    close(pfd[0]); close(pfd[1]);

    /* a watch whose fd is closed UNDER it must wake once and self-retire
     * (LV_POLLNVAL) — before the 2026-07-05 maintenance fix, poll() returned
     * instantly with an event the readiness mask ignored and the loop
     * busy-spun forever (RuntimeLoop.cpp had the identical gap). */
    int pfd2[2];
    CHECK(pipe(pfd2) == 0);
    g_loopFireCount = 0;
    LvValue cb4; lvrt_closure_new(&cb4, 200);
    LvValue fdVal2 = { LV_INT, pfd2[0] };
    LvValue watchId2;
    lvrt_syswatch(&watchId2, &fdVal2, &cb4);
    close(pfd2[0]); close(pfd2[1]);          /* close under the armed watch */
    CHECK(lvrt_loop_has_work());
    for (int i = 0; i < 100 && lvrt_loop_has_work(); i++) lvrt_loop_step();
    CHECK(!lvrt_loop_has_work());            /* watch retired itself */
    CHECK(g_loopFireCount == 1);             /* woke exactly once */
    CHECK(lvrt_live_bytes() == mark);        /* registry's cb ref released */

    fprintf(stderr, "OK: event loop (timers one-shot+repeat+cancel, fd watch, dead-fd retire)\n");
}

static void test_sys_file_natives(void) {
    const char* path = "/tmp/lv_selftest_bm3_file.txt";
    remove(path);
    LvValue pathVal; lvrt_str_new(&pathVal, path, (int64_t)strlen(path));

    LvValue flagsWrite = { LV_INT, 2 };
    LvValue fdw; lvrt_sysopen(&fdw, &pathVal, &flagsWrite);
    CHECK(fdw.payload >= 0);
    LvValue content; lvrt_str_new(&content, "hello-loop", 10);
    LvValue nwritten; lvrt_syswrite(&nwritten, &fdw, &content);
    CHECK(nwritten.payload == 10);
    lvrt_sysclose(&fdw);

    LvValue existsField = { LV_INT, 0 }, existsOut;
    lvrt_sysstat(&existsOut, &pathVal, &existsField);
    CHECK(existsOut.payload == 1);
    LvValue sizeField = { LV_INT, 1 }, sizeOut;
    lvrt_sysstat(&sizeOut, &pathVal, &sizeField);
    CHECK(sizeOut.payload == 10);

    LvValue flagsRead = { LV_INT, 1 };
    LvValue fdr; lvrt_sysopen(&fdr, &pathVal, &flagsRead);
    CHECK(fdr.payload >= 0);
    LvValue maxLen = { LV_INT, 4096 }, readBack;
    lvrt_sysread(&readBack, &fdr, &maxLen);
    CHECK(readBack.tag == LV_STR);
    CHECK(lv_test_len(readBack.payload) == 10);
    CHECK(memcmp(lv_test_bytes(readBack.payload), "hello-loop", 10) == 0);
    lvrt_sysclose(&fdr);
    remove(path);

    LvValue badPath; lvrt_str_new(&badPath, "/tmp/lv_selftest_bm3_missing_xyz", 32);
    LvValue existsOut2; lvrt_sysstat(&existsOut2, &badPath, &existsField);
    CHECK(existsOut2.payload == 0);
    LvValue sizeOut2; lvrt_sysstat(&sizeOut2, &badPath, &sizeField);
    CHECK(sizeOut2.payload == -1);

    /* LLVM filesystem parity: the floor stages names once; the runtime turns
     * them into a fresh boxed Array<string> or None. Use a pid-specific tree so
     * runtime_selftest and its optional Valgrind twin may run concurrently. */
    char dir[128], fileA[160], fileB[160];
    snprintf(dir, sizeof dir, "/tmp/lv_selftest_fs_%ld", (long)getpid());
    snprintf(fileA, sizeof fileA, "%s/a.txt", dir);
    snprintf(fileB, sizeof fileB, "%s/b.txt", dir);
    (void)remove(fileA); (void)remove(fileB); (void)remove(dir);

    LvValue dirVal, fileAVal, fileBVal;
    lvrt_str_new(&dirVal, dir, (int64_t)strlen(dir));
    lvrt_str_new(&fileAVal, fileA, (int64_t)strlen(fileA));
    lvrt_str_new(&fileBVal, fileB, (int64_t)strlen(fileB));

    LvValue mkdirOut;
    lvrt_sysmkdir(&mkdirOut, &dirVal);
    CHECK(mkdirOut.tag == LV_INT && mkdirOut.payload == 0);

    LvValue listing;
    lvrt_syslistdir(&listing, &dirVal);
    CHECK(listing.tag == LV_ARR && lv_test_len(listing.payload) == 0);
    lvrt_retain(&listing); lvrt_release(&listing);

    LvValue fileFd;
    lvrt_sysopen(&fileFd, &fileAVal, &flagsWrite);
    CHECK(fileFd.tag == LV_INT && fileFd.payload >= 0);
    if (fileFd.payload >= 0) lvrt_sysclose(&fileFd);

    lvrt_syslistdir(&listing, &dirVal);
    CHECK(listing.tag == LV_ARR && lv_test_len(listing.payload) == 1);
    CHECK(lv_test_arr_has_string(&listing, "a.txt"));
    lvrt_retain(&listing); lvrt_release(&listing);

    LvValue renameOut;
    lvrt_sysrename(&renameOut, &fileAVal, &fileBVal);
    CHECK(renameOut.tag == LV_INT && renameOut.payload == 0);
    lvrt_syslistdir(&listing, &dirVal);
    CHECK(listing.tag == LV_ARR && lv_test_len(listing.payload) == 1);
    CHECK(!lv_test_arr_has_string(&listing, "a.txt"));
    CHECK(lv_test_arr_has_string(&listing, "b.txt"));
    lvrt_retain(&listing); lvrt_release(&listing);

    LvValue removeOut;
    lvrt_sysremove(&removeOut, &fileBVal);
    CHECK(removeOut.tag == LV_INT && removeOut.payload == 0);
    lvrt_sysremove(&removeOut, &dirVal);
    CHECK(removeOut.tag == LV_INT && removeOut.payload == 0);
    lvrt_syslistdir(&listing, &dirVal);
    CHECK(listing.tag == LV_NONE && listing.payload == 0);
    lvrt_sysremove(&removeOut, &dirVal);
    CHECK(removeOut.tag == LV_INT && removeOut.payload == -1);
    lvrt_sysrename(&renameOut, &fileAVal, &fileBVal);
    CHECK(renameOut.tag == LV_INT && renameOut.payload == -1);

    /* A non-empty directory is never removed recursively, and the failed call
     * leaves its child observable. Keep that tree for the ARC churn below. */
    lvrt_sysmkdir(&mkdirOut, &dirVal);
    CHECK(mkdirOut.payload == 0);
    lvrt_sysopen(&fileFd, &fileAVal, &flagsWrite);
    CHECK(fileFd.payload >= 0);
    if (fileFd.payload >= 0) lvrt_sysclose(&fileFd);
    lvrt_sysremove(&removeOut, &dirVal);
    CHECK(removeOut.payload == -1);
    lvrt_syslistdir(&listing, &dirVal);
    CHECK(listing.tag == LV_ARR && lv_test_arr_has_string(&listing, "a.txt"));
    lvrt_retain(&listing); lvrt_release(&listing);

    int64_t listMark = lvrt_live_bytes();
    for (int i = 0; i < 128; i++) {
        LvValue success;
        lvrt_syslistdir(&success, &dirVal);
        CHECK(success.tag == LV_ARR && lv_test_arr_has_string(&success, "a.txt"));
        lvrt_retain(&success); lvrt_release(&success);

        LvValue failure;
        lvrt_syslistdir(&failure, &fileBVal);   /* absent path -> None */
        CHECK(failure.tag == LV_NONE && failure.payload == 0);
        lvrt_retain(&failure); lvrt_release(&failure); /* None retain is a no-op */
    }
    CHECK(lvrt_live_bytes() == listMark);

    lvrt_sysremove(&removeOut, &fileAVal);
    CHECK(removeOut.payload == 0);
    lvrt_sysremove(&removeOut, &dirVal);
    CHECK(removeOut.payload == 0);

    fprintf(stderr, "OK: sys file natives (open/close/stat/read/mkdir/list/rename/remove + ARC churn)\n");
}

static void test_sockets(void) {
    LvValue port = { LV_INT, 18453 };
    LvValue listenFd; lvrt_systcplisten(&listenFd, &port);
    CHECK(listenFd.payload >= 0);

    LvValue host; lvrt_str_new(&host, "127.0.0.1", 9);
    LvValue clientFd; lvrt_systcpconnect(&clientFd, &host, &port);
    CHECK(clientFd.payload >= 0);

    LvValue serverFd = { LV_INT, -1 };
    for (int i = 0; i < 1000 && serverFd.payload < 0; i++)
        lvrt_sysaccept(&serverFd, &listenFd);
    CHECK(serverFd.payload >= 0);

    LvValue msg; lvrt_str_new(&msg, "ping", 4);
    LvValue sent; lvrt_syssend(&sent, &clientFd, &msg);
    CHECK(sent.payload == 4);

    LvValue maxLen = { LV_INT, 64 }, recvOut;
    recvOut.tag = LV_STR; recvOut.payload = 0;
    for (int i = 0; i < 1000; i++) {
        lvrt_sysrecv(&recvOut, &serverFd, &maxLen);
        if (recvOut.tag != LV_STR || lv_test_len(recvOut.payload) != 0) break;
    }
    CHECK(recvOut.tag == LV_STR);
    CHECK(lv_test_len(recvOut.payload) == 4);
    CHECK(memcmp(lv_test_bytes(recvOut.payload), "ping", 4) == 0);

    /* orderly close on the client side must surface as None (EOF), not "" */
    lvrt_sysclose(&clientFd);
    LvValue eofOut; eofOut.tag = LV_STR; eofOut.payload = 0;
    for (int i = 0; i < 1000 && eofOut.tag != LV_NONE; i++)
        lvrt_sysrecv(&eofOut, &serverFd, &maxLen);
    CHECK(eofOut.tag == LV_NONE);

    lvrt_sysclose(&serverFd);
    lvrt_sysclose(&listenFd);

    fprintf(stderr, "OK: sockets (tcp connect/listen/accept/send/recv, EOF->None)\n");
}

/* --- G-LANG-2 process floor (techdesign-spawn-llvm.md §7.4/§7.5) ------------
 * Pins the plat floor + the lvrt marshaling independently of codegen; the
 * valgrind lane settles D1's ARC convention empirically. The reap loops below
 * poll lv_plat_reap directly WITHOUT a pidfd — §7.5's pidfd-fallback lane. */
static int lv_test_reap_poll(int pid) {
    int code = -1;
    for (int i = 0; i < 5000 && code < 0; i++) {
        code = lv_plat_reap(pid);
        if (code < 0) usleep(1000);
    }
    return code;
}

static void test_process_floor(void) {
    /* cycle 1: /bin/echo — spawn, drain stdout, reap 0 */
    char* argv1[] = { (char*)"/bin/echo", (char*)"spawnok", NULL };
    int fds1[3];
    int pid1 = lv_plat_spawn("/bin/echo", argv1, fds1);
    CHECK(pid1 > 0);
    char buf[64]; int64_t got = -1;
    for (int i = 0; i < 5000; i++) {
        got = lv_plat_read(fds1[1], buf, sizeof buf);
        if (got >= 0) break;                  /* -1 = EAGAIN (O_NONBLOCK), retry */
        usleep(1000);
    }
    CHECK(got == 8 && memcmp(buf, "spawnok\n", 8) == 0);
    CHECK(lv_test_reap_poll(pid1) == 0);
    int pfd1 = lv_plat_pidfd_open(pid1);      /* already reaped: -1 or a dead pidfd */
    if (pfd1 >= 0) lv_plat_close(pfd1);
    lv_plat_close(fds1[0]); lv_plat_close(fds1[1]); lv_plat_close(fds1[2]);

    /* cycle 2: /bin/cat — stdin round trip, then kill(SIGTERM) -> 128+15 = 143.
     * The echo-back FIRST is load-bearing, not decoration: it proves the child
     * has actually exec'd into cat before the kill (under valgrind the fork's
     * pre-exec window is slow, and a SIGTERM landing inside the forked-valgrind
     * image doesn't die as a clean signal-terminated cat). */
    char* argv2[] = { (char*)"/bin/cat", NULL };
    int fds2[3];
    int pid2 = lv_plat_spawn("/bin/cat", argv2, fds2);
    CHECK(pid2 > 0);
    CHECK(lv_plat_write(fds2[0], "ping\n", 5) == 5);
    got = -1;
    for (int i = 0; i < 5000; i++) {
        got = lv_plat_read(fds2[1], buf, sizeof buf);
        if (got >= 0) break;                  /* -1 = EAGAIN, cat not through yet */
        usleep(1000);
    }
    CHECK(got == 5 && memcmp(buf, "ping\n", 5) == 0);
    CHECK(lv_plat_kill(pid2, 15) == 0);
    CHECK(lv_test_reap_poll(pid2) == 143);
    lv_plat_close(fds2[0]); lv_plat_close(fds2[1]); lv_plat_close(fds2[2]);
    CHECK(lv_plat_kill(-1, 15) == -1);        /* broadcast forms refused at the floor */
    CHECK(lv_plat_kill(pid2, -1) == -1);

    /* fd hygiene: both cycles fully released their numbers, so a third spawn
     * reuses cycle 2's exact fd triple (the lowest-available rule). */
    int fds3[3];
    int pid3 = lv_plat_spawn("/bin/echo", argv1, fds3);
    CHECK(pid3 > 0);
    CHECK(fds3[0] == fds2[0] && fds3[1] == fds2[1] && fds3[2] == fds2[2]);
    CHECK(lv_test_reap_poll(pid3) >= 0);
    lv_plat_close(fds3[0]); lv_plat_close(fds3[1]); lv_plat_close(fds3[2]);

    /* lvrt marshaling (D1's ARC convention): fresh rc-0 Array<int> of 4,
     * retained/released exactly like codegen's retainDst() + register death. */
    LvValue pathV; lvrt_str_new(&pathV, "/bin/echo", 9);
    LvValue argsV; lvrt_arr_new(&argsV, 1);
    LvValue a0;    lvrt_str_new(&a0, "m", 1);
    ((int64_t*)(intptr_t)(argsV.payload + 8))[0] = a0.tag;   /* element store */
    ((int64_t*)(intptr_t)(argsV.payload + 8))[1] = a0.payload;
    lvrt_retain(&a0);                          /* buffer owns element */
    LvValue sp; lvrt_sysspawn(&sp, &pathV, &argsV);
    CHECK(sp.tag == LV_ARR && lv_test_len(sp.payload) == 4);
    int mpid = (int)((int64_t*)(intptr_t)(sp.payload + 8))[1];
    int min_ = (int)((int64_t*)(intptr_t)(sp.payload + 8 + 16))[1];
    int mout = (int)((int64_t*)(intptr_t)(sp.payload + 8 + 32))[1];
    int merr = (int)((int64_t*)(intptr_t)(sp.payload + 8 + 48))[1];
    CHECK(mpid > 0);
    got = -1;
    for (int i = 0; i < 5000; i++) {
        got = lv_plat_read(mout, buf, sizeof buf);
        if (got >= 0) break;
        usleep(1000);
    }
    CHECK(got == 2 && memcmp(buf, "m\n", 2) == 0);
    CHECK(lv_test_reap_poll(mpid) == 0);
    lv_plat_close(min_); lv_plat_close(mout); lv_plat_close(merr);
    lvrt_retain(&sp);    lvrt_release(&sp);    /* the sysArgs convention: rc-0 fresh */
    lvrt_retain(&pathV); lvrt_release(&pathV);
    lvrt_retain(&argsV); lvrt_release(&argsV); /* recursively drops the element */

    /* empty path -> [] spawn failure (frozen contract) */
    LvValue emptyV; lvrt_str_new(&emptyV, "", 0);
    LvValue noargs; lvrt_arr_new(&noargs, 0);
    LvValue sf; lvrt_sysspawn(&sf, &emptyV, &noargs);
    CHECK(sf.tag == LV_ARR && lv_test_len(sf.payload) == 0);
    lvrt_retain(&sf); lvrt_release(&sf);
    lvrt_retain(&emptyV); lvrt_release(&emptyV);
    lvrt_retain(&noargs); lvrt_release(&noargs);

    fprintf(stderr, "OK: process floor (spawn/drain/reap/kill/pidfd, fd hygiene, [] on empty path)\n");
}

/* --- G-LANG-2 terminal half: pty floor (designs/pty/ 02 §5) -----------------
 * Pins lv_plat_pty_spawn/resize + the lvrt marshaling independently of codegen,
 * and asserts the D-P4 EIO collapse AT THE C LEVEL: on Linux the master's read
 * errors EIO once the child is gone, and lv_plat_recv must still report 0.
 * Without the collapse the drain loop below never terminates. */
static int64_t lv_test_pty_drain(int master, char* buf, int64_t cap) {
    int64_t total = 0;
    for (int i = 0; i < 5000; i++) {
        int64_t r = lv_plat_recv(master, buf + total, cap - total);
        if (r == 0) return total;                 /* orderly close (EIO collapsed) */
        if (r > 0) { total += r; i = 0; continue; }
        usleep(1000);                             /* -1 = EAGAIN, nothing yet */
    }
    return -1;                                    /* never closed: the collapse is missing */
}

static void test_pty_floor(void) {
    /* round 1: /bin/echo on the DETERMINISTIC profile (flags bit0) — the frozen
     * termios's ONLCR turns echo's "\n" into "\r\n", the goldens' shape. */
    char* argv1[] = { (char*)"/bin/echo", (char*)"hi", NULL };
    int m1 = -1;
    int pid1 = lv_plat_pty_spawn("/bin/echo", argv1, 24, 80, 1, &m1);
    CHECK(pid1 > 0 && m1 >= 0);
    char buf[128];
    int64_t got = lv_test_pty_drain(m1, buf, sizeof buf);
    CHECK(got == 4 && memcmp(buf, "hi\r\n", 4) == 0);
    CHECK(lv_test_reap_poll(pid1) == 0);
    CHECK(lv_plat_pty_resize(m1, 30, 100) == 0);  /* live master: accepted */
    lv_plat_close(m1);
    CHECK(lv_plat_pty_resize(m1, 30, 100) == -1); /* closed master: refused */
    CHECK(lv_plat_pty_resize(-1, 30, 100) == -1);

    /* refusals leak nothing: empty path / rows 0 / cols 0 all -1 with no fd
     * consumed, so round 2 reuses round 1's exact master fd number (the
     * lowest-available rule — the spawn selftest's hygiene probe idiom). */
    int mx = -1;
    CHECK(lv_plat_pty_spawn("", argv1, 24, 80, 1, &mx) == -1);
    CHECK(lv_plat_pty_spawn("/bin/echo", argv1, 0, 80, 1, &mx) == -1);
    CHECK(lv_plat_pty_spawn("/bin/echo", argv1, 24, 0, 1, &mx) == -1);
    CHECK(mx == -1);                              /* untouched on every refusal */

    int m2 = -1;
    int pid2 = lv_plat_pty_spawn("/bin/echo", argv1, 24, 80, 1, &m2);
    CHECK(pid2 > 0);
    CHECK(m2 == m1);                              /* ±0 fds across the whole round */
    CHECK(lv_test_pty_drain(m2, buf, sizeof buf) == 4);
    CHECK(lv_test_reap_poll(pid2) == 0);
    lv_plat_close(m2);

    /* the VEOF protocol (D-P3): canonical mode, VEOF frozen at 4, so writing
     * "\x04" is the only way to EOF a pty (there is no closable write half). */
    char* argv3[] = { (char*)"/bin/cat", NULL };
    int m3 = -1;
    int pid3 = lv_plat_pty_spawn("/bin/cat", argv3, 24, 80, 1, &m3);
    CHECK(pid3 > 0);
    CHECK(lv_plat_send(m3, "ping\n", 5) == 5);
    CHECK(lv_plat_send(m3, "\x04", 1) == 1);
    got = lv_test_pty_drain(m3, buf, sizeof buf);
    CHECK(got == 6 && memcmp(buf, "ping\r\n", 6) == 0);   /* echo off, ONLCR on */
    CHECK(lv_test_reap_poll(pid3) == 0);
    lv_plat_close(m3);

    /* lvrt marshaling (K5 ARC convention, settled by the valgrind lane): fresh
     * rc-0 Array<int> of 2, retained/released exactly like codegen's
     * retainDst() + register death. */
    LvValue pathV; lvrt_str_new(&pathV, "/bin/echo", 9);
    LvValue argsV; lvrt_arr_new(&argsV, 1);
    LvValue a0;    lvrt_str_new(&a0, "z", 1);
    ((int64_t*)(intptr_t)(argsV.payload + 8))[0] = a0.tag;
    ((int64_t*)(intptr_t)(argsV.payload + 8))[1] = a0.payload;
    lvrt_retain(&a0);                          /* buffer owns element */
    LvValue rowsV; rowsV.tag = LV_INT; rowsV.payload = 24;
    LvValue colsV; colsV.tag = LV_INT; colsV.payload = 80;
    LvValue flagV; flagV.tag = LV_INT; flagV.payload = 1;
    LvValue sp; lvrt_sysptyspawn(&sp, &pathV, &argsV, &rowsV, &colsV, &flagV);
    CHECK(sp.tag == LV_ARR && lv_test_len(sp.payload) == 2);
    int mpid = (int)((int64_t*)(intptr_t)(sp.payload + 8))[1];
    int mmst = (int)((int64_t*)(intptr_t)(sp.payload + 8 + 16))[1];
    CHECK(mpid > 0 && mmst >= 0);
    got = lv_test_pty_drain(mmst, buf, sizeof buf);
    CHECK(got == 3 && memcmp(buf, "z\r\n", 3) == 0);
    CHECK(lv_test_reap_poll(mpid) == 0);
    LvValue rz; LvValue mfV; mfV.tag = LV_INT; mfV.payload = mmst;
    lvrt_sysptyresize(&rz, &mfV, &rowsV, &colsV);
    CHECK(rz.tag == LV_INT && rz.payload == 0);
    lv_plat_close(mmst);
    lvrt_retain(&sp);    lvrt_release(&sp);    /* the sysArgs convention: rc-0 fresh */
    lvrt_retain(&pathV); lvrt_release(&pathV);
    lvrt_retain(&argsV); lvrt_release(&argsV); /* recursively drops the element */

    /* bad args -> [] (frozen: empty array, not None) */
    LvValue emptyV; lvrt_str_new(&emptyV, "", 0);
    LvValue noargs; lvrt_arr_new(&noargs, 0);
    LvValue sf; lvrt_sysptyspawn(&sf, &emptyV, &noargs, &rowsV, &colsV, &flagV);
    CHECK(sf.tag == LV_ARR && lv_test_len(sf.payload) == 0);
    lvrt_retain(&sf); lvrt_release(&sf);
    LvValue zero; zero.tag = LV_INT; zero.payload = 0;
    LvValue okp; lvrt_str_new(&okp, "/bin/echo", 9);
    LvValue sf2; lvrt_sysptyspawn(&sf2, &okp, &noargs, &zero, &colsV, &flagV);
    CHECK(sf2.tag == LV_ARR && lv_test_len(sf2.payload) == 0);
    lvrt_retain(&sf2); lvrt_release(&sf2);
    lvrt_retain(&okp);    lvrt_release(&okp);
    lvrt_retain(&emptyV); lvrt_release(&emptyV);
    lvrt_retain(&noargs); lvrt_release(&noargs);

    fprintf(stderr, "OK: pty floor (spawn/termios/drain, EIO collapse, VEOF, resize, "
                    "fd hygiene, [] on bad args)\n");
}

/* ==========================================================================
 * LA-30 §9 — task-substrate selftest additions
 * (designs/suspension/techdesign-01-task-substrate.md; engine-free, same
 * harness discipline as the rest of this file). Item #8 of §9 — the whole
 * suite under LANG_RT_SANITIZE=ON — is a build lane, not a function here.
 * ========================================================================== */
#include "lv_task.h"
#include <sys/wait.h>
#include <time.h>

/* mirror lv_task.c's sanitizer detection for the gates below */
#if defined(__SANITIZE_ADDRESS__)
# define LV_ST_ASAN 1
#elif defined(__has_feature)
# if __has_feature(address_sanitizer)
#  define LV_ST_ASAN 1
# endif
#endif
#ifndef LV_ST_ASAN
# define LV_ST_ASAN 0
#endif
#if defined(__has_include)
# if __has_include(<valgrind/valgrind.h>)
#  include <valgrind/valgrind.h>
#  define LV_ST_VALGRIND RUNNING_ON_VALGRIND
# endif
#endif
#ifndef LV_ST_VALGRIND
# define LV_ST_VALGRIND 0
#endif

static int g_tlog[16]; static int g_tlogN;
static void tlog(int v) { if (g_tlogN < 16) g_tlog[g_tlogN++] = v; }

static int poll_intflag(const void* obj) { return *(const int*)obj ? 1 : 0; }

/* generic parker: a = int-flag key, b = log base. Logs base on entry, base+1
 * on a SETTLED resume, base+2 on a DRAINED resume. */
static void st_parker(void* a, void* b) {
    int base = (int)(intptr_t)b;
    CHECK(lv_task_self() != NULL);
    tlog(base);
    int r = lv_task_park_on(a);
    tlog(r == LV_PARK_SETTLED ? base + 1 : (r == LV_PARK_DRAINED ? base + 2 : -base));
}
static void st_logger(void* a, void* b) { (void)b; tlog((int)(intptr_t)a); }
static void st_noop(void* a, void* b)   { (void)a; (void)b; }

/* fork harness: run fn in a child with stderr piped back; used by the stats-
 * shape (#7), pool (#3), and guard-page (#5) tests. */
static void run_task_child(void (*fn)(void), int* status, char* out, size_t cap) {
    int pfd[2];
    CHECK(pipe(pfd) == 0);
    fflush(NULL);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        dup2(pfd[1], 2);
        close(pfd[0]); close(pfd[1]);
        fn();
        _exit(g_failures ? 1 : 0);
    }
    close(pfd[1]);
    size_t n = 0; ssize_t r;
    while (n < cap - 1 && (r = read(pfd[0], out + n, cap - 1 - n)) > 0) n += (size_t)r;
    out[n] = 0;
    close(pfd[0]);
    waitpid(pid, status, 0);
}

static long long stat_field(const char* out, const char* key) {
    const char* p = strstr(out, key);
    return p ? atoll(p + strlen(key)) : -1;
}

/* --- §9 #1: spawn/park/wake round-trip; FIFO order of 3 tasks --- */
static int g_key1;
static int step_settle_key1(void) { if (!g_key1) { g_key1 = 1; return 1; } return 0; }
static void t1_tasks_main(void* a, void* b) {
    (void)a; (void)b;
    tlog(1);
    lv_task_spawn(st_parker, &g_key1, (void*)(intptr_t)10);
    lv_task_spawn(st_logger, (void*)2, NULL);
    lv_task_spawn(st_logger, (void*)3, NULL);
    tlog(4);
}
static void test_tasks_basic(void) {
    setenv("LANG_TASKS", "1", 1);
    CHECK(lv_tasks_enabled() == 1);
    lv_sched_init();
    CHECK(lv_task_self() == NULL);          /* scheduler context */
    g_tlogN = 0; g_key1 = 0;
    lv_sched_hooks(poll_intflag, step_settle_key1);
    lv_sched_run(t1_tasks_main, NULL, NULL);
    /* task 0 runs whole (1,4); then FIFO: parker(10), loggers 2,3; the loop
     * batch settles the key; the settle-scan wakes the parker -> 11. */
    CHECK(g_tlogN == 6);
    CHECK(g_tlog[0] == 1 && g_tlog[1] == 4 && g_tlog[2] == 10 &&
          g_tlog[3] == 2 && g_tlog[4] == 3 && g_tlog[5] == 11);
    CHECK(lv_task_self() == NULL);
    fprintf(stderr, "OK: tasks basic (spawn/park/settle-wake round-trip, FIFO of 3)\n");
}

/* --- §9 #2: 1M ping-pong switches; perf gate --- */
static void t3_tasks_main(void* a, void* b);   /* pool-cycling main, defined at #3 below */
static long long g_ppDone;
static void t2_pp(void* a, void* b) {
    (void)b;
    long n = (long)(intptr_t)a;
    for (long i = 0; i < n; i++) lv_task_yield();
    g_ppDone++;
}
static void t2_tasks_main(void* a, void* b) {
    (void)b;
    lv_task_spawn(t2_pp, a, NULL);
    lv_task_spawn(t2_pp, a, NULL);
}
static void test_tasks_bench(void) {
    long n = LV_ST_VALGRIND ? 5000 : 250000;   /* 2 tasks x n yields x 2 = ~1M switches */
    g_ppDone = 0;
    lv_sched_hooks(NULL, NULL);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    lv_sched_run(t2_tasks_main, (void*)(intptr_t)n, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    CHECK(g_ppDone == 2);
    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
    double switches = 4.0 * (double)n;
    fprintf(stderr, "OK: tasks bench (%.0f switches in %.3fs = %.1fM/s)\n",
            switches, secs, switches / secs / 1e6);
#if !LV_ST_ASAN && !defined(LV_CTX_UCONTEXT)
    if (!LV_ST_VALGRIND)
        CHECK(switches / secs >= 20e6);   /* M0 perf gate (§12); ucontext build exempt (§9) */
#endif

    /* §12's second gate: spawn/complete >= 2M/s pooled (stats off, so the
     * completion path carries no mincore sample). Reuses the pool-cycling
     * main: each round is one full spawn -> run -> complete -> pool return. */
    long rounds = LV_ST_VALGRIND ? 5000 : 500000;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    lv_sched_run(t3_tasks_main, (void*)(intptr_t)rounds, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
    fprintf(stderr, "OK: tasks spawn bench (%ld spawn/complete in %.3fs = %.2fM/s pooled)\n",
            rounds, secs, (double)rounds / secs / 1e6);
#if !LV_ST_ASAN && !defined(LV_CTX_UCONTEXT)
    if (!LV_ST_VALGRIND)
        CHECK((double)rounds / secs >= 2e6);
#endif
}

/* --- §9 #4: drain-wake — never-settling keys; LV_PARK_DRAINED, park order --- */
static int g_dead1, g_dead2;
static int step_none(void) { return 0; }
static void t4_tasks_main(void* a, void* b) {
    (void)a; (void)b;
    lv_task_spawn(st_parker, &g_dead1, (void*)(intptr_t)100);
    lv_task_spawn(st_parker, &g_dead2, (void*)(intptr_t)200);
}
static void test_tasks_drain(void) {
    g_tlogN = 0; g_dead1 = g_dead2 = 0;
    lv_sched_hooks(poll_intflag, step_none);
    lv_sched_run(t4_tasks_main, NULL, NULL);
    /* park order 100,200; the quiescent drain wakes both DRAINED, park order */
    CHECK(g_tlogN == 4);
    CHECK(g_tlog[0] == 100 && g_tlog[1] == 200 &&
          g_tlog[2] == 102 && g_tlog[3] == 202);
    fprintf(stderr, "OK: tasks drain-wake (LV_PARK_DRAINED, park order)\n");
}

/* --- §9 #6: poll determinism — settle both between polls; wake order = park order --- */
static int g_k61, g_k62, g_step6Done;
static int step_settle_both(void) {
    if (!g_step6Done) { g_step6Done = 1; g_k61 = 1; g_k62 = 1; return 1; }
    return 0;
}
static void t6_tasks_main(void* a, void* b) {
    (void)a; (void)b;
    lv_task_spawn(st_parker, &g_k61, (void*)(intptr_t)300);
    lv_task_spawn(st_parker, &g_k62, (void*)(intptr_t)400);
}
static void test_tasks_poll_determinism(void) {
    g_tlogN = 0; g_k61 = g_k62 = g_step6Done = 0;
    lv_sched_hooks(poll_intflag, step_settle_both);
    lv_sched_run(t6_tasks_main, NULL, NULL);
    CHECK(g_tlogN == 4);
    CHECK(g_tlog[0] == 300 && g_tlog[1] == 400 &&
          g_tlog[2] == 301 && g_tlog[3] == 401);   /* both SETTLED, in park order */
    fprintf(stderr, "OK: tasks poll determinism (settle both, wake order = park order)\n");
}

/* --- §9 #3 + #7: pool reuse (10k spawn/complete, mapped <= 9) + stats shape --- */
static void t3_tasks_main(void* a, void* b) {
    (void)b;
    long rounds = (long)(intptr_t)a;
    for (long i = 0; i < rounds; i++) {
        lv_task_spawn(st_noop, NULL, NULL);
        lv_task_yield();          /* FIFO: the noop runs to completion first */
    }
}
static void child_pool_stats(void) {
    setenv("LANG_TASK_STATS", "1", 1);
    lv_sched_hooks(NULL, NULL);
    lv_sched_run(t3_tasks_main, (void*)(intptr_t)(LV_ST_VALGRIND ? 2000 : 10000), NULL);
}
static void test_tasks_pool_stats(void) {
    char out[4096]; int status = 0;
    run_task_child(child_pool_stats, &status, out, sizeof out);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    /* #7 stats line shape — the grep-able tokens */
    CHECK(strstr(out, "[tasks] spawned=") != NULL);
    CHECK(strstr(out, "stacks: mapped=") != NULL);
    CHECK(strstr(out, "drained_wakes=") != NULL);
    CHECK(strstr(out, "hwm_committed=") != NULL);
    /* #3 pool reuse: every completion returned its stack for the next spawn */
    long long spawned = stat_field(out, "spawned=");
    long long completed = stat_field(out, "completed=");
    long long mapped = stat_field(out, "mapped=");
    CHECK(spawned > 10000 || LV_ST_VALGRIND);
    CHECK(spawned == completed);
    CHECK(mapped >= 1 && mapped <= 9);
    fprintf(stderr, "OK: tasks pool+stats (spawned=%lld mapped=%lld <= 9, [tasks] line shape)\n",
            spawned, mapped);
}

/* ==================== LA-30 B2 (doc 06) — cancel / shield / multi-park ======
 * C-level proofs of the substrate semantics the language legs build on:
 * delivery-at-park-only, mark-consumed-on-delivery, settle-wins, the shield
 * mask, park_any2's which-index, join, and the §8 uncancellable report. */

/* st_parker's B2 twin: logs -base on a CANCELLED resume (st_parker already
 * does); this one parks TWICE — first on `a`, then on g_b2_dead — so the
 * mark-holds-through-a-settled-park cases can observe the second park. */
static int g_b2_dead;
static void st_parker2(void* a, void* b) {
    int base = (int)(intptr_t)b;
    tlog(base);
    int r = lv_task_park_on(a);
    tlog(r == LV_PARK_SETTLED ? base + 1 : (r == LV_PARK_DRAINED ? base + 2 : -(base + 1)));
    if (r != LV_PARK_SETTLED) return;
    r = lv_task_park_on(&g_b2_dead);
    tlog(r == LV_PARK_CANCELLED ? -(base + 2) : base + 3);
}

/* shielded parker: shield(+1), park `a`, unshield, park g_b2_dead. A cancel
 * while shield-parked must NOT wake it; the held mark must deliver at the
 * second (unshielded) park's entry. */
static void st_shielded(void* a, void* b) {
    int base = (int)(intptr_t)b;
    tlog(base);
    lv_task_shield(1);
    int r = lv_task_park_on(a);
    tlog(r == LV_PARK_SETTLED ? base + 1 : -(base + 1));
    lv_task_shield(-1);
    r = lv_task_park_on(&g_b2_dead);
    tlog(r == LV_PARK_CANCELLED ? -(base + 2) : base + 3);
}

static void st_any2(void* a, void* b) {
    (void)a; (void)b;
    /* two dead keys settled by the main task while we are parked */
    static int kA, kB; kA = 0; kB = 1;      /* B pre-settled: first poll wins ix=1 */
    int which = -1;
    int r = lv_task_park_any2(&kA, &kB, &which);
    tlog(r == LV_PARK_SETTLED ? 500 + which : -500);
    kA = 1; kB = 0;                          /* now A settled: ix=0 */
    which = -1;
    r = lv_task_park_any2(&kA, &kB, &which);
    tlog(r == LV_PARK_SETTLED ? 510 + which : -510);
}

/* --- B2 #1: cancel-at-park wake, park-entry delivery, idempotence --- */
static void b2_noop(void* a, void* b) { (void)a; (void)b; tlog(700); }
static void b2_main_cancel(void* a, void* b) {
    (void)a; (void)b;
    g_b2_dead = 0;
    /* parked cancel: child parks on a dead key, then we cancel it */
    uint64_t id1 = lv_task_spawn_registered(st_parker, &g_b2_dead, (void*)(intptr_t)600);
    lv_task_yield();                          /* child runs: logs 600, parks     */
    CHECK(lv_task_cancel(id1) == 1);          /* dequeue + wake CANCELLED        */
    CHECK(lv_task_cancel(id1) == 1);          /* double-cancel: marked no-op     */
    lv_task_yield();                          /* child resumes: logs -600, dies  */
    CHECK(lv_task_cancel(id1) == 0);          /* done: idempotent no-op          */
    /* park-entry delivery: mark BEFORE the child ever parks */
    uint64_t id2 = lv_task_spawn_registered(st_parker, &g_b2_dead, (void*)(intptr_t)610);
    CHECK(lv_task_cancel(id2) == 1);          /* RUNNABLE: mark only             */
    lv_task_yield();                          /* child: logs 610, entry-delivery -610 */
    /* a plain (unregistered) noop is unreachable by id */
    lv_task_spawn(b2_noop, NULL, NULL);
    lv_task_yield();
}
static void test_tasks_b2_cancel(void) {
    g_tlogN = 0;
    lv_sched_hooks(poll_intflag, step_none);
    lv_sched_run(b2_main_cancel, NULL, NULL);
    CHECK(g_tlogN == 5);
    CHECK(g_tlog[0] == 600 && g_tlog[1] == -600 &&
          g_tlog[2] == 610 && g_tlog[3] == -610 && g_tlog[4] == 700);
    fprintf(stderr, "OK: tasks B2 cancel (parked wake, entry delivery, idempotent)\n");
}

/* --- B2 #2: settle-wins race + mark holds for the NEXT park --- */
static int g_b2_k1;
static void b2_main_settlewins(void* a, void* b) {
    (void)a; (void)b;
    g_b2_k1 = 0; g_b2_dead = 0;
    uint64_t id = lv_task_spawn_registered(st_parker2, &g_b2_k1, (void*)(intptr_t)620);
    lv_task_yield();                          /* child: logs 620, parks on k1    */
    g_b2_k1 = 1;                              /* settle FIRST ...                */
    CHECK(lv_task_cancel(id) == 1);           /* ... then cancel: settle wins    */
    /* child stays parked; the scheduler's poll delivers SETTLED (621), the held
     * mark then fires at its second park's ENTRY (-622). */
}
static void test_tasks_b2_settle_wins(void) {
    g_tlogN = 0;
    lv_sched_hooks(poll_intflag, step_none);
    lv_sched_run(b2_main_settlewins, NULL, NULL);
    CHECK(g_tlogN == 3);
    CHECK(g_tlog[0] == 620 && g_tlog[1] == 621 && g_tlog[2] == -622);
    fprintf(stderr, "OK: tasks B2 settle-wins (settled park delivers; mark holds for next park)\n");
}

/* --- B2 #3: shield masks delivery; held mark fires after unshield --- */
static void b2_main_shield(void* a, void* b) {
    (void)a; (void)b;
    g_b2_k1 = 0; g_b2_dead = 0;
    uint64_t id = lv_task_spawn_registered(st_shielded, &g_b2_k1, (void*)(intptr_t)630);
    lv_task_yield();                          /* child: 630, shield, parks k1    */
    CHECK(lv_task_cancel(id) == 1);           /* shielded: mark only, stays parked */
    g_b2_k1 = 1;                              /* settle: poll wakes it SETTLED   */
    /* child: 631, unshields, parks dead -> entry-delivery -632 */
}
static void test_tasks_b2_shield(void) {
    g_tlogN = 0;
    lv_sched_hooks(poll_intflag, step_none);
    lv_sched_run(b2_main_shield, NULL, NULL);
    CHECK(g_tlogN == 3);
    CHECK(g_tlog[0] == 630 && g_tlog[1] == 631 && g_tlog[2] == -632);
    fprintf(stderr, "OK: tasks B2 shield (masked while shielded; delivers after unshield)\n");
}

/* --- B2 #4: park_any2 which-index (pre-settled and poll-settled) + join --- */
static void b2_join_child(void* a, void* b) {
    (void)b;
    tlog((int)(intptr_t)a);
}
static void b2_main_any2_join(void* a, void* b) {
    (void)a; (void)b;
    lv_task_spawn(st_any2, NULL, NULL);
    lv_task_yield();                          /* any2 parks; next quiescence polls */
    lv_task_yield();                          /* let both any2 rounds complete     */
    /* join: two registered children run to completion, then the join returns */
    uint64_t ids[2];
    ids[0] = lv_task_spawn_registered(b2_join_child, (void*)(intptr_t)801, NULL);
    ids[1] = lv_task_spawn_registered(b2_join_child, (void*)(intptr_t)802, NULL);
    int r = lv_task_park_join(ids, 2);
    tlog(r == LV_PARK_SETTLED ? 810 : -810);
    r = lv_task_park_join(ids, 2);            /* all done: fast path, no park    */
    tlog(r == LV_PARK_SETTLED ? 811 : -811);
    r = lv_task_park_join(NULL, 0);           /* empty: fast path                */
    tlog(r == LV_PARK_SETTLED ? 812 : -812);
}
static void test_tasks_b2_any2_join(void) {
    g_tlogN = 0;
    lv_sched_hooks(poll_intflag, step_none);
    lv_sched_run(b2_main_any2_join, NULL, NULL);
    /* deterministic trace: the join children run while any2+main are parked
     * (801, 802); the quiescence poll then wakes BOTH in park order — any2
     * first (501: B pre-settled -> which==1, then re-parks), main second
     * (810/811/812: join wake + both fast paths); the final poll delivers
     * any2's second round (510: A settled -> which==0). */
    CHECK(g_tlogN == 7);
    CHECK(g_tlog[0] == 801 && g_tlog[1] == 802);
    CHECK(g_tlog[2] == 501);
    CHECK(g_tlog[3] == 810 && g_tlog[4] == 811 && g_tlog[5] == 812);
    CHECK(g_tlog[6] == 510);
    fprintf(stderr, "OK: tasks B2 any2+join (which-index, join wake, fast paths)\n");
}

/* --- B2 #5: the §8 uncancellable report — fork child, tiny threshold --- */
static int g_b2_step_calls;
static int b2_step_slow_settle(void) {
    struct timespec ts = {0, 3 * 1000000};    /* 3ms: outlast the 1ms threshold */
    nanosleep(&ts, NULL);
    g_b2_step_calls++;
    if (g_b2_step_calls == 2) { g_b2_k1 = 1; return 1; }   /* settle counts as work: the
                                                              NEXT poll must see it before
                                                              the drain branch can fire */
    return g_b2_step_calls < 2 ? 1 : 0;
}
static void b2_main_uncancellable(void* a, void* b) {
    (void)a; (void)b;
    g_b2_k1 = 0; g_b2_dead = 0; g_b2_step_calls = 0;
    uint64_t ids[1];
    ids[0] = lv_task_spawn_registered(st_shielded, &g_b2_k1, (void*)(intptr_t)640);
    lv_task_yield();                          /* child shields + parks k1        */
    CHECK(lv_task_cancel(ids[0]) == 1);       /* mark holds (shielded refuser)   */
    int r = lv_task_park_join(ids, 1);        /* wait; report fires mid-wait     */
    tlog(r == LV_PARK_SETTLED ? 820 : -820);
}
static void child_uncancellable(void) {
    setenv("LANG_TASK_UNCANCELLABLE_MS", "1", 1);
    g_tlogN = 0;
    lv_sched_hooks(poll_intflag, b2_step_slow_settle);
    lv_sched_run(b2_main_uncancellable, NULL, NULL);
    CHECK(g_tlogN == 4);
    CHECK(g_tlog[0] == 640 && g_tlog[1] == 641 && g_tlog[2] == -642 && g_tlog[3] == 820);
}
static void test_tasks_b2_uncancellable_report(void) {
    char out[4096]; int status = 0;
    run_task_child(child_uncancellable, &status, out, sizeof out);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(strstr(out, "[tasks] uncancellable=1") != NULL);
    fprintf(stderr, "OK: tasks B2 uncancellable report (loud, not hung; join still completes)\n");
}

/* --- §9 #5: guard page — child overflows deliberately; diagnostic + death signal.
 * Skipped under ASan: ASan owns SIGSEGV classification there (its stack-overflow
 * report supersedes the runtime's diagnostic); the plain and valgrind lanes
 * assert the substrate's own handler. --- */
#if !LV_ST_ASAN
__attribute__((noinline)) static int t5_recurse(int depth) {
    volatile char pad[512];                          /* volatile: the frame must exist */
    pad[0] = (char)depth; pad[511] = (char)depth;
    if (depth > 0) return t5_recurse(depth + 1) + (int)pad[0];   /* no tail call */
    return 0;
}
static void t5_overflow(void* a, void* b) { (void)a; (void)b; t5_recurse(1); }
static void child_overflow(void) {
    lv_sched_hooks(NULL, NULL);
    lv_sched_run(t5_overflow, NULL, NULL);
}
static void test_tasks_guard_page(void) {
    char out[4096]; int status = 0;
    run_task_child(child_overflow, &status, out, sizeof out);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    CHECK(strstr(out, "task stack overflow (task #") != NULL);
    CHECK(strstr(out, "raise LANG_TASK_STACK") != NULL);
    fprintf(stderr, "OK: tasks guard page (overflow diagnostic, SIGSEGV death)\n");
}
#endif

int main(void) {
    lv_rt_init(0, NULL);
    setup_classes();

    test_allocator();
    test_arc_object_graph();
    test_strings();
    test_arrays_maps();
    test_idxset_move();   /* known_bugs_2.md worktree-agent-a7c3e630889a1bf22-107 */
    test_columnar();
    test_map_with();
    test_dispatch_and_issub();
    test_scalar_core();
    test_to_string();
    test_exceptions();
    test_exit_code();
    test_arena_hardening();
    test_event_loop();
    test_sys_file_natives();
    test_sockets();
    test_process_floor();   /* G-LANG-2 (techdesign-spawn-llvm.md §7.4) */
    test_pty_floor();       /* G-LANG-2 terminal half (designs/pty/ 02 §5) */

    /* LA-30 §9 — task substrate */
    test_tasks_basic();
    test_tasks_bench();
    test_tasks_drain();
    test_tasks_poll_determinism();
    /* LA-30 B2 (doc 06) — cancel/shield/multi-park/join */
    test_tasks_b2_cancel();
    test_tasks_b2_settle_wins();
    test_tasks_b2_shield();
    test_tasks_b2_any2_join();
    /* The fork-based tests run a child that either exits or deliberately
     * SIGSEGVs; under valgrind a forked child stays instrumented and its exit
     * code / deliberate guard-page fault collide with --error-exitcode. They
     * validate pool watermarks + the guard-page diagnostic, not the
     * VALGRIND_STACK_REGISTER annotation the valgrind lane exists to prove
     * (every in-process switch above already exercises that), so skip them
     * under valgrind — mirrors the perf-gate exemption (#2). */
    if (!LV_ST_VALGRIND) {
        test_tasks_pool_stats();
        test_tasks_b2_uncancellable_report();   /* fork child: report grep + timing */
#if !LV_ST_ASAN
        test_tasks_guard_page();
#endif
    } else {
        fprintf(stderr, "SKIP: tasks pool/stats + guard-page under valgrind "
                        "(fork+deliberate-fault vs --error-exitcode)\n");
    }

    if (g_failures) {
        fprintf(stderr, "%d CHECK(S) FAILED\n", g_failures);
        return 1;
    }
    fprintf(stderr, "ALL TESTS PASSED (peak=%lld live-at-exit=%lld)\n",
            (long long)lvrt_peak_bytes(), (long long)lvrt_live_bytes());
    return 0;
}
