/* Track B — the normative contract between generated code and the runtime.
 * See docs/techdesign-portable-backend-2.md §2. Track A codes against this
 * file; Track B implements it in lv_runtime.c. Neither track changes the
 * shapes below unilaterally (contract changes are a STOP event, doc §0.2).
 *
 * Boundary rule (§2.1, hurdle H-1): every value crosses the generated<->
 * runtime boundary as an `LvValue*` pointer (out-param first, then inputs).
 * No function here passes or returns LvValue by value — clang lowers
 * 16-byte structs differently per target (SysV splits into two scalars,
 * Win64 passes by pointer); the pointer convention sidesteps that entirely.
 */
#ifndef LV_ABI_H
#define LV_ABI_H

#include <stdint.h>

/* Track 10 (threads): the runtime's mutable hot-state globals are thread-local
 * so each worker gets its own allocator/loop/throw state (per-worker heaps,
 * info.md §14 isolation). Threads are POSIX/LLVM-only in v1; a Windows build
 * never spawns, and mingw's emulated-TLS ABI does not agree with LLVM's native
 * COFF TLS lowering, so on Windows these stay plain single-thread globals. The
 * LlvmGen codegen side gates the matching InitialExec/non-TLS choice on the
 * same `!_WIN32` condition (src/LlvmGen.cpp emitObject). */
#if defined(_WIN32)
#define LV_TLS
#else
#define LV_TLS _Thread_local
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ============================== §2.3 Values ============================ */

typedef struct LvValue { int64_t tag; int64_t payload; } LvValue;   /* 16 bytes */

enum {
    LV_VOID = 0, LV_INT = 1, LV_FLOAT = 2, LV_BOOL = 3, LV_STR = 4,
    LV_OBJ  = 5, LV_ARR  = 6, LV_MAP  = 7, LV_NONE = 8, LV_CLO  = 9,
    /* Track 03 §1/§3 addendum (deferal-char-block-abi.md): the closed 0-9 set
     * is extended by two co-landed tags. LV_CHAR is a PURE IMMEDIATE (payload
     * = the Unicode scalar; no heap, retain/release no-op — like INT/BOOL).
     * LV_BLOCK is a HEAP type (body {parentPtr, off, len, dataPtr}); a slice
     * sets parentPtr to the ROOT and retains it, so the shared bytes outlive
     * any single view. Both land here together with lv_runtime.c's matching
     * ARC/to_string rows and LlvmGen's native emission — a contract change is
     * never half-shipped (doc-2 §0.2). */
    LV_CHAR = 10, LV_BLOCK = 11,
    /* F5 runtime-internal proxy. Never visible as a language value; weak
     * field slots own this cell while the cell's target is non-owning. */
    LV_WEAK_PROXY = 12
};

/* Numbering follows the CGen/X64Gen reference (src/X64Gen.hpp:16-23) — NOT
 * the VKind enum order (src/RuntimeValue.hpp:22), which differs (hurdle
 * H-3: VKind orders Closure before Array; copying that order corrupts every
 * array/closure dispatch). LV_FLOAT stores the double bit pattern in
 * `payload` (memcpy, never a cast through long). LV_BOOL/LV_INT/LV_CHAR store
 * the value directly; tags 4/5/6/7/9/11 store heap pointers per §2.4 below.
 * Tag-5 payloads may point INLINE into a dense array buffer (value-struct
 * records, §2.4) — never assume a tag-5 pointer owns an allocation header;
 * always isvalueclass()-gate before touching [payload-16].
 */

/* ============================ §2.4 Heap layouts ==========================
 *
 * LvHeader, immediately BEFORE every counted heap payload (at payload-16):
 *   int64_t rc;    -1 = arena allocation (never counted, never freed via
 *                  release — bulk-reclaimed by lvrt_arena_restore);
 *                   0 = fresh/unowned (retain()'s target; release() on
 *                  rc<=0 is a documented NO-OP — extracted from X64Gen's
 *                  genRelease "<=0 skip" gate: an object is only ever freed
 *                  by the release that brings a *counted* (>=1) rc back to
 *                  zero, not by the implicit "nobody aliased it" case);
 *                  >=1 = owned (aliased at least once).
 *   int64_t meta;  type-specific bookkeeping, read only by lv_runtime.c —
 *                  never by generated code. See per-type notes below.
 *
 * Extraction note (hurdle H-2): X64Gen.hpp:50-53's "[classId][fieldHead]
 * linked-node" comment describes a pre-packed-slot era and is stale; the
 * facts below come from the current genMkObj/genGetField/genIdxSet/
 * genRecursiveFree code (src/X64Gen.cpp), the ground truth.
 *
 * string  (LV_STR): payload -> { int64_t len; char bytes[len]; char nul; }
 *   meta = total raw allocation bytes (header included). Literal strings
 *   (Track A's LLVM constant data) carry NO header at all and are exempt
 *   from ARC by the region-registry check in lvrt_retain/lvrt_release —
 *   never dereference payload-16 without first confirming the payload is
 *   runtime-owned (in a registered heap/arena region).
 *   DEVIATION FROM X64GEN (logged techdesign-portable-backend-2.md §10):
 *   X64Gen's descriptors have no trailing NUL; this runtime adds one for
 *   C-string interop, matching this doc's normative §2.4 shape.
 *
 * object  (LV_OBJ): payload -> { int64_t classId; int64_t dyn;
 *   LvValue slots[nslots]; }  — slot i at byte offset 16 + 16*i.
 *   `dyn` heads a singly-linked dynamic-key fallback list (used when
 *   lvrt_fieldindex() returns -1 — in practice only closure captures hit
 *   this path; declared fields always resolve to a fixed slot). Node is a
 *   RAW 32-byte block with NO LvHeader (freed directly with size 32, never
 *   through the size-classed ARC accounting path):
 *     { int64_t next; int64_t keyPtr; int64_t valTag; int64_t valPay; }
 *   keyPtr is an INTERNED key pointer, compared by pointer identity — the
 *   caller must pass the same pointer for the same field/capture name at
 *   every call site (mirrors X64Gen's genFieldIndex/genCapGet/genCapSet,
 *   which rely on its own internString() dedup cache for the same reason).
 *   meta = total raw allocation bytes.
 *
 * array   (LV_ARR): THREE representations, discriminated by the top two bits
 *   of the first body field (bit 63 = "not boxed"; bit 62 = "columnar").
 *   BOXED:  payload -> { int64_t len; LvValue elems[capacity]; }
 *           word0 = len (>= 0, both top bits clear).
 *           header meta = capacity (a SLOT COUNT, not a byte size — matches
 *           X64Gen's genMkArr/genArrAppend; lv_runtime.c recomputes the
 *           byte size for freeing as 24 + 16*capacity).
 *           COW (§2.5): idxset mutates in place when rc==1; else copies.
 *   DENSE:  payload -> { int64_t lenWithBit; int64_t recBytes;
 *           uint8_t records[cleanLen * recBytes]; }
 *           lenWithBit = cleanLen | (1<<63)  (bit 62 CLEAR). Each record is a
 *           full inline object body (classId, dyn=0, declared slots) —
 *           reading an element yields a tag-5 LvValue whose payload POINTS
 *           INTO the buffer (§2.3). Dense arrays only ever hold value-struct
 *           elements (never refcounted), so recursiveFree does no
 *           per-element work — just frees the buffer.
 *           header meta = true total raw bytes. DEVIATION FROM X64GEN:
 *           X64Gen overwrites its header-size slot with recBytes for dense
 *           arrays (a stale-accounting quirk — hfree is then handed
 *           recBytes instead of the true allocation size); this runtime
 *           stores recBytes in the BODY instead and keeps meta accurate,
 *           to avoid a real (if invisible-to-corpus) live-bytes skew.
 *   COLUMNAR (struct-of-arrays; techdesign-columnar-arrays.md §4):
 *           payload -> { int64_t lenWithBits; int64_t classId;
 *                        uint8_t columns[fieldCount * 8 * cleanLen]; }
 *           lenWithBits = cleanLen | (1<<63) | (1<<62). cleanLen recovered
 *           by masking BOTH top bits: word0 & ~(3ULL<<62). Word 1 stores the
 *           element classId (dense stores recBytes there instead — bit 62
 *           disambiguates). The body is fieldCount contiguous 8-byte-stride
 *           columns; field k of element i is the raw PAYLOAD word (no tag) at
 *           columnBase(k) + 8*i, where columnBase(k) = payload + 16 +
 *           k*8*cleanLen. Tags are synthesized on read from the emitted
 *           per-class descriptor (lv_col_typecode). Eligibility (all fields
 *           scalar int/float/bool/char) guarantees the buffer holds NO heap
 *           references, so — like dense — recursiveFree just frees the buffer
 *           and a columnar array NEVER hands out a pointer into itself (every
 *           element read gathers a fresh standalone record, §2.3 tag-5 caveat
 *           is NOT extended to columnar). length == capacity (append copies +
 *           grows, no slack — parity with dense). header meta = true total
 *           raw bytes. This layout is chosen at flip time only when generated
 *           code was compiled columnar (lv_cfg_columnar) and the element class
 *           is eligible (lv_col_eligible); otherwise the array goes dense.
 *
 * map     (LV_MAP): payload -> { int64_t len; LvPair entries[len]; }
 *   LvPair = { LvValue k; LvValue v; } (32 bytes). Insertion order is
 *   preserved and observable (linear scan; append or in-place update).
 *   meta = total raw allocation bytes.
 *
 * closure (LV_CLO): payload -> { int64_t fnIndex; int64_t captureHead; }
 *   Reuses the object dynamic-fallback-list mechanism verbatim — X64Gen's
 *   MakeClosure literally calls its mkobj() helper with the closure's
 *   fnIndex in place of a classId and zero declared slots, so captureHead
 *   is a dyn-list head identical in shape to an object's. CaptureVar
 *   retains the captured value (the node owns it); recursiveFree walks the
 *   list, releasing each value and freeing each 32-byte node, then frees
 *   the closure body itself.
 *   meta = total raw allocation bytes (32: fnIndex + captureHead, no
 *   declared slots).
 *
 * block   (LV_BLOCK, Track 03 §3): payload -> { int64_t parentPtr;
 *   int64_t off; int64_t len; int64_t dataPtr; } (32-byte body).
 *   A ROOT block has parentPtr==0 and OWNS a separately-allocated byte
 *   buffer at dataPtr (its own lv_halloc_prefixed alloc; the buffer's header
 *   rc is unused — the block object's ARC governs the pair's lifetime). A
 *   SLICE shares the root's buffer (dataPtr == root.dataPtr, off absolute
 *   into it), sets parentPtr to the ROOT, and retains it — so a slice that
 *   outlives its parent keeps the bytes alive (the aliasing view, §3.1).
 *   recursiveFree: a slice RELEASES parentPtr (never frees dataPtr); a root
 *   FREES dataPtr, then frees its own 48-byte alloc. meta = 48 (header+body).
 */

