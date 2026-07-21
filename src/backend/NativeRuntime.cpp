// The native value runtime linked into LLVM-compiled binaries. C ABI; the
// semantics replicate the shared scalar arithPrim/valueToString exactly, so
// native output is identical to the interpreters. Strings are immutable
// malloc'd buffers; intermediates are leaked for now (the ownership pass will
// drive real frees once codegen consumes it).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {

// kind: 0 void, 1 int, 2 float, 3 bool, 4 string
struct LV {
    int k;
    long long i;
    double f;
    const char* s;
};

void lvrt_void(LV* d) { d->k = 0; d->i = 0; d->f = 0; d->s = nullptr; }
void lvrt_int(LV* d, long long x) { d->k = 1; d->i = x; d->f = 0; d->s = nullptr; }
void lvrt_float(LV* d, double x) { d->k = 2; d->i = 0; d->f = x; d->s = nullptr; }
void lvrt_bool(LV* d, int x) { d->k = 3; d->i = x; d->f = 0; d->s = nullptr; }
void lvrt_str(LV* d, const char* x) { d->k = 4; d->i = 0; d->f = 0; d->s = x; }

static const char* lvToString(const LV* v) {
    // must match valueToString for the scalar kinds
    switch (v->k) {
        case 1: {
            std::string t = std::to_string(v->i);
            return strdup(t.c_str());
        }
        case 2: {
            std::string t = std::to_string(v->f);
            return strdup(t.c_str());
        }
        case 3: return v->i ? "true" : "false";
        case 4: return v->s ? v->s : "";
    }
    return "";
}

int lvrt_truth(LV* v) { return v->k == 3 ? (int)v->i : 0; }

void lvrt_not(LV* d, LV* a) { lvrt_bool(d, !lvrt_truth(a)); }
void lvrt_neg(LV* d, LV* a) {
    if (a->k == 2) lvrt_float(d, -a->f);   // a float negates its .f payload
    else lvrt_int(d, -a->i);
}

// op codes: 0 + | 1 == | 2 != | 3 - | 4 * | 5 / | 6 % | 7 < | 8 > | 9 <= | 10 >=
void lvrt_arith(int op, LV* d, LV* l, LV* r) {
    if (l->k == 4 || r->k == 4) {
        if (op == 0) {
            const char* ls = lvToString(l);
            const char* rs = lvToString(r);
            size_t n = strlen(ls) + strlen(rs);
            char* buf = (char*)malloc(n + 1);
            strcpy(buf, ls);
            strcat(buf, rs);
            lvrt_str(d, buf);
            return;
        }
        const char* ls = l->s ? l->s : "";
        const char* rs = r->s ? r->s : "";
        if (op == 1) { lvrt_bool(d, strcmp(ls, rs) == 0); return; }
        if (op == 2) { lvrt_bool(d, strcmp(ls, rs) != 0); return; }
    }
    // Float arithmetic in double (an int operand promotes) — mirrors arithPrim.
    if ((l->k == 2 && (r->k == 2 || r->k == 1)) || (r->k == 2 && l->k == 1)) {
        double a = l->k == 2 ? l->f : (double)l->i;
        double b = r->k == 2 ? r->f : (double)r->i;
        switch (op) {
            case 0:  lvrt_float(d, a + b); return;
            case 3:  lvrt_float(d, a - b); return;
            case 4:  lvrt_float(d, a * b); return;
            case 5:  lvrt_float(d, a / b); return;
            case 1:  lvrt_bool(d, a == b); return;
            case 2:  lvrt_bool(d, a != b); return;
            case 7:  lvrt_bool(d, a < b); return;
            case 8:  lvrt_bool(d, a > b); return;
            case 9:  lvrt_bool(d, a <= b); return;
            case 10: lvrt_bool(d, a >= b); return;
        }
        lvrt_void(d);                       // % / bitwise: no float form
        return;
    }
    long long a = l->i, b = r->i;
    switch (op) {
        case 0:  lvrt_int(d, a + b); return;
        case 1:  lvrt_bool(d, a == b); return;
        case 2:  lvrt_bool(d, a != b); return;
        case 3:  lvrt_int(d, a - b); return;
        case 4:  lvrt_int(d, a * b); return;
        case 5:  lvrt_int(d, b ? a / b : 0); return;
        case 6:  lvrt_int(d, b ? a % b : 0); return;
        case 7:  lvrt_bool(d, a < b); return;
        case 8:  lvrt_bool(d, a > b); return;
        case 9:  lvrt_bool(d, a <= b); return;
        case 10: lvrt_bool(d, a >= b); return;
    }
    lvrt_void(d);
}

void lvrt_print(LV* v) {
    const char* t = lvToString(v);
    fwrite(t, 1, strlen(t), stdout);
}

void lvrt_print_nl(void) { fputc('\n', stdout); }

void lvrt_syswrite(LV* d, LV* fd, LV* s) {
    const char* data = s->s ? s->s : "";
    size_t n = strlen(data);
    fwrite(data, 1, n, fd->i == 2 ? stderr : stdout);
    lvrt_int(d, (long long)n);
}

void lvrt_readline(LV* d, LV* fd) {
    (void)fd;
    std::string line;
    int c;
    while ((c = fgetc(stdin)) != EOF && c != '\n') line.push_back((char)c);
    lvrt_str(d, strdup(line.c_str()));
}

}  // extern "C"
