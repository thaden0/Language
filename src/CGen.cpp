#include "CGen.hpp"
#include <climits>
#include <functional>
#include <set>
#include <unordered_map>
#include <unordered_set>

// The embedded mini-runtime: a scalar Value with EXACTLY the oracle's
// arithmetic and stringification semantics (arithPrim/valueToString for the
// scalar kinds), so native output is bit-identical to both interpreters.
static const char* kRuntime = R"rt(
#include <cstdio>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <termios.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>
struct Obj;
struct BlockData;   // Track 03 §3
struct V {
    int k = 0;   // 0 void 1 int 2 float 3 bool 4 string 5 object 6 array 7 map 8 none 9 closure 10 char 11 block
    long long i = 0;
    double f = 0;
    bool b = false;
    std::string s;
    std::shared_ptr<Obj> o;
    std::shared_ptr<std::vector<V>> arr;
    std::shared_ptr<std::vector<std::pair<V, V>>> mp;
    std::shared_ptr<BlockData> bl;     // Track 03 §3: byte buffer (k==11)
    int aelem = 0;   // §9 B1: 0 = boxed array (arr holds values); else a value-struct
                     // classId — arr holds fieldCount(aelem) flat slots per element
};
struct Obj {
    int cls = 0;
    int fn = -1;                       // closures (k==9): the lambda's function id
    std::vector<V> slots;              // declared fields — PACKED by shape index (§7)
    std::map<std::string, V> extra;    // dynamic keys (closure captures)
    std::vector<std::weak_ptr<Obj>> weakSlots;
    std::map<std::string, std::weak_ptr<Obj>> weakExtra;
};
struct BlockData {                     // Track 03 §3: slice shares `bytes`, aliasing
    std::shared_ptr<std::vector<unsigned char>> bytes;
    size_t off = 0, len = 0;
};
static int fieldSlot(int cls, const std::string& key);   // generated (or -1 = dynamic)
static int fieldCount(int cls);                          // generated
static V objget(const V& o, const std::string& key) {
    int i = fieldSlot(o.o->cls, key);
    if (i >= 0) return (size_t)i < o.o->slots.size() ? o.o->slots[i] : V{};
    auto it = o.o->extra.find(key);
    return it != o.o->extra.end() ? it->second : V{};
}
static void objset(const V& o, const std::string& key, const V& val) {
    int i = fieldSlot(o.o->cls, key);
    if (i >= 0) { if ((size_t)i >= o.o->slots.size()) o.o->slots.resize(i + 1); o.o->slots[i] = val; }
    else o.o->extra[key] = val;
}
static V weakget(const V& o, const std::string& key) {
    int i = fieldSlot(o.o->cls, key); std::shared_ptr<Obj> p;
    if (i >= 0) {
        if ((size_t)i < o.o->weakSlots.size()) p = o.o->weakSlots[i].lock();
    } else { auto it = o.o->weakExtra.find(key); if (it != o.o->weakExtra.end()) p = it->second.lock(); }
    if (!p) { V n; n.k = 8; return n; }
    V v; v.k = 5; v.o = std::move(p); return v;
}
static void weakset(const V& o, const std::string& key, const V& val) {
    int i = fieldSlot(o.o->cls, key);
    std::weak_ptr<Obj> p; if (val.k == 5 && val.o) p = val.o;
    if (i >= 0) { if ((size_t)i >= o.o->weakSlots.size()) o.o->weakSlots.resize(i + 1); o.o->weakSlots[i] = p; }
    else o.o->weakExtra[key] = p;
}
static V vi(long long x) { V v; v.k = 1; v.i = x; return v; }
static V vf(double x)    { V v; v.k = 2; v.f = x; return v; }
static V vb(bool x)      { V v; v.k = 3; v.b = x; return v; }
static V vs(const char* x) { V v; v.k = 4; v.s = x; return v; }
static V vstr(std::string x) { V v; v.k = 4; v.s = std::move(x); return v; }
static V vch(long long x) { V v; v.k = 10; v.i = x; return v; }   // Track 03 §1: char (Unicode scalar)
static V vblk(std::shared_ptr<BlockData> bd) { V v; v.k = 11; v.bl = std::move(bd); return v; }
static V vblknew(long long n) {                                   // Track 03 §3: Block(n)
    auto bd = std::make_shared<BlockData>();
    size_t len = n < 0 ? 0 : (size_t)n;
    bd->bytes = std::make_shared<std::vector<unsigned char>>(len, 0);
    bd->off = 0; bd->len = len;
    return vblk(bd);
}
static V vblkstr(const std::string& s) {                          // Block::fromString(s)
    auto bd = std::make_shared<BlockData>();
    bd->bytes = std::make_shared<std::vector<unsigned char>>(s.begin(), s.end());
    bd->off = 0; bd->len = bd->bytes->size();
    return vblk(bd);
}
static std::string u8enc(long long cp) {
    std::string o; unsigned long c = (unsigned long)cp;
    if (c <= 0x7F) o.push_back((char)c);
    else if (c <= 0x7FF) { o.push_back((char)(0xC0|(c>>6))); o.push_back((char)(0x80|(c&0x3F))); }
    else if (c <= 0xFFFF) { o.push_back((char)(0xE0|(c>>12))); o.push_back((char)(0x80|((c>>6)&0x3F))); o.push_back((char)(0x80|(c&0x3F))); }
    else { o.push_back((char)(0xF0|(c>>18))); o.push_back((char)(0x80|((c>>12)&0x3F))); o.push_back((char)(0x80|((c>>6)&0x3F))); o.push_back((char)(0x80|(c&0x3F))); }
    return o;
}
static long long u8dec(const std::string& s, size_t i, size_t& len, bool& boundary) {
    len = 1; boundary = true;
    if (i >= s.size()) return 0xFFFD;
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) return c;
    if ((c & 0xC0) == 0x80) { boundary = false; return 0xFFFD; }
    int need; long long cp;
    if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; }
    else return 0xFFFD;
    for (int k = 1; k <= need; ++k) {
        if (i + k >= s.size() || ((unsigned char)s[i+k] & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | ((unsigned char)s[i+k] & 0x3F);
    }
    len = need + 1; return cp;
}
static V vnone() { V v; v.k = 8; return v; }
static V mkobj(int cls) { V v; v.k = 5; v.o = std::make_shared<Obj>(); v.o->cls = cls; v.o->slots.resize(fieldCount(cls)); v.o->weakSlots.resize(fieldCount(cls)); return v; }
static V mkclo(int fn) { V v; v.k = 9; v.o = std::make_shared<Obj>(); v.o->fn = fn; return v; }
static bool isValueClass(int cls);                       // generated
static V mkarr();
static V mkmap();
// Value-type (struct) copy: reference objects alias; value structs deep-copy.
static V copyval(const V& v) {
    if (v.k != 5 || !v.o || !isValueClass(v.o->cls)) return v;
    V r = v; r.o = std::make_shared<Obj>(); r.o->cls = v.o->cls; r.o->fn = v.o->fn;
    for (const V& s : v.o->slots) r.o->slots.push_back(copyval(s));
    for (const auto& kv : v.o->extra) r.o->extra[kv.first] = copyval(kv.second);
    return r;
}
static V transfer1(const V& v, std::unordered_map<const void*, V>& seen) {
    if (v.k == 5 && v.o) {
        auto it = seen.find(v.o.get()); if (it != seen.end()) return it->second;
        V r = mkobj(v.o->cls); seen[v.o.get()] = r;
        for (size_t i = 0; i < v.o->slots.size(); ++i) r.o->slots[i] = transfer1(v.o->slots[i], seen);
        for (const auto& kv : v.o->extra) r.o->extra[kv.first] = transfer1(kv.second, seen);
        // weakSlots/weakExtra intentionally stay empty: the referent is not copied.
        return r;
    }
    if (v.k == 6 && v.arr) {
        auto it = seen.find(v.arr.get()); if (it != seen.end()) return it->second;
        V r = mkarr(); r.aelem = v.aelem; seen[v.arr.get()] = r;
        for (const V& e : *v.arr) r.arr->push_back(transfer1(e, seen)); return r;
    }
    if (v.k == 7 && v.mp) {
        auto it = seen.find(v.mp.get()); if (it != seen.end()) return it->second;
        V r = mkmap(); seen[v.mp.get()] = r;
        for (const auto& kv : *v.mp) r.mp->push_back({transfer1(kv.first, seen), transfer1(kv.second, seen)});
        return r;
    }
    return v;
}
static V threadtransfer(const V& v) { std::unordered_map<const void*, V> seen; return transfer1(v, seen); }
static V mkarr() { V v; v.k = 6; v.arr = std::make_shared<std::vector<V>>(); return v; }
static V mkmap() { V v; v.k = 7; v.mp = std::make_shared<std::vector<std::pair<V, V>>>(); return v; }

// §9 B1: a value-struct array stores each element's fields inline in one flat
// buffer (aelem = the classId, fieldCount(aelem) slots per element) instead of a
// pointer to a separate heap object per element — one allocation, contiguous,
// no pointer chase. Element access materializes a fresh object (value identity);
// field access on it and iteration read the flat slots directly.
static long long arrLen(const V& a) {
    if (!a.arr) return 0;
    return a.aelem ? (long long)a.arr->size() / fieldCount(a.aelem) : (long long)a.arr->size();
}
static V arrGet(const V& a, long long i) {
    if (!a.aelem) return (*a.arr)[i];
    int F = fieldCount(a.aelem); V o = mkobj(a.aelem);
    for (int f = 0; f < F; ++f) o.o->slots[f] = (*a.arr)[i * F + f];
    return o;
}
static void arrPush(V& a, const V& x) {
    if (a.aelem == 0 && a.arr->empty() && x.k == 5 && x.o && isValueClass(x.o->cls))
        a.aelem = x.o->cls;                       // first value-struct element -> go dense
    if (!a.aelem) { a.arr->push_back(x); return; }
    int F = fieldCount(a.aelem);
    for (int f = 0; f < F; ++f)
        a.arr->push_back(f < (int)x.o->slots.size() ? x.o->slots[f] : V{});
}
static void arrSet(V& a, long long i, const V& x) {
    if (!a.aelem) { (*a.arr)[i] = x; return; }
    int F = fieldCount(a.aelem);
    for (int f = 0; f < F; ++f)
        (*a.arr)[i * F + f] = f < (int)x.o->slots.size() ? x.o->slots[f] : V{};
}
// A copy-on-write clone of an array (shares aelem, fresh buffer).
static V arrCow(const V& a) { V r = mkarr(); r.aelem = a.aelem; if (a.arr) *r.arr = *a.arr; return r; }
static std::string ts(const V& v) {
    switch (v.k) {
        case 1: return std::to_string(v.i);
        case 10: return u8enc(v.i);                 // Track 03 §1: char -> UTF-8
        case 11: return "Block(len=" + std::to_string(v.bl ? v.bl->len : 0) + ")";
        case 2: return std::to_string(v.f);
        case 3: return v.b ? "true" : "false";
        case 4: return v.s;
        case 5:
            if (v.o) {
                V a = objget(v, "start"), b = objget(v, "end");
                if (a.k == 1 && b.k == 1)
                    return std::to_string(a.i) + ".." + std::to_string(b.i);
            }
            return "<object>";
        case 6: {
            std::string r = "[";
            for (long long j = 0; j < arrLen(v); ++j)
                { if (j) r += ", "; r += ts(arrGet(v, j)); }
            return r + "]";
        }
        case 7: {
            std::string r = "{"; bool first = true;
            if (v.mp) for (auto& kv : *v.mp)
                { if (!first) r += ", "; first = false; r += ts(kv.first) + ": " + ts(kv.second); }
            return r + "}";
        }
        case 8: return "None";
    }
    return "";
}
// Track 05 C3: primitives by value; structs field-wise recursive (a struct IS
// its fields, §9); classes by identity — mirrors keyEquals in RuntimeValue.hpp.
static bool keyEq(const V& a, const V& b) {
    if (a.k != b.k) return false;
    switch (a.k) {
        case 1: return a.i == b.i;
        case 10: return a.i == b.i;              // Track 03 §1: char by scalar

        case 4: return a.s == b.s;
        case 3: return a.b == b.b;
        case 2: return a.f == b.f;
        case 5:
            if (a.o && b.o && a.o->cls == b.o->cls && isValueClass(a.o->cls)) {
                if (a.o->slots.size() != b.o->slots.size()) return false;
                for (size_t i = 0; i < a.o->slots.size(); ++i)
                    if (!keyEq(a.o->slots[i], b.o->slots[i])) return false;
                return true;
            }
            return a.o == b.o;
        case 8: return true;
        default: return false;
    }
}
// Prelude globals (std::read, console, ...): initialized by @ginit at startup.
static std::vector<V> g_globals;
// Exceptions: interpreter model — a pending-throw flag checked after each call.
static bool g_throwing = false;
static V g_thrown;
static int RTE_ID = 0;    // RuntimeException class id (set at startup)
static V mkexc(int cls, const std::string& m) {
    V v = mkobj(cls); objset(v, "message", vstr(m)); return v;
}
static void raise(const std::string& m) { g_throwing = true; g_thrown = mkexc(RTE_ID, m); }
// op codes match TokenKind values baked in by the generator
static V rt_syswrite(const V& fd, const V& s) {
    if (fd.i == 2) fwrite(s.s.data(), 1, s.s.size(), stderr);
    else fwrite(s.s.data(), 1, s.s.size(), stdout);
    return vi((long long)s.s.size());
}
static V rt_bytetostring(const V& b) {
    if (b.i < 0 || b.i > 255) { raise("byte value " + std::to_string(b.i) + " out of range 0..255"); return V{}; }
    return vstr(std::string(1, (char)(unsigned char)b.i));
}
static V rt_sysnow() {                                                   // Track 08 F2 (C6)
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return vi((long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static V rt_charfromcode(const V& c) {                                   // Track 03 §1
    if (c.i < 0 || c.i > 0x10FFFF || (c.i >= 0xD800 && c.i <= 0xDFFF)) {
        raise("code point " + std::to_string(c.i) + " is not a valid Unicode scalar (0..0x10FFFF minus surrogates)");
        return V{};
    }
    return vch(c.i);
}
// argv, captured from the real process entry (main below). Bytes verbatim —
// strings are byte strings, so non-UTF-8 argv round-trips (designs/argv.md §9).
static std::vector<std::string> g_prog_args{""};   // length >= 1 (argv[0])
static V rt_sysargs() {
    V r = mkarr();
    for (const std::string& a : g_prog_args) r.arr->push_back(vstr(a));
    return r;
}
// Terminal raw mode (designs/terminal-raw-mode.md §3.2/§4). Single saved slot;
// rt_termshutdown is called from the generated main's epilogue so the terminal
// is restored on normal exit even if the program forgot (the emit-C++ analogue
// of lvrt_term_shutdown).
static struct termios g_tty_saved; static bool g_tty_raw = false; static int g_tty_fd = -1;
static int rt_termrestore() {
    if (!g_tty_raw) return 0;
    int rc = tcsetattr(g_tty_fd, TCSAFLUSH, &g_tty_saved) == 0 ? 0 : -1;
    g_tty_raw = false; return rc;
}
static void rt_termshutdown() { rt_termrestore(); }
// Terminal size + raw-state query (designs/techdesign-terminal-floor.md §2).
// rt_winsize returns rows|cols by field (0|1), -1 on failure or a 0x0 report.
static long long rt_winsize(long long fd, long long field) {
    struct winsize ws;
    if (ioctl((int)fd, TIOCGWINSZ, &ws) != 0 || ws.ws_row == 0 || ws.ws_col == 0) return -1;
    return field == 0 ? ws.ws_row : (field == 1 ? ws.ws_col : -1);
}
// Raw-mode safety handlers (designs/techdesign-terminal-floor.md §3): restore
// the terminal on an external SIGTERM/HUP/INT/QUIT then re-raise default, so a
// killed emit-C++ TUI never orphans the shell. Async-signal-safe (tcsetattr +
// raise); SA_RESETHAND fires once. Disjoint from any SEGV handling.
static void rt_term_safety(int sig) { rt_termrestore(); raise(sig); }
static void rt_term_install_safety() {
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_handler = rt_term_safety; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_RESETHAND;
    int sigs[4] = { SIGTERM, SIGHUP, SIGINT, SIGQUIT };
    for (int i = 0; i < 4; i++) sigaction(sigs[i], &sa, nullptr);
}
static int rt_termraw(long long fd) {
    if (g_tty_raw) return 0;
    if (tcgetattr((int)fd, &g_tty_saved) != 0) return -1;
    struct termios r = g_tty_saved;
    r.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    r.c_oflag &= ~(OPOST);
    r.c_cflag |= (CS8);
    r.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    r.c_cc[VMIN] = 1; r.c_cc[VTIME] = 0;
    if (tcsetattr((int)fd, TCSAFLUSH, &r) != 0) return -1;
    g_tty_raw = true; g_tty_fd = (int)fd; rt_term_install_safety(); return 0;
}
// Process exit (designs/exit-codes.md §3.1/§4). g_exit_code backs the generated
// main's return (was `return 0`); rt_syssetexitcode records it for normal
// completion, rt_sysexit terminates now via the shared restore-then-exit
// epilogue. Unix status is 8-bit, so writes are masked (§5) — identical to the
// runtime's g_exit_code so every engine agrees.
static int g_exit_code = 0;
static void rt_syssetexitcode(long long code) { g_exit_code = (int)(code & 0xFF); }
static void rt_sysexit(long long code) {
    rt_termshutdown();                  // restore cooked mode if raw (§3.4)
    exit((int)(code & 0xFF));            // never returns
}
static V rt_readline() {
    V v; v.k = 4;
    int c;
    bool any = false;
    while ((c = fgetc(stdin)) != EOF && c != '\n') { v.s.push_back((char)c); any = true; }
    (void)any;
    return v;
}
// sysRead(fd, max) -> string: up to `max` raw bytes (empty at EOF/would-block).
// The string-carried floor read (designs/techdesign-terminal-floor.md §2 pulls
// it in via term::size()'s cursor-report fallback).
static V rt_read(long long fd, long long max) {
    V v; v.k = 4;
    if (max <= 0) return v;
    std::string buf;
    buf.resize((size_t)max);
    long long n = read((int)fd, &buf[0], (size_t)max);
    if (n > 0) v.s.assign(buf.data(), (size_t)n);
    return v;
}
static V ar(int op, const V& l, const V& r) {
    if (l.k == 8 || r.k == 8) {                                          // None
        if (op == 1) return vb(l.k == r.k);
        if (op == 2) return vb(l.k != r.k);
        return V{};
    }
    if (l.k == 3 && r.k == 3) {                                          // bool
        if (op == 1) return vb(l.b == r.b);                              // ==
        if (op == 2) return vb(l.b != r.b);                              // !=
        return V{};
    }
    if (l.k == 11 && r.k == 11) {                                        // Block identity
        if (op == 1) return vb(l.bl == r.bl);                             // ==
        if (op == 2) return vb(l.bl != r.bl);                             // !=
        return V{};
    }
    if (l.k == 4 || r.k == 4) {
        if (op == 0) { V v; v.k = 4; v.s = ts(l) + ts(r); return v; }   // +
        if (op == 1) return vb(l.s == r.s);                              // ==
        if (op == 2) return vb(l.s != r.s);                              // !=
        if (op == 7) return vb(l.s <  r.s);                              // <  (lexicographic)
        if (op == 8) return vb(l.s >  r.s);                              // >
        if (op == 9) return vb(l.s <= r.s);                              // <=
        if (op == 10) return vb(l.s >= r.s);                             // >=
    }
    if ((l.k == 2 && (r.k == 2 || r.k == 1)) || (r.k == 2 && l.k == 1)) {   // float (int promotes)
        double a = l.k == 2 ? l.f : (double)l.i;
        double b = r.k == 2 ? r.f : (double)r.i;
        switch (op) {
            case 0:  return vf(a + b);
            case 3:  return vf(a - b);
            case 4:  return vf(a * b);
            case 5:  return vf(a / b);
            case 1:  return vb(a == b);
            case 2:  return vb(a != b);
            case 7:  return vb(a < b);
            case 8:  return vb(a > b);
            case 9:  return vb(a <= b);
            case 10: return vb(a >= b);
        }
        return V{};                                                      // % / bitwise: no float form
    }
    long long a = l.i, b = r.i;
    switch (op) {
        case 0:  return vi(a + b);
        case 1:  return vb(a == b);
        case 2:  return vb(a != b);
        case 3:  return vi(a - b);
        case 4:  return vi(a * b);
        // DIV/MOD by zero raise a catchable RuntimeException (§3.7, bug.md #10),
        // same shape as the shift range check below; the emitted throwCheck after
        // each arith() call unwinds. Floats keep IEEE inf/nan (bug.md #12).
        case 5:  if (!b) { raise("division by zero"); return V{}; } return vi(a / b);
        case 6:  if (!b) { raise("modulo by zero");   return V{}; } return vi(a % b);
        case 7:  return vb(a < b);
        case 8:  return vb(a > b);
        case 9:  return vb(a <= b);
        case 10: return vb(a >= b);
        case 11: return vi(a & b);
        case 12: return vi(a | b);
        case 13:                                       // << (shl)
            if (b < 0 || b > 63) { raise("shift count out of range"); return V{}; }
            return vi(a << b);
        case 14:                                       // >> (sar; arithmetic on signed a)
            if (b < 0 || b > 63) { raise("shift count out of range"); return V{}; }
            return vi(a >> b);
        case 15: return vi(a ^ b);                      // xor
    }
    return V{};
}
#include <cctype>
// Strict full-string parses backing toInt/toFloat (mirror RuntimeNatives.cpp).
static bool strictParseInt(const std::string& s, long long& out) {
    if (s.empty()) return false;
    size_t i = s[0] == '-' ? 1 : 0;
    if (i == s.size()) return false;
    for (size_t j = i; j < s.size(); ++j) if (!isdigit((unsigned char)s[j])) return false;
    errno = 0;
    char* end = nullptr;
    long long v = strtoll(s.c_str(), &end, 10);
    if (errno == ERANGE || end != s.c_str() + s.size()) return false;
    out = v; return true;
}
static bool strictParseFloat(const std::string& s, double& out) {
    if (s.empty() || isspace((unsigned char)s.front()) || isspace((unsigned char)s.back())) return false;
    errno = 0;
    char* end = nullptr;
    double v = strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || !std::isfinite(v)) return false;
    out = v; return true;
}
// Native method cores (mirror RuntimeNatives::nativeCall for the value model).
static V callnative(const V& self, const std::string& m, std::vector<V>& args) {
    if (self.k == 4) {                                                   // string
        const std::string& s = self.s;
        auto AS = [&](size_t i){ return i < args.size() ? args[i].s : std::string(); };
        auto AI = [&](size_t i){ return i < args.size() ? args[i].i : 0; };
        if (m == "length")   return vi((long long)s.size());
        if (m == "isEmpty")  return vb(s.empty());
        if (m == "toUpper")  { std::string r=s; for(char&c:r)c=(char)toupper((unsigned char)c); return vstr(r); }
        if (m == "toLower")  { std::string r=s; for(char&c:r)c=(char)tolower((unsigned char)c); return vstr(r); }
        if (m == "charAt")   { long long i=AI(0); return vstr(i>=0&&i<(long long)s.size()?std::string(1,s[(size_t)i]):""); }
        if (m == "subStr")   { long long a=AI(0),n=AI(1); return vstr(a>=0&&a<=(long long)s.size()?s.substr((size_t)a,(size_t)n):""); }
        if (m == "contains") return vb(s.find(AS(0)) != std::string::npos);
        if (m == "indexOf")  { auto p=s.find(AS(0)); return vi(p==std::string::npos?-1:(long long)p); }
        if (m == "startsWith"){ std::string p=AS(0); return vb(s.rfind(p,0)==0); }
        if (m == "endsWith") { std::string p=AS(0); return vb(p.size()<=s.size()&&s.compare(s.size()-p.size(),p.size(),p)==0); }
        if (m == "trim")     { size_t a=s.find_first_not_of(" \t\n\r"),b=s.find_last_not_of(" \t\n\r"); return vstr(a==std::string::npos?"":s.substr(a,b-a+1)); }
        if (m == "toInt")    { long long v; return strictParseInt(s, v) ? vi(v) : vnone(); }
        if (m == "toFloat")  { double v; return strictParseFloat(s, v) ? vf(v) : vnone(); }
        if (m == "byteAt")   {
            long long i = AI(0);
            if (i < 0 || i >= (long long)s.size()) {
                raise("index " + std::to_string(i) + " out of bounds (length " + std::to_string(s.size()) + ")");   // matches Array.at
                return V{};
            }
            return vi((long long)(unsigned char)s[(size_t)i]);
        }
        if (m == "at") {                                                // Track 03 §1 (C1)
            long long i = AI(0);
            if (i < 0 || i >= (long long)s.size()) {
                raise("index " + std::to_string(i) + " out of bounds (length " + std::to_string(s.size()) + ")");
                return V{};
            }
            size_t len; bool boundary; long long cp = u8dec(s, (size_t)i, len, boundary);
            if (!boundary) { raise("byte offset " + std::to_string(i) + " is not a scalar boundary"); return V{}; }
            return vch(cp);
        }
        if (m == "chars") {                                             // full UTF-8 decode
            V r = mkarr(); size_t i = 0;
            while (i < s.size()) { size_t len; bool boundary; long long cp = u8dec(s, i, len, boundary); r.arr->push_back(vch(cp)); i += len; }
            return r;
        }
        if (m == "toString") return self;
    } else if (self.k == 10) {                                          // Track 03 §1: char
        if (m == "code")     return vi(self.i);
        if (m == "toString") return vstr(u8enc(self.i));
    } else if (self.k == 11) {                                          // Track 03 §3: Block
        if (!self.bl || !self.bl->bytes) { raise("null block"); return V{}; }
        BlockData& bd = *self.bl; std::vector<unsigned char>& by = *bd.bytes;
        auto AI = [&](size_t i){ return i < args.size() ? args[i].i : 0; };
        auto oob = [&](long long i)->V { raise("index " + std::to_string(i) + " out of bounds (length " + std::to_string(bd.len) + ")"); return V{}; };
        if (m == "length") return vi((long long)bd.len);
        if (m == "byteAt") { long long i=AI(0); if(i<0||i>=(long long)bd.len) return oob(i); return vi((long long)by[bd.off+(size_t)i]); }
        if (m == "setByte") { long long i=AI(0),v=AI(1); if(i<0||i>=(long long)bd.len) return oob(i);
            if(v<0||v>255){ raise("byte value "+std::to_string(v)+" out of range 0..255"); return V{}; }
            by[bd.off+(size_t)i]=(unsigned char)v; return V{}; }
        if (m == "slice") { long long o=AI(0),l=AI(1); if(o<0||l<0||o+l>(long long)bd.len) return oob(o);
            auto nb=std::make_shared<BlockData>(); nb->bytes=bd.bytes; nb->off=bd.off+(size_t)o; nb->len=(size_t)l; return vblk(nb); }
        if (m == "toString") { long long o=AI(0),l=AI(1); if(o<0||l<0||o+l>(long long)bd.len) return oob(o);
            return vstr(std::string(by.begin()+bd.off+o, by.begin()+bd.off+o+l)); }
        if (m == "int32At") { long long i=AI(0); if(i<0||i+4>(long long)bd.len) return oob(i);
            uint32_t u=0; for(int k=0;k<4;++k) u|=(uint32_t)by[bd.off+i+k]<<(8*k); return vi((long long)(int32_t)u); }
        if (m == "setInt32") { long long i=AI(0),v=AI(1); if(i<0||i+4>(long long)bd.len) return oob(i);
            uint32_t u=(uint32_t)v; for(int k=0;k<4;++k) by[bd.off+i+k]=(unsigned char)(u>>(8*k)); return V{}; }
        if (m == "int64At") { long long i=AI(0); if(i<0||i+8>(long long)bd.len) return oob(i);
            uint64_t u=0; for(int k=0;k<8;++k) u|=(uint64_t)by[bd.off+i+k]<<(8*k); return vi((long long)u); }
        if (m == "setInt64") { long long i=AI(0),v=AI(1); if(i<0||i+8>(long long)bd.len) return oob(i);
            uint64_t u=(uint64_t)v; for(int k=0;k<8;++k) by[bd.off+i+k]=(unsigned char)(u>>(8*k)); return V{}; }
        if (m == "fill") { long long o=AI(0),l=AI(1),v=AI(2); if(o<0||l<0||o>(long long)bd.len||l>(long long)bd.len-o) return oob(o);
            if(v<0||v>255){ raise("byte value "+std::to_string(v)+" out of range 0..255"); return V{}; }
            if(l) std::memset(by.data()+bd.off+(size_t)o,(int)v,(size_t)l); return V{}; }
        if (m == "blit") { long long d=AI(0),s=AI(2),l=AI(3); if(d<0||l<0||d>(long long)bd.len||l>(long long)bd.len-d) return oob(d);
            if(args.size()<2||!args[1].bl||!args[1].bl->bytes){ raise("null block"); return V{}; }
            BlockData& sb=*args[1].bl; if(s<0||s>(long long)sb.len||l>(long long)sb.len-s){ raise("index "+std::to_string(s)+" out of bounds (length "+std::to_string(sb.len)+")"); return V{}; }
            if(l) std::memmove(by.data()+bd.off+(size_t)d,sb.bytes->data()+sb.off+(size_t)s,(size_t)l); return V{}; }
        if (m == "equals") { if(args.empty()||!args[0].bl||!args[0].bl->bytes){ raise("null block"); return V{}; }
            BlockData& ob=*args[0].bl; return vb(bd.len==ob.len&&(bd.len==0||std::memcmp(by.data()+bd.off,ob.bytes->data()+ob.off,bd.len)==0)); }
        if (m == "mismatch") { if(args.empty()||!args[0].bl||!args[0].bl->bytes){ raise("null block"); return V{}; }
            BlockData& ob=*args[0].bl; if(bd.len!=ob.len){ raise("Block.mismatch requires equal lengths"); return V{}; }
            long long f=AI(1); if(f<0||f>(long long)bd.len) return oob(f); size_t i=(size_t)f;
            while(i+64<=bd.len&&std::memcmp(by.data()+bd.off+i,ob.bytes->data()+ob.off+i,64)==0)i+=64;
            while(i<bd.len&&by[bd.off+i]==(*ob.bytes)[ob.off+i])++i; return vi(i==bd.len?-1:(long long)i); }
    } else if (self.k == 6) {                                           // Array
        long long n = arrLen(self);
        if (m == "length") return vi(n);
        if (m == "at")     { long long i=args.empty()?0:args[0].i;
            if (i<0||i>=n) { raise("index "+std::to_string(i)+" out of bounds (length "+std::to_string(n)+")"); return V{}; }
            return arrGet(self, i); }
        if (m == "add")    {
            if (self.arr && self.arr.use_count() == 1 && !args.empty()) {   // uniquely owned: COW in place
                V s = self; arrPush(s, args[0]); return s;
            }
            V r=arrCow(self); if(!args.empty()) arrPush(r, args[0]); return r; }
        if (m == "concatAll") {                             // O(total): sum, reserve, append
            size_t total = 0;
            if (self.arr) for (const V& v : *self.arr) total += v.s.size();
            std::string r; r.reserve(total);
            if (self.arr) for (const V& v : *self.arr) r += v.s;
            return vstr(std::move(r));
        }
    } else if (self.k == 7) {                                           // Map
        auto& e = *self.mp;
        if (m == "length") return vi((long long)e.size());
        if (m == "at")     { for(auto&kv:e) if(!args.empty()&&keyEq(kv.first,args[0])) return kv.second;
            raise("key not found: "+(args.empty()?std::string("<none>"):ts(args[0]))); return V{}; }
        if (m == "has")    { for(auto&kv:e) if(!args.empty()&&keyEq(kv.first,args[0])) return vb(true); return vb(false); }
        if (m == "with")   { V r=mkmap(); *r.mp=e; bool rep=false; if(args.size()==2){for(auto&kv:*r.mp)if(keyEq(kv.first,args[0])){kv.second=args[1];rep=true;break;} if(!rep)r.mp->push_back({args[0],args[1]});} return r; }
        if (m == "without"){ V r=mkmap(); for(auto&kv:e) if(args.empty()||!keyEq(kv.first,args[0])) r.mp->push_back(kv); return r; }
        if (m == "keys")   { V r=mkarr(); for(auto&kv:e) r.arr->push_back(kv.first); return r; }
        if (m == "values") { V r=mkarr(); for(auto&kv:e) r.arr->push_back(kv.second); return r; }
    } else if (self.k == 1) {
        if (m == "toString") return vstr(std::to_string(self.i));
        if (m == "toFloat")  return vf((double)self.i);
    } else if (self.k == 3) { if (m == "toString") return vstr(self.b?"true":"false"); }
    else if (self.k == 2) {
        if (m == "toString") return vstr(std::to_string(self.f));
        if (m == "floor") return vf(std::floor(self.f));
        if (m == "ceil")  return vf(std::ceil(self.f));
        if (m == "round") return vf(std::round(self.f));   // half-away-from-zero
        if (m == "trunc") return vf(std::trunc(self.f));
        if (m == "sqrt")  return vf(std::sqrt(self.f));    // negative -> NaN (IEEE)
        if (m == "pow")   return vf(std::pow(self.f, args.empty() ? 0.0 : args[0].f));
        if (m == "toInt") {
            if (!std::isfinite(self.f) || self.f < -9223372036854775808.0 ||
                self.f >= 9223372036854775808.0) {
                raise("float is not finite or out of int64 range for toInt()");
                return V{};
            }
            return vi((long long)self.f);
        }
    }
    return V{};
}
static long long iterlen(const V& it) {
    if (it.k == 6) return arrLen(it);
    if (it.k == 7) return it.mp ? (long long)it.mp->size() : 0;
    if (it.k == 5 && it.o) { long long lo=objget(it,"start").i, hi=objget(it,"end").i; return hi>=lo?hi-lo+1:0; }
    return 0;
}
)rt";

static int opCode(TokenKind k) {
    switch (k) {
        case TokenKind::Plus:    return 0;
        case TokenKind::EqEq:    return 1;
        case TokenKind::BangEq:  return 2;
        case TokenKind::Minus:   return 3;
        case TokenKind::Star:    return 4;
        case TokenKind::Slash:   return 5;
        case TokenKind::Percent: return 6;
        case TokenKind::Lt:      return 7;
        case TokenKind::Gt:      return 8;
        case TokenKind::Le:      return 9;
        case TokenKind::Ge:      return 10;
        case TokenKind::Amp:     return 11;    // int bitwise (OpenMode flags)
        case TokenKind::Pipe:    return 12;
        // LtLt/GtGt double as the int-shift ops (Track 01 F1) and, on an
        // object left operand, the (<<)/(>>) transfer-operator selector —
        // `arith()` picks scalar-`ar` vs object-`opm` by the LEFT operand's
        // tag, same resolution-by-type rule as every other operator.
        case TokenKind::LtLt:    return 13;
        case TokenKind::GtGt:    return 14;
        case TokenKind::Caret:   return 15;
        default:                 return -1;
    }
}

std::string CGen::escapeString(const std::string& s) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                // Any other non-printable byte (control chars, NUL, high-bit
                // bytes — strings are byte-clean through the floor, so these
                // ARE reachable, e.g. Track 01 F3's `\0`/`\xNN` literals): a
                // raw byte embedded straight into the generated C++ source
                // breaks the enclosing string literal (confirmed: `\0` and
                // `\r` both produced g++'s "missing terminating \" character"
                // before this). Emit `\xHH`, then immediately close/reopen
                // the literal — C++ adjacent string literals concatenate, and
                // `\x` escapes are otherwise GREEDY (they'd swallow a
                // following hex-digit character into the SAME escape, so
                // `\x00` + 'a' must not become the single code point
                // `\x00a`).
                if (c < 0x20 || c >= 0x7F) {
                    out += "\\x";
                    out.push_back(hex[(c >> 4) & 0xF]);
                    out.push_back(hex[c & 0xF]);
                    out += "\"\"";
                } else {
                    out.push_back((char)c);
                }
                break;
        }
    }
    return out;
}