typedef struct LvPair { LvValue k; LvValue v; } LvPair;   /* 32 bytes */

/* allocation tier, extracted from X64Gen's g_use_arena-flag scheme (§15
 * escaping-tier ARC) but passed explicitly here rather than through global
 * mutable state, per the pointer-explicit spirit of the boundary rule. */
enum { LV_TIER_HEAP = 0, LV_TIER_ARENA = 1 };

/* ============================ §2.5 Memory API ============================ */

void*   lvrt_halloc(int64_t size, int tier);   /* raw allocator; NO header is
                                                   written — callers that need
                                                   an ARC-prefixed block ask
                                                   for size+16 and write the
                                                   header themselves, mirroring
                                                   X64Gen's alloc()/halloc() */
void    lvrt_retain(const LvValue* v);
void    lvrt_release(const LvValue* v);        /* recursive free at rc 1->0 */
void    lvrt_vfree(const LvValue* v);          /* dead standalone value-struct
                                                   copy (§15 "vfree" helper) */
int64_t lvrt_arena_save(void);                 /* frame prologue watermark */
void    lvrt_arena_restore(int64_t mark);      /* bulk-free back to the mark */
int64_t lvrt_live_bytes(void);                 /* churn-harness meter */
int64_t lvrt_peak_bytes(void);

/* ================== §2.10 Inline hot-state globals (fast path) ===========
 * Generated code MAY load/store these directly instead of calling the
 * function entry points above, once past lv_rt_init (guaranteed post the
 * A-M5 entry flip). lv_g_arena_cursor is exactly what lvrt_arena_save reads
 * and lvrt_arena_restore writes — no poisoning, no bookkeeping. Generated
 * code must never WRITE lv_g_throwing (throw_set/catch_bind own that
 * transition, including the thrown-value ownership it entails); it may only
 * be loaded in place of calling lvrt_throwing(). The function entry points
 * stay in the ABI unchanged — this is a fast path, not a replacement. */
extern LV_TLS uint8_t* lv_g_arena_cursor;

/* ======================= §2.6 Program registration ======================= */

typedef void (*LvDispatchFn)(int64_t fnIndex, LvValue* ret, LvValue* args, int64_t argc);

/* member-kind discriminator for LvClassInfo.methodKinds — X64Gen builds four
 * SEPARATE dispatch switches (genGetm getters, genSetm setters, genOpm
 * operators, genCallM methods); a single undiscriminated name list cannot
 * reproduce that split (a field read must never dispatch a plain method).
 * Ruled into the contract at the 2026-07-05 maintenance pass (Fable) — lands
 * in the same Track A relink as the struct extension below. */
enum { LV_M_METHOD = 0, LV_M_GET = 1, LV_M_SET = 2, LV_M_OP = 3 };

typedef struct LvClassInfo {
    const char*    name;
    int64_t        classId;
    int64_t        nslots;
    int32_t        isValue;         /* struct semantics: copy, never refcount */
    const char**   slotNames;       /* index = slot; NUL-terminated C strings.
                                        Compared pointer-first, then by CONTENT
                                        (strcmp fallback) — see the key-comparison
                                        ruling below */
    const int64_t* subtypeIds;      /* transitive closure incl. self, for issub */
    int64_t        nSubtypeIds;
    const char**   methodNames;     /* compared by content (strcmp) — table-driven
                                        runtime dispatch, not extracted from X64Gen
                                        (which bakes per-program switches); for
                                        LV_M_OP entries the "name" is the operator
                                        symbol text ("+", "==", ...) */
    const int64_t* methodFnIndex;
    const int8_t*  methodKinds;     /* LV_M_* per entry; NULL == all LV_M_METHOD */
    int64_t        nMethods;
} LvClassInfo;

/* LLVM emission shape (Track A): a constant array of
 *   {ptr, i64, i64, i32, ptr, ptr, i64, ptr, ptr, ptr, i64}
 * — the FULL struct, never a prefix. A shorter per-element struct changes the
 * ARRAY STRIDE and the runtime reads garbage past entry 0 (this is not
 * hypothetical: the A-M2 stub-era emission used a 5-field prefix and must be
 * re-emitted at relink; see doc-2 §10, 2026-07-05 maintenance entry). Unused
 * tail columns are null/0.
 *
 * KEY COMPARISON (ruled 2026-07-05): slot names, dynamic-list keys, and
 * capture keys are compared POINTER-FIRST (the interned fast path, free when
 * codegen dedups its key globals) with a CONTENT fallback (strcmp). Codegen
 * SHOULD still intern (one global per distinct key content) for speed, but a
 * missed dedup is now a slow lookup, not a silent field miss. */

