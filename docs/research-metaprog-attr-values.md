# Research: Attribute-Value Reflection & the Rest of the Metaprog Phase-4 Subset

**Status:** research input for a tech design. Not a design; carries no rulings.
**Feeds:** `designs/requests/request-metaprog-attr-values.md` (LA-4/5/6/7, LA-21, LA-27 —
items A–G).
**Date:** 2026-07-19. **Author target:** whoever writes the tech design for this ticket.
**Grounding:** current tree, `HEAD` = `386e476` ("Merge remote-tracking branch
'origin/master' into agent1"). Every file:line citation below was read fresh from the
current source, not copied from an older design doc — several of those (Phase 3, Phase 4)
are themselves stale against this tree in exactly the way `techdesign-metaprog-phase4.md`
predicts (its own §1 admits its `Parser.cpp:1041` citation had already drifted by the time
Phase 4 shipped). Where an older doc's claim disagrees with the code, this document trusts
the code and says so.

This assembles everything needed to write the design: the exact current shape of the rule
engine's attribute/reflection machinery (with full code, not paraphrase), a corrected
landing-status table for the request's seven items (three of the seven are further along
than the request ticket — filed 2026-07-06/07 — could have known), and a worked design
for each remaining gap, including the one genuinely hard problem in the whole ticket
(item G's member-reference-as-value) with a concrete, buildable resolution mechanism.

---

## 0. Contents

1. Executive summary — corrected landing status per item
2. The substrate: how the rule engine's attribute/reflection machinery works today
3. Item A — attribute-value reflection (`meta::Attr`) — the headline
4. Item B — statement-position `$for`
5. Item C — named attribute arguments — **already landed**
6. Item D — Layer D `rewrites`/`replace`/`$body` — **already landed**
7. Item E — inherited/mixin field visibility (`C.allFields`)
8. Item F — `meta::Class.attrs`
9. Item G — member references + pipes as attribute argument values
10. Cross-cutting design considerations
11. Documentation gaps worth closing regardless
12. File-level change map
13. Suggested sequencing
14. Open questions for the design doc to rule on

---

## 1. Executive summary — corrected landing status

The request ticket was filed 2026-07-06/07 against a tree that has since moved. Two
items are **fully done**, one is **effectively done** with a minor residual, and the
remaining four are genuinely open — but two of those four (E, F) are now cheap given
data the resolver already computes, and one (G) is the one real design problem in the
ticket.

| Item | Ask | Status today | Where |
|---|---|---|---|
| **A** | Attribute-value reflection (`meta::Attr`) | **Open.** Designed-not-built per `techdesign-metaprog-phase4.md` §9.1 (item I); no `meta::Attr`, no `attrObjects`, confirmed absent by direct grep. | §3 below |
| **B** | Statement-position `$for` | **Open.** Designed-not-built per phase4 §9.2 (item J); only the expr/array-literal `ForSplice` exists, no `StmtKind::ForSplice`. | §4 below |
| **C** | Named attribute args (`@Column(name: "x")`) | **LANDED.** Rode the general named-arguments feature (`designs/complete/techdesign-named-arguments.md`, 2026-07-10). `RuleEngine::evalAttrArgs` (`src/Rules.cpp:1155-1233`) already does full positional+named binding against an attribute's fields, with duplicate/unknown-name diagnostics and field-initializer defaults. Corpus: `tests/corpus/meta/attr_named.lev`. | §5 below |
| **D** | Layer D `rewrites`/`replace`/`$body` | **LANDED.** Shipped 2026-07-10 per `techdesign-metaprog-phase4.md` §14 — after the request was filed. `rewrites body of`, `replace`, `$body`, `reentrant`, M30–M35 are all in tree (`src/Parser.cpp:1368-1408`, `src/Rules.cpp:1774-1809` and surrounding). | §6 below |
| **E** | Inherited/mixin field visibility (`C.allFields`) | **Open**, but cheap: the resolver already flattens inherited fields into `Shape::slots` (`src/Resolver.cpp:6266-6354`) for entirely unrelated reasons (object layout). `buildMetaValue` just never reads that table — it walks `decl->body` only. | §7 below |
| **F** | `meta::Class.attrs` | **Open**, trivial: mirrors a pattern (`Array<string> attrs` + `hasAttr`) already built twice (Field, Method); just never applied to Class. | §8 below |
| **G** | Named args *(discharged by C)* + member references + pipes as attribute values | **Open**, and the one real design problem. Syntactically nothing is missing — the parser already accepts arbitrary expressions (including `A::B::c` chains and `x \| y`) as attribute arguments. The gap is semantic: attribute values are evaluated by an untyped comptime oracle gated to `int/float/bool/string`, and a qualified reference to a non-value entity (a class, a field) silently folds to `Void`/`0` today rather than erroring — see §9.2 for the concrete landmine. | §9 below |

Two corrections worth internalizing before designing anything:

1. **Don't design a named-args mechanism for attributes (item C's literal ask).** It
   exists, is tested, and item G's `self:` label already "just works" as an ordinary
   named argument the moment its *value* can be represented (see §9.5 — `self:` is not a
   new keyword or a new binding mechanism at all).
2. **Don't design Layer D.** It shipped days after this ticket was filed. If the design
   doc inherits the request's text verbatim it will re-litigate settled ground.

---

## 2. The substrate: how attribute/reflection machinery works today

Everything in §§3–9 is an extension of this. Read this section once; every later section
cites back into it rather than repeating it.

### 2.1 Pipeline order — attributes are resolved *before* the Checker, by a separate oracle

`src/main.cpp`, non-error path:

| Step | Call | Location |
|---|---|---|
| Parse | `parser.parseProgram()` | `main.cpp:387-388` |
| Resolver pass 1 (builds `Shape::slots`, see §2.2) | `resolver.run(program)` | `main.cpp:446-448` |
| **RuleEngine** (attributes, rules, macros, comptime) | `engine->run(program)` | `main.cpp:489` |
| Resolver pass 2 (only if the tree changed) | `resolver2->run(program)` | `main.cpp:493-495` |
| **Checker** | `checker.run(program, ...)` | `main.cpp:542-543` |

`RuleEngine::run` (`src/Rules.cpp:64-108`) itself is lettered passes A–G (unchanged since
Phase 3 — see `techdesign-metaprog-phase3.md` §9):

```cpp
bool RuleEngine::run(Program& program) {
    changed_ = false;
    collectRules(program.items, "<root>");        // A. detach rules/macros
    validateMacroDecls();                          // M22
    validateRewriteRules();                         // M32/M35
    comptimeScope_ = sema_.global;
    walkTopLevelItems(program.items);               // B. comptime fold only
    imports_ = computeFileImports(files_, program);  // C. recompute imports post-fold
    macroExpansionEnabled_ = true;
    walkTopLevelItems(program.items);               // D. macro-call expansion
    macroExpansionEnabled_ = false;
    walkAttrs(program.items);                        // E. attribute resolution (evalAttrArgs)
    if (!rules_.empty()) { /* F. match + expand rules */ ... }
    ...
```

**The Checker never touches attributes.** `grep -n "attrs\|AttrUse" src/Checker.cpp
src/Checker.hpp` returns one unrelated comment hit. Every real read/write of
`Stmt::attrs`/`AttrUse` in the compiler is inside `src/Rules.cpp` (~15 sites). By the
time `Checker::run` sees the tree, attributes have already been fully evaluated.

This matters for design: **attribute-argument values are computed by
`Evaluator::evalComptime` — a dynamically-typed tree-walk interpreter (the "oracle"),
not by the Checker's static type resolver.** Any new attribute-argument value kind (item
G) has to either extend that oracle, or bypass it with a separate static-resolution path
that runs inside `RuleEngine` directly against `Sema`. §9.3 recommends the latter, and
explains why.

One consequence worth stating plainly for the design doc: **`Shape::slots` (§2.2, the
fully inheritance-flattened field/method table) is completely built by the time
`RuleEngine` even exists** — `Resolver::run()` ends with `for (Symbol* cls :
classSymbols_) buildShape(cls);` (`Resolver.cpp:6858`) and only afterward does
`main.cpp:471` construct the `RuleEngine`. So anything §7 (item E) or §9 (item G) needs
from inheritance is *already computed and sitting there* — nothing in this ticket
requires writing new inheritance-walking logic.

### 2.2 `Shape::slots` — the already-flattened, inheritance-inclusive field table

`src/Symbols.hpp:35-52`:

```cpp
struct Slot {
    std::string_view name;
    std::string canonical;    // field: type; method: "(params) -> ret"
    std::string paramsCanon;   // method only: "(params)"
    std::string retCanon;      // method only: return type
    Symbol* source = nullptr; // the class that declared this slot (for ::)
    const Stmt* decl = nullptr;
    Access access = Access::Default;
    bool isMethod = false;
    bool distinct = false;    // kept separate under a same-name+same-type clash
    bool isConst = false;
    bool isReadonly = false;
    bool isWeak = false;
};

struct Shape {
    std::vector<Slot> slots;
    bool built = false;
    bool building = false;
};
```

`Resolver::buildShape` (`Resolver.cpp:6266-6354`) merges every non-interface base's
already-built `shape.slots` into the derived class's own, recursively, before adding the
class's own members:

```cpp
for (const TypeRefPtr& base : cls->decl->bases) {
    Symbol* baseSym = base->resolvedSymbol;
    if (!baseSym || baseSym->kind != SymbolKind::Class) continue;
    buildShape(baseSym);
    if (baseSym->isValue)
        sink_.error(..., "cannot inherit from struct '...'; value types are final");
    else if (cls->isValue && !baseSym->isInterface())
        sink_.error(..., "struct '...' cannot inherit implementation; ...interfaces");
    if (baseSym->isInterface() && !cls->isInterface()) {
        for (const Slot& s : baseSym->shape.slots) interfaceReqs.push_back(...);
    } else {
        for (const Slot& s : baseSym->shape.slots) mergeSlot(slots, ...);   // <-- the flatten
    }
}
for (const StmtPtr& m : cls->decl->body) { ... mergeSlot(slots, slotOf(m.get(), cls)); }
```

**`class` supports true multiple field-bearing inheritance** — there is no "only one base
can carry state" restriction; the restriction (`docs/reference.md:511-525`) applies only
to `struct` ("final... may implement interfaces, may not inherit implementation"). A
`class User : DbTracking, Timestamps` already has both bases' fields flattened into
`User`'s `shape.slots`, and this is not incidental — `src/Checker.cpp:444-452` has a
comment reasoning explicitly about "sibling mixin" dispatch, and `docs/reference.md:498`
labels the feature "Multiple inheritance" in the language's own reference doc. Every
runtime/codegen backend already consumes this flattened table for real object layout —
`Evaluator::initFields` (`src/Eval.cpp:339-344`), `Lowerer::fieldSlotOf`
(`src/Lower.cpp:93-106`), `LlvmGen.cpp:521` (`fieldKeysOf`), etc. — so `User`'s runtime
instances already *have* `Timestamps.createdAt` as a real field; only the
*metaprogramming reflection* of that fact is missing (§7).

Critically, **`Slot::decl` points straight back to the original member `Stmt*`** — the
exact same `Stmt*` type `buildMetaValue` (§2.3) already knows how to turn into a
`meta::Field`/`meta::Method` object. This is what makes item E additive rather than a new
subsystem (§7.3).

### 2.3 `buildMetaValue` — how a decl becomes a `meta::*` reflection object

`src/Rules.cpp:1378-1450`, called lazily (only if a `where`/`$for`/template-hole
expression actually reads a decl binding as a plain value — most rules never trigger it —
via `materializeBindings`, `Rules.cpp:1452-1463`) and cached per-`Stmt*` in `metaCache_`:

```cpp
Value RuleEngine::buildMetaValue(Stmt* decl) {
    if (!decl) return vvoid();
    auto cached = metaCache_.find(decl);
    if (cached != metaCache_.end()) return cached->second;

    Value v;
    if (decl->kind == StmtKind::Class) {
        Symbol* cls = metaClassSymbol("Class");
        if (!cls) return vvoid();
        v.kind = VKind::Object;
        v.obj = std::make_shared<Object>();
        v.obj->cls = cls;
        v.obj->fields["name"] = vstr(std::string(decl->name));

        Value bases; bases.kind = VKind::Array;
        bases.arr = std::make_shared<std::vector<Value>>();
        for (const TypeRefPtr& base : decl->bases)
            if (base) bases.arr->push_back(vstr(base->canonical));
        v.obj->fields["bases"] = bases;

        Value fields; fields.kind = VKind::Array;
        fields.arr = std::make_shared<std::vector<Value>>();
        Value methods; methods.kind = VKind::Array;
        methods.arr = std::make_shared<std::vector<Value>>();
        for (const StmtPtr& m : decl->body) {          // <-- §7's gap: OWN body only
            if (m->kind != StmtKind::Member) continue;
            if (m->callable) methods.arr->push_back(buildMetaValue(m.get()));
            else fields.arr->push_back(buildMetaValue(m.get()));
        }
        v.obj->fields["fields"] = fields;
        v.obj->fields["methods"] = methods;
    } else if (decl->kind == StmtKind::Member) {
        Symbol* cls = metaClassSymbol(decl->callable ? "Method" : "Field");
        if (!cls) return vvoid();
        v.kind = VKind::Object;
        v.obj = std::make_shared<Object>();
        v.obj->cls = cls;
        v.obj->fields["name"] = vstr(std::string(decl->name));

        Value attrs; attrs.kind = VKind::Array;
        attrs.arr = std::make_shared<std::vector<Value>>();
        for (const AttrUse& a : decl->attrs)            // <-- §3's gap: names only
            if (a.resolved) attrs.arr->push_back(vstr(std::string(a.resolved->name)));
        v.obj->fields["attrs"] = attrs;

        if (decl->callable) {
            v.obj->fields["returnType"] = vstr(decl->type ? decl->type->canonical : std::string());
            Symbol* paramCls = metaClassSymbol("Param");
            Value params; params.kind = VKind::Array;
            params.arr = std::make_shared<std::vector<Value>>();
            for (const Param& p : decl->params) {
                Value pv; pv.kind = VKind::Object;
                pv.obj = std::make_shared<Object>();
                pv.obj->cls = paramCls;
                pv.obj->fields["name"] = vstr(std::string(p.name));
                pv.obj->fields["type"] = vstr(p.type ? p.type->canonical : std::string());
                params.arr->push_back(pv);
            }
            v.obj->fields["params"] = params;
        } else {
            v.obj->fields["type"] = vstr(decl->type ? decl->type->canonical : std::string());
        }
    } else {
        v = vvoid();
    }
    metaCache_[decl] = v;
    return v;
}
```

`metaClassSymbol` (`Rules.cpp:1370-1372`) is just `lookupClassIn(namespaceScope("meta"),
name)` — a namespace-scope lookup, the same primitive used for macro/rule namespace
resolution elsewhere.

The prelude these objects are instances of, `src/Resolver.cpp:2787-2814` (inside
`kPreludeRest`):

```leviathan
namespace meta {
    Ast parseExpr(string source) { throw RuntimeException("meta::parseExpr() is compile-time-only"); }
    Ast parseStmts(string source) { throw RuntimeException("meta::parseStmts() is compile-time-only"); }
    class Param  { string name; string type; }
    class Field  {
        string name; string type;
        Array<string> attrs;
        bool hasAttr(string n) => attrs.contains(n);
    }
    class Method {
        string name; string returnType;
        Array<meta::Param> params;
        Array<string> attrs;
        bool hasAttr(string n) => attrs.contains(n);
        int arity() => params.length();
    }
    class Class {
        string name;
        Array<string> bases;
        Array<meta::Field> fields;
        Array<meta::Method> methods;
        bool hasBase(string n) => bases.contains(n);
    }
}
```

Confirmed by exhaustive grep: no `meta::Attr`, no `attrObjects`, no `meta::Class.attrs`,
no `allFields`, anywhere in `src/`.

### 2.4 Attribute-argument evaluation — the int/float/bool/string gate

`RuleEngine::evalAttrArgs` (`src/Rules.cpp:1155-1233`) — quoted in full here because
every later section either extends or bypasses it:

```cpp
void RuleEngine::evalAttrArgs(AttrUse& a, Symbol* attrClass) {
    std::vector<const Stmt*> fields;
    for (const StmtPtr& m : attrClass->decl->body)
        if (m->kind == StmtKind::Member && !m->callable) fields.push_back(m.get());

    if (a.args.size() > fields.size()) {
        sink_.error(a.span, "'@" + std::string(a.name) + "' takes at most " +
                    std::to_string(fields.size()) + " argument(s); got " +
                    std::to_string(a.args.size()));
        return;
    }

    std::vector<Expr*> mapped(fields.size(), nullptr);
    size_t positional = 0;
    for (const ExprPtr& arg : a.args) {
        size_t fi = fields.size();
        if (arg->argLabel.empty()) {
            fi = positional++;
            if (fi >= fields.size()) return;
        } else {
            for (size_t i = 0; i < fields.size(); ++i)
                if (fields[i]->name == arg->argLabel) { fi = i; break; }
            if (fi == fields.size()) {
                sink_.error(arg->span, "no parameter named '" + std::string(arg->argLabel) + "'");
                return;
            }
            if (mapped[fi]) {
                sink_.error(arg->span, "parameter '" + std::string(arg->argLabel) +
                            "' is bound both positionally and by name");
                return;
            }
        }
        mapped[fi] = arg.get();
    }

    AttrValue values;
    for (size_t i = 0; i < fields.size(); ++i) {
        const Stmt* f = fields[i];
        Value v;
        if (mapped[i]) {
            std::string err; bool failed = false;
            v = evalComptimeAt(mapped[i], err, failed);
            if (failed) { sink_.error(mapped[i]->span, "attribute argument is not comptime-evaluable: " + err); return; }
            const std::string& canon = f->type ? f->type->canonical : std::string();
            bool okType = (canon == "int"    && v.kind == VKind::Int) ||
                          (canon == "float"  && v.kind == VKind::Float) ||
                          (canon == "bool"   && v.kind == VKind::Bool) ||
                          (canon == "string" && v.kind == VKind::String);
            if (!okType) {
                sink_.error(mapped[i]->span, "'@" + std::string(a.name) + "' field '" + std::string(f->name) +
                            "' is " + (canon.empty() ? "?" : canon) + "; got '" + valueToString(v) + "'");
                return;
            }
        } else if (f->init) {
            std::string err; bool failed = false;
            v = evalComptimeAt(f->init.get(), err, failed);
            if (failed) v = vvoid();
        } else {
            sink_.error(a.span, "'@" + std::string(a.name) + "' is missing '" + std::string(f->name) +
                        "' (field " + std::to_string(i + 1) + ", no default)");
            return;
        }
        values.push_back({std::string(f->name), v});
    }
    attrValues_[&a] = std::move(values);
}
```

