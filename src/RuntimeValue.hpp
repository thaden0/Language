#pragma once
#include "Symbols.hpp"
#include "Token.hpp"
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
//  The shared runtime value model.
//
//  Both execution engines — the tree-walking evaluator (the semantics oracle)
//  and the bytecode IR interpreter — operate on THIS value model, so their
//  observable behavior (arithmetic, stringification) is identical by
//  construction. That equivalence is what the differential corpus tests check.
// ---------------------------------------------------------------------------

struct Object;
struct Closure;
struct AstPayload;

// Track 03 §3: `Block` — a fixed-length mutable byte buffer (contract C4). A
// slice is an ALIASING view: it shares the same `bytes` with a different
// off/len, so a write through any view is visible to the parent (zero-copy,
// documented). Reference semantics (like Array/Map): assignment shares the
// BlockData; there is NO copy-on-write (mutation is honest, §3.1).
struct BlockData {
    std::shared_ptr<std::vector<uint8_t>> bytes;   // the backing store (root-owned)
    size_t off = 0, len = 0;                        // this view's window into `bytes`
};

// Track 03 §1: `char` is a Unicode-scalar value primitive. Unboxed, no heap, no
// ARC — a pure immediate like Int/Bool, with the scalar carried in the `i` field.
enum class VKind { Void, Int, Float, Bool, String, Object, Closure, Array, Map, None, Char, Block, Ast };

// Objects/arrays/maps are references (shared_ptr): assignment/passing shares
// them. Primitives are values. (Array/Map *methods* are pure — §11.)
struct Value {
    VKind kind = VKind::Void;
    long long i = 0;
    double f = 0;
    bool b = false;
    std::string s;
    std::shared_ptr<Object> obj;
    std::shared_ptr<Closure> closure;
    std::shared_ptr<std::vector<Value>> arr;
    // Map entries in insertion order (linear lookup — fine for the oracle).
    std::shared_ptr<std::vector<std::pair<Value, Value>>> map;
    std::shared_ptr<BlockData> block;   // Track 03 §3: byte buffer (VKind::Block)
    std::shared_ptr<AstPayload> ast;    // F4: opaque comptime-only code carrier
};

struct Object {
    Symbol* cls = nullptr;
    std::unordered_map<std::string, Value> fields;   // key: "name" or "Source::name"
    std::unordered_map<std::string, std::weak_ptr<Object>> weakFields;
};

inline Value weakObjectRead(const std::shared_ptr<Object>& obj, const std::string& key) {
    Value none; none.kind = VKind::None;
    if (!obj) return none;
    auto it = obj->weakFields.find(key);
    if (it == obj->weakFields.end()) return none;
    auto target = it->second.lock();
    if (!target) return none;
    Value v; v.kind = VKind::Object; v.obj = std::move(target); return v;
}

inline void weakObjectWrite(const std::shared_ptr<Object>& obj, const std::string& key,
                            const Value& value) {
    if (!obj) return;
    if (value.kind == VKind::Object && value.obj) obj->weakFields[key] = value.obj;
    else obj->weakFields.erase(key);
}

// A lambda plus the environment it closed over (captured by snapshot).
struct Closure {
    const Expr* lambda = nullptr;
    std::vector<std::unordered_map<std::string_view, Value>> env;
    std::shared_ptr<Object> thisObj;
    Symbol* thisClass = nullptr;
};

// --- value constructors ------------------------------------------------------

inline Value vint(long long x)   { Value v; v.kind = VKind::Int; v.i = x; return v; }
inline Value vchar(long long x)  { Value v; v.kind = VKind::Char; v.i = x; return v; }
inline Value vblock(std::shared_ptr<BlockData> bd) {
    Value v; v.kind = VKind::Block; v.block = std::move(bd); return v;
}
// A fresh, zeroed root block of length n.
inline Value vblockNew(long long n) {
    auto bd = std::make_shared<BlockData>();
    bd->bytes = std::make_shared<std::vector<uint8_t>>((size_t)(n < 0 ? 0 : n), 0);
    bd->off = 0; bd->len = (size_t)(n < 0 ? 0 : n);
    return vblock(bd);
}
inline Value vbool(bool x)       { Value v; v.kind = VKind::Bool; v.b = x; return v; }
inline Value vfloat(double x)    { Value v; v.kind = VKind::Float; v.f = x; return v; }
inline Value vstr(std::string x) { Value v; v.kind = VKind::String; v.s = std::move(x); return v; }
inline Value vvoid()             { return Value{}; }
inline Value vnone()             { Value v; v.kind = VKind::None; return v; }
inline Value vast(std::shared_ptr<AstPayload> x) {
    Value v; v.kind = VKind::Ast; v.ast = std::move(x); return v;
}
inline Value varr(std::vector<Value> xs) {
    Value v; v.kind = VKind::Array;
    v.arr = std::make_shared<std::vector<Value>>(std::move(xs));
    return v;
}