std::string CGen::genFunction(int index) {
    const IrFunction& fn = mod_.functions[index];
    std::string out;
    out += "static V f" + std::to_string(index) + "(";
    for (int p = 0; p < fn.nparams; ++p)
        out += (p ? ", V p" : "V p") + std::to_string(p);
    out += ") {\n";
    out += "    V r[" + std::to_string(fn.nregs > 0 ? fn.nregs : 1) + "];\n";
    for (int p = 0; p < fn.nparams; ++p)
        out += "    r[" + std::to_string(p) + "] = p" + std::to_string(p) + ";\n";

    // which pcs need labels (jump targets, and every handler entry)
    std::vector<bool> target(fn.code.size() + 1, false);
    for (const Inst& in : fn.code) {
        if (in.op == Op::Jump) target[in.a] = true;
        if (in.op == Op::JumpIfFalse || in.op == Op::JumpIfTrue) target[in.b] = true;
    }
    for (const Handler& h : fn.handlers) target[h.handlerPc] = true;

    // A pending throw at `pc` dispatches to the first covering handler whose
    // clause type matches (issub), binding the value; else it unwinds.
    bool usesUnwind = false;
    auto throwCheck = [&](int pc) -> std::string {
        std::string s = "    if (g_throwing) {\n";
        for (const Handler& h : fn.handlers) {
            if (pc < h.start || pc >= h.end) continue;
            int tid = h.type ? clsId(h.type) : 0;
            s += "      if (g_thrown.k == 5 && issub(g_thrown.o->cls, " +
                 std::to_string(tid) + ")) { g_throwing = false; r[" +
                 std::to_string(h.bindReg) + "] = g_thrown; goto L" +
                 std::to_string(h.handlerPc) + "; }\n";
        }
        s += "      goto UNWIND;\n    }\n";
        usesUnwind = true;
        return s;
    };

    for (size_t pc = 0; pc < fn.code.size(); ++pc) {
        const Inst& in = fn.code[pc];
        if (target[pc]) out += "L" + std::to_string(pc) + ":\n";
        auto R = [](int r) { return "r[" + std::to_string(r) + "]"; };
        switch (in.op) {
            case Op::LoadConst: {
                const Value& c = fn.consts[in.b];
                std::string lit;
                switch (c.kind) {
                    case VKind::Int:
                        // INT64_MIN has no positive counterpart to negate, so its
                        // literal spelling (-9223372036854775808LL) overflows a
                        // signed long long during parsing and g++ warns "so large
                        // that it is unsigned" — spell it the way <climits> does,
                        // reachable now via `1 << 63` (Track 01 F1) or an
                        // 0x8000000000000000 literal (F2).
                        lit = c.i == INT64_MIN ? "vi(-9223372036854775807LL-1)"
                                               : "vi(" + std::to_string(c.i) + "LL)";
                        break;
                    case VKind::Float:  lit = "vf(" + std::to_string(c.f) + ")"; break;
                    case VKind::Char:   lit = "vch(" + std::to_string(c.i) + "LL)"; break;
                    case VKind::Bool:   lit = c.b ? "vb(true)" : "vb(false)"; break;
                    case VKind::String:
                        // `vs(const char*)` builds its std::string via strlen,
                        // which truncates at the FIRST embedded NUL — silently
                        // wrong for a literal containing `\0` (Track 01 F3).
                        // The 2-arg std::string(ptr, len) ctor keeps every
                        // byte; the length is already known at codegen time.
                        lit = "vstr(std::string(\"" + escapeString(c.s) + "\", " +
                              std::to_string(c.s.size()) + "))";
                        break;
                    case VKind::None:   lit = "vnone()"; break;
                    default:            lit = "V{}"; break;
                }
                out += "    " + R(in.a) + " = " + lit + ";\n";
                break;
            }
            case Op::Default: {
                std::string lit;
                const std::string& c = in.sname;
                if (c == "int") lit = "vi(0)";
                else if (c == "bool") lit = "vb(false)";
                else if (c == "float") lit = "vf(0)";
                else if (c == "string") lit = "vs(\"\")";
                else if (c == "None") lit = "vnone()";
                else if (c.rfind("Array", 0) == 0) lit = "mkarr()";
                else if (c.rfind("Map", 0) == 0) lit = "mkmap()";
                else if (c.find(" | ") != std::string::npos &&
                         c.find("None") != std::string::npos) lit = "vnone()";
                else lit = "V{}";      // unmodeled default: void (matches interpreter)
                out += "    " + R(in.a) + " = " + lit + ";\n";
                break;
            }
            case Op::Move:
                out += "    " + R(in.a) + " = " + R(in.b) + ";\n";
                break;
            case Op::MoveClear:   // move the source in (empties it) so a COW op sees it uniquely
                out += "    " + R(in.a) + " = std::move(" + R(in.b) + ");\n";
                break;
            case Op::CopyVal:
                out += "    " + R(in.a) + " = copyval(" + R(in.b) + ");\n";
                break;
            case Op::Arith: {
                // LtLt/GtGt double as int-shift (Track 01 F1) and the (<<)/(>>)
                // object transfer-operator selector — `in.decl` (set only when
                // the checker resolved an operator-METHOD overload) tells them
                // apart. bug.md #14: object transfer-operator dispatch now gets
                // real codegen (falls through to arith()/opm() like every other
                // operator-method below), EXCEPT for OutStream<T> specifically
                // (and, through it, IOStream<T>, which inherits OutStream's own
                // (<<) unchanged) — StreamBuffer<T>'s underlying push/pull
                // queue plumbing has its own separate, pre-existing emit-cpp
                // bug (confirmed while fixing #14: `.pull()` itself, not just
                // the `stream >>` sugar, silently returns void here) that a
                // past attempt to enable this exact dispatch ran straight into
                // (subscribe's drain spins — the failure this guard used to
                // cite). Leaving OutStream<T> on the old hard error preserves
                // today's clean "unsupported" skip instead of trading it for a
                // hang or a silent wrong value; see bug.md for the separate
                // StreamBuffer-on-emit-cpp entry this uncovered.
                bool blockedObjectXfer = false;
                if ((in.tk == TokenKind::LtLt || in.tk == TokenKind::GtGt) && in.decl &&
                    mod_.byDecl.count(in.decl)) {
                    const std::string& fn = mod_.functions[mod_.byDecl.at(in.decl)].name;
                    blockedObjectXfer = fn.rfind("OutStream.", 0) == 0;
                }
                int oc = blockedObjectXfer ? -1 : opCode(in.tk);
                if (oc < 0) { sink_.error({}, "native backend: operator"); ok_ = false; break; }
                out += "    " + R(in.a) + " = arith(" + std::to_string(oc) + ", " + R(in.b) +
                       ", " + R(in.c) + ");\n";
                out += throwCheck((int)pc);
                break;
            }
            case Op::NewObject: {
                int cls = in.sym ? clsId(in.sym) : 0;
                out += "    " + R(in.a) + " = mkobj(" + std::to_string(cls) + ");\n";
                if (in.b >= 0) out += "    f" + std::to_string(in.b) + "(" + R(in.a) + ");\n";
                break;
            }
            case Op::IsType: {
                // `is` / `match` type test: OR the per-member checks (kind for
                // primitives/collections, issub for classes) — mirrors the IR.
                static const std::unordered_map<std::string, int> kindOf = {
                    {"int",1},{"float",2},{"bool",3},{"string",4},
                    {"Array",6},{"Map",7},{"None",8}};
                std::string terms;
                auto addTerm = [&](const std::string& m, Symbol* fallback) {
                    if (!terms.empty()) terms += " || ";
                    std::string base = m.substr(0, m.find('<'));   // Array<T> -> Array
                    auto k = kindOf.find(base);
                    if (k != kindOf.end()) {
                        terms += R(in.b) + ".k == " + std::to_string(k->second);
                        return;
                    }
                    // A class member: resolve by its bare name; a NAMESPACED type
                    // (e.g. `Sonar::Component`) is not found by a global bare-name
                    // lookup, so fall back to the op's own resolved symbol — the
                    // same in.sym fallback LlvmGen uses (bug.md #36). Without it the
                    // test collapsed to `false` and `x is NS::C` never narrowed.
                    Symbol* s = mod_.sema->global->lookup(base);
                    if (!s) s = fallback;
                    if (s)
                        terms += "(" + R(in.b) + ".k == 5 && issub(" + R(in.b) +
                                 ".o->cls, " + std::to_string(clsId(s)) + "))";
                    else terms += "false";
                };
                if (in.sname.find(" | ") != std::string::npos) {
                    size_t p = 0, q;
                    std::string s = in.sname;
                    while ((q = s.find(" | ", p)) != std::string::npos) {
                        addTerm(s.substr(p, q - p), nullptr); p = q + 3;
                    }
                    addTerm(s.substr(p), nullptr);
                } else {
                    addTerm(in.sname, in.sym);   // single class member: in.sym is exact
                }
                out += "    " + R(in.a) + " = vb(" + (terms.empty() ? "false" : terms) + ");\n";
                break;
            }
            case Op::GetMember:
                out += "    " + R(in.a) + " = getm(" + R(in.b) + ", \"" +
                       escapeString(in.sname) + "\");\n";
                break;
            case Op::SetMember:
                out += "    setm(" + R(in.b) + ", \"" + escapeString(in.sname) + "\", " +
                       R(in.a) + ");\n";
                break;
            case Op::RawGet:
                if (in.d > 0)                              // §7: compile-time packed slot
                    out += "    " + R(in.a) + " = " + R(in.b) + ".o->slots[" +
                           std::to_string(in.d - 1) + "];\n";
                else
                    out += "    " + R(in.a) + " = objget(" + R(in.b) + ", \"" +
                           escapeString(in.sname) + "\");\n";
                break;
            case Op::RawGetWeak:
                out += "    " + R(in.a) + " = weakget(" + R(in.b) + ", \"" +
                       escapeString(in.sname) + "\");\n";
                break;
            case Op::RawSet:
                if (in.d > 0)
                    out += "    " + R(in.b) + ".o->slots[" + std::to_string(in.d - 1) +
                           "] = " + R(in.a) + ";\n";
                else
                    out += "    objset(" + R(in.b) + ", \"" + escapeString(in.sname) + "\", " +
                           R(in.a) + ");\n";
                break;
            case Op::RawSetWeak:
                out += "    weakset(" + R(in.b) + ", \"" + escapeString(in.sname) + "\", " +
                       R(in.a) + ");\n";
                break;
            case Op::CallDyn: {
                if (in.decl && mod_.byDecl.count(in.decl)) {       // in-language method
                    out += "    " + R(in.a) + " = f" +
                           std::to_string(mod_.byDecl.at(in.decl)) + "(";
                    for (int k = 0; k < in.d; ++k) out += (k ? ", " : "") + R(in.c + k);
                    out += ");\n";
                } else {                                           // dynamic dispatch by name
                    out += "    { std::vector<V> _a = {";
                    for (int k = 1; k < in.d; ++k) out += (k > 1 ? ", " : "") + R(in.c + k);
                    out += "}; " + R(in.a) + " = callm(" + R(in.c) + ", \"" +
                           escapeString(in.sname) + "\", _a); }\n";
                }
                out += throwCheck((int)pc);
                break;
            }
            case Op::NewArray: {
                out += "    { V _a = mkarr();\n";
                for (int k = 0; k < in.d; ++k) {
                    std::string e = R(in.c + k);
                    out += "      if (" + e + ".k == 5 && " + e + ".o && " + e +
                           ".o->cls == " + std::to_string(rangeId()) + ") { for (long long _x = objget(" +
                           e + ", \"start\").i; _x <= objget(" + e +
                           ", \"end\").i; ++_x) arrPush(_a, vi(_x)); }\n";
                    out += "      else arrPush(_a, " + e + ");\n";
                }
                out += "      " + R(in.a) + " = _a; }\n";
                break;
            }
            case Op::NewArraySized: {
                out += "    { V _a = mkarr();";
                if (in.d == 2)
                    out += " for (long long _k = 0; _k < " + R(in.c) + ".i; ++_k) arrPush(_a, " +
                           R(in.c + 1) + ");";
                out += " " + R(in.a) + " = _a; }\n";
                break;
            }
            case Op::NewMap:
                out += "    " + R(in.a) + " = mkmap();\n";
                break;
            case Op::NewBlock:                                 // Track 03 §3
                out += "    " + R(in.a) + " = vblknew(" + R(in.c) + ".i);\n";
                break;
            case Op::NewBlockStr:
                out += "    " + R(in.a) + " = vblkstr(" + R(in.c) + ".s);\n";
                break;
            case Op::MakeRange:
                out += "    { V _r = mkobj(" + std::to_string(rangeId()) + "); objset(_r, \"start\", " +
                       R(in.b) + "); objset(_r, \"end\", " + R(in.c) + "); " + R(in.a) + " = _r; }\n";
                break;
            case Op::GetIndex:
                out += "    " + R(in.a) + " = idxget(" + R(in.b) + ", " + R(in.c) + ");\n";
                out += throwCheck((int)pc);
                break;
            case Op::ColGet:
                // techdesign-columnar §7: emit-C++'s dense buffer is self-contained
                // and layout-unobservable (the oracle is the columnar layout's
                // reference), so the fused read is simply element-gather + field
                // read here — correct on the differential, no separate column math.
                out += "    " + R(in.a) + " = getm(idxget(" + R(in.b) + ", " + R(in.c) +
                       "), \"" + escapeString(in.sname) + "\");\n";
                out += throwCheck((int)pc);
                break;
            case Op::IndexStore:
                // §11/§15 COW: drop the dest temp's stale ref from the previous
                // execution, and the rebind chain's (`CopyVal t <- nb; Move L <- t`)
                // stale t, so a uniquely-owned base reads use_count 1 inside idxset
                // and mutates in place. Registers are never reused, so both are
                // written-once temps rewritten before any read on every path.
                out += "    " + R(in.a) + " = V{};\n";
                if (pc + 2 < fn.code.size()) {
                    const Inst& n1 = fn.code[pc + 1];
                    const Inst& n2 = fn.code[pc + 2];
                    if (n1.op == Op::CopyVal && n1.b == in.a &&
                        n2.op == Op::Move && n2.b == n1.a &&
                        n1.a != in.b && n1.a != in.c && n1.a != in.d)
                        out += "    " + R(n1.a) + " = V{};\n";
                }
                out += "    " + R(in.a) + " = idxset(" + R(in.b) + ", " + R(in.c) + ", " +
                       R(in.d) + ");\n";
                break;
            case Op::IterLen:
                out += "    " + R(in.a) + " = vi(iterlen(" + R(in.b) + "));\n";
                break;
            case Op::IterAt:
                out += "    " + R(in.a) + " = iterat(" + R(in.b) + ", " + R(in.c) + ".i);\n";
                break;
            case Op::MakeClosure:
                out += "    " + R(in.a) + " = mkclo(" + std::to_string(in.b) + ");\n";
                break;
            case Op::CaptureVar:
                out += "    objset(" + R(in.a) + ", \"" + escapeString(in.sname) + "\", " +
                       R(in.b) + ");\n";
                break;
            case Op::LoadCapture:
                out += "    " + R(in.a) + " = objget(r[0], \"" + escapeString(in.sname) +
                       "\");\n";
                break;
            case Op::CallValue: {
                out += "    { std::vector<V> _a = {";
                for (int k = 1; k < in.d; ++k) out += (k > 1 ? ", " : "") + R(in.c + k);
                out += "}; " + R(in.a) + " = callclosure(" + R(in.c) + ", _a); }\n";
                out += throwCheck((int)pc);
                break;
            }
            case Op::Throw:
                out += "    g_throwing = true; g_thrown = " + R(in.a) + ";\n";
                out += throwCheck((int)pc);
                break;
            case Op::Not:
                out += "    " + R(in.a) + " = vb(!" + R(in.b) + ".b);\n";
                break;
            case Op::Neg:
                out += "    " + R(in.a) + " = " + R(in.b) + ".k == 2 ? vf(-" + R(in.b) +
                       ".f) : vi(-" + R(in.b) + ".i);\n";
                break;
            case Op::Jump:
                out += "    goto L" + std::to_string(in.a) + ";\n";
                break;
            case Op::JumpIfFalse:
                out += "    if (!" + R(in.a) + ".b) goto L" + std::to_string(in.b) + ";\n";
                break;
            case Op::JumpIfTrue:
                out += "    if (" + R(in.a) + ".b) goto L" + std::to_string(in.b) + ";\n";
                break;
            case Op::Call: {
                out += "    " + R(in.a) + " = f" + std::to_string(in.b) + "(";
                for (int k = 0; k < in.d; ++k)
                    out += (k ? ", " : "") + R(in.c + k);
                out += ");\n";
                out += throwCheck((int)pc);
                break;
            }
            case Op::Ret:
                out += "    return " + R(in.a) + ";\n";
                break;
            case Op::RetVoid:
                out += "    return V{};\n";
                break;
            case Op::CallNativeFn:
                if (in.sname == "sysWrite" && in.d == 2)
                    out += "    " + R(in.a) + " = rt_syswrite(" + R(in.c) + ", " +
                           R(in.c + 1) + ");\n";
                else if (in.sname == "sysReadLine")
                    out += "    " + R(in.a) + " = rt_readline();\n";
                else if (in.sname == "sysRead" && in.d == 2)
                    out += "    " + R(in.a) + " = rt_read(" + R(in.c) + ".i, " +
                           R(in.c + 1) + ".i);\n";
                else if (in.sname == "byteToString" && in.d == 1)
                    out += "    " + R(in.a) + " = rt_bytetostring(" + R(in.c) + ");\n";
                else if (in.sname == "charFromCode" && in.d == 1)
                    out += "    " + R(in.a) + " = rt_charfromcode(" + R(in.c) + ");\n";
                else if (in.sname == "sysArgs" && in.d == 0)
                    out += "    " + R(in.a) + " = rt_sysargs();\n";
                else if (in.sname == "sysThreadTransfer" && in.d == 1)
                    out += "    " + R(in.a) + " = threadtransfer(" + R(in.c) + ");\n";
                else if (in.sname == "sysNow" && in.d == 0)
                    out += "    " + R(in.a) + " = rt_sysnow();\n";
                else if (in.sname == "sysTermRaw" && in.d == 1)
                    out += "    " + R(in.a) + " = vi(rt_termraw(" + R(in.c) + ".i));\n";
                else if (in.sname == "sysTermRestore" && in.d == 1)
                    out += "    " + R(in.a) + " = vi(rt_termrestore());\n";
                else if (in.sname == "sysWinSize" && in.d == 2)
                    out += "    " + R(in.a) + " = vi(rt_winsize(" + R(in.c) + ".i, " +
                           R(in.c + 1) + ".i));\n";
                else if (in.sname == "sysTermIsRaw" && in.d == 1)
                    out += "    " + R(in.a) + " = vb(g_tty_raw);\n";
                else if (in.sname == "sysSetExitCode" && in.d == 1)
                    out += "    rt_syssetexitcode(" + R(in.c) + ".i); " + R(in.a) + " = vi(0);\n";
                else if (in.sname == "sysExit" && in.d == 1)
                    out += "    rt_sysexit(" + R(in.c) + ".i); " + R(in.a) + " = vi(0);\n";
                // Track 06 std::math transcendentals: libm, straight through
                // (the kRuntime preamble already #includes <cmath>).
                else if (in.sname == "log" && in.d == 1)
                    out += "    " + R(in.a) + " = vf(std::log(" + R(in.c) + ".f));\n";
                else if (in.sname == "log2" && in.d == 1)
                    out += "    " + R(in.a) + " = vf(std::log2(" + R(in.c) + ".f));\n";
                else if (in.sname == "exp" && in.d == 1)
                    out += "    " + R(in.a) + " = vf(std::exp(" + R(in.c) + ".f));\n";
                else if (in.sname == "sin" && in.d == 1)
                    out += "    " + R(in.a) + " = vf(std::sin(" + R(in.c) + ".f));\n";
                else if (in.sname == "cos" && in.d == 1)
                    out += "    " + R(in.a) + " = vf(std::cos(" + R(in.c) + ".f));\n";
                else if (in.sname == "tan" && in.d == 1)
                    out += "    " + R(in.a) + " = vf(std::tan(" + R(in.c) + ".f));\n";
                else if (in.sname == "atan2" && in.d == 2)
                    out += "    " + R(in.a) + " = vf(std::atan2(" + R(in.c) + ".f, " +
                           R(in.c + 1) + ".f));\n";
                else { sink_.error({}, "native backend: native '" + in.sname + "'"); ok_ = false; }
                // byteToString is the first native FREE function that can
                // raise (Track 04 M3); CallDyn/CallValue/Call already check
                // g_throwing after the call, CallNativeFn never needed to
                // before since sysWrite/sysReadLine never raise.
                out += throwCheck((int)pc);
                break;
            case Op::Print:
                out += "    { std::string t = ts(" + R(in.a) + "); fwrite(t.data(), 1, t.size(), stdout); }\n";
                break;
            case Op::PrintNl:
                out += "    fputc('\\n', stdout);\n";
                break;
            case Op::VFree:
                break;   // §15: ELF-backend reclamation; shared_ptr frees here

            case Op::LoadGlobal:
                out += "    " + R(in.a) + " = g_globals[" + std::to_string(in.b) + "];\n";
                break;
            case Op::StoreGlobal:
                out += "    g_globals[" + std::to_string(in.b) + "] = " + R(in.a) + ";\n";
                break;
            default:
                sink_.error({}, "native backend does not yet cover this construct "
                                "(objects/collections/closures/exceptions)");
                ok_ = false;
                break;
        }
        if (!ok_) break;
    }
    if (target[fn.code.size()]) out += "L" + std::to_string(fn.code.size()) + ":\n";
    if (usesUnwind) out += "UNWIND:\n";                // propagate a pending throw
    out += "    return V{};\n}\n\n";
    return out;
}