void lvrt_register(const LvClassInfo* classes, int64_t nclasses, LvDispatchFn dispatch);

/* bug #35 — the spawn-body global-Promise guard seam (reject route A).
 *
 * A `std::spawn` body may reference a Promise-derived object through a bare
 * GLOBAL rather than a captured local. The capture flatten only walks the body
 * closure's capture list, so a global reference bypasses A-1's cross-thread-
 * Promise reject: on LLVM the worker reads lv_globals in the SHARED address
 * space and silently drains. These two entry points close that hole. Generated
 * code (LlvmGen's `lv_spawn_global_check`, which alone can GEP the module-
 * private lv_globals) calls lvrt_value_has_promise on each global slot a spawn
 * body references, and installs itself via lvrt_register_spawn_check; the spawn
 * path (lv_thread.c) consults it BEFORE flattening, before any worker starts.
 *
 * lvrt_value_has_promise scans a value read-only for a Promise-derived object
 * the A-1 boundary forbids (through object fields / array elements / map
 * entries — Channel portals are relocatable and skipped), returning the
 * byte-identical reject message or NULL. LvSpawnCheckFn maps a closure's IR
 * function index to that message (or NULL). The registration is weak/optional:
 * a runtime with no generated program (runtime_selftest) never registers one,
 * so the spawn path simply no-ops the check. */
typedef const char* (*LvSpawnCheckFn)(int64_t fnIndex);
const char* lvrt_value_has_promise(const LvValue* v);
void        lvrt_register_spawn_check(LvSpawnCheckFn fn);

void    lvrt_obj_new(LvValue* out, int64_t classId);     /* heap tier, rc=0 */
int32_t lvrt_isvalueclass(int64_t classId);
void    lvrt_copyval(LvValue* out, const LvValue* in);
int32_t lvrt_issub(int64_t a, int64_t b);

/* Closures are not named in doc-2 §2.6's dispatch-helper list, but the
 * shape note in §2.4 says they reuse the object mechanism "verbatim" —
 * X64Gen's MakeClosure literally calls its mkobj() helper and re-tags the
 * result as LV_CLO itself (src/X64Gen.cpp:3466-3471). Since lvrt_obj_new
 * always tags its output LV_OBJ, closures need their own thin constructor
 * over the same underlying packed-allocation primitive; these three are
 * Track B's non-normative (but documented, logged) completion of that gap. */
void    lvrt_closure_new(LvValue* out, int64_t fnIndex);
void    lvrt_capture_set(const LvValue* clo, const char* internedKey, const LvValue* val);
void    lvrt_capture_get(LvValue* out, const LvValue* clo, const char* internedKey);

/* Raw (non-accessor) field access — NO ARC performed (matches X64Gen's raw
 * setfield exactly: "raw setfield bypasses the SetMember hook"). Callers
 * that need ownership transfer release the old value / retain the new one
 * explicitly around the call, exactly as X64Gen's genRaise does. */
void    lvrt_getfield(LvValue* out, const LvValue* obj, const char* internedKey);
void    lvrt_setfield(const LvValue* obj, const char* internedKey, const LvValue* val);
void    lvrt_weak_load(LvValue* out, const LvValue* slot);
void    lvrt_weak_store(LvValue* slot, const LvValue* val);
void    lvrt_weak_getfield(LvValue* out, const LvValue* obj, const char* internedKey);
void    lvrt_weak_setfield(const LvValue* obj, const char* internedKey, const LvValue* val);
int64_t lvrt_fieldcount(int64_t classId);
int64_t lvrt_fieldindex(int64_t classId, const char* internedKey);

/* ===== Columnar Array<struct> descriptor contract (techdesign-columnar §4.3/§4.4) =====
 *
 * These three symbols are EMITTED BY GENERATED CODE (LlvmGen — and hand-provided
 * by runtime/selftest.c, which stubs the codegen-emitted tables), and READ BY the
 * runtime archive at columnar flip / column-layout time. They are the columnar
 * analogue of the fieldcount/isvalueclass tables the runtime already reads. Every
 * generated program defines all three in BOTH modes (flag off => lv_cfg_columnar==0
 * and lv_col_eligible returns 0 everywhere) so the link graph is mode-independent.
 *
 *   lv_cfg_columnar   : 1 iff this program was compiled with --columnar. Consulted
 *                       once per flip (lv_arr_go_dense) — a stale runtime archive
 *                       still agrees because the symbol travels with the program.
 *                       A runtime env-var gate is deliberately rejected: the
 *                       lowerer's element-read ownership discipline differs between
 *                       modes, so the mode must be baked at compile time (§4.4).
 *   lv_col_eligible   : per class, 1 iff columnar-eligible per §3 (value struct,
 *                       >=1 field, every field int/float/bool/char, none weak/
 *                       distinct) AND compiled columnar. 0 for any classId in a
 *                       flag-off build. Ineligible classes keep the dense layout.
 *   lv_col_typecode   : per class, per field slot k, the field's scalar LV tag —
 *                       the values EQUAL the LvValue tags (LV_INT=1, LV_FLOAT=2,
 *                       LV_BOOL=3, LV_CHAR=10) so tag synthesis on gather is a copy,
 *                       not a mapping. Only meaningful for eligible classes.
 *
 * v1 column stride is a hardcoded 8 (payload word only); a future lv_col_stride
 * channel is reserved for narrow-int columns (§4.3) — not emitted yet. */
extern const int32_t lv_cfg_columnar;
int32_t lv_col_eligible(int64_t classId);
int32_t lv_col_typecode(int64_t classId, int64_t k);

/* Name-based dynamic dispatch (getter/setter/operator/method), table-driven
 * over LvClassInfo — falls back to raw field access when no declared member
 * of the right LV_M_* kind matches, mirroring genGetm/genSetm's fallback
 * chain. Ownership (ruled 2026-07-05, doc-2 §10):
 *
 *   lvrt_getm   returns +1 (TRANSFER), uniformly: the field-read path retains
 *               before returning; a getter-dispatch path passes its call
 *               result's +1 through. Generated code must NOT add a wrap
 *               retain on getm results (doing so is +2 = leak the moment
 *               getter dispatch exists). Dispatches LV_M_GET entries only.
 *   lvrt_setm   does the value-slot ARC INTERNALLY on the field-write path:
 *               release-old-then-retain-new (§B-H1 order; the IR-level read
 *               protocol protects self-assignment). No slot ARC when an
 *               LV_M_SET setter is dispatched — the setter body owns its own
 *               stores. Matches Track A doc §4.2's "inside the runtime
 *               helper" row.
 *   lvrt_opm    dispatches LV_M_OP entries on the LEFT operand's class;
 *               derives != from == when only == is declared (genOpm parity).
 *               Result is +1 (call transfer).
 *   lvrt_callm  dispatches LV_M_METHOD entries; result +1 (call transfer).
 *               Native fallback (string/array/map cores) wired at B-M3.
 */
void    lvrt_getm(LvValue* out, const LvValue* recv, const char* name);
void    lvrt_setm(const LvValue* recv, const char* name, const LvValue* val);
void    lvrt_opm(LvValue* out, int64_t opcode, const LvValue* l, const LvValue* r);
void    lvrt_callm(LvValue* out, const LvValue* recv, const char* name,
                    LvValue* args, int64_t argc);

/* operator index, matches X64Gen::opCode/symbolOp (src/X64Gen.cpp:20-52):
 * + == != - * / % < > <= >= & | << >> ^ */