// Value-type (`struct`) copy: reference objects alias, value structs are copied
// at every binding (§9). Deep — a struct field that is itself a struct copies
// too; a reference-class field is shared (only the value wrapper is fresh).
inline Value copyValue(const Value& v) {
    if (v.kind != VKind::Object || !v.obj || !v.obj->cls || !v.obj->cls->isValue)
        return v;
    auto o = std::make_shared<Object>();
    o->cls = v.obj->cls;
    for (const auto& [k, fv] : v.obj->fields) o->fields[k] = copyValue(fv);
    Value r = v; r.obj = o; return r;
}

// --- UTF-8 (Track 03 §1: char <-> bytes; shared by every engine) -------------

// Encode one Unicode scalar to its UTF-8 bytes (assumes a valid scalar; the
// char primitive only ever holds 0..0x10FFFF minus surrogates).
inline std::string utf8Encode(long long cp) {
    std::string out;
    unsigned long c = (unsigned long)cp;
    if (c <= 0x7F) out.push_back((char)c);
    else if (c <= 0x7FF) {
        out.push_back((char)(0xC0 | (c >> 6)));
        out.push_back((char)(0x80 | (c & 0x3F)));
    } else if (c <= 0xFFFF) {
        out.push_back((char)(0xE0 | (c >> 12)));
        out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (c & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (c >> 18)));
        out.push_back((char)(0x80 | ((c >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (c & 0x3F)));
    }
    return out;
}

// Decode the scalar starting at byte offset `i` of `s`. On success returns the
// scalar and sets `len` to its byte length. On an invalid/truncated sequence
// returns U+FFFD with len=1 (replacement policy — data is never a crash, Track
// 03 §5 problem #2). `boundary` is set false when byte `i` is a continuation
// byte (mid-sequence): `string.at` uses it to throw, `chars()` never does.
inline long long utf8DecodeAt(const std::string& s, size_t i, size_t& len, bool& boundary) {
    len = 1; boundary = true;
    if (i >= s.size()) return 0xFFFD;
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) return c;
    if ((c & 0xC0) == 0x80) { boundary = false; return 0xFFFD; }   // continuation byte
    int need; long long cp;
    if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; }
    else return 0xFFFD;                                            // invalid lead byte
    for (int k = 1; k <= need; ++k) {
        if (i + k >= s.size() || ((unsigned char)s[i + k] & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
    }
    len = need + 1;
    return cp;
}

// --- stringification (console output; must match across engines) -------------

inline std::string valueToString(const Value& v) {
    switch (v.kind) {
        case VKind::Int:    return std::to_string(v.i);
        case VKind::Char:   return utf8Encode(v.i);
        case VKind::Bool:   return v.b ? "true" : "false";
        case VKind::Float:  return std::to_string(v.f);
        case VKind::String: return v.s;
        case VKind::Object:
            if (v.obj && v.obj->cls && v.obj->cls->name == "Range") {   // 1..5
                auto s = v.obj->fields.find("start"), e = v.obj->fields.find("end");
                if (s != v.obj->fields.end() && e != v.obj->fields.end())
                    return std::to_string(s->second.i) + ".." + std::to_string(e->second.i);
            }
            return "<object>";
        case VKind::Closure: return "<closure>";
        case VKind::Array: {
            std::string out = "[";
            if (v.arr)
                for (size_t i = 0; i < v.arr->size(); ++i) {
                    if (i) out += ", ";
                    out += valueToString((*v.arr)[i]);
                }
            return out + "]";
        }
        case VKind::Map: {
            std::string out = "{";
            if (v.map) {
                bool first = true;
                for (const auto& [k, val] : *v.map) {
                    if (!first) out += ", ";
                    first = false;
                    out += valueToString(k) + ": " + valueToString(val);
                }
            }
            return out + "}";
        }
        case VKind::None:   return "None";
        case VKind::Void:   return "";
        case VKind::Block:  return "Block(len=" + std::to_string(v.block ? v.block->len : 0) + ")";
        case VKind::Ast:    return "Ast(" + (v.s.empty() ? std::string("empty") : v.s) + ")";
    }
    return "";
}

// Split a union canonical ("string | None", bracket-aware) into member spellings.
inline std::vector<std::string> unionMembersOf(const std::string& canonical) {
    std::vector<std::string> out;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < canonical.size(); ++i) {
        char c = canonical[i];
        if (c == '<' || c == '(') ++depth;
        else if (c == '>' || c == ')') --depth;
        else if (depth == 0 && c == '|' && i > 0 && canonical[i - 1] == ' ' &&
                 i + 1 < canonical.size() && canonical[i + 1] == ' ') {
            out.push_back(canonical.substr(start, i - 1 - start));
            start = i + 2;
        }
    }
    out.push_back(canonical.substr(start));
    return out;
}