Guarded on the declaration side by `validateAttributeDecl` (`Rules.cpp:1062-1071`):

```cpp
void RuleEngine::validateAttributeDecl(Stmt* cls) {
    for (const StmtPtr& m : cls->body) {
        if (m->kind != StmtKind::Member || m->callable) continue;
        const std::string& canon = m->type ? m->type->canonical : std::string();
        if (canon != "int" && canon != "string" && canon != "bool" && canon != "float")
            sink_.error(m->span, "attribute field '" + std::string(m->name) +
                        "' must be int, float, bool, or string (got '" + (canon.empty() ? "?" : canon) + "')");
    }
}
```

An `attribute Name { fields }` declaration is parsed as an ordinary `StmtKind::Class`
with `isAttribute = true` (`Parser.cpp:1314-1341`) — fields ARE the argument surface,
methods/ctors are rejected at parse time. Field defaults are ordinary field initializers
(`Stmt::init`, `Parser.cpp:1212`), not the newer `Param::defaultValue` machinery.

The storage type (`src/Rules.hpp:54-55, 93`):

```cpp
using AttrValue = std::vector<std::pair<std::string, Value>>;
std::map<const AttrUse*, AttrValue> attrValues_;
```

Two facts about `Value` that matter a lot for §§3 and 9: `Value` (`src/RuntimeValue.hpp:37-54`)
already carries a `VKind::Object` case with a `shared_ptr<Object>` (`Object { Symbol*
cls; unordered_map<string, Value> fields; }`, `RuntimeValue.hpp:56-59`) — so **storing a
structured reflection object as an attribute's evaluated value requires no new `Value`
variant at all**; it's the exact same shape `buildMetaValue` already produces for
`meta::Field`/`meta::Method`/`meta::Class`. And `attrValues_` is keyed by `&a` — the raw
address of the live `AttrUse` still sitting in `decl->attrs`, not a copy — a pattern
already relied on elsewhere (`tryMatch`, `Rules.cpp:1493-1507`, looks values up by the
same address). §3.3 leans on this directly.

### 2.5 The attribute *binding* (already unified on `Value` since Phase 3)

When a rule's `match @Attr(bind)` clause captures an attribute, `tryMatch`
(`Rules.cpp:1493-1507`) builds a synthetic `Object` of the attribute's *own* class,
populated field-by-field from `attrValues_`:

```cpp
if (!m.attrBind.empty()) {
    auto it = attrValues_.find(found);
    if (it != attrValues_.end()) {
        Binding b;
        b.hasVal = true;
        b.val.kind = VKind::Object;
        b.val.obj = std::make_shared<Object>();
        b.val.obj->cls = found->resolved;
        for (const auto& [fname, fval] : it->second) b.val.obj->fields[fname] = fval;
        out[m.attrBind] = b;
    }
}
```

This is why `$r.method`/`where r.method == "GET"` already works today (`r` is bound to an
`Object` of the `Route` class) — but it only works when a rule *structurally matches*
that specific attribute (`match @Route(r) on ...`). It does not help a rule matched on
something else (a class, in the ORM case) that wants to read a *different* attribute's
values off a `$for`-iterated field — that's exactly item A's gap, and it's a gap in
`buildMetaValue`, not in this binding path.

---

## 3. Item A — attribute-value reflection (`meta::Attr`)

### 3.1 The gap, concretely

`tests/corpus/meta/rule_orm.ext` (current, full):

```leviathan
// §16.5 Phase 3 §5: $for list splices, adapted from proposal §10.2 (an ORM
// column map) to the actual meta.* surface (§3): attribute VALUES aren't
// reflected (attrs are names only, by design — see meta::Field), so columns
// are named by the field itself rather than an @Column("alias") override.
namespace Orm {
    attribute Table { string name; }
    attribute Column { }
    rule buildSchema {
        match @Table(t) on class C
        inject `Array<string> schema() =>
            [ $t.name, $for f in C.fields.where((x) => x.hasAttr("Column")) : $f.name ];`
               at member of C
    }
}
uses Orm;

@Table("users")
class User {
    @Column int id;
    @Column string name;
    int internalCounter;
}
```

The `attribute Column {}` has **zero fields** — the corpus deliberately can't demonstrate
`@Column("full_name")` because there is nowhere for the rule to read that value from once
`$for f in C.fields` is iterating. This is exactly `techdesign-metaprog-phase3.md`'s own
landed-note (§5): *"the proposal wrote `f.attr(Column).name`... but §3's deliberately-
minimal `meta.*` surface reflects attribute NAMES only... The landed `rule_orm` corpus
test... uses `hasAttr("Column")` and names columns by field name."* The request's
acceptance criterion 1 is literally "make this corpus test byte-different in the way it
couldn't be before" — extend `Column` with a `name` field and prove
`@Column("full_name")` flows into `schema()`.

### 3.2 Design space: two prior sketches, and why one wins