int CGen::clsId(Symbol* cls) {
    auto it = clsId_.find(cls);
    if (it != clsId_.end()) return it->second;
    int id = (int)clsId_.size() + 1;
    clsId_[cls] = id;
    return id;
}

// All accessor/method/operator members of a class, walking base classes too.
static void collectMembers(Symbol* cls, std::vector<const Stmt*>& out) {
    if (!cls || !cls->decl) return;
    for (const StmtPtr& m : cls->decl->body)
        if (m->kind == StmtKind::Member && m->callable) out.push_back(m.get());
    for (const TypeRefPtr& b : cls->decl->bases) collectMembers(b->resolvedSymbol, out);
}

std::string CGen::generate() {
    // Reachability from the entry: follow direct calls, constructor $init, and
    // resolved dynamic dispatch; every instantiated class pulls in its members.
    std::vector<bool> reachable(mod_.functions.size(), false);
    std::vector<int> work{mod_.entry};
    reachable[mod_.entry] = true;
    if (mod_.ginit >= 0) { reachable[mod_.ginit] = true; work.push_back(mod_.ginit); }
    std::vector<Symbol*> instClasses;
    auto mark = [&](int idx) {
        if (idx >= 0 && idx < (int)mod_.functions.size() && !reachable[idx]) {
            reachable[idx] = true;
            work.push_back(idx);
        }
    };
    // bug.md #27/#28: an unresolved by-name CallDyn (a bare self-call inside an
    // unchecked prelude method body, or the iterator-protocol's `iterator`/
    // `hasNext`/`next` calls — techdesign-07 §2) carries only a method NAME, so
    // the walk can't follow it as it does a resolved CallDyn. Index every
    // in-language method by name so such a call marks its same-named candidates
    // (and pulls their declaring class into the callm dispatch table).
    std::unordered_map<std::string, std::vector<std::pair<int, Symbol*>>> methodFns;
    if (mod_.sema && mod_.sema->global)
        for (const auto& [nm, syms] : mod_.sema->global->names)
            for (Symbol* cls : syms)
                if (cls->kind == SymbolKind::Class)
                    for (const Slot& s : cls->shape.slots)
                        if (s.isMethod && s.decl && mod_.byDecl.count(s.decl))
                            methodFns[std::string(s.name)].push_back({mod_.byDecl.at(s.decl), cls});
    // SU-1: a by-name (decl-less) CallDyn dispatches on the receiver's runtime
    // class. A **reference** class can be that receiver only if it was actually
    // constructed (NewObject → `instClasses`); a never-instantiated reference
    // class is provably unreachable, so marking its same-named method emits dead
    // code — harmless for a pure-Leviathan body, but a HARD compile error when it
    // calls a native this backend can't lower (a plain-stream program dragging in
    // `TaskGroup::close` → `sysTaskCancel`, which emit-C++ rejects as loop-bound;
    // the SU-1 `InStream.close()` → `buf.close()` by-name call was the trigger).
    // **Value types (primitives/structs) are always possible receivers** — they
    // are inhabited by literals/value construction, never NewObject — so they are
    // never pruned (they are how `int.toString()` etc. reach callm's table via
    // `byNameClasses_`). Because instantiation is discovered DURING the walk,
    // remember each by-name and re-sweep (below) whenever `instClasses` grows.
    auto isPossibleRecv = [&](Symbol* c) {
        if (c->isValueType()) return true;                 // primitive/struct: always live
        for (Symbol* x : instClasses) if (x == c) return true;
        return false;
    };
    std::unordered_set<std::string> byNameSeen;
    auto markByName = [&](const std::string& sname) {
        byNameSeen.insert(sname);
        auto it = methodFns.find(sname);
        if (it == methodFns.end()) return;
        for (auto& [fnIdx, cls] : it->second) {
            if (!isPossibleRecv(cls)) continue;
            mark(fnIdx);
            bool seen = false;
            for (Symbol* c : byNameClasses_) if (c == cls) seen = true;
            if (!seen) byNameClasses_.push_back(cls);
        }
    };
    // Re-run every remembered by-name mark now that more reference classes may be
    // known instantiated; `mark` is idempotent so this only adds newly-live
    // targets. Returns true if it marked anything (drives the fixpoint below).
    auto byNameSweep = [&]() {
        bool progressed = false;
        for (const std::string& sname : byNameSeen) {
            auto it = methodFns.find(sname);
            if (it == methodFns.end()) continue;
            for (auto& [fnIdx, cls] : it->second) {
                if (!isPossibleRecv(cls) || reachable[fnIdx]) continue;
                mark(fnIdx);
                progressed = true;
                bool seen = false;
                for (Symbol* c : byNameClasses_) if (c == cls) seen = true;
                if (!seen) byNameClasses_.push_back(cls);
            }
        }
        return progressed;
    };
    // Drain `work` to fixpoint: follow direct calls, constructor $init, dynamic
    // dispatch (resolved by decl, else by name), and closures; every instantiated
    // class pulls in ALL its members (so name-based CallDyn can reach them).
    // Reused after the collection seed below so an iterator class constructed
    // inside Array/Map.iterator() (§2.1 uniformity) also gets its members emitted.
    auto drain = [&]() {
        while (!work.empty()) {
            int fi = work.back();
            work.pop_back();
            for (const Inst& in : mod_.functions[fi].code) {
                if (in.op == Op::Call) mark(in.b);
                else if (in.op == Op::NewObject) {
                    mark(in.b);                                   // $init
                    if (in.sym) {
                        bool seen = false;
                        for (Symbol* c : instClasses) if (c == in.sym) seen = true;
                        if (!seen) {
                            instClasses.push_back(in.sym);
                            std::vector<const Stmt*> mem;
                            collectMembers(in.sym, mem);
                            for (const Stmt* m : mem)
                                if (mod_.byDecl.count(m)) mark(mod_.byDecl.at(m));
                        }
                    }
                } else if (in.op == Op::CallDyn) {
                    if (in.decl && mod_.byDecl.count(in.decl)) mark(mod_.byDecl.at(in.decl));
                    else markByName(in.sname);                    // unresolved: follow by name
                } else if (in.op == Op::MakeClosure) {
                    mark(in.b);                                    // the lambda function
                    bool seen = false;
                    for (int f : closureFns_) if (f == in.b) seen = true;
                    if (!seen) closureFns_.push_back(in.b);
                }
            }
        }
    };
    // Fixpoint: drain follows calls/instantiations; the sweep then lights up any
    // by-name target whose class just became known-instantiated, which may itself
    // instantiate more — loop until neither makes progress.
    do { drain(); } while (byNameSweep());

    // If the program uses collections, seed the in-language Array/Map/Range
    // methods (map/where/reduce/iterator/...) so name-based dynamic dispatch
    // (callm) can reach them — e.g. `c.map(fn)` in a generic HKT function where
    // c's type is a type variable, or `for (x in it)` over an IIterable-typed
    // variable holding an Array/Range (techdesign-07 §2.1 uniformity).
    bool usesColl = false, usesRange = false;
    for (size_t i = 0; i < mod_.functions.size(); ++i)
        if (reachable[i])
            for (const Inst& in : mod_.functions[i].code)
                switch (in.op) {
                    case Op::NewArray: case Op::NewArraySized: case Op::NewMap:
                    case Op::GetIndex: case Op::IndexStore: case Op::IterAt:
                        usesColl = true; break;
                    case Op::MakeRange: usesRange = true; break;
                    default: break;
                }
    if (usesColl || usesRange) {
        std::vector<const char*> seed;
        if (usesColl) { seed.push_back("Array"); seed.push_back("Map"); }
        if (usesRange) seed.push_back("Range");
        for (const char* cn : seed) {
            Symbol* c = mod_.sema->global->lookup(cn);
            if (!c || !c->decl) continue;
            collClasses_.push_back(c);
            for (const StmtPtr& m : c->decl->body)
                if (m->kind == StmtKind::Member && m->callable && !m->isCtor &&
                    mod_.byDecl.count(m.get()))
                    mark(mod_.byDecl.at(m.get()));
        }
        do { drain(); } while (byNameSweep());   // full expansion: iterator classes' members too
    }

    // Pre-assign class ids for everything the dispatchers/issub reference, so
    // both are complete before any use: instantiated classes + their bases,
    // catch-clause types + their bases, and Range/Pair.
    std::function<void(Symbol*)> idChain = [&](Symbol* c) {
        if (!c || !c->decl) return;
        clsId(c);
        for (const TypeRefPtr& b : c->decl->bases) idChain(b->resolvedSymbol);
    };
    for (Symbol* c : instClasses) idChain(c);
    for (Symbol* c : byNameClasses_) idChain(c);   // bug.md #27/#28
    for (size_t i = 0; i < mod_.functions.size(); ++i)
        if (reachable[i])
            for (const Handler& h : mod_.functions[i].handlers) idChain(h.type);
    rangeId(); pairId();

    reachable_ = reachable;
    std::string out = "// generated by leviathan --emit-cpp\n";
    out += kRuntime;
    out += "\n";
    out += "static bool issub(int a, int b);\n";
    out += "static V callclosure(const V& c, std::vector<V>& a);\n";
    out += "static const char* clsname(int id);\n";
    out += "static V callm(const V& recv, const std::string& name, std::vector<V>& a);\n";
    for (size_t i = 0; i < mod_.functions.size(); ++i) {
        if (!reachable[i]) continue;
        const IrFunction& fn = mod_.functions[i];
        out += "static V f" + std::to_string(i) + "(";
        for (int p = 0; p < fn.nparams; ++p) out += (p ? ", V" : "V");
        out += ");\n";
    }
    out += "\n" + genDispatchers(instClasses) + "\n";
    for (size_t i = 0; i < mod_.functions.size(); ++i)
        if (reachable[i]) out += genFunction((int)i);
    out += genFieldSlot() + genFieldCount() + genIsValueClass() + genClosureDispatch() + genIsSub() + genClsName() + genCallM(instClasses);
    Symbol* rte = mod_.sema->global->lookup("RuntimeException");
    out += "int main(int argc, char** argv) {\n";
    // argv capture (designs/argv.md §5.3): real process args; rt_sysargs reads
    // this. Overwrites the [""] default only when argc >= 1 (always, in practice).
    out += "  if (argc > 0) { g_prog_args.clear();\n"
           "    for (int _i = 0; _i < argc; ++_i) g_prog_args.push_back(argv[_i]); }\n";
    out += "  RTE_ID = " + std::to_string(rte ? clsId(rte) : 0) + ";\n";
    if (mod_.ginit >= 0) {
        out += "  g_globals.resize(" + std::to_string(mod_.nglobals) + ");\n";
        out += "  f" + std::to_string(mod_.ginit) + "();\n";   // prelude globals
    }
    out += "  f" + std::to_string(mod_.entry) + "();\n";
    // Guaranteed restore-on-exit (designs/terminal-raw-mode.md §3.4): restore the
    // terminal BEFORE any uncaught traceback lands, so the message is readable.
    out += "  rt_termshutdown();\n";
    // Uncaught exception at top level — matches the interpreters' report and,
    // per designs/exit-codes.md §5 (gap b), sets the process status to 1 so a
    // crashed program reports failure to a shell / `set -e` / CI.
    out += "  if (g_throwing) {\n"
           "    g_exit_code = 1;\n"
           "    std::string cls = (g_thrown.k == 5 && g_thrown.o) ? clsname(g_thrown.o->cls) : \"value\";\n"
           "    std::string msg = (g_thrown.k == 5 && g_thrown.o) "
           "? objget(g_thrown, \"message\").s : \"\";\n"
           "    std::string t = \"Uncaught \" + cls + (msg.empty() ? \"\" : \": \" + msg) + \"\\n\";\n"
           "    fwrite(t.data(), 1, t.size(), stdout);\n"
           "  }\n";
    // exit-codes.md §4: return the program's exit code (was `return 0`).
    // env.setExitCode wrote it; env.exit already terminated via rt_sysexit.
    out += "  return g_exit_code;\n}\n";
    return ok_ ? out : "";
}