enum {
    LV_OP_ADD = 0, LV_OP_EQ = 1, LV_OP_NE = 2, LV_OP_SUB = 3, LV_OP_MUL = 4,
    LV_OP_DIV = 5, LV_OP_MOD = 6, LV_OP_LT = 7, LV_OP_GT = 8, LV_OP_LE = 9,
    LV_OP_GE = 10, LV_OP_AND = 11, LV_OP_OR = 12, LV_OP_SHL = 13, LV_OP_SHR = 14,
    LV_OP_XOR = 15   /* Track 01 F1 (2026-07-06): int ^ int; `~x` lowers as x ^ -1 */
};

/* ================== Scalar core, stringify, minimal IO ====================
 *
 * Ratified 2026-07-05 (Fable maintenance pass): these are the entry points
 * the LLVM backend's scalar core already emits (they predate B-M1 as the old
 * NativeRuntime/stub contract and were missing from this header — a B-M1
 * enumeration gap, closed here; doc-2 §10 has the ruling). Names are frozen
 * to what LlvmGen emits today so the stub->runtime swap is a pure relink.
 *
 * lvrt_arith opcode = the LV_OP_* table above (X64Gen::opCode numbering).
 * String rows: op 0 stringifies both sides and concatenates (CONSUMING
 * unowned rc-0 string operands, the §15 dropped-temporary discipline);
 * ops 1/2 compare by content; ops 7-10 compare lexicographically (bytewise).
 * Numeric rows: if either side is LV_FLOAT the math is double (bit-pattern
 * payloads); % on floats is fmod. Int division/modulo by zero yields 0
 * (stub-era behavior, kept pending an oracle ruling — corpus differential
 * will adjudicate; logged).
 *
 * lvrt_to_string byte-matches valueToString (RuntimeValue.hpp:75-114):
 * ints base-10, bools true/false, floats printf-"%f", strings pass through
 * AS THE SAME VALUE (no copy — X64Gen ts_build returns self), Range objects
 * "start..end", other objects "<object>", closures "<closure>", arrays
 * "[a, b]", maps "{k: v}" in insertion order, None "None", void "".
 * Returns +0 (fresh rc-0) when it builds; the pass-through case returns the
 * input's own payload.
 *
 * lvrt_print_val stringifies to stdout via the raw platform floor (no stdio
 * buffering — B-H4), then CONSUMES the rendered temp AND an unowned rc-0
 * string input (print is a consumer of dropped string temporaries, matching
 * X64Gen's print path + emitStrTempFree).
 * lvrt_syswrite BORROWS its string arg (native contract row: sysWrite
 * borrows), writes to fd 1/2, returns LV_INT byte count in *out.
 * lvrt_readline reads fd 0 to newline/EOF, returns a fresh string +0.
 */
int32_t lvrt_truth(const LvValue* v);                 /* bool payload; non-bool -> 0 */
void    lvrt_not(LvValue* out, const LvValue* a);
void    lvrt_neg(LvValue* out, const LvValue* a);     /* int or float aware */
void    lvrt_arith(int32_t op, LvValue* out, const LvValue* l, const LvValue* r);
void    lvrt_to_string(LvValue* out, const LvValue* v);
void    lvrt_print_val(const LvValue* v);
void    lvrt_print_nl(void);
void    lvrt_syswrite(LvValue* out, const LvValue* fd, const LvValue* s);
void    lvrt_readline(LvValue* out, const LvValue* fd);

/* ================================ Strings ================================= */

void    lvrt_str_new(LvValue* out, const char* bytes, int64_t len);   /* fresh, rc=0 */
void    lvrt_str_concat(LvValue* out, const LvValue* a, const LvValue* b);
int32_t lvrt_str_eq(const LvValue* a, const LvValue* b);
void    lvrt_str_substr(LvValue* out, const LvValue* s, int64_t start, int64_t n);
int64_t lvrt_str_indexof(const LvValue* s, const LvValue* needle);
/* Track 04 M2 (ABI evolution, doc-2 §0.2): toInt/toFloat became optional-
 * returning (strict full-string parse; anything else -> None), so the result
 * needs a TAG, not a bare scalar -- matches the existing lvrt_str_substr/
 * trim/case out-param shape. `int64_t lvrt_str_toint(s)` (no tag) was the
 * pre-Track-04 shape; both call sites (LlvmGen.cpp, lv_runtime.c) moved
 * together in the same change, so no cross-track drift window exists. */
void    lvrt_str_toint(LvValue* out, const LvValue* s);     /* tag INT or NONE */
void    lvrt_str_tofloat(LvValue* out, const LvValue* s);   /* tag FLOAT or NONE */
void    lvrt_str_trim(LvValue* out, const LvValue* s);
void    lvrt_str_case(LvValue* out, const LvValue* s, int32_t toLower);
void    lvrt_int_to_str(LvValue* out, int64_t v);
/* Track 04 M3: byte-level access. OOB/out-of-range RAISES (RuntimeException,
 * matches lvrt_arr's OOB discipline) rather than returning None -- neither
 * function ever returns a NONE tag. `fromByte` has no static-native home
 * (no `static` keyword exists in the language, prelude declares it as the
 * free function std::byteToString instead — designs/techdesign-04-stdlib-
 * strings.md problem #6). */
void    lvrt_str_byteat(LvValue* out, const LvValue* s, int64_t i);
void    lvrt_str_frombyte(LvValue* out, int64_t b);

/* ==================== Track 03 §1 — char (LV_CHAR) ========================
 * char is a pure immediate: LoadConst/charFromCode/code() all inline in
 * LlvmGen (retag payload). Only UTF-8 en/decoding needs the runtime:
 *   lvrt_char_to_string  scalar -> fresh UTF-8 string (+1); char.toString()
 *                         and to_string/print's LV_CHAR row.
 *   lvrt_str_at          string.at(i): decode the scalar STARTING at byte i
 *                         (C1, O(1)); OOB or a mid-sequence byte RAISES.
 *                         Result tag LV_CHAR (immediate, no transfer).
 *   lvrt_str_chars       full UTF-8 decode to a fresh Array<char> (+1);
 *                         invalid bytes -> U+FFFD, never a throw. */
void    lvrt_char_to_string(LvValue* out, int64_t scalar);
void    lvrt_str_at(LvValue* out, const LvValue* s, int64_t i);
void    lvrt_str_chars(LvValue* out, const LvValue* s);

/* ==================== Track 03 §3 — Block (LV_BLOCK) ======================
 * Fixed-length mutable byte buffer, contract C4. Constructors return a fresh
 * root block at +0 (LlvmGen's NewBlock/NewBlockStr wrap retains it, dk==1);
 * `slice` returns a fresh view at +0 that already retains its root. Every
 * accessor is bounds-checked and RAISES a catchable RuntimeException on OOB
 * (lvrt_raise_oob, "index N out of bounds (length M)") or a bad byte value
 * ("byte value V out of range 0..255"); the setters return nothing. int32At
 * is little-endian sign-extended; int64At little-endian. */
void    lvrt_block_new(LvValue* out, int64_t size);
void    lvrt_block_fromstr(LvValue* out, const LvValue* s);   /* borrows s */
void    lvrt_block_byteat(LvValue* out, const LvValue* b, int64_t i);
void    lvrt_block_setbyte(const LvValue* b, int64_t i, int64_t value);
void    lvrt_block_slice(LvValue* out, const LvValue* b, int64_t off, int64_t len);
void    lvrt_block_tostring(LvValue* out, const LvValue* b, int64_t off, int64_t len);
void    lvrt_block_int32at(LvValue* out, const LvValue* b, int64_t i);
void    lvrt_block_setint32(const LvValue* b, int64_t i, int64_t value);
void    lvrt_block_int64at(LvValue* out, const LvValue* b, int64_t i);
void    lvrt_block_setint64(const LvValue* b, int64_t i, int64_t value);
void    lvrt_block_fill(const LvValue* b, int64_t off, int64_t len, int64_t value);
void    lvrt_block_blit(const LvValue* dst, int64_t dstOff, const LvValue* src, int64_t srcOff, int64_t len);
void    lvrt_block_equals(LvValue* out, const LvValue* b, const LvValue* other);
void    lvrt_block_mismatch(LvValue* out, const LvValue* b, const LvValue* other, int64_t from);