**Sketch 1 (the request's own, `request-metaprog-attr-values.md:20-22`):**
```
class meta::Attr { string name; string argStr(int i); int argInt(int i); }
Array<meta::Attr> attributes;   meta::Attr? attr(string name);
```
Positional accessor methods, dodging the "how do you store heterogeneous per-arg values
in a statically-typed field" problem by pushing it into method dispatch.

**Sketch 2 (`techdesign-metaprog-phase4.md` §9.1, item I's own recorded resolution):**
```
meta::Attr { name : string; fields : Array<meta::AttrField> }
```
A name + an array of per-field value wrappers, explicitly deferring the wrapper's own
shape ("Additive" is as far as it commits).

Both sketches under-specify the one real question: **what is `meta::AttrField`'s type**,
given attribute field values are `int | float | bool | string` and Leviathan's static
type system has no field that can hold "one of four different primitive kinds" without
either a union type or a tagged-variant class.

Two mechanisms exist in the compiler for exactly this shape — pick the second:

- **A closed union type** (`docs/reference.md:157,180-193`, real, shipped): `int | float |
  bool | string` is a legal field type today. But every read requires narrowing (`is T` /
  `!= None` flow-typing) before touching the value — usable, but the *caller* (the rule
  author writing a template hole) would need to narrow inline in a `$for`/`where`
  expression, which is exactly the ergonomic cost the request's own `argStr(i)`/`argInt(i)`
  sketch exists to avoid.
- **A tagged-variant class with typed `T?`-returning accessors** — this is not
  hypothetical, it is the **exact shape the prelude already uses** for `JsonValue`
  (`Resolver.cpp:4965-4992`, chosen specifically because "the language has no type
  aliases, so a recursive named union cannot be declared" — `docs/reference.md:2235`):
  ```leviathan
  class JsonValue {
      int kind = 0;
      bool b = false; float num = 0.0; string str = "";
      Array<JsonValue> items = []; Map<string, JsonValue> fields;
      bool?   asBool()   => kind == 1 ? b : None;
      float?  asNum()    => kind == 2 ? num : None;
      string? asStr()    => kind == 3 ? str : None;
      ...
  }
  ```
  This needs **zero engine-native call interception** — it's ordinary interpreted
  Leviathan code, dispatched by the existing `findMethod`/`callFunction` path the same way
  `Field.hasAttr`/`Method.arity` already are. (Contrast: `meta::parseExpr`/`parseStmts`
  *do* need a native intercept — `Evaluator::evalCall`, `Eval.cpp:1204-1216` — but only
  because they need genuine host behavior, parsing a string. Reading an already-computed
  `Value` out of a `unordered_map<string,Value>` needs none of that; `buildMetaValue`
  already has the `Value` in hand, in C++, at construction time.)

**Recommendation: `meta::AttrField` as a `JsonValue`-shaped tagged variant**, and
`meta::Attr.argStr(i)`/`argInt(i)`/`argFloat(i)`/`argBool(i)` as **native intercepted
methods** (not interpreted Leviathan bodies) for the *positional* convenience API the
request explicitly asks for (§9.4's `argStr(0) ?? f.name` phrasing), backed by the
`AttrField` array for the general/structural case (`$for af in a.fields : af.name`). Two
surfaces, one storage — see §3.3. The native-intercept choice for the accessor methods
(rather than writing them as interpreted Leviathan bodies over the tagged-variant array)
is because `argStr(int i)` needs a **type-mismatch diagnostic** ("`@Column`'s argument 0
is `int`, not `string` — did you mean `argInt(0)`?", satisfying the request's own "well-
defined" bar for acceptance criterion 2) — an interpreted body can only return `None`,
losing the distinction between "out of range" and "wrong kind." A native intercept can
`sink_.error` with the exact message. (This mirrors the `meta::parseExpr` precedent's own
justification: native intercepts exist for cases needing capability the interpreter
doesn't have — here, a *diagnostic*, not host I/O — not merely as a shortcut.)

### 3.3 Recommended shape

Prelude additions (`Resolver.cpp`, inside the existing `namespace meta { ... }` block,
`Resolver.cpp:2787-2814`):

```leviathan
namespace meta {
    class AttrField {
        string name;
        int kind = 0;              // 1=int 2=float 3=bool 4=string (JsonValue precedent)
        int    i = 0;
        float  f = 0.0;
        bool   b = false;
        string s = "";
        int?    asInt()   => kind == 1 ? i : None;
        float?  asFloat() => kind == 2 ? f : None;
        bool?   asBool()  => kind == 3 ? b : None;
        string? asStr()   => kind == 4 ? s : None;
    }
    class Attr {
        string name;
        Array<meta::AttrField> fields;
        meta::AttrField? field(string n) => fields.firstOrNone((x) => x.name == n);
        // Positional convenience — native intercepts, see below; declared here so
        // `$for`/`where` code and templates see ordinary method-call syntax.
        string argStr(int i);
        int    argInt(int i);
        float  argFloat(int i);
        bool   argBool(int i);
    }
    class Field {
        string name; string type;
        Array<string> attrs;                       // unchanged — keep for back-compat
        Array<meta::Attr> attributes;               // NEW
        bool hasAttr(string n) => attrs.contains(n);
        meta::Attr? attr(string n) => attributes.firstOrNone((x) => x.name == n);
    }
    class Method {
        // symmetric addition: attrs/attributes/attr(), same shape as Field
        ...
    }
}
```

(`firstOrNone`/an `Array<T>.firstOrNone((T)=>bool) => T?` helper may not exist yet in the
`Array<T>` prelude surface — check `Resolver.cpp`'s `Array<T>` block before assuming it;
if absent, either add it as a small independent prelude method (broadly useful, not
attribute-specific) or spell `attr(n)`/`field(n)` as an explicit loop.)

`buildMetaValue`'s `StmtKind::Member` branch (§2.3) grows one loop, parallel to the
existing `attrs` array build, immediately after it:

```cpp
Value attrObjs; attrObjs.kind = VKind::Array;
attrObjs.arr = std::make_shared<std::vector<Value>>();
for (const AttrUse& a : decl->attrs) {
    if (!a.resolved) continue;
    auto it = attrValues_.find(&a);
    if (it == attrValues_.end()) continue;         // e.g. resolution failed upstream
    Symbol* attrCls = metaClassSymbol("Attr");
    Symbol* fieldCls = metaClassSymbol("AttrField");
    Value av; av.kind = VKind::Object;
    av.obj = std::make_shared<Object>();
    av.obj->cls = attrCls;
    av.obj->fields["name"] = vstr(std::string(a.resolved->name));
    Value fieldsArr; fieldsArr.kind = VKind::Array;
    fieldsArr.arr = std::make_shared<std::vector<Value>>();
    for (const auto& [fname, fval] : it->second) {
        Value fv; fv.kind = VKind::Object;
        fv.obj = std::make_shared<Object>();
        fv.obj->cls = fieldCls;
        fv.obj->fields["name"] = vstr(fname);
        switch (fval.kind) {
            case VKind::Int:    fv.obj->fields["kind"] = vint(1); fv.obj->fields["i"] = fval; break;
            case VKind::Float:  fv.obj->fields["kind"] = vint(2); fv.obj->fields["f"] = fval; break;
            case VKind::Bool:   fv.obj->fields["kind"] = vint(3); fv.obj->fields["b"] = fval; break;
            case VKind::String: fv.obj->fields["kind"] = vint(4); fv.obj->fields["s"] = fval; break;
            default: break;   // §9 grows this switch for memberref/classref kinds
        }
        fieldsArr.arr->push_back(fv);
    }
    av.obj->fields["fields"] = fieldsArr;
    attrObjs.arr->push_back(av);
}
v.obj->fields["attributes"] = attrObjs;
```

(The `switch` intentionally leaves the memberref/classref cases (§9) as a documented `//
§9 grows this switch` seam — see §9.7's cross-reference and §10's "build A with G in
view" note. Getting `kind` numbering right the first time avoids a breaking renumber
later.)

`argStr(int i)`/`argInt(int i)`/etc. as native intercepts: same mechanism family as
`meta::parseExpr` (§2.3/`Eval.cpp:1204-1216`) — a shape check in `Evaluator::evalCall`
gated on `comptime_`, keyed this time on **the callee resolving to a `meta::Attr`
instance method** (receiver's `Object::cls->name == "Attr"` and
`callee->text` ∈ {argStr,argInt,argFloat,argBool}), reading `receiver->fields["fields"]`
positionally and either returning the typed value or `sink_.error`ing "'@Column' argument
0 is `string`, not `int` — use argStr(0)" / "'@Column' has only N argument(s); index I is
out of range." Both diagnostics are new (§3.5).

### 3.4 Acceptance-criteria walkthrough

Acceptance criterion 1 (`request-metaprog-attr-values.md:46-47`) — extend `rule_orm.ext`:

```leviathan
namespace Orm {
    attribute Table { string name; }
    attribute Column { string name = ""; }        // NOW takes a value
    rule buildSchema {
        match @Table(t) on class C
        inject `Array<string> schema() =>
            [ $t.name, $for f in C.fields.where((x) => x.hasAttr("Column")) :
                ($f.attr("Column").argStr(0) ?? $f.name) ];`
               at member of C
    }
}
...
@Table("users")
class User {
    @Column("full_name") string name;
    @Column int id;                                 // bare — falls back
    int internalCounter;
}
```

Bare `@Column` (criterion 2) already "just works" through the *existing*
`evalAttrArgs`/field-initializer-default path (`Rules.cpp:1220-1223`, unchanged by this
item) — `Column.name` defaults to `""`, so `argStr(0)` returns `""`, and `?? $f.name`
(the request's own suggested spelling, `request-metaprog-attr-values.md:25`) falls back
to the field name. Whether `argStr(0)` on a value that IS present but empty-string
returns `""` (falsy only via `??`'s None-check, not empty-string-check) or whether
`Column.name` should instead default via `None`-typed `string?` is exactly the
"`None`-or-the-default, P4-owner's-call" open point criterion 2 flags — see §14.

Criterion 3 (`--expand` shows reified values) is free: reification of `$f.attr(...)`'s
result is an ordinary hole-splice through the existing `reify()`/`cloneExpr` path (§2.5's
`Object`-Value pattern already reifies through `$r.method`-style holes today); nothing
new needed there.

### 3.5 New diagnostics

| Trigger | Suggested code |
|---|---|
| `argStr`/`argInt`/`argFloat`/`argBool` called with an out-of-range index | M36 |
| `argStr`/etc. called on an argument of the wrong kind (names the right accessor) | M37 |

(M36 is free — see §10.2. Both are checked at comptime-evaluation time, same failure
class as the existing "attribute argument is not comptime-evaluable" `Rules.cpp:1203`
message, not new parse/collection-time statics.)

---

## 4. Item B — statement-position `$for`

Fully designed already, phase4 §9.2 — nothing to re-derive. Current state, verified: only
the **expression**-position form exists.

- `ExprKind::ForSplice` (`Ast.hpp:126-128`) — consumed by `cloneArrayElements`
  (`Rules.cpp:1935-1969`), array-literal-element position only.
- `StmtKind` (`Ast.hpp:237-252`) has **no** `ForSplice`/`$for`-shaped member — only the
  ordinary runtime `For`/`ForIn` loop statements.

Phase4's design (repeated here only as a pointer, not re-derived): a new
`StmtKind::ForSplice`, parsed inside statement fragments (`parseStmtsFragment`) when a
statement begins `$for <id> in <expr> :`, expanded in `cloneStmt`'s statement-list walk
the same way `cloneArrayElements` expands the array form (iterator via
`materializeBindings`, M21 on non-array, per-item extended bindings). Bounded on purpose
— no `$if`/`$while` follows it; the array form remains the only other splice shape.

Driver example from the request (`request-metaprog-attr-values.md:33-35`): a
statement-position `$for` in a `ctor`/`body` template lets serializer codegen write
direct member statements (`toJson`/`applyRow`) instead of building an array and
interpreting it. Worth noting this item's value **compounds with item A**: a
`toJson`-style rule wants both "for every field with `@Json`" (already possible via the
existing array-literal `$for`) **and** "emit one assignment statement per field, using
`@Json`'s alias" (needs both A and B together) — so if the design doc sequences A before
B (per the request's own priority order), the first *real* consumer of B likely also
exercises A in the same corpus file, which argues for one combined corpus test rather
than two independent ones.

---

## 5. Item C — named attribute arguments — LANDED

**No design work needed.** Full verification against current source:

- General named arguments: `designs/complete/techdesign-named-arguments.md`, "Status:
  implemented, verified, and archived. Date: 2026-07-10." Call-site spelling is `label:
  expr` (not `label = expr` — `=` is declaration-default-only); positional args must
  precede named ones.
- Attribute-specific binding: `RuleEngine::evalAttrArgs` (§2.4, quoted in full above)
  already resolves a mix of positional and named args against an attribute's declared
  field list, by name, with:
  - unknown-name error: `"no parameter named 'X'"` (`Rules.cpp:1181-1183`)
  - double-bound error: `"parameter 'X' is bound both positionally and by name"`
    (`Rules.cpp:1186-1188`)
  - field-initializer defaults for unfilled fields (`Rules.cpp:1220-1223`)
- Corpus: `tests/corpus/meta/attr_named.lev` + `.expected` — exercises reordering
  (`@Route(path: "/users", method: "POST")` even though `method` is declared first) and
  an all-defaulted case (`@Route(path: "/users")`), oracle-verified (`GET
  /users`/`POST /users`).

**One important nuance for the design doc, not a gap in coverage but a correction to a
prior doc's framing:** `evalAttrArgs` is a **hand-rolled, independent** binder — its own
loop, its own `mapped[]` vector, its own diagnostic strings. It does *not* call into the
Checker's general-purpose named-arg resolver (`Checker.cpp:2551-2561`,
`normalizeCallArgs`). What's actually shared between ordinary calls and attribute uses is
only the **parser** (`parseArgs`, `Parser.cpp:308-331`, called from both `parseAttrUses`
at `Parser.cpp:1306` and ordinary call parsing at `Parser.cpp:628`) — `Expr::argLabel`
gets populated identically either way, but binding-to-declaration is two separate C++
implementations that happen to agree on diagnostic wording. This matters if item G
touches `evalAttrArgs` — you're editing the attribute-specific binder, not a shared
resolver that also affects ordinary function calls.

**Residual (optional, not blocking):** no existing corpus case exercises a *mix* of
positional and named args in the same attribute use (`attr_named.lev`'s two uses are both
100%-named). If the design doc wants full belt-and-suspenders coverage before building on
top of this (item G will), a one-line corpus addition (`@Route("/x", method: "POST")`)
closes that gap cheaply. `docs/reference.md` also has no worked example of
`@Attr(field: value)` specifically (see §11).

---

## 6. Item D — Layer D `rewrites`/`replace`/`$body` — LANDED

**No design work needed; do not re-litigate.** Shipped 2026-07-10 (`techdesign-metaprog-
phase4.md` §14, four days after this request was filed), verified in the current tree:

- Grammar: `rule N rewrites body of <bind> { match ... replace \`...\` }` —
  `Parser.cpp:1368-1380` (header), `Parser.cpp:1393-1408, 1521-1535` (`replace`/`inject`
  XOR check).
- `AnchorKind::BodyReplace` — `Ast.hpp:294-296`.
- Static validation (M32/M35) — `validateRewriteRules`, `Rules.cpp:709-745`.
- Runtime expansion + `$body` splice (statement and expression position) —
  `Rules.cpp:1774-1809`, `1991-2008`, `2016-2032`.
- Confluence (M33) for two `replace`s on one body — `Rules.cpp:1789-1801`.
- `reentrant` fixpoint + M34 — parses (`Parser.cpp:1364`); documented,
  `docs/reference.md:2139-2143`.
- Documented end-to-end: `docs/reference.md:2107-2145` ("Body-replacing rules (Layer D)"),
  with the exact `@Timed`-shaped worked example the original proposal used.

The request's item (D) exists on the ticket only "so its priority relative to (A) is on
record" (`request-metaprog-attr-values.md:40-43`) — it was never asking for new design
work, and now there's nothing left to prioritize; it shipped ahead of everything else on
this ticket.

---

## 7. Item E — inherited/mixin field visibility (`C.allFields`)

### 7.1 The gap is in `buildMetaValue`, not in the resolver

Restating §2.2/§2.3's punchline: `Shape::slots` is *already* the flattened,
inheritance-inclusive field+method table, built once by the resolver and consumed by
every backend for real object layout. `buildMetaValue`'s `StmtKind::Class` branch
(`Rules.cpp:1402`) ignores it entirely and walks `decl->body` (the class's own AST
member list) instead:

```cpp
for (const StmtPtr& m : decl->body) {          // OWN body only — the gap
    if (m->kind != StmtKind::Member) continue;
    if (m->callable) methods.arr->push_back(buildMetaValue(m.get()));
    else fields.arr->push_back(buildMetaValue(m.get()));
}
```

For `class User : DbTracking, Timestamps`, `Timestamps.createdAt` is a real key in
every `User` instance's runtime `Object::fields` map (`Evaluator::initFields`,
`Eval.cpp:339-344`) but invisible to `C.fields` in a `$for`/`where` expression — exactly
the request's own description (`request-metaprog-attr-values.md:61-67`).

### 7.2 The request's own two options, evaluated

> "either `C.fields` includes inherited fields (flagged), or a separate `C.allFields`"

- **Change `fields`'s meaning** to be inheritance-inclusive: breaks nothing in the
  *existing* corpus (`rule_orm.ext`'s `User` has no base classes, so its behavior is
  unaffected), but it's a **silent semantic widening** of a field the phase3 design doc
  explicitly called "a forever-API" (`techdesign-metaprog-phase3.md:143`) — any *future*
  rule relying on "own fields only" (e.g. a rule generating a base-class-agnostic
  mixin-contribution registry, which needs exactly the own/inherited distinction) would
  lose the ability to ask the narrower question. Given the forever-API framing this
  project applies everywhere else in `meta.*` (§3.2, §8), silently repurposing an
  existing field is out of character for the rest of this design family.
- **Add `allFields`/`allMethods`** — additive, keeps `fields`/`methods` meaning "this
  class's own declaration" (useful on its own — e.g. a rule that wants to know what a
  class *itself* contributes, as opposed to what it has), and gives the mixin-visibility
  ask its own explicit name. **Recommended** — matches the additive-only discipline every
  other item in this ticket follows (A, F both add rather than repurpose).

### 7.3 Mechanism — reuse `buildMetaValue`'s own per-member branch, unchanged

Because `Slot::decl` (`Symbols.hpp:44`) already points at the original member `Stmt*`,
and `buildMetaValue`'s `StmtKind::Member` branch already turns any such `Stmt*` into a
`meta::Field`/`meta::Method` object (§2.3), the new code is a second loop over
`cls->shape.slots` instead of `decl->body`, calling the *same* function:

```cpp
// New: `RuleEngine::buildMetaValue` needs access to the class's resolved Symbol*, not
// just its Stmt*, to reach shape.slots. `DeclInfo::sym` already carries this
// (Rules.cpp:1286-1287) for the *subject* binding path; buildMetaValue is currently
// keyed only on Stmt* (materializeBindings calls buildMetaValue(bnd.declStmt)), so
// either (a) buildMetaValue grows an overload/parameter taking the Symbol* too (bindings
// already have declSym alongside declStmt, Rules.hpp:187 — free to plumb through), or
// (b) a lookup from Stmt* -> Symbol* is added (decls_ already holds this pairing,
// Rules.cpp:1286-1287, currently built for a different purpose and not indexed for
// point lookup — indexing it is the one small addition this item needs).
Value allFields; allFields.kind = VKind::Array;
allFields.arr = std::make_shared<std::vector<Value>>();
Value allMethods; allMethods.kind = VKind::Array;
allMethods.arr = std::make_shared<std::vector<Value>>();
for (const Slot& s : cls->shape.slots) {
    if (!s.decl) continue;                      // synthesized slots (rare) skip cleanly
    if (s.isMethod) allMethods.arr->push_back(buildMetaValue(const_cast<Stmt*>(s.decl)));
    else            allFields.arr->push_back(buildMetaValue(const_cast<Stmt*>(s.decl)));
}
v.obj->fields["allFields"] = allFields;
v.obj->fields["allMethods"] = allMethods;
```

`buildMetaValue`'s existing per-`Stmt*` cache (`metaCache_`) means a field declared once
and inherited by three subclasses is reflected as three *separate* `meta::Field` Values
that happen to wrap the same `Stmt*` and hit the cache — cheap, and each occurrence
belongs to a different owning class's `allFields` array, which is correct (the same
`Timestamps.createdAt` decl legitimately appears once per class that mixes `Timestamps`
in).

**Open sub-question (flag for the design doc, §14):** should a `meta::Field` gain a
`declaringType : string` (from `Slot::source->name`, already tracked) so a rule
consuming `C.allFields` can tell "this field came from `Timestamps`, not `User` itself"?
`Slot::source` (`Symbols.hpp:44`) already carries exactly this. Cheap to add alongside
`allFields`, and directly useful (a serializer rule might want to skip mixin-contributed
audit columns, or namespace-prefix them) — but it's new API surface on `meta::Field`
itself (affects the non-inherited case too, where `source == the class itself`), so it's
a real decision, not a free addition.

**`distinct`-collapsed collisions:** two same-name+type inherited fields collapse to one
`Slot` by default (`mergeSlot`, `Resolver.cpp:6142-6144`); a `distinct` field keeps both
as separately-qualified slots. `cls->shape.slots` already reflects this correctly (each
`distinct` slot is its own entry with its own `Slot::source`), so `allFields` inherits
this behavior for free — no special-casing needed, just document it (a rule iterating
`allFields` sees exactly the same field set a hand-written `this.Timestamps::x` access
would resolve against).

---

## 8. Item F — `meta::Class.attrs`

The smallest item on the ticket. `meta::Field`/`meta::Method` already have `Array<string>
attrs` + `hasAttr(string)`, built from `decl->attrs` (§2.3's existing loop, lines
1423-1424). `meta::Class` has neither — not because classes can't carry attributes
(`docs/reference.md:1965`: "Attributes attach to classes, structs, interfaces, members,
functions, globals, and namespaces" — `Stmt::attrs`, `Ast.hpp:319`, is on the generic
`Stmt`, not scoped to `StmtKind::Member`), purely because `buildMetaValue`'s `StmtKind::Class`
branch never added the loop.

Mechanism: copy the exact four lines from the `StmtKind::Member` branch into the
`StmtKind::Class` branch:

```cpp
Value attrs; attrs.kind = VKind::Array;
attrs.arr = std::make_shared<std::vector<Value>>();
for (const AttrUse& a : decl->attrs)
    if (a.resolved) attrs.arr->push_back(vstr(std::string(a.resolved->name)));
v.obj->fields["attrs"] = attrs;
```

Prelude: add `Array<string> attrs; bool hasAttr(string n) => attrs.contains(n);` to
`meta::Class` (`Resolver.cpp:2808-2813`), matching `Field`/`Method`'s existing spelling
exactly for symmetry (the request's own framing, `request-metaprog-attr-values.md:68`:
"small symmetry gap").

If item A (§3) lands first, `meta::Class` should get **both** `attrs` (names) and
`attributes` (`Array<meta::Attr>`, values) at once, for the same reason `Field`/`Method`
get both there — no reason for `Class` to lag the other two reflection classes on the
same axis. Sequence F right after A rather than independently (§13).

---

## 9. Item G — member references + pipes as attribute argument values

The one genuine design problem in this ticket. Read §2.1 and §2.4 first — this section
builds on both directly.

### 9.1 Motivation, already load-bearing elsewhere in the codebase

This isn't a hypothetical future need — Atlantis's own ORM design is already blocked on
it, with the interim spelling named explicitly as a placeholder to be deleted:

- `designs/atlantis/techdesign-06-orm.md:546-550` (`@HasMany`/`@BelongsTo`): *"Entity
  names are strings v1 (LA-27 upgrades to checked member references; attributes swap
  field types, consumers unchanged)."*
- `designs/atlantis/techdesign-06-orm.md:565-572` (`@ManyToMany`, the exact shape this
  ticket's item G targets): *"`@ManyToMany(through: "App::Models::Relationship", a:
  "user1", b: "user2")`... String member names v1; LA-27's `self:`-pipe spelling replaces
  them verbatim when it lands."*
- The owner's own worked example, `designs/atlantis/example/Models/User.lev:13-18`,
  carries the semantic ruling as a comment: *"self marks both sides of the pipe as
  'match against the owning instance.' Since a pipe only ever has two members... the
  return column falls out mechanically — whichever one isn't self — without needing a
  separate `symmetric:true` toggle."*

So this item isn't "add a feature nobody's waiting on" — it's "delete a string-typed
placeholder a sibling design already committed to deleting."

### 9.2 Why this is hard: the landmine, demonstrated

Nothing here is a parser problem. Verified directly:

- `parseArgs` (`Parser.cpp:308-331`, shared by ordinary calls and `@Attr(...)`, §2.4)
  parses each argument as a full `parseExpr(0)` — so `App::Relationship::user1` and
  `App::Relationship::user1 | App::Relationship::user2` **already parse** as attribute
  arguments today, with zero grammar changes.
- `A::B::c` parses as nested `Member` nodes (`Parser.cpp:604-624`, `Expr::colon` marking
  `::` vs `.`, `Ast.hpp:151`) with no segment-count cap.
- `|` is an ordinary binary operator (`Token.hpp:98`, `Parser.cpp:49-77` precedence tier
  5) producing a plain `ExprKind::Binary` node — no special casing, parses two
  member-reference chains either side without complaint.

The problem is **semantic**, and it's a genuine correctness landmine today, not just a
missing feature:

1. `evalAttrArgs` evaluates every argument through `evalComptimeAt` →
   `Evaluator::evalComptime` — an untyped tree-walker.
2. `eval()`'s `Member` case requires the base to evaluate to a live `VKind::Object`
   instance (`memberTarget`, `Eval.cpp:947-965`). `App::Relationship::user1`'s base,
   `App`, is a *namespace* — evaluating a bare namespace `Name` produces nothing
   Object-shaped, so `memberTarget` fails and `eval()` falls through to `return
   vvoid();` (`Eval.cpp:1461`).
3. Critically, **this does not set `failed = true`** (`finishComptime`, `Eval.cpp:553-576`,
   only fails on a thrown exception or budget exhaustion) — so `evalComptimeAt` reports
   success with `Value{kind=VKind::Void}` for each operand.
4. `evalBinary` → `arithPrim(TokenKind::Pipe, l, r)` (`RuntimeValue.hpp:389-494`) falls to
   its default integer path since neither operand is String/Float/Bool/Char/Block/None.
   `Value::i` defaults to `0` for every kind (`RuntimeValue.hpp:37-53`), so **`Void | Void`
   silently folds to `vint(0)`** — no crash, no comptime failure.
5. This is only ever caught downstream, and only by accident: `evalAttrArgs`'s `okType`
   gate (§2.4) rejects it *unless the attribute field happens to be declared `int`* — in
   which case **the attribute silently binds the value `0`**, with no diagnostic
   whatsoever, and the rule consuming it proceeds on wrong data.

So the honest framing for the design doc: this item isn't purely additive the way A/B/E/F
are — until it lands, `App::Relationship::user1 | App::Relationship::user2` as an
attribute value is a **silent-wrong** hole (not merely "doesn't work yet"), of exactly
the kind this project's own diagnostics philosophy treats as the worst failure mode
(`techdesign-metaprog-phase3.md §1`, "Gap A... the worst failure mode (silent wrong)").
There's a real argument for prioritizing at minimum a **guard** (loudly reject a
non-primitive-evaluable attribute argument, M20-style, rather than silently folding to
0) ahead of/independent from the full memberref feature — see §14.

### 9.3 Design: `memberref` as a new attribute-field "kind," resolved statically, not via the oracle

The oracle (Evaluator/`evalComptime`) is the wrong tool for this: it's a *runtime*-value
interpreter, and `App::Relationship::user1` doesn't name a runtime value — it names a
*symbol* (a specific field declaration). The Checker already knows how to resolve exactly
this shape (`Checker::typeOfMember`'s namespace-qualified block, `Checker.cpp:1207-1223`,
plus the "static on a type value" fallback, `Checker.cpp:1277-1300`, using `slotsNamed`,
`Checker.cpp:116-121`) — but the Checker never sees attribute arguments (§2.1), and
running it early would be a bigger pipeline change than this item needs.

**Recommendation: `RuleEngine` resolves memberref-shaped attribute arguments directly
against `Sema`, statically, bypassing `evalComptimeAt` entirely for fields of this new
kind.** `RuleEngine` already does namespace/class-symbol resolution of exactly this shape
for unrelated purposes — `namespaceScope(ns)` + `lookupClassIn(...)` is the same pair
`metaClassSymbol` (§2.3) already uses to find `meta::Class`/etc. This is a **small,
self-contained addition**, not a Checker integration:

```cpp
// New. Given an Expr* that is expected to name a class ("classref") or a class
// member ("memberref"), resolve it statically against Sema — no oracle, no runtime
// Value involved. Mirrors Checker::typeOfMember's namespace-qualified + slotsNamed
// path (Checker.cpp:1207-1223, 1277-1300), but returns symbol identity instead of a
// Type, since the design goal is "typo'd column is a compile error naming the class
// and member," not type-checking.
//
// Walks a `Member(colon=true)` / `Name` chain left-to-right:
//   - a bare Name: look up as a namespace, or (failing that) as a class visible in
//     the rule's own scope
//   - Member(base, text) where base resolves to a Namespace: look up `text` in that
//     namespace's scope (may yield a Class, or another Namespace for further nesting)
//   - Member(base, text) where base resolves to a Class: look up `text` among
///    `slotsNamed(cls->shape, text)` (§2.2 — already inheritance-flattened, so a
//     memberref to a mixin-contributed field just works)
// Returns {classSym, memberSlot-or-null}; memberSlot is null for a bare classref.
struct RefResolution { Symbol* cls = nullptr; const Slot* member = nullptr; bool ok = false; };
RefResolution RuleEngine::resolveStaticRef(const Expr* e);
```

`slotsNamed`'s current form is a free function local to `Checker.cpp` (`Checker.cpp:116-121`,
no header declaration) — `RuleEngine` needs either a forward declaration reusing the same
linked symbol, or (simpler, avoids any cross-TU coupling) its own four-line copy in
`Rules.cpp` operating on `Shape`/`Slot` from `Symbols.hpp`, which both files already
include. Trivial either way; not a design decision, just an implementation note.

**Value representation — reuse the `Object`-based reflection pattern (§2.3/§2.4), not a
new `VKind`.** A new `VKind` enum member touches every backend's switch statements
(`Eval.cpp`, `IrInterp.cpp`, `CGen.cpp`, `LlvmGen.cpp`, `RuntimeValue.hpp`'s
`valueToString`, etc.) even though `meta::*` values never reach a backend (they're
consumed entirely inside the metaprogramming stage, pre-Checker). New prelude classes,
mirroring `meta::Field`/`meta::Attr`'s existing pattern:

```leviathan
namespace meta {
    class TypeRef { string name; }                         // a bare class/type reference
    class MemberRef { meta::TypeRef declaringType; string memberName; }
    class MemberRefPair { meta::MemberRef a; meta::MemberRef b; }
}
```

`resolveStaticRef` builds these directly (same `make_shared<Object>()` / `obj->cls =
metaClassSymbol("MemberRef")` pattern already used throughout `buildMetaValue`), no new
`Value` storage kind — `VKind::Object` already suffices, exactly as it does for every
other `meta::*` reflection value.

**Field-type recognition — reuse the existing type name, no new keyword.** An attribute
field typed `meta::TypeRef` or `meta::MemberRef` resolves through the **ordinary** type
resolver (Resolver pass 1, §2.1) like any other field reference to a prelude class — `attribute
HasMany { meta::TypeRef target; string fk = ""; }` requires **no parser change and no new
reserved word**: `meta::TypeRef` is already a legal type name the moment the class exists
in the prelude. The only code that needs to *recognize* it specially is:

1. `validateAttributeDecl` (`Rules.cpp:1062-1071`) — extend the allowed-canon check
   beyond `int/string/bool/float` to also accept `meta::TypeRef`, `meta::MemberRef`,
   `meta::MemberRefPair` (a short, explicit allowlist — not "any class," keeping the
   "attribute fields are the typed argument surface, kept deliberately small" discipline
   `techdesign-metaprogramming.md` states everywhere else in this family).
2. `evalAttrArgs` (`Rules.cpp:1155-1233`) — branch on `f`'s canonical type *before*
   calling `evalComptimeAt`: for `meta::TypeRef`/`meta::MemberRef`/`meta::MemberRefPair`
   fields, call `resolveStaticRef` (or its pipe-aware sibling, §9.4) instead, producing an
   `Object`-kind `Value` directly rather than going through the oracle at all. This is a
   clean `if`/`else if` added to the existing type-dispatch in that function — not a
   rewrite of it.

### 9.4 The pipe shape

For a field typed `meta::MemberRefPair`, `evalAttrArgs`'s new branch requires the raw
`Expr*` to be shaped exactly `Binary(op=Pipe, a, b)` — anything else (a single ref, an
array literal, a different operator) is a new diagnostic (§9.7). Both `a` and `b` are
independently resolved via `resolveStaticRef`; if either fails to resolve to a class
member, the existing "typo'd column is a compile error" property (the entire point of
this item, `request-metaprog-attr-values.md:86-87`) falls out automatically — the error
names the specific unresolvable segment, not the whole pipe expression.

Whether `MemberRefPair` should generalize to `Array<meta::MemberRef>` of arbitrary length
(pipe as sugar for a 2-element array) or stay a fixed two-field pair is a real open
question (§14) — the owner's own ruling text is explicit that *this* feature is
deliberately fixed at two ("since a pipe only ever has two members... without needing a
separate `symmetric:true` toggle"), which argues for keeping `MemberRefPair` a dedicated
2-field shape rather than a general array, at least for v1. A dedicated shape also means
`|` stays meaningfully distinct from an array literal in attribute-argument position —
useful if a *different* future attribute field ever wants "a list of member refs" (e.g.
composite-key columns) via ordinary array syntax without colliding with the pipe's
join-matching semantic.

### 9.5 `self:` is not a compiler concept — confirmed, and important

Grep across `src/` for any special-casing of the label `self` (or any named-arg label
treated as a *marker* rather than an ordinary value binding) in `evalAttrArgs`
(`Rules.cpp:1170-1193`) or the Checker's general named-arg binder (`Checker.cpp:2551-2561`)
returns nothing — every named-arg label in the compiler, everywhere, is used exclusively
to look up a matching declared field/parameter **name**. There is no reserved-word or
marker-flag concept anywhere in the binding algorithm, and there shouldn't be one added
for this: **`self` is simply the field name the `hasManyBelongsToMany`/`ManyToMany`
attribute class chooses to declare**, e.g.:

```leviathan
attribute HasManyBelongsToMany {
    meta::TypeRef through;
    meta::MemberRefPair self;
}
```

Once memberref/pair values exist at all (§9.3/§9.4), `self: App::Relationship::user1 |
App::Relationship::user2` is bound by the **already-landed** (§5) named-argument
machinery with zero additional work — `evalAttrArgs` matches the label `self` against
the declared field named `self`, exactly like any other named argument. The "self marks
both sides of the pipe as match-against-owning-instance" semantic (the owner's ruling,
§9.1) is entirely **rule-author interpretation** of the resulting `MemberRefPair` value —
the `ManyToMany`-consuming *rule* reads `$attr.self.a`/`$attr.self.b`, and its own
`where`/template logic decides which side is "self" vs. "projection" (comparing against
the matched class's own identity, itself already reflectable — `C.name`, or the richer
`$C`-decl-identity splice `reference.md:2007-2009` already documents). **Nothing in the
engine needs to know what `self` means** — this significantly shrinks the scope of item
G relative to how the request phrases it: the "three ingredients" the request lists
(`request-metaprog-attr-values.md:83`) are really just two engine-level additions
(named args — done; memberref/pair values — new) plus zero new engine semantics for the
third ("`self:` marks..." is 100% rule-body logic, already expressible once the value
exists).

### 9.6 Relationship to LA-31 expression reification — related, not reusable

`designs/expr-reification/` (landed 2026-07-19, `src/Checker.cpp` +740 lines) reifies a
**lambda body** into a walkable `expr::Node` tree for the ORM's query-lambda DSL (`(u) =>
u.active`). It's worth citing as a *precedent for the philosophy* ("stop naming things
with strings when the compiler can check a real reference") but it is **architecturally
the wrong mechanism to extend for item G**, for a reason worth stating explicitly so a
design doc doesn't waste time trying to unify them:

- LA-31 reification runs **inside the Checker** (`designs/expr-reification/techdesign-00-overview.md`
  ruling R1: "reification happens inside the Checker... requires types... overload
  resolution"), triggered by **target-typing** (a lambda literal in a position typed
  `expr::Expr<F>`). Attribute arguments are resolved **before** the Checker ever runs
  (§2.1) — there is no Checker pass to hook into at that point in the pipeline without
  restructuring the whole rule-stage/Checker ordering, which is far outside this ticket's
  scope.
- LA-31's `Field` node reifies **parameter-rooted member chains inside a checked lambda
  body** (`u.active` where `u` is the lambda's own parameter) — a different shape than a
  **namespace-qualified static reference** (`App::Relationship::user1`, rooted at a
  namespace, not a lambda parameter). The resolution algorithms don't overlap even
  though both "turn a member chain into structured data."
- LA-31's `Bind`/snapshot machinery (rulings R3, R5) exists to handle *captured runtime
  values* inside a lambda closure — entirely inapplicable to a static, compile-time-only
  reference used as an attribute argument.

**What *is* worth reusing in spirit:** LA-31's decision to represent member paths as
structured objects rather than strings, and its precedent that "new reflection-adjacent
prelude classes живут in their own namespace" (`expr::` for LA-31, `meta::` for this
ticket — consistent naming discipline, not a shared implementation).

### 9.7 New diagnostics

| Trigger | Suggested code |
|---|---|
| A `meta::TypeRef`/`meta::MemberRef` field's argument doesn't resolve to a known namespace/class | M38 |
| A `meta::MemberRef` field's argument resolves to a class but not a member of it (the "typo'd column" case — the whole point of this item) | M39 |
| A `meta::MemberRefPair` field's argument isn't shaped `a \| b` | M40 |
| Either side of a `meta::MemberRefPair`'s `\|` fails to resolve (reuses M38/M39, naming the specific side) | (reuse M38/M39) |

All four are collection/evaluation-time errors inside `evalAttrArgs`'s new branch (§9.3),
same severity class and reporting mechanism as the existing "attribute argument is not
comptime-evaluable" (`Rules.cpp:1203-1204`) — no new diagnostic *infrastructure*, just
new messages.

---

## 10. Cross-cutting design considerations

### 10.1 Build item A with item G's future shape in view

Item A's `meta::AttrField.kind` tag (§3.3) is an `int` enum (`1=int 2=float 3=bool
4=string`). If item G ships later, `meta::MemberRef`/`meta::MemberRefPair`-valued
attribute fields need a `kind` value too (so `meta::Attr.field(n)`/`argStr`-family can
report "this argument is a member reference, not a string" instead of silently returning
`None`/erroring unhelpfully). **Reserve `kind` values 5 and 6 now** (`5=memberref,
6=memberrefpair`) even if item G isn't built in the same pass, so `AttrField`'s shape
doesn't need a breaking change later. This is the one place in the whole ticket where
sequencing items independently (as the request's own priority order suggests: A, then
E/F, then B, then G) creates a real coupling risk if not flagged up front — hence calling
it out here rather than leaving it implicit in §3 and §9 separately.

### 10.2 Diagnostic numbering

The `M`-series (M01–M35) is exhaustively confirmed as the ceiling (`grep -rohE '\bM[0-9]{2}\b'
src/ designs/ docs/ tests/` — nothing at or above M36 anywhere in the repo). Two sibling
precedents for *not* continuing the M-series: `target::` needed zero new codes;
LA-31 (expr-reification) started a fresh `E1`/`E2`/`E3` family scoped to its own design
doc. Given this ticket's new diagnostics (§3.5, §9.7) are extensions of the *existing*
`Rules.cpp` attribute/rule-engine machinery (not a self-contained new subsystem the way
LA-31 is), **continuing M36+ is the better fit** — these are the same family of "a rule-
stage static/evaluation check failed" errors M01–M35 already are, in the same file, using
the same `sink_.error` call convention. Recommended allocation, in request-priority order
(final numbers are the design doc's call, this is proposed sequencing):
`M36`/`M37` (item A, §3.5), `M38`–`M40` (item G, §9.7). No new codes needed for B (reuses
M21's "iterator didn't yield an array" family) or E/F (purely additive reflection fields,
no new failure mode — a class with no bases just gets an empty `allFields` delta beyond
`fields`, not an error).

### 10.3 Zero backend impact, confirmed

Every item in this ticket (A, B, E, F, G) is entirely inside the pre-Checker rule stage
(`src/Rules.cpp`/`Rules.hpp`) plus prelude source (`src/Resolver.cpp`'s embedded
Leviathan string literals). None of it touches `Lower.cpp`, `Eval.cpp`'s runtime
(non-comptime) path, `IrInterp.cpp`, `CGen.cpp`, `LlvmGen.cpp`, or `X64Gen.cpp` — `meta.*`
reflection values never survive past the rule stage; they're consumed entirely by
`where`/`$for`/template-hole evaluation and never reach a backend. (Item G's one
Eval.cpp-adjacent touch, §9.2's landmine, is a *diagnostic* fix inside the comptime oracle
path — still pre-Checker, still no backend involvement.) This mirrors exactly Phase 1–4's
own zero-cost guarantee (`hasMeta == false` skips the entire stage) and should be stated
as an explicit acceptance property in the design doc's testing plan, the same way
`techdesign-metaprog-phase4.md §12` states it ("Zero-cost guard... a program with rules
but no [new markers] is unaffected").

### 10.4 Testing conventions to inherit

- **Corpus pairing:** every new positive case gets a **hand-written twin**
  (`rule_orm.ext` + `rule_orm_twin.ext`, byte-identical `.expected`) — the project's
  standing acceptance bar for this whole feature family (`request-metaprog-attr-values.md`
  §2 criterion 1 already asks for this explicitly).
- **File extension:** `rule_orm.ext` is pre-existing `.ext` (grandfathered); any
  genuinely **new** corpus file this ticket adds must be `.lev`
  (`techdesign-metaprogramming-tail.md` §6 problem 4: "the standing hard rule is all new
  source/test files are `.lev`, never `.ext` — even when a design doc says otherwise").
- **`--expand` round-trip:** the existing harness (`tests/run_expand_roundtrip.sh`,
  `techdesign-metaprog-phase4.md §14` item 4) already sweeps every meta corpus file; new
  files are picked up automatically (directory-scanned, no new CMake registration needed
  per the `Triple` research doc's identical finding for ordinary corpus files —
  `docs/research-triple.md` §8.1 — the same auto-discovery applies here).
- **metatests negatives:** one per new M-code (§10.2), in `tests/test_meta.cpp`, matching
  the existing M19–M35 negative-test density.

---

## 11. Documentation gaps worth closing regardless

Independent of which items ship, `docs/reference.md §6.9` (the entire metaprogramming
surface, `docs/reference.md:1947-2177`) has three gaps confirmed by exhaustive grep that
predate this ticket and will only get worse if new `meta::*` classes land without
closing them:

1. **The `meta::*` reflection classes' field/method shapes are never spelled out** —
   `meta::Field`/`meta::Method`/`meta::Class`/`meta::Param` are only referenced by name
   once in passing (`reference.md:2004`); a reader has no documented way to learn `C.fields`
   returns `Array<meta::Field>` or what `hasAttr`/`arity()` do. Any new class this ticket
   adds (`meta::Attr`, `meta::AttrField`, `meta::TypeRef`, `meta::MemberRef`,
   `meta::MemberRefPair`) should land with its shape documented in the *same* pass that
   adds it, plus a backfill for the four that already exist undocumented.
2. **`$for` has no worked example** — mentioned three times in passing, never shown with
   real syntax. Item B (statement-position `$for`) landing without ever having documented
   the array form first would be an odd ordering.
3. **Attribute named args have no `@Attr(field: value)` example** — the general
   named-arguments doc (`reference.md:312-324, 719-748`) covers ordinary calls; the
   attributes subsection (`reference.md:1953-1973`) still shows only positional examples
   (`@Route("GET", "/users")`). Since item C is already landed (§5) this is pure
   documentation debt, cheap to close independent of any of the open items.

---

## 12. File-level change map

| File | Change |
|---|---|
| `src/Resolver.cpp` (prelude, `namespace meta { ... }`, `Resolver.cpp:2787-2814`) | Item A: `meta::AttrField`, `meta::Attr`; `Field`/`Method` gain `attributes`/`attr()`. Item F: `Class` gains `attrs`/`hasAttr()`/`attributes`/`attr()`. Item G: `meta::TypeRef`, `meta::MemberRef`, `meta::MemberRefPair`. Item E: `Class` gains `allFields`/`allMethods`. |
| `src/Rules.cpp` (`buildMetaValue`, `Rules.cpp:1378-1450`) | Item A: build `attributes` array per Field/Method. Item F: build `attrs`/`attributes` for Class. Item E: new `allFields`/`allMethods` loop over `cls->shape.slots`, needs a `Stmt* -> Symbol*` lookup (§7.3) or a `declSym`-threaded overload. |
| `src/Rules.cpp` (`evalAttrArgs`, `validateAttributeDecl`, `Rules.cpp:1062-1233`) | Item G: extend the allowed-canon set; branch to `resolveStaticRef` (new function) for `meta::TypeRef`/`MemberRef`/`MemberRefPair`-typed fields, bypassing `evalComptimeAt` for those. |
| `src/Rules.cpp` (`Evaluator::evalCall`-adjacent, or a new native-intercept table) | Item A: `meta::Attr.argStr/argInt/argFloat/argBool` native intercepts, modeled on the `meta::parseExpr` precedent (`Eval.cpp:1204-1216`, `setComptimeParseHook`-style callback wiring from `Rules.cpp:16-29`). |
| `src/Ast.hpp` | Item B: `StmtKind::ForSplice`. Nothing else in this ticket needs an `Ast.hpp` change — A/E/F/G are all reflection-value/prelude-class additions, no new AST node kinds. |
| `src/Parser.cpp` | Item B only: `$for` in statement-fragment position (`parseStmtsFragment`). Items A/C/D/E/F/G need **zero** parser changes (confirmed §9.2 for G specifically — the grammar already accepts everything needed). |
| `docs/reference.md §6.9` | All items — see §11's backfill list, plus per-item worked examples as each lands. |
| `tests/corpus/meta/rule_orm.ext` + `rule_orm_twin.ext` | Item A's acceptance test (§3.4) — extend in place, per the request's own criterion 1. |
| New `.lev` corpus files | One per item beyond A (B, E, F, G), each with a hand-written twin, per §10.4. |

---

## 13. Suggested sequencing

Following the request's own priority order (A > E/F alongside A > B > G, D already done,
C already done), with the two adjustments this research surfaces:

1. **Item A** (`meta::Attr`/`AttrField`) — the headline, and per §10.1, build its `kind`
   tag with room for item G's future variants even if G isn't scheduled yet.
2. **Item F** (`meta::Class.attrs`/`.attributes`) — trivial, and per §8's closing note,
   cheapest done in the same pass as A (same loop, one more call site).
3. **Item E** (`C.allFields`) — cheap (§7.3: read already-computed data), independent of
   A, no reason to block on it.
4. **Item B** (statement-position `$for`) — already fully designed (phase4 §9.2),
   independent of A/E/F, but pairs naturally with A in a shared corpus test (§4's note).
5. **Item G** (memberref/pipe) — last, both because the request says so and because it
   is the only item requiring genuinely new machinery (`resolveStaticRef`, §9.3) rather
   than additive reads of existing data. Before scheduling it, resolve §9.2's landmine at
   minimum as a standalone guard (loud error instead of silent `0`) — that's a small,
   independently valuable fix that should not wait on the full feature.

Items C and D need no scheduling — they're done.

---

## 14. Open questions for the design doc to rule on

| # | Question | Where raised | This doc's lean, if any |
|---|---|---|---|
| 1 | `meta::Attr`'s positional accessors (`argStr(i)`) as native intercepts, vs. requiring callers to always go through `field(name).asStr()` on the tagged-array surface? | §3.2 | Both — natives for the ergonomic positional case the request explicitly asked for; the array stays as the general/structural escape hatch. |
| 2 | Bare `@Column`'s default (`Column.name = ""`) — is `argStr(0)` on an unset-but-defaulted field `""`, or should the field type be `string?` so it's `None` and `?? f.name` behaves as the request's own sketch implies? | §3.4, request criterion 2 | Leaning `string?`/`None` — matches `JsonValue`'s own `T?`-returning-on-mismatch convention and makes `??` meaningful; `""` is indistinguishable from "explicitly set to empty string." |
| 3 | `meta::Field.declaringType` on `allFields` entries — worth the permanent API surface to distinguish own vs. inherited fields inline, or is `C.hasBase(...)` at the call site sufficient? | §7.3 | Genuinely open; real usefulness (skip mixin-audit-columns) vs. forever-API cost. |
| 4 | `meta::MemberRefPair` as a fixed 2-field shape vs. `Array<meta::MemberRef>` with `\|` as 2-element sugar | §9.4 | Leaning fixed pair — matches the owner's own "a pipe only ever has two members" framing; keeps `\|` meaningfully distinct from array-literal attribute values. |
| 5 | Diagnostic family: continue `M36`+ or start a fresh family like LA-31's `E1`-`E3`? | §10.2 | Leaning M36+ — this is an extension of the existing Rules.cpp family, not a new subsystem. |
| 6 | Item E: repurpose `fields`'s meaning (inheritance-inclusive) vs. additive `allFields`? | §7.2 | Leaning additive — consistent with the forever-API discipline stated everywhere else in this design family. |
| 7 | Should §9.2's silent-`0` landmine be fixed as a standalone guard ahead of the full item G, given it's a live silent-wrong hole today? | §9.2, §13 | Yes — independently valuable, small, and the project's own diagnostics philosophy treats silent-wrong as the worst failure mode. |

---

*Companions: `designs/requests/request-metaprog-attr-values.md` (the ask this document
feeds), `designs/complete/techdesign-metaprog-phase3.md` / `-phase4.md` (the substrate;
phase4 §9.1 is item A/B's own prior — not-yet-built — resolution), `designs/complete/
techdesign-metaprogramming-tail.md` (the closed tracker that re-grounded H–L against this
tree on 2026-07-17), `designs/complete/techdesign-named-arguments.md` (discharges item
C), `designs/expr-reification/` (item G's related-but-distinct precedent, §9.6),
`designs/atlantis/techdesign-06-orm.md` §5.2/§5.4 (item G's real consumer and the
interim spelling it retires), `designs/requests/request-metaprog-splices.md` (the
companion ticket — "where a bound thing may be spliced," vs. this ticket's "what can be
read").*