// callm(recv, name, args): name-based dynamic dispatch (the interpreter's
// CallDyn fallback) — in-language methods of the receiver's class (objects,
// Array, Map, primitives), else the native cores.
std::string CGen::genCallM(const std::vector<Symbol*>& instClasses) {
    // Dedup by class id: instClasses / collClasses_ / byNameClasses_ can overlap
    // (bug.md #27/#28 — a class reached by name may also be instantiated or a
    // collection), and two `case <same id>:` labels would not compile.
    std::vector<Symbol*> classes;
    std::set<int> seenCls;
    auto addCls = [&](Symbol* c) {
        if (c && seenCls.insert(clsId(c)).second) classes.push_back(c);
    };
    for (Symbol* c : instClasses) addCls(c);
    for (Symbol* c : collClasses_) addCls(c);
    for (Symbol* c : byNameClasses_) addCls(c);
    for (const char* p : {"string", "int", "bool", "float"}) {
        if (Symbol* c = mod_.sema->global->lookup(p)) addCls(c);
    }
    // classOf: map a value to its class id (inline literals from clsId()).
    auto id = [&](const char* n) {
        Symbol* c = mod_.sema->global->lookup(n);
        return std::to_string(c ? clsId(c) : 0);
    };
    std::string s =
        "static int classOf(const V& v) {\n"
        "  switch (v.k) { case 5: return v.o->cls; case 6: return " + id("Array") +
        "; case 7: return " + id("Map") + "; case 4: return " + id("string") +
        "; case 1: return " + id("int") + "; case 3: return " + id("bool") +
        "; case 2: return " + id("float") + "; }\n  return 0;\n}\n";
    s += "static V callm(const V& recv, const std::string& name, std::vector<V>& a) {\n"
         "  switch (classOf(recv)) {\n";
    for (Symbol* cls : classes) {
        // Dynamic dispatch must target each (name, arity)'s EFFECTIVE method for
        // this class — the collapse/override winner (info.md §4). `shape.slots`
        // already holds exactly one method slot per resolved member, so iterate
        // it rather than the raw inheritance walk (`collectMembers`), which lists
        // an overridden base method AND its override and let first-branch-wins
        // pick the base — wrong when the override is on a sibling mixin composed
        // only at a leaf (`Panel : Container, Bordered`, bug.md #56).
        std::string body;
        for (const Slot& sl : cls->shape.slots) {
            if (!sl.isMethod || !sl.decl) continue;
            const Stmt* m = sl.decl;
            if (!mod_.byDecl.count(m) || m->isGet || m->isSet || m->isCtor ||
                m->selector.symbolic)
                continue;
            int fi = mod_.byDecl.at(m);
            if (fi >= (int)reachable_.size() || !reachable_[fi]) continue;  // only emitted fns
            int np = mod_.functions[fi].nparams;   // incl. receiver
            std::string call = "f" + std::to_string(fi) + "(recv";
            for (int k = 1; k < np; ++k) call += ", a[" + std::to_string(k - 1) + "]";
            call += ")";
            // bug.md #13/#27: ALWAYS guard the branch by argument count. Two
            // reasons: (a) a same-name overload within one class must dispatch by
            // arity, not first-branch-wins; (b) an in-language method whose name
            // ALSO has a native 0/other-arity overload (e.g. `int.toString(radix)`
            // beside the native `int.toString()`) must not swallow a call of the
            // other arity — without the guard a 0-arg `toString()` matched the
            // 1-arg branch and read `a[0]` out of bounds. The language has no
            // default/variadic params, so an exact-arity guard never rejects a
            // legitimate call; a mismatch correctly falls through to the native
            // core below.
            std::string guard = "name == \"" + std::string(m->name) +
                                "\" && (int)a.size() == " + std::to_string(np - 1);
            body += "      if (" + guard + ") return " + call + ";\n";
        }
        if (!body.empty())
            s += "    case " + std::to_string(clsId(cls)) + ":\n" + body + "      break;\n";
    }
    // bug.md #2: no in-language method named `name` — before falling to the
    // native cores (which silently return V{} for an object receiver, since
    // none of their branches match k==5), check whether it's a FIELD holding
    // a closure and call that instead. Mirrors Eval.cpp's evalCall fallback
    // ("callee evaluates to a closure") so `b.fn(args)` on a closure-typed
    // field behaves the same on emit-C++ as on the oracle, instead of
    // silently discarding the call and returning a default value.
    s += "  }\n"
         "  if (recv.k == 5 && recv.o) {\n"
         "    V fv = objget(recv, name);\n"
         "    if (fv.k == 9) return callclosure(fv, a);\n"
         "  }\n"
         "  return callnative(recv, name, a);\n}\n";
    return s;
}

