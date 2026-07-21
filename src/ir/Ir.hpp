#pragma once
#include "core/Ast.hpp"
#include "runtime/RuntimeValue.hpp"
#include "core/Symbols.hpp"
#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
//  The bytecode IR (§17): the single semantic truth both variants share.
//
//  A register machine with high-level object/collection ops backed by the
//  shared runtime (the Python/Ruby-bytecode model): the AOT backend will later
//  lower these further (shape offsets, direct calls); the IR interpreter
//  executes them directly. Method/accessor/constructor BODIES — including the
//  in-language stdlib from the prelude — are themselves lowered to IR
//  functions; `this` is register 0 of member functions.
//
//  Instructions carry Symbol*/Stmt* pointers into the semantic model. That is
//  fine for an in-memory IR tied to one compilation; serialization would
//  index them.
// ---------------------------------------------------------------------------

enum class Op {
    // data
    LoadConst,    // a=dst, b=const index
    Default,      // a=dst, sname=canonical type   (§3 auto-construct default)
    Move,         // a=dst, b=src
    MoveClear,    // a=dst, b=src  — move then clear src (transfer ownership; makes
                  //   `x = x.op(...)` hand its buffer to op uniquely, enabling COW-in-place)
    CopyVal,      // a=dst, b=src  — deep-copy iff src is a value struct (else move)
    Arith,        // a=dst, b=lhs, c=rhs, tk=op    (objects: (op) method / (!=) deriv)
    Not,          // a=dst, b=src
    Neg,          // a=dst, b=src
    // control
    Jump,         // a=target
    JumpIfFalse,  // a=cond, b=target
    JumpIfTrue,   // a=cond, b=target
    Ret,          // a=value reg
    RetVoid,      //
    Throw,        // a=value reg
    // calls
    Call,         // a=dst, b=fn index, c=arg window start, d=argc
    CallDyn,      // a=dst, c=window start (window[0]=receiver), d=1+argc,
                  //   sname=method name, decl=resolved decl or null
    CallValue,    // a=dst, c=window start (window[0]=callable value), d=1+argc
    CallNativeFn, // a=dst, c=arg window start, d=argc, sname=std::sys native
    // objects
    NewObject,    // a=dst, sym=class (runs the class's $init via b=init fn index, -1 none)
    GetMember,    // a=dst, b=obj, sname="name" or "Source::name"  (accessor-aware)
    SetMember,    // a=value, b=obj, sname=...                     (accessor-aware)
    RawGet,       // a=dst, b=obj, sname=key   (inside own accessor / $init)
    RawSet,       // a=value, b=obj, sname=key
    RawGetWeak,   // a=dst (+1 ordinary value if live), b=obj, weak slot
    RawSetWeak,   // a=value, b=obj; slot stores no ownership of value
    // collections
    NewArray,     // a=dst, c=elem window start, d=count (Range elements spread)
    NewArraySized,// a=dst, c=arg window start, d=argc (0: empty; 2: n, fill)
    NewMap,       // a=dst
    NewBlock,     // a=dst, c=size reg (Block(n))                     — Track 03 §3
    NewBlockStr,  // a=dst, c=string reg (Block::fromString(s))       — Track 03 §3
    MakeRange,    // a=dst, b=lo reg, c=hi reg
    GetIndex,     // a=dst, b=base, c=index    (([]) accessor / native)
    IndexStore,   // a=dst(new base), b=base, c=index, d=value reg
    IterLen,      // a=dst, b=iterable         (array/map/range)
    IterAt,       // a=dst, b=iterable, c=index reg
    ColGet,       // a=dst, b=arr, c=index, d=slotK, sname=field, sym=elem struct
                  //   techdesign-columnar-arrays.md §5.4: the fused element-field
                  //   read of a columnar Array<struct> — dst = arr[index].field
                  //   read straight from column d (no gather). Boxed-fallback
                  //   (array still empty) reads the element field the boxed way.
    IsType,       // a=dst(bool), b=src, sname=tested canonical, sym=class or null
    Await,        // a=dst, b=promise reg  (pump loop until ready; yield value)
    LoadGlobal,   // a=dst, b=global index
    StoreGlobal,  // a=src, b=global index
    // closures
    MakeClosure,  // a=dst, b=fn index
    CaptureVar,   // a=closure reg, b=src reg, sname=name
    LoadCapture,  // a=dst, sname=name          (reads from this-closure = r0)
    // output
    Print,        // a=value reg
    PrintNl,      //
    // §15 escaping-tier ARC
    VFree,        // a=value reg — free a DEAD standalone value-struct copy: the
                  //   return-site copy of a struct-returning call, already consumed
                  //   (copied out) by the caller. ELF backend reclaims; GC'd engines no-op.
};