/* ============================== Collections ================================ */

void    lvrt_arr_new(LvValue* out, int64_t len);               /* boxed, zeroed */
void    lvrt_arr_append(LvValue* out, const LvValue* arr, const LvValue* val);
/* Array(n, fill): one-shot O(n) sized construction (techdesign-columnar §5.1) —
 * columnar columns / row-major dense records / boxed elements per the fill's
 * type + mode. Returns owned (+1). Supersedes the O(n^2) codegen append loop. */
void    lvrt_arr_fill(LvValue* out, int64_t n, const LvValue* fill);
/* Track 04 M4: Array<T>.concatAll() -- O(total): sum lengths, one alloc,
 * memcpy each part. Declared on Array<T> generically (no specialization
 * exists) but only meaningful, and only ever called, on an Array<string>
 * (StringBuilder's engine); elements are read as strings unconditionally. */
void    lvrt_arr_concatall(LvValue* out, const LvValue* arr);
void    lvrt_map_new(LvValue* out, int64_t len);
void    lvrt_idxget(LvValue* out, const LvValue* base, const LvValue* idx);
void    lvrt_idxset(LvValue* out, const LvValue* base, const LvValue* idx, const LvValue* val);
/* §15 destination ownership: lvrt_idxset with the VALUE operand CONSUMED — the
 * caller hands over a fresh standalone value-struct copy (lowerAssign's
 * definite `CopyVal c=1` bind) that it will never read again. Only the layout
 * knows whether the store copies bytes (dense/columnar memcpy/scatter; map
 * upsert deep-copies) or takes the pointer itself (boxed in-range slot), so
 * the consume decision lives here: any path that copied the record in — or
 * stored nothing at all (OOB / unknown base) — vfrees the dead standalone
 * copy; an in-range boxed store transfers ownership to the slot (the array's
 * free path vfrees elements) and must NOT free it. lvrt_vfree's tag/class/rc
 * guards make the call a no-op whenever the operand turns out not to be an
 * rc-0 heap value struct (arena sentinel, counted refs, primitives), so this
 * degrades exactly to lvrt_idxset in every non-struct case. */
void    lvrt_idxset_move(LvValue* out, const LvValue* base, const LvValue* idx, const LvValue* val);
/* bug.md #30: map.with under the CallDyn (+1 transfer) convention. Wraps
 * lv_map_upsert (which lvrt_idxset also uses under the dk==1/IndexStore
 * convention, unchanged) and retains a fresh copy so the result crosses the
 * boundary OWNED. See lvrt_map_with's definition and the ownership-table rows
 * (map.with/map.without) below. */
void    lvrt_map_with(LvValue* out, const LvValue* base, const LvValue* idx, const LvValue* val);
/* Track 05 C3: Map key equality -- primitives by value; structs field-wise
 * recursive (a struct IS its fields); classes by identity. Shared by
 * lvrt_idxget/lvrt_idxset's map branch and LlvmGen's native "has" row. */
int32_t lvrt_keyeq(const LvValue* a, const LvValue* b);

/* struct-equality design §3/§8: the canonical float relation. Args are the two
 * floats' raw bit payloads (LV_FLOAT stores bits in `payload`, §2.3); returns
 * 1 iff canonically equal. The synthesized struct `(==)` compares float fields
 * through this (via float.canonEq), and lvrt_keyeq's LV_FLOAT leg reuses it —
 * both routing through lv_runtime.c's single canon (hash-consistency law). */
int32_t lvrt_canoneq(int64_t abits, int64_t bbits);

/* ============================ §2.7 Natives ================================
 *
 * Enumerated at B-M1 from `grep -n '"' src/RuntimeNatives.cpp` (free/system
 * natives) and X64Gen::genCallM/genCallNative (src/X64Gen.cpp:1694-1914,
 * the instance-method natives — its comment "mirroring RuntimeNatives" is
 * the tell that these are the SAME logical native set, just reached via a
 * different IR op: CallNativeFn for the free functions below, CallDyn/CallM
 * native-fallback for the instance methods). Reconcile against Track A's
 * A-M2 step-0 enumeration (docs/techdesign-portable-backend.md §5.1) before
 * B-M3 implements these — this table is a SEED, not yet wired to code.
 *
 * native                          | contract
 * ---------------------------------|--------------------------------------
 * string.length, .isEmpty          | borrows; returns unboxed
 * string.indexOf, .contains        | borrows args; returns unboxed
 * string.toString                  | returns the RECEIVER retained (+1) —
 *                                   | not a fresh value, but still a transfer
 * string.subStr, .trim, .charAt,   | borrows args; returns a FRESH string
 *   .toUpper, .toLower             | at +1 (transfer)
 * string.startsWith, .endsWith     | borrows args; returns unboxed bool
 *                                   | (builds + frees an internal probe)
 * array.length                     | borrows; returns unboxed (masks the
 *                                   | dense marker bit first)
 * array.at                         | borrows base+idx; returns +1 (transfer)
 *                                   | of the borrowed element ref; raises
 *                                   | RuntimeException on OOB
 * array.add                        | COW: mutates base in place when
 *                                   | uniquely owned (rc==1), else copies;
 *                                   | returns the (possibly new) array +1
 * map.length, .has                 | borrows; returns unboxed
 * map.at                           | borrows base+key; returns +1 (transfer)
 *                                   | of the borrowed value, void if absent
 * map.keys, .values                | borrows; returns a FRESH array at +1,
 *                                   | retaining every copied element
 * map.with (lvrt_map_with)         | in-place (COW, uniquely-owned receiver):
 *                                   | returns the receiver, whose consumed +1
 *                                   | rides through; else returns a FRESH map
 *                                   | at +1 (entries retained). Never takes the
 *                                   | base's fate — the with row releases the
 *                                   | consumed receiver ref itself (in.b-gated).
 * map.without (LlvmGen inline row) | borrows args; returns a FRESH map at +1,
 *                                   | entries retained; the row releases the
 *                                   | consumed receiver ref itself (in.b-gated).
 * int.toString                     | returns a FRESH string at +1
 * bool.toString                    | returns a STATIC LITERAL ("true"/
 *                                   | "false") — no retain (literals are
 *                                   | ARC-exempt; a retain would be a
 *                                   | harmless no-op via the region check)
 * sysWrite/sysReadLine/sysRead/    | borrow args; sysReadLine/sysRecv
 *   sysOpen/sysClose/sysStat       | return a fresh string +1; recv/read
 *                                   | returning empty at EOF maps to None
 * sockets (sysTcp*, sysSend/Recv,  | borrow args (seed row from doc-2 §2.7;
 *   sysWatch/sysUnwatch)           | wired at B-M3)
 * timers (sysTimerStart/Cancel)    | the registry OWNS the callback closure
 *                                   | (+1 on add, released on cancel/last
 *                                   | fire) — seed row, wired at B-M3
 */