// --- the canonical float relation (struct-equality design §3) ----------------
//
// THE one canon helper for the oracle + IR interp (they share this header).
// Hash-consistency law (§3.3): there is exactly one canon symbol per engine and
// every canonical float relation — keyEquals' float leg below, the float
// `canonEq` native, and any future canon_hash — MUST normalize through THIS
// function. Integer/branchless form only (§3.2): never an FPU compare, so it is
// immune to -ffast-math folding by construction. NaN (any sign/payload) ->
// 0x7FF8000000000000; ±0.0 -> 0; every other pattern -> its raw bits.
inline uint64_t lv_canon(double x) {
    uint64_t b;
    std::memcpy(&b, &x, 8);                                    // bit reinterpret, no cast UB
    uint64_t is_nan  = ((b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull)
                     & ((b & 0x000FFFFFFFFFFFFFull) != 0);
    uint64_t is_zero = (b << 1) == 0;
    return is_nan ? 0x7FF8000000000000ull : (is_zero ? 0 : b);
}

// --- key equality for Map (Track 05 C3: primitives by value; structs
// field-wise recursive (a struct IS its fields, §9); classes by identity) -----

inline bool keyEquals(const Value& a, const Value& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case VKind::Int:    return a.i == b.i;
        case VKind::Char:   return a.i == b.i;
        case VKind::String: return a.s == b.s;
        case VKind::Bool:   return a.b == b.b;
        case VKind::Float:  return lv_canon(a.f) == lv_canon(b.f);   // Map keys canonical (§4)
        case VKind::Object:
            if (a.obj && b.obj && a.obj->cls && a.obj->cls == b.obj->cls && a.obj->cls->isValue) {
                if (a.obj->fields.size() != b.obj->fields.size()) return false;
                for (const auto& [k, av] : a.obj->fields) {
                    auto it = b.obj->fields.find(k);
                    if (it == b.obj->fields.end() || !keyEquals(av, it->second)) return false;
                }
                return true;
            }
            return a.obj == b.obj;
        case VKind::None:   return true;
        case VKind::Block:  return a.block == b.block;   // reference identity
        case VKind::Ast:    return a.ast == b.ast;
        default:            return false;
    }
}

// --- default value for a bare declaration (§3 auto-construct) -----------------

inline Value defaultForCanonical(const std::string& c) {
    // Unions: None member -> None (never fabricate data); else first member.
    // Bracket-aware: a naive `c.find(" | ")` also matches a " | " nested
    // inside a generic argument (e.g. "Promise<T | None>" is one type, not a
    // top-level union), and unionMembersOf's bracket-aware split then returns
    // the whole string back unchanged, so recursing into members[0] recursed
    // forever (bug.md #1). unionMembersOf is already bracket-aware, so trust
    // its split count instead of the naive substring search.
    std::vector<std::string> members = unionMembersOf(c);
    if (members.size() > 1) {
        for (const std::string& m : members)
            if (m == "None") return vnone();
        return defaultForCanonical(members[0]);
    }
    if (c == "None") return vnone();
    if (c == "int") return vint(0);
    if (c == "char") return vchar(0);        // Track 03 §1: bare `char` = scalar 0 ('\0')
    if (c == "bool") return vbool(false);
    if (c == "float") return vfloat(0);
    if (c == "string") return vstr("");
    if (c.rfind("Array", 0) == 0) {
        Value v; v.kind = VKind::Array; v.arr = std::make_shared<std::vector<Value>>();
        return v;
    }
    if (c.rfind("Map", 0) == 0) {
        Value v; v.kind = VKind::Map;
        v.map = std::make_shared<std::vector<std::pair<Value, Value>>>();
        return v;
    }
    return Value{};
}