struct Inst {
    Op op;
    int a = 0, b = 0, c = 0, d = 0;
    TokenKind tk = TokenKind::End;
    std::string sname;
    Symbol* sym = nullptr;
    const Stmt* decl = nullptr;
};

// One catch clause of a try range (in clause order).
struct Handler {
    int start = 0, end = 0;      // protected pc range [start, end)
    int handlerPc = 0;
    int bindReg = 0;
    Symbol* type = nullptr;
};

struct IrFunction {
    std::string name;
    int nparams = 0;             // registers holding inputs (incl. `this` if hasThis)
    bool hasThis = false;
    int nregs = 0;
    std::vector<Inst> code;
    std::vector<Value> consts;
    std::vector<Handler> handlers;
};

struct IrModule {
    std::vector<IrFunction> functions;
    std::unordered_map<const Stmt*, int> byDecl;    // member/function decl -> fn index
    std::unordered_map<Symbol*, int> initByClass;   // class -> $init fn index
    const Sema* sema = nullptr;                     // shapes/symbols for dynamic ops
    int entry = -1;                                 // synthetic @main
    int ginit = -1;                                 // prelude-globals init (interp-only)
    int nglobals = 0;
    std::unordered_map<std::string, int> globalIndex;
    // techdesign-columnar-arrays.md: compiled with --columnar (staged flag). Set
    // by the Lowerer; read by LlvmGen (descriptor + lv_cfg_columnar emission) and
    // the fusion/ownership discipline. Interpreters ignore it (they stay boxed).
    bool columnar = false;
};

// bug #35: the set of global slots (LoadGlobal/StoreGlobal indices) each IR
// function references, unioned TRANSITIVELY over the closures it CREATES
// (MakeClosure targets) but NOT over the top-level functions it merely CALLS.
// This depth mirrors the interpreters' AST free-name walk (a spawn body's
// nested lambdas ARE part of its body; a called function's body is not), so
// the bare-global-Promise reject (route A) fires identically on every engine:
// LlvmGen bakes it into the codegen `lv_spawn_global_check` switch and the IR
// interpreter consults it at the spawn call. The MakeClosure graph is a DAG (a
// nested lambda's fn index is always fresh, never an ancestor's), so the
// memoized walk needs no cycle back-edge handling beyond the in-progress
// guard. Returns one sorted, duplicate-free slot list per function index.
inline std::vector<std::vector<int>> computeFnGlobalRefs(const IrModule& mod) {
    size_t n = mod.functions.size();
    std::vector<std::vector<int>> refs(n);
    std::vector<char> state(n, 0);   // 0 unvisited, 1 in-progress, 2 done
    std::function<const std::vector<int>&(size_t)> visit =
        [&](size_t fi) -> const std::vector<int>& {
        if (state[fi] != 0) return refs[fi];   // done, or in-progress (DAG: no back-edge)
        state[fi] = 1;
        std::vector<int> slots;
        for (const Inst& in : mod.functions[fi].code) {
            if (in.op == Op::LoadGlobal || in.op == Op::StoreGlobal)
                slots.push_back(in.b);
            else if (in.op == Op::MakeClosure && in.b >= 0 && (size_t)in.b < n)
                for (int s : visit((size_t)in.b)) slots.push_back(s);
        }
        std::sort(slots.begin(), slots.end());
        slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
        refs[fi] = std::move(slots);
        state[fi] = 2;
        return refs[fi];
    };
    for (size_t i = 0; i < n; i++) visit(i);
    return refs;
}