/* ===================== §2.7 natives, B-M3 wiring (this milestone) ==========
 *
 * Concrete signatures for the sys-, timer-, and watch-native seed row
 * above — it was a table, not code, until now. All args cross as LvValue*
 * per the boundary rule (§2.1/H-1), matching the already-ratified
 * lvrt_syswrite/lvrt_readline style, even for scalars (fd, port, ids) that
 * a caller might expect as raw int64_t: they are language-level values a
 * program computes and stores, not compiler-burned-in constants (contrast
 * lvrt_obj_new's classId, which IS compiler metadata and stays a raw
 * int64_t). Behavior extracted from src/RuntimeNatives.cpp (the oracle) and
 * X64Gen's genTcp-connect/listen, genRecv, genWatchAdd/Cancel, and
 * genTimerAdd/Cancel (ground truth for ownership) — never invented; ties
 * to the corresponding lv_plat_* floor call one-for-one.
 *
 * lvrt_sysopen: flagBits 1=read 2=write 4=append (RuntimeNatives.cpp parity).
 * lvrt_sysstat: field 0=exists(0/1) 1=size 2=mtime; -1 for a missing path's
 *   size/mtime (byte-parity with RuntimeNatives.cpp's sysStat).
 * lvrt_sysread: fresh string of the bytes actually read (short reads
 *   included), "" if max<=0 or the read failed.
 * lvrt_systcpconnect/listen: -1 on any failure (socket/bind/listen/connect),
 *   no fd leaked. listen binds 127.0.0.1 with backlog 16 (oracle parity).
 * lvrt_sysaccept: -1 if none pending; the accepted fd is pre-set nonblocking.
 * lvrt_syssend: byte count written; -1 retryable (would-block, nothing
 *   written), -2 fatal (peer gone) — Track 08 F5.6, RuntimeNatives parity;
 *   pipes take the same call (ENOTSOCK falls back to write(2) in the floor).
 * lvrt_sysrecv: three-way narrowing (oracle's sysRecv comment) — tag LV_NONE
 *   on orderly close (peer EOF), a fresh "" on would-block (EAGAIN), else a
 *   fresh string of the received bytes.
 * lvrt_systimerstart: delayMs/intervalMs (0 interval = one-shot); registers
 *   `cb` retained (+1, registry-owned) and returns its timer id. Fires with
 *   the 1-based tick count as the callback's single argument (RuntimeLoop
 *   parity), re-arming intervals, releasing one-shots on completion.
 * lvrt_systimercancel: no-op on an unknown/already-fired id; releases the
 *   registry's callback ref if the id was still active.
 * lvrt_syswatch: registers a read-readiness watch on `fd`, `cb` retained
 *   (+1); fires with `fd` as the callback's single argument whenever
 *   POLLIN/HUP/ERR is observed (mirrors src/RuntimeLoop.cpp's readiness
 *   check — a half-closed peer must still wake it).
 * lvrt_sysunwatch: releases the registry's ref; no-op on an unknown id.
 * lvrt_syssocketbuffer: advisory SO_SNDBUF/SO_RCVBUF sizing (RuntimeNatives.cpp
 *   parity). A non-positive direction is left untouched; returns 0 if every
 *   requested direction applied, -1 if a requested setsockopt failed.
 *
 * lvrt_loop_has_work / lvrt_loop_step are not natives (no `sys` name calls
 * them) — they are what Await's codegen pumps directly (mirrors X64Gen's
 * inline has_work()/loop_step() calls, src/X64Gen.cpp:3573-3593): step()
 * blocks up to the earliest due timer (or indefinitely if only watches are
 * pending), runs one poll pass, and fires every batch of now-ready
 * watches/due timers before returning.
 */
void    lvrt_sysopen(LvValue* out, const LvValue* path, const LvValue* flagBits);
void    lvrt_sysclose(const LvValue* fd);
void    lvrt_sysstat(LvValue* out, const LvValue* path, const LvValue* field);
void    lvrt_sysmkdir(LvValue* out, const LvValue* path);
void    lvrt_sysremove(LvValue* out, const LvValue* path);
void    lvrt_sysrename(LvValue* out, const LvValue* from, const LvValue* to);
/* Fresh boxed Array<string> at rc 0, or LV_NONE. Each string is retained once
 * by the array; compiled callers retain the returned array for their dest. */
void    lvrt_syslistdir(LvValue* out, const LvValue* path);
void    lvrt_sysread(LvValue* out, const LvValue* fd, const LvValue* max);
/* Track 03 M4 — zero-copy Block I/O overloads (§6.6.5). Borrow the Block; read/
 * recv fill from offset 0, write/send take a [off,off+len) window; bounds
 * raised loud. Arity-distinct from the string forms, so codegen selects by
 * argument count. Defined in lv_runtime.c (lv_block_at is file-local there). */
void    lvrt_sysread_block(LvValue* out, const LvValue* fd, const LvValue* b, const LvValue* max);
void    lvrt_syswrite_block(LvValue* out, const LvValue* fd, const LvValue* b, const LvValue* off, const LvValue* len);
void    lvrt_syssend_block(LvValue* out, const LvValue* fd, const LvValue* b, const LvValue* off, const LvValue* len);
void    lvrt_sysrecv_block(LvValue* out, const LvValue* fd, const LvValue* b, const LvValue* max);
/* lvrt_sysargs: the process argument vector as a fresh Array<string>, argv[0]
 * first; length >= 1 always (designs/argv.md §4). Captured in lv_rt_init. */
void    lvrt_sysargs(LvValue* out);
/* terminal raw mode (designs/terminal-raw-mode.md): scalar-in, int-out (0/-1).
 * lvrt_term_shutdown restores the terminal iff raw — the guaranteed restore-on-
 * exit, called from lv_entry's epilogue, lvrt_uncaught, and lv_die (§3.4). */
void    lvrt_systermraw(LvValue* out, const LvValue* fd);
void    lvrt_systermrestore(LvValue* out, const LvValue* fd);
void    lvrt_term_shutdown(void);

void    lvrt_systcpconnect(LvValue* out, const LvValue* host, const LvValue* port);
void    lvrt_systcplisten(LvValue* out, const LvValue* port);
void    lvrt_systcplisten_reuse(LvValue* out, const LvValue* port, const LvValue* reusePort);
void    lvrt_syscpucount(LvValue* out);
/* lvrt_syssocketbuffer: advisory SO_SNDBUF/SO_RCVBUF sizing (RuntimeNatives.cpp
 * parity). A non-positive direction is left untouched; returns 0 if every
 * requested direction applied, -1 if a requested setsockopt failed. */
void    lvrt_syssocketbuffer(LvValue* out, const LvValue* fd,
                             const LvValue* sendBytes, const LvValue* recvBytes);
/* Track 10 flatten/rebuild across a thread boundary. The interpreters implement
 * the deep copy (RuntimeNatives.cpp); the LLVM leg — the C flatten/rebuild
 * engine + true pthreads — is M3. Until then a spawn/Channel program built with
 * the LLVM backend aborts LOUDLY here if it actually reaches a transfer (never a
 * silent wrong-copy); the symbol exists so unrelated programs that merely lower
 * an unused Channel/Worker method still link and run. */
void    lvrt_systhreadtransfer(LvValue* out, const LvValue* v);
/* Track 10 M3c — the true-thread seam (techdesign-threads-3 §5; lv_thread.c,
 * POSIX-only). start flattens the body's captures, creates the join eventfd,
 * pthread_creates the worker, and returns the eventfd as an int (the capability
 * probe: >= 0 = true threads exist, so the prelude spawn watches it; -1 on the
 * interpreters). result (spawner thread) drains the eventfd, rebuilds the
 * worker's result into this heap at +1, reaps the thread, and rethrows the
 * carried message if the worker failed. */
void    lvrt_systhreadstart(LvValue* out, const LvValue* body);
void    lvrt_systhreadresult(LvValue* out, const LvValue* joinFd);
/* Track 10 M3d — Channel<T> on the LLVM leg (techdesign-threads-3 §6; lv_thread.c).
 * new returns a channel id (>= 0; the interpreters return -1 and run the in-process
 * queue). send flattens+enqueues (block/drop/error on full); receive dequeues+
 * rebuilds into this heap (+1) or returns None on closed+drained (F-8); close marks
 * the channel closed and wakes a parked consumer. */