// clsname(id): the source name of a class id, for uncaught-exception reports.
std::vector<std::string> CGen::fieldKeys(Symbol* cls) {
    std::vector<std::string> keys;
    if (!cls) return keys;
    for (const Slot& s : cls->shape.slots) {
        if (s.isMethod) continue;
        keys.push_back((s.distinct && s.source)
            ? std::string(s.source->name) + "::" + std::string(s.name)
            : std::string(s.name));
    }
    return keys;
}

// fieldSlot/fieldCount: the packed field layout per class (§7). Deterministic,
// so getters and setters always agree; classes not covered use the `extra` map.
std::string CGen::genFieldSlot() {
    std::string s = "static int fieldSlot(int cls, const std::string& key) {\n  switch (cls) {\n";
    for (const auto& [sym, id] : clsId_) {
        std::vector<std::string> keys = fieldKeys(sym);
        if (keys.empty()) continue;
        s += "    case " + std::to_string(id) + ":\n";
        for (size_t i = 0; i < keys.size(); ++i)
            s += "      if (key == \"" + keys[i] + "\") return " + std::to_string(i) + ";\n";
        s += "      return -1;\n";
    }
    s += "  }\n  return -1;\n}\n";
    return s;
}
std::string CGen::genFieldCount() {
    std::string s = "static int fieldCount(int cls) {\n  switch (cls) {\n";
    for (const auto& [sym, id] : clsId_) {
        int n = (int)fieldKeys(sym).size();
        if (n) s += "    case " + std::to_string(id) + ": return " + std::to_string(n) + ";\n";
    }
    s += "  }\n  return 0;\n}\n";
    return s;
}
std::string CGen::genIsValueClass() {
    std::string s = "static bool isValueClass(int cls) {\n  switch (cls) {\n";
    for (const auto& [sym, id] : clsId_)
        if (sym->isValue) s += "    case " + std::to_string(id) + ": return true;\n";
    s += "  }\n  return false;\n}\n";
    return s;
}

