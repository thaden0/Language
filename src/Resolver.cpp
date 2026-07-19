#include "Resolver.hpp"
#include <algorithm>
#include <cctype>

namespace {

// A minimal prelude: primitives are registered directly; these library types
// are just ordinary declarations gathered like any other source (§12). This is
// also what lets the IOStream diamond be a real, testable shape.
//
// Track 09 §0 (prelude-scale decision): the prelude is split into several
// adjacent raw-string segments concatenated at gather (parsePrelude below).
// This is a mechanical, zero-semantic partition — whole-program resolution
// (§12) makes segment order irrelevant, so the seams are just organizational.
//   kPreludeCore : primitive object masks + collections + Seq + streams glue
//   kPreludeStd  : `namespace std` (sys floor, aggregates, promise/timer,
//                  sockets + HTTP, files, exception hierarchy)
//   kPreludeRest : top-level Range/StreamBuffer/Console + meta/math/env/term
//   kPreludeWeb  : Track 09 web foundations (encoding, digest, DateTime, json)
//   kPreludeRegexCore : Track 10 regex engine (namespace regex internals)
//   kPreludeRegexApi  : Track 10 regex public surface (Regex/Match/Group/...)
const char* kPreludeCore = R"prelude(
// Primitive object masks (§6.5): value types that carry a method shape. Stored
// unboxed; `this` is the value. Empty-bodied methods are native intrinsics;
// bodies present (e.g. int.abs) are ordinary in-language methods.
class bool {
    string toString();
}
class int {
    int abs() => this < 0 ? 0 - this : this;
    int max(int other) => this > other ? this : other;
    int min(int other) => this < other ? this : other;
    string toString();

    // --- Track 06: math & numeric masks --------------------------------------
    // Square-and-multiply; e<0 -> 0 (integer semantics, documented); overflow
    // wraps (int64 two's complement — no trap v1, matches the rest of int).
    int pow(int e) {
        if (e < 0) return 0;
        int result = 1;
        int base = this;
        int n = e;
        while (n > 0) {
            if (n % 2 == 1) result = result * base;
            base = base * base;
            n = n / 2;
        }
        return result;
    }
    int clamp(int lo, int hi) {
        if (lo > hi) throw RuntimeException("clamp: lo > hi");
        if (this < lo) return lo;
        if (this > hi) return hi;
        return this;
    }
    int sign() => this < 0 ? 0 - 1 : (this > 0 ? 1 : 0);
    // Exact for |x| < 2^53; native (RuntimeNatives.cpp).
    float toFloat();
    // Hex digits, lowercase, no `0x` prefix, `-` sign for negatives. INT64_MIN
    // is special-cased: negating it overflows (two's complement has no
    // positive counterpart), so shifting toward 0 would never terminate.
    string toHex() {
        if (this == 0) return "0";
        if (this == 1 << 63) return "-8000000000000000";
        string digits = "0123456789abcdef";
        bool neg = this < 0;
        int n = neg ? 0 - this : this;
        string r = "";
        while (n != 0) {
            r = digits.charAt(n & 15) + r;
            n = n >> 4;
        }
        return neg ? "-" + r : r;
    }
    // radix 2..36; else throw. Never negates `this` (division/modulo toward
    // zero terminates for every int64 value, including INT64_MIN, unlike the
    // shift-based toHex above).
    string toString(int radix) {
        if (radix < 2 || radix > 36) throw RuntimeException("radix out of range 2..36");
        if (this == 0) return "0";
        string digits = "0123456789abcdefghijklmnopqrstuvwxyz";
        bool neg = this < 0;
        int n = this;
        string r = "";
        while (n != 0) {
            int d = n % radix;
            int dd = d < 0 ? 0 - d : d;
            r = digits.charAt(dd) + r;
            n = n / radix;
        }
        return neg ? "-" + r : r;
    }
}
// Track 03 §1: `char` — one Unicode scalar, unboxed value primitive. `code()`
// and `toString()` are native (UTF-8); the classification/case helpers are
// in-language over `code()` (ASCII-only in v1, documented; non-ASCII -> false /
// unchanged). No arithmetic operators — use `code()`.
class char {
    int code();
    string toString();
    // NOTE: these bodies avoid BARE self-calls (e.g. `isLower()` inside another
    // method) — those aren't checker-resolved in prelude bodies (bug.md #13) and
    // misbehave on the compiled backends (bug.md #27 family). Each helper works
    // off the native `code()` directly instead, so every one is a leaf.
    bool isDigit() => code() >= 48 && code() <= 57;
    bool isUpper() => code() >= 65 && code() <= 90;
    bool isLower() => code() >= 97 && code() <= 122;
    bool isAlpha() => (code() >= 65 && code() <= 90) || (code() >= 97 && code() <= 122);
    bool isSpace() {
        int c = code();
        return c == 32 || c == 9 || c == 10 || c == 13;
    }
    char toUpper() => (code() >= 97 && code() <= 122) ? std::charFromCode(code() - 32) : this;
    char toLower() => (code() >= 65 && code() <= 90) ? std::charFromCode(code() + 32) : this;
}
// Track 03 §3: `Block` — a fixed-length mutable byte buffer (contract C4).
// Reference semantics; `slice` is an aliasing view (shared bytes). Every access
// is bounds-checked (throws RuntimeException). All methods are native; the
// engines construct/dispatch Block specially (like Array/Map). `equals` is
// CONTENT equality; `==` remains reference identity. No ELF lane (frozen).
class Block {
    new Block(int size) {}
    new fromString(string s) {}
    int length();
    int byteAt(int i);
    void setByte(int i, int value);
    Block slice(int off, int len);
    string toString(int off, int len);
    int int32At(int i);
    void setInt32(int i, int value);
    int int64At(int i);
    void setInt64(int i, int value);
    void fill(int off, int len, int value);
    void blit(int dstOff, Block src, int srcOff, int len);
    bool equals(Block other);
    // First differing index at or after `from`, or -1. Equal lengths required.
    int mismatch(Block other, int from);
}

// F4: opaque compile-time code carrier. It has no constructible/traversable
// runtime surface; meta::parse* below are intercepted by the comptime oracle.
class Ast;
class float {
    string toString();

    // --- Track 06: math & numeric masks --------------------------------------
    float abs() => this < 0.0 ? 0.0 - this : this;   // NaN passes through, fine
    // Native (RuntimeNatives.cpp): round = half-away-from-zero (C `round`).
    float floor();
    float ceil();
    float round();
    float trunc();
    // Truncation; NaN/±inf/out-of-int64-range -> RuntimeException (loud).
    int toInt();
    float sqrt();       // negative -> NaN (IEEE, not a throw — math convention)
    float pow(float e);
    // struct-equality design §8: raw IEEE-754 bit access (serialization wants
    // these regardless). `bits()` = bit_cast to int64; `float::fromBits(int)`
    // is math::floatFromBits, routed by the checker. `canonEq` is the
    // canonical-relation primitive (§3) the synthesized struct `(==)` compares
    // float fields through — every engine implements it via its ONE canon.
    int bits();
    bool canonEq(float other);
    bool isNaN() => this != this;   // IEEE self-inequality — honest on all engines
    // Reformulated from the design's `this == 1.0/0.0` sentinel: bug.md #12
    // (LLVM's 0.0/0.0 silently yields 0.0, not NaN/inf — not this track's to
    // fix) would make that form always-false on that one engine. Comparing
    // the magnitude against the largest finite double sidesteps the division
    // entirely and stays honest for +inf/-inf/finite/NaN on all five engines
    // (verified: P2 probe, this track's implementation log).
    bool isInfinite() {
        float m = 179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368.0;
        return this.abs() > m;
    }
}
class string {
    int length();
    bool isEmpty();
    string charAt(int i);
    string subStr(int start, int len);
    // Byte-indexed range slice — matches subStr(start, len) exactly (Range has
    // no unit of its own; this is "the same value used as a slice argument",
    // applied to the byte-indexed core per the Bytes-vs-scalars rule, not a
    // new rune-indexed slice). Range is inclusive-inclusive (class Range's own
    // ctor), so len = end - start + 1.
    //
    // Deliberately NOT named `subStr` (not an overload) -- a same-named
    // body-bearing overload would hijack every existing bare self-call to
    // subStr(int,int) inside this class on the frozen --emit-elf backend
    // (name-only, arity-blind X64Gen::genCallM dynamic dispatch;
    // designs/techdesign-stdlib-tail-methods.md §2.1). Same shape of fix
    // Track 04 used for indexOf/indexOfFrom.
    //
    // Empty/backwards ranges (end < start) are a defined "" on every engine,
    // decided uniformly here before the native subStr's own cross-engine
    // negative-len disagreement is ever reached. Out-of-bounds end inherits
    // subStr's historical clamp-not-throw (unlike Array.slice, which throws).
    string subStrRange(Range r) {
        int len = r.end - r.start + 1;
        return len <= 0 ? "" : subStr(r.start, len);
    }
    string toUpper();
    string toLower();
    string trim();
    bool contains(string sub);
    bool startsWith(string prefix);
    bool endsWith(string suffix);
    int indexOf(string sub);
    // Strict full-string parse: optional leading '-', digits only, no
    // surrounding space; anything else (or overflow) -> None. BREAKING vs the
    // old atoll-garbage-to-0 behavior (Track 04 M2; migrate callers with
    // `int? p = s.toInt(); if (p != None) { ... }`).
    int? toInt();
    float? toFloat();
    // Byte value 0..255 at index `i`. OOB throws RuntimeException, same as
    // Array.at (Track 04 M3).
    int byteAt(int i);
    // Track 03 §1 (contract C1): `at` decodes the Unicode scalar STARTING at
    // byte offset i (O(1)); a mid-sequence offset throws. `chars` full-decodes
    // to an Array<char> (invalid bytes -> U+FFFD, never a throw). Native.
    char at(int i);
    Array<char> chars();
    string toString() => this;

    // --- Track 04: in-language toolkit over the native core above -----------
    // Bodies slice whole segments (subStr/charAt), never accumulate char-by-char.

    // indexOf from an offset. NOT an `indexOf` overload: primitive-mask method
    // bodies aren't checker-verified (Checker::walk only walks the user
    // program, never the prelude — bug.md), so a same-name bare self-call
    // inside this class would resolve through Eval/Lower's arity-blind
    // by-name fallback and silently pick the wrong arity. Distinct name
    // sidesteps the ambiguity entirely (per this track's design, problem #5).
    // Negative `from` clamps to 0; a `from` past the end is -1, never an error.
    int indexOfFrom(string sub, int from) {
        int f = from < 0 ? 0 : from;
        if (f > length()) return -1;
        int r = subStr(f, length() - f).indexOf(sub);
        return r < 0 ? -1 : r + f;
    }

    // Earliest index at which ANY needle occurs, or -1 if none do (incl. an
    // empty needles array). Does not report WHICH needle matched; a caller
    // that needs the identity can re-check each needle's indexOf against the
    // returned position. Order-independence: the POSITION found is what's
    // minimized, not needle array order. `""` in needles -> 0 always (indexOf
    // on an empty pattern is 0 on every engine), a defined edge case.
    int indexOfAny(Array<string> needles) {
        int best = -1;
        for (string needle in needles) {
            int p = indexOf(needle);
            if (p >= 0 && (best < 0 || p < best)) best = p;
        }
        return best;
    }

    int lastIndexOf(string sub) {
        int last = -1;
        int idx = indexOf(sub);
        while (idx >= 0) {
            last = idx;
            idx = indexOfFrom(sub, idx + 1);
        }
        return last;
    }

    // Empty separator -> array of 1-char strings (JS-compatible). Otherwise
    // keeps empty segments (leading/trailing/adjacent separators all count).
    Array<string> split(string sep) {
        Array<string> r = [];
        if (sep.isEmpty()) {
            for (int i in 0 .. length() - 1) r = r.add(charAt(i));
            return r;
        }
        int start = 0;
        int idx = indexOf(sep);
        while (idx >= 0) {
            r = r.add(subStr(start, idx - start));
            start = idx + sep.length();
            idx = indexOfFrom(sep, start);
        }
        r = r.add(subStr(start, length() - start));
        return r;
    }

    // Replace-all; segment-concat between matches. `from == ""` is a no-op
    // (defined, rather than looping forever).
    string replace(string from, string to) {
        if (from.isEmpty()) return this;
        string r = "";
        int start = 0;
        int idx = indexOf(from);
        while (idx >= 0) {
            r = r + subStr(start, idx - start) + to;
            start = idx + from.length();
            idx = indexOfFrom(from, start);
        }
        r = r + subStr(start, length() - start);
        return r;
    }

    string padStart(int targetLen, string pad) {
        if (pad.isEmpty() || length() >= targetLen) return this;
        int need = targetLen - length();
        string p = pad.repeat(need / pad.length() + 1);
        return p.subStr(0, need) + this;
    }
    string padEnd(int targetLen, string pad) {
        if (pad.isEmpty() || length() >= targetLen) return this;
        int need = targetLen - length();
        string p = pad.repeat(need / pad.length() + 1);
        return this + p.subStr(0, need);
    }

    // Exponentiation-by-squaring: O(log n) concats, not O(n) char appends.
    string repeat(int n) {
        if (n <= 0) return "";
        string result = "";
        string base = this;
        int k = n;
        while (k > 0) {
            if (k % 2 == 1) result = result + base;
            base = base + base;
            k = k / 2;
        }
        return result;
    }

    string trimStart() {
        int i = 0;
        int n = length();
        while (i < n && (charAt(i) == " " || charAt(i) == "\t" || charAt(i) == "\n" || charAt(i) == "\r")) i = i + 1;
        return subStr(i, n - i);
    }
    string trimEnd() {
        int n = length();
        int i = n;
        while (i > 0 && (charAt(i - 1) == " " || charAt(i - 1) == "\t" || charAt(i - 1) == "\n" || charAt(i - 1) == "\r")) i = i - 1;
        return subStr(0, i);
    }

    // Splits on "\n", then trims exactly one trailing "\r" per line (handles
    // both CRLF and bare LF without disturbing intentional trailing spaces).
    Array<string> splitLines() {
        Array<string> lines = split("\n");
        Array<string> r = [];
        for (string line in lines) {
            if (line.endsWith("\r")) r = r.add(line.subStr(0, line.length() - 1));
            else r = r.add(line);
        }
        return r;
    }

    bool isBlank() => trim().isEmpty();

    // Non-overlapping occurrence count. `sub == ""` -> 0 (defined).
    int count(string sub) {
        if (sub.isEmpty()) return 0;
        int n = 0;
        int idx = indexOf(sub);
        while (idx >= 0) {
            n = n + 1;
            idx = indexOfFrom(sub, idx + sub.length());
        }
        return n;
    }

    string removePrefix(string prefix) => startsWith(prefix) ? subStr(prefix.length(), length() - prefix.length()) : this;
    string removeSuffix(string suffix) => endsWith(suffix) ? subStr(0, length() - suffix.length()) : this;

    // v1: allocates via toLower(); a non-allocating byte-compare is a follow-up.
    bool equalsIgnoreCase(string other) => length() == other.length() && toLower() == other.toLower();

    // UTF-8-correct reversal: reverses SCALARS, not bytes ("désert" -> "tréséd"
    // with the é intact — a byte-reverse would shred every multi-byte sequence).
    // Ill-formed bytes normalize to U+FFFD via chars() (§3.2 policy). The handoff
    // promised in techdesign-04 §8 / techdesign-utf8-chars-string-ops.md §4.1.
    string reverse() => chars().reverse().joinToString("");

    // LA-31 ruling R13: SQL LIKE semantics, byte-oriented, anchored full-string
    // match. `%` = any run of bytes incl. empty; `_` = exactly one byte; `\`
    // escapes the next byte to a literal (a lone trailing `\` matches a literal
    // `\`). `ilike` folds A-Z<->a-z per byte at each comparison site, no folded
    // string copies. Explicit `this.` on every self-call (this-receiver rule).
    bool like(string pattern) return this.__likeMatch(pattern, false);

    bool ilike(string pattern) return this.__likeMatch(pattern, true);

    bool __likeMatch(string pattern, bool fold) {
        int n = this.length();
        int m = pattern.length();
        int i = 0;
        int j = 0;
        int star = 0 - 1;
        int mark = 0;
        while (i < n) {
            int pb = 0 - 1;
            if (j < m) pb = pattern.byteAt(j);
            if (pb == 92 && j + 1 < m) {
                int lit = pattern.byteAt(j + 1);
                int tb = this.byteAt(i);
                if (fold) {
                    if (lit >= 65 && lit <= 90) lit = lit + 32;
                    if (tb >= 65 && tb <= 90) tb = tb + 32;
                }
                if (lit == tb) { i = i + 1; j = j + 2; }
                else if (star >= 0) { j = star + 1; mark = mark + 1; i = mark; }
                else return false;
            } else if (pb == 95) {
                i = i + 1; j = j + 1;
            } else if (pb == 37) {
                star = j; mark = i; j = j + 1;
            } else {
                int tb = this.byteAt(i);
                int qb = pb;
                if (fold) {
                    if (qb >= 65 && qb <= 90) qb = qb + 32;
                    if (tb >= 65 && tb <= 90) tb = tb + 32;
                }
                if (j < m && qb == tb) { i = i + 1; j = j + 1; }
                else if (star >= 0) { j = star + 1; mark = mark + 1; i = mark; }
                else return false;
            }
        }
        while (j < m && pattern.byteAt(j) == 37) j = j + 1;
        return j == m;
    }
}

// Array<T>: a tiny native core (length/at/add — add is pure, returns a new
// array) with the LINQ/JS surface written in the language on top of it.
// A 2-tuple, used as the result element of relational joins.
class Pair<A, B> {
    A first;
    B second;
    new Of(A a, B b) { first = a; second = b; }
}

// The iteration protocol (techdesign-07 §1). `for (T x in e)` where e's static
// type implements IIterable<T> desugars to a hasNext()/next() pull loop. The
// built-in collections take dedicated fast paths (contract C5); these interfaces
// give protocol UNIFORMITY — an Array/Map/Range can stand where an IIterable is
// wanted, and user collections plug into the same `for..in`.
interface IIterator<T> {
    bool hasNext();
    T next();      // past-the-end is unspecified; stdlib iterators throw (loud)
}
interface IIterable<T> {
    IIterator<T> iterator();
}

class Array<T> : IIterable<T> {
    new Array() { }                    // empty (native)
    new Array(int n, T fill) { }       // n copies of fill (native), like vector(n, v)

    int length();                 // native
    T at(int i);                  // native
    Array<T> add(T item);         // native (pure: returns a new array)

    get ([])(int i) => at(i);     // arrays index through the same ([]) any class can define

    // Protocol uniformity (techdesign-07 §2.1): `for..in` over an array still
    // takes the IterLen/IterAt fast path — this exists so an Array can be passed
    // where an IIterable<T> is wanted (e.g. asSeq()'s source). Snapshots a pure
    // value, so the iterator can never be invalidated (design problem #2).
    IIterator<T> iterator() => ArrayIterator(this);

    // The bridge into the lazy pipeline (techdesign-07 §3): arrays are eager,
    // `Seq` is the opt-in lazy form (answering info.md §19#4).
    Seq<T> asSeq() => ArraySeq(this);

    bool isEmpty() => length() == 0;
    T first() => at(0);
    T last() => at(length() - 1);
    T? firstOrNone() => isEmpty() ? None : first();
    T? lastOrNone() => isEmpty() ? None : last();
    T? find((T) => bool pred) {
        for (T x in this) if (pred(x)) return x;
        return None;
    }

    Array<T> where((T) => bool pred) {
        Array<T> r = [];
        for (T x in this) if (pred(x)) r = r.add(x);
        return r;
    }
    Array<T> filter((T) => bool pred) => this.where(pred);
    Array<U> map<U>((T) => U fn) {
        Array<U> r = [];
        for (T x in this) r = r.add(fn(x));
        return r;
    }
    Array<U> select<U>((T) => U fn) => this.map(fn);
    A reduce<A>(A seed, (A, T) => A fn) {
        A acc = seed;
        for (T x in this) acc = fn(acc, x);
        return acc;
    }
    bool any((T) => bool pred) {
        for (T x in this) if (pred(x)) return true;
        return false;
    }
    bool all((T) => bool pred) {
        for (T x in this) if (!pred(x)) return false;
        return true;
    }
    int count((T) => bool pred) {
        int n = 0;
        for (T x in this) if (pred(x)) n = n + 1;
        return n;
    }
    bool contains(T item) {
        for (T x in this) if (x == item) return true;
        return false;
    }
    int indexOf(T item) {
        int i = 0;
        for (T x in this) { if (x == item) return i; i = i + 1; }
        return -1;
    }
    Array<T> reverse() {
        Array<T> r = [];
        int n = length();
        for (int i in 0 .. n - 1) r = r.add(at(n - 1 - i));
        return r;
    }
    Array<T> take(int k) {
        Array<T> r = [];
        int m = k < length() ? k : length();
        for (int i in 0 .. m - 1) r = r.add(at(i));
        return r;
    }
    Array<T> skip(int k) {
        Array<T> r = [];
        for (int i in k .. length() - 1) r = r.add(at(i));
        return r;
    }
    Array<T> concat(Array<T> other) {
        Array<T> r = this;
        for (T x in other) r = r.add(x);
        return r;
    }
    // Relational joins (§11). Predicate form for now; nested-loop (O(n*m)) —
    // the by-key hidden index needs a Dictionary type (later). `U` is inferred
    // from `other`, so these are honestly typed thanks to method-level generics.
    Array<Pair<T, U>> join<U>(Array<U> other, (T, U) => bool pred) {
        Array<Pair<T, U>> r = [];
        for (T x in this)
            for (U y in other)
                if (pred(x, y)) r = r.add(Pair::Of(x, y));
        return r;
    }
    Array<Pair<T, Array<U>>> groupJoin<U>(Array<U> other, (T, U) => bool pred) {
        Array<Pair<T, Array<U>>> r = [];
        for (T x in this) {
            Array<U> matches = [];
            for (U y in other) if (pred(x, y)) matches = matches.add(y);
            r = r.add(Pair::Of(x, matches));
        }
        return r;
    }
    Array<U> flatMap<U>((T) => Array<U> fn) {
        Array<U> r = [];
        for (T x in this) for (U y in fn(x)) r = r.add(y);
        return r;
    }
    void forEach((T) => void fn) {
        for (T x in this) fn(x);
    }
    // Named `unique`, not `distinct` — `distinct` is a reserved member
    // modifier keyword (diamond-inheritance disambiguation), so it can't be
    // a method name at class-body declaration position.
    Array<T> unique() {
        Array<T> r = [];
        for (T x in this) if (!r.contains(x)) r = r.add(x);
        return r;
    }
    // O(n^2) scan-map v1 (correctness now, Dictionary later, §6): `m[k]`
    // rebind-accumulate, not `.with()` (portable to every engine's `[]` sugar).
    Map<K, Array<T>> groupBy<K>((T) => K key) {
        Map<K, Array<T>> m = Map();
        for (T x in this) {
            K k = key(x);
            Array<T> cur = m.has(k) ? m[k] : [];
            m[k] = cur.add(x);
        }
        return m;
    }
    Array<Pair<T, U>> zip<U>(Array<U> other) {
        Array<Pair<T, U>> r = [];
        int n = length() < other.length() ? length() : other.length();
        for (int i in 0 .. n - 1) r = r.add(Pair::Of(at(i), other.at(i)));
        return r;
    }
    Array<T> takeWhile((T) => bool pred) {
        Array<T> r = [];
        for (T x in this) { if (!pred(x)) return r; r = r.add(x); }
        return r;
    }
    Array<T> skipWhile((T) => bool pred) {
        Array<T> r = [];
        bool skipping = true;
        for (T x in this) {
            if (skipping) {
                if (pred(x)) continue;
                skipping = false;
            }
            r = r.add(x);
        }
        return r;
    }
    int indexWhere((T) => bool pred) {
        int i = 0;
        for (T x in this) { if (pred(x)) return i; i = i + 1; }
        return -1;
    }
    Array<Pair<int, T>> withIndex() {
        Array<Pair<int, T>> r = [];
        int i = 0;
        for (T x in this) { r = r.add(Pair::Of(i, x)); i = i + 1; }
        return r;
    }
    Array<T> insertAt(int i, T v) {
        if (i < 0 || i > length()) throw RuntimeException("insertAt: index out of bounds");
        Array<T> r = take(i);
        r = r.add(v);
        for (int j in i .. length() - 1) r = r.add(at(j));
        return r;
    }
    Array<T> removeAt(int i) {
        if (i < 0 || i >= length()) throw RuntimeException("removeAt: index out of bounds");
        Array<T> r = take(i);
        for (int j in i + 1 .. length() - 1) r = r.add(at(j));
        return r;
    }
    // Pure update at index `i` — mirrors Map's `with` vocabulary (Array.with
    // sets an index; Map.with adds a key), an added overload, no name clash.
    Array<T> with(int i, T v) {
        if (i < 0 || i >= length()) throw RuntimeException("with: index out of bounds");
        Array<T> r = [];
        for (int j in 0 .. length() - 1) r = r.add(j == i ? v : at(j));
        return r;
    }
    // Throws on OOB (unlike string.subStr's historical clamp).
    Array<T> slice(int start, int len) {
        if (start < 0 || len < 0 || start + len > length())
            throw RuntimeException("slice: out of bounds");
        Array<T> r = [];
        for (int j in start .. start + len - 1) r = r.add(at(j));
        return r;
    }
    // Stable merge sort: equal-key order is preserved (the `<=` tie-break
    // below keeps the earlier-index run first) — a contract `orderBy` (later)
    // depends on. Pure: returns a new array.
    Array<T> sort((T, T) => int cmp) {
        int n = length();
        if (n <= 1) return this;
        Array<T> left = take(n / 2).sort(cmp);
        Array<T> right = skip(n / 2).sort(cmp);
        Array<T> r = [];
        int i = 0;
        int j = 0;
        while (i < left.length() && j < right.length()) {
            if (cmp(left.at(i), right.at(j)) <= 0) { r = r.add(left.at(i)); i = i + 1; }
            else { r = r.add(right.at(j)); j = j + 1; }
        }
        while (i < left.length()) { r = r.add(left.at(i)); i = i + 1; }
        while (j < right.length()) { r = r.add(right.at(j)); j = j + 1; }
        return r;
    }
    // Duck-typed on K's `<` (instantiation-time error if K has none — honest).
    Array<T> sortBy<K>((T) => K key) {
        return sort((a, b) => key(a) < key(b) ? 0 - 1 : (key(b) < key(a) ? 1 : 0));
    }
    // orderBy is the K=1-run degenerate case of thenBy: everything starts in
    // one run (no key applied yet), so thenBy's own run-refinement collapses
    // to exactly a stable sort by `key` -- same algorithm, same correctness
    // guarantee as multi-key chains, zero duplicated sort logic.
    OrderedArray<T> orderBy<K>((T) => K key) {
        Array<int> allRun0 = [];
        for (int i in 0 .. length() - 1) allRun0 = allRun0.add(0);
        return OrderedArray(this, allRun0).thenBy(key);
    }
    T? minBy<K>((T) => K key) {
        if (isEmpty()) return None;
        T best = first();
        K bestKey = key(best);
        for (T x in this) {
            K k = key(x);
            if (k < bestKey) { best = x; bestKey = k; }
        }
        return best;
    }
    T? maxBy<K>((T) => K key) {
        if (isEmpty()) return None;
        T best = first();
        K bestKey = key(best);
        for (T x in this) {
            K k = key(x);
            if (bestKey < k) { best = x; bestKey = k; }
        }
        return best;
    }

    // Note: `join` is reserved for the by-key relational join (§11), so the
    // string-building form (JS Array.join / Kotlin joinToString) is named thus.
    string joinToString(string sep) {
        string s = "";
        int i = 0;
        for (T x in this) {
            if (i > 0) s = s + sep;
            s = s + x.toString();
            i = i + 1;
        }
        return s;
    }

    // O(total): sum lengths, one allocation, memcpy each part — the engine
    // behind StringBuilder.toString() (Track 04 M4). Declared on Array<T>
    // (return type `string`, not `T`) rather than specialized to Array<string>
    // — the language has no generic specialization — but is only meaningful,
    // and only ever called, on an Array<string>.
    string concatAll();     // native
}

// The Array's protocol iterator (techdesign-07 §2.1). An honest mutable cursor
// field `i` over a snapshotted pure-value array — reference semantics keep the
// cursor advancing across pulls (the composition-class stance of design
// problem #3; a snapshot closure would freeze it).
class ArrayIterator<T> : IIterator<T> {
    Array<T> a;
    int i = 0;
    new ArrayIterator(Array<T> src) { a = src; }
    bool hasNext() => i < a.length();
    T next() { T v = a.at(i); i = i + 1; return v; }
}

// A pure, in-language, zero-native wrapper over a sorted Array<T> -- exactly
// Set<T>'s mold. Carries a run id per element (a plain, type-erased int,
// monotonically assigned in current order) so that thenBy can tell "was
// already tied on every earlier key" from "differs on an earlier key"
// without growing a new generic parameter per chained call. Produced by
// Array.orderBy / OrderedArray.thenBy -- not meant to be hand-constructed
// (same norm as RangeIterator/ArrayIterator, technically public but
// protocol-internal; no private/module-visibility keyword exists).
class OrderedArray<T> : IIterable<T> {
    Array<T> items;
    Array<int> runId;   // runId[i] == runId[i-1]  <=>  items[i-1]/items[i]
                         // tie on every key applied so far
    new OrderedArray(Array<T> its, Array<int> ids) { items = its; runId = ids; }

    // Reuses the already-proven-stable Array.sort rather than a hand-rolled
    // segmented sort: tag each element with its EXISTING run id, sort tagged
    // pairs comparing run id first -- key() is never even evaluated for a
    // cross-run pair, so cross-run order literally cannot be touched, not
    // just "usually preserved" -- then recompute a finer run partition.
    OrderedArray<T> thenBy<K>((T) => K key) {
        Array<Pair<int, T>> tagged = [];
        for (int i in 0 .. items.length() - 1) {
            tagged = tagged.add(Pair::Of(runId.at(i), items.at(i)));
        }
        Array<Pair<int, T>> sorted = tagged.sort((p, q) => {
            if (p.first < q.first) return 0 - 1;
            if (q.first < p.first) return 1;
            K ka = key(p.second);
            K kb = key(q.second);
            return ka < kb ? 0 - 1 : (kb < ka ? 1 : 0);
        });
        Array<T> newItems = sorted.select((pr) => pr.second);
        Array<int> newRunId = [];
        int counter = 0;
        for (int i in 0 .. sorted.length() - 1) {
            if (i > 0) {
                Pair<int, T> prev = sorted.at(i - 1);
                Pair<int, T> cur = sorted.at(i);
                bool sameOldRun = prev.first == cur.first;
                K kprev = key(prev.second);
                K kcur = key(cur.second);
                bool tiedOnKey = !(kprev < kcur) && !(kcur < kprev);
                if (!(sameOldRun && tiedOnKey)) counter = counter + 1;
            }
            newRunId = newRunId.add(counter);
        }
        return OrderedArray(newItems, newRunId);
    }

    Array<T> toArray() => items;
    int length() => items.length();
    bool isEmpty() => items.isEmpty();
    IIterator<T> iterator() => OrderedArrayIterator(this);
}

class OrderedArrayIterator<T> : IIterator<T> {
    Array<T> a;
    int i = 0;
    new OrderedArrayIterator(OrderedArray<T> src) { a = src.items; }
    bool hasNext() => i < a.length();
    T next() { T v = a.at(i); i = i + 1; return v; }
}

// A mutable accumulator over Array<string>.concatAll() (Track 04 M4): a
// CLASS (reference semantics) since mutation across call sites is the point.
// `parts = parts.add(s)` is the same self-append shape Array<T>'s own bodies
// use; toString() is the one O(total) native call, not a join loop.
class StringBuilder {
    Array<string> parts = [];
    int len = 0;
    StringBuilder add(string s) { parts = parts.add(s); len = len + s.length(); return this; }
    StringBuilder (<<)(string s) => add(s);
    int length() => len;
    bool isEmpty() => len == 0;
    string toString() => parts.concatAll();
}

// Map<K, V>: a pure associative value (same model as Array): "changing"
// methods return a new map; m[k] = v is rebind-sugar. Insertion-ordered.
// (`get`/`set` are keywords, hence the pure vocabulary: at/with/without.)
class Map<K, V> : IIterable<Pair<K, V>> {
    new Map() { }

    int length();                     // native
    V at(K key);                      // native
    Map<K, V> with(K key, V val);     // native (pure add/update)
    Map<K, V> without(K key);         // native (pure remove)
    bool has(K key);                  // native
    Array<K> keys();                  // native
    Array<V> values();                // native

    get ([])(K key) => at(key);
    bool isEmpty() => length() == 0;

    // Protocol uniformity (techdesign-07 §2.1): the built-in `for (Pair e in m)`
    // fast path and this iterator yield the identical Pair sequence, in insertion
    // order (design problem #7). `for..in` over a map still takes the fast path.
    IIterator<Pair<K, V>> iterator() => MapIterator(this);

    V? atOrNone(K key) => has(key) ? at(key) : None;
    V atOr(K key, V dflt) => has(key) ? at(key) : dflt;
    // Iteration already yields Pair<K, V> (§11) — no keys()+at() double scan.
    Array<Pair<K, V>> entries() {
        Array<Pair<K, V>> r = [];
        for (Pair e in this) r = r.add(e);
        return r;
    }
    // Bracket-sugar accumulate (not `.with()`): portable to every engine,
    // including the two Bug 18 blocks (`.with()`/`.without()` are missing
    // from LLVM's and X64Gen's named-method native dispatch).
    Map<K, V> withAll(Map<K, V> o) {
        Map<K, V> m = this;
        for (Pair e in o) m[e.first] = e.second;
        return m;
    }
    Map<K, U> mapValues<U>((V) => U fn) {
        Map<K, U> m = Map();
        for (Pair e in this) m[e.first] = fn(e.second);
        return m;
    }
    Map<K, V> whereEntries((K, V) => bool pred) {
        Map<K, V> m = Map();
        for (Pair e in this) if (pred(e.first, e.second)) m[e.first] = e.second;
        return m;
    }
}

// The Map's protocol iterator (techdesign-07 §2.1). Snapshots the insertion-
// ordered key list and yields Pair<K, V> in that order — the same sequence the
// built-in Map fast path produces (design problem #7).
class MapIterator<K, V> : IIterator<Pair<K, V>> {
    Map<K, V> m;
    Array<K> ks;
    int i = 0;
    new MapIterator(Map<K, V> src) { m = src; ks = src.keys(); }
    bool hasNext() => i < ks.length();
    Pair<K, V> next() {
        K k = ks.at(i);
        i = i + 1;
        return Pair::Of(k, m.at(k));
    }
}

// Set<T>: in-language over Map<T, bool>, pure value semantics, insertion-
// ordered, zero natives (§4.1). `with`/`without` rebuild via bracket sugar
// (`m[v] = true`, a fresh scan-rebuild for without), never `.with()`/
// `.without()` by name — Bug 18 (bug.md) means those are missing from LLVM's
// and X64Gen's native dispatch, and Set has no reason to inherit that gap
// when the portable sugar path does the same job.
class Set<T> {
    Map<T, bool> m;
    new Set() { m = Map(); }
    // Convenience ctor: fold an Array into a Set.
    new Set(Array<T> items) {
        Map<T, bool> mm = Map();
        for (T x in items) mm[x] = true;
        m = mm;
    }
    int length() => m.length();
    bool isEmpty() => m.isEmpty();
    bool has(T v) => m.has(v);
    Set<T> with(T v) {
        Set<T> r = Set();
        Map<T, bool> mm = m;
        mm[v] = true;
        r.m = mm;
        return r;
    }
    Set<T> without(T v) {
        Set<T> r = Set();
        Map<T, bool> mm = Map();
        for (T x in m.keys()) if (x != v) mm[x] = true;
        r.m = mm;
        return r;
    }
    Array<T> toArray() => m.keys();
    string toString() => "{" + toArray().joinToString(", ") + "}";
    Set<T> union(Set<T> o) {
        Set<T> r = this;
        for (T x in o.toArray()) r = r.with(x);
        return r;
    }
    Set<T> intersect(Set<T> o) {
        Set<T> r = Set();
        for (T x in toArray()) if (o.has(x)) r = r.with(x);
        return r;
    }
    Set<T> except(Set<T> o) {
        Set<T> r = Set();
        for (T x in toArray()) if (!o.has(x)) r = r.with(x);
        return r;
    }
}

// ===========================================================================
//  Seq<T> — the lazy pipeline (techdesign-07 §3). Pure library over the
//  iterator protocol: NOTHING runs until a terminal (toArray/firstOrNone/
//  count/forEach/reduce) pulls; each combinator's fn runs at most once per
//  pulled element; firstOrNone short-circuits.
//
//  Implementation strategy (the design's central decision, problem #1/#3):
//  iterator-COMPOSITION classes, not closures-holding-closures. Each combinator
//  returns a wrapper Seq whose iterator wraps the source's iterator, holding
//  honest mutable cursor state in fields — a snapshot-capture closure would
//  freeze that state (§7.1). The source is held by the INTERFACE type
//  (IIterable<T> / IIterator<T>), never the class, so `source.iterator()` and
//  the pull calls dispatch to the real concrete subclass (a class-typed
//  receiver would bake in the static declaration — the language's dynamic
//  dispatch is interface-only, P2 finding).
// ===========================================================================

class Seq<T> : IIterable<T> {
    // Abstract: every concrete Seq overrides this. A throwing body (rather than
    // a bodyless declaration, which the IR treats as a native) keeps the class
    // valid; it never runs, since a Seq value is always a concrete subclass and
    // the terminals below pull through `for..in`'s dynamic iterator() dispatch.
    IIterator<T> iterator() { throw RuntimeException("Seq is abstract: a subclass provides iterator()"); }

    // --- combinators (lazy: build a wrapper, pull nothing) ---
    Seq<U> map<U>((T) => U fn) => MapSeq(this, fn);
    Seq<T> where((T) => bool pred) => FilterSeq(this, pred);
    Seq<T> take(int n) => TakeSeq(this, n);
    Seq<T> takeWhile((T) => bool pred) => TakeWhileSeq(this, pred);
    Seq<T> skip(int n) => SkipSeq(this, n);

    // --- terminals (drive the pull) ---
    // Each terminal upcasts `this` to the INTERFACE before pulling: a call
    // through IIterable/IIterator dispatches on the runtime object's concrete
    // subclass, whereas a bare `this.iterator()` would statically bind the
    // abstract base above (the language's dynamic dispatch is interface-only,
    // P2 finding). Driving the iterator by hand (not `for..in`) also keeps these
    // bodies independent of the checker's forInProtocol path-record, which the
    // trusted prelude is not run through.
    Array<T> toArray() {
        IIterable<T> self = this;
        IIterator<T> it = self.iterator();
        Array<T> r = [];
        while (it.hasNext()) r = r.add(it.next());
        return r;
    }
    T? firstOrNone() {
        IIterable<T> self = this;
        IIterator<T> it = self.iterator();
        if (it.hasNext()) return it.next();   // the lazy payoff: pulls exactly one
        return None;
    }
    int count() {
        IIterable<T> self = this;
        IIterator<T> it = self.iterator();
        int n = 0;
        while (it.hasNext()) { it.next(); n = n + 1; }
        return n;
    }
    void forEach((T) => void fn) {
        var f = fn;                          // copy the closure field to a local
        IIterable<T> self = this;
        IIterator<T> it = self.iterator();
        while (it.hasNext()) f(it.next());
    }
    A reduce<A>(A seed, (A, T) => A fn) {
        IIterable<T> self = this;
        IIterator<T> it = self.iterator();
        A acc = seed;
        while (it.hasNext()) acc = fn(acc, it.next());
        return acc;
    }
}

// The bridge in: view a pure Array as a lazy Seq. ArraySeq's iterator is the
// array's own ArrayIterator (a snapshot of a value — never invalidated).
class ArraySeq<T> : Seq<T> {
    Array<T> items;
    new ArraySeq(Array<T> a) { items = a; }
    IIterator<T> iterator() => ArrayIterator(items);
}

class MapSeqIterator<T, U> : IIterator<U> {
    IIterator<T> src;
    (T) => U fn;
    new MapSeqIterator(IIterator<T> s, (T) => U f) { src = s; fn = f; }
    bool hasNext() => src.hasNext();
    U next() { var f = fn; return f(src.next()); }
}
class MapSeq<T, U> : Seq<U> {
    IIterable<T> source;
    (T) => U fn;
    new MapSeq(IIterable<T> src, (T) => U f) { source = src; fn = f; }
    IIterator<U> iterator() => MapSeqIterator(source.iterator(), fn);
}

class FilterSeqIterator<T> : IIterator<T> {
    IIterator<T> src;
    (T) => bool pred;
    bool primed = false;
    bool has = false;
    T val;
    new FilterSeqIterator(IIterator<T> s, (T) => bool p) { src = s; pred = p; }
    // Advance to the next matching element, buffering it once (at-most-once).
    void prime() {
        if (primed) return;
        primed = true;
        has = false;
        var p = pred;
        while (src.hasNext()) {
            T v = src.next();
            if (p(v)) { val = v; has = true; return; }
        }
    }
    bool hasNext() { prime(); return has; }
    T next() { prime(); primed = false; return val; }
}
class FilterSeq<T> : Seq<T> {
    IIterable<T> source;
    (T) => bool pred;
    new FilterSeq(IIterable<T> src, (T) => bool p) { source = src; pred = p; }
    IIterator<T> iterator() => FilterSeqIterator(source.iterator(), pred);
}

class TakeSeqIterator<T> : IIterator<T> {
    IIterator<T> src;
    int remaining;
    new TakeSeqIterator(IIterator<T> s, int n) { src = s; remaining = n; }
    bool hasNext() => remaining > 0 && src.hasNext();
    T next() { remaining = remaining - 1; return src.next(); }
}
class TakeSeq<T> : Seq<T> {
    IIterable<T> source;
    int n;
    new TakeSeq(IIterable<T> src, int cnt) { source = src; n = cnt; }
    IIterator<T> iterator() => TakeSeqIterator(source.iterator(), n);
}

class TakeWhileSeqIterator<T> : IIterator<T> {
    IIterator<T> src;
    (T) => bool pred;
    bool primed = false;
    bool has = false;
    bool done = false;
    T val;
    new TakeWhileSeqIterator(IIterator<T> s, (T) => bool p) { src = s; pred = p; }
    void prime() {
        if (primed || done) return;
        primed = true;
        if (src.hasNext()) {
            T v = src.next();
            var p = pred;
            if (p(v)) { val = v; has = true; }
            else { has = false; done = true; }
        } else { has = false; done = true; }
    }
    bool hasNext() { prime(); return has; }
    T next() { prime(); primed = false; return val; }
}
class TakeWhileSeq<T> : Seq<T> {
    IIterable<T> source;
    (T) => bool pred;
    new TakeWhileSeq(IIterable<T> src, (T) => bool p) { source = src; pred = p; }
    IIterator<T> iterator() => TakeWhileSeqIterator(source.iterator(), pred);
}

class SkipSeqIterator<T> : IIterator<T> {
    IIterator<T> src;
    int toSkip;
    bool skipped = false;
    new SkipSeqIterator(IIterator<T> s, int k) { src = s; toSkip = k; }
    void doSkip() {
        if (skipped) return;
        skipped = true;
        int k = 0;
        while (k < toSkip && src.hasNext()) { src.next(); k = k + 1; }
    }
    bool hasNext() { doSkip(); return src.hasNext(); }
    T next() { doSkip(); return src.next(); }
}
class SkipSeq<T> : Seq<T> {
    IIterable<T> source;
    int n;
    new SkipSeq(IIterable<T> src, int cnt) { source = src; n = cnt; }
    IIterator<T> iterator() => SkipSeqIterator(source.iterator(), n);
}

)prelude";

// --- kPreludeStd: the `std` namespace ------------------------------------
const char* kPreludeStd = R"prelude(
// The standard exception hierarchy. Interfaces define CATCHABILITY (catch by
// contract); classes provide the standard payload. A thrown value must
// implement IException. The `std` namespace is implicitly imported.
namespace std {
    // The system-call floor (§16's bottom shelf): the ONLY native seam for
    // I/O, declared here in the language; C++ stands behind these until the
    // gated Block/raw-memory features land, at which point they collapse to
    // `syscall(nr, ...)` written in the language's own unsafe layer.
    // INTERIM: string-carried until Block exists. sysReadLine returns "" at
    // end of input (a Result-shaped signal replaces this later).
    int sysWrite(int fd, string data);
    string sysReadLine(int fd);
    // The static-side natives.md's `class string { static ... }` sketch
    // anticipated has no plumbing (no `static` keyword/token exists) — this is
    // the fallback the design pre-registered for exactly that gap (Track 04
    // M3, problem #6): a free function. 1-byte string from a byte value
    // 0..255; out of range throws RuntimeException, same as string.byteAt.
    string byteToString(int b);
    // Track 03 §1: char factory (homed as a std:: free function — class
    // static-side fields don't exist). Out-of-range / surrogate -> RuntimeException.
    char charFromCode(int code);

    // LA-20: comptime file inclusion. At compile time the comptime driver
    // intercepts this call (by symbol identity, Eval.cpp callFunction) and
    // returns the declared build input's content as a comptime string. At
    // runtime there is nothing to intercept — a program that reaches this
    // body asked for a compile-time construct at runtime, and runtime
    // failures are loud (§12.6): a real, catchable RuntimeException.
    string import(string path) {
        throw RuntimeException("import() is compile-time-only: call it from comptime context (LA-20)");
    }

    // Track 05 §2: Array<T> has no specialization mechanism, so the numeric
    // aggregates are overloaded free functions (resolution by argument type
    // — suggested-features §6.2), not methods. `min`/`max` return `T?` (None
    // on empty); `sum`/`average` are 0/nan-on-empty via ordinary arithmetic
    // (average's int/float mixed division promotes, matching arithPrim).
    int sum(Array<int> a) {
        int s = 0;
        for (int x in a) s = s + x;
        return s;
    }
    float sum(Array<float> a) {
        float s = 0.0;
        for (float x in a) s = s + x;
        return s;
    }
    int? min(Array<int> a) {
        if (a.isEmpty()) return None;
        int m = a.at(0);
        for (int x in a) if (x < m) m = x;
        return m;
    }
    int? max(Array<int> a) {
        if (a.isEmpty()) return None;
        int m = a.at(0);
        for (int x in a) if (m < x) m = x;
        return m;
    }
    float? min(Array<float> a) {
        if (a.isEmpty()) return None;
        float m = a.at(0);
        for (float x in a) if (x < m) m = x;
        return m;
    }
    float? max(Array<float> a) {
        if (a.isEmpty()) return None;
        float m = a.at(0);
        for (float x in a) if (m < x) m = x;
        return m;
    }
    float average(Array<int> a) {
        float total = 0.0;
        for (int x in a) total = total + x;
        return total / a.length();
    }
    float average(Array<float> a) {
        float total = 0.0;
        for (float x in a) total = total + x;
        return total / a.length();
    }

    int sysOpen(string path, int flags);
    int sysClose(int fd);
    string sysRead(int fd, int max);
    int sysStat(string path, int field);   // 0=exists, 1=size, 2=mtime, 3=isDir
    // The process argument vector, argv[0] first (argv.md). A pure Array<string>
    // materialized fresh per call — an exec-time process fact, not a stream, so
    // it rides the floor like sysStat rather than the event loop. Denied in
    // comptime automatically (the `sys*`-prefix hermeticity gate).
    Array<string> sysArgs();
    // Terminal raw mode (designs/terminal-raw-mode.md). Scalar in/out, the
    // sysStat shape: 0 ok, -1 not-a-tty/fail. sys*-prefixed, so comptime-denied
    // automatically (a build never toggles the build machine's terminal).
    int sysTermRaw(int fd);
    int sysTermRestore(int fd);
    // Terminal floor completion (designs/techdesign-terminal-floor.md). All
    // sys*-prefixed, so comptime-denied automatically (a build never sizes or
    // signal-watches the build machine's terminal). sysWinSize(fd, field):
    // field 0=rows, 1=cols; -1 = failure (the sysStat field-indexed shape).
    // sysTermIsRaw(fd): raw-mode state, the term::size() cursor-report guard.
    // sysSignal* build the signal stream in-language over sysWatch (§3): open a
    // readable fd blocking the given signals, drain one signo per Next call
    // (-1 = none now), close + unblock. SIGKILL/SIGSTOP in the set -> fd -1.
    int  sysWinSize(int fd, int field);
    bool sysTermIsRaw(int fd);
    int  sysSignalOpen(Array<int> sigs);
    int  sysSignalNext(int fd);
    int  sysSignalClose(int fd);
    // Process exit (designs/exit-codes.md §3.2). sysSetExitCode records the code
    // for normal completion; sysExit terminates immediately (typed int but never
    // returns — the language has no Never; documented). Both sys*-prefixed, so
    // comptime-denied automatically — a build cannot terminate the compiler or
    // set a build-time exit code.
    int sysSetExitCode(int code);
    int sysExit(int code);
    int sysTimerStart(int delayMs, int intervalMs, (int) => void cb);
    int sysTimerCancel(int id);
    // Track 08 F2 (contract C6): wall-clock epoch milliseconds. DateTime.now()
    // is its consumer (Track 09). sys*-prefixed, so comptime-denied by the
    // hermeticity gate (a build never reads the wall clock).
    int sysNow();
    // Track 08 F2/F3/F4/F6 floor natives. All sys*-prefixed, so comptime-denied
    // automatically (the hermeticity gate: a build never reads the monotonic
    // clock, entropy, the filesystem shape, a tty, or DNS). Optional returns
    // carry the three-state fact (§9): None is distinct from "" / [].
    int sysMonotonic();                       // CLOCK_MONOTONIC ms (durations)
    string sysRandom(int n);                  // n crypto-random bytes (string-carried); n>1MB throws
    bool sysIsTty(int fd);                     // fd is a terminal
    string? sysEnv(string key);               // None = variable unset
    int sysMkdir(string path);                // 0 ok / -1 fail
    int sysRemove(string path);               // unlink, rmdir on a dir; 0/-1
    int sysRename(string from, string to);    // 0/-1
    Array<string>? sysListDir(string path);   // entry names (no . / ..); None = not a dir
    string? sysResolve(string host);          // first A record (dotted-quad); None = fail

    // A Promise is a ONE-SHOT stream (§14): a value that arrives later. `await`
    // parks on the event loop until `ready`, then yields `value` — so await and
    // stream-pull are the same suspension, two surfaces. No function coloring:
    // await pumps the loop, so any function may await (the engine reads `ready`
    // and `value` directly, as it does for Range/Pair).
    class Promise<T> {
        T value;
        bool ready = false;
        (T) => void cont;
        bool hasCont = false;
        new Promise() { }
        new Promise(T v) { value = v; ready = true; }    // pre-resolved promise
        void resolve(T v) {
            value = v;
            ready = true;
            if (hasCont) { var cb = cont; cb(v); }
        }
        bool isReady() => ready;
        T get() => value;
        void then((T) => void cb) {
            if (ready) { var c = cb; c(value); }
            else { cont = cb; hasCont = true; }
        }
    }

    // Track 10 (techdesign-threads-2, LA-1): a Worker<T> is the handle to a
    // value produced on another execution unit. It IS a Promise<T> — no second
    // suspension surface; `await w` is the join (R-2). The boundary is
    // COPY-ALWAYS (§2 D1'): `spawn` snapshots the body's captures now, so a
    // later mutation of a captured original is unobserved (acceptance #1), and
    // the result is copied back at join, so a counted value lives on exactly one
    // execution unit and the non-atomic refcount fast path is preserved. On the
    // interpreters a worker is a cooperative loop task (a 0-delay timer, run in
    // spawn order — deterministic §7); on LLVM it is a real OS thread (§4).
    //
    // techdesign-threads-3 §3.2: the minimal reject slot. If the body throws,
    // the worker rejects with the exception's message (v1 carrier); `await`
    // rethrows it as a RuntimeException at the join (§3.3). A Worker/Promise
    // handle is NOT transferable across a thread boundary (A-1) — a cross-thread
    // resolve would invoke the stored continuation on the wrong thread, the very
    // race D1' exists to prevent; Channel<T> is the one sanctioned conduit.
    // (failMessage is a plain string, not an IException field: a Promise-typed
    // or optional-generic field crashes default construction — bug.md #1.)
    class Worker<T> : Promise<T> {
        bool failed = false;
        string failMessage = "";
        new Worker() { }
        void reject(IException e) {
            failed = true;
            failMessage = e.message;
            ready = true;              // wakes the awaiter; Await checks failed first
        }
    }

    // The flatten/rebuild floor (§4.4): deep-copy a value across a thread
    // boundary. On the single-heap interpreters it is one deep copy that rejects
    // a value which cannot cross in v1 (nested closure, fd-carrier, Block, or a
    // Worker/Promise handle per A-1) — loud and local (§6). One mechanism serves
    // every crossing: the spawn-body capture (T binds to the body's closure
    // type), the join result, and channel sends.
    T sysThreadTransfer<T>(T value);

    // techdesign-threads-3 §2 — the capability-probe seam. sysThreadStart runs
    // `body` on a real OS thread and returns its join fd where true threads exist
    // (LLVM/POSIX); it returns -1 on the serial engines (no true threads here), so
    // one prelude `spawn` body drives a cooperative task on the interpreters and a
    // pthread on LLVM without an engine-forked prelude. sysThreadResult drains the
    // join fd, rebuilds the worker's result into THIS thread's heap, reaps the
    // thread, and throws the rebuilt exception if the worker failed — it is only
    // ever reached on the true-thread leg (the -1 path never registers a watch).
    int sysThreadStart<T>(() => T body);
    T sysThreadResult<T>(int joinFd);

    // spawn(closure) -> Worker<T> (R-1). The capture snapshot happens now (so a
    // later mutation of a captured original is unobserved, acceptance #1). One
    // body, two execution legs behind the sysThreadStart probe (§3.1): on the
    // serial engines (jfd < 0) the body runs as a cooperative loop task in spawn
    // order; on LLVM it is already running on its pthread and the join-fd watch
    // rebuilds+reaps on THIS loop when it signals. Either leg routes an uncaught
    // body throw to `reject` (the join then rethrows at `await`).
    Worker<T> spawn<T>(() => T body) {
        Worker<T> w = Worker();
        // The true-thread leg flattens the body's captures INSIDE sysThreadStart
        // (before pthread_create), so acceptance #1 holds there without a
        // pre-snapshot — and skipping it matters: a redundant main-heap copy of a
        // captured graph would leak per spawn if the graph has a cycle (§15). The
        // cooperative leg has no such flatten, so it takes the snapshot itself.
        int jfd = std::sysThreadStart(body);          // -1 = no true threads here
        if (jfd < 0) {
            // Cooperative leg (oracle/IR): snapshot the captures NOW (acceptance
            // #1), then run the body as an immediate loop task, spawn-ordered.
            var snap = std::sysThreadTransfer(body);
            std::sysTimerStart(0, 0, (n) => {
                try { w.resolve(std::sysThreadTransfer(snap())); }
                catch (IException e) { w.reject(e); }
            });
        } else {
            // True-thread leg (LLVM): the body is ALREADY running on its pthread;
            // the watch fires on THIS loop when the worker signals, and the
            // result/throw rebuild + reap happen inside sysThreadResult, here.
            std::sysWatch(jfd, (fd) => {
                try { w.resolve(std::sysThreadResult(fd)); }
                catch (IException e) { w.reject(e); }
            });
        }
        return w;
    }

    // === LA-30 B2 (doc 06): cancellation, timeouts, structured concurrency ===
    // The natives are a floor over the doc-1 task substrate's B2 extensions
    // (lv_task.c: cancel mark + shield mask + id registry + N=2 multi-park +
    // join park). Ids — not object handles — cross the native boundary (no ARC
    // entanglement in the registry). All of these require the task scheduler:
    // under LANG_PUMP=1 the parking/spawning trio raises (the pump is a dying
    // escape hatch, deleted at M6) and the id-only pair no-ops.

    // sysTaskRun(body): start `body` as a same-thread group-owned task -> its
    // registry id. sysTaskCancel(id): mark cancelled — delivered as a
    // CancelledException at the task's NEXT park (§2, never preemption);
    // idempotent, 0 for done/unknown ids. sysTaskShield(delta): the §8 shield
    // counter — cancel delivery is masked while >0 (TaskGroup.close()'s unwind
    // guard; internal, not a user surface). sysTaskJoinAll(ids): PARK until
    // every id is done -> count (a rethrow point: CancelledException /
    // drained-loop RuntimeException). sysAwaitAny2(a, b): PARK on both promises
    // -> 0/1 = which settled first (reads no value fields — the value read and
    // failed-rethrow stay in the ordinary `await` issued after it returns).
    int sysTaskRun(() => void body);
    int sysTaskCancel(int id);
    int sysTaskShield(int delta);
    int sysTaskJoinAll(Array<int> ids);
    int sysAwaitAny2<T>(Promise<T> a, Promise<bool> b);

    // TaskGroup (§3): structured concurrency with zero new syntax — it IS a
    // resource, so `using TaskGroup g = TaskGroup();` runs close() on every exit
    // edge (§5.2: fall-off/return/throw/break), cancelling stragglers then
    // joining every child; no orphan task outlives its lexical scope. `run` (a
    // same-thread task: cheap, cancellable) is deliberately NOT `spawn` (a thread:
    // parallel, uncancellable) — the naming carries the semantics and the two must
    // never blur. The group holds ids, not handles.
    class TaskGroup : IDisposable {
        Array<int> live;
        new TaskGroup() { }
        // The wrapper is the §2 absorption rule: an UNCAUGHT CancelledException
        // in a group-owned child is absorbed at the group boundary (cancellation
        // is not a program error); every other uncaught throw stays
        // program-uncaught (C2). A child may catch it itself and refuse — with
        // the visibility §8's close() report gives that choice.
        void run(() => void body) {
            int id = std::sysTaskRun(() => {
                try { body(); }
                catch (ICancelledException e) { }
            });
            live = live.add(id);
        }
        void cancelAll() {
            for (int id in live) std::sysTaskCancel(id);
        }
        // close() = cancelAll, then join everything. Shielded (§8): a task
        // closing a group while itself being cancelled must not have the
        // delivery abort its join — the mark holds and fires at its next
        // UNshielded park, so `using` + cancel cannot livelock. No `finally`
        // exists (reference §5.2's stance), hence the catch-restore-rethrow.
        void close() {
            std::sysTaskShield(1);
            cancelAll();
            try { std::sysTaskJoinAll(live); }
            catch (IException e) {
                std::sysTaskShield(0 - 1);
                live = [];
                throw e;
            }
            std::sysTaskShield(0 - 1);
            live = [];
        }
    }

    // awaitTimeout (§3): park on {work, timer}; the timeout arm is an ordinary
    // loop timer resolving an ordinary Promise (no raw timer->park edge, so no
    // stale-wake hazard: an abandoned arm just resolves a dead promise). On
    // timeout -> None (expected absence via the union rule — an outcome, not a
    // failure). The winner path re-awaits `work`: it is ready, so the await is
    // the no-yield fast path AND the one place the value read + Worker-failed
    // rethrow live (G15/S2 discipline). v1 narrowing (documented): timeout
    // stops the WAIT; it cancels nothing — no promise->producer-task mapping
    // exists, so compose `awaitTimeout(p, ms)` + `g.cancelAll()` for the
    // kernel's kill-switch shape (§7). A `Worker` is never cancelled by
    // timeout either way (threads cannot be wall-clock killed — kernel §7).
    T? awaitTimeout<T>(Promise<T> work, int ms) {
        if (work.isReady()) {
            T ready = await work;
            return ready;
        }
        Promise<bool> timer = Promise();
        int tid = std::sysTimerStart(ms, 0, (n) => { timer.resolve(true); });
        int who = 0;
        try { who = std::sysAwaitAny2(work, timer); }
        catch (IException e) {
            // cancellation (or a drained loop) landed at OUR park: retire the
            // timer arm so it cannot keep the loop alive, then rethrow.
            std::sysTimerCancel(tid);
            throw e;
        }
        if (who == 0) {
            std::sysTimerCancel(tid);
            T v = await work;
            return v;
        }
        return None;
    }

    // OverflowPolicy (§3.3/§13): the forced pairing — a channel capacity comes
    // WITH a policy for a full buffer. `block` = backpressure (park the
    // producer), `drop` = discard the new item, `error` = throw at send. `grow`
    // is deliberately absent (F-6: a lock-free ring cannot resize under a
    // concurrent peer without a locked path — a silent-distant cliff). Carried as
    // a small int (0/1/2) with these named accessors — the `OverflowPolicy` enum
    // surface the design names lands once prelude enums desugar (Track 03 enum
    // desugaring currently runs on the user program only).
    int overflowBlock() => 0;
    int overflowDrop()  => 1;
    int overflowError() => 2;

    // The Channel native ring (techdesign-threads-3 §6) — the LLVM leg behind the
    // same probe as spawn. sysChannelNew returns a channel id (>= 0) where a real
    // lock-free ring + two eventfds exist (LLVM/POSIX) and -1 where they don't
    // (the interpreters, which run the in-process queue). send/receive/close then
    // delegate to the process-global record over the id; the Channel object
    // carries only scalars, so a captured handle relocates by plain field copy —
    // both rebuilt handles name the same record (portal semantics, §3.4).
    int sysChannelNew(int capacity, int policy);
    int sysChannelSend<T>(int id, T value);        // may throw (closed / overflow)
    T? sysChannelReceive<T>(int id);               // None = closed AND drained (F-8)
    int sysChannelClose(int id);

    // Channel<T> (R-4): a single-producer / single-consumer conduit between
    // execution units. Behind the id>=0 probe (§3.4) it is the native lock-free
    // ptr-slot ring + two eventfds (LLVM); on the serial engines (id<0) it is an
    // in-process queue on the one loop — `send` copies the value in (flatten §4.4)
    // and either hands it straight to a parked receiver or enqueues it; `receive()`
    // is a Promise<T?> resolving with the next item, or None once the channel is
    // closed AND drained (F-8, the sysRecv `string?` precedent — the receiver
    // narrows with `!= None`). A captured Channel handle is a portal (§6): both
    // units feed the SAME endpoint. On the serial engines a running task cannot
    // park mid-body, so `block` degrades to enqueue (unbounded) while `drop`/
    // `error` still enforce capacity at send.
    class Channel<T> {
        Array<T> buf;
        Array<Promise> waiters;      // raw Promise: a Promise<T?> field crashes
                                     // default-construction (nested-optional bug)
        bool closed = false;
        int cap = 0;
        int policy = 0;
        int id = 0 - 1;              // >= 0 = native ring (LLVM); -1 = in-process

        new Channel(int capacity, int p) {
            cap = capacity; policy = p;
            id = std::sysChannelNew(capacity, p);
        }

        void send(T value) {
            if (id >= 0) { std::sysChannelSend(id, value); return; }   // native ring
            if (closed) { throw RuntimeException("send on a closed channel"); }
            var v = std::sysThreadTransfer(value);      // copy-in (isolation)
            if (waiters.length() > 0) {                 // hand straight to a parked receiver
                Promise w = waiters.first();
                waiters = waiters.skip(1);
                w.resolve(v);
                return;
            }
            if (cap > 0 && buf.length() >= cap) {
                if (policy == 1) { return; }                                   // drop
                if (policy == 2) { throw RuntimeException("channel overflow"); }  // error
            }
            buf = buf.add(v);
        }

        Promise<T?> receive() {
            Promise<T?> p = Promise();
            if (id >= 0) {                              // native ring: park+dequeue in the native
                p.resolve(std::sysChannelReceive(id));
                return p;
            }
            if (buf.length() > 0) {                     // ready item
                T v = buf.first();
                buf = buf.skip(1);
                p.resolve(v);
            } else if (closed) {                        // closed AND drained -> None (F-8)
                p.resolve(None);
            } else {
                waiters = waiters.add(p);               // park until a send or close
            }
            return p;
        }

        void close() {
            if (id >= 0) { std::sysChannelClose(id); return; }
            closed = true;
            for (Promise w in waiters) { w.resolve(None); }
            waiters = waiters.skip(waiters.length());
        }
    }

    // A timer is a SYSTEM STREAM (§13): the loop pushes tick numbers into a
    // real StreamBuffer, so subscribe/pull are the ordinary stream machinery.
    // The program keeps running while timers are live; cancel() releases the
    // work (one-shots release themselves after firing).
    class Timer {
        int id = 0 - 1;
        StreamBuffer<int> buf = StreamBuffer();
        new Timer(int delayMs, int intervalMs) {
            OutStream<int> w = OutStream(buf);
            id = std::sysTimerStart(delayMs, intervalMs, (n) => w << n);
        }
        InStream<int> ticks() => InStream(buf);
        void subscribe((int) => void cb) {
            InStream<int> r = InStream(buf);
            r.subscribe(cb);
        }
        void cancel() { std::sysTimerCancel(id); }
    }
    Timer every(int ms) => Timer(ms, ms);
    Timer after(int ms) => Timer(ms, 0);

    // --- sockets: the network is just more streams (§13) ---------------------
    int sysTcpConnect(string host, int port);   // -1 on failure; ':' in host = IPv6
    int sysTcpListen(int port);                  // socket+bind+listen, -1 on fail
    int sysTcpListen(int port, bool reusePort);  // sets SO_REUSEPORT before bind
    int sysCpuCount();                           // online logical processors (>= 1)
    int cpuCount() => sysCpuCount();
    int sysAccept(int fd);                       // -1 if none pending
    // sysSend: bytes written; -1 retryable (would-block), -2 fatal (peer gone).
    // Works on pipes too (F7: a Process stdin takes the same call).
    int sysSend(int fd, string data);
    string? sysRecv(int fd, int max);            // None = peer closed; "" = none now
    // Track 03 M4 — zero-copy Block I/O overloads. Arity-distinct from the
    // string forms (overload resolution by argument type selects). Read and
    // recv fill the Block from offset 0; write and send take an explicit
    // [off, off+len) window. Every off/len is bounds-checked (throws).
    int  sysRead(int fd, Block b, int max);           // bytes read into b; 0 at EOF
    int  sysWrite(int fd, Block b, int off, int len); // bytes written from b[off,off+len)
    int  sysSend(int fd, Block b, int off, int len);  // like sysSend(string): -1/-2 split
    int? sysRecv(int fd, Block b, int max);           // None = peer closed; 0 = none now
    int sysWatch(int fd, (int) => void cb);      // fire cb when fd is read-ready
    int sysUnwatch(int id);                      // cancels read AND write watches
    // Track 08 F5: the connect-timeout floor (all three composed in-language by
    // connectTimeout below). sys*-prefixed => comptime-denied automatically.
    int sysTcpConnectNb(string host, int port);  // fd even mid-connect; -1 = immediate fail
    int sysWatchWrite(int fd, (int) => void cb); // fire cb when fd is write-ready
    int sysConnectResult(int fd);                // SO_ERROR: 0 connected, else errno-shaped
    // LA-29 (techdesign-socket-options.md): advisory send/recv buffer sizing.
    // Names the INTENT (bytes per direction); the OS mapping (SO_SNDBUF/SO_RCVBUF)
    // is hidden in the floor — the same shape as sysTcpListen's reusePort bool
    // hiding SO_REUSEPORT. A NON-POSITIVE value leaves that direction UNCHANGED.
    // Returns 0 (every requested direction applied) | -1 (a requested setsockopt
    // failed). BEST-EFFORT: the kernel clamps (Linux doubles + enforces a floor;
    // Windows/BSD differ) — a caller must not assume an exact size. A permanent
    // systems capability (flow-control tuning any real stack needs), not a test
    // hook. sys*-prefixed => comptime-denied automatically.
    int sysSocketBuffer(int fd, int sendBytes, int recvBytes);
    // --- TLS + crypto floor (LA-2, techdesign-tls-crypto.md §3) ------------
    // The fd IS the socket fd (wrap-in-place): arm TLS over a connected (client)
    // or accepted (server) socket, then sysSend/sysRecv/sysClose route through
    // the session transparently and sysWatch* keep polling the raw fd. All
    // sys*-prefixed => comptime-denied automatically. Full-arity natives plus
    // thin prelude wrapper overloads carry the ticket's short call shapes (no
    // reliance on native defaulted params — the surface is identical either way).
    //   verifyMode: 0 full (chain + hostname, HttpClient's only mode) | 1 chain
    //   only | 2 encrypt-only (no verification; greppable, never a default).
    //   caFile "" = system roots (+ SSL_CERT_FILE/DIR env); else an ADDITIONAL
    //   trust anchor. alpn "" = no ALPN; else "h2,http/1.1".
    int sysTlsConnect(int fd, string host, string alpn, string caFile, int verifyMode);
    int sysTlsConnect(int fd, string host, string alpn) => sysTlsConnect(fd, host, alpn, "", 0);
    int sysTlsConnect(int fd, string host)             => sysTlsConnect(fd, host, "", "", 0);
    int sysTlsAccept(int fd, string certPath, string keyPath, string alpn);
    int sysTlsAccept(int fd, string certPath, string keyPath) => sysTlsAccept(fd, certPath, keyPath, "");
    int sysTlsHandshake(int fd);                 // one step: 0 done | 1 want-read | 2 want-write | -1
    string sysTlsError(int fd);                  // "" | latest error detail for the fd
    string sysTlsAlpn(int fd);                   // negotiated ALPN protocol | ""
    int sysTlsVersion(int fd);                   // 12 | 13 | 0 (no session / not established)
    // R-3: RSA public-key encrypt for auth handshakes (key transport, not a
    // general crypto surface). None on parse/capacity/encrypt failure. padding
    // "oaep" (default, caching_sha2_password) | "pkcs1" (legacy sha256_password).
    string? sysRsaEncrypt(string pubKeyPem, string bytes, string padding);
    string? sysRsaEncrypt(string pubKeyPem, string bytes) => sysRsaEncrypt(pubKeyPem, bytes, "oaep");
    // Track 08 F7: spawn floor. [pid, stdinFd, stdoutFd, stderrFd], [] = spawn
    // failure (exec failure instead arrives as exit code 127 via sysReap).
    Array<int> sysSpawn(string path, Array<string> args);
    int sysPidfdOpen(int pid);                   // pollable fd, ready when pid exits
    int sysReap(int pid);                        // -1 running; else code (128+sig if signaled)
    int sysKill(int pid, int sig);               // 0/-1; pid <= 0 refused
    // G-LANG-2 terminal half (designs/pty/): [pid, masterFd] | [] on failure.
    // One fd, read+write — a pty fuses stdout/stderr. rows/cols <= 0 refused.
    // flags bit0 = deterministic termios (goldens). Resize: kernel SIGWINCHes
    // the child; 0/-1.
    Array<int> sysPtySpawn(string path, Array<string> args, int rows, int cols, int flags);
    int sysPtyResize(int masterFd, int rows, int cols);

    // A connected socket, wearing the stream surface. Reads are event-driven:
    // onData subscribes a read-watch that recvs and delivers chunks; None from
    // the kernel means the peer closed, which fires onClose and releases the
    // watch (so the loop can exit). Writes are synchronous (fine for the sizes
    // a request/response carries; Block + backpressure refine this later).
    class TcpStream {
        int fd = 0 - 1;
        int watchId = 0 - 1;
        (string) => void onChunk;
        () => void onClosed;
        bool watching = false;
        bool hasCloseCb = false;
        string pending = "";
        bool draining = false;
        int drainId = 0 - 1;

        new TcpStream(int f) { fd = f; }
        int rawFd() => fd;                   // the underlying socket fd (TLS wrap-in-place)

        // Writes and close are guarded on fd >= 0: after close() the fd
        // NUMBER may be recycled by the OS for an unrelated connection
        // (POSIX hands out the lowest free descriptor), so a write through
        // a stale handle would corrupt someone else's stream. Found via the
        // lcurl fixture: a leftover per-connection timer wrote its payload
        // into the NEXT client's connection. Stale operations are no-ops.
        TcpStream (<<)(string s) {
            send(s);
            return this;
        }
        // Queue-and-drain (Track 08 F5.6, closing curl-design §2.1): the fd is
        // non-blocking, so a large payload short-writes. The unsent tail is
        // buffered and a write-watch drains it as the kernel makes room. A
        // fatal send (-2: peer gone) drops the buffer — the read side observes
        // the close and delivers it; writes never throw.
        void send(string s) {
            if (fd < 0) return;
            if (draining) { pending = pending + s; return; }
            int n = std::sysSend(fd, s);
            if (n == 0 - 2) return;
            if (n < 0) n = 0;
            if (n < s.length()) {
                pending = s.subStr(n, s.length() - n);
                draining = true;
                TcpStream self = this;
                drainId = std::sysWatchWrite(fd, (ready) => self.drain());
            }
        }
        void drain() {
            if (fd < 0) return;
            int n = std::sysSend(fd, pending);
            if (n == 0 - 2) { pending = ""; stopDrain(); return; }
            if (n < 0) return;                   // spurious wake; keep watching
            if (n >= pending.length()) { pending = ""; stopDrain(); }
            else pending = pending.subStr(n, pending.length() - n);
        }
        void stopDrain() {
            if (draining) { std::sysUnwatch(drainId); draining = false; drainId = 0 - 1; }
        }

        void onData((string) => void cb) {
            onChunk = cb;
            watching = true;
            // Capture `this` into a local so the loop callback (a lambda) reaches
            // the object via an ordinary captured variable — not the implicit
            // receiver, which a lambda does not close over.
            TcpStream self = this;
            watchId = std::sysWatch(fd, (ready) => self.pump());
        }
        void onClose(() => void cb) { onClosed = cb; hasCloseCb = true; }

        // Invoked by the loop when the socket is read-ready.
        void pump() {
            if (fd < 0) return;                  // stale watch after close()
            string? chunk = std::sysRecv(fd, 4096);
            if (chunk == None) {                 // peer closed -> deliver close
                close();
                if (hasCloseCb) { var cb = onClosed; cb(); }
            } else if (chunk != None) {
                var cb = onChunk;
                cb(chunk);                       // narrowed to string
            }
        }
        // Drain everything already buffered on the descriptor NOW: pump until
        // EOF (delivered as the usual close) or would-block. Process (F7) calls
        // this when the child's exit races output still sitting in the pipe —
        // the pidfd can poll ready before the loop has pumped the last chunks.
        void pumpAll() {
            if (!watching) return;               // no consumer: nothing to deliver
            bool more = true;
            while (more) {
                if (fd < 0) { more = false; }
                else {
                    string? chunk = std::sysRecv(fd, 4096);
                    if (chunk == None) {
                        close();
                        if (hasCloseCb) { var ccb = onClosed; ccb(); }
                        more = false;
                    } else {
                        string got = chunk ?? "";
                        if (got.isEmpty()) { more = false; }
                        else { var cb = onChunk; cb(got); }
                    }
                }
            }
        }
        // Idempotent: the fd is invalidated so a second close() (or a write
        // through a retained handle) can never hit a recycled descriptor.
        void close() {
            if (fd < 0) return;
            stopDrain();
            if (watching) { std::sysUnwatch(watchId); watching = false; }
            std::sysClose(fd);
            fd = 0 - 1;
        }
    }

    // A listening socket presented as a STREAM OF CONNECTIONS: subscribing
    // registers an accept-watch that pushes a TcpStream per incoming client.
    class TcpListener {
        int fd = 0 - 1;
        int watchId = 0 - 1;
        bool listening = false;
        (TcpStream) => void onConn;
        new TcpListener(int port) { fd = std::sysTcpListen(port); }
        bool ok() => fd >= 0;

        void connections((TcpStream) => void cb) {
            onConn = cb;
            listening = true;
            TcpListener self = this;
            watchId = std::sysWatch(fd, (ready) => self.acceptAll());
        }
        void acceptAll() {
            var cb = onConn;
            int cfd = std::sysAccept(fd);
            while (cfd >= 0) {                    // drain the accept backlog
                cb(TcpStream(cfd));
                cfd = std::sysAccept(fd);
            }
        }
        // Same stale-fd discipline as TcpStream.close(): idempotent, and the
        // fd is invalidated so a retained handle can't touch a recycled one.
        void stop() {
            if (fd < 0) return;
            if (listening) { std::sysUnwatch(watchId); listening = false; }
            std::sysClose(fd);
            fd = 0 - 1;
        }
    }

    // --- connect with a deadline (Track 08 F5.4) ------------------------------
    // The in-language composition the non-blocking floor exists for: nb-connect
    // + write-watch + SO_ERROR verdict + a one-shot Timer. cb receives the
    // connected fd (non-blocking, ready for TcpStream), or -1 — refused,
    // unreachable, bad literal, or deadline hit; the fd is closed on every
    // failure path. One-shot state machine: whichever of writable/expire fires
    // first settles it, the loser sees `done` and does nothing.
    class ConnectAttempt {
        int fd = 0 - 1;
        int watchId = 0 - 1;
        int timerId = 0 - 1;
        bool done = false;
        (int) => void onDone;

        void begin(string host, int port, int ms, (int) => void cb) {
            onDone = cb;
            fd = std::sysTcpConnectNb(host, port);
            if (fd < 0) { settle(0 - 1); return; }
            ConnectAttempt self = this;
            watchId = std::sysWatchWrite(fd, (ready) => self.writable());
            timerId = std::sysTimerStart(ms, 0, (n) => self.expire());
        }
        void writable() {
            if (done) return;
            int e = std::sysConnectResult(fd);
            if (e == 0) { settle(fd); }
            else { std::sysClose(fd); settle(0 - 1); }
        }
        void expire() {
            if (done) return;
            std::sysClose(fd);
            settle(0 - 1);
        }
        void settle(int result) {
            done = true;
            if (watchId >= 0) { std::sysUnwatch(watchId); watchId = 0 - 1; }
            if (timerId >= 0) { std::sysTimerCancel(timerId); timerId = 0 - 1; }
            var cb = onDone;
            cb(result);
        }
    }
    void connectTimeout(string host, int port, int ms, (int) => void cb) {
        ConnectAttempt a = ConnectAttempt();
        a.begin(host, port, ms, cb);
    }

    // --- TLS handshake drives (LA-2, techdesign-tls-crypto.md §6.1/6.2) --------
    // The lazy-at-the-floor handshake driven LOUDLY above: sysTlsConnect/Accept
    // arm the session and step once; these drive it to completion. One-shot
    // state machine mirroring ConnectAttempt — step the handshake, and on
    // want-read/want-write (re)arm a watch in that direction, on done call
    // cb(fd), on failure call cb(-1) (sysTlsError already carries the reason).
    // Every path unwatches. No T? narrowing in prelude bodies — int fd / -1
    // sentinels only. The §2.3 poll augmentation lets the steady-state TcpStream
    // watches keep progressing across a TLS 1.3 KeyUpdate's direction flip; this
    // drive arms the exact direction each handshake step needs.
    class TlsDrive {
        int fd = 0 - 1;
        int watchId = 0 - 1;
        bool watching = false;
        bool done = false;
        (int) => void onDone;

        void begin(int f, (int) => void cb) {
            fd = f;
            onDone = cb;
            step();
        }
        void step() {
            if (done) return;
            int r = std::sysTlsHandshake(fd);
            if (r == 0) { settle(fd); return; }         // handshake complete
            if (r < 0) { settle(0 - 1); return; }       // failure (sysTlsError set)
            stopWatch();                                // switch direction cleanly
            TlsDrive self = this;
            if (r == 1) { watchId = std::sysWatch(fd, (ready) => self.step()); watching = true; }
            else        { watchId = std::sysWatchWrite(fd, (ready) => self.step()); watching = true; }
        }
        void settle(int result) {
            done = true;
            stopWatch();
            var cb = onDone;
            cb(result);
        }
        void stopWatch() {
            if (watching) { std::sysUnwatch(watchId); watching = false; watchId = 0 - 1; }
        }
    }
    // Drive an already-armed fd's handshake; cb(fd) on success, cb(-1) on failure.
    void tlsDrive(int fd, (int) => void cb) {
        TlsDrive d = TlsDrive();
        d.begin(fd, cb);
    }
    // Client: arm + drive + LOUD failure. Throws RuntimeException("TLS handshake:
    // <reason>") on setup or handshake failure (cert verify, hostname mismatch —
    // verify BEFORE the first byte of protocol, §2.2). cb(fd) on success.
    void tlsConnect(int fd, string host, string alpn, string caFile,
                    int verifyMode, (int) => void cb) {
        int armed = std::sysTlsConnect(fd, host, alpn, caFile, verifyMode);
        if (armed < 0) {
            throw RuntimeException("TLS handshake: " + std::sysTlsError(fd) +
                                   " (host '" + host + "', fd " + fd + ")");
        }
        var c = cb;
        string h = host;
        std::tlsDrive(fd, (result) => {
            if (result < 0) {
                throw RuntimeException("TLS handshake: " + std::sysTlsError(fd) +
                                       " (host '" + h + "', fd " + fd + ")");
            }
            c(result);
        });
    }
    // Server: arm + drive + a one-shot deadline (slowloris posture, §4 #10).
    // Expiry or failure closes the fd and calls cb(-1) — server policy is DROP,
    // never throw: one bad client must not unwind the accept loop.
    class TlsAccept {
        int fd = 0 - 1;
        int watchId = 0 - 1;
        int timerId = 0 - 1;
        bool watching = false;
        bool done = false;
        (int) => void onDone;

        void begin(int f, string cert, string key, string alpn, int deadlineMs, (int) => void cb) {
            fd = f;
            onDone = cb;
            int armed = std::sysTlsAccept(fd, cert, key, alpn);
            if (armed < 0) { settle(0 - 1); return; }
            TlsAccept self = this;
            timerId = std::sysTimerStart(deadlineMs, 0, (n) => self.expire());
            step();
        }
        void step() {
            if (done) return;
            int r = std::sysTlsHandshake(fd);
            if (r == 0) { settle(fd); return; }
            if (r < 0) { settle(0 - 1); return; }
            stopWatch();
            TlsAccept self = this;
            if (r == 1) { watchId = std::sysWatch(fd, (ready) => self.step()); watching = true; }
            else        { watchId = std::sysWatchWrite(fd, (ready) => self.step()); watching = true; }
        }
        void expire() {
            if (done) return;
            settle(0 - 1);
        }
        void settle(int result) {
            done = true;
            stopWatch();
            if (timerId >= 0) { std::sysTimerCancel(timerId); timerId = 0 - 1; }
            if (result < 0 && fd >= 0) { std::sysClose(fd); }   // drop, don't throw
            var cb = onDone;
            cb(result);
        }
        void stopWatch() {
            if (watching) { std::sysUnwatch(watchId); watching = false; watchId = 0 - 1; }
        }
    }
    void tlsAccept(int fd, string cert, string key, string alpn, int deadlineMs, (int) => void cb) {
        TlsAccept a = TlsAccept();
        a.begin(fd, cert, key, alpn, deadlineMs, cb);
    }

    // --- Process: a child process wearing the stream surface (Track 08 F7) ----
    // Thin in-language wrapper over the spawn floor. The three pipes are
    // TcpStreams (pipes are fds; sysSend/sysRecv take both), so stdin writes
    // queue-and-drain and stdout/stderr ride the ordinary read-watch loop.
    // Reaping is the design's no-SIGCHLD route: exitCode() opens a pidfd,
    // watches it like any other fd, and on readiness collects the code with a
    // non-blocking waitid — resolving the Promise and closing every owned fd
    // (problem #4's churn guarantee). A program that never calls exitCode()
    // keeps no reap machinery alive and may exit with children live
    // (documented: standard Unix, no magic — they are not reaped).
    class Process {
        int pid = 0 - 1;
        int pidfd = 0 - 1;
        int reapWatchId = 0 - 1;
        int reapTimerId = 0 - 1;
        bool reaping = false;
        bool exited = false;
        TcpStream stdinS = TcpStream(0 - 1);
        TcpStream stdoutS = TcpStream(0 - 1);
        TcpStream stderrS = TcpStream(0 - 1);
        Promise<int> exitP = Promise();

        new Process(string path, Array<string> args) {
            Array<int> r = std::sysSpawn(path, args);
            if (r.length() == 4) {
                pid = r.at(0);
                stdinS = TcpStream(r.at(1));
                stdoutS = TcpStream(r.at(2));
                stderrS = TcpStream(r.at(3));
            }
        }
        bool ok() => pid > 0;

        void write(string s) { stdinS.send(s); }
        // /bin/cat-style children read until stdin EOF: closing our end IS the
        // protocol. Idempotent (TcpStream discipline).
        void closeStdin() { stdinS.close(); }
        void onStdout((string) => void cb) { stdoutS.onData(cb); }
        void onStderr((string) => void cb) { stderrS.onData(cb); }
        void kill() {
            if (pid > 0 && !exited) {
                std::sysKill(pid, 15);
                stdinS.close();          // SIGTERM'd child reads no more
            }
        }

        Promise<int> exitCode() {
            if (exited || reaping) return exitP;
            if (pid <= 0) {              // spawn failure: the 127 convention
                exited = true;           // (problem #3 — "could not run")
                exitP.resolve(127);
                return exitP;
            }
            reaping = true;
            pidfd = std::sysPidfdOpen(pid);
            Process self = this;
            if (pidfd >= 0) {
                reapWatchId = std::sysWatch(pidfd, (ready) => self.tryReap());
            } else {
                // pidfd unavailable (exotic kernel): coarse poll-reap fallback.
                // Still no signals, still non-blocking.
                reapTimerId = std::sysTimerStart(20, 20, (n) => self.tryReap());
            }
            return exitP;
        }
        void tryReap() {
            if (exited) return;
            int code = std::sysReap(pid);
            if (code < 0) return;        // spurious wake; child still running
            exited = true;
            if (reapWatchId >= 0) { std::sysUnwatch(reapWatchId); reapWatchId = 0 - 1; }
            if (reapTimerId >= 0) { std::sysTimerCancel(reapTimerId); reapTimerId = 0 - 1; }
            if (pidfd >= 0) { std::sysClose(pidfd); pidfd = 0 - 1; }
            stdinS.close();
            // The child's last output can race the pidfd readiness: deliver
            // whatever is still in the pipes BEFORE closing them, then resolve.
            stdoutS.pumpAll();
            stderrS.pumpAll();
            stdoutS.close();
            stderrS.close();
            exitP.resolve(code);
        }
    }

    // A child on a pseudo-terminal (designs/pty/ D-P1): ONE merged byte stream —
    // there is no separate stderr on a pty. The master fd rides TcpStream (send
    // queue-and-drain, read-watch delivery, D-P9); exit rides the pidfd-watch/
    // poll-reap pair exactly like Process. EOF protocol: a pty has no closable
    // write half — canonical mode's VEOF does it: write("\x04").
    // Retirement (D-P5): reap success -> pumpAll -> close -> resolve; a master
    // close alone (child side gone) fires onClose but never resolves exitCode.
    class Pty {
        int pid = 0 - 1;
        int pidfd = 0 - 1;
        int reapWatchId = 0 - 1;
        int reapTimerId = 0 - 1;
        bool reaping = false;
        bool exited = false;
        TcpStream io = TcpStream(0 - 1);
        Promise<int> exitP = Promise();

        new Pty(string path, Array<string> args, int rows, int cols) {
            Array<int> r = std::sysPtySpawn(path, args, rows, cols, 0);
            if (r.length() == 2) { pid = r.at(0); io = TcpStream(r.at(1)); }
        }
        // The frozen deterministic termios profile (echo family off) — goldens.
        new Deterministic(string path, Array<string> args, int rows, int cols) {
            Array<int> r = std::sysPtySpawn(path, args, rows, cols, 1);
            if (r.length() == 2) { pid = r.at(0); io = TcpStream(r.at(1)); }
        }
        bool ok() => pid > 0;

        void write(string s) { io.send(s); }
        void onData((string) => void cb) { io.onData(cb); }
        void onClose(() => void cb) { io.onClose(cb); }
        int resize(int rows, int cols) {
            if (io.rawFd() < 0) return 0 - 1;            // stale-fd guard (TcpStream discipline)
            return std::sysPtyResize(io.rawFd(), rows, cols);
        }
        void kill() {
            if (pid > 0 && !exited) { std::sysKill(pid, 15); }
            // do NOT close io here: D-P5 — the dying child's last output is still
            // in the line discipline; tryReap drains it.
        }

        Promise<int> exitCode() {
            if (exited || reaping) return exitP;
            if (pid <= 0) {                              // spawn failure: 127 (F7 convention)
                exited = true;
                exitP.resolve(127);
                return exitP;
            }
            reaping = true;
            pidfd = std::sysPidfdOpen(pid);
            Pty self = this;
            if (pidfd >= 0) {
                reapWatchId = std::sysWatch(pidfd, (ready) => self.tryReap());
            } else {
                // pidfd unavailable (exotic kernel; Windows, doc 03): poll-reap.
                reapTimerId = std::sysTimerStart(20, 20, (n) => self.tryReap());
            }
            return exitP;
        }
        void tryReap() {
            if (exited) return;
            int code = std::sysReap(pid);
            if (code < 0) return;                        // spurious wake
            exited = true;
            if (reapWatchId >= 0) { std::sysUnwatch(reapWatchId); reapWatchId = 0 - 1; }
            if (reapTimerId >= 0) { std::sysTimerCancel(reapTimerId); reapTimerId = 0 - 1; }
            if (pidfd >= 0) { std::sysClose(pidfd); pidfd = 0 - 1; }
            io.pumpAll();                                // drain BEFORE close (D-P5, pitfall #11)
            io.close();
            exitP.resolve(code);
        }
    }

    // --- HTTP: a Layer-2 reshaping over TCP streams, entirely in-language -----
    // Framework-grade (Track 09 F4): ordered case-insensitive HeaderMap, chunked
    // transfer both directions, server-side keep-alive + a 500 error path, and a
    // real client. All in-language over TcpStream; still text bodies until Block.

    // One header line. A struct (data, no identity) — order + duplicates are
    // preserved by HeaderMap, so Set-Cookie survives.
    struct Header {
        string name = "";
        string value = "";
    }
    // An ordered multimap with case-insensitive name matching (RFC 7230 §3.2).
    // Backed by Array<Header> so entries() keeps insertion order + duplicates.
    // Internal lookups use firstOr/allOf (plain values, no int?/string? crossing
    // a prelude boundary — the static backends misread narrowed unions there).
    class HeaderMap {
        Array<Header> items = [];
        HeaderMap add(string name, string value) {
            Header h;
            h.name = name;
            h.value = value;
            items = items.add(h);
            return this;
        }
        // replace every existing entry of `name`, then append once.
        HeaderMap set(string name, string value) {
            string ln = name.toLower();
            Array<Header> next = [];
            for (Header h in items) { if (h.name.toLower() != ln) next = next.add(h); }
            Header nh;
            nh.name = name;
            nh.value = value;
            next = next.add(nh);
            items = next;
            return this;
        }
        bool has(string name) {
            string ln = name.toLower();
            bool got = false;
            for (Header h in items) { if (h.name.toLower() == ln) got = true; }
            return got;
        }
        // first value for `name`, or `dflt` — the narrowing-free internal form.
        string firstOr(string name, string dflt) {
            string ln = name.toLower();
            string found = dflt;
            bool got = false;
            for (Header h in items) {
                if (!got && h.name.toLower() == ln) { found = h.value; got = true; }
            }
            return found;
        }
        string? first(string name) => this.has(name) ? this.firstOr(name, "") : None;
        Array<string> all(string name) {
            string ln = name.toLower();
            Array<string> r = [];
            for (Header h in items) { if (h.name.toLower() == ln) r = r.add(h.value); }
            return r;
        }
        HeaderMap remove(string name) {
            string ln = name.toLower();
            Array<Header> next = [];
            for (Header h in items) { if (h.name.toLower() != ln) next = next.add(h); }
            items = next;
            return this;
        }
        Array<Header> entries() => items;
        int length() => items.length();
        // "Name: value\r\n" per entry (no trailing blank line). Plain `+`
        // concatenation, NOT StringBuilder: StringBuilder.toString() lowers to
        // the post-freeze `concatAll` native, absent on the frozen ELF backend
        // that the corpus/elf HTTP programs still target.
        string render() {
            string r = "";
            for (Header h in items) { r = r + h.name + ": " + h.value + "\r\n"; }
            return r;
        }
    }

    // Chunked transfer decoder (RFC 7230 §4.1), ported from lcurl's
    // fragmentation-proof state machine (curl-design §5.6): feed arbitrary byte
    // fragments, get back the dechunked body incrementally. `bad` on a malformed
    // size line; `isDone` after the terminating 0-chunk.
    class ChunkedDecoder {
        string buf = "";
        int state = 0;     // 0 size line, 1 data, 2 trailing CRLF, 3 trailer, 4 done
        int need = 0;
        bool isDone = false;
        bool bad = false;
        string feed(string chunk) {
            buf = buf + chunk;
            string outp = "";
            bool progress = true;
            while (progress) {
                progress = false;
                if (state == 0) {
                    int nl = buf.indexOf("\r\n");
                    if (nl >= 0) {
                        string sizeLine = buf.subStr(0, nl);
                        buf = buf.subStr(nl + 2, buf.length() - (nl + 2));
                        int semi = sizeLine.indexOf(";");
                        string sizeHex = (semi >= 0 ? sizeLine.subStr(0, semi) : sizeLine).trim();
                        int size = 0;
                        bool ok = sizeHex.length() > 0;
                        int k = 0;
                        while (k < sizeHex.length()) {
                            int d = encoding::hexNibble(sizeHex.byteAt(k));
                            if (d < 0) ok = false;
                            else size = size * 16 + d;
                            k = k + 1;
                        }
                        if (!ok) { bad = true; isDone = true; return outp; }
                        if (size == 0) state = 3; else { need = size; state = 1; }
                        progress = true;
                    }
                } else if (state == 1) {
                    if (buf.length() > 0) {
                        int take = need < buf.length() ? need : buf.length();
                        if (take > 0) {
                            outp = outp + buf.subStr(0, take);
                            buf = buf.subStr(take, buf.length() - take);
                            need = need - take;
                            progress = true;
                        }
                        if (need == 0) { state = 2; progress = true; }
                    }
                } else if (state == 2) {
                    if (buf.length() >= 2) {
                        buf = buf.subStr(2, buf.length() - 2);
                        state = 0;
                        progress = true;
                    }
                } else if (state == 3) {
                    int nl = buf.indexOf("\r\n");
                    if (nl >= 0) {
                        string line = buf.subStr(0, nl);
                        buf = buf.subStr(nl + 2, buf.length() - (nl + 2));
                        if (line.isEmpty()) { state = 4; isDone = true; }
                        progress = true;
                    }
                }
            }
            return outp;
        }
    }
    // Encoder side (trivial): one chunk, then the terminator.
    string chunkEncode(string data) => data.length().toHex() + "\r\n" + data + "\r\n";
    string chunkEnd() => "0\r\n\r\n";

    // Parse the "Name: value" lines of a header block into a HeaderMap. Blank
    // names / missing colons are skipped; values are trimmed.
    HeaderMap parseHeaderLines(string block) {
        HeaderMap hm = HeaderMap();
        string hs = block;
        while (hs.length() > 0) {
            int nl = hs.indexOf("\r\n");
            string line = nl >= 0 ? hs.subStr(0, nl) : hs;
            hs = nl >= 0 ? hs.subStr(nl + 2, hs.length() - (nl + 2)) : "";
            int colon = line.indexOf(":");
            if (colon >= 0) {
                string k = line.subStr(0, colon).trim();
                string v = line.subStr(colon + 1, line.length() - (colon + 1)).trim();
                if (k.length() > 0) hm.add(k, v);
            }
        }
        return hm;
    }
    // Content-Length as a plain int (-1 = absent/invalid) — no int? narrowing.
    int bodyLenOf(HeaderMap hm) {
        string cl = hm.firstOr("Content-Length", "");
        if (cl.length() == 0) return 0 - 1;
        int v = 0;
        int i = 0;
        while (i < cl.length()) {
            int c = cl.byteAt(i);
            if (c < 48 || c > 57) return 0 - 1;
            v = v * 10 + (c - 48);
            i = i + 1;
        }
        return v;
    }

    class HttpRequest {
        string method = "";
        string path = "";
        string version = "";
        string body = "";
        HeaderMap headers;
        // incremental parse state (HttpConnection drives feed())
        string acc = "";
        bool headersDone = false;
        bool complete = false;
        int bodyNeeded = 0;

        void parseHead(string head) {
            int lineEnd = head.indexOf("\r\n");
            string reqLine = lineEnd >= 0 ? head.subStr(0, lineEnd) : head;
            int sp1 = reqLine.indexOf(" ");
            method = sp1 >= 0 ? reqLine.subStr(0, sp1) : reqLine;
            string rest = sp1 >= 0 ? reqLine.subStr(sp1 + 1, reqLine.length() - (sp1 + 1)) : "";
            int sp2 = rest.indexOf(" ");
            path = sp2 >= 0 ? rest.subStr(0, sp2) : rest;
            version = sp2 >= 0 ? rest.subStr(sp2 + 1, rest.length() - (sp2 + 1)) : "";
            string hs = lineEnd >= 0 ? head.subStr(lineEnd + 2, head.length() - (lineEnd + 2)) : "";
            headers = std::parseHeaderLines(hs);
        }
        // one-shot parse of a fully-buffered request (kept for direct callers).
        void parse(string raw) {
            int headerEnd = raw.indexOf("\r\n\r\n");
            string head = headerEnd >= 0 ? raw.subStr(0, headerEnd) : raw;
            this.parseHead(head);
            if (headerEnd >= 0) body = raw.subStr(headerEnd + 4, raw.length() - (headerEnd + 4));
        }
        // incremental: feed bytes; true once the full request (head + body by
        // Content-Length) is available. Extra trailing bytes are dropped
        // (buffered-but-serial: no pipelining in v1 — documented).
        bool feed(string chunk) {
            if (complete) return true;
            acc = acc + chunk;
            if (!headersDone) {
                int he = acc.indexOf("\r\n\r\n");
                if (he < 0) return false;
                this.parseHead(acc.subStr(0, he));
                acc = acc.subStr(he + 4, acc.length() - (he + 4));
                headersDone = true;
                int bl = std::bodyLenOf(headers);
                bodyNeeded = bl > 0 ? bl : 0;
            }
            if (acc.length() >= bodyNeeded) {
                body = acc.subStr(0, bodyNeeded);
                complete = true;
                return true;
            }
            return false;
        }
        string header(string name) => headers.firstOr(name, "");
    }

    class HttpResponse {
        int status = 200;
        string reasonText = "";
        string body = "";
        HeaderMap headers;
        bool keepAlive = false;
        new HttpResponse(int s, string b) { status = s; body = b; headers = HeaderMap(); }
        HttpResponse withHeader(string name, string value) { headers.set(name, value); return this; }
        string reason() {
            if (reasonText.length() > 0) return reasonText;
            if (status == 200) return "OK";
            if (status == 201) return "Created";
            if (status == 204) return "No Content";
            if (status == 301) return "Moved Permanently";
            if (status == 302) return "Found";
            if (status == 400) return "Bad Request";
            if (status == 401) return "Unauthorized";
            if (status == 403) return "Forbidden";
            if (status == 404) return "Not Found";
            if (status == 500) return "Internal Server Error";
            return "OK";
        }
        // Content-Length + Connection are computed here; any user header of those
        // names is dropped so the wire copy stays authoritative. Plain `+`
        // concatenation (ELF has no StringBuilder.toString/concatAll — see
        // HeaderMap.render); int operands convert implicitly, as the old code did.
        string render() {
            string extra = "";
            for (Header h in headers.entries()) {
                string ln = h.name.toLower();
                if (ln != "content-length" && ln != "connection") {
                    extra = extra + h.name + ": " + h.value + "\r\n";
                }
            }
            string conn = keepAlive ? "keep-alive" : "close";
            return "HTTP/1.1 " + status + " " + this.reason() +
                   "\r\nContent-Length: " + body.length() +
                   "\r\nConnection: " + conn + "\r\n" + extra + "\r\n" + body;
        }
        // Parse a fully-buffered response; decodes a chunked body transparently.
        void parse(string raw) {
            int lineEnd = raw.indexOf("\r\n");
            string statusLine = lineEnd >= 0 ? raw.subStr(0, lineEnd) : raw;
            int sp1 = statusLine.indexOf(" ");
            string rest = sp1 >= 0 ? statusLine.subStr(sp1 + 1, statusLine.length() - (sp1 + 1)) : statusLine;
            int sp2 = rest.indexOf(" ");
            string code = sp2 >= 0 ? rest.subStr(0, sp2) : rest;
            int? parsedStatus = code.toInt();
            status = parsedStatus != None ? parsedStatus : 0;   // 0: unparseable status line
            reasonText = sp2 >= 0 ? rest.subStr(sp2 + 1, rest.length() - (sp2 + 1)) : "";
            int bodyStart = raw.indexOf("\r\n\r\n");
            headers = HeaderMap();
            if (bodyStart >= 0 && lineEnd >= 0) {
                headers = std::parseHeaderLines(raw.subStr(lineEnd + 2, bodyStart - (lineEnd + 2)));
                string rawBody = raw.subStr(bodyStart + 4, raw.length() - (bodyStart + 4));
                if (headers.firstOr("Transfer-Encoding", "").toLower() == "chunked") {
                    ChunkedDecoder dec = ChunkedDecoder();
                    body = dec.feed(rawBody);
                } else {
                    body = rawBody;
                }
            }
        }
    }

    // One server-side connection. Drives HttpRequest.feed incrementally; on a
    // complete request runs the handler under a try/catch (an uncaught throw ->
    // 500 + Connection: close, loop survives — the framework error-page seed).
    // Keep-alive: if neither side closes and the per-conn cap isn't hit, the
    // parser is re-armed for the next request; every other path routes through
    // closeConn() so no watch is left armed (the §2.3 lifetime invariant).
    class HttpConnection {
        TcpStream conn;
        (HttpRequest) => HttpResponse handler;
        HttpRequest req;
        bool closed = false;
        int served = 0;
        int maxRequests = 100;
        new HttpConnection(TcpStream c, (HttpRequest) => HttpResponse h) {
            conn = c;
            handler = h;
            req = HttpRequest();
        }
        void start() {
            HttpConnection self = this;
            conn.onData((chunk) => self.feed(chunk));
        }
        void closeConn() {
            if (closed) return;
            closed = true;
            conn.close();
        }
        void feed(string chunk) {
            if (closed) return;
            if (!req.feed(chunk)) return;               // not a full request yet
            HttpResponse resp = HttpResponse(500, "Internal Server Error");
            bool ok = false;
            try {
                var h = handler;
                resp = h(req);
                ok = true;
            } catch (IException e) {
                resp = HttpResponse(500, "Internal Server Error");
                ok = false;
            }
            bool wantKeep = ok && req.header("Connection").toLower() != "close" && served < maxRequests;
            resp.keepAlive = wantKeep;
            conn << resp.render();
            if (wantKeep) {
                served = served + 1;
                req = HttpRequest();                    // re-arm for the next request
            } else {
                this.closeConn();
            }
        }
    }

    class HttpServer {
        TcpListener listener;
        (HttpRequest) => HttpResponse handler;
        bool tls = false;
        string certPath = "";
        string keyPath = "";
        new HttpServer(int port) { listener = TcpListener(port); }
        // HTTPS: same handler surface, cert/key stored; accept() wraps each
        // connection via tlsAccept and starts the HttpConnection only once the
        // handshake completes (a failed client handshake is a dropped
        // connection — the accept loop survives). Zero handler-facing change.
        new HttpServer(int port, string cert, string key) {
            listener = TcpListener(port);
            tls = true; certPath = cert; keyPath = key;
        }
        bool ok() => listener.ok();
        void handle((HttpRequest) => HttpResponse h) {
            handler = h;
            HttpServer self = this;
            listener.connections((conn) => self.accept(conn));
        }
        void accept(TcpStream conn) {
            if (!tls) {
                HttpConnection hc = HttpConnection(conn, handler);
                hc.start();
                return;
            }
            var h = handler;
            std::tlsAccept(conn.rawFd(), certPath, keyPath, "", 10000, (sfd) => {
                if (sfd < 0) return;                    // handshake failed / deadline: drop
                HttpConnection hc = HttpConnection(TcpStream(sfd), h);
                hc.start();
            });
        }
        void stop() { listener.stop(); }
    }

    // Client: blocking connect + send, then read the response through the loop
    // until the peer closes, and parse (chunked bodies decode in parse()).
    class HttpResponseReader {
        TcpStream stream;
        (HttpResponse) => void onResp;
        string acc = "";
        new HttpResponseReader(TcpStream s, (HttpResponse) => void cb) {
            stream = s;
            onResp = cb;
        }
        void start() {
            HttpResponseReader self = this;
            stream.onData((chunk) => self.feed(chunk));
            stream.onClose(() => self.finish());
        }
        void feed(string chunk) { acc = acc + chunk; }
        void finish() {
            HttpResponse resp = HttpResponse(0, "");
            resp.parse(acc);
            var cb = onResp;
            cb(resp);
        }
    }

    class HttpClient {
        // The general form: method + explicit headers + body. Sends Connection:
        // close (one connection per request — client pooling is a framework-era
        // follow-up). Host/port/path form; URL-string parsing + redirects are
        // the documented next step.
        void request(string method, string host, int port, string path,
                     HeaderMap headers, string body, (HttpResponse) => void onResp) {
            int fd = std::sysTcpConnect(host, port);
            // Same guard as requestTls: don't feed a failed connect (-1) into
            // sysSend/TcpStream — surface the connect error plainly.
            if (fd < 0) {
                throw RuntimeException("TCP connect failed (host '" + host +
                                       "', port " + port + ")");
            }
            string extra = "";
            for (Header h in headers.entries()) {
                string ln = h.name.toLower();
                if (ln != "host" && ln != "connection" && ln != "content-length") {
                    extra = extra + h.name + ": " + h.value + "\r\n";
                }
            }
            string clh = body.length() > 0 ? ("Content-Length: " + body.length() + "\r\n") : "";
            string reqstr = method + " " + path + " HTTP/1.1\r\nHost: " + host +
                            "\r\nConnection: close\r\n" + extra + clh + "\r\n" + body;
            std::sysSend(fd, reqstr);
            TcpStream stream = TcpStream(fd);
            HttpResponseReader reader = HttpResponseReader(stream, onResp);
            reader.start();
        }
        void get(string host, int port, string path, (HttpResponse) => void onResp) {
            this.request("GET", host, port, path, HeaderMap(), "", onResp);
        }
        void post(string host, int port, string path, string body, (HttpResponse) => void onResp) {
            this.request("POST", host, port, path, HeaderMap(), body, onResp);
        }
        // Await-able form: the callback reshaped into a Promise (§14 — a promise
        // is a one-shot stream). `HttpResponse r = await client.fetch(...);`
        Promise<HttpResponse> fetch(string host, int port, string path) {
            Promise<HttpResponse> p = Promise();
            this.get(host, port, path, (resp) => p.resolve(resp));
            return p;
        }

        // --- HTTPS (LA-2 §6.3): connect + full verification + identical tail ---
        // Always verifyMode 0 (chain + hostname). tlsConnect throws a loud named
        // RuntimeException on verification failure (before the first byte of
        // protocol). URL parsing stays deferred (info.md §19 #17) — explicit
        // host/port/path, same as the plaintext client.
        void requestTls(string method, string host, int port, string path,
                        HeaderMap headers, string body, (HttpResponse) => void onResp) {
            int fd = std::sysTcpConnect(host, port);
            // Fail on the real cause: a TCP connect failure (fd < 0) must not be
            // handed to tlsConnect, which would report it as an empty-reason
            // "TLS handshake:  (host '...', fd -1)". Surface the connect error.
            if (fd < 0) {
                throw RuntimeException("TCP connect failed (host '" + host +
                                       "', port " + port + ")");
            }
            string extra = "";
            for (Header h in headers.entries()) {
                string ln = h.name.toLower();
                if (ln != "host" && ln != "connection" && ln != "content-length") {
                    extra = extra + h.name + ": " + h.value + "\r\n";
                }
            }
            string clh = body.length() > 0 ? ("Content-Length: " + body.length() + "\r\n") : "";
            string reqstr = method + " " + path + " HTTP/1.1\r\nHost: " + host +
                            "\r\nConnection: close\r\n" + extra + clh + "\r\n" + body;
            var onr = onResp;
            std::tlsConnect(fd, host, "", "", 0, (cfd) => {
                std::sysSend(cfd, reqstr);
                TcpStream stream = TcpStream(cfd);
                HttpResponseReader reader = HttpResponseReader(stream, onr);
                reader.start();
            });
        }
        void getTls(string host, int port, string path, (HttpResponse) => void onResp) {
            this.requestTls("GET", host, port, path, HeaderMap(), "", onResp);
        }
        void postTls(string host, int port, string path, string body, (HttpResponse) => void onResp) {
            this.requestTls("POST", host, port, path, HeaderMap(), body, onResp);
        }
        Promise<HttpResponse> fetchTls(string host, int port, string path) {
            Promise<HttpResponse> p = Promise();
            this.getTls(host, port, path, (resp) => p.resolve(resp));
            return p;
        }
    }

    // Path attributes, usable without opening (File wraps these per-instance).
    bool fileExists(string path) => sysStat(path, 0) == 1;
    int fileSize(string path) => sysStat(path, 1);
    int fileModified(string path) => sysStat(path, 2);
    // request-stat-isdir.md: one stat(2) word instead of a per-entry sysListDir
    // probe; also correct on an unreadable directory, which a listDir-based
    // probe misclassifies as a file.
    bool isDir(string path) => sysStat(path, 3) == 1;

    // Open-mode flags: plain values whose (|) is an ordinary operator method —
    // `std::read | std::write` is operators-as-methods, NOT a union type.
    class OpenMode {
        int bits = 0;
        new OpenMode(int b) { bits = b; }
        OpenMode (|)(OpenMode o) => OpenMode(bits | o.bits);
        bool has(OpenMode o) => (bits & o.bits) == o.bits;
    }
    const OpenMode read = OpenMode(1);
    const OpenMode write = OpenMode(2);
    const OpenMode append = OpenMode(4);
    const OpenMode binary = OpenMode(8);    // inert until Block lands (text-only reads)

    interface IFileException : IException { }
    class FileException : Exception, IFileException {
        new FileException(string msg) { Exception::Exception(msg); }
    }

    // File streams: fd-backed endpoints with the stream operator surface.
    class FileInStream {
        int fd = 0;
        new FileInStream(int f) { fd = f; }
        string pull() => std::sysReadLine(fd);      // "" = end of input (interim)
        string read(int max) => std::sysRead(fd, max);
    }
    class FileOutStream {
        int fd = 0;
        new FileOutStream(int f) { fd = f; }
        FileOutStream (<<)(string s) {
            std::sysWrite(fd, s);
            return this;
        }
    }

    // techdesign-02 F3: the `using` contract — a type declares deterministic
    // cleanup by implementing this and a `using` declaration calls close() on
    // every block-exit edge (fallthrough/return/throw/break/continue).
    interface IDisposable {
        void close();
    }

    // The File object: owns the descriptor + mode; construction opens.
    class File : IDisposable {
        string path;
        OpenMode mode = OpenMode(0);
        int fd = 0 - 1;
        bool opened = false;

        new File(string p, OpenMode m) {
            path = p;
            mode = m;
            open();
        }

        void open() {
            if (opened) throw FileException("already open: " + path);
            fd = std::sysOpen(path, mode.bits);
            if (fd < 0) throw FileException("cannot open: " + path);
            opened = true;
        }
        void close() {
            if (!opened) throw FileException("not open: " + path);
            std::sysClose(fd);
            opened = false;
            fd = 0 - 1;
        }
        bool isOpen() => opened;

        bool exists() => std::fileExists(path);
        int size() => std::fileSize(path);
        int modified() => std::fileModified(path);

        FileInStream reader() => FileInStream(fd);
        FileOutStream writer() => FileOutStream(fd);

        void write(string s) { std::sysWrite(fd, s); }
        void writeln(string s) { std::sysWrite(fd, s + "\n"); }
        string readln() => std::sysReadLine(fd);
        string read(int max) => std::sysRead(fd, max);
        // Track 03 M4 — zero-copy Block overloads over the same sys floor.
        int read(Block b, int max) => std::sysRead(fd, b, max);
        void write(Block b, int off, int len) { std::sysWrite(fd, b, off, len); }
    }

    interface IException {
        string message;
        string toString();
    }
    class Exception : IException {
        string message;
        new Exception(string msg) { message = msg; }
        string toString() => message;
    }
    interface IRuntimeException : IException { }
    class RuntimeException : Exception, IRuntimeException {
        new RuntimeException(string msg) { Exception::Exception(msg); }
    }
    interface ILogicException : IException { }
    class LogicException : Exception, ILogicException {
        new LogicException(string msg) { Exception::Exception(msg); }
    }
    // LA-30 B2 (doc 06 §2): cancellation is an exception delivered at park
    // points, and only there — an ordinary IException carrier, catchable by
    // type or interface like everything else (no second error channel, the same
    // collapse LA-19 makes for rejection). A task MAY catch it and refuse
    // cancellation (with the visibility that implies); a group's close() absorbs
    // it at the join, while an unowned callback rides the normal uncaught path.
    interface ICancelledException : IException { }
    class CancelledException : Exception, ICancelledException {
        new CancelledException(string msg) { Exception::Exception(msg); }
    }
}
)prelude";

// --- kPreludeRest: top-level Range/StreamBuffer/Console + meta/math/env/term
const char* kPreludeRest = R"prelude(

// An integer range produced by `a .. b` (inclusive). Iterable by `for..in`
// (the counted-loop fast path, contract C5); IIterable for protocol uniformity.
class Range : IIterable<int> {
    int start = 0;
    int end = 0;
    new Range(int s, int e) { start = s; end = e; }
    IIterator<int> iterator() => RangeIterator(this);
}
// The Range's protocol iterator (techdesign-07 §2.1). `for..in` over a range
// still counts inline; this exists so a Range can stand where an IIterable is
// wanted. Inclusive of `end`, matching the counted-loop fast path.
class RangeIterator : IIterator<int> {
    int cur = 0;
    int last = 0;
    new RangeIterator(Range r) { cur = r.start; last = r.end; }
    bool hasNext() => cur <= last;
    int next() { int v = cur; cur = cur + 1; return v; }
}


// --- Streams: THE system boundary (§13) -------------------------------------
// The buffer is a QUEUE (single consumer; each element consumed once). The
// rate-matching ring over raw memory is the later implementation of this same
// class; the interface is the contract. subscribe() is a STANDING PULL: it
// claims the consumer end (pull afterwards is an error); broadcast is a
// Layer-2 reshaping (EventEmitter), never a substrate property.
class StreamBuffer<T> {
    Array items;
    (T) => void handler;
    bool hasHandler = false;
    bool closed = false;                       // SU-1 §4.1: consumer-closed cutoff
    bool claimed = false;                      // D-B/DM-3: iterator() exclusivity (parallels hasHandler)
    Array<Promise<bool>> waiters;               // D-B/DM-3: parked hasNext() callers

    new StreamBuffer() { }

    int count() => items.length();
    bool isEmpty() => items.isEmpty();

    void push(T v) {
        if (closed) return;                    // SU-1: silent drop — strict fanout cutoff (R5)
        if (hasHandler) {
            var cb = handler;
            cb(v);
            return;
        }
        items = items.add(v);
        wake(true);                            // D-B/DM-3: a parked hasNext() can now proceed
    }
    // D-B/DM-3: raw dequeue, bypassing the iterator-claim guard — StreamIterator
    // IS the claim holder, so it must be able to drain through it. ITEMS WIN
    // OVER CLOSED: a producer's natural "push the last item, then close()" must
    // still deliver that item (the closed flag can already be true by the time
    // a parked hasNext() resumes and calls next() — resolve() does not resume
    // its awaiter synchronously, so a same-tick push-then-close can race ahead
    // of the resume). This deliberately differs from pull() below: see there
    // for why the two must NOT share one ordering.
    T pullRaw() {
        if (!isEmpty()) {
            T v = items.first();
            items = items.skip(1);
            return v;
        }
        if (closed) throw RuntimeException("stream is closed");
        throw RuntimeException("stream is empty");
    }
    // UNCHANGED from SU-1 (closed wins unconditionally, even over leftover
    // buffered items) — pinned by tests/corpus/floor/unsub_inmem.lev's
    // "pull:stream is closed" with 2 un-pulled items still sitting in the
    // buffer. That is a CONSUMER-close contract (the closer said done, drop
    // everything); pullRaw()/pullOrNone() below are the producer-EOF-shaped
    // siblings this design adds beside it, per techdesign-stream-unsubscribe.md
    // §8's hand-off ("introduce pullOrNone beside the throwing pull").
    T pull() {
        if (hasHandler) throw RuntimeException("consumer end is claimed by a subscriber");
        if (claimed) throw RuntimeException("consumer end is claimed by an iterator");
        if (closed) throw RuntimeException("stream is closed");
        if (isEmpty()) throw RuntimeException("stream is empty");
        T v = items.first();
        items = items.skip(1);
        return v;
    }
    // D-B/DM-3 (§2.3 step 1): the honest non-blocking pull. None collapses
    // "nothing buffered yet" and "closed and drained" into one outcome — a
    // poller doesn't need the strict trichotomy the blocking iterator below
    // does (that one gets it from waitForData's true/false, not from a value).
    // Items win over closed, like pullRaw() (the producer-EOF drain rule).
    T? pullOrNone() {
        if (hasHandler) throw RuntimeException("consumer end is claimed by a subscriber");
        if (claimed) throw RuntimeException("consumer end is claimed by an iterator");
        if (!isEmpty()) {
            T v = items.first();
            items = items.skip(1);
            return v;
        }
        return None;
    }
    void setHandler((T) => void cb) {
        if (closed) throw RuntimeException("stream is closed");
        if (hasHandler) throw RuntimeException("consumer end is already claimed");
        if (claimed) throw RuntimeException("consumer end is claimed by an iterator");
        handler = cb;
        hasHandler = true;
        while (!items.isEmpty()) {
            T v = items.first();
            items = items.skip(1);
            cb(v);
        }
    }
    // D-B/DM-3: claims the consumer end for iteration — the same exclusivity
    // rule subscribe() enforces for callbacks, on a second surface (§13: a
    // stream has ONE consumer).
    void claimIterator() {
        if (hasHandler) throw RuntimeException("consumer end is claimed by a subscriber");
        if (claimed) throw RuntimeException("consumer end is already claimed");
        claimed = true;
    }
    // D-B/DM-3 (§2.3 step 2): the blocking-pull wait, built entirely on the
    // existing Promise/await suspension surface — no new native, no new IR op.
    // Resolves true the instant data is available (fast path: an
    // already-ready-shaped return needs no park), false once the stream is
    // closed with nothing left. Items win over closed (matches pullRaw()), so
    // hasNext()/next() always agree with each other on what "left" means.
    Promise<bool> waitForData() {
        Promise<bool> p = Promise();
        if (!isEmpty()) { p.resolve(true); return p; }
        if (closed) { p.resolve(false); return p; }
        waiters = waiters.add(p);
        return p;
    }
    void wake(bool v) {
        if (waiters.length() == 0) return;
        Array<Promise<bool>> w = waiters;
        waiters = waiters.skip(waiters.length());
        for (Promise<bool> p in w) p.resolve(v);
    }
    void close() {                             // SU-1: idempotent, never throws
        closed = true;
        hasHandler = false;                    // detach any claimed consumer
        wake(false);                           // D-B/DM-3: wake parked hasNext() with "no more data"
    }
}

// Typed views: capability is which operators the type exposes (structural).
// SU-1: InStream is IDisposable — an optional producer-attached teardown
// closure rides alongside the buffer (the TcpStream onClose/hasCloseCb
// pattern), so `using` works uniformly on every stream (real teardown only
// where a producer attached one; a plain in-memory stream just closes its
// buffer). SU-1: : IDisposable so `using InStream<int> w = signal::on(sig);`
// releases the subscription (and, on last-out, its fd/watch) on scope exit.
// D-B/DM-3 (techdesign-http-and-streams-maturity.md §2.3 step 3): also
// IIterable<T>, so `for (T x in stream)` works directly. Because Track 07's
// dispatch is static-by-type through contract C5 (fast paths, then protocol)
// and the protocol path lowers to plain CallDyn, this flip costs zero
// checker/Eval/Lower/backend work — it is an ordinary prelude interface add.
class InStream<T> : IDisposable, IIterable<T> {
    StreamBuffer<T> buf;
    () => void onDispose;
    bool hasDispose = false;

    new InStream(StreamBuffer<T> b) { buf = b; }

    T pull() => buf.pull();
    T? pullOrNone() => buf.pullOrNone();       // D-B/DM-3: honest non-blocking pull
    bool hasData() => !buf.isEmpty();
    void subscribe((T) => void cb) buf.setHandler(cb);

    // D-B/DM-3: claims the consumer end like subscribe() does, then hands off
    // to StreamIterator, which drives the buffer directly so it can bypass the
    // very claim it just took (buf.pullRaw()) while external pull()/subscribe()
    // calls made afterward still see the exclusivity error.
    IIterator<T> iterator() {
        buf.claimIterator();
        return StreamIterator(buf);
    }

    // The bridge into the lazy pipeline (mirrors Array.asSeq(), techdesign-07
    // §3): `for..in` already works via IIterable above; this additionally
    // unlocks `.map()/.take()/.where()/...` over a live stream.
    Seq<T> asSeq() => StreamSeq(this);

    void close() {
        if (hasDispose) {
            hasDispose = false;                // flip BEFORE running — reentrancy/double-close safe
            var d = onDispose;
            d();
        }
        buf.close();
    }
}
// D-B/DM-3: the stream's protocol iterator. Holds the StreamBuffer directly
// (not the InStream) so next() can drain via pullRaw(), bypassing the very
// claim this iterator itself holds. hasNext() parks on the waiter-promise
// wait — the same suspension surface `await` uses, so a live producer's next
// push wakes it, and close() (consumer- or producer-initiated — both route
// through the same StreamBuffer.close()) wakes it with "no more data" (the
// "Timer-fed for..in delivers all ticks then exits at close" contract).
class StreamIterator<T> : IIterator<T> {
    StreamBuffer<T> buf;
    new StreamIterator(StreamBuffer<T> b) { buf = b; }
    bool hasNext() {
        Promise<bool> p = buf.waitForData();
        bool got = await p;
        return got;
    }
    T next() => buf.pullRaw();
}
// D-B/DM-3: lets a stream join the same lazy `.map()/.take()/.where()`
// pipeline arrays get via asSeq() — a stream is exactly as valid an
// IIterable source. Track 07 §5#4's "terminals require finite sources"
// applies verbatim here: an unclosed stream is an infinite source, `take(n)`
// is the caller's bound (no runtime guard, same stance as `while (true)`).
class StreamSeq<T> : Seq<T> {
    InStream<T> source;
    new StreamSeq(InStream<T> src) { source = src; }
    IIterator<T> iterator() => source.iterator();
}
class OutStream<T> {
    StreamBuffer<T> buf;
    new OutStream(StreamBuffer<T> b) { buf = b; }
    OutStream<T> (<<)(T v) {
        buf.push(v);
        return this;
    }
}
// The diamond: both sides' `buf` collapse to ONE slot (same name + type, not
// distinct) — a bidirectional stream has both ends on the same conduit (§13).
class IOStream<T> : InStream<T>, OutStream<T> {
    new IOStream(StreamBuffer<T> b) {
        InStream::InStream(b);
        OutStream::OutStream(b);
    }
}

// The console: stdout as a real object (§13 — nothing crosses the process
// boundary except through a stream surface). `write`/`writeln` stringify
// their argument and emit it — native intrinsics (the engines' stringifier)
// until varargs exist; the generic T binds per call, so any value goes
// through one declared surface. (<<) is the ordinary transfer operator, so
// `console << s` chains like any out-stream. Declared here so `console`
// resolves, type-checks, and aliases like any other global — the name is no
// longer a compiler special case.
class Console {
    void write<T>(T v);        // native: stringify + write to stdout
    void writeln<T>(T v);      // native: stringify + write + newline
    void writeln();            // native: bare newline
    Console (<<)(string s) {
        std::sysWrite(1, s);
        return this;
    }
}
const Console console = Console();

// §16.5 Phase 3 §3: the reflection value surface `where` clauses read. Kept
// deliberately minimal (every exposed field is a forever-API) — types are
// canonical strings, not a structured Type; attrs are names only.
namespace meta {
    Ast parseExpr(string source) {
        throw RuntimeException("meta::parseExpr() is compile-time-only");
    }
    Ast parseStmts(string source) {
        throw RuntimeException("meta::parseStmts() is compile-time-only");
    }
    class Param  { string name; string type; }
    // Attribute-value reflection (LA-4 item A, P4-I): a matched attribute already
    // binds as an instance of its own class (`match @Column(c) ... $c.name`); this
    // gives the SAME argument values to `$for`-iteration, where only names were
    // visible before. `args` is positional (attribute-field declaration order,
    // the same order `evalAttrArgs` fills); each slot carries every primitive
    // form so the typed accessors need no per-arg type tag. Attribute fields are
    // int/float/bool/string only (validateAttributeDecl), so four slots suffice.
    class AttrArg { string s; int i; bool b; float f; bool present; }
    class Attr {
        string name;                               // the attribute's name, e.g. "Column"
        Array<meta::AttrArg> args;                  // positional argument values
        int    argCount()   => args.length();
        // A defaulted (not explicitly written) argument reads as None, so
        // `attr("Column")?.argStr(0) ?? field.name` falls back cleanly.
        string? argStr(int i)   => args.at(i).present ? args.at(i).s : None;
        int?    argInt(int i)   => args.at(i).present ? args.at(i).i : None;
        bool?   argBool(int i)  => args.at(i).present ? args.at(i).b : None;
        float?  argFloat(int i) => args.at(i).present ? args.at(i).f : None;
    }
    class Field  {
        string name; string type;
        Array<string> attrs;                       // resolved attribute names
        Array<meta::Attr> attributes;              // names + reified argument values
        bool hasAttr(string n) => attrs.contains(n);
        meta::Attr? attr(string n) => attributes.find((a) => a.name == n);
    }
    class Method {
        string name; string returnType;
        Array<meta::Param> params;
        Array<string> attrs;
        Array<meta::Attr> attributes;              // names + reified argument values
        bool hasAttr(string n) => attrs.contains(n);
        meta::Attr? attr(string n) => attributes.find((a) => a.name == n);
        int arity() => params.length();
    }
    class Class {
        string name;
        Array<string> bases;                       // canonical base spellings
        Array<meta::Field> fields;
        Array<meta::Method> methods;
        bool hasBase(string n) => bases.contains(n);
    }
}

// Track 06: `std::math` per the design (§1.3) hits a real qualified-static-
// path resolution gap on nested namespaces — `std::math::pi` silently
// resolves wrong on the oracle and hard-errors ("IR: not yet lowerable: name
// 'std'") on --ir/--emit-cpp (probed against this tree; bug.md, filed
// separately — bug.md #1/#2's nested-namespace-import fix does not cover
// this qualified-*path* case). The design's own problem #3 pre-authorizes
// exactly this fallback: a TOP-LEVEL `namespace math` (not nested in `std`),
// which the resolver already handles like any other namespace — verified
// working on all five engines, including the int/float `min`/`max` overload
// set (problem #5) and `uses math;` unqualified access.
namespace math {
    const float pi = 3.141592653589793;
    const float e  = 2.718281828459045;
    // Native free functions (RuntimeNatives.cpp / CGen); deferred with a
    // clean diagnostic on the ELF/LLVM native backends (the zero-dep /
    // best-effort boundary — polynomial emission is a later project).
    float log(float x);
    float log2(float x);
    float exp(float x);
    float sin(float x);
    float cos(float x);
    float tan(float x);
    float atan2(float y, float x);
    // struct-equality design §8: the `float::fromBits(int)` factory. No
    // `static` keyword exists in the language (Track 04 M3 problem #6), so —
    // like std::charFromCode / std::byteToString — the static-on-primitive is
    // homed as a free function; the checker routes `float::fromBits(x)` here
    // (mirrors Enum::fromCode). `math::floatFromBits` is the same native.
    float floatFromBits(int bits);
    int   min(int a, int b) => a < b ? a : b;
    int   max(int a, int b) => a > b ? a : b;
    float min(float a, float b) => a < b ? a : b;
    float max(float a, float b) => a > b ? a : b;
}

// argv (argv.md): the program's command-line arguments. A TOP-LEVEL namespace,
// exactly like `math` above and for the same reason — nested `std::env::...`
// hits the qualified-path resolution gap, whereas a flat `namespace env` is
// handled like any other and gives the identical `env::args()` spelling on all
// engines. `args()` is a CALL, not a binding (argv.md §3): a fresh pure
// Array<string> per call, native deferred to the call site so comptime
// hermeticity falls out for free. args()[0] is the program name; args().skip(1)
// is the real argument list. Length is always >= 1, never empty.
namespace env {
    Array<string> args() => std::sysArgs();
    string name() {
        Array<string> a = std::sysArgs();
        return a.at(0);
    }
    // Read an environment variable (Track 08 F1). None = unset — distinct from
    // set-but-empty, so a caller tells "PROXY not set" from "PROXY=''" apart.
    // The native is deferred to the call site, so comptime hermeticity holds.
    string? get(string key) => std::sysEnv(key);
    // Process exit (designs/exit-codes.md §3.3). Exit is a process fact, so it
    // joins argv under `env` rather than a new namespace. Functions, not
    // bindings — the native is deferred to the call site, so comptime
    // hermeticity holds (a `comptime env::exit(1)` hits the sys* deny). exit()
    // terminates now (abandons pending loop work); setExitCode() records the
    // code and lets execution finish (the loop drains, cleanups run).
    void exit(int code)        { std::sysExit(code); }
    void setExitCode(int code) { std::sysSetExitCode(code); }
}

// system-binds.md §4: capability interfaces + system implementations + root
// binds — the DI-typed alternative to the ambient globals above (console,
// env::*, File, TcpStream/TcpListener). Reopens `namespace std` (§12: a
// namespace is declaration-based, not one textual block) so `use std::IEnv;`
// (Channel 1, system-binds.md §5) can activate these binds by naming the
// type; the implicit `uses std;` sees only the NAMES (the wall, §1) —
// nothing is ambiently bound just by being visible.
namespace std {
    // What a consumer may learn from the process environment.
    interface IEnv {
        Array<string> args();
        string? variable(string name);      // None = unset (distinct from set-but-empty)
    }

    // What a consumer may say to / hear from the terminal.
    interface IConsole {
        void write(string s);
        void writeln(string s);
        void writeln();
    }

    // What a consumer may know about time.
    interface IClock {
        int now();                          // epoch milliseconds (std::sysNow)
    }

    // What a consumer may do to the filesystem: acquire File objects + cheap queries.
    interface IFileSystem {
        File open(string path, OpenMode mode);   // throws FileException, same as File()
        bool exists(string path);
    }

    // What a consumer may do to the network: acquire streams/listeners.
    interface INet {
        TcpStream? connect(string host, int port);   // None = connect failure
        TcpListener listen(int port);                 // caller checks .ok(), same as today
    }

    // System impls are always shim classes, never the existing globals
    // directly (`console` can't satisfy IConsole — its write/writeln are
    // generic, §4.1). Each shim is a one-line delegation to the convenience
    // surface, so there is exactly one behavior underneath both paths (§4.2).
    class SystemEnv : IEnv {
        Array<string> args() => env::args();
        string? variable(string name) => env::get(name);     // env::get, NOT env::variable
    }
    class SystemConsole : IConsole {
        void write(string s) { console.write(s); }
        void writeln(string s) { console.writeln(s); }
        void writeln() { console.writeln(); }
    }
    class SystemClock : IClock {
        int now() => std::sysNow();
    }
    class SystemFileSystem : IFileSystem {
        File open(string path, OpenMode mode) => File(path, mode);
        bool exists(string path) => std::fileExists(path);
    }
    class SystemNet : INet {
        TcpStream? connect(string host, int port) {
            int fd = std::sysTcpConnect(host, port);
            if (fd < 0) return None;
            return TcpStream(fd);
        }
        TcpListener listen(int port) => TcpListener(port);
    }

    // Root binds (§4.3, revised post-M1): fresh-per-injection, not a shared
    // `const` global instance. An eagerly-instantiated global forces emit-C++
    // to mark ALL of its class's methods reachable unconditionally (the
    // `[[leviathan-prelude-backend-gotchas]]` eager-global gotcha) — for
    // these five that pulled in natives (sysEnv, sysTaskCancel via
    // File/TcpStream's IDisposable-name over-marking) emit-C++ doesn't
    // implement, breaking EVERY program's `--build`, not just ones using a
    // capability. All five shims are stateless, so fresh-per-injection is
    // behaviorally identical to sharing one instance; only the reachability
    // shape differs. Inert on their own regardless (§3): the Checker never
    // walks prelude bodies, so nothing registers these into any bind scope
    // until Channel 1 (§5) indexes them — and even then only an explicit
    // `use std::I...;` activates one; the implicit `uses std;` never does.
    bind IEnv        => SystemEnv();
    bind IConsole    => SystemConsole();
    bind IClock      => SystemClock();
    bind IFileSystem => SystemFileSystem();
    bind INet        => SystemNet();
}

// Terminal control (designs/terminal-raw-mode.md §3.3). A TOP-LEVEL namespace,
// like `env`/`math`, so `term::` resolves on every engine. Functions, not
// bindings — I/O deferred to the call site (argv.md §3), so comptime hermeticity
// holds. enableRaw()/restore() drive stdin (fd 0); the runtime GUARANTEES the
// terminal is restored on any exit path even if `restore()` is never reached
// (§3.4), so a TUI that crashes mid-draw never orphans the shell.
namespace term {
    bool enableRaw() => std::sysTermRaw(0) == 0;    // true if it took (false: not a tty)
    void restore()   { std::sysTermRestore(0); }
    // isRaw() (design §3.3, promised to the winsize work): the raw-mode state
    // query, backed by the sysTermIsRaw native rather than a namespace global
    // (bug.md #1/#7). Its consumer is size()'s cursor-report fallback guard.
    bool isRaw() => std::sysTermIsRaw(0);

    // Terminal size (designs/techdesign-terminal-floor.md §2). WinSize is
    // rows/cols; a Sonar Size is w=cols/h=rows — the axis mapping is the
    // consumer's job (§2), so the floor stays honest.
    class WinSize {
        int rows;
        int cols;
        new WinSize() { rows = 24; cols = 80; }        // bare decl -> the safe default
        new WinSize(int r, int c) { rows = r; cols = c; }
    }
    // size(): ask the kernel (sysWinSize over stdout, fd 1); on failure fall
    // back — if raw mode is active — to the ANSI cursor-report probe (move the
    // cursor far, ask where it landed), else the 24x80 default. The probe is
    // guarded on raw mode because cooked mode can't read the \x1b[6n reply
    // unbuffered; a garbage reply parses to failure -> default, never a throw.
    // Deliberately int-sentinel throughout (no T?/narrowing): prelude code that
    // leans on union flow-narrowing is misread by LLVM (leviathan-prelude-no-
    // narrowing), so failure is a -1/0 sentinel, never a None.
    WinSize size() {
        int r = std::sysWinSize(1, 0);
        int c = std::sysWinSize(1, 1);
        if (r > 0 && c > 0) return term::WinSize(r, c);
        if (term::isRaw()) {
            std::sysWrite(1, "\x1b[999C\x1b[999B\x1b[6n");
            string resp = term::readCursorReport();
            int lb   = resp.indexOf("[");
            int semi = resp.indexOf(";");
            int rr   = resp.indexOf("R");
            if (lb >= 0 && semi > lb && rr > semi) {
                int pr = term::parseNum(resp, lb + 1, semi);
                int pc = term::parseNum(resp, semi + 1, rr);
                if (pr > 0 && pc > 0) return term::WinSize(pr, pc);
            }
        }
        return term::WinSize(24, 80);
    }
    // Read stdin (fd 0) until the report terminator 'R' arrives or a read yields
    // nothing. Raw mode's VMIN=1 blocks for >=1 byte, so this returns as soon as
    // the terminal answers; the 64-iteration cap bounds a misbehaving terminal.
    string readCursorReport() {
        string acc = "";
        int guard = 0;
        while (guard < 64) {
            string chunk = std::sysRead(0, 16);
            if (chunk.isEmpty()) return acc;
            acc = acc + chunk;
            if (acc.indexOf("R") >= 0) return acc;
            guard = guard + 1;
        }
        return acc;
    }
    // Decimal parse of resp[start, end) as a pure int sentinel: -1 if empty or
    // any non-digit byte (avoids string.toInt()'s int? and any narrowing).
    int parseNum(string resp, int start, int end) {
        if (end <= start) return 0 - 1;
        int v = 0;
        int i = start;
        while (i < end) {
            int b = resp.byteAt(i);
            if (b < 48 || b > 57) return 0 - 1;
            v = v * 10 + (b - 48);
            i = i + 1;
        }
        return v;
    }
}

// Signals as streams (designs/techdesign-terminal-floor.md §3). Ruling
// (signals.md, normative): language code NEVER runs in signal context — a
// signal is a readable system stream. A TOP-LEVEL namespace like term/env so
// `signal::` resolves on every engine. The consts are the POSIX numbers
// (Linux). SIGKILL/SIGSTOP are deliberately absent (the floor rejects them).
namespace signal {
    const int HUP   = 1;
    const int INT   = 2;
    const int QUIT  = 3;
    const int USR1  = 10;
    const int TERM  = 15;
    const int WINCH = 28;

    // Registry state as a PLAIN data holder (no native-using methods), so the
    // eager global below compiles cheaply on EVERY backend — including emit-C++,
    // where the signal STREAM itself is loop-bound and unavailable (§3 lane
    // matrix, same boundary as timers/sockets). The native work lives in the
    // free functions, which are demand-compiled: a program that never calls
    // signal::on never drags sysSignalOpen/sysWatch into the emit-C++ output (a
    // methods-on-a-global-instance shape would force it on every program).
    // Parallel arrays keyed by DISTINCT signal number: subs[i] is the fanout
    // list for sigs[i], fds[i] its open fd. One fd per signal number is required
    // for correct multi-subscriber fanout (two signalfds for one signal would
    // race for the single pending delivery). v1 leaks the subscription until
    // program end (open question #1) — Sonar holds one WINCH sub for its App's
    // lifetime, so that is acceptable. The interpreter closes the fds and
    // unblocks at program end (M4); a compiled process's exit does the same.
    class SignalState {
        Array<int> sigs = [];
        Array<int> fds  = [];
        Array<int> watchIds = [];                  // SU-1 R3: sysWatch ids, enables sysUnwatch
        Array<Array<StreamBuffer<int>>> subs = [];
        Array<Array<int>> subIds = [];              // SU-1 R3: parallel per-sub int ids (R2)
        int nextId = 0;                             // SU-1: monotonic subscription id source
    }
    // Qualified construction: bare same-namespace construction (M2) is deferred
    // (leviathan-bug32-namespace-ctor-landed), so name the namespace explicitly.
    const SignalState st = signal::SignalState();

    int findSig(int sig) {
        int i = 0;
        while (i < signal::st.sigs.length()) { if (signal::st.sigs.at(i) == sig) return i; i = i + 1; }
        return 0 - 1;
    }
    int findFd(int fd) {
        int i = 0;
        while (i < signal::st.fds.length()) { if (signal::st.fds.at(i) == fd) return i; i = i + 1; }
        return 0 - 1;
    }

    // The loop calls this when a signalfd is read-ready. Drain EVERY queued
    // delivery (SIGWINCH coalescing: a resize storm collapses to >=1 tick — the
    // read drains all pending) and broadcast each signo to that signal's
    // subscribers (§13: broadcast is a Layer-2 reshaping, never a substrate
    // property; a slow listener is visibly its own problem). One fd carries one
    // signal number, so signo == the subscribed signal.
    // SU-1 R4: idx is re-resolved at the TOP OF EVERY drain iteration (not
    // cached once) — a mid-drain last-close tears the registry row down, and
    // the next iteration must see that rather than index a stale row.
    void deliver(int fd) {
        int signo = std::sysSignalNext(fd);
        while (signo >= 0) {
            int idx = signal::findFd(fd);
            if (idx < 0) return;               // registry row torn down mid-drain: stop
            Array<StreamBuffer<int>> list = signal::st.subs.at(idx);   // pure-value snapshot
            int i = 0;
            while (i < list.length()) {
                OutStream<int> w = OutStream(list.at(i));
                w << signo;                    // push: closed buffers silently drop (R5)
                i = i + 1;
            }
            signo = std::sysSignalNext(fd);
        }
    }

    // The one public entry: subscribe to a signal, get a readable int stream of
    // its deliveries. No pre-subscription buffering — a signal delivered before
    // any subscriber is dropped (pinned by test). With ISIG cleared in raw mode,
    // signal::on(INT) fires only from an external `kill`, never ^C. SU-1: the
    // returned InStream carries an IDisposable teardown to `signal::off`, so
    // `using InStream<int> w = signal::on(sig);` releases the subscription (and,
    // on last-out, the fd/watch) deterministically.
    InStream<int> on(int sig) {
        StreamBuffer<int> b = StreamBuffer();
        int myId = signal::st.nextId;
        signal::st.nextId = signal::st.nextId + 1;
        int idx = signal::findSig(sig);
        if (idx < 0) {
            // First subscriber for this signal: open the fd + arm the watch.
            int fd = std::sysSignalOpen([sig]);
            if (fd < 0) throw RuntimeException("signal: cannot watch signal " + sig.toString());
            int wid = std::sysWatch(fd, (ready) => signal::deliver(fd));
            signal::st.sigs     = signal::st.sigs.add(sig);
            signal::st.fds      = signal::st.fds.add(fd);
            signal::st.watchIds = signal::st.watchIds.add(wid);
            signal::st.subs     = signal::st.subs.add([b]);
            signal::st.subIds   = signal::st.subIds.add([myId]);
        } else {
            signal::st.subs   = signal::st.subs.with(idx, signal::st.subs.at(idx).add(b));
            signal::st.subIds = signal::st.subIds.with(idx, signal::st.subIds.at(idx).add(myId));
        }
        InStream<int> s = InStream(b);
        s.onDispose = () => signal::off(sig, myId);
        s.hasDispose = true;
        return s;
    }

    // SU-1 R6/R7: unsubscribe. A free function (never a method on the `st`
    // eager global — that would drag the signal natives into every emit-C++
    // compile, techdesign-terminal-floor.md's regression). Idempotent: a
    // signal already fully torn down, or a subId already removed, is a
    // silent no-op — never a double sysSignalClose. No await/park point
    // anywhere in this function, so there is no reentry window between
    // fanout removal and last-out teardown.
    void off(int sig, int subId) {
        int idx = signal::findSig(sig);
        if (idx < 0) return;                   // signal already fully torn down: idempotent no-op
        Array<int> ids = signal::st.subIds.at(idx);
        int k = ids.indexOf(subId);
        if (k < 0) return;                     // this sub already removed: idempotent no-op
        signal::st.subs   = signal::st.subs.with(idx, signal::st.subs.at(idx).removeAt(k));
        signal::st.subIds = signal::st.subIds.with(idx, ids.removeAt(k));
        if (signal::st.subs.at(idx).length() == 0) {          // LAST subscriber: reclaim the source
            std::sysUnwatch(signal::st.watchIds.at(idx));     // 1. stop loop dispatch first
            std::sysSignalClose(signal::st.fds.at(idx));      // 2. unblock -> default disposition
            signal::st.sigs     = signal::st.sigs.removeAt(idx);   // 3. drop the row (all 5 arrays)
            signal::st.fds      = signal::st.fds.removeAt(idx);
            signal::st.watchIds = signal::st.watchIds.removeAt(idx);
            signal::st.subs     = signal::st.subs.removeAt(idx);
            signal::st.subIds   = signal::st.subIds.removeAt(idx);
        }
    }
}
)prelude";

// --- kPreludeRegexCore: Track 10 linear-time regular-expression engine ----
// Pure Leviathan code: deliberately no native methods.  The public Regex API
// is a separate prelude segment owned by techdesign-regex-library.md.
const char* kPreludeRegexCore = R"prelude(
namespace regex {
    // AST: 1 byte, 2 class, 3 any, 4 concat, 5 alt, 6 star, 7 plus,
    // 8 optional, 9 empty, 10 ^, 11 $, 12 word-boundary, 13 non-boundary.
    // VM: 1 byte, 2 class, 3 any, 4 any-with-newline, 5 split, 6 jump,
    // 7 assert, 8 accept, 9 save. A compiled program is a self-contained
    // Array<int>. Its fixed header also carries match-length and prefilter facts;
    // the tail carries byte-equivalence classes and encoded named groups.
    // Class bytes are 0/1 entries (rather than bitmaps):
    // this is intentionally comptime-reifiable and avoids signed-shift traps.

    // Engine internals, namespace-scoped (bug.md #32 fixed the qualified-
    // construction dispatch gap that once forced these to top level); not
    // part of the public Regex API (owned by techdesign-regex-library.md).
    // Every construction site below is qualified (regex::Name(...)) rather
    // than bare, since bare same-namespace construction (M2) stays deferred.
    class RegexCoreFragment {
        int start;
        Array<int> outs;
        new RegexCoreFragment(int s, Array<int> o) { start = s; outs = o; }
    }

    class RegexCoreCompiler {
        string pattern;
        int pos = 0;
        int depth = 0;
        int flags = 0; // i=1, m=2, s=4
        Array<int> nodeOp = [];
        Array<int> nodeA = [];
        Array<int> nodeB = [];
        Array<int> classes = [];
        int classCount = 0;
        int groupCount = 0;
        Array<int> nameData = [];
        Array<int> ops = [];
        Array<int> argA = [];
        Array<int> argB = [];

        new RegexCoreCompiler(string p, string f) {
            pattern = p;
            int i = 0;
            while (i < f.length()) {
                int c = f.byteAt(i);
                if (c == 105) flags = flags | 1;
                else if (c == 109) flags = flags | 2;
                else if (c == 115) flags = flags | 4;
                else throw RuntimeException("regex: unknown flag at offset " + i.toString());
                i = i + 1;
            }
        }

        int addNode(int op, int a, int b) {
            if (nodeOp.length() >= 4096) throw RuntimeException("regex: pattern too large");
            int n = nodeOp.length();
            nodeOp = nodeOp.add(op); nodeA = nodeA.add(a); nodeB = nodeB.add(b);
            return n;
        }
        int peek() => pos < pattern.length() ? pattern.byteAt(pos) : -1;
        int take() { int c = peek(); if (c >= 0) pos = pos + 1; return c; }
        bool atomStart(int c) => c >= 0 && c != 41 && c != 124;
        int fold(int c) => ((flags & 1) != 0 && c >= 65 && c <= 90) ? c + 32 : c;
        bool word(int c) => (c >= 48 && c <= 57) || (c >= 65 && c <= 90) ||
                            (c >= 97 && c <= 122) || c == 95;

        int newClass(bool inverted) {
            int idx = classCount;
            classCount = classCount + 1;
            int i = 0;
            while (i < 256) { classes = classes.add(inverted ? 1 : 0); i = i + 1; }
            return idx;
        }
        void classSet(int ci, int c, bool yes) {
            if (c >= 0 && c < 256) classes = classes.with(ci * 256 + c, yes ? 1 : 0);
            if ((flags & 1) != 0) {
                if (c >= 65 && c <= 90) classes = classes.with(ci * 256 + c + 32, yes ? 1 : 0);
                if (c >= 97 && c <= 122) classes = classes.with(ci * 256 + c - 32, yes ? 1 : 0);
            }
        }
        void classKind(int ci, int kind, bool yes) {
            int c = 0;
            while (c < 256) {
                bool hit = kind == 1 ? (c >= 48 && c <= 57) :
                    (kind == 2 ? word(c) : (c == 32 || c == 9 || c == 10 || c == 13 || c == 12));
                if (hit) classSet(ci, c, yes);
                c = c + 1;
            }
        }
        void classToken(int ci, int kind, bool negatedKind, bool outerInverted) {
            int c = 0;
            while (c < 256) {
                bool hit = kind == 1 ? (c >= 48 && c <= 57) :
                    (kind == 2 ? word(c) : (c == 32 || c == 9 || c == 10 || c == 13 || c == 12));
                bool selected = negatedKind ? !hit : hit;
                if (selected) classSet(ci, c, !outerInverted); c = c + 1;
            }
        }
        int escapeAtom() {
            if (pos >= pattern.length()) throw RuntimeException("regex: trailing escape at offset " + pos.toString());
            int e = take();
            if (e == 98) return addNode(12, 0, 0);
            if (e == 66) return addNode(13, 0, 0);
            if (e == 100 || e == 119 || e == 115 || e == 68 || e == 87 || e == 83) {
                bool inv = e == 68 || e == 87 || e == 83;
                int ci = newClass(inv);
                int k = (e == 100 || e == 68) ? 1 : ((e == 119 || e == 87) ? 2 : 3);
                classKind(ci, k, !inv);
                return addNode(2, ci, 0);
            }
            if (e == 110) e = 10; else if (e == 114) e = 13; else if (e == 116) e = 9;
            return addNode(1, fold(e), 0);
        }
        int classChar() {
            if (pos >= pattern.length()) throw RuntimeException("regex: unterminated class at offset " + pos.toString());
            int c = take();
            if (c != 92) return c;
            if (pos >= pattern.length()) throw RuntimeException("regex: trailing class escape at offset " + pos.toString());
            int e = take();
            if (e == 110) return 10; if (e == 114) return 13; if (e == 116) return 9;
            return e;
        }
        int parseClass() {
            take();
            bool inv = false;
            if (peek() == 94) { take(); inv = true; }
            int ci = newClass(inv);
            bool any = false;
            while (peek() >= 0 && peek() != 93) {
                if (peek() == 92 && pos + 1 < pattern.length()) {
                    int e = pattern.byteAt(pos + 1);
                    if (e == 100 || e == 119 || e == 115 || e == 68 || e == 87 || e == 83) {
                        pos = pos + 2;
                        bool negKind = e == 68 || e == 87 || e == 83;
                        int k = (e == 100 || e == 68) ? 1 : ((e == 119 || e == 87) ? 2 : 3);
                        classToken(ci, k, negKind, inv);
                        any = true; continue;
                    }
                }
                int lo = classChar(); int hi = lo;
                if (peek() == 45 && pos + 1 < pattern.length() && pattern.byteAt(pos + 1) != 93) {
                    take(); hi = classChar();
                    if (lo > hi) throw RuntimeException("regex: reversed class range at offset " + pos.toString());
                }
                int c = lo; while (c <= hi) { classSet(ci, c, !inv); c = c + 1; }
                any = true;
            }
            if (peek() != 93) throw RuntimeException("regex: unterminated class at offset " + pos.toString());
            take();
            if (!any) throw RuntimeException("regex: empty class at offset " + pos.toString());
            return addNode(2, ci, 0);
        }
        int parseAtom() {
            int c = peek();
            if (c == 40) {
                take(); depth = depth + 1;
                if (depth > 200) throw RuntimeException("regex: nesting too deep");
                bool capture = true; string groupName = "";
                if (peek() == 63) {
                    take();
                    if (peek() == 58) { take(); capture = false; }
                    else if (peek() == 60) {
                        take();
                        int nameStart = pos;
                        while (peek() >= 0 && peek() != 62) take();
                        if (peek() != 62) throw RuntimeException("regex: bad group name at offset " + pos.toString());
                        if (pos == nameStart) throw RuntimeException("regex: empty group name at offset " + pos.toString());
                        groupName = pattern.subStr(nameStart, pos - nameStart);
                        take();
                    } else throw RuntimeException("regex: unsupported group at offset " + pos.toString());
                }
                int gi = -1;
                if (capture) { groupCount = groupCount + 1; gi = groupCount; }
                int n = parseAlt();
                if (peek() != 41) throw RuntimeException("regex: unterminated group at offset " + pos.toString());
                take(); depth = depth - 1;
                if (groupName.length() > 0) {
                    nameData = nameData.add(gi).add(groupName.length());
                    int ni = 0; while (ni < groupName.length()) { nameData = nameData.add(groupName.byteAt(ni)); ni = ni + 1; }
                }
                return capture ? addNode(16, n, gi) : n;
            }
            if (c == 91) return parseClass();
            if (c == 92) { take(); return escapeAtom(); }
            take();
            if (c == 46) return addNode(3, 0, 0);
            if (c == 94) return addNode(10, 0, 0);
            if (c == 36) return addNode(11, 0, 0);
            if (c == 42 || c == 43 || c == 63 || c == 123)
                throw RuntimeException("regex: quantifier without atom at offset " + (pos - 1).toString());
            return addNode(1, fold(c), 0);
        }
        int number() {
            if (peek() < 48 || peek() > 57) return -1;
            int n = 0;
            while (peek() >= 48 && peek() <= 57) { n = n * 10 + take() - 48; if (n > 1000) throw RuntimeException("regex: repetition too large"); }
            return n;
        }
        int parseRepeat() {
            int n = parseAtom();
            bool again = true;
            while (again) {
                int c = peek(); int op = 0;
                if (c == 42) { take(); op = 6; }
                else if (c == 43) { take(); op = 7; }
                else if (c == 63) { take(); op = 8; }
                else if (c == 123) {
                    take(); int lo = number(); if (lo < 0) throw RuntimeException("regex: bad repetition at offset " + pos.toString());
                    int hi = lo;
                    if (peek() == 44) { take(); hi = number(); }
                    if (peek() != 125 || (hi >= 0 && hi < lo)) throw RuntimeException("regex: bad repetition at offset " + pos.toString());
                    take(); n = addNode(14, n, lo * 1002 + (hi + 1)); op = 0;
                } else again = false;
                if (op != 0) n = addNode(op, n, 0);
                if (op != 0 || c == 123) {
                    if (peek() == 63) { take(); n = addNode(15, n, 0); }
                    if (peek() == 42 || peek() == 43 || peek() == 63 || peek() == 123)
                        throw RuntimeException("regex: repeated quantifier at offset " + pos.toString());
                }
            }
            return n;
        }
        int parseConcat() {
            int n = -1;
            while (atomStart(peek())) { int r = parseRepeat(); n = n < 0 ? r : addNode(4, n, r); }
            return n < 0 ? addNode(9, 0, 0) : n;
        }
        int parseAlt() {
            int n = parseConcat();
            while (peek() == 124) { take(); n = addNode(5, n, parseConcat()); }
            return n;
        }
        int emit(int op, int a, int b) {
            if (ops.length() >= 4096) throw RuntimeException("regex: program too large");
            int pc = ops.length(); ops = ops.add(op); argA = argA.add(a); argB = argB.add(b); return pc;
        }
        Array<int> oneOut(int pc, int side) { Array<int> r = []; return r.add(pc * 2 + side); }
        void patch(Array<int> out, int target) {
            for (int q in out) { int pc = q / 2; if (q % 2 == 0) argA = argA.with(pc, target); else argB = argB.with(pc, target); }
        }
        RegexCoreFragment build(int id, bool lazy) {
            int o = nodeOp.at(id);
            if (o == 1) { int p = emit(1, nodeA.at(id), -1); return regex::RegexCoreFragment(p, oneOut(p, 1)); }
            if (o == 2) { int p = emit(2, nodeA.at(id), -1); return regex::RegexCoreFragment(p, oneOut(p, 1)); }
            if (o == 3) { int p = emit((flags & 4) != 0 ? 4 : 3, -1, 0); return regex::RegexCoreFragment(p, oneOut(p, 0)); }
            if (o >= 10 && o <= 13) { int p = emit(7, o - 10, -1); return regex::RegexCoreFragment(p, oneOut(p, 1)); }
            if (o == 9) { int p = emit(6, -1, 0); return regex::RegexCoreFragment(p, oneOut(p, 0)); }
            if (o == 4) { RegexCoreFragment x = build(nodeA.at(id), lazy); RegexCoreFragment y = build(nodeB.at(id), lazy); patch(x.outs, y.start); return regex::RegexCoreFragment(x.start, y.outs); }
            if (o == 5) { RegexCoreFragment x = build(nodeA.at(id), lazy); RegexCoreFragment y = build(nodeB.at(id), lazy); int p = emit(5, x.start, y.start); return regex::RegexCoreFragment(p, x.outs.concat(y.outs)); }
            if (o == 15) return build(nodeA.at(id), true);
            if (o == 16) {
                int before = emit(9, nodeB.at(id) * 2, -1);
                RegexCoreFragment x = build(nodeA.at(id), lazy);
                int after = emit(9, nodeB.at(id) * 2 + 1, -1);
                argB = argB.with(before, x.start); patch(x.outs, after);
                return regex::RegexCoreFragment(before, oneOut(after, 1));
            }
            if (o == 6 || o == 7 || o == 8) {
                RegexCoreFragment x = build(nodeA.at(id), lazy); int p;
                if (lazy) p = emit(5, -1, x.start); else p = emit(5, x.start, -1);
                if (o == 6 || o == 7) patch(x.outs, p);
                if (o == 7) return regex::RegexCoreFragment(x.start, oneOut(p, lazy ? 0 : 1));
                if (o == 8) return regex::RegexCoreFragment(p, x.outs.concat(oneOut(p, lazy ? 0 : 1)));
                return regex::RegexCoreFragment(p, oneOut(p, lazy ? 0 : 1));
            }
            // Bounded repetition node: b packs min and max+1; max=-1 is open.
            if (o == 14) {
                int lo = nodeB.at(id) / 1002; int hi = nodeB.at(id) % 1002 - 1;
                RegexCoreFragment result = regex::RegexCoreFragment(-1, []); int i = 0;
                while (i < lo) { RegexCoreFragment z = build(nodeA.at(id), lazy); if (result.start < 0) result = z; else { patch(result.outs, z.start); result = regex::RegexCoreFragment(result.start, z.outs); } i = i + 1; }
                if (hi < 0) { RegexCoreFragment z = build(addNode(6, nodeA.at(id), 0), lazy); if (result.start < 0) return z; patch(result.outs, z.start); return regex::RegexCoreFragment(result.start, z.outs); }
                while (i < hi) { RegexCoreFragment z = build(addNode(8, nodeA.at(id), 0), lazy); if (result.start < 0) result = z; else { patch(result.outs, z.start); result = regex::RegexCoreFragment(result.start, z.outs); } i = i + 1; }
                if (result.start < 0) return build(addNode(9, 0, 0), lazy);
                return result;
            }
            throw RuntimeException("regex: internal compiler error");
        }
        int minLength(int id) {
            int o = nodeOp.at(id);
            if (o >= 1 && o <= 3) return 1;
            if (o == 9 || (o >= 10 && o <= 13)) return 0;
            if (o == 4) return minLength(nodeA.at(id)) + minLength(nodeB.at(id));
            if (o == 5) { int a = minLength(nodeA.at(id)); int b = minLength(nodeB.at(id)); return a < b ? a : b; }
            if (o == 6 || o == 8) return 0;
            if (o == 7) return minLength(nodeA.at(id));
            if (o == 14) return minLength(nodeA.at(id)) * (nodeB.at(id) / 1002);
            if (o == 15 || o == 16) return minLength(nodeA.at(id));
            return 0;
        }
        int maxLength(int id) {
            int o = nodeOp.at(id);
            if (o >= 1 && o <= 3) return 1;
            if (o == 9 || (o >= 10 && o <= 13)) return 0;
            if (o == 4) { int a = maxLength(nodeA.at(id)); int b = maxLength(nodeB.at(id)); return a < 0 || b < 0 ? -1 : a + b; }
            if (o == 5) { int a = maxLength(nodeA.at(id)); int b = maxLength(nodeB.at(id)); return a < 0 || b < 0 ? -1 : (a > b ? a : b); }
            if (o == 6) return maxLength(nodeA.at(id)) == 0 ? 0 : -1;
            if (o == 7) return maxLength(nodeA.at(id)) == 0 ? 0 : -1;
            if (o == 8) return maxLength(nodeA.at(id));
            if (o == 14) { int hi = nodeB.at(id) % 1002 - 1; int a = maxLength(nodeA.at(id)); return hi < 0 || a < 0 ? -1 : hi * a; }
            if (o == 15 || o == 16) return maxLength(nodeA.at(id));
            return 0;
        }
        bool isLiteral(int id) {
            int o = nodeOp.at(id);
            if (o == 1 || o == 9) return true;
            if (o == 4) return isLiteral(nodeA.at(id)) && isLiteral(nodeB.at(id));
            if (o == 16) return isLiteral(nodeA.at(id));
            return false;
        }
        Array<int> literalBytes(int id) {
            int o = nodeOp.at(id); Array<int> r = [];
            if (o == 1) return r.add(nodeA.at(id));
            if (o == 4) return literalBytes(nodeA.at(id)).concat(literalBytes(nodeB.at(id)));
            if (o == 16) return literalBytes(nodeA.at(id));
            return r;
        }
        Array<int> requiredPrefix(int id) {
            Array<int> none = []; if ((flags & 1) != 0) return none;
            int o = nodeOp.at(id);
            if (o == 1) return none.add(nodeA.at(id));
            if (o == 16 || o == 15) return requiredPrefix(nodeA.at(id));
            if (o == 4) {
                Array<int> a = requiredPrefix(nodeA.at(id));
                if (isLiteral(nodeA.at(id))) return literalBytes(nodeA.at(id)).concat(requiredPrefix(nodeB.at(id)));
                return a;
            }
            return none;
        }
        bool startsAnchored(int id) {
            int o = nodeOp.at(id);
            if (o == 10) return true;
            if (o == 16 || o == 15) return startsAnchored(nodeA.at(id));
            if (o == 4) return startsAnchored(nodeA.at(id));
            return false;
        }
        int firstExact(int id) {
            if ((flags & 1) != 0) return -1; int o = nodeOp.at(id);
            if (o == 1) return nodeA.at(id);
            if (o == 2) { int only = -1; int c = 0; while (c < 256) { if (classes.at(nodeA.at(id) * 256 + c) != 0) { if (only >= 0) return -1; only = c; } c = c + 1; } return only; }
            if (o == 16 || o == 15) return firstExact(nodeA.at(id));
            if (o == 4) { int a = firstExact(nodeA.at(id)); return minLength(nodeA.at(id)) > 0 ? a : -1; }
            return -1;
        }
        bool hasAssert(int id) {
            int o = nodeOp.at(id);
            if (o >= 10 && o <= 13) return true;
            if (o == 4 || o == 5) return hasAssert(nodeA.at(id)) || hasAssert(nodeB.at(id));
            if (o >= 6 && o <= 8 || o == 14 || o == 15 || o == 16) return hasAssert(nodeA.at(id));
            return false;
        }
        bool sameByte(int x, int y) {
            int pc = 0;
            while (pc < ops.length()) {
                int o = ops.at(pc); bool a = false; bool b = false;
                if (o == 1) { a = argA.at(pc) == fold(x); b = argA.at(pc) == fold(y); }
                else if (o == 2) { a = classes.at(argA.at(pc) * 256 + x) != 0; b = classes.at(argA.at(pc) * 256 + y) != 0; }
                else if (o == 3) { a = x != 10; b = y != 10; }
                else if (o == 4) { a = true; b = true; }
                if (a != b) return false; pc = pc + 1;
            }
            return true;
        }
        Array<int> equivalenceMap() {
            Array<int> map = Array(256, -1); int next = 0; int c = 0;
            while (c < 256) {
                int rep = -1; int r = 0;
                while (r < c && rep < 0) { if (sameByte(c, r)) rep = r; r = r + 1; }
                if (rep >= 0) map = map.with(c, map.at(rep));
                else { map = map.with(c, next); next = next + 1; }
                c = c + 1;
            }
            return map;
        }
        Array<int> compile() {
            int root = parseAlt();
            if (pos != pattern.length()) throw RuntimeException("regex: unexpected token at offset " + pos.toString());
            int wrapped = addNode(16, root, 0);
            RegexCoreFragment f = build(wrapped, false); int accept = emit(8, 0, 0); patch(f.outs, accept);
            // Header v2: magic,n,classes,flags,start,groups,min,max,anchored,
            // literalOnly,prefixLen,firstByte,equivCount,nameInts,dfaSafe,version.
            // Conservative length/prefilter analysis is filled by helpers below.
            int mn = minLength(root); int mx = maxLength(root);
            Array<int> prefix = requiredPrefix(root); bool literal = isLiteral(root) && (flags & 1) == 0;
            int first = prefix.length() > 0 ? prefix.at(0) : firstExact(root);
            bool anchored = startsAnchored(root);
            Array<int> eq = equivalenceMap(); int eqCount = 0;
            for (int q in eq) if (q + 1 > eqCount) eqCount = q + 1;
            bool dfaSafe = !hasAssert(root);
            Array<int> r = [1380271952, ops.length(), classCount, flags, f.start,
                groupCount, mn, mx, anchored ? 1 : 0, literal ? 1 : 0,
                prefix.length(), first, eqCount, nameData.length(), dfaSafe ? 1 : 0, 2];
            r = r.concat(ops).concat(argA).concat(argB).concat(classes).concat(prefix).concat(eq).concat(nameData); return r;
        }
    }

    class RegexCoreVm {
        Array<int> p; int n; int cc; int flags; int start; int groups; int minLen; int maxLen;
        int anchored; int literalOnly; int prefixLen; int firstByte; int eqCount; int dfaSafe;
        int opOff; int aOff; int bOff; int clsOff; int prefixOff; int eqOff; int nameOff;
        Array<Array<int>> dfaStates = [];
        Array<int> dfaAccept = [];
        Array<int> dfaTrans = [];
        Map<string, int> dfaIds = Map();
        int scanOverflows = 0;
        int dfaBudget = 512;
        // techdesign-regex-linear-gate.md D1: deterministic work counter for the
        // Pike leg — one increment per (pc, position) examination. Ratio-checked
        // by regex_pathological_linear to prove O(n) scaling without wall-clock.
        int pikeSteps = 0;
        new RegexCoreVm(Array<int> program) {
            if (program.length() < 16 || program.at(0) != 1380271952 || program.at(15) != 2) throw RuntimeException("regex: invalid program");
            p = program; n = p.at(1); cc = p.at(2); flags = p.at(3); start = p.at(4);
            groups = p.at(5); minLen = p.at(6); maxLen = p.at(7); anchored = p.at(8);
            literalOnly = p.at(9); prefixLen = p.at(10); firstByte = p.at(11);
            eqCount = p.at(12); dfaSafe = p.at(14);
            opOff = 16; aOff = opOff + n; bOff = aOff + n; clsOff = bOff + n;
            prefixOff = clsOff + cc * 256; eqOff = prefixOff + prefixLen;
            nameOff = eqOff + 256;
            if (p.length() != nameOff + p.at(13)) throw RuntimeException("regex: invalid program");
        }
        bool wordAt(string s, int i) {
            if (i < 0 || i >= s.length()) return false; int c = s.byteAt(i);
            return (c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c == 95;
        }
        bool assertion(int k, string s, int pos) {
            if (k == 0) return pos == 0 || ((flags & 2) != 0 && pos > 0 && s.byteAt(pos - 1) == 10);
            if (k == 1) return pos == s.length() || ((flags & 2) != 0 && pos < s.length() && s.byteAt(pos) == 10);
            bool b = wordAt(s, pos - 1) != wordAt(s, pos); return k == 2 ? b : !b;
        }
        Array<int> closure(Array<int> seed, string s, int pos) {
            Array<int> out = []; Array<int> stack = seed; Array<int> seen = Array(n, 0);
            int i = 0;
            while (i < stack.length()) {
                int pc = stack.at(i); i = i + 1;
                if (pc < 0 || pc >= n || seen.at(pc) != 0) continue;
                seen = seen.with(pc, 1); int op = p.at(opOff + pc);
                if (op == 5) { stack = stack.add(p.at(aOff + pc)); stack = stack.add(p.at(bOff + pc)); }
                else if (op == 6) stack = stack.add(p.at(aOff + pc));
                else if (op == 7) { if (assertion(p.at(aOff + pc), s, pos)) stack = stack.add(p.at(bOff + pc)); }
                else if (op == 9) stack = stack.add(p.at(bOff + pc));
                else out = out.add(pc);
            }
            return out;
        }
        bool accepted(Array<int> state) { for (int pc in state) if (p.at(opOff + pc) == 8) return true; return false; }
        bool oldIsMatch(string s, int from) {
            int pos = from < 0 ? 0 : from; Array<int> state = [];
            while (pos <= s.length()) {
                state = closure(state.add(start), s, pos); // one-pass unanchored NFA
                if (accepted(state)) return true;
                if (pos == s.length()) return false;
                int c = s.byteAt(pos); int fc = ((flags & 1) != 0 && c >= 65 && c <= 90) ? c + 32 : c;
                Array<int> next = [];
                for (int pc in state) {
                    int op = p.at(opOff + pc); bool hit = false;
                    if (op == 1) hit = p.at(aOff + pc) == fc;
                    else if (op == 2) hit = p.at(clsOff + p.at(aOff + pc) * 256 + c) != 0;
                    else if (op == 3) hit = c != 10; else if (op == 4) hit = true;
                    if (hit) next = next.add((op == 1 || op == 2) ? p.at(bOff + pc) : p.at(aOff + pc));
                }
                state = next; pos = pos + 1;
            }
            return false;
        }

        string stateKey(Array<int> state) {
            Array<string> bits = []; int pc = 0;
            while (pc < n) { bits = bits.add(std::byteToString(state.contains(pc) ? 49 : 48)); pc = pc + 1; }
            return bits.concatAll();
        }
        void clearDfa() { dfaStates = []; dfaAccept = []; dfaTrans = []; dfaIds = Map(); }
        int internDfa(Array<int> state) {
            string key = stateKey(state); if (dfaIds.has(key)) return dfaIds[key];
            if (dfaStates.length() >= dfaBudget) {
                scanOverflows = scanOverflows + 1; clearDfa();
                if (scanOverflows > 1) return -1;
            }
            int id = dfaStates.length(); dfaStates = dfaStates.add(state);
            dfaAccept = dfaAccept.add(accepted(state) ? 1 : 0);
            int k = 0; while (k < eqCount) { dfaTrans = dfaTrans.add(-1); k = k + 1; }
            dfaIds[key] = id; return id;
        }
        Array<int> dfaExpanded(Array<int> raw, string s, int pos) => closure(raw.add(start), s, pos);
        int dfaStep(int id, int c, string s, int pos) {
            int bc = p.at(eqOff + c); int ti = id * eqCount + bc; int cached = dfaTrans.at(ti);
            if (cached >= 0) return cached;
            int generation = scanOverflows;
            Array<int> next = [];
            for (int pc in dfaStates.at(id)) {
                int op = p.at(opOff + pc); bool hit = false;
                if (op == 1) { int fc = ((flags & 1) != 0 && c >= 65 && c <= 90) ? c + 32 : c; hit = p.at(aOff + pc) == fc; }
                else if (op == 2) hit = p.at(clsOff + p.at(aOff + pc) * 256 + c) != 0;
                else if (op == 3) hit = c != 10; else if (op == 4) hit = true;
                if (hit) next = next.add((op == 1 || op == 2) ? p.at(bOff + pc) : p.at(aOff + pc));
            }
            int nid = internDfa(dfaExpanded(next, s, pos + 1));
            // A cache flush invalidates id/ti; only memoize if the old row survived.
            if (nid >= 0 && generation == scanOverflows) dfaTrans = dfaTrans.with(ti, nid);
            return nid;
        }
        bool dfaIsMatch(string s, int from) {
            if (dfaSafe == 0) return pikeIsMatch(s, from);
            int begin = from < 0 ? 0 : from; if (begin > s.length() || s.length() - begin < minLen) return false;
            scanOverflows = 0; int id = internDfa(dfaExpanded([], s, begin)); int pos = begin;
            while (id >= 0 && pos <= s.length()) {
                if (dfaAccept.at(id) != 0) return true;
                if (pos == s.length()) return false;
                id = dfaStep(id, s.byteAt(pos), s, pos); pos = pos + 1;
            }
            return pikeIsMatch(s, from);
        }

        Array<int> flatPc = []; Array<int> flatCaps = [];
        Array<int> stackPc = []; Array<int> stackCaps = []; int stackTop = 0;
        int capWidth() => (groups + 1) * 2;
        Array<int> capAt(Array<int> bank, int row) {
            Array<int> c = []; int j = 0; int w = capWidth();
            while (j < w) { c = c.add(bank.at(row * w + j)); j = j + 1; } return c;
        }
        Array<int> capAdd(Array<int> bank, Array<int> c) { for (int v in c) bank = bank.add(v); return bank; }
        void stackPush(int pc, Array<int> caps) {
            if (stackTop < stackPc.length()) stackPc = stackPc.with(stackTop, pc); else stackPc = stackPc.add(pc);
            int w = capWidth(); int j = 0;
            while (j < w) { int at = stackTop * w + j; if (at < stackCaps.length()) stackCaps = stackCaps.with(at, caps.at(j)); else stackCaps = stackCaps.add(caps.at(j)); j = j + 1; }
            stackTop = stackTop + 1;
        }
        void closureFlat(Array<int> seeds, Array<int> seedCaps, string s, int pos) {
            flatPc = []; flatCaps = []; Array<int> seen = Array(n, 0); int si = 0;
            while (si < seeds.length()) {
                stackPc = []; stackCaps = []; stackTop = 0; stackPush(seeds.at(si), capAt(seedCaps, si));
                while (stackTop > 0) {
                    pikeSteps = pikeSteps + 1;   // D1: one closure pop examined
                    stackTop = stackTop - 1; int pc = stackPc.at(stackTop); Array<int> caps = capAt(stackCaps, stackTop);
                    if (pc < 0 || pc >= n || seen.at(pc) != 0) continue;
                    seen = seen.with(pc, 1); int op = p.at(opOff + pc);
                    if (op == 5) { stackPush(p.at(bOff + pc), caps); stackPush(p.at(aOff + pc), caps); }
                    else if (op == 6) stackPush(p.at(aOff + pc), caps);
                    else if (op == 7) { if (assertion(p.at(aOff + pc), s, pos)) stackPush(p.at(bOff + pc), caps); }
                    else if (op == 9) { caps = caps.with(p.at(aOff + pc), pos); stackPush(p.at(bOff + pc), caps); }
                    else { flatPc = flatPc.add(pc); flatCaps = capAdd(flatCaps, caps); }
                }
                si = si + 1;
            }
        }
        Array<int> literalResult(string s, int from) {
            Array<int> bytes = []; int i = 0;
            while (i < prefixLen) { bytes = bytes.add(p.at(prefixOff + i)); i = i + 1; }
            Array<string> parts = []; for (int b in bytes) parts = parts.add(std::byteToString(b));
            string lit = parts.concatAll(); int at = lit.length() == 0 ? from : s.indexOfFrom(lit, from);
            if (at < 0 || (anchored != 0 && (flags & 2) == 0 && at != 0)) return [];
            Array<int> caps = Array((groups + 1) * 2, -1); caps = caps.with(0, at).with(1, at + lit.length()); return caps;
        }
        Array<int> pikeFindRaw(string s, int from, bool onlyAtFrom) {
            int pos = from < 0 ? 0 : from; Array<int> state = []; Array<int> stateCaps = [];
            Array<int> best = []; bool mayStart = true;
            while (pos <= s.length()) {
                if (state.length() == 0 && best.length() > 0) return best;
                Array<int> seeds = state; Array<int> seedCaps = stateCaps;
                if (mayStart) { seeds = seeds.add(start); seedCaps = capAdd(seedCaps, Array(capWidth(), -1)); }
                closureFlat(seeds, seedCaps, s, pos); state = flatPc; stateCaps = flatCaps;
                int ai = -1; int j = 0;
                while (j < state.length() && ai < 0) { if (p.at(opOff + state.at(j)) == 8) ai = j; j = j + 1; }
                if (ai == 0) return capAt(stateCaps, 0);
                if (ai > 0) {
                    best = capAt(stateCaps, ai); state = state.take(ai);
                    Array<int> kept = []; int z = 0; while (z < ai * capWidth()) { kept = kept.add(stateCaps.at(z)); z = z + 1; } stateCaps = kept; mayStart = false;
                }
                if (pos == s.length()) return best;
                int c = s.byteAt(pos); int fc = ((flags & 1) != 0 && c >= 65 && c <= 90) ? c + 32 : c;
                Array<int> next = []; Array<int> nextCaps = []; int ti = 0;
                while (ti < state.length()) {
                    pikeSteps = pikeSteps + 1;   // D1: one thread transition tested
                    int pc = state.at(ti); int op = p.at(opOff + pc); bool hit = false;
                    if (op == 1) hit = p.at(aOff + pc) == fc;
                    else if (op == 2) hit = p.at(clsOff + p.at(aOff + pc) * 256 + c) != 0;
                    else if (op == 3) hit = c != 10; else if (op == 4) hit = true;
                    if (hit) { next = next.add((op == 1 || op == 2) ? p.at(bOff + pc) : p.at(aOff + pc)); nextCaps = capAdd(nextCaps, capAt(stateCaps, ti)); }
                    ti = ti + 1;
                }
                state = next; stateCaps = nextCaps; pos = pos + 1; if (onlyAtFrom) mayStart = false;
            }
            return best;
        }
        string prefixString() {
            Array<string> parts = []; int i = 0;
            while (i < prefixLen) { parts = parts.add(std::byteToString(p.at(prefixOff + i))); i = i + 1; }
            return parts.concatAll();
        }
        Array<int> find(string s, int from) {
            int begin = from < 0 ? 0 : from;
            if (begin > s.length() || s.length() - begin < minLen) return [];
            if (literalOnly != 0) return literalResult(s, begin);
            if (anchored != 0 && (flags & 2) == 0) return begin > 0 ? [] : pikeFindRaw(s, 0, true);
            if (prefixLen > 0) {
                string pre = prefixString(); int at = s.indexOfFrom(pre, begin);
                while (at >= 0) { Array<int> r = pikeFindRaw(s, at, true); if (r.length() > 0) return r; at = s.indexOfFrom(pre, at + 1); }
                return [];
            }
            if (firstByte >= 0) {
                string one = std::byteToString(firstByte); int at = s.indexOfFrom(one, begin);
                while (at >= 0) { Array<int> r = pikeFindRaw(s, at, true); if (r.length() > 0) return r; at = s.indexOfFrom(one, at + 1); }
                return [];
            }
            return pikeFindRaw(s, begin, false);
        }
        bool isMatch(string s, int from) {
            if (literalOnly != 0 || prefixLen > 0 || anchored != 0 || dfaSafe == 0) return find(s, from).length() > 0;
            return dfaIsMatch(s, from);
        }
        bool pikeIsMatch(string s, int from) => find(s, from).length() > 0;
        // techdesign-regex-linear-gate.md D2: run the Pike leg from 0 and report
        // work done — [matched(0/1), pikeSteps]. Diagnostics/testing only.
        Array<int> pikeProbe(string s) {
            pikeSteps = 0;
            bool hit = pikeIsMatch(s, 0);
            int h = hit ? 1 : 0;
            return [h, pikeSteps];
        }
        int count(string s) {
            int total = 0; int pos = 0;
            while (pos <= s.length()) {
                Array<int> m = find(s, pos); if (m.length() == 0) return total;
                total = total + 1; int end = m.at(1); pos = end > m.at(0) ? end : end + 1;
            }
            return total;
        }
        int groupIndex(string name) {
            int i = nameOff; int end = nameOff + p.at(13);
            while (i < end) {
                int gi = p.at(i); int len = p.at(i + 1); i = i + 2;
                bool same = len == name.length(); int j = 0;
                while (j < len) { if (same && p.at(i + j) != name.byteAt(j)) same = false; j = j + 1; }
                if (same) return gi; i = i + len;
            }
            return -1;
        }
        int groupTotal() => groups;
        int minimumLength() => minLen;
        int maximumLength() => maxLen;
        int dfaOverflowCount(string s) { dfaIsMatch(s, 0); return scanOverflows; }
        int dfaOverflowCountWithBudget(string s, int budget) { dfaBudget = budget; dfaIsMatch(s, 0); return scanOverflows; }
        // Raw (gi,len,byte...) named-group records, for doc 2 (techdesign-
        // regex-library.md)'s Regex.groupNames()/Match named-lookup: an
        // additive accessor alongside groupIndex above, no behavior change.
        Array<int> nameRecords() {
            Array<int> out = []; int i = nameOff; int end = nameOff + p.at(13);
            while (i < end) { out = out.add(p.at(i)); i = i + 1; }
            return out;
        }
    }

    Array<int> compileProgram(string pattern, string flags) => regex::RegexCoreCompiler(pattern, flags).compile();
    bool programIsMatch(Array<int> program, string input) => regex::RegexCoreVm(program).isMatch(input, 0);
    bool programIsMatchFrom(Array<int> program, string input, int from) => regex::RegexCoreVm(program).isMatch(input, from);
    Array<int> programFind(Array<int> program, string input) => regex::RegexCoreVm(program).find(input, 0);
    Array<int> programFindFrom(Array<int> program, string input, int from) => regex::RegexCoreVm(program).find(input, from);
    bool programIsMatchPike(Array<int> program, string input) => regex::RegexCoreVm(program).pikeIsMatch(input, 0);
    // techdesign-regex-linear-gate.md: [matched(0/1), pikeSteps] — the Pike leg's
    // deterministic work count, for the linearity gate. Diagnostics-only internal.
    Array<int> programPikeProbe(Array<int> program, string input) => regex::RegexCoreVm(program).pikeProbe(input);
    bool programIsMatchDfa(Array<int> program, string input) => regex::RegexCoreVm(program).dfaIsMatch(input, 0);
    int programCount(Array<int> program, string input) => regex::RegexCoreVm(program).count(input);
    int programGroupCount(Array<int> program) => regex::RegexCoreVm(program).groupTotal();
    int programGroupIndex(Array<int> program, string name) => regex::RegexCoreVm(program).groupIndex(name);
    Array<int> programNameData(Array<int> program) => regex::RegexCoreVm(program).nameRecords();
    int programMinLength(Array<int> program) => regex::RegexCoreVm(program).minimumLength();
    int programMaxLength(Array<int> program) => regex::RegexCoreVm(program).maximumLength();
    int programDfaOverflowCount(Array<int> program, string input) => regex::RegexCoreVm(program).dfaOverflowCount(input);
    int programDfaOverflowCountWithBudget(Array<int> program, string input, int budget) => regex::RegexCoreVm(program).dfaOverflowCountWithBudget(input, budget);
    Array<bool> programIsMatchAll(Array<int> program, Array<string> rows) {
        RegexCoreVm vm = regex::RegexCoreVm(program); Array<bool> out = [];
        for (string row in rows) out = out.add(vm.isMatch(row, 0)); return out;
    }
    Array<int> programCountAll(Array<int> program, Array<string> rows) {
        RegexCoreVm vm = regex::RegexCoreVm(program); Array<int> out = [];
        for (string row in rows) out = out.add(vm.count(row)); return out;
    }
}
)prelude";

// --- kPreludeRegexApi: Track 10 regex public surface ---------------------
// techdesign-regex-library.md (doc 2, fronting kPreludeRegexCore's engine).
// Public types are top-level (qualified type names don't parse in type
// position yet, doc 1 §7 problem 6); the C#-static-style conveniences and all
// non-public plumbing (helper functions prefixed `api*`) re-open `namespace
// regex` alongside the engine. Every internal call is fully qualified
// (`regex::apiFoo(...)`) even from within the same namespace, and every
// method body here only ever calls a DISTINCTLY NAMED helper rather than a
// same-type overloaded sibling — the bug #13 discipline doc 1 §7 problem 3
// established (overloads are only resolved safely from checked user code).
const char* kPreludeRegexApi = R"prelude(
struct RegexOptions {
    bool ignoreCase = false;
    bool multiline = false;
    bool dotAll = false;
    new RegexOptions(bool ignoreCase = false, bool multiline = false, bool dotAll = false) {
        this.ignoreCase = ignoreCase;
        this.multiline = multiline;
        this.dotAll = dotAll;
    }
}

// An unparticipating group inside a successful match is a real state
// (alternation arms) — matched stays false, index -1, value "".
struct Group {
    bool matched = false;
    int index = -1;
    int length = 0;
    string value = "";
}

struct Match {
    int index = -1;
    int length = 0;
    string value = "";
    Array<Group> groups = [];
    // Internal: name -> group index, so group(name) works on a detached
    // value with no back-pointer to the compiling Regex (doc 2 §1.2).
    Map<string, int> nameIndex = Map();

    Group groupAt(int i) => groups.at(i);   // distinct name; OOB throws like Array.at
    Group group(int i) => groupAt(i);
    Group? group(string name) {
        if (!nameIndex.has(name)) return None;
        return groupAt(nameIndex.at(name));
    }
}

class RegexException : Exception {
    int offset = -1;
    new RegexException(string msg, int off) {
        Exception::Exception(msg);
        offset = off;
    }
}

class Regex {
    Array<int> program = [];
    string patternText = "";
    RegexOptions opts = RegexOptions();

    new Regex(string pattern) {
        patternText = pattern;
        opts = RegexOptions();
        program = regex::apiCompileOrThrow(pattern, "");
    }
    new Regex(string pattern, RegexOptions options) {
        patternText = pattern;
        opts = options;
        program = regex::apiCompileOrThrow(pattern, regex::apiFlagsString(options));
    }
    new Regex(string pattern, string flags) {
        patternText = pattern;
        opts = regex::apiOptionsFromFlags(flags);
        program = regex::apiCompileOrThrow(pattern, flags);
    }
    // comptime path (doc 1 §3): O(1) wrap of an already-compiled program.
    new FromProgram(Array<int> prog) {
        patternText = "";
        opts = RegexOptions();
        program = prog;
    }

    string pattern() => patternText;
    RegexOptions options() => opts;
    int groupCount() => regex::programGroupCount(program);
    Array<string> groupNames() => regex::apiGroupNames(program);

    bool isMatch(string s) => regex::programIsMatch(program, s);
    bool isMatch(string s, int from) => regex::programIsMatchFrom(program, s, from);
    Match? find(string s) => regex::apiFind(program, s, 0);
    Match? find(string s, int from) => regex::apiFind(program, s, from);
    Array<Match> matches(string s) => regex::apiMatches(program, s);
    int count(string s) => regex::programCount(program, s);

    // NOTE: the lambda-typed overloads are declared BEFORE their string-typed
    // siblings — see the bug-report note by apiCachedProgram below (a lambda
    // literal argument is wrongly scored as applicable to a `string`
    // parameter too; declaration order is the tie-break, so the intended
    // overload must come first). Purely a source-order workaround — the
    // public signatures/behavior match doc 2 exactly either way.
    string replace(string s, (Match) => string fn) => regex::apiReplaceFn(program, s, fn, -1);
    string replace(string s, (Match) => string fn, int count) => regex::apiReplaceFn(program, s, fn, count);
    string replace(string s, string replacement) => regex::apiReplaceLiteral(program, s, replacement, -1);
    string replace(string s, string replacement, int count) => regex::apiReplaceLiteral(program, s, replacement, count);

    Array<string> split(string s) => regex::apiSplit(program, s, -1);
    Array<string> split(string s, int limit) => regex::apiSplit(program, s, limit);

    Array<bool> isMatchAll(Array<string> rows) => regex::programIsMatchAll(program, rows);
    Array<int> countAll(Array<string> rows) => regex::programCountAll(program, rows);
}

namespace regex {
    // ---- flags/options plumbing --------------------------------------
    string apiFlagsString(RegexOptions o) {
        StringBuilder sb = StringBuilder();
        if (o.ignoreCase) sb.add("i");
        if (o.multiline) sb.add("m");
        if (o.dotAll) sb.add("s");
        return sb.toString();
    }
    RegexOptions apiOptionsFromFlags(string flags) {
        bool ic = false; bool ml = false; bool da = false;
        int i = 0;
        while (i < flags.length()) {
            int c = flags.byteAt(i);
            if (c == 105) ic = true;
            else if (c == 109) ml = true;
            else if (c == 115) da = true;
            i = i + 1;
        }
        return RegexOptions(ic, ml, da);
    }

    // ---- §2.1 error-path split: pattern-as-code throws RegexException --
    // (compileProgram's parser throws a RuntimeException carrying "... at
    // offset N" in its message; apiOffsetOf recovers N for the public
    // exception type, defaulting to -1 when a message has no offset.)
    int apiOffsetOf(string msg) {
        string marker = "offset ";
        int at = msg.indexOf(marker);
        if (at < 0) return -1;
        int i = at + marker.length();
        int n = msg.length();
        bool neg = false;
        if (i < n && msg.byteAt(i) == 45) { neg = true; i = i + 1; }
        int start = i;
        int val = 0;
        while (i < n) {
            int c = msg.byteAt(i);
            if (c < 48 || c > 57) break;
            val = val * 10 + (c - 48);
            i = i + 1;
        }
        if (i == start) return -1;
        return neg ? 0 - val : val;
    }
    Array<int> apiCompileOrThrow(string pattern, string flags) {
        try {
            return regex::compileProgram(pattern, flags);
        } catch (RuntimeException e) {
            throw RegexException(e.message, regex::apiOffsetOf(e.message));
        }
    }
    // pattern-as-data path: None on malformed, never a throw.
    Regex? apiCompileSafe(string pattern, string flags) {
        try {
            Array<int> prog = regex::compileProgram(pattern, flags);
            return Regex::FromProgram(prog);
        } catch (RuntimeException e) {
            return None;
        }
    }

    // ---- named-group decoding (feeds Regex.groupNames() and each Match's
    // internal nameIndex; the engine only exposes a raw (gi,len,byte...)
    // record blob via programNameData, doc 1's additive accessor). ----
    Map<string, int> apiNameIndex(Array<int> program) {
        Array<int> raw = regex::programNameData(program);
        Map<string, int> m = Map();
        int i = 0;
        while (i < raw.length()) {
            int gi = raw.at(i);
            int len = raw.at(i + 1);
            i = i + 2;
            StringBuilder sb = StringBuilder();
            int j = 0;
            while (j < len) { sb.add(std::byteToString(raw.at(i + j))); j = j + 1; }
            i = i + len;
            m[sb.toString()] = gi;
        }
        return m;
    }
    Array<string> apiGroupNames(Array<int> program) => regex::apiNameIndex(program).keys();

    // ---- Match construction from a raw extents array (doc 1's `find`
    // core: [] on no match, else (groupCount+1)*2 ints, pairs of
    // start/end per group, group 0 = whole match). ----------------------
    Match apiBuildMatchWithNames(Array<int> program, string s, Array<int> ext, Map<string, int> names) {
        Match m;
        m.index = ext.at(0);
        m.length = ext.at(1) - ext.at(0);
        m.value = s.subStr(m.index, m.length);
        int n = regex::programGroupCount(program);
        Array<Group> gs = [];
        int gi = 0;
        while (gi <= n) {
            int st = ext.at(gi * 2);
            int en = ext.at(gi * 2 + 1);
            Group g;
            if (st < 0) {
                g.matched = false; g.index = -1; g.length = 0; g.value = "";
            } else {
                g.matched = true; g.index = st; g.length = en - st; g.value = s.subStr(st, en - st);
            }
            gs = gs.add(g);
            gi = gi + 1;
        }
        m.groups = gs;
        m.nameIndex = names;
        return m;
    }
    Match apiBuildMatch(Array<int> program, string s, Array<int> ext) =>
        regex::apiBuildMatchWithNames(program, s, ext, regex::apiNameIndex(program));

    Match? apiFind(Array<int> program, string s, int from) {
        Array<int> ext = regex::programFindFrom(program, s, from);
        if (ext.length() == 0) return None;
        return regex::apiBuildMatch(program, s, ext);
    }
    // §4 row 2: a zero-length match at i is reported, then the scan
    // resumes at i+1 (prevents the classic infinite loop).
    Array<Match> apiMatches(Array<int> program, string s) {
        Map<string, int> names = regex::apiNameIndex(program);
        Array<Match> out = [];
        int pos = 0;
        while (pos <= s.length()) {
            Array<int> ext = regex::programFindFrom(program, s, pos);
            if (ext.length() == 0) return out;
            out = out.add(regex::apiBuildMatchWithNames(program, s, ext, names));
            int end = ext.at(1);
            pos = end > ext.at(0) ? end : end + 1;
        }
        return out;
    }

    // ---- §3 replacement-string grammar: $0.."$99 (greedy two-digit,
    // longest valid group number wins), ${name}, $$, bare $ at end/before
    // anything else -> literal $. Parsed once per call, before scanning.
    // Encoded as Pair<int,string>: first < 0 -> literal text in second;
    // first >= 0 -> substitute that group's value (unmatched -> "").
    Array<Pair<int, string>> apiParseReplacement(Array<int> program, string repl) {
        Map<string, int> names = regex::apiNameIndex(program);
        int n = regex::programGroupCount(program);
        Array<Pair<int, string>> parts = [];
        StringBuilder lit = StringBuilder();
        int i = 0;
        int len = repl.length();
        while (i < len) {
            int c = repl.byteAt(i);
            if (c != 36) { lit.add(std::byteToString(c)); i = i + 1; continue; }
            if (i + 1 >= len) { lit.add("$"); i = i + 1; continue; }
            int c1 = repl.byteAt(i + 1);
            if (c1 == 36) { lit.add("$"); i = i + 2; continue; }
            if (c1 == 123) {
                int close = repl.indexOfFrom("}", i + 2);
                if (close < 0) throw RegexException("regex: unterminated ${ in replacement", i);
                string name = repl.subStr(i + 2, close - (i + 2));
                if (!names.has(name)) throw RegexException("regex: unknown group name in replacement: " + name, i);
                parts = parts.add(Pair::Of(0 - 1, lit.toString()));
                lit = StringBuilder();
                parts = parts.add(Pair::Of(names.at(name), ""));
                i = close + 1;
                continue;
            }
            if (c1 >= 48 && c1 <= 57) {
                int d1 = c1 - 48;
                int gi = d1;
                int consumed = 2;
                if (i + 2 < len) {
                    int c2 = repl.byteAt(i + 2);
                    if (c2 >= 48 && c2 <= 57) {
                        int two = d1 * 10 + (c2 - 48);
                        if (two <= n) { gi = two; consumed = 3; }
                    }
                }
                if (gi > n) throw RegexException("regex: group reference out of range in replacement: $" + gi.toString(), i);
                parts = parts.add(Pair::Of(0 - 1, lit.toString()));
                lit = StringBuilder();
                parts = parts.add(Pair::Of(gi, ""));
                i = i + consumed;
                continue;
            }
            lit.add("$");
            i = i + 1;
        }
        parts = parts.add(Pair::Of(0 - 1, lit.toString()));
        return parts;
    }
    string apiApplyReplacementParts(Array<Pair<int, string>> parts, Match m) {
        StringBuilder sb = StringBuilder();
        for (Pair<int, string> p in parts) {
            if (p.first < 0) sb.add(p.second);
            else sb.add(m.groups.at(p.first).value);
        }
        return sb.toString();
    }
    string apiReplaceLiteral(Array<int> program, string s, string replacement, int limit) {
        Array<Pair<int, string>> parts = regex::apiParseReplacement(program, replacement);
        Map<string, int> names = regex::apiNameIndex(program);
        StringBuilder sb = StringBuilder();
        int pos = 0;
        int done = 0;
        while (pos <= s.length()) {
            if (limit >= 0 && done >= limit) break;
            Array<int> ext = regex::programFindFrom(program, s, pos);
            if (ext.length() == 0) break;
            int mstart = ext.at(0); int mend = ext.at(1);
            sb.add(s.subStr(pos, mstart - pos));
            Match m = regex::apiBuildMatchWithNames(program, s, ext, names);
            sb.add(regex::apiApplyReplacementParts(parts, m));
            done = done + 1;
            if (mend > mstart) { pos = mend; }
            else { if (mend < s.length()) sb.add(s.subStr(mend, 1)); pos = mend + 1; }
        }
        int tail = pos < s.length() ? pos : s.length();
        sb.add(s.subStr(tail, s.length() - tail));
        return sb.toString();
    }
    string apiReplaceFn(Array<int> program, string s, (Match) => string fn, int limit) {
        Map<string, int> names = regex::apiNameIndex(program);
        StringBuilder sb = StringBuilder();
        int pos = 0;
        int done = 0;
        while (pos <= s.length()) {
            if (limit >= 0 && done >= limit) break;
            Array<int> ext = regex::programFindFrom(program, s, pos);
            if (ext.length() == 0) break;
            int mstart = ext.at(0); int mend = ext.at(1);
            sb.add(s.subStr(pos, mstart - pos));
            Match m = regex::apiBuildMatchWithNames(program, s, ext, names);
            sb.add(fn(m));
            done = done + 1;
            if (mend > mstart) { pos = mend; }
            else { if (mend < s.length()) sb.add(s.subStr(mend, 1)); pos = mend + 1; }
        }
        int tail = pos < s.length() ? pos : s.length();
        sb.add(s.subStr(tail, s.length() - tail));
        return sb.toString();
    }

    // ---- §4 row 3: split includes captured-group text; limit bounds the
    // piece count, remainder unsplit in the last piece. ------------------
    Array<string> apiSplit(Array<int> program, string s, int limit) {
        Array<string> out = [];
        int n = regex::programGroupCount(program);
        int pos = 0;
        int lastEnd = 0;
        while (pos <= s.length()) {
            if (limit > 0 && out.length() >= limit - 1) break;
            Array<int> ext = regex::programFindFrom(program, s, pos);
            if (ext.length() == 0) break;
            int mstart = ext.at(0); int mend = ext.at(1);
            out = out.add(s.subStr(lastEnd, mstart - lastEnd));
            int gi = 1;
            while (gi <= n) {
                int st = ext.at(gi * 2); int en = ext.at(gi * 2 + 1);
                out = out.add(st < 0 ? "" : s.subStr(st, en - st));
                gi = gi + 1;
            }
            lastEnd = mend;
            pos = mend > mstart ? mend : mend + 1;
        }
        out = out.add(s.subStr(lastEnd, s.length() - lastEnd));
        return out;
    }

    string escape(string literal) {
        StringBuilder sb = StringBuilder();
        int i = 0;
        while (i < literal.length()) {
            int c = literal.byteAt(i);
            if (regex::apiIsMetaByte(c)) sb.add("\\");
            sb.add(std::byteToString(c));
            i = i + 1;
        }
        return sb.toString();
    }
    bool apiIsMetaByte(int c) => c == 92 || c == 46 || c == 94 || c == 36 || c == 124 ||
        c == 40 || c == 41 || c == 91 || c == 93 || c == 123 || c == 125 || c == 42 || c == 43 || c == 63;

    // ---- pattern cache: LRU, capacity 16 (C# Regex.CacheSize precedent).
    // Namespace state is legal (concretely-typed singleton, info.md §6.6);
    // single-threaded today (an LA-1 confinement rider, per doc 1 §2).
    Map<string, Array<int>> apiCache = Map();
    Array<string> apiCacheOrder = [];
    Map<string, Array<int>> apiWithoutKey(Map<string, Array<int>> src, string key) {
        // Rebuilds via bracket sugar rather than .without() by name — the
        // same defensive convention Set<T>.without() already uses in this
        // prelude for named-native-dispatch safety on every engine.
        Map<string, Array<int>> out = Map();
        for (Pair<string, Array<int>> e in src) if (e.first != key) out[e.first] = e.second;
        return out;
    }
    Array<int> apiCachedProgram(string pattern, string flags) {
        // NOTE: reassignment to a namespace-scoped variable must be BARE
        // (unqualified) here, not `regex::apiCache = ...` — a qualified
        // write to a namespace global fails to lower (info.md §12's "use
        // ... as" write-through rule; bug.md #7's ruling on qualified
        // writes applies the same way to a plain `NS::x = ...`). Reads stay
        // qualified per this file's usual discipline; only writes are bare.
        string key = pattern + std::byteToString(1) + flags;
        if (regex::apiCache.has(key)) {
            Array<string> reordered = [];
            for (string k in regex::apiCacheOrder) if (k != key) reordered = reordered.add(k);
            apiCacheOrder = reordered.add(key);
            return regex::apiCache.at(key);
        }
        Array<int> prog = regex::apiCompileOrThrow(pattern, flags);
        Map<string, Array<int>> m = regex::apiCache;
        m[key] = prog;
        Array<string> order = regex::apiCacheOrder.add(key);
        if (order.length() > 16) {
            string evict = order.at(0);
            order = order.removeAt(0);
            m = regex::apiWithoutKey(m, evict);
        }
        apiCache = m;
        apiCacheOrder = order;
        return prog;
    }

    // ---- §2.2 conveniences (compile-per-call semantics, backed by the
    // LRU cache above) -----------------------------------------------------
    Regex? compile(string pattern) => regex::apiCompileSafe(pattern, "");
    Regex? compile(string pattern, RegexOptions options) => regex::apiCompileSafe(pattern, regex::apiFlagsString(options));
    Regex? compile(string pattern, string flags) => regex::apiCompileSafe(pattern, flags);

    bool isMatch(string s, string pattern) => regex::programIsMatch(regex::apiCachedProgram(pattern, ""), s);
    bool isMatch(string s, string pattern, RegexOptions o) => regex::programIsMatch(regex::apiCachedProgram(pattern, regex::apiFlagsString(o)), s);
    bool isMatch(string s, string pattern, string flags) => regex::programIsMatch(regex::apiCachedProgram(pattern, flags), s);

    Match? find(string s, string pattern) => regex::apiFind(regex::apiCachedProgram(pattern, ""), s, 0);
    Match? find(string s, string pattern, RegexOptions o) => regex::apiFind(regex::apiCachedProgram(pattern, regex::apiFlagsString(o)), s, 0);
    Match? find(string s, string pattern, string flags) => regex::apiFind(regex::apiCachedProgram(pattern, flags), s, 0);

    Array<Match> matches(string s, string pattern) => regex::apiMatches(regex::apiCachedProgram(pattern, ""), s);
    Array<Match> matches(string s, string pattern, RegexOptions o) => regex::apiMatches(regex::apiCachedProgram(pattern, regex::apiFlagsString(o)), s);
    Array<Match> matches(string s, string pattern, string flags) => regex::apiMatches(regex::apiCachedProgram(pattern, flags), s);

    // Lambda-typed overloads declared first — see the reorder note on
    // Regex.replace above (same lambda-vs-string applicability bug).
    string replace(string s, string pattern, (Match) => string fn) => regex::apiReplaceFn(regex::apiCachedProgram(pattern, ""), s, fn, -1);
    string replace(string s, string pattern, (Match) => string fn, RegexOptions o) => regex::apiReplaceFn(regex::apiCachedProgram(pattern, regex::apiFlagsString(o)), s, fn, -1);
    string replace(string s, string pattern, (Match) => string fn, string flags) => regex::apiReplaceFn(regex::apiCachedProgram(pattern, flags), s, fn, -1);
    string replace(string s, string pattern, string replacement) => regex::apiReplaceLiteral(regex::apiCachedProgram(pattern, ""), s, replacement, -1);
    string replace(string s, string pattern, string replacement, RegexOptions o) => regex::apiReplaceLiteral(regex::apiCachedProgram(pattern, regex::apiFlagsString(o)), s, replacement, -1);
    string replace(string s, string pattern, string replacement, string flags) => regex::apiReplaceLiteral(regex::apiCachedProgram(pattern, flags), s, replacement, -1);

    Array<string> split(string s, string pattern) => regex::apiSplit(regex::apiCachedProgram(pattern, ""), s, -1);
    Array<string> split(string s, string pattern, RegexOptions o) => regex::apiSplit(regex::apiCachedProgram(pattern, regex::apiFlagsString(o)), s, -1);
    Array<string> split(string s, string pattern, string flags) => regex::apiSplit(regex::apiCachedProgram(pattern, flags), s, -1);

    int count(string s, string pattern) => regex::programCount(regex::apiCachedProgram(pattern, ""), s);
    int count(string s, string pattern, RegexOptions o) => regex::programCount(regex::apiCachedProgram(pattern, regex::apiFlagsString(o)), s);
    int count(string s, string pattern, string flags) => regex::programCount(regex::apiCachedProgram(pattern, flags), s);
}
)prelude";

// --- kPreludeWeb: Track 09 web foundations -------------------------------
// Encoding (base64/percent/hex), digests (md5/sha1/sha256/hmac), DateTime/
// Duration, and JSON. Every declaration here is ordinary in-language code
// over the primitive/collection surface above (zero new natives beyond the
// documented Track 08 `sysNow` clock dependency, which `namespace std` owns).
const char* kPreludeWeb = R"prelude(
// === Track 09 F3: encoding =================================================
// Byte-true text encodings over the string byte surface (byteAt/length are
// byte-indexed; std::byteToString rebuilds a 1-byte string). Every decoder is
// TOTAL: malformed input is a value (None), never a throw (§12.6 — data errors
// are Results). Helpers carry DISTINCT names (no overloads) so prelude-body
// resolution is unambiguous without the checker (Track 04 problem #5).
namespace encoding {
    // hex nibble 0..15 for a '0'..'9'/'a'..'f'/'A'..'F' byte, else -1.
    int hexNibble(int c) {
        if (c >= 48 && c <= 57)  return c - 48;
        if (c >= 97 && c <= 102) return c - 97 + 10;
        if (c >= 65 && c <= 70)  return c - 65 + 10;
        return 0 - 1;
    }
    // standard base64 alphabet value 0..63, else -1 ('=' is handled by caller).
    int b64Val(int c) {
        if (c >= 65 && c <= 90)  return c - 65;         // A-Z -> 0..25
        if (c >= 97 && c <= 122) return c - 97 + 26;    // a-z -> 26..51
        if (c >= 48 && c <= 57)  return c - 48 + 52;    // 0-9 -> 52..61
        if (c == 43) return 62;                         // '+'
        if (c == 47) return 63;                         // '/'
        return 0 - 1;
    }
    bool pctUnreserved(int c) {
        if (c >= 65 && c <= 90)  return true;           // A-Z
        if (c >= 97 && c <= 122) return true;           // a-z
        if (c >= 48 && c <= 57)  return true;           // 0-9
        return c == 45 || c == 95 || c == 46 || c == 126;   // - _ . ~
    }

    string hexEncode(string bytes) {
        string digits = "0123456789abcdef";
        StringBuilder sb = StringBuilder();
        int i = 0;
        while (i < bytes.length()) {
            int b = bytes.byteAt(i);
            sb.add(digits.charAt((b >> 4) & 15));
            sb.add(digits.charAt(b & 15));
            i = i + 1;
        }
        return sb.toString();
    }
    string? hexDecode(string s) {
        int n = s.length();
        if (n % 2 != 0) return None;
        StringBuilder sb = StringBuilder();
        int i = 0;
        while (i < n) {
            int hi = encoding::hexNibble(s.byteAt(i));
            int lo = encoding::hexNibble(s.byteAt(i + 1));
            if (hi < 0 || lo < 0) return None;
            sb.add(std::byteToString((hi << 4) | lo));
            i = i + 2;
        }
        return sb.toString();
    }

    string base64Encode(string bytes) {
        string alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        StringBuilder sb = StringBuilder();
        int n = bytes.length();
        int i = 0;
        while (i + 3 <= n) {
            int t = (bytes.byteAt(i) << 16) | (bytes.byteAt(i + 1) << 8) | bytes.byteAt(i + 2);
            sb.add(alpha.charAt((t >> 18) & 63));
            sb.add(alpha.charAt((t >> 12) & 63));
            sb.add(alpha.charAt((t >> 6) & 63));
            sb.add(alpha.charAt(t & 63));
            i = i + 3;
        }
        int rem = n - i;
        if (rem == 1) {
            int t = bytes.byteAt(i) << 16;
            sb.add(alpha.charAt((t >> 18) & 63));
            sb.add(alpha.charAt((t >> 12) & 63));
            sb.add("==");
        } else if (rem == 2) {
            int t = (bytes.byteAt(i) << 16) | (bytes.byteAt(i + 1) << 8);
            sb.add(alpha.charAt((t >> 18) & 63));
            sb.add(alpha.charAt((t >> 12) & 63));
            sb.add(alpha.charAt((t >> 6) & 63));
            sb.add("=");
        }
        return sb.toString();
    }
    // Strict: length must be a multiple of 4; '=' padding only in the final
    // group; any non-alphabet byte -> None.
    string? base64Decode(string b64) {
        int n = b64.length();
        if (n % 4 != 0) return None;
        StringBuilder sb = StringBuilder();
        int i = 0;
        while (i < n) {
            int c0 = b64.byteAt(i);
            int c1 = b64.byteAt(i + 1);
            int c2 = b64.byteAt(i + 2);
            int c3 = b64.byteAt(i + 3);
            int v0 = encoding::b64Val(c0);
            int v1 = encoding::b64Val(c1);
            if (v0 < 0 || v1 < 0) return None;
            bool last = i + 4 == n;
            if (c2 == 61) {                              // "xx=="
                if (!last || c3 != 61) return None;
                int t = (v0 << 18) | (v1 << 12);
                sb.add(std::byteToString((t >> 16) & 255));
            } else {
                int v2 = encoding::b64Val(c2);
                if (v2 < 0) return None;
                if (c3 == 61) {                          // "xxx="
                    if (!last) return None;
                    int t = (v0 << 18) | (v1 << 12) | (v2 << 6);
                    sb.add(std::byteToString((t >> 16) & 255));
                    sb.add(std::byteToString((t >> 8) & 255));
                } else {
                    int v3 = encoding::b64Val(c3);
                    if (v3 < 0) return None;
                    int t = (v0 << 18) | (v1 << 12) | (v2 << 6) | v3;
                    sb.add(std::byteToString((t >> 16) & 255));
                    sb.add(std::byteToString((t >> 8) & 255));
                    sb.add(std::byteToString(t & 255));
                }
            }
            i = i + 4;
        }
        return sb.toString();
    }
    // URL-safe base64 (JWT-adjacent): +/ -> -_ and no '=' padding. Decode
    // reverses the substitution and re-pads to a 4-multiple before delegating.
    string base64UrlEncode(string bytes) {
        string s = encoding::base64Encode(bytes);
        return s.replace("+", "-").replace("/", "_").removeSuffix("=").removeSuffix("=");
    }
    string? base64UrlDecode(string s) {
        string t = s.replace("-", "+").replace("_", "/");
        int r = t.length() % 4;
        if (r == 1) return None;                         // impossible base64 length
        if (r == 2) t = t + "==";
        if (r == 3) t = t + "=";
        return encoding::base64Decode(t);
    }

    string percentEncode(string s) {
        string HEX = "0123456789ABCDEF";
        StringBuilder sb = StringBuilder();
        int i = 0;
        while (i < s.length()) {
            int b = s.byteAt(i);
            if (encoding::pctUnreserved(b)) {
                sb.add(std::byteToString(b));
            } else {
                sb.add("%");
                sb.add(HEX.charAt((b >> 4) & 15));
                sb.add(HEX.charAt(b & 15));
            }
            i = i + 1;
        }
        return sb.toString();
    }
    string? percentDecode(string s) {
        int n = s.length();
        StringBuilder sb = StringBuilder();
        int i = 0;
        while (i < n) {
            int b = s.byteAt(i);
            if (b == 37) {                               // '%'
                if (i + 2 >= n) return None;
                int hi = encoding::hexNibble(s.byteAt(i + 1));
                int lo = encoding::hexNibble(s.byteAt(i + 2));
                if (hi < 0 || lo < 0) return None;
                sb.add(std::byteToString((hi << 4) | lo));
                i = i + 3;
            } else {
                sb.add(std::byteToString(b));
                i = i + 1;
            }
        }
        return sb.toString();
    }
}

// === Track 09 F2: DateTime & Duration ======================================
// UTC-only value types (data, no identity — exactly what `struct` is for).
// Civil<->days is the public-domain Howard Hinnant integer algorithm (pure
// int64, no tables, leap seconds ignored per the Unix convention). The
// "static" surface splits by whether it can fail: infallible factories are
// labeled constructors (DateTime::now(), DateTime::ofEpochMs(x) — §2), and the
// fallible parsers are `datetime::` free functions returning DateTime? (the
// no-`static`-keyword fallback the design's P2 pre-registered).
namespace datetime {
    // floor(epochMs / 86400000): the epoch day number, correct for negatives.
    int epochDays(int epochMs) {
        int d = epochMs / 86400000;
        int r = epochMs % 86400000;
        if (r < 0) d = d - 1;
        return d;
    }
    // ms since midnight, always [0, 86399999] even for pre-1970 instants.
    int dayMs(int epochMs) {
        int r = epochMs % 86400000;
        if (r < 0) r = r + 86400000;
        return r;
    }
    // Hinnant civil_from_days: z = days since 1970-01-01 -> [year, month, day].
    Array<int> civilFromDays(int z) {
        int zz = z + 719468;
        int era = (zz >= 0 ? zz : zz - 146096) / 146097;
        int doe = zz - era * 146097;                                    // [0, 146096]
        int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
        int y = yoe + era * 400;
        int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);              // [0, 365]
        int mp = (5 * doy + 2) / 153;                                    // [0, 11]
        int d = doy - (153 * mp + 2) / 5 + 1;                            // [1, 31]
        int m = mp < 10 ? mp + 3 : mp - 9;                               // [1, 12]
        int year = m <= 2 ? y + 1 : y;
        return [year, m, d];
    }
    // Hinnant days_from_civil: (year, month, day) -> days since 1970-01-01.
    int daysFromCivil(int y, int m, int d) {
        int yy = m <= 2 ? y - 1 : y;
        int era = (yy >= 0 ? yy : yy - 399) / 400;
        int yoe = yy - era * 400;                                        // [0, 399]
        int mp = m > 2 ? m - 3 : m + 9;
        int doy = (153 * mp + 2) / 5 + d - 1;                            // [0, 365]
        int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                 // [0, 146096]
        return era * 146097 + doe - 719468;
    }
    int epochFromCivil(int y, int mo, int d, int h, int mi, int s, int ms) {
        return datetime::daysFromCivil(y, mo, d) * 86400000 +
               (h * 3600 + mi * 60 + s) * 1000 + ms;
    }
    // 0=Sunday .. 6=Saturday (anchor: 1970-01-01 = Thursday = 4).
    int weekday(int z) {
        int w = (z + 4) % 7;
        if (w < 0) w = w + 7;
        return w;
    }
    string weekdayName(int w) {
        Array<string> names = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
        return names.at(w);
    }
    string monthName(int m) {
        Array<string> names = ["", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
        return names.at(m);
    }
    int monthIndex(string mon) {
        Array<string> names = ["", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
        int i = 1;
        while (i <= 12) {
            if (names.at(i) == mon) return i;
            i = i + 1;
        }
        return 0;
    }
    string pad2(int n) => n.toString().padStart(2, "0");

    // Plain-int digit parsers (return -1 on any non-digit / empty / OOB). The
    // parsers below use THESE, never string.toInt() + int? narrowing: prelude
    // bodies are unchecked, so flow-narrowing annotations the static backends
    // rely on to unwrap `int?` are absent (they'd misread the union) — a plain
    // int with a -1 sentinel is backend-robust. (Learned M2, this track's log.)
    int parseUInt(string s) {
        if (s.isEmpty()) return 0 - 1;
        int v = 0;
        int i = 0;
        while (i < s.length()) {
            int c = s.byteAt(i);
            if (c < 48 || c > 57) return 0 - 1;
            v = v * 10 + (c - 48);
            i = i + 1;
        }
        return v;
    }
    int parseDigitsAt(string s, int off, int len) {
        if (off < 0 || off + len > s.length()) return 0 - 1;
        return datetime::parseUInt(s.subStr(off, len));
    }

    // Fallible "static" parsers (see header). IMF-fixdate required; obsolete
    // RFC 850 / asctime forms -> None in v1 (documented).
    DateTime? parseHttpDate(string s) {
        Array<string> parts = s.split(" ");
        if (parts.length() != 6) return None;
        if (!parts.at(0).endsWith(",")) return None;   // IMF-fixdate: "Wdy," required
        if (parts.at(5) != "GMT") return None;
        int mo = datetime::monthIndex(parts.at(2));
        if (mo == 0) return None;
        int d = datetime::parseUInt(parts.at(1));
        int y = datetime::parseUInt(parts.at(3));
        Array<string> hms = parts.at(4).split(":");
        if (hms.length() != 3) return None;
        int h  = datetime::parseUInt(hms.at(0));
        int mi = datetime::parseUInt(hms.at(1));
        int se = datetime::parseUInt(hms.at(2));
        if (d < 0 || y < 0 || h < 0 || mi < 0 || se < 0) return None;
        return DateTime::ofEpochMs(datetime::epochFromCivil(y, mo, d, h, mi, se, 0));
    }
    // "YYYY-MM-DDTHH:MM:SS" with optional ".fff" and a "Z" or "+hh:mm"/"-hh:mm"
    // offset (normalized to UTC). Fixed-width date/time positions; offset math
    // subtracts the zone so the stored epoch is always UTC.
    DateTime? parseIso8601(string s) {
        if (s.length() < 19) return None;
        if (s.charAt(4) != "-" || s.charAt(7) != "-" || s.charAt(10) != "T") return None;
        if (s.charAt(13) != ":" || s.charAt(16) != ":") return None;
        int y  = datetime::parseDigitsAt(s, 0, 4);
        int mo = datetime::parseDigitsAt(s, 5, 2);
        int d  = datetime::parseDigitsAt(s, 8, 2);
        int h  = datetime::parseDigitsAt(s, 11, 2);
        int mi = datetime::parseDigitsAt(s, 14, 2);
        int se = datetime::parseDigitsAt(s, 17, 2);
        if (y < 0 || mo < 0 || d < 0 || h < 0 || mi < 0 || se < 0) return None;
        int i = 19;
        int frac = 0;
        if (i < s.length() && s.charAt(i) == ".") {
            i = i + 1;
            int digits = 0;
            int scale = 100;
            while (i < s.length() && s.byteAt(i) >= 48 && s.byteAt(i) <= 57 && digits < 3) {
                frac = frac + (s.byteAt(i) - 48) * scale;
                scale = scale / 10;
                i = i + 1;
                digits = digits + 1;
            }
            // skip any excess fractional digits (sub-ms precision dropped)
            while (i < s.length() && s.byteAt(i) >= 48 && s.byteAt(i) <= 57) i = i + 1;
        }
        int offsetMs = 0;
        if (i < s.length()) {
            string zc = s.charAt(i);
            if (zc == "Z") {
                i = i + 1;
            } else if (zc == "+" || zc == "-") {
                if (i + 6 > s.length()) return None;
                if (s.charAt(i + 3) != ":") return None;
                int oh = datetime::parseDigitsAt(s, i + 1, 2);
                int om = datetime::parseDigitsAt(s, i + 4, 2);
                if (oh < 0 || om < 0) return None;
                offsetMs = (oh * 3600 + om * 60) * 1000;
                if (zc == "-") offsetMs = 0 - offsetMs;
                i = i + 6;
            } else {
                return None;
            }
        }
        if (i != s.length()) return None;    // trailing garbage -> None (strict)
        return DateTime::ofEpochMs(datetime::epochFromCivil(y, mo, d, h, mi, se, frac) - offsetMs);
    }
}

struct Duration {
    int ms;
    new ofMillis(int m)  { ms = m; }
    new ofSeconds(int s) { ms = s * 1000; }
    new ofMinutes(int m) { ms = m * 60000; }
    new ofHours(int h)   { ms = h * 3600000; }
    new ofDays(int d)    { ms = d * 86400000; }
    int toMillis()  => ms;
    int toSeconds() => ms / 1000;
    Duration plus(Duration o)  { Duration r; r.ms = ms + o.ms; return r; }
    Duration minus(Duration o) { Duration r; r.ms = ms - o.ms; return r; }
    // "1h02m03s" (higher units suppress leading zeros; sub-second -> "Nms").
    string toString() {
        int t = ms < 0 ? 0 - ms : ms;
        string sign = ms < 0 ? "-" : "";
        int sec = t / 1000;
        int h = sec / 3600;
        int m = (sec / 60) % 60;
        int s = sec % 60;
        if (h > 0) return sign + h.toString() + "h" + datetime::pad2(m) + "m" + datetime::pad2(s) + "s";
        if (m > 0) return sign + m.toString() + "m" + datetime::pad2(s) + "s";
        if (sec > 0) return sign + s.toString() + "s";
        return sign + t.toString() + "ms";
    }
}

struct DateTime {
    int epochMs;
    new ofEpochMs(int e) { epochMs = e; }
    new now()            { epochMs = std::sysNow(); }

    int year()   => datetime::civilFromDays(datetime::epochDays(epochMs)).at(0);
    int month()  => datetime::civilFromDays(datetime::epochDays(epochMs)).at(1);
    int day()    => datetime::civilFromDays(datetime::epochDays(epochMs)).at(2);
    int hour()   => (datetime::dayMs(epochMs) / 3600000) % 24;
    int minute() => (datetime::dayMs(epochMs) / 60000) % 60;
    int second() => (datetime::dayMs(epochMs) / 1000) % 60;
    int milli()  => datetime::dayMs(epochMs) % 1000;
    int weekday() => datetime::weekday(datetime::epochDays(epochMs));

    DateTime plus(Duration d)   { DateTime r; r.epochMs = epochMs + d.ms; return r; }
    Duration minus(DateTime o)  { Duration r; r.ms = epochMs - o.epochMs; return r; }

    // RFC 7231 IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT".
    string httpDate() {
        int z = datetime::epochDays(epochMs);
        Array<int> c = datetime::civilFromDays(z);
        return datetime::weekdayName(datetime::weekday(z)) + ", " +
               datetime::pad2(c.at(2)) + " " + datetime::monthName(c.at(1)) + " " +
               c.at(0).toString().padStart(4, "0") + " " +
               datetime::pad2(hour()) + ":" + datetime::pad2(minute()) + ":" +
               datetime::pad2(second()) + " GMT";
    }
    // ISO 8601 UTC: "2026-07-05T12:00:00Z" (second precision; ".mmm" if nonzero).
    string iso8601() {
        int z = datetime::epochDays(epochMs);
        Array<int> c = datetime::civilFromDays(z);
        string base = c.at(0).toString().padStart(4, "0") + "-" +
                      datetime::pad2(c.at(1)) + "-" + datetime::pad2(c.at(2)) + "T" +
                      datetime::pad2(hour()) + ":" + datetime::pad2(minute()) + ":" +
                      datetime::pad2(second());
        int frac = datetime::dayMs(epochMs) % 1000;
        if (frac != 0) base = base + "." + frac.toString().padStart(3, "0");
        return base + "Z";
    }
}

// === Track 09 F3: digests ==================================================
// md5/sha1/sha256/hmacSha256 return RAW BYTES (compose with encoding::hexEncode
// / base64Encode). In-language over int64 with the C7 mask idiom: every word is
// kept in an int64 and masked to 32 bits (& 0xFFFFFFFF) after each add/xor/rot,
// so it stays non-negative and `>>` (arithmetic) equals a logical shift — the
// correctness linchpin (never right-shift an unmasked value). `~x` is written
// `(x ^ 0xFFFFFFFF)`. Verified against RFC 1321 / RFC 3174 / FIPS 180-4 /
// RFC 4231 vectors, incl. the 55/56/63/64/65-byte padding-boundary trap set.
namespace digest {
    int rotl32(int x, int n) => ((x << n) | (x >> (32 - n))) & 0xFFFFFFFF;
    int rotr32(int x, int n) => ((x >> n) | (x << (32 - n))) & 0xFFFFFFFF;

    // Pad to the SHA/MD5 block: append 0x80, zero-fill to 56 mod 64, then the
    // 64-bit BIT length (big-endian for sha1/sha256, little-endian for md5).
    Array<int> pad(string msg, bool bigEndianLen) {
        Array<int> b = [];
        int n = msg.length();
        int i = 0;
        while (i < n) { b = b.add(msg.byteAt(i)); i = i + 1; }
        b = b.add(128);
        while (b.length() % 64 != 56) b = b.add(0);
        int bits = n * 8;
        int j = 0;
        while (j < 8) {
            int shift = bigEndianLen ? (56 - j * 8) : (j * 8);
            b = b.add((bits >> shift) & 255);
            j = j + 1;
        }
        return b;
    }
    int wordBE(Array<int> b, int off) =>
        ((b.at(off) << 24) | (b.at(off + 1) << 16) | (b.at(off + 2) << 8) | b.at(off + 3)) & 0xFFFFFFFF;
    int wordLE(Array<int> b, int off) =>
        (b.at(off) | (b.at(off + 1) << 8) | (b.at(off + 2) << 16) | (b.at(off + 3) << 24)) & 0xFFFFFFFF;
    // 4 bytes of a 32-bit word, big-endian, appended to sb.
    void emitBE(StringBuilder sb, int w) {
        sb.add(std::byteToString((w >> 24) & 255));
        sb.add(std::byteToString((w >> 16) & 255));
        sb.add(std::byteToString((w >> 8) & 255));
        sb.add(std::byteToString(w & 255));
    }
    void emitLE(StringBuilder sb, int w) {
        sb.add(std::byteToString(w & 255));
        sb.add(std::byteToString((w >> 8) & 255));
        sb.add(std::byteToString((w >> 16) & 255));
        sb.add(std::byteToString((w >> 24) & 255));
    }

    string sha256(string msg) {
        Array<int> b = digest::pad(msg, true);
        Array<int> K = [
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 ];
        int h0 = 0x6a09e667; int h1 = 0xbb67ae85; int h2 = 0x3c6ef372; int h3 = 0xa54ff53a;
        int h4 = 0x510e527f; int h5 = 0x9b05688c; int h6 = 0x1f83d9ab; int h7 = 0x5be0cd19;
        int blocks = b.length() / 64;
        int blk = 0;
        while (blk < blocks) {
            int base = blk * 64;
            Array<int> w = [];
            int t = 0;
            while (t < 16) { w = w.add(digest::wordBE(b, base + t * 4)); t = t + 1; }
            while (t < 64) {
                int w15 = w.at(t - 15); int w2 = w.at(t - 2);
                int s0 = (digest::rotr32(w15, 7) ^ digest::rotr32(w15, 18) ^ (w15 >> 3)) & 0xFFFFFFFF;
                int s1 = (digest::rotr32(w2, 17) ^ digest::rotr32(w2, 19) ^ (w2 >> 10)) & 0xFFFFFFFF;
                w = w.add((w.at(t - 16) + s0 + w.at(t - 7) + s1) & 0xFFFFFFFF);
                t = t + 1;
            }
            int a = h0; int bb = h1; int c = h2; int d = h3;
            int e = h4; int f = h5; int g = h6; int hh = h7;
            int r = 0;
            while (r < 64) {
                int S1 = (digest::rotr32(e, 6) ^ digest::rotr32(e, 11) ^ digest::rotr32(e, 25)) & 0xFFFFFFFF;
                int ch = (e & f) ^ ((e ^ 0xFFFFFFFF) & g);
                int temp1 = (hh + S1 + ch + K.at(r) + w.at(r)) & 0xFFFFFFFF;
                int S0 = (digest::rotr32(a, 2) ^ digest::rotr32(a, 13) ^ digest::rotr32(a, 22)) & 0xFFFFFFFF;
                int maj = (a & bb) ^ (a & c) ^ (bb & c);
                int temp2 = (S0 + maj) & 0xFFFFFFFF;
                hh = g; g = f; f = e; e = (d + temp1) & 0xFFFFFFFF;
                d = c; c = bb; bb = a; a = (temp1 + temp2) & 0xFFFFFFFF;
                r = r + 1;
            }
            h0 = (h0 + a) & 0xFFFFFFFF; h1 = (h1 + bb) & 0xFFFFFFFF; h2 = (h2 + c) & 0xFFFFFFFF; h3 = (h3 + d) & 0xFFFFFFFF;
            h4 = (h4 + e) & 0xFFFFFFFF; h5 = (h5 + f) & 0xFFFFFFFF; h6 = (h6 + g) & 0xFFFFFFFF; h7 = (h7 + hh) & 0xFFFFFFFF;
            blk = blk + 1;
        }
        StringBuilder sb = StringBuilder();
        digest::emitBE(sb, h0); digest::emitBE(sb, h1); digest::emitBE(sb, h2); digest::emitBE(sb, h3);
        digest::emitBE(sb, h4); digest::emitBE(sb, h5); digest::emitBE(sb, h6); digest::emitBE(sb, h7);
        return sb.toString();
    }

    string sha1(string msg) {
        Array<int> data = digest::pad(msg, true);
        int h0 = 0x67452301; int h1 = 0xefcdab89; int h2 = 0x98badcfe; int h3 = 0x10325476; int h4 = 0xc3d2e1f0;
        int blocks = data.length() / 64;
        int blk = 0;
        while (blk < blocks) {
            int base = blk * 64;
            Array<int> w = [];
            int t = 0;
            while (t < 16) { w = w.add(digest::wordBE(data, base + t * 4)); t = t + 1; }
            while (t < 80) {
                int x = w.at(t - 3) ^ w.at(t - 8) ^ w.at(t - 14) ^ w.at(t - 16);
                w = w.add(digest::rotl32(x, 1));
                t = t + 1;
            }
            int a = h0; int b = h1; int c = h2; int d = h3; int e = h4;
            int r = 0;
            while (r < 80) {
                int f = 0; int k = 0;
                if (r < 20)      { f = (b & c) | ((b ^ 0xFFFFFFFF) & d); k = 0x5A827999; }
                else if (r < 40) { f = b ^ c ^ d;                       k = 0x6ED9EBA1; }
                else if (r < 60) { f = (b & c) | (b & d) | (c & d);     k = 0x8F1BBCDC; }
                else             { f = b ^ c ^ d;                       k = 0xCA62C1D6; }
                int temp = (digest::rotl32(a, 5) + f + e + k + w.at(r)) & 0xFFFFFFFF;
                e = d; d = c; c = digest::rotl32(b, 30); b = a; a = temp;
                r = r + 1;
            }
            h0 = (h0 + a) & 0xFFFFFFFF; h1 = (h1 + b) & 0xFFFFFFFF; h2 = (h2 + c) & 0xFFFFFFFF;
            h3 = (h3 + d) & 0xFFFFFFFF; h4 = (h4 + e) & 0xFFFFFFFF;
            blk = blk + 1;
        }
        StringBuilder sb = StringBuilder();
        digest::emitBE(sb, h0); digest::emitBE(sb, h1); digest::emitBE(sb, h2);
        digest::emitBE(sb, h3); digest::emitBE(sb, h4);
        return sb.toString();
    }

    string md5(string msg) {
        Array<int> data = digest::pad(msg, false);
        Array<int> K = [
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 ];
        Array<int> S = [
            7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
            5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
            6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 ];
        int a0 = 0x67452301; int b0 = 0xefcdab89; int c0 = 0x98badcfe; int d0 = 0x10325476;
        int blocks = data.length() / 64;
        int blk = 0;
        while (blk < blocks) {
            int base = blk * 64;
            Array<int> M = [];
            int t = 0;
            while (t < 16) { M = M.add(digest::wordLE(data, base + t * 4)); t = t + 1; }
            int A = a0; int B = b0; int C = c0; int D = d0;
            int i = 0;
            while (i < 64) {
                int F = 0; int g = 0;
                if (i < 16)      { F = (B & C) | ((B ^ 0xFFFFFFFF) & D); g = i; }
                else if (i < 32) { F = (D & B) | ((D ^ 0xFFFFFFFF) & C); g = (5 * i + 1) % 16; }
                else if (i < 48) { F = B ^ C ^ D;                       g = (3 * i + 5) % 16; }
                else             { F = C ^ (B | (D ^ 0xFFFFFFFF));      g = (7 * i) % 16; }
                F = (F + A + K.at(i) + M.at(g)) & 0xFFFFFFFF;
                A = D; D = C; C = B;
                B = (B + digest::rotl32(F, S.at(i))) & 0xFFFFFFFF;
                i = i + 1;
            }
            a0 = (a0 + A) & 0xFFFFFFFF; b0 = (b0 + B) & 0xFFFFFFFF; c0 = (c0 + C) & 0xFFFFFFFF; d0 = (d0 + D) & 0xFFFFFFFF;
            blk = blk + 1;
        }
        StringBuilder sb = StringBuilder();
        digest::emitLE(sb, a0); digest::emitLE(sb, b0); digest::emitLE(sb, c0); digest::emitLE(sb, d0);
        return sb.toString();
    }

    // HMAC (RFC 2104): H((K' ^ opad) || H((K' ^ ipad) || msg)); K' = key padded
    // to the 64-byte block (hashed first when longer). Returns raw bytes.
    string hmacSha256(string key, string msg) {
        string k = key;
        if (k.length() > 64) k = digest::sha256(k);
        Array<int> kb = [];
        int i = 0;
        while (i < k.length()) { kb = kb.add(k.byteAt(i)); i = i + 1; }
        while (kb.length() < 64) kb = kb.add(0);
        StringBuilder inner = StringBuilder();
        StringBuilder outerKey = StringBuilder();
        int j = 0;
        while (j < 64) {
            inner.add(std::byteToString(kb.at(j) ^ 0x36));
            outerKey.add(std::byteToString(kb.at(j) ^ 0x5c));
            j = j + 1;
        }
        string innerHash = digest::sha256(inner.toString() + msg);
        return digest::sha256(outerKey.toString() + innerHash);
    }
}

// === Track 09 F1: JSON =====================================================
// The value model is a CLASS (v1 §1.1): no type aliases, so a recursive named
// union can't be declared. `kind` is an int tag (0 null,1 bool,2 number,3
// string,4 array,5 object) — a mechanical upgrade to `enum JsonKind` is a
// non-blocking cross-track rider. The "static" ctors are labeled constructors
// (JsonValue::ofStr(x) etc., §2); parse is a json:: free function returning
// JsonValue? (None on malformed — parse is total, §12.6, NOT exceptions).
class JsonValue {
    int kind = 0;
    bool b = false;
    float num = 0.0;
    string str = "";
    Array<JsonValue> items = [];
    Map<string, JsonValue> fields;

    new ofNull()                       { kind = 0; }
    new ofBool(bool v)                 { kind = 1; b = v; }
    new ofNum(float v)                 { kind = 2; num = v; }
    new ofStr(string v)                { kind = 3; str = v; }
    new ofArray(Array<JsonValue> v)    { kind = 4; items = v; }
    new ofObject(Map<string, JsonValue> v) { kind = 5; fields = v; }

    bool isNull()   => kind == 0;
    bool isBool()   => kind == 1;
    bool isNum()    => kind == 2;
    bool isStr()    => kind == 3;
    bool isArray()  => kind == 4;
    bool isObject() => kind == 5;

    // Typed accessors: None on a kind mismatch (a value, not a throw).
    bool?                   asBool()   => kind == 1 ? b : None;
    float?                  asNum()    => kind == 2 ? num : None;
    string?                 asStr()    => kind == 3 ? str : None;
    Array<JsonValue>?       asArray()  => kind == 4 ? items : None;
    Map<string, JsonValue>? asObject() => kind == 5 ? fields : None;

    // Loud navigation: at() throws on kind/bounds/missing (the runtime-failures-
    // are-loud rule, §12.6); atOrNone is the None-returning object lookup.
    JsonValue at(int i) {
        if (kind != 4) throw RuntimeException("JsonValue.at(int): not an array");
        return items.at(i);
    }
    JsonValue at(string key) {
        if (kind != 5) throw RuntimeException("JsonValue.at(string): not an object");
        return fields.at(key);
    }
    JsonValue? atOrNone(string key) =>
        (kind == 5 && fields.has(key)) ? fields.at(key) : None;
    int size() => kind == 4 ? items.length() : (kind == 5 ? fields.length() : 0);

    string render() {
        if (kind == 0) return "null";
        if (kind == 1) return b ? "true" : "false";
        if (kind == 2) return num.toString();
        if (kind == 3) return json::renderStr(str);
        if (kind == 4) {
            StringBuilder sb = StringBuilder();
            sb.add("[");
            int n = items.length();
            int i = 0;
            while (i < n) {
                if (i > 0) sb.add(",");
                sb.add(items.at(i).render());
                i = i + 1;
            }
            sb.add("]");
            return sb.toString();
        }
        StringBuilder sb = StringBuilder();
        sb.add("{");
        Array<string> ks = fields.keys();
        int n = ks.length();
        int i = 0;
        while (i < n) {
            if (i > 0) sb.add(",");
            string k = ks.at(i);
            sb.add(json::renderStr(k));
            sb.add(":");
            sb.add(fields.at(k).render());
            i = i + 1;
        }
        sb.add("}");
        return sb.toString();
    }
    string renderPretty(int indent) => this.renderPrettyLevel(indent, 0);
    string renderPrettyLevel(int indent, int level) {
        if (kind == 4) {
            if (items.length() == 0) return "[]";
            StringBuilder sb = StringBuilder();
            sb.add("[\n");
            string inner = " ".repeat(indent * (level + 1));
            int n = items.length();
            int i = 0;
            while (i < n) {
                sb.add(inner);
                sb.add(items.at(i).renderPrettyLevel(indent, level + 1));
                if (i < n - 1) sb.add(",");
                sb.add("\n");
                i = i + 1;
            }
            sb.add(" ".repeat(indent * level));
            sb.add("]");
            return sb.toString();
        }
        if (kind == 5) {
            if (fields.length() == 0) return "{}";
            StringBuilder sb = StringBuilder();
            sb.add("{\n");
            string inner = " ".repeat(indent * (level + 1));
            Array<string> ks = fields.keys();
            int n = ks.length();
            int i = 0;
            while (i < n) {
                sb.add(inner);
                string k = ks.at(i);
                sb.add(json::renderStr(k));
                sb.add(": ");
                sb.add(fields.at(k).renderPrettyLevel(indent, level + 1));
                if (i < n - 1) sb.add(",");
                sb.add("\n");
                i = i + 1;
            }
            sb.add(" ".repeat(indent * level));
            sb.add("}");
            return sb.toString();
        }
        return this.render();
    }
}

// Recursive-descent parser with an index cursor and a `failed` flag: every
// method returns a concrete JsonValue and signals malformation by setting
// `failed` (checked once by json::parse), so the descent NEVER consumes a
// JsonValue? via narrowing — prelude bodies are unchecked, so union narrowing
// misreads on the static backends ([[leviathan-prelude-no-narrowing]]).
class JsonParser {
    string s = "";
    int i = 0;
    bool failed = false;
    new JsonParser(string src) { s = src; }

    void fail() { failed = true; }
    int cur() => i < s.length() ? s.byteAt(i) : 0 - 1;
    void skipWs() {
        bool go = true;
        while (go && i < s.length()) {
            int c = s.byteAt(i);
            if (c == 32 || c == 9 || c == 10 || c == 13) i = i + 1;
            else go = false;
        }
    }

    JsonValue parseValue(int depth) {
        if (depth > 128) { this.fail(); return JsonValue::ofNull(); }
        this.skipWs();
        int c = this.cur();
        if (c == 123) return this.parseObject(depth);
        if (c == 91)  return this.parseArray(depth);
        if (c == 34)  return JsonValue::ofStr(this.parseRawString());
        if (c == 116) return this.parseLit("true", JsonValue::ofBool(true));
        if (c == 102) return this.parseLit("false", JsonValue::ofBool(false));
        if (c == 110) return this.parseLit("null", JsonValue::ofNull());
        if (c == 45 || (c >= 48 && c <= 57)) return this.parseNumber();
        this.fail();
        return JsonValue::ofNull();
    }
    JsonValue parseLit(string lit, JsonValue v) {
        int n = lit.length();
        if (i + n > s.length() || s.subStr(i, n) != lit) { this.fail(); return JsonValue::ofNull(); }
        i = i + n;
        return v;
    }
    JsonValue parseNumber() {
        int start = i;
        if (this.cur() == 45) i = i + 1;
        if (this.cur() == 48) { i = i + 1; }
        else if (this.cur() >= 49 && this.cur() <= 57) {
            while (this.cur() >= 48 && this.cur() <= 57) i = i + 1;
        } else { this.fail(); return JsonValue::ofNull(); }
        if (this.cur() == 46) {
            i = i + 1;
            if (!(this.cur() >= 48 && this.cur() <= 57)) { this.fail(); return JsonValue::ofNull(); }
            while (this.cur() >= 48 && this.cur() <= 57) i = i + 1;
        }
        if (this.cur() == 101 || this.cur() == 69) {
            i = i + 1;
            if (this.cur() == 43 || this.cur() == 45) i = i + 1;
            if (!(this.cur() >= 48 && this.cur() <= 57)) { this.fail(); return JsonValue::ofNull(); }
            while (this.cur() >= 48 && this.cur() <= 57) i = i + 1;
        }
        float f = json::toNum(s.subStr(start, i - start));
        JsonValue v = JsonValue::ofNum(f);
        return v;
    }
    JsonValue parseArray(int depth) {
        i = i + 1;                                   // '['
        Array<JsonValue> arr = [];
        this.skipWs();
        if (this.cur() == 93) { i = i + 1; return JsonValue::ofArray(arr); }
        while (!failed) {
            JsonValue v = this.parseValue(depth + 1);
            if (failed) return JsonValue::ofNull();
            arr = arr.add(v);
            this.skipWs();
            int c = this.cur();
            if (c == 93) { i = i + 1; return JsonValue::ofArray(arr); }
            if (c != 44) { this.fail(); return JsonValue::ofNull(); }
            i = i + 1;
        }
        return JsonValue::ofNull();
    }
    JsonValue parseObject(int depth) {
        i = i + 1;                                   // '{'
        Map<string, JsonValue> m;
        this.skipWs();
        if (this.cur() == 125) { i = i + 1; return JsonValue::ofObject(m); }
        while (!failed) {
            this.skipWs();
            if (this.cur() != 34) { this.fail(); return JsonValue::ofNull(); }
            string key = this.parseRawString();
            if (failed) return JsonValue::ofNull();
            this.skipWs();
            if (this.cur() != 58) { this.fail(); return JsonValue::ofNull(); }   // ':'
            i = i + 1;
            JsonValue val = this.parseValue(depth + 1);
            if (failed) return JsonValue::ofNull();
            m = m.with(key, val);
            this.skipWs();
            int c = this.cur();
            if (c == 125) { i = i + 1; return JsonValue::ofObject(m); }
            if (c != 44) { this.fail(); return JsonValue::ofNull(); }
            i = i + 1;
        }
        return JsonValue::ofNull();
    }
    // cur() is the opening quote. Returns the decoded string; sets failed on an
    // unterminated string or bad escape. Full escape set incl. \uXXXX (surrogate
    // pairs combined; a lone/unpaired surrogate -> U+FFFD, Track-03 rule).
    string parseRawString() {
        i = i + 1;
        StringBuilder sb = StringBuilder();
        while (i < s.length()) {
            int c = s.byteAt(i);
            if (c == 34) { i = i + 1; return sb.toString(); }
            if (c == 92) {
                i = i + 1;
                if (i >= s.length()) { this.fail(); return ""; }
                int e = s.byteAt(i);
                if (e == 34)       { sb.add("\""); i = i + 1; }
                else if (e == 92)  { sb.add("\\"); i = i + 1; }
                else if (e == 47)  { sb.add("/"); i = i + 1; }
                else if (e == 98)  { sb.add(std::byteToString(8));  i = i + 1; }
                else if (e == 102) { sb.add(std::byteToString(12)); i = i + 1; }
                else if (e == 110) { sb.add(std::byteToString(10)); i = i + 1; }
                else if (e == 114) { sb.add(std::byteToString(13)); i = i + 1; }
                else if (e == 116) { sb.add(std::byteToString(9));  i = i + 1; }
                else if (e == 117) { sb.add(this.parseUnicode()); }
                else { this.fail(); return ""; }
            } else {
                sb.add(std::byteToString(c));
                i = i + 1;
            }
        }
        this.fail();
        return "";
    }
    // i points at 'u'. Reads \uXXXX (already past the backslash). Returns the
    // UTF-8 bytes of the scalar (charFromCode(cp).toString()).
    string parseUnicode() {
        i = i + 1;                                   // past 'u'
        int cp = this.hex4();
        if (cp < 0) { this.fail(); return ""; }
        if (cp >= 55296 && cp <= 56319) {            // high surrogate D800..DBFF
            int save = i;
            if (i + 1 < s.length() && s.byteAt(i) == 92 && s.byteAt(i + 1) == 117) {
                i = i + 2;
                int lo = this.hex4();
                if (lo >= 56320 && lo <= 57343) {    // low surrogate DC00..DFFF
                    int combined = 65536 + ((cp - 55296) << 10) + (lo - 56320);
                    return json::utf8(combined);
                }
                i = save;                            // not a valid pair; rewind
            }
            return json::utf8(65533);                // U+FFFD
        }
        if (cp >= 56320 && cp <= 57343) return json::utf8(65533);
        return json::utf8(cp);
    }
    int hex4() {
        if (i + 4 > s.length()) return 0 - 1;
        int v = 0;
        int k = 0;
        while (k < 4) {
            int d = encoding::hexNibble(s.byteAt(i));
            if (d < 0) return 0 - 1;
            v = v * 16 + d;
            i = i + 1;
            k = k + 1;
        }
        return v;
    }
}

namespace json {
    JsonValue? parse(string s) {
        JsonParser p = JsonParser(s);
        JsonValue v = p.parseValue(0);
        if (p.failed) return None;
        p.skipWs();
        if (p.i != s.length()) return None;          // trailing garbage (strict)
        return v;
    }
    string render(JsonValue v) => v.render();

    // string escaping: " \ and \b \f \n \r \t as short escapes, other control
    // bytes as \u00XX; non-ASCII passes through raw (UTF-8 in, UTF-8 out).
    string renderStr(string v) {
        StringBuilder sb = StringBuilder();
        sb.add("\"");
        string hex = "0123456789abcdef";
        int i = 0;
        while (i < v.length()) {
            int c = v.byteAt(i);
            if (c == 34)      sb.add("\\\"");
            else if (c == 92) sb.add("\\\\");
            else if (c == 8)  sb.add("\\b");
            else if (c == 12) sb.add("\\f");
            else if (c == 10) sb.add("\\n");
            else if (c == 13) sb.add("\\r");
            else if (c == 9)  sb.add("\\t");
            else if (c < 32) {
                sb.add("\\u00");
                sb.add(hex.charAt((c >> 4) & 15));
                sb.add(hex.charAt(c & 15));
            } else {
                sb.add(std::byteToString(c));
            }
            i = i + 1;
        }
        sb.add("\"");
        return sb.toString();
    }
    // string -> float for the parser. The number syntax is already validated by
    // JsonParser.parseNumber, so toFloat() succeeds; the ?? keeps this free of a
    // narrowed int?/float? crossing an argument boundary in the prelude.
    float toNum(string s) => s.toFloat() ?? 0.0;

    // UTF-8 encode a scalar to its bytes — done by hand over std::byteToString
    // (not char.toString(): `char` is LLVM-excluded, Track 03), so it lowers on
    // every active engine. Caller guarantees cp is a non-surrogate scalar.
    string utf8(int cp) {
        StringBuilder sb = StringBuilder();
        if (cp < 128) {
            sb.add(std::byteToString(cp));
        } else if (cp < 2048) {
            sb.add(std::byteToString(192 | (cp >> 6)));
            sb.add(std::byteToString(128 | (cp & 63)));
        } else if (cp < 65536) {
            sb.add(std::byteToString(224 | (cp >> 12)));
            sb.add(std::byteToString(128 | ((cp >> 6) & 63)));
            sb.add(std::byteToString(128 | (cp & 63)));
        } else {
            sb.add(std::byteToString(240 | (cp >> 18)));
            sb.add(std::byteToString(128 | ((cp >> 12) & 63)));
            sb.add(std::byteToString(128 | ((cp >> 6) & 63)));
            sb.add(std::byteToString(128 | (cp & 63)));
        }
        return sb.toString();
    }
}
)prelude";

// --- kPreludeWasm: Track W W-M3 the JS/DOM bridge surface -----------------
// techdesign-05-dom-bridge.md §2-§6 / hard-06-hostbridge-seam.md. The
// hand-written v1 `Dom` prelude surface (doc 05 §2): opaque JS values wrapped
// in handle classes (an `int` slot, nothing more, §2), reached through the
// generic `std::sysHost*` bridge natives (hard-06), with DOM events surfaced as
// §13 stream endpoints (§6). Shipped in the SHARED prelude for now (doc 05 §1 /
// overview §5: "ordinary prelude code until the packaging ruling" — W-M4/doc 06
// §2 makes it a per-target segment). On native targets the bridge natives raise
// (a wasm-gained capability); nothing in the shared corpus reaches them, so the
// four-lane differential is unaffected. Every symbol is namespaced (`Dom`,
// `std::sysHost*`) so it cannot collide with existing surface.
const char* kPreludeWasm = R"prelude(
namespace std {
    // The JS/DOM host bridge (hard-06). op selects the operation; h0 is the
    // primary node handle (0 = document/none), h1 a secondary handle (child /
    // closure-table index), a/b string args ("" when unused). The three
    // return-type-distinguished decls share one C entry (lvrt_hostcall); the
    // result's dynamic tag is decided host-side by op. Marshaling — every
    // Leviathan value <-> JS value conversion — is one reflective routine in
    // lv_host.js (doc 05 §3), never one import per method.
    int    sysHostI(string op, int h0, int h1, string a, string b);
    string sysHostS(string op, int h0, int h1, string a, string b);
    void   sysHostV(string op, int h0, int h1, string a, string b);
    // Register a handler closure as a doc-05 §5 ROOT (retained host-side) ->
    // its closure-table index. addEventListener rides sysHostV with h1 = idx;
    // removeEventListener rides sysHostV and releases idx; the live count is
    // sysHostI("cloCount", 0, 0, "", "") (the §5 leak pin).
    int    sysHostCloReg((int) => void cb);
    // The reflective round-trip probe (doc 05 §8): marshal v to JS and return a
    // host-side rendering (exercises every tag). Not a DOM op; pins only.
    string sysHostEcho<T>(T v);
}

// A DOM event, handle-wrapped (doc 05 §2). The bridge hands the trampoline a
// bare int handle; this wraps it so user handlers take a typed DomEvent.
class DomEvent {
    int h;
    new DomEvent(int handle) { h = handle; }
    int handle() => h;
    string type() => std::sysHostS("eventType", h, 0, "", "");
    // value of a form control the event targeted ("" if none) — enough for the
    // Atlantis demo's input handling without a full Event surface.
    string targetValue() => std::sysHostS("eventTargetValue", h, 0, "", "");
}

// A DOM node, handle-wrapped (doc 05 §2): an int slot, nothing more. Leviathan
// never dereferences the handle — only the JS glue does. Methods chain, mapping
// 1:1 onto the bridge ops.
class DomNode {
    int h;
    new DomNode(int handle) { h = handle; }
    int handle() => h;
    bool exists() => h != 0;                    // getElementById miss -> 0

    DomNode setAttr(string name, string value) {
        std::sysHostV("setAttribute", h, 0, name, value);
        return this;
    }
    string attr(string name) => std::sysHostS("getAttribute", h, 0, name, "");
    DomNode setText(string t) {
        std::sysHostV("setText", h, 0, t, "");
        return this;
    }
    string text() => std::sysHostS("getText", h, 0, "", "");
    DomNode append(DomNode child) {
        std::sysHostV("appendChild", h, child.h, "", "");
        return this;
    }

    // Events as a §13 stream endpoint (doc 05 §6): one host listener per
    // subscription feeds the trampoline, which pushes DomEvents into an ordinary
    // StreamBuffer. The closure is a §5 root; the stream's IDisposable teardown
    // detaches the listener AND releases the root (removeEventListener), so
    // `using` / close() returns the closure table to size 0 (the leak pin).
    InStream<DomEvent> events(string type) {
        StreamBuffer<DomEvent> buf = StreamBuffer();
        OutStream<DomEvent> w = OutStream(buf);
        int idx = std::sysHostCloReg((int eh) => w << DomEvent(eh));
        std::sysHostV("addEventListener", h, idx, type, "");
        InStream<DomEvent> r = InStream(buf);
        int node = h;
        r.onDispose = () => std::sysHostV("removeEventListener", node, idx, type, "");
        r.hasDispose = true;
        return r;
    }

    // Direct handler form; returns a listener token (the closure index) so a
    // caller can off() it — the churn-corpus "bind before holding" discipline
    // (doc 05 §5).
    int on(string type, (DomEvent) => void cb) {
        var c = cb;
        int idx = std::sysHostCloReg((int eh) => { var f = c; f(DomEvent(eh)); });
        std::sysHostV("addEventListener", h, idx, type, "");
        return idx;
    }
    void off(string type, int listener) {
        std::sysHostV("removeEventListener", h, listener, type, "");
    }

    // Synthesize an event on this node — a real DOM dispatchEvent (in a page the
    // user clicks; a headless harness / test self-dispatches through the same
    // API, doc 05 §8).
    void dispatch(string type) {
        std::sysHostV("dispatchEvent", h, 0, type, "");
    }
    void click() { this.dispatch("click"); }

    void release() { std::sysHostV("release", h, 0, "", ""); }
}

// The document surface (doc 05 §2): the well-known entry points. document-scoped
// ops ignore the primary handle (h0 = 0). A getElementById miss returns a
// DomNode with handle 0 (exists() == false).
namespace Dom {
    DomNode body()            => DomNode(std::sysHostI("documentBody", 0, 0, "", ""));
    DomNode create(string t)  => DomNode(std::sysHostI("createElement", 0, 0, t, ""));
    DomNode textNode(string s)=> DomNode(std::sysHostI("createTextNode", 0, 0, s, ""));
    DomNode byId(string id)   => DomNode(std::sysHostI("getElementById", 0, 0, id, ""));
    // The §5 leak-pin meter: live closure roots (0 when every listener is off).
    int listenerCount()       => std::sysHostI("cloCount", 0, 0, "", "");
}
)prelude";

// --- kPreludeExpr: LA-31 expression reification `expr::Expr<F>` -----------
// The reified-tree node taxonomy. Bodies are trivial field stores only (the
// checker never walks the prelude — prelude-not-checked rule); constructor
// parameter names deliberately differ from field names so every store is an
// unambiguous bare write. No module-level `expr::` globals (emit-C++ eager-
// global-instance footgun) and no methods beyond constructors (the tree is
// data; behavior lives in consumers).
const char* kPreludeExpr = R"prelude(
namespace expr {

    class Node { }

    class Field : Node {
        Array<string> path;
        new Field(Array<string> p) { path = p; }
    }

    class Lit : Node {
        string | int | float | bool | None v;
        new Lit(string | int | float | bool | None value) { v = value; }
    }

    class Bind : Node {
        int slot;
        new Bind(int s) { slot = s; }
    }

    class Bin : Node {
        string op;
        Node l;
        Node r;
        new Bin(string o, Node left, Node right) { op = o; l = left; r = right; }
    }

    class Un : Node {
        string op;
        Node e;
        new Un(string o, Node inner) { op = o; e = inner; }
    }

    class Call : Node {
        string name;
        Node recv;
        Array<Node> args;
        new Call(string n, Node receiver, Array<Node> a) { name = n; recv = receiver; args = a; }
    }

    class Assign : Node {
        Field target;
        Node value;
        new Assign(Field t, Node val) { target = t; value = val; }
    }

    class Expr<F> {
        F fn;
        expr::Node tree;
        Array<string | int | float | bool | None> binds;
        int siteId;
        new Expr(F f, expr::Node t, Array<string | int | float | bool | None> b, int s) {
            fn = f; tree = t; binds = b; siteId = s;
        }
    }
}
)prelude";

bool isTypeKind(SymbolKind k) {
    return k == SymbolKind::Class || k == SymbolKind::TypeParam ||
           k == SymbolKind::Primitive;
}

Symbol* findLocal(Scope* scope, std::string_view name, SymbolKind kind) {
    const std::vector<Symbol*>* v = scope->localLookup(name);
    if (!v) return nullptr;
    for (Symbol* s : *v)
        if (s->kind == kind) return s;
    return nullptr;
}

void addToScope(Scope* scope, Symbol* sym) {
    scope->names[sym->name].push_back(sym);
}

// --- 005 R2/R3: match-arm value→type reclassification helpers ---------------

// One classifiable leaf of a match-arm value pattern: a pure `::`-chain
// (namespace `path` + leaf `name`) or a bare `Name` (empty path).
struct ArmLeaf {
    std::vector<std::string_view> path;   // namespace segments (empty for bare Name)
    std::string_view name;                // final segment
    SourceSpan span;
};

// Extract a bare `Name` or a pure `::`-chain into an ArmLeaf. Returns false for
// anything else — a `.`-link anywhere (colon=false), a Call, Index, etc. — so
// only genuine qualified-name-shaped patterns are ever reclassified.
bool asChainLeaf(const Expr* e, ArmLeaf& out) {
    if (!e) return false;
    if (e->kind == ExprKind::Name) {
        out.path.clear();
        out.name = e->text;
        out.span = e->span;
        return true;
    }
    if (e->kind == ExprKind::Member && e->colon) {
        std::vector<std::string_view> segs;      // collected leaf..root
        const Expr* cur = e;
        while (cur->kind == ExprKind::Member && cur->colon) {
            segs.push_back(cur->text);
            cur = cur->a.get();
        }
        if (!cur || cur->kind != ExprKind::Name) return false;   // a `.` link ⇒ disqualify
        segs.push_back(cur->text);               // the root Name
        std::reverse(segs.begin(), segs.end());  // now root..leaf
        out.name = segs.back();
        out.path.assign(segs.begin(), segs.end() - 1);
        out.span = e->span;
        return true;
    }
    return false;
}

// Flatten a `|` (Pipe) tree on the value route into its operand leaves.
void collectPipeLeaves(const Expr* e, std::vector<const Expr*>& out) {
    if (e && e->kind == ExprKind::Binary && e->op == TokenKind::Pipe) {
        collectPipeLeaves(e->a.get(), out);
        collectPipeLeaves(e->b.get(), out);
    } else {
        out.push_back(e);
    }
}

// Classify a leaf against `scope`, navigating exactly as resolveType's Named
// case. Returns navOK (false ⇒ root not a namespace, or a middle/leaf miss on a
// qualified chain — the pattern is left untouched). On navOK, sets hasType /
// hasValue from the leaf's visible symbols (a type-kind symbol vs a Var value).
bool leafClassify(const ArmLeaf& lf, Scope* scope, bool& hasType, bool& hasValue) {
    hasType = hasValue = false;
    const std::vector<Symbol*>* cands = nullptr;
    if (lf.path.empty()) {
        for (Scope* sc = scope; sc && !cands; sc = sc->parent)
            cands = sc->localLookup(lf.name);
        if (!cands) return true;                 // unknown bare name: navOK, no symbols
    } else {
        Symbol* ns = nullptr;
        for (size_t i = 0; i < lf.path.size(); ++i) {
            std::string_view seg = lf.path[i];
            if (i == 0)
                for (Scope* sc = scope; sc && !ns; sc = sc->parent)
                    ns = findLocal(sc, seg, SymbolKind::Namespace);
            else
                ns = (ns && ns->scope) ? findLocal(ns->scope, seg, SymbolKind::Namespace)
                                       : nullptr;
            if (!ns) return false;               // root not a namespace / middle miss
        }
        if (!ns || !ns->scope) return false;
        cands = ns->scope->localLookup(lf.name);
        if (!cands) return false;                // leaf absent in the namespace
    }
    for (Symbol* s : *cands) {
        if (isTypeKind(s->kind)) hasType = true;
        else if (s->kind == SymbolKind::Var) hasValue = true;
    }
    return true;
}

// A leaf is a clean type leaf iff navigation succeeded and it names a type but
// not a value (ambiguous both-cases are handled explicitly at the R2 call site).
bool leafIsType(const ArmLeaf& lf, Scope* scope) {
    bool hasType = false, hasValue = false;
    return leafClassify(lf, scope, hasType, hasValue) && hasType && !hasValue;
}

}  // namespace

// ---------------------------------------------------------------------------
//  gather
// ---------------------------------------------------------------------------

Program Resolver::parsePrelude() {
    preludeFile_.name = "<prelude>";
    preludeFile_.text = std::string(kPreludeCore) + kPreludeStd +
                        kPreludeRest + kPreludeRegexCore + kPreludeRegexApi + kPreludeWeb +
                        kPreludeWasm + kPreludeExpr;
    DiagnosticSink dummy;  // the prelude is trusted; ignore its diagnostics
    Lexer lexer(preludeFile_, dummy);
    Parser parser(lexer.tokenize(), preludeFile_, dummy);
    return parser.parseProgram();
}

void Resolver::gatherClass(Stmt* cls, Scope* parent) {
    if (findLocal(parent, cls->name, SymbolKind::Class))
        sink_.error(cls->span, "duplicate class '" + std::string(cls->name) + "'");

    Symbol* sym = sema_.newSymbol(SymbolKind::Class, cls->name, cls);
    sym->isValue = cls->isValue;              // `struct`: value semantics
    Scope* classScope = sema_.newScope(parent);
    sym->scope = classScope;
    addToScope(parent, sym);
    classSymbols_.push_back(sym);

    for (std::string_view g : cls->generics)
        addToScope(classScope, sema_.newSymbol(SymbolKind::TypeParam, g));
}

void Resolver::gatherInto(std::vector<StmtPtr>& items, Scope* scope, Symbol* enclosingNs) {
    for (StmtPtr& item : items) {
        Stmt* s = item.get();
        switch (s->kind) {
            case StmtKind::Namespace: {
                Symbol* ns = findLocal(scope, s->name, SymbolKind::Namespace);
                if (!ns) {                       // reopen if already present (§12)
                    ns = sema_.newSymbol(SymbolKind::Namespace, s->name, s);
                    ns->scope = sema_.newScope(scope);
                    addToScope(scope, ns);
                }
                gatherInto(s->body, ns->scope, ns);
                break;
            }
            case StmtKind::Class:
                gatherClass(s, scope);
                break;
            case StmtKind::Member:               // a free function at this level
                if (s->callable) {
                    addToScope(scope, sema_.newSymbol(SymbolKind::Function, s->name, s));
                    // bug.md #32 M2: remember which namespace (if any) directly
                    // encloses this free function, so unchecked prelude bodies can
                    // resolve a bare `Class(...)` construction against it.
                    s->enclosingNs = enclosingNs;
                }
                break;
            case StmtKind::Var:
                addToScope(scope, sema_.newSymbol(SymbolKind::Var, s->name, s));
                break;
            default:
                break;                           // exprs / binds: not names
        }
    }
}

// ---------------------------------------------------------------------------
//  imports (`uses NS;`)
// ---------------------------------------------------------------------------

// Resolve one `uses NS;` and dump all of NS's names into `into` (nearer
// declarations already present win, since lookup returns the front).
void Resolver::importOne(Stmt* s, Scope* into) {
    Symbol* ns = nullptr;
    for (size_t i = 0; i < s->generics.size(); ++i) {
        std::string_view seg = s->generics[i];
        if (i == 0) {
            for (Scope* sc = into; sc && !ns; sc = sc->parent)
                ns = findLocal(sc, seg, SymbolKind::Namespace);
        } else {
            ns = (ns && ns->scope) ? findLocal(ns->scope, seg, SymbolKind::Namespace)
                                   : nullptr;
        }
        if (!ns) { sink_.error(s->span, "unknown namespace '" + std::string(seg) + "'"); break; }
    }
    if (ns && ns->scope)
        for (auto& [name, syms] : ns->scope->names)
            for (Symbol* sym : syms) {
                // An attribute's class symbol is not ordinary-name surface —
                // `@Name` resolution goes through the imports map + namespace
                // scopes (RuleEngine::resolveAttr), never the overlay. Dumping
                // it here would let e.g. `@Row` (Atlantis::Orm) silently
                // shadow a real class `Row` (Atlantis::Data) for every bare
                // type/value use in the importing file.
                if (sym->kind == SymbolKind::Class && sym->decl &&
                    sym->decl->isAttribute) continue;
                into->names[name].push_back(sym);
            }
}

// Resolve one selective `use Path::name (as alias);` (imports.md §3/§4) and
// bind the (possibly aliased) name into `into`. Every declaration kind
// imports uniformly: whatever symbols share the final segment's name in its
// home scope all travel together (the overload-set case for functions).
void Resolver::useOne(Stmt* s, Scope* into) {
    const std::vector<std::string_view>& path = s->generics;
    if (path.empty()) return;                 // parse already reported an error

    Symbol* ns = nullptr;
    const std::vector<Symbol*>* found = nullptr;
    if (path.size() == 1) {
        // No `::` at all: a bare name resolved through the ordinary scope
        // chain (no namespace to cross).
        for (Scope* sc = into; sc && !found; sc = sc->parent)
            found = sc->localLookup(path[0]);
    } else {
        for (size_t i = 0; i + 1 < path.size(); ++i) {
            std::string_view seg = path[i];
            if (i == 0)
                for (Scope* sc = into; sc && !ns; sc = sc->parent)
                    ns = findLocal(sc, seg, SymbolKind::Namespace);
            else
                ns = (ns && ns->scope) ? findLocal(ns->scope, seg, SymbolKind::Namespace)
                                       : nullptr;
            if (!ns) {
                sink_.error(s->span, "unknown namespace '" + std::string(seg) + "'");
                return;
            }
        }
        // A qualified name must not leak out to enclosing scopes (localLookup,
        // not lookup) — same rule as resolveType's Named-path navigation.
        found = ns->scope ? ns->scope->localLookup(path.back()) : nullptr;
    }
    if (!found || found->empty()) {
        sink_.error(s->span, "unknown name '" + std::string(path.back()) + "'");
        return;
    }
    for (Symbol* sym : *found) into->names[s->name].push_back(sym);

    // system-binds.md §5.3 (Channel 1): a qualified `use NS::T;` that resolves
    // to a class/interface also carries the (namespace, type) pair the
    // Checker consults to activate NS's exported bind for T (§1). A bare
    // `use name;` (no `::`, ns == null) has no namespace to search and never
    // activates anything.
    if (ns && (*found)[0]->kind == SymbolKind::Class) {
        s->useResolvedNs = ns;
        s->useResolvedTypeKey = (*found)[0]->name;
    }
}

bool Resolver::hasImports(const std::vector<StmtPtr>& items) const {
    for (const StmtPtr& s : items)
        if (s && (s->kind == StmtKind::UsesImport || s->kind == StmtKind::Use)) return true;
    return false;
}

// A direct-child factory `bind T => …;` (the `s->type`-present form — the same
// filter the Checker's bind stack applied). An object-install `bind expr;` (no
// type) is not a factory bind and stays on its Checker-discovered path.
bool Resolver::hasFactoryBinds(const std::vector<StmtPtr>& items) const {
    for (const StmtPtr& s : items)
        if (s && s->kind == StmtKind::Bind && s->type) return true;
    return false;
}

void Resolver::fillBinds(const std::vector<StmtPtr>& items, Scope* scope) {
    if (!scope) return;
    for (const StmtPtr& s : items) {
        if (!s || s->kind != StmtKind::Bind || !s->type) continue;   // factory form only
        // Key exactly as indexNamespaceBinds does: the resolved canonical type
        // string, or the bare name if unresolved.
        std::string key = s->type->canonical.empty()
            ? std::string(s->type->name) : s->type->canonical;
        // block-scoped-use §3.2: duplicate-in-scope detection lives here now (the
        // substrate owns registration). First-declared wins; the message text is
        // the Checker's verbatim so diagnostics stay byte-identical.
        auto [it, inserted] = scope->binds.emplace(key, s.get());
        (void)it;
        if (!inserted)
            sink_.error(s->span, "duplicate binding for '" + key + "' in this scope");
    }
}

void Resolver::fillDeclBinds(std::vector<StmtPtr>& items, Scope* scope) {
    if (!scope) return;
    fillBinds(items, scope);   // this scope's own direct factory binds
    for (StmtPtr& s : items) {
        if (!s || s->kind != StmtKind::Namespace) continue;
        if (Symbol* ns = findLocal(scope, s->name, SymbolKind::Namespace))
            fillDeclBinds(s->body, ns->scope);
    }
}

// Top-level and namespace-body `uses`/`use` (bug.md #8, imports.md — lexical
// import scoping):
//   - a TOP-LEVEL import (called with the global scope) resolves into its own
//     file's overlay scope, so a top-of-file import covers exactly that file
//     and never leaks program-wide;
//   - an import in a namespace body resolves into that (shared) namespace scope.
// Block-level imports are handled per-block in resolveStmtTypes.
//
// `use` runs before `uses` in the same scope so a selective import's symbol
// lands at the front of the scope's name vector — "specific beats bulk"
// (imports.md §4): Scope::lookup() returns .front(), so a `use` shadows a
// same-named `uses`-dumped symbol even though both add to the same table.
// (Function overloads are unaffected: functionOverloads collects the whole
// vector, not just the front, so two same-named functions still merge.)
void Resolver::processImports(std::vector<StmtPtr>& items, Scope* scope) {
    bool top = (scope == sema_.global);
    for (StmtPtr& item : items) {
        Stmt* s = item.get();
        if (s->kind == StmtKind::Use)
            useOne(s, top ? sema_.fileScopeFor(s->span.offset) : scope);
    }
    for (StmtPtr& item : items) {
        Stmt* s = item.get();
        if (s->kind == StmtKind::UsesImport) {
            importOne(s, top ? sema_.fileScopeFor(s->span.offset) : scope);
        } else if (s->kind == StmtKind::Namespace) {
            if (Symbol* nsSym = findLocal(scope, s->name, SymbolKind::Namespace))
                processImports(s->body, nsSym->scope);
        }
    }
}

// ---------------------------------------------------------------------------
//  type resolution
// ---------------------------------------------------------------------------

std::string Resolver::resolveType(TypeRef* t, Scope* scope) {
    if (!t) return "";
    switch (t->kind) {
        case TypeKind::Inferred:
            return t->canonical = "var";
        case TypeKind::Union: {
            std::string c;
            for (size_t i = 0; i < t->members.size(); ++i) {
                if (i) c += " | ";
                c += resolveType(t->members[i].get(), scope);
            }
            return t->canonical = c;
        }
        case TypeKind::Function: {
            std::string c = "(";
            for (size_t i = 0; i < t->funcParams.size(); ++i) {
                if (i) c += ", ";
                c += resolveType(t->funcParams[i].get(), scope);
            }
            c += ") => " + resolveType(t->funcRet.get(), scope);
            return t->canonical = c;
        }
        case TypeKind::Named: {
            // Build the qualified display/canonical prefix (`Room1::TheClass`).
            std::string qualName;
            for (std::string_view seg : t->path) { qualName += seg; qualName += "::"; }
            qualName += std::string(t->name);

            // Rule hygiene (bug.md #22, type position): a rule may have spliced
            // `$C` as a type and pre-resolved it to the matched class. Kept as a
            // FALLBACK only (not an override — overriding would lock in a stale
            // pass-1 symbol on re-resolution and break ordinary base-chain
            // checks): used just when by-name lookup below finds nothing or a
            // non-type, e.g. a `$C`-as-type colliding with the same-named
            // `bool $C = seed(...)` value the injection itself declares.
            Symbol* hygieneSym = (t->resolvedSymbol && isTypeKind(t->resolvedSymbol->kind))
                                 ? t->resolvedSymbol : nullptr;
            Symbol* sym = nullptr;
            bool navFailed = false;
            if (t->path.empty()) {
                sym = scope->lookup(t->name);
                // An attribute's class symbol is not an ordinary TYPE — `@Row`
                // (Atlantis::Orm) must not silently shadow a real class named
                // Row (Atlantis::Data) imported into the same file. Prefer a
                // non-attribute type when one is visible.
                if (sym && sym->decl && sym->decl->isAttribute) {
                    Symbol* nonAttr = nullptr;
                    for (Scope* sc = scope; sc && !nonAttr; sc = sc->parent)
                        if (const auto* v = sc->localLookup(t->name))
                            for (Symbol* c : *v)
                                if (isTypeKind(c->kind) &&
                                    !(c->decl && c->decl->isAttribute)) { nonAttr = c; break; }
                    if (nonAttr) sym = nonAttr;
                }
                if (hygieneSym && (!sym || !isTypeKind(sym->kind))) sym = hygieneSym;
            } else {
                // ::-qualified: walk the namespace path (§12), then find the type
                // in the final namespace's OWN scope (localLookup, not lookup —
                // a qualified name must not leak out to enclosing scopes).
                Symbol* ns = nullptr;
                for (size_t i = 0; i < t->path.size(); ++i) {
                    std::string_view seg = t->path[i];
                    if (i == 0)
                        for (Scope* sc = scope; sc && !ns; sc = sc->parent)
                            ns = findLocal(sc, seg, SymbolKind::Namespace);
                    else
                        ns = (ns && ns->scope) ? findLocal(ns->scope, seg, SymbolKind::Namespace)
                                               : nullptr;
                    if (!ns) {
                        sink_.error(t->span, "unknown namespace '" + std::string(seg) + "'");
                        navFailed = true;
                        break;
                    }
                }
                if (ns && ns->scope)
                    if (const auto* v = ns->scope->localLookup(t->name)) {
                        for (Symbol* c : *v)
                            if (isTypeKind(c->kind) &&
                                !(c->decl && c->decl->isAttribute)) { sym = c; break; }
                        if (!sym)
                            for (Symbol* c : *v) if (isTypeKind(c->kind)) { sym = c; break; }
                    }
            }

            if (navFailed) { /* namespace error already reported */ }
            else if (!sym)
                sink_.error(t->span, "unknown type '" + qualName + "'");
            else if (!isTypeKind(sym->kind))
                sink_.error(t->span, "'" + qualName + "' is not a type");
            else
                t->resolvedSymbol = sym;

            std::string c = qualName;
            if (!t->generics.empty()) {
                c += "<";
                for (size_t i = 0; i < t->generics.size(); ++i) {
                    if (i) c += ", ";
                    c += resolveType(t->generics[i].get(), scope);
                }
                c += ">";
            }
            return t->canonical = c;
        }
    }
    return t->canonical;
}

// Descend through an expression to resolve the TypeRefs a `match` carries — its
// arm patterns, and (recursively) any declarations in an arm body. The
// statement-level pass otherwise never reaches these: a `match` reaching a
// statement position is wrapped in an `ExprStmt` (or sits in a Var initializer /
// return / etc.), so a union-typed `Var` declared *inside* an arm block never
// had its type resolved, leaving the union's members unresolved (canonical "")
// and misfiring the inner match's exhaustiveness check. Only `match` arm
// patterns/bodies are resolved here; `is`/`inject` targets stay on the checker's
// existing lazy path, so programs with no nested match are unaffected.
void Resolver::resolveExprTypes(Expr* e, Scope* scope) {
    if (!e) return;
    if (e->kind == ExprKind::Match) {
        resolveExprTypes(e->a.get(), scope);         // subject
        for (MatchArm& arm : e->arms) {
            reclassifyMatchArm(arm, scope);          // 005 R2/R3: value → type pattern
            if (arm.type)      resolveType(arm.type.get(), scope);
            if (arm.value)     resolveExprTypes(arm.value.get(), scope);
            if (arm.bodyExpr)  resolveExprTypes(arm.bodyExpr.get(), scope);
            if (arm.bodyBlock) resolveStmtTypes(arm.bodyBlock.get(), scope);
        }
        return;
    }
    // Generic descent: find matches nested anywhere inside the expression.
    resolveExprTypes(e->a.get(), scope);
    resolveExprTypes(e->b.get(), scope);
    resolveExprTypes(e->c.get(), scope);
    for (ExprPtr& x : e->list) resolveExprTypes(x.get(), scope);
    if (e->block) resolveStmtTypes(e->block.get(), scope);   // lambda block body
}

// 005 R2/R3: `NS::Type` and `Enum::Member` share a token shape, so the parser
// leaves every `::`-qualified arm head on the neutral value route. Here — in the
// pass all four engines share, before Eval/Lower — reclassify to a type pattern
// when the leaf actually names a type. Parse neutrally, classify semantically.
void Resolver::reclassifyMatchArm(MatchArm& arm, Scope* scope) {
    if (!arm.value) return;
    Expr* v = arm.value.get();

    // R3: a `|` union on the value route (`ns::Sub | None =>`). `|` binds tighter
    // than the arm route, so it parses as a Binary(Pipe) tree. If ANY leaf is a
    // type, ALL must be — build a Union type pattern; a mixed tree is an error;
    // no type leaves ⇒ leave it (a bitwise-or of int consts stays a value).
    if (v->kind == ExprKind::Binary && v->op == TokenKind::Pipe) {
        std::vector<const Expr*> leaves;
        collectPipeLeaves(v, leaves);
        std::vector<ArmLeaf> parsed(leaves.size());
        std::vector<bool> isType(leaves.size(), false);
        int types = 0;
        for (size_t i = 0; i < leaves.size(); ++i) {
            if (asChainLeaf(leaves[i], parsed[i]) && leafIsType(parsed[i], scope)) {
                isType[i] = true;
                ++types;
            }
        }
        if (types == 0) return;                       // stays a value pattern
        if (types != (int)leaves.size()) {
            sink_.error(v->span, "mixed type/value '|' match pattern");
            return;
        }
        auto uni = std::make_unique<TypeRef>(TypeKind::Union);
        uni->span = v->span;
        for (const ArmLeaf& lf : parsed) {
            auto ref = std::make_unique<TypeRef>(TypeKind::Named);
            ref->span = lf.span;
            ref->path = lf.path;
            ref->name = lf.name;
            uni->members.push_back(std::move(ref));
        }
        resolveType(uni.get(), scope);
        arm.type = std::move(uni);
        arm.value = nullptr;
        return;
    }

    // R2: a single pure `::`-chain. A bare Name never reaches the value route
    // (the parser sends it to parseType), so require a non-empty namespace path.
    ArmLeaf lf;
    if (!asChainLeaf(v, lf) || lf.path.empty()) return;
    bool hasType = false, hasValue = false;
    if (!leafClassify(lf, scope, hasType, hasValue)) return;   // nav miss ⇒ leave
    if (hasType && hasValue) {
        std::string spelled;
        for (std::string_view seg : lf.path) { spelled += seg; spelled += "::"; }
        spelled += std::string(lf.name);
        sink_.error(v->span, "ambiguous match pattern '" + spelled +
                             "': names both a type and a value");
        return;
    }
    if (hasType) {                                   // type, no value ⇒ type pattern
        auto ref = std::make_unique<TypeRef>(TypeKind::Named);
        ref->span = v->span;
        ref->path = lf.path;
        ref->name = lf.name;
        resolveType(ref.get(), scope);
        arm.type = std::move(ref);
        arm.value = nullptr;
    }
    // value-only, or neither ⇒ leave as a value pattern (backstop/runtime stay loud).
}

void Resolver::resolveStmtTypes(Stmt* s, Scope* scope) {
    if (!s) return;
    switch (s->kind) {
        case StmtKind::Block: {
            // designs/complete/techdesign-block-scoped-use.md §3.2: a block's own lexical
            // scope carries both its imports (`use`/`uses`) and its type-keyed
            // bind table. Materialized lazily — only when the block directly
            // contains an import or a factory `bind` — and consulted here (for
            // type refs), by the Checker (for names + binds), and the Lowerer
            // (for namespace re-derivation) via `s->blockScope`.
            //
            // (a) Unconditional reset: pass 2 (post-fold re-resolution) must
            // never inherit a pass-1 scope chain holding pass-1 symbols. Owned
            // by the substrate, not by luck (§1.2.3 / §5 P3a).
            s->blockScope = nullptr;
            Scope* inner = scope;
            if (hasImports(s->body) || hasFactoryBinds(s->body)) {
                inner = sema_.newScope(scope);
                // `use` before `uses` — same shadowing order as processImports.
                for (StmtPtr& c : s->body)
                    if (c->kind == StmtKind::Use) useOne(c.get(), inner);
                for (StmtPtr& c : s->body)
                    if (c->kind == StmtKind::UsesImport) importOne(c.get(), inner);
                s->blockScope = inner;
            }
            for (StmtPtr& c : s->body) resolveStmtTypes(c.get(), inner);
            // (b) Bind table fills AFTER the child walk: a bind's key is its
            // bound type's canonical string, which resolveStmtTypes' Bind case
            // (below) is what resolves.
            if (s->blockScope) fillBinds(s->body, s->blockScope);
            break;
        }
        case StmtKind::Var:
            if (s->type) resolveType(s->type.get(), scope);
            resolveExprTypes(s->init.get(), scope);
            break;
        case StmtKind::ExprStmt:
        case StmtKind::Return:
        case StmtKind::Throw:
            resolveExprTypes(s->expr.get(), scope);
            break;
        case StmtKind::If:
            // §16.5 Phase 3 §9: a comptime-if's branches are not unconditional
            // syntax — the rule stage folds the condition and keeps only the
            // taken branch before pass 2. Resolving both branches in pass 1
            // would type-check code that may never survive the fold (and,
            // per the "only syntax is unconditional" stance, never should).
            if (s->isComptime) break;
            resolveExprTypes(s->expr.get(), scope);
            resolveStmtTypes(s->thenBranch.get(), scope);
            resolveStmtTypes(s->elseBranch.get(), scope);
            break;
        case StmtKind::While:
        case StmtKind::DoWhile:
            resolveExprTypes(s->expr.get(), scope);
            resolveStmtTypes(s->thenBranch.get(), scope);
            break;
        case StmtKind::For:
            resolveStmtTypes(s->forInit.get(), scope);
            resolveExprTypes(s->expr.get(), scope);
            resolveExprTypes(s->forStep.get(), scope);
            resolveStmtTypes(s->thenBranch.get(), scope);
            break;
        case StmtKind::ForIn:
            if (s->type) resolveType(s->type.get(), scope);
            resolveExprTypes(s->expr.get(), scope);
            resolveStmtTypes(s->thenBranch.get(), scope);
            break;
        case StmtKind::Try:
            resolveStmtTypes(s->thenBranch.get(), scope);
            for (CatchClause& c : s->catches) {
                if (c.type) resolveType(c.type.get(), scope);
                resolveStmtTypes(c.body.get(), scope);
            }
            break;
        case StmtKind::Bind:                     // §12.5: resolve the bound type + factory body
            if (s->type) resolveType(s->type.get(), scope);
            if (s->memberBody) resolveStmtTypes(s->memberBody.get(), scope);
            break;
        default:
            break;
    }
}

void Resolver::resolveMember(Stmt* m, Scope* classScope) {
    // A generic method/function introduces its own type-param slots (same rule
    // as a generic class — just a different carrier).
    Scope* scope = classScope;
    if (!m->generics.empty()) {
        scope = sema_.newScope(classScope);
        for (std::string_view g : m->generics)
            addToScope(scope, sema_.newSymbol(SymbolKind::TypeParam, g));
    }
    if (m->type) resolveType(m->type.get(), scope);
    for (Param& p : m->params)
        if (p.type) resolveType(p.type.get(), scope);
    if (m->memberBody) resolveStmtTypes(m->memberBody.get(), scope);
}

void Resolver::resolveTypesIn(std::vector<StmtPtr>& items, Scope* scope) {
    // At the top level, each item's own types/exprs resolve through its file's
    // import overlay (bug.md #8) so a top-of-file `uses` is visible in that
    // file only; declarations are still *looked up* in the gather scope (global
    // at top level, the namespace scope when nested), where every name lives.
    bool top = (scope == sema_.global);
    for (StmtPtr& item : items) {
        Stmt* s = item.get();
        Scope* lex = top ? sema_.fileScopeFor(s->span.offset) : scope;
        switch (s->kind) {
            case StmtKind::Namespace: {
                Symbol* ns = findLocal(scope, s->name, SymbolKind::Namespace);
                if (ns) resolveTypesIn(s->body, ns->scope);
                break;
            }
            case StmtKind::Class: {
                Symbol* sym = findLocal(scope, s->name, SymbolKind::Class);
                // Top-level class scopes are re-parented to their file overlay
                // in run(), so `classScope` already reaches the file's imports.
                Scope* classScope = sym ? sym->scope : lex;
                for (TypeRefPtr& base : s->bases) resolveType(base.get(), classScope);
                for (StmtPtr& m : s->body) resolveMember(m.get(), classScope);
                break;
            }
            case StmtKind::Member:               // free function
                resolveMember(s, lex);
                break;
            default:
                // Var, Bind, and top-level executable statements (ExprStmt,
                // If/While/For, Return, ...) all resolve their types the same
                // way a function body does — including descending into `match`
                // arms, so a top-level nested match resolves like a nested one.
                resolveStmtTypes(s, lex);
                break;
        }
    }
}

// ---------------------------------------------------------------------------
//  shapes + collision detection
// ---------------------------------------------------------------------------

Slot Resolver::slotOf(const Stmt* member, Symbol* source) {
    Slot s;
    s.name = member->selector.text;
    s.source = source;
    s.decl = member;
    s.access = member->access;
    s.distinct = member->distinct;
    s.isConst = member->isConst;
    s.isReadonly = member->isReadonly;
    s.isWeak = member->isWeak;
    if (member->callable) {
        s.isMethod = true;
        std::string params = "(";
        for (size_t i = 0; i < member->params.size(); ++i) {
            if (i) params += ", ";
            params += member->params[i].type ? member->params[i].type->canonical : "?";
        }
        params += ")";
        s.paramsCanon = std::move(params);
        s.retCanon = member->type ? member->type->canonical : "void";
        s.canonical = s.paramsCanon + " -> " + s.retCanon;
    } else {
        s.canonical = member->type ? member->type->canonical : "?";
    }
    return s;
}

// True if class `sub` transitively inherits from class `base` (proper: sub != base).
// Walks the AST base clauses, which are resolved before shapes are merged.
static bool classInheritsFrom(Symbol* sub, Symbol* base) {
    if (!sub || !base || sub == base) return false;
    std::vector<Symbol*> stack;
    auto pushBases = [&](Symbol* c) {
        if (!c || !c->decl) return;
        for (const TypeRefPtr& b : c->decl->bases)
            if (b->resolvedSymbol && b->resolvedSymbol->kind == SymbolKind::Class)
                stack.push_back(b->resolvedSymbol);
    };
    pushBases(sub);
    int guard = 0;
    while (!stack.empty() && ++guard < 100000) {
        Symbol* cur = stack.back(); stack.pop_back();
        if (cur == base) return true;
        pushBases(cur);
    }
    return false;
}

void Resolver::mergeSlot(std::vector<Slot>& slots, Slot incoming) {
    // A collision is same name AND same canonical type (§4). Different name or
    // different type coexist (resolution by type disambiguates at the use site).
    for (Slot& s : slots) {
        if (s.name == incoming.name && s.canonical == incoming.canonical) {
            if (s.distinct || incoming.distinct) {
                // Keep both, per source; one degree of `distinct` marks the pair.
                s.distinct = true;
                incoming.distinct = true;
                slots.push_back(incoming);
            } else if (s.isConst != incoming.isConst) {
                // const.md §4: a same-name+same-type collapse whose sides disagree
                // on constness would leave the merged slot's constness ambiguous —
                // refuse to guess rather than silently pick a side.
                sink_.error(incoming.decl ? incoming.decl->span : SourceSpan{},
                            "'" + std::string(incoming.name) + " : " + incoming.canonical +
                            "' is declared both `const` and non-const across bases; "
                            "mark `distinct` or match the declarations");
            } else if (s.isReadonly != incoming.isReadonly) {
                // techdesign-readonly §4.5: same collision rule, parallel arm —
                // a merged slot's write-once-ness can't be ambiguous either.
                sink_.error(incoming.decl ? incoming.decl->span : SourceSpan{},
                            "'" + std::string(incoming.name) + " : " + incoming.canonical +
                            "' is declared both `readonly` and non-readonly across bases; "
                            "mark `distinct` or match the declarations");
            } else if (s.isWeak != incoming.isWeak) {
                sink_.error(incoming.decl ? incoming.decl->span : SourceSpan{},
                            "'" + std::string(incoming.name) + " : " + incoming.canonical +
                            "' is declared both `weak` and strong across bases; "
                            "mark `distinct` or match the declarations");
            } else if (s.source && incoming.source && s.source != incoming.source &&
                       classInheritsFrom(s.source, incoming.source)) {
                // Diamond override (bug.md #65): both bases share a common
                // ancestor that declares this member; the existing slot is a
                // more-derived OVERRIDE of it (its declaring class inherits from
                // the incoming slot's declaring class), while `incoming` is only
                // the ancestor's copy carried in through the other base. The
                // override wins regardless of base order — do NOT let the base
                // copy replace it. (Two genuinely unrelated declarations still
                // fall to "later base wins" below; an incoming override of the
                // existing base also still wins there.)
            } else {
                s = incoming;               // collapse: later/overriding wins
            }
            return;
        }
    }
    slots.push_back(incoming);
}

// Whole-token replace of `from` with `to` inside a canonical type-text string
// (never a substring match inside a longer identifier — "T" must not touch
// "T2" or "MyT").
static std::string substituteGenericToken(const std::string& text, std::string_view from,
                                          const std::string& to) {
    std::string out;
    out.reserve(text.size());
    auto isIdentChar = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };
    size_t i = 0;
    while (i < text.size()) {
        if (text.compare(i, from.size(), from) == 0 &&
            (i == 0 || !isIdentChar(text[i - 1])) &&
            (i + from.size() == text.size() || !isIdentChar(text[i + from.size()]))) {
            out += to;
            i += from.size();
        } else {
            out += text[i];
            ++i;
        }
    }
    return out;
}

static Slot substituteSlotGenerics(Slot s,
                                   const std::vector<std::pair<std::string_view, std::string>>& subst) {
    for (const auto& [from, to] : subst) {
        s.canonical = substituteGenericToken(s.canonical, from, to);
        s.paramsCanon = substituteGenericToken(s.paramsCanon, from, to);
        s.retCanon = substituteGenericToken(s.retCanon, from, to);
    }
    return s;
}

// `canonical` text is built from the REFERENCE SITE's spelling
// (Resolver::resolveType bakes in `path`/`name` as written, not the resolved
// symbol's own identity), so a dependency's class named through an alias
// (`A::Data::Foo`) and the same class named bare from inside its own package
// (`Foo`, as an interface declared in that package spells it) produce
// different canonical strings for the identical Symbol. Recurses through
// generic arguments so `Promise<A::Data::Foo>` also matches a required
// `Promise<Foo>`, not just the bare non-generic case.
static bool namedTypeSameSymbol(const TypeRef* a, const TypeRef* b) {
    if (!a || !b || a->kind != TypeKind::Named || b->kind != TypeKind::Named)
        return false;
    if (!a->resolvedSymbol || a->resolvedSymbol != b->resolvedSymbol) return false;
    if (a->generics.size() != b->generics.size()) return false;
    for (size_t i = 0; i < a->generics.size(); ++i)
        if (!namedTypeSameSymbol(a->generics[i].get(), b->generics[i].get()))
            return false;
    return true;
}

// F6: covariant return satisfaction is intentionally Resolver-local and
// intentionally narrow.  The declaration's resolved symbol proves that the
// provided return is a declared class/interface type; the canonical strings
// are then compared exactly while walking its bases.  No union/function/type-
// expression parsing is involved.  Generic substitutions are carried along
// each base edge so C : R<int> can satisfy a requirement returning R<int>.
bool Resolver::returnAssignable(const Slot& provided, const Slot& required) const {
    if (provided.retCanon == required.retCanon) return true;
    if (!provided.isMethod || !required.isMethod || !provided.decl ||
        !provided.decl->type || provided.decl->type->kind != TypeKind::Named)
        return false;

    Symbol* providedSym = provided.decl->type->resolvedSymbol;
    if (!providedSym || providedSym->kind != SymbolKind::Class) return false;

    // Same declared type under a different spelling: satisfied regardless of
    // the (possibly stale/unrelated) covariant-base walk below, which exists
    // for a genuine subclass return, not spelling variance of one class.
    if (required.decl && namedTypeSameSymbol(provided.decl->type.get(), required.decl->type.get()))
        return true;

    std::string requiredCanon = required.retCanon;
    constexpr std::string_view optionalSuffix = " | None";
    if (requiredCanon.size() > optionalSuffix.size() &&
        requiredCanon.compare(requiredCanon.size() - optionalSuffix.size(),
                              optionalSuffix.size(), optionalSuffix) == 0)
        requiredCanon.resize(requiredCanon.size() - optionalSuffix.size());
    else if (provided.retCanon.size() > optionalSuffix.size() &&
             provided.retCanon.compare(provided.retCanon.size() - optionalSuffix.size(),
                                       optionalSuffix.size(), optionalSuffix) == 0)
        return false; // an optional value cannot satisfy a required non-optional return

    struct Work {
        Symbol* sym;
        std::vector<std::pair<std::string_view, std::string>> subst;
    };
    std::vector<Work> work;
    Work initial{providedSym, {}};
    if (providedSym->decl)
        for (size_t i = 0;
             i < providedSym->decl->generics.size() &&
             i < provided.decl->type->generics.size(); ++i)
            initial.subst.emplace_back(providedSym->decl->generics[i],
                                       provided.decl->type->generics[i]->canonical);
    work.push_back(std::move(initial));
    std::vector<Work> seen;

    while (!work.empty()) {
        Work cur = std::move(work.back());
        work.pop_back();
        if (!cur.sym || !cur.sym->decl) continue;
        bool visited = std::any_of(seen.begin(), seen.end(), [&](const Work& old) {
            if (old.sym != cur.sym || old.subst.size() != cur.subst.size()) return false;
            for (size_t i = 0; i < old.subst.size(); ++i)
                if (old.subst[i].first != cur.subst[i].first ||
                    old.subst[i].second != cur.subst[i].second)
                    return false;
            return true;
        });
        if (visited) continue;
        seen.push_back(cur);

        for (const TypeRefPtr& base : cur.sym->decl->bases) {
            std::string baseCanon = base->canonical;
            for (const auto& [from, to] : cur.subst)
                baseCanon = substituteGenericToken(baseCanon, from, to);
            if (baseCanon == requiredCanon) return true;

            Symbol* baseSym = base->resolvedSymbol;
            if (!baseSym || baseSym->kind != SymbolKind::Class) continue;
            std::vector<std::pair<std::string_view, std::string>> next;
            if (baseSym->decl) {
                for (size_t i = 0;
                     i < baseSym->decl->generics.size() && i < base->generics.size(); ++i) {
                    std::string actual = base->generics[i]->canonical;
                    for (const auto& [from, to] : cur.subst)
                        actual = substituteGenericToken(actual, from, to);
                    next.emplace_back(baseSym->decl->generics[i], std::move(actual));
                }
            }
            work.push_back({baseSym, std::move(next)});
        }
    }
    return false;
}

// Same idea as returnAssignable's identity fallback, but for the PARAMETER
// list that gates interface-satisfaction candidacy in the first place: a
// provided method whose params are spelled through an alias (`A::Data::Foo`)
// must still be recognized as matching an interface requirement spelled bare
// (`Foo`, as written inside its own package) — a plain paramsCanon string
// compare treats the two spellings as different types.
static bool paramsAssignable(const Slot& provided, const Slot& required) {
    if (provided.paramsCanon == required.paramsCanon) return true;
    if (!provided.decl || !required.decl ||
        provided.decl->params.size() != required.decl->params.size())
        return false;
    for (size_t i = 0; i < provided.decl->params.size(); ++i) {
        const TypeRefPtr& pt = provided.decl->params[i].type;
        const TypeRefPtr& rt = required.decl->params[i].type;
        if (!pt || !rt) return false;
        if (pt->canonical == rt->canonical) continue;
        if (!namedTypeSameSymbol(pt.get(), rt.get())) return false;
    }
    return true;
}

void Resolver::buildShape(Symbol* cls) {
    if (cls->shape.built) return;
    if (cls->shape.building) {
        sink_.error(cls->decl->span,
                    "inheritance cycle involving '" + std::string(cls->name) + "'");
        return;
    }
    cls->shape.building = true;

    std::vector<Slot> slots;
    std::vector<Slot> interfaceReqs;

    for (const TypeRefPtr& base : cls->decl->bases) {
        Symbol* baseSym = base->resolvedSymbol;
        if (!baseSym || baseSym->kind != SymbolKind::Class) continue;
        buildShape(baseSym);
        // Value types are flat (§9): a struct may implement interfaces but not
        // inherit implementation, and nothing may inherit from a struct.
        if (baseSym->isValue)
            sink_.error(cls->decl->span, "cannot inherit from struct '" +
                        std::string(baseSym->name) + "'; value types are final");
        else if (cls->isValue && !baseSym->isInterface())
            sink_.error(cls->decl->span, "struct '" + std::string(cls->name) +
                        "' cannot inherit implementation; it may only implement interfaces");
        // A base/interface's own slots are collected in ITS OWN generic-
        // parameter names (e.g. `class Seq<T> { IIterator<T> iterator(); }`
        // stores `iterator : () -> IIterator<T>`; `interface IIterator<T> {
        // T next(); }` stores `next : () -> T`). A subclass implementing a
        // *parameterized* instantiation under a differently-named parameter
        // (e.g. `class MapSeq<T, U> : Seq<U> { IIterator<U> iterator() ...
        // }`, or `class MapIterator<T, U> : IIterator<U> { U next() ... }`)
        // is correctly overriding/satisfying it — substitute the base's
        // declared generic names with THIS base clause's actual type-
        // argument text before comparing, or a same-shape method spuriously
        // fails to collapse (mergeSlot) / satisfy (the loop below) purely
        // because the two sides spell their type variable differently.
        std::vector<std::pair<std::string_view, std::string>> subst;
        if (baseSym->decl)
            for (size_t i = 0;
                 i < baseSym->decl->generics.size() && i < base->generics.size(); ++i)
                subst.emplace_back(baseSym->decl->generics[i], base->generics[i]->canonical);
        if (baseSym->isInterface() && !cls->isInterface()) {
            // a class implementing an interface: requirements to satisfy.
            for (const Slot& s : baseSym->shape.slots)
                interfaceReqs.push_back(subst.empty() ? s : substituteSlotGenerics(s, subst));
        } else {
            // class base — or an interface extending an interface, which
            // absorbs the base's requirements as its own
            for (const Slot& s : baseSym->shape.slots)
                mergeSlot(slots, subst.empty() ? s : substituteSlotGenerics(s, subst));
        }
    }

    for (const StmtPtr& m : cls->decl->body) {
        if (m->kind != StmtKind::Member) continue;
        if (m->isCtor || m->isGet || m->isSet) continue;   // not shape slots
        mergeSlot(slots, slotOf(m.get(), cls));
    }

    // Interface satisfaction: the class must declare each required member.
    for (const Slot& req : interfaceReqs) {
        bool ok = false;
        const Slot* nearMiss = nullptr;
        for (const Slot& s : slots) {
            if (s.name != req.name) continue;
            if (s.isMethod && req.isMethod && paramsAssignable(s, req)) {
                if (returnAssignable(s, req)) { ok = true; break; }
                if (!nearMiss) nearMiss = &s;
            } else if (s.canonical == req.canonical) {
                ok = true;
                break;
            }
        }
        if (!ok && nearMiss)
            sink_.error(cls->decl->span,
                        "'" + std::string(cls->name) + "' does not satisfy interface: '" +
                        std::string(nearMiss->name) + " : " + nearMiss->canonical +
                        "' found, but return type '" + nearMiss->retCanon +
                        "' is not assignable to required '" + req.retCanon + "'");
        else if (!ok)
            sink_.error(cls->decl->span,
                        "'" + std::string(cls->name) + "' does not satisfy interface: "
                        "missing '" + std::string(req.name) + " : " + req.canonical + "'");
    }

    cls->shape.slots = std::move(slots);
    cls->shape.built = true;
    cls->shape.building = false;
}

// ---------------------------------------------------------------------------
//  driver
// ---------------------------------------------------------------------------

// Extract a constant integer carrier from an enum member's explicit-value expr.
// v1 accepts an integer literal, optionally negated. Anything else -> false.
static bool enumConstInt(const Expr* e, long long& out) {
    if (!e) return false;
    if (e->kind == ExprKind::IntLit) { out = parseIntLiteral(e->text); return true; }
    if (e->kind == ExprKind::Unary && e->op == TokenKind::Minus && e->a &&
        e->a->kind == ExprKind::IntLit) {
        out = -parseIntLiteral(e->a->text); return true;
    }
    return false;
}

// Track 03 §2: lower each top-level or namespace-local `enum` to a value struct + per-member
// mangled const globals + a `fromCode` free function (Option B, §2.2). We emit
// ordinary source text and parse it with the real front-end (zero fragile
// hand-built AST), then rename the member globals + `fromCode` to `$`-mangled
// interned names — the `$` is excluded from user identifiers (Lexer), so those
// names are unreachable from user code and cannot collide. The struct field
// stays lexable (`_ord`); only the *global* names' `$` is load-bearing.
void Resolver::desugarEnums(Program& program) {
    auto lower = [&](auto&& self, std::vector<StmtPtr>& items) -> void {
    std::vector<StmtPtr> out;
    out.reserve(items.size());
    for (StmtPtr& item : items) {
        if (!item || item->kind != StmtKind::Enum) {
            if (item && item->kind == StmtKind::Namespace)
                self(self, item->body);
            out.push_back(std::move(item));
            continue;
        }
        Stmt* en = item.get();

        // carrier (v1: int only; string carriers deferred — deferal-track03-type-surface.md)
        if (en->type && !(en->type->kind == TypeKind::Named && en->type->path.empty() &&
                          en->type->name == "int"))
            sink_.error(en->type->span,
                        "enum carrier must be 'int' in v1 (string carriers are deferred)");

        // resolve carriers + duplicate-value check
        struct M { std::string_view name; long long carrier; };
        std::vector<M> members;
        long long next = 0;
        for (const StmtPtr& mem : en->body) {
            long long c = next;
            if (mem->init && !enumConstInt(mem->init.get(), c)) {
                sink_.error(mem->init->span, "enum carrier value must be an integer literal");
                c = next;
            }
            for (const M& prev : members)
                if (prev.carrier == c)
                    sink_.error(mem->span,
                                "duplicate enum carrier value " + std::to_string(c) +
                                " (members '" + std::string(prev.name) + "' and '" +
                                std::string(mem->name) + "')");
            members.push_back({mem->name, c});
            next = c + 1;
        }
        if (members.empty()) {
            sink_.error(en->span, "enum '" + std::string(en->name) + "' has no members");
            continue;   // drop the empty enum entirely
        }

        std::string N(en->name);
        auto lit = [](long long v) { return std::to_string(v); };

        // --- generate desugar source (lexable placeholder names) ---
        std::string src;
        src += "struct " + N + " {\n";
        src += "    int _ord = " + lit(members[0].carrier) + ";\n";      // default = first member
        src += "    new " + N + "(int c) { _ord = c; }\n";
        src += "    int code() => _ord;\n";
        src += "    string toString() => match (_ord) {\n";
        for (const M& m : members)
            src += "        " + lit(m.carrier) + " => \"" + std::string(m.name) + "\";\n";
        src += "        else => \"\";\n    };\n";
        src += "    bool (==)(" + N + " o) => _ord == o._ord;\n";
        src += "}\n";
        for (size_t i = 0; i < members.size(); ++i)
            src += N + " __enumg_" + N + "_" + lit((long long)i) + " = " +
                   N + "(" + lit(members[i].carrier) + ");\n";
        src += N + "? __enumfc_" + N + "(int c) => match (c) {\n";
        for (const M& m : members)
            src += "    " + lit(m.carrier) + " => " + N + "(" + lit(m.carrier) + ");\n";
        src += "    else => None;\n};\n";

        // --- parse with the real front-end ---
        program.synthFiles.push_back(SourceFile{"<enum " + N + ">", std::move(src)});
        SourceFile& sf = program.synthFiles.back();
        DiagnosticSink dummy;
        Lexer lexer(sf, dummy);
        Parser parser(lexer.tokenize(), sf, dummy);
        Program sub = parser.parseProgram();
        if (dummy.hasErrors())
            sink_.error(en->span, "internal: enum '" + N + "' desugar failed to parse");

        // --- rename mangled decls, record metadata, splice in place ---
        EnumDesugar meta;
        meta.name = en->name;
        size_t gi = 0;
        for (StmtPtr& si : sub.items) {
            if (si->kind == StmtKind::Var) {                 // a per-member global
                program.synthNames.push_back(N + "$" + std::string(members[gi].name));
                si->name = program.synthNames.back();
                si->isConst = true;
                meta.members.push_back({members[gi].name, members[gi].carrier, si.get()});
                gi++;
            } else if (si->kind == StmtKind::Member && si->callable) {   // fromCode
                program.synthNames.push_back(N + "$fromCode");
                si->name = program.synthNames.back();
                si->selector.text = si->name;
                meta.fromCode = si.get();
            } else if (si->kind == StmtKind::Class) {
                si->access = en->access;                     // the enum's struct
            }
            out.push_back(std::move(si));
        }
        program.enumDesugars.push_back(std::move(meta));
    }
    items = std::move(out);
    };
    lower(lower, program.items);
}

// ---------------------------------------------------------------------------
//  derived struct (==) synthesis (struct-equality §5.5, packet 02)
// ---------------------------------------------------------------------------

// An explicit symbolic `(==)` member — the author's, a rule-injected one, or
// the one desugarEnums generates. Synthesized members never count (they are
// stripped and regenerated each pass).
static bool hasExplicitEq(const Stmt* cls) {
    for (const StmtPtr& m : cls->body)
        if (m->kind == StmtKind::Member && m->callable && !m->isSynthEq &&
            m->selector.symbolic && m->selector.text == "==")
            return true;
    return false;
}

// A struct data field: a non-callable member. Ctors/accessors are callable,
// so the extra guards are belt-and-braces.
static bool isDataField(const Stmt* m) {
    return m->kind == StmtKind::Member && !m->callable && !m->isCtor &&
           !m->isGet && !m->isSet;
}

namespace {
// The §5.2 comparability ladder over resolved field TypeRefs. classify()
// returns "" for comparable, else the parenthetical for the checker's §5.1
// message ("a function value", "an Array", ...). Struct verdicts memoize per
// symbol; the in-progress state fails closed on re-entry (value structs
// cannot cycle by value — infinite size is already rejected — so this only
// guards error-recovery states).
struct EqLadder {
    const std::unordered_map<const Symbol*, const char*>& banned;
    enum V { kInProgress = 1, kYes, kNo };
    std::unordered_map<const Symbol*, int> verdicts;

    std::string classify(const TypeRef* t) {
        if (!t) return "an untyped field";
        switch (t->kind) {
            case TypeKind::Inferred: return "an inferred type";
            case TypeKind::Function: return "a function value";
            case TypeKind::Union:
                // Comparable iff every non-None member is; the None leg is a
                // tag compare that already works end-to-end.
                for (const TypeRefPtr& m : t->members) {
                    if (m->kind == TypeKind::Named && m->path.empty() && m->name == "None")
                        continue;
                    if (std::string bad = classify(m.get()); !bad.empty()) return bad;
                }
                return "";
            case TypeKind::Named: {
                if (t->path.empty() && t->name == "None") return "";
                const Symbol* sym = t->resolvedSymbol;
                if (!sym) return "an unresolvable type";   // fail closed, never silently
                if (sym->kind == SymbolKind::TypeParam) return "a type parameter";
                if (sym->kind != SymbolKind::Class) return "a non-comparable type";
                if (auto b = banned.find(sym); b != banned.end()) return b->second;
                if (sym->isPrimitive) return "";           // int/bool/char/string/float
                if (sym->isValue)
                    return structComparable(sym)
                               ? ""
                               : "a non-comparable struct ('" + std::string(sym->name) + "')";
                return "";   // reference class / interface: identity compare
            }
        }
        return "an unresolvable type";
    }

    bool structComparable(const Symbol* sym) {
        if (auto it = verdicts.find(sym); it != verdicts.end()) return it->second == kYes;
        if (!sym->decl) return false;
        if (hasExplicitEq(sym->decl)) { verdicts[sym] = kYes; return true; }
        if (!sym->decl->generics.empty()) { verdicts[sym] = kNo; return false; }  // v1 restriction
        verdicts[sym] = kInProgress;
        bool ok = true;
        for (const StmtPtr& m : sym->decl->body)
            if (isDataField(m.get()) && !classify(m->type.get()).empty()) { ok = false; break; }
        verdicts[sym] = ok ? kYes : kNo;
        return ok;
    }
};
}  // namespace

void Resolver::synthesizeStructEquality(Program& program) {
    // Erase-then-regenerate (idempotency & the two-pass resolver): rules can
    // inject fields or an explicit (==) between the passes, so every prior
    // synthesized member is dropped FIRST — everywhere, before any
    // classification reads a struct body — then the truth is rebuilt from the
    // current field lists.
    program.structEqSynths.clear();
    auto strip = [](auto&& self, std::vector<StmtPtr>& items) -> void {
        for (StmtPtr& it : items) {
            if (!it) continue;
            if (it->kind == StmtKind::Namespace) { self(self, it->body); continue; }
            if (it->kind == StmtKind::Class)
                it->body.erase(std::remove_if(it->body.begin(), it->body.end(),
                                              [](const StmtPtr& m) { return m->isSynthEq; }),
                               it->body.end());
        }
    };
    strip(strip, program.items);

    // §5.2's named exclusions, resolved to their prelude symbols — a user's
    // own namespaced `Array` is an ordinary reference class and stays
    // identity-comparable. Function types are TypeKind::Function, not a name.
    const std::pair<const char*, const char*> kBanned[] = {
        {"Array", "an Array"},     {"Map", "a Map"},         {"Block", "a Block"},
        {"Ast", "an Ast"},         {"Promise", "a Promise"}, {"Channel", "a Channel"}};
    std::unordered_map<const Symbol*, const char*> banned;
    for (const auto& [n, note] : kBanned)
        if (Symbol* s = findLocal(sema_.global, n, SymbolKind::Class)) banned[s] = note;

    EqLadder ladder{banned};

    auto walk = [&](auto&& self, std::vector<StmtPtr>& items, Scope* scope) -> void {
        for (StmtPtr& it : items) {
            if (!it) continue;
            if (it->kind == StmtKind::Namespace) {
                if (Symbol* ns = findLocal(scope, it->name, SymbolKind::Namespace);
                    ns && ns->scope)
                    self(self, it->body, ns->scope);
                continue;
            }
            if (it->kind != StmtKind::Class || !it->isValue || it->isAttribute) continue;
            Stmt* cls = it.get();
            if (hasExplicitEq(cls)) continue;   // §5.4: the author's relation wins
                                                // (covers desugared enums too)

            program.synthNames.push_back(std::string(cls->name));
            StructEqSynth rec;
            rec.structName = program.synthNames.back();

            // v1: no derived (==) for generic structs (the self-type spelling
            // with type params is deferred; no existing corpus exercises it).
            if (!cls->generics.empty()) {
                rec.badKindNote = "a generic struct (derived '(==)' is not synthesized in v1)";
                program.structEqSynths.push_back(std::move(rec));
                continue;
            }

            // Classify every field; the first non-comparable one gates the
            // struct. No diagnostic here — the gate fires at a use site
            // (packet 03), not at the declaration.
            std::string note;
            const Stmt* bad = nullptr;
            for (const StmtPtr& m : cls->body) {
                if (!isDataField(m.get())) continue;
                note = ladder.classify(m->type.get());
                if (!note.empty()) { bad = m.get(); break; }
            }
            if (bad) {
                rec.badField = std::string(bad->selector.text);
                rec.badKindNote = std::move(note);
                program.structEqSynths.push_back(std::move(rec));
                continue;
            }

            // Generate + parse through the desugarEnums channel (the wrapper
            // struct gives the parser a member context), then lift the member
            // out and splice it into the real struct.
            std::string N(cls->name);
            std::string body;
            for (const StmtPtr& m : cls->body) {
                if (!isDataField(m.get())) continue;
                std::string f(m->selector.text);
                if (!body.empty()) body += " && ";
                // struct-equality design §5.2: float fields compare through the
                // canonical relation (§3), so a struct holding NaN is equal to
                // itself and ±0.0 agree — via float.canonEq (each engine's ONE
                // canon). Every other field kind keeps the scalar `==`.
                const TypeRef* t = m->type.get();
                bool isFloat = t && t->kind == TypeKind::Named && t->path.empty() &&
                               t->name == "float" &&
                               (!t->resolvedSymbol || t->resolvedSymbol->isPrimitive);
                if (isFloat)
                    body += "this." + f + ".canonEq(other." + f + ")";
                else
                    body += "this." + f + " == other." + f;
            }
            if (body.empty()) body = "true";   // zero-field struct: reflexively equal
            std::string src = "struct __eq_" + N + " {\n"
                              "    bool (==)(" + N + " other) => " + body + ";\n"
                              "}\n";

            program.synthFiles.push_back(SourceFile{"<eq " + N + ">", std::move(src)});
            SourceFile& sf = program.synthFiles.back();
            DiagnosticSink dummy;
            Lexer lexer(sf, dummy);
            Parser parser(lexer.tokenize(), sf, dummy);
            Program sub = parser.parseProgram();
            Stmt* wrapper = (sub.items.size() == 1 && sub.items[0]->kind == StmtKind::Class)
                                ? sub.items[0].get() : nullptr;
            if (dummy.hasErrors() || !wrapper || wrapper->body.size() != 1) {
                sink_.error(cls->span,
                            "internal: struct '" + N + "' (==) synthesis failed to parse");
                rec.badKindNote = "an internal synthesis failure";
                program.structEqSynths.push_back(std::move(rec));
                continue;
            }
            StmtPtr method = std::move(wrapper->body[0]);
            method->isSynthEq = true;
            // This pass runs after resolveTypesIn, so resolve the new member's
            // types by hand against the real class scope (param `N`, ret bool).
            if (Symbol* sym = findLocal(scope, cls->name, SymbolKind::Class);
                sym && sym->scope)
                resolveMember(method.get(), sym->scope);
            cls->body.push_back(std::move(method));
            rec.synthesized = true;
            program.structEqSynths.push_back(std::move(rec));
        }
    };
    walk(walk, program.items, sema_.global);
}

// Struct-equality §6 (packet 06): the `float::NaN` language constant. One
// definition, one place — synthesized through the same synth channel the enum
// member globals use (desugarEnums above), so it flows through gather / type
// resolution / check / global-init exactly like hand-written source and every
// engine reads it with zero per-engine work. Decision 1 (ratified): the bit
// pattern is a LANGUAGE constant — 0x7FF8000000000000, the canonical positive
// quiet NaN (§3.1) — no per-target configuration, no `#ifdef`.
void Resolver::synthesizeFloatNaN(Program& program) {
    if (program.floatNaNGlobal) return;   // idempotent: the two-pass resolver
                                          // re-runs on the SAME Program.
    // Materialize the global ONLY when the program actually references
    // `float::NaN` — exactly the enum precedent (enum globals appear only when
    // an `enum` is written). Unconditional injection would push a
    // `float::fromBits(...)` initializer into every program, including the
    // churn/expand corpora whose FROZEN ELF lane cannot lower that native and
    // whose expand-roundtrip re-parses the printed source. `file_.text` is the
    // whole combined source buffer (every user file concatenated); a token scan
    // for the `float :: NaN` sequence is spacing-robust and skips comments and
    // string bodies (the lexer already did). The one language constant, but
    // paid for only where used.
    DiagnosticSink scanSink;
    std::vector<Token> toks = Lexer(file_, scanSink).tokenize();
    bool used = false;
    for (size_t i = 0; i + 2 < toks.size(); ++i)
        if (toks[i].text == "float" && toks[i + 1].kind == TokenKind::ColonColon &&
            toks[i + 2].text == "NaN") { used = true; break; }
    if (!used) return;

    // The `$` in the final mangled name is unlexable in user identifiers, so
    // parse a lexable placeholder first, then rename to `float$NaN` (backed by
    // synthNames for string_view stability) — exactly the desugarEnums move.
    // 9221120237041090560 == 0x7FF8000000000000 (fits a signed int64).
    std::string src = "float __floatnan = float::fromBits(9221120237041090560);\n";
    program.synthFiles.push_back(SourceFile{"<float::NaN>", std::move(src)});
    SourceFile& sf = program.synthFiles.back();
    DiagnosticSink dummy;
    Lexer lexer(sf, dummy);
    Parser parser(lexer.tokenize(), sf, dummy);
    Program sub = parser.parseProgram();
    if (dummy.hasErrors() || sub.items.size() != 1 ||
        sub.items[0]->kind != StmtKind::Var) {
        sink_.error(SourceSpan{}, "internal: float::NaN constant synthesis failed to parse");
        return;
    }
    StmtPtr g = std::move(sub.items[0]);
    program.synthNames.push_back("float$NaN");
    g->name = program.synthNames.back();
    g->isConst = true;                    // follows the enum member globals
    program.floatNaNGlobal = g.get();
    // Prepend, not append: top-level global initializers run in source order,
    // so the constant must be initialized BEFORE any user top-level statement
    // (e.g. `main();`) that reads `float::NaN`. Enum member globals sit at their
    // enum's source position (ahead of the call site); this one has no natural
    // site, so it leads.
    program.items.insert(program.items.begin(), std::move(g));
}

void Resolver::run(Program& program) {
    desugarEnums(program);                    // Track 03 §2: enum -> struct + globals + fromCode
    synthesizeFloatNaN(program);              // struct-equality §6: the float::NaN constant
    sema_.global = sema_.newScope(nullptr);
    // `void` is the only pure primitive with no method surface; int/string/bool/
    // float are declared as value-type classes in the prelude (the object mask).
    addToScope(sema_.global, sema_.newSymbol(SymbolKind::Primitive, "void"));
    addToScope(sema_.global, sema_.newSymbol(SymbolKind::Primitive, "None"));

    preludeProgram_ = parsePrelude();
    gatherInto(preludeProgram_.items, sema_.global);
    // Boundary between prelude and program classes in `classSymbols_`: only the
    // program's classes get file-scoped below (prelude spans live in a separate
    // buffer whose offsets would otherwise collide with the program's).
    size_t preludeClassCount = classSymbols_.size();
    gatherInto(program.items, sema_.global);

    for (const char* p : {"int", "string", "bool", "float", "char"})
        if (Symbol* s = findLocal(sema_.global, p, SymbolKind::Class)) s->isPrimitive = true;

    // Implicit `uses std;` — the standard namespace is always imported. It lives
    // in `global` (visible in every file), not in any one file's overlay.
    if (Symbol* stdNs = findLocal(sema_.global, "std", SymbolKind::Namespace))
        for (auto& [name, syms] : stdNs->scope->names)
            for (Symbol* sym : syms)
                sema_.global->names[name].push_back(sym);

    // Per-file import overlays (bug.md #8). One scope per source file (a child
    // of `global`); a lone source becomes one file covering the whole buffer.
    if (fileRanges_.empty())
        fileRanges_.push_back({0u, (uint32_t)file_.text.size()});
    sema_.fileRanges = fileRanges_;
    sema_.fileScopes.clear();
    for (size_t i = 0; i < fileRanges_.size(); ++i)
        sema_.fileScopes.push_back(sema_.newScope(sema_.global));

    // Re-parent each TOP-LEVEL program class scope (parent == global) to its
    // file's overlay, so file-level `uses` are visible inside the class and a
    // class in one file never sees another file's top-level imports. Namespaced
    // classes (parent == a namespace scope) keep resolving through their
    // namespace; prelude classes are left alone (they live in `global`).
    for (size_t i = preludeClassCount; i < classSymbols_.size(); ++i) {
        Symbol* cls = classSymbols_[i];
        if (cls->scope && cls->scope->parent == sema_.global && cls->decl)
            cls->scope->parent = sema_.fileScopeFor(cls->decl->span.offset);
    }

    // Likewise make each TOP-LEVEL namespace scope see its files' overlays, so a
    // top-of-file `uses B` is visible to code inside a reopened `namespace A {
    // ... }` block — the namespace scope chain otherwise runs straight to global,
    // bypassing the overlay where `uses` names live (bug.md #45).
    //
    // A namespace reopened across files (`namespace Helm` in both proc.lev and
    // command.lev) has ONE shared scope but N blocks, each in its own file with
    // its own top-of-file `uses`. Parenting that single scope to just the FIRST
    // block's overlay made the whole namespace see only that one file's imports:
    // if the first block's file lacked a `uses Sonar` that a later block relies
    // on, every unqualified cross-dependency type in the namespace failed to
    // resolve, order-dependently (regression floor:
    // tests/corpus/project/reopen_ns_uses_order). So instead give each reopened
    // namespace its OWN aggregate overlay (parent == global) and fold in the
    // file overlays of EVERY block, making import visibility order-independent.
    // The parent-== global guard makes one aggregate per shared namespace scope;
    // nested namespaces keep resolving through their encloser.
    std::unordered_map<Symbol*, Scope*> nsAgg;
    std::unordered_map<Symbol*, std::vector<Scope*>> nsBlockOverlays;
    for (StmtPtr& item : program.items) {
        if (item->kind != StmtKind::Namespace) continue;
        Symbol* ns = findLocal(sema_.global, item->name, SymbolKind::Namespace);
        if (!ns || !ns->scope) continue;
        Scope* fileOv = sema_.fileScopeFor(item->span.offset);
        std::vector<Scope*>& ovs = nsBlockOverlays[ns];
        if (std::find(ovs.begin(), ovs.end(), fileOv) == ovs.end()) ovs.push_back(fileOv);
        if (ns->scope->parent == sema_.global) {
            Scope* agg = sema_.newScope(sema_.global);
            ns->scope->parent = agg;
            nsAgg[ns] = agg;
        }
    }

    processImports(program.items, sema_.global);   // resolve `uses` before type resolution

    // Populate each namespace's aggregate overlay from ALL its blocks' file
    // overlays (which processImports has now filled). File overlays hold only
    // imported names, so this copies imports and nothing else.
    for (auto& [ns, agg] : nsAgg)
        for (Scope* ov : nsBlockOverlays[ns])
            for (auto& [name, syms] : ov->names)
                for (Symbol* sym : syms)
                    agg->names[name].push_back(sym);

    resolveTypesIn(preludeProgram_.items, sema_.global);
    resolveTypesIn(program.items, sema_.global);

    // block-scoped-use §3.2(b): register top-level (program-wide) and
    // namespace-body factory binds into their scopes' bind tables, now that
    // their bound types are resolved (canonical keys). Program only — prelude
    // binds reach the checker via the Channel-1 index (namespaceBinds_), never
    // the top-level frame, exactly as before. Block binds were filled per-block.
    fillDeclBinds(program.items, sema_.global);

    // Struct equality §5.5 (packet 02): after types resolve (field
    // classification reads resolvedSymbol), before shapes (the spliced member
    // becomes an ordinary "==" slot).
    synthesizeStructEquality(program);

    for (Symbol* cls : classSymbols_) buildShape(cls);
}

std::string Resolver::dumpShapes() const {
    std::string out;
    for (const std::unique_ptr<Symbol>& sp : sema_.symbols) {
        const Symbol* cls = sp.get();
        if (cls->kind != SymbolKind::Class || !cls->decl) continue;
        out += "Shape " + std::string(cls->name);
        if (cls->isInterface()) out += " (interface)";
        out += "\n";
        for (const Slot& s : cls->shape.slots) {
            out += "  ";
            if (s.distinct) out += "distinct ";
            out += std::string(s.name) + " : " + s.canonical;
            if (s.source && s.source != cls) out += "  (from " + std::string(s.source->name) + ")";
            if (s.isMethod) out += "  [method]";
            out += "\n";
        }
    }
    return out;
}