void    lvrt_syschannelnew(LvValue* out, const LvValue* capacity, const LvValue* policy);
void    lvrt_syschannelsend(LvValue* out, const LvValue* id, const LvValue* value);
void    lvrt_syschannelreceive(LvValue* out, const LvValue* id);
void    lvrt_syschannelclose(LvValue* out, const LvValue* id);
/* §4.2: env-gated (LANG_THREAD_STATS) [threads] spawns/reaps/transfer-outstanding
 * line, emitted by lv_main at exit right after the [heap] meter (M6 parses it). */
void    lvrt_thread_stats_report(void);
void    lvrt_sysaccept(LvValue* out, const LvValue* fd);
void    lvrt_syssend(LvValue* out, const LvValue* fd, const LvValue* s);
void    lvrt_sysrecv(LvValue* out, const LvValue* fd, const LvValue* max);
/* Track 08 F5: the connect-timeout floor. connectnb returns the fd even
 * mid-handshake (-1 only on immediate failure); connectresult is the
 * SO_ERROR verdict (0 = connected). Completion rides lvrt_syswatchwrite. */
void    lvrt_systcpconnectnb(LvValue* out, const LvValue* host, const LvValue* port);
void    lvrt_sysconnectresult(LvValue* out, const LvValue* fd);

/* LA-2 (designs/complete/techdesign-tls-crypto.md §5.2) — the ENUMERATED TLS +
 * crypto contract additions (anything beyond this list is a STOP event). Thin
 * shims over the lv_tls_* / lv_rsa_encrypt provider seam (lv_tls.h) + lv_plat_
 * random. The fd IS the socket fd (wrap-in-place); send/recv/close route through
 * the session inside their existing natives (lv_loop.c / lv_runtime.c) — no new
 * calling convention. The three string-returning natives yield fresh strings
 * (+1; LlvmGen retainDst, like sysRecv's string path); sysrsaencrypt returns a
 * fresh string or LV_NONE; sysrandom a fresh string. */
void    lvrt_systlsconnect(LvValue* out, const LvValue* fd, const LvValue* host,
                           const LvValue* alpn, const LvValue* caFile, const LvValue* verify);
void    lvrt_systlsaccept (LvValue* out, const LvValue* fd, const LvValue* cert,
                           const LvValue* key, const LvValue* alpn);
void    lvrt_systlshandshake(LvValue* out, const LvValue* fd);
void    lvrt_systlserror  (LvValue* out, const LvValue* fd);   /* fresh string +1        */
void    lvrt_systlsalpn   (LvValue* out, const LvValue* fd);   /* fresh string +1        */
void    lvrt_systlsversion(LvValue* out, const LvValue* fd);
void    lvrt_sysrsaencrypt(LvValue* out, const LvValue* pem, const LvValue* bytes,
                           const LvValue* pad);                /* string +1 | LV_NONE    */
void    lvrt_sysrandom    (LvValue* out, const LvValue* n);    /* fresh string +1 (§8)   */
void    lvrt_sysenv       (LvValue* out, const LvValue* key);  /* env var: string +1 | LV_NONE (bug #68) */

/* Process floor (techdesign-spawn-llvm.md; oracle RuntimeNatives.cpp:1841+).
 * lvrt_sysspawn: fresh Array<int> [pid, stdinFd, stdoutFd, stderrFd], or the
 *   empty array on SPAWN failure (pipe/fork; empty path). Exec failure is the
 *   child's _exit(127), collected via lvrt_sysreap. Returned array follows the
 *   lvrt_sysargs ownership convention (rc 0; codegen retains).
 * lvrt_syspidfdopen / lvrt_sysreap / lvrt_syskill: scalar LV_INT results,
 *   values per the frozen F7 contract (Resolver.cpp:1479-1482).              */
void    lvrt_sysspawn(LvValue* out, const LvValue* path, const LvValue* args);
void    lvrt_syspidfdopen(LvValue* out, const LvValue* pid);
void    lvrt_sysreap(LvValue* out, const LvValue* pid);
void    lvrt_syskill(LvValue* out, const LvValue* pid, const LvValue* sig);

/* Pty floor (designs/pty/ 02; oracle RuntimeNatives.cpp sysPtySpawn).
 * lvrt_sysptyspawn: fresh Array<int> [pid, masterFd], or the empty array on
 *   failure (bad args / allocation / fork). Exec failure is the child's
 *   _exit(127) via lvrt_sysreap. Return follows the lvrt_sysargs ownership
 *   convention (rc 0; codegen retains) — the lvrt_sysspawn parity.
 * lvrt_sysptyresize: scalar LV_INT 0/-1.                                     */
void    lvrt_sysptyspawn(LvValue* out, const LvValue* path, const LvValue* args,
                         const LvValue* rows, const LvValue* cols, const LvValue* flags);
void    lvrt_sysptyresize(LvValue* out, const LvValue* master,
                          const LvValue* rows, const LvValue* cols);

void    lvrt_systimerstart(LvValue* out, const LvValue* delayMs,
                            const LvValue* intervalMs, const LvValue* cb);
void    lvrt_systimercancel(const LvValue* id);
void    lvrt_syswatch(LvValue* out, const LvValue* fd, const LvValue* cb);
/* Track 08 F5: write-readiness watch — same registry, id space, and cancel
 * (lvrt_sysunwatch) as read watches; fires on LV_POLLOUT (or ERR/HUP: a
 * refused async connect must wake it so sysConnectResult can rule). */
void    lvrt_syswatchwrite(LvValue* out, const LvValue* fd, const LvValue* cb);
void    lvrt_sysunwatch(const LvValue* id);

int32_t lvrt_loop_has_work(void);
void    lvrt_loop_step(void);

/* LA-30 (designs/suspension/techdesign-02-llvm-leg.md §2). Generated code calls
 * ONLY lvrt_await for Op::Await; the rest (park/pump, promise-state poll,
 * task-spawn-on-dispatch) is runtime-internal (lv_runtime.c / lv_loop.c /
 * lv_task.h), not declared here. Declaration only — implementation is a
 * SENSITIVE-GATE (S2) section awaiting model escalation; not yet defined. */
void    lvrt_await(LvValue* dst, const LvValue* promise);

/* Track W hard-03 (doc 02 §5, tier 2): the wasm capability-gate trap stub.
 * Prints "<what>: not on the wasm-browser target" to fd 2 and exits 134.
 * Present on every target (one archive source); only wasm codegen emits
 * calls to it — for gated natives in prelude bodies that are emitted but
 * not reachable from user code (reachable ones are a compile-time
 * diagnostic instead). Never returns. */
void    lvrt_unsupported(const char* what);

/* Track W hard-05 (designs/wasm-frontend/hard-05-callclosure-seam.md): the
 * C-callable closure-invocation seam. Invokes an LV_CLO value (§2.4 closure
 * layout above: body word0 = fnIndex, word1 = captureHead) exactly the way
 * generated CallValue code does — through the registered dispatch
 * trampoline, with the closure passed as the callee's own args[0] so the
 * body reads captures via lvrt_capture_get. A PARALLEL C entry, not a
 * replacement: generated code keeps its own CallValue emission; only the
 * wasm glue calls this in v1 (lv_loop.c/lv_thread.c keep their private
 * inline copies of the same pattern). `args`/`nargs` are the real
 * arguments only (the closure is added internally; args may be NULL when
 * nargs is 0); inputs are borrowed. *out receives the callee's +1 result —
 * the caller releases it. A throw inside the closure surfaces as the
 * pending-throw flag (lvrt_throwing), checked by the caller; nothing
 * unwinds across this boundary. *out is VOID (and nothing runs) when the
 * value is not a closure or no dispatch is registered. */