std::string CGen::genClsName() {
    std::string s = "static const char* clsname(int id) {\n  switch (id) {\n";
    for (const auto& [sym, id] : clsId_)
        s += "    case " + std::to_string(id) + ": return \"" + std::string(sym->name) + "\";\n";
    s += "  }\n  return \"object\";\n}\n";
    return s;
}

// callclosure: invoke a lambda value — switch on its function id, calling the
// generated function with (closure, args...) at that function's arity.
std::string CGen::genClosureDispatch() {
    std::string s = "static V callclosure(const V& c, std::vector<V>& a) {\n"
                    "  switch (c.o->fn) {\n";
    for (int fnIdx : closureFns_) {
        int np = mod_.functions[fnIdx].nparams;   // includes the closure (r0)
        s += "    case " + std::to_string(fnIdx) + ": return f" + std::to_string(fnIdx) +
             "(c";
        for (int k = 1; k < np; ++k) s += ", a[" + std::to_string(k - 1) + "]";
        s += ");\n";
    }
    s += "  }\n  return V{};\n}\n";
    return s;
}

// issub(a, b): is class a the same as, or a subclass/implementor of, b —
// generated from the class hierarchy (bases include interfaces).
std::string CGen::genIsSub() {
    std::string s = "static bool issub(int a, int b) {\n  if (a == b) return true;\n"
                    "  switch (a) {\n";
    for (const auto& [sym, id] : clsId_) {
        if (!sym->decl || sym->decl->bases.empty()) continue;
        s += "    case " + std::to_string(id) + ": return ";
        bool first = true;
        for (const TypeRefPtr& b : sym->decl->bases) {
            if (!b->resolvedSymbol) continue;
            s += (first ? "" : " || ");
            first = false;
            s += "issub(" + std::to_string(clsId(b->resolvedSymbol)) + ", b)";
        }
        s += (first ? "false" : "");
        s += ";\n";
    }
    s += "  }\n  return false;\n}\n";
    return s;
}