// --- native method implementations (string/int/bool/float/Array/Map cores) ----
// Returns true if (cls, method) is native. On failure err is set and the engine
// should raise a RuntimeException with it.
bool nativeCall(std::string_view cls, const std::string& method, const Value& self,
                std::vector<Value>& args, Value& out, std::string& err,
                bool cowReceiver = false);

// Native FREE functions (the std::sys floor: sysWrite, sysReadLine). `sink`
// receives fd 1/2 writes when the engine captures output (interpreters); null
// sink writes to the real descriptors.
bool nativeFreeCall(const std::string& name, std::vector<Value>& args, Value& out,
                    std::string& err, std::string* sink);

// bug #35 (reject route A): does `v` (a program global's CURRENT value) reach a
// Promise-derived object the A-1 thread boundary forbids? A spawn body may
// reference a Promise through a bare global rather than a captured local, and
// the capture flatten only walks captures — so both interpreters re-scan the
// globals a spawn body references at the spawn call. Recurses object fields,
// array elements, and map entries (Channel portals are relocatable — skipped),
// scoped to Promise-derived objects. Returns true and sets `err` to the
// byte-identical reject message on the first hit. Mirrors the LLVM runtime's
// lvrt_value_has_promise so every engine rejects the same shape.
bool lvThreadValueHasPromise(const Value& v, std::string& err);

// argv registry for the interpreters (--run/--ir). The driver (main.cpp) stashes
// the program's argument vector here (synthesized argv[0] + the `--` tail) before
// an engine runs; the `sysArgs` native reads it. Compiled backends (LLVM,
// emit-C++) get argv from the OS instead and never consult this (designs/argv.md
// §5.2). Always holds >= 1 element once set.
void setProgramArgs(std::vector<std::string> args);

// Process exit for the interpreters (designs/exit-codes.md §4). The sysExit /
// sysSetExitCode natives record a code (and, for exit-now, an exit-requested
// flag) here rather than raw _exit'ing inside the leviathan process. Both
// engines reset this at the start of a run, poll interpExitRequested() after a
// native call to trigger a clean unwind, and read interpExitCode() to thread the
// code out to main.cpp (which flushes the captured output, then std::exit).
void interpResetExit();
int  interpExitCode();
bool interpExitRequested();

// Terminal floor (designs/techdesign-terminal-floor.md §3, M4): close every
// signal fd the program opened and unblock its signals at program end, so the
// leviathan process (which outlives a --run/--ir program) never carries a stale
// signal block or descriptor. Called from Eval::run / IrInterp::run.
void interpSignalCleanup();

// --- primitive arithmetic / comparison / string ops (shared semantics) -------