void    lvrt_callclosure(LvValue* out, const LvValue* clo,
                         const LvValue* args, int nargs);

/* Track W hard-06 (designs/wasm-frontend/hard-06-hostbridge-seam.md): the
 * JS/DOM host-bridge seam. THREE emitted-against entry points the hand-written
 * `Dom` prelude surface (doc 05 §2) funnels every DOM/host operation through —
 * one reflective JS marshaler (doc 05 §3) behind them, never one wasm import
 * per method. Built for ALL targets (one archive source): on wasm they cross to
 * the `lv.dom_call` host import (lv_bridge_wasm.c); on every native target they
 * RAISE ("host bridge: available on the wasm-browser target only") — a
 * wasm-*gained* capability, so unlike the hard-03 gate there is no compile-time
 * diagnostic, just a loud runtime error if native code reaches one (the
 * existing corpus never does, so the four-lane differential is unaffected).
 * All args cross as LvValue* per the boundary rule (§2.1). Results that can be
 * a fresh counted value are retained +1 into the register by generated code:
 * lvrt_hostcall (a string/handle result transfers +1; retain is a no-op on its
 * int/void results via the immediate gate) and lvrt_hostecho (a fresh string).
 * lvrt_host_clo_reg's result is ALWAYS an int index, so it takes no retain.
 *
 * lvrt_hostcall: the generic sync host call. `op` (string) selects the
 *   operation; `h0` is the primary node handle (INT; 0 = document/none), `h1`
 *   a secondary handle (INT; child for appendChild, closure-table index for
 *   add/removeEventListener), `a`/`b` string args ("" when unused). Result tag
 *   depends on `op` — handle (INT), string (LV_STR), or VOID. Arity 5 covers
 *   every DOM v1 op with a fixed signature.
 * lvrt_host_clo_reg: the one closure-taking entry. Retains `cb` (an LV_CLO,
 *   `(int) => void`) into the bridge's closure-table ROOT (doc 05 §5) and
 *   returns its index (INT). addEventListener then rides lvrt_hostcall with
 *   h1 = that index; removeEventListener rides it and RELEASES index h1;
 *   "cloCount" rides it and returns the live root count (the §5 leak pin).
 * lvrt_hostecho: the reflective round-trip probe (doc 05 §8). Marshals its one
 *   LvValue arg to JS through the §3 marshaler and returns a host-side string
 *   rendering — exercises every tag path. Not a DOM op; pins only. */
void    lvrt_hostcall(LvValue* out, const LvValue* op,
                      const LvValue* h0, const LvValue* h1,
                      const LvValue* a,  const LvValue* b);
void    lvrt_host_clo_reg(LvValue* out, const LvValue* cb);
void    lvrt_hostecho(LvValue* out, const LvValue* v);

/* Track W hard-06 companion (NOT emitted-against — called by the JS marshaler
 * through the wasm EXPORT table, never by generated code; same host-facing
 * accessor discipline as lv_park_poll). Returns the interned slot-name pointer
 * for field `i` of `classId` (NUL-terminated C string in linear memory), or
 * NULL when out of range. The JS LV_OBJ marshaler pairs it with the existing
 * lvrt_fieldcount to walk an object's slots (doc 05 §3's "read slotNames
 * through linear memory", realized as a typed accessor rather than JS parsing
 * the LvClassInfo struct layout). */
const char* lvrt_class_field_name(int64_t classId, int64_t i);

/* Track W hard-06 companion (NOT emitted-against — host-accessor discipline as
 * above). Returns the interned class-name pointer for `classId` (NUL-terminated
 * C string in linear memory), or NULL when unknown. The JS marshaler reads it
 * to identify the handle-wrapper classes (DomNode/DomEvent) by name rather than
 * by an ambiguous slot-shape heuristic (doc 05 §3). */
const char* lvrt_class_name(int64_t classId);

/* LA-30 B2 (doc 06 §4) — the sysTask* floor (lv_loop.c). Ids, not handles,
 * cross this boundary; every result is a scalar int. run/joinAll/awaitAny2
 * raise under LANG_PUMP=1 (B2 requires the scheduler); joinAll and awaitAny2
 * PARK the calling task and are rethrow points (CancelledException on a
 * delivered cancel, the C3 RuntimeException on a drained loop). */
void    lvrt_systaskrun(LvValue* out, const LvValue* clo);      /* -> id       */
void    lvrt_systaskcancel(LvValue* out, const LvValue* id);    /* -> 1|0      */
void    lvrt_systaskshield(LvValue* out, const LvValue* delta); /* -> 0        */
void    lvrt_systaskjoinall(LvValue* out, const LvValue* ids);  /* -> count    */
void    lvrt_sysawaitany2(LvValue* out, const LvValue* a, const LvValue* b);   /* -> 0|1 */

/* ============================ §2.8 Exception state ========================= */

void    lvrt_throw_set(const LvValue* v);   /* retains v (in-flight ownership) */
int32_t lvrt_throwing(void);
extern LV_TLS int32_t lv_g_throwing;  /* §2.10 — load-only fast path for
                                                 lvrt_throwing(); see §2.5 for
                                                 lv_g_arena_cursor's counterpart */
void    lvrt_thrown(LvValue* out);          /* peek, no transfer */
void    lvrt_catch_bind(LvValue* bindSlot); /* clear flag; release previous
                                                binding; transfer thrown value */
void    lvrt_uncaught(void);                /* report byte-matching the oracle
                                                (src/Eval.cpp:1092); does NOT
                                                exit — X64Gen's genUncaught
                                                doesn't either, and the process
                                                always exits 0 regardless of an
                                                uncaught throw (verified against
                                                X64Gen's unconditional exit(0)
                                                and Eval::run's non-distinguishing
                                                return) */
void    lvrt_raise(const char* msg);                  /* fresh RuntimeException */
void    lvrt_raise_cls(const char* clsName, const char* msg);   /* B2: named prelude
                                                         exception class (CancelledException);
                                                         falls back to RuntimeException */
void    lvrt_raise_str(const LvValue* s);             /* RuntimeException from an lv string
                                                         (Track 10 §3.3 await rethrow) */
void    lvrt_raise_oob(int64_t idx, int64_t len);     /* index OOB message */

/* Canonical interned pointer for the built-in RuntimeException "message"
 * field. HISTORY: under B-M1's original pointer-identity-only key rule this
 * was mandatory; the 2026-07-05 content-fallback ruling (see LvClassInfo)
 * demoted it to an optional fast-path — any "message" literal now works.
 * Kept exported (harmless, still the fastest path, and existing users
 * reference it). */
extern const char* const lv_key_message;

/* ========================= Process entry (§2.9) ============================
 * Wired at B-M2 (archive/link driver) and A-M5 (main() ownership flip). */

void lv_rt_init(int argc, char** argv);
/* Process exit code (designs/exit-codes.md §3.1). lv_entry.c returns
 * lv_rt_exit_code() at process end; lv_rt_set_exit_code masks to Unix 8-bit.
 * lvrt_uncaught sets it to 1 (gap b). */
int  lv_rt_exit_code(void);
void lv_rt_set_exit_code(int code);
/* env.setExitCode / env.exit natives (§3.1). Args cross as LvValue* (boundary
 * rule); the int rides payload. lvrt_sysexit routes through the terminal-restore
 * epilogue then lv_plat_exit — it never returns. */
void lvrt_syssetexitcode(const LvValue* code);
void lvrt_sysexit(const LvValue* code);

#ifdef __cplusplus
}
#endif

#endif /* LV_ABI_H */