// getm/setm (accessor-aware member access), opm (operator dispatch with (!=)
// derivation), and arith (object -> opm, else scalar ar) — the object dispatch
// the IR interpreter does dynamically, generated as switch tables over the
// classes actually instantiated in this program.
std::string CGen::genDispatchers(const std::vector<Symbol*>& instClasses) {
    std::string getm = "static V getm(const V& o, const std::string& key) {\n"
        "  std::string nm = key; auto p = key.rfind(\"::\"); "
        "if (p != std::string::npos) nm = key.substr(p + 2);\n"
        "  switch (o.o->cls) {\n";
    std::string setm = "static void setm(V& o, const std::string& key, const V& val) {\n"
        "  std::string nm = key; auto p = key.rfind(\"::\"); "
        "if (p != std::string::npos) nm = key.substr(p + 2);\n"
        "  switch (o.o->cls) {\n";
    std::string opm = "static V opm(int op, const V& l, const V& r) {\n"
        "  switch (l.o->cls) {\n";

    std::string idxg, idxs;   // ([]) get/set accessor dispatch (per class)

    for (Symbol* cls : instClasses) {
        std::vector<const Stmt*> mem;
        collectMembers(cls, mem);
        std::string gets, sets, ops, eqIdx, ig, is;
        for (const Stmt* m : mem) {
            if (!mod_.byDecl.count(m)) continue;
            std::string fn = "f" + std::to_string(mod_.byDecl.at(m));
            bool indexer = std::string(m->name) == "[]";
            if (m->isGet && indexer)
                ig += "      return " + fn + "(base, idx);\n";
            else if (m->isSet && indexer)
                is += "      " + fn + "(base, idx, val); return base;\n";
            else if (m->isGet)
                gets += "      if (nm == \"" + std::string(m->name) + "\") return " +
                        fn + "(o);\n";
            else if (m->isSet)
                sets += "      if (nm == \"" + std::string(m->name) + "\") { " +
                        fn + "(o, val); return; }\n";
            else if (m->selector.symbolic) {
                std::string sym(m->selector.text);
                int oc = -1;
                const char* names[] = {"+","==","!=","-","*","/","%","<",">","<=",">=",
                                       "&","|","<<",">>"};
                for (int k = 0; k < 15; ++k) if (sym == names[k]) oc = k;
                if (oc >= 0)
                    ops += "      if (op == " + std::to_string(oc) + ") return " +
                           fn + "(l, r);\n";
                if (sym == "==") eqIdx = fn;
            }
        }
        int id = clsId(cls);
        std::string c = "    case " + std::to_string(id) + ":\n";
        if (!gets.empty()) getm += c + gets + "      break;\n";
        if (!sets.empty()) setm += c + sets + "      break;\n";
        if (!ig.empty())   idxg += c + ig;
        if (!is.empty())   idxs += c + is;
        if (!ops.empty() || !eqIdx.empty()) {
            opm += c + ops;
            if (!eqIdx.empty()) opm += "      if (op == 2) return vb(!" + eqIdx + "(l, r).b);\n";
            opm += "      break;\n";
        }
    }
    getm += "  }\n  return objget(o, key);\n}\n";
    setm += "  }\n  objset(o, key, val);\n}\n";
    // bug #77: a struct with no explicit (==) is field-wise by default
    // (info.md §9); a class with no (==) is reference identity. keyEq already
    // does the field-wise recursion for Map keys — reuse it here.
    opm += "  }\n"
           "  if (op == 1 || op == 2) { bool same = (l.o && isValueClass(l.o->cls)) "
           "? keyEq(l, r) : (r.k == 5 && l.o == r.o); "
           "return vb(op == 1 ? same : !same); }\n"
           "  return V{};\n}\n";

    // idxget/idxset: ([]) accessor on objects; native index on arrays/maps.
    std::string idxget =
        "static V idxget(const V& base, const V& idx) {\n"
        "  if (base.k == 5) { switch (base.o->cls) {\n" + idxg + "  } }\n"
        "  if (base.k == 6) { long long i = idx.i, n = arrLen(base);\n"
        "    if (!base.arr || i < 0 || i >= n) { "
        "raise(\"index \" + std::to_string(i) + \" out of bounds (length \" + "
        "std::to_string(n) + \")\"); return V{}; }\n"
        "    return arrGet(base, i); }\n"
        "  if (base.k == 7) { for (auto& kv : *base.mp) if (keyEq(kv.first, idx)) return kv.second; }\n"
        "  return V{};\n}\n";
    std::string idxset =
        "static V idxset(const V& base, const V& idx, const V& val) {\n"
        "  if (base.k == 5) { switch (base.o->cls) {\n" + idxs + "  } return base; }\n"
        // §11/§15 COW: uniquely owned -> mutate in place; aliased -> pure copy.
        "  if (base.k == 6) { long long i = idx.i;\n"
        "    if (base.arr && base.arr.use_count() == 1) { V s = base; "
        "if (i >= 0 && i < arrLen(s)) arrSet(s, i, val); return s; }\n"
        "    V r = arrCow(base); "
        "if (i >= 0 && i < arrLen(r)) arrSet(r, i, val); return r; }\n"
        "  if (base.k == 7) {\n"
        "    if (base.mp && base.mp.use_count() == 1) { "
        "for (auto& kv : *base.mp) if (keyEq(kv.first, idx)) { kv.second = val; return base; } "
        "base.mp->push_back({idx, val}); return base; }\n"
        "    V r = mkmap(); if (base.mp) *r.mp = *base.mp; bool rep = false; "
        "for (auto& kv : *r.mp) if (keyEq(kv.first, idx)) { kv.second = val; rep = true; break; } "
        "if (!rep) r.mp->push_back({idx, val}); return r; }\n"
        "  return base;\n}\n";

    // iterat: array element / range value / map entry as a Pair object.
    std::string iterat =
        "static V iterat(const V& it, long long idx) {\n"
        "  if (it.k == 6) return (it.arr && idx >= 0 && idx < arrLen(it)) ? arrGet(it, idx) : V{};\n"
        "  if (it.k == 5 && it.o) return vi(objget(it, \"start\").i + idx);\n"
        "  if (it.k == 7 && it.mp && idx >= 0 && idx < (long long)it.mp->size()) {\n"
        "    V p = mkobj(" + std::to_string(pairId()) + "); objset(p, \"first\", (*it.mp)[idx].first); "
        "objset(p, \"second\", (*it.mp)[idx].second); return p; }\n"
        "  return V{};\n}\n";

    std::string arith =
        "static V arith(int op, const V& l, const V& r) {\n"
        // `==`/`!=` against None compare by kind, never via the object's own
        // `==` operator (which would deref None) — mirrors Eval::combine's None
        // short-circuit. Without this, `obj == None` dispatches into opm and
        // crashes (or returns void for a struct with no `==`). Track 03 §2.
        "  if ((op == 1 || op == 2) && (l.k == 8 || r.k == 8)) return ar(op, l, r);\n"
        "  if (l.k == 5) return opm(op, l, r);\n"
        "  return ar(op, l, r);\n}\n";
    return getm + setm + opm + idxget + idxset + iterat + arith;
}

int CGen::rangeId() {
    Symbol* r = mod_.sema->global->lookup("Range");
    return r ? clsId(r) : 0;
}
int CGen::pairId() {
    Symbol* p = mod_.sema->global->lookup("Pair");
    return p ? clsId(p) : 0;
}