// `err`, when non-null and left non-empty by this call, signals a runtime
// exception the CALLER must raise (arithPrim is a free function shared by the
// oracle and the IR interpreter — it has no access to either engine's throw
// machinery). Today the only such case is a shift count outside 0..63.
inline Value arithPrim(TokenKind op, const Value& l, const Value& r,
                       std::string* err = nullptr) {
    // None equality first: None equals only None (never a present value).
    if (l.kind == VKind::None || r.kind == VKind::None) {
        if (op == TokenKind::EqEq)   return vbool(l.kind == r.kind);
        if (op == TokenKind::BangEq) return vbool(l.kind != r.kind);
        return vvoid();
    }
    // Bool equality/inequality compares .b — never populated in the int
    // fallback below, same never-populated-field family as the float and
    // string relational rows above/below (bug.md #11).
    if (l.kind == VKind::Bool && r.kind == VKind::Bool) {
        if (op == TokenKind::EqEq)   return vbool(l.b == r.b);
        if (op == TokenKind::BangEq) return vbool(l.b != r.b);
        return vvoid();
    }
    // Block is a reference type: operators compare the view object identity.
    // Byte content comparison is the explicit Block.equals() native.
    if (l.kind == VKind::Block && r.kind == VKind::Block) {
        if (op == TokenKind::EqEq)   return vbool(l.block == r.block);
        if (op == TokenKind::BangEq) return vbool(l.block != r.block);
        return vvoid();
    }
    // Track 03 §1: char compares by scalar value (the `i` field). No arithmetic
    // (the checker allows only comparisons — use `code()` for math).
    if (l.kind == VKind::Char && r.kind == VKind::Char) {
        long long a = l.i, b = r.i;
        switch (op) {
            case TokenKind::EqEq:   return vbool(a == b);
            case TokenKind::BangEq: return vbool(a != b);
            case TokenKind::Lt:     return vbool(a <  b);
            case TokenKind::Gt:     return vbool(a >  b);
            case TokenKind::Le:     return vbool(a <= b);
            case TokenKind::Ge:     return vbool(a >= b);
            default:                return vvoid();
        }
    }
    if (l.kind == VKind::String || r.kind == VKind::String) {
        if (op == TokenKind::Plus)   return vstr(valueToString(l) + valueToString(r));
        if (op == TokenKind::EqEq)   return vbool(l.s == r.s);
        if (op == TokenKind::BangEq) return vbool(l.s != r.s);
        // Relational ops compare lexicographically (std::string already does the
        // right thing) — not the never-populated .i field (bug.md #2).
        if (op == TokenKind::Lt)     return vbool(l.s <  r.s);
        if (op == TokenKind::Gt)     return vbool(l.s >  r.s);
        if (op == TokenKind::Le)     return vbool(l.s <= r.s);
        if (op == TokenKind::Ge)     return vbool(l.s >= r.s);
    }
    // Float arithmetic computes in double (an int operand promotes). Before this
    // branch existed every float op fell through to the int path below and
    // silently computed on the never-populated .i field — 1.5 + 2.5 was 0.
    if ((l.kind == VKind::Float && (r.kind == VKind::Float || r.kind == VKind::Int)) ||
        (r.kind == VKind::Float && l.kind == VKind::Int)) {
        double a = l.kind == VKind::Float ? l.f : (double)l.i;
        double b = r.kind == VKind::Float ? r.f : (double)r.i;
        switch (op) {
            case TokenKind::Plus:   return vfloat(a + b);
            case TokenKind::Minus:  return vfloat(a - b);
            case TokenKind::Star:   return vfloat(a * b);
            case TokenKind::Slash:  return vfloat(a / b);   // IEEE: /0 -> inf/nan
            case TokenKind::EqEq:   return vbool(a == b);
            case TokenKind::BangEq: return vbool(a != b);
            case TokenKind::Lt:     return vbool(a < b);
            case TokenKind::Gt:     return vbool(a > b);
            case TokenKind::Le:     return vbool(a <= b);
            case TokenKind::Ge:     return vbool(a >= b);
            default:                return vvoid();          // % and bitwise: no float form
        }
    }
    long long a = l.i, b = r.i;
    switch (op) {
        case TokenKind::Plus:    return vint(a + b);
        case TokenKind::Minus:   return vint(a - b);
        case TokenKind::Star:    return vint(a * b);
        // Integer division/modulo by zero throws a catchable RuntimeException
        // (§3.7 loudness, bug.md #10) rather than silently yielding 0. Floats
        // keep IEEE inf/nan above (a defined answer; integers have none). The
        // caller raises from `err` — same mechanism as the shift range check.
        case TokenKind::Slash:
            if (b == 0) { if (err) *err = "division by zero"; return vvoid(); }
            return vint(a / b);
        case TokenKind::Percent:
            if (b == 0) { if (err) *err = "modulo by zero"; return vvoid(); }
            return vint(a % b);
        case TokenKind::EqEq:    return vbool(a == b);
        case TokenKind::BangEq:  return vbool(a != b);
        case TokenKind::Lt:      return vbool(a < b);
        case TokenKind::Gt:      return vbool(a > b);
        case TokenKind::Le:      return vbool(a <= b);
        case TokenKind::Ge:      return vbool(a >= b);
        case TokenKind::Pipe:    return vint(a | b);
        case TokenKind::Amp:     return vint(a & b);
        case TokenKind::Caret:   return vint(a ^ b);
        // Shift count outside 0..63 throws rather than inheriting x86's silent
        // mask-to-6-bits or C++'s undefined behavior (§3.7 loudness). Within
        // range, C++20 gives `<<`/`>>` on a signed operand well-defined bit-
        // pattern / arithmetic-shift semantics — no manual sign handling.
        case TokenKind::LtLt:
            if (b < 0 || b > 63) { if (err) *err = "shift count out of range"; return vvoid(); }
            return vint(a << b);
        case TokenKind::GtGt:
            if (b < 0 || b > 63) { if (err) *err = "shift count out of range"; return vvoid(); }
            return vint(a >> b);
        default:                 return vvoid();
    }
}
