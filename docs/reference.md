# Leviathan Language Reference

Formal reference for the language as currently implemented. The design rationale lives in
`info.md`; this document specifies syntax and behavior. Features that are designed but not yet
implemented are marked **[planned]**. The project is named **Leviathan**; the compiler binary is
`leviathan`, and source files use the `.lev` extension by default (`.ext` is still accepted
indefinitely — existing demo/example/corpus fixtures keep it). The package manager / build driver
is a **separate** binary, `trident`, which owns the `trident.toml` manifest and drives the
compiler (§8); see `designs/complete/techdesign-toolchain.md`.

---

## 0. Last Updated

**Reference synced to the implementation on 2026-07-12.**

Most recently landed and documented here: **covariant-return interface satisfaction**
(F6 — invariant parameters and a declared class/interface subtype return, §4.2),
**weak fields** (F5 — field-only optional
class/interface back-references, §4.3d), **true suspension** (LA-30 — `await` parks a
stackful task on oracle/IR/LLVM, §6.6.67) and its **cancellation / structured-concurrency
follow-on** (LA-30 B2 — `CancelledException`, `TaskGroup`, `awaitTimeout`, §6.6.68). Before
that: the **regex public library** (Track 10,
§6.4.6 — `Regex`/`Match`/`Group`/`RegexOptions`/`RegexException` + `namespace regex`
conveniences, fronting the **regex engine core** §6.4.4), **TLS + crypto** (LA-2,
§6.6.5x), **threads / workers** (Track 10, §6.6.66), **comptime
`import()`** (LA-20, §6.9), **named arguments + default parameters** (§4.4/§4.5, "Argument
binding"), **method references** (LA-25, §3.4), **runtime-slot method dispatch with static
devirtualization** (§3.4a), **`char`/`Block`/`enum`** (Track 03, §2.2/§6.10/§4.2c), and
**`namespace math`** (Track 06, §6.1b).

**Backend note (applies throughout this document).** The **pure x86-64 / ELF backend
(`--emit-elf`) is FROZEN and reference-only** as of the portable-backend pivot (2026-07-05);
**ELF is not a project target.** It is retained only as the differential-testing anchor and
the zero-dependency bootstrap seed and is never extended, so every feature that landed after
the pivot carries "no ELF lane" and the per-feature "engine coverage" notes below name the
**four active engines** (oracle, IR, emit-C++, LLVM — LLVM being the primary AOT backend).
See §7.3.

---

## 1. Lexical structure

### 1.1 Comments
```
// line comment
/* block comment */
```

### 1.2 Identifiers
`[A-Za-z_][A-Za-z0-9_]*`. Internal word boundaries capitalize each stem after the first
(`subString`, not `substring`).

### 1.3 Keywords
```
namespace  class  struct  enum  interface  public  private  new  mutating  distinct  const
get  set  return  var  let  await  bind  inject  use  uses
if  else  while  for  in  match  is  this  true  false
break  continue  do  using  try  catch  throw
```
Primitive type names (`int`, `string`, `bool`, `float`, `void`) are **not** keywords; they are
ordinary names resolved to prelude declarations.

### 1.4 Literals
| form | examples | type |
|---|---|---|
| integer | `42`, `0`, `0xFF`, `0b1010` | `int` |
| float | `3.14` | `float` |
| string | `"hello"`, `'hi'` (both quote styles) | `string` |
| boolean | `true`, `false` | `bool` |
| array | `[1, 2, 3]`, `[]`, `[1..5]`, `[1..3, 7]` | `Array<T>` |
| range | `1..10` (inclusive) | `Range` |

A `Range` element inside an array literal **spreads**: `[1..3, 7]` is `[1, 2, 3, 7]`.

**Integer literals** are decimal, hex (`0x`/`0X`), or binary (`0b`/`0B`) — no
octal. `_` separates digits for readability in any base, including a float's
fractional digits (`1_000_000`, `0x1_FF`, `1_000.000_5`); a separator may not
lead, trail, or double up (`0x_1F`, `1000_`, `1__000` are compile errors). A
hex/binary literal is always an int — `0x1.5` (a dot followed by a digit) is
a compile error, not a fractional part; `0xA..0xF` (a dot followed by another
dot) is the `..` range operator applied to two hex ints, left alone. Hex/
binary literals reinterpret their full 64-bit pattern, so `0x8000000000000000`
prints the same as `1 << 63` (§3.5b).

**String escapes** (both quote styles; `'` and `"` are otherwise identical):
`\n` `\t` `\r` `\0` (newline/tab/carriage-return/NUL — strings are byte-clean
through the floor, so an embedded NUL survives, it does not terminate the
string) and `\xNN` (exactly two hex digits → that byte). `\"`/`\\`/`\'` and
any other `\<c>` pass `c` through literally — an unrecognized escape is not a
compile error (compat; `\q` is just `q`). A malformed `\x` (not followed by
exactly two hex digits, e.g. `\x4` or `\xZZ`) is likewise left alone: `x`
passes through and the following character(s) are read as ordinary content,
not consumed as part of a (failed) escape.

**`\u{H+}`** (request-string-literal-tail, 1-6 hex digits) inserts the UTF-8
encoding of that Unicode scalar — the sibling of `\xNN`, reusing `char`'s own
scalar-validity rule: a surrogate (`D800`-`DFFF`) or a codepoint past
`10FFFF` decodes to **U+FFFD** rather than throwing (a compile-time literal
has nothing to catch; this is the same "invalid data never crashes"
replacement policy as UTF-8 decode). A malformed `\u{...}` (no hex digits, or
no closing `}`) is left alone exactly like a malformed `\x`: `u` passes
through and `{...}` is read as ordinary content. `\u{1F600}` in a
single-quoted literal target-typing to `char` (above) works the same as any
other single-scalar literal.

**Raw strings — `r"..."` / `r'...'`** (request-string-literal-tail): a `r`
immediately followed by a quote (no space — that adjacency was never a legal
two-token sequence before, so it is unambiguous) opens a literal where
backslash is an ordinary character. No escape processing and no `${...}`
interpolation happen inside — the entire point of "raw" — so the closing
quote must be reached un-backslash-adjusted; there is no way to embed the
delimiter quote character itself (v1 limit, matched to the intended use:
regex patterns, Windows paths, embedded JSON/HTML fixtures rarely need it).
Single-line only.

**Multiline strings — `"""..."""` / `'''...'''`** (request-string-literal-
tail): a literal delimited by three quote characters may contain raw
newlines and a lone/double embedded quote of the same character (only three
in a row closes it). Ordinary escape processing **and** `${...}` interpolation
both still apply inside — unlike a raw string, this form is the multiline
sibling of an ordinary string literal, not a second raw-string spelling. A
triple-quoted literal never target-types to `char`, even a single-scalar one
— multiline syntax is not char syntax.

**Char literals are target-typed** (Track 03 §1). A single-quoted literal is a
`string` by default (both quote styles lex to string literals), but it **re-types
to `char`** when — and only when — the expected type in context is `char` *and*
its decoded content is exactly one Unicode scalar: a declaration with declared
type `char`, a `char?`/`char`-carrying context, a comparison against a `char`
operand (`c == 'a'`), a `char` match arm, or a return into `char`. `char c = 'a';`
is a char; `var s = 'a';` stays `string`; double-quoted literals are never char.
Escapes work in char literals (`'\n'`, `'\x41'`). A single-quoted literal
compared against a *string* keeps string typing (back-compat, §6.1 char note).
Call-argument position is **not** yet a target-typing site (a `char`-typed value
binds to a `char` parameter, but a bare `'x'` argument stays `string` — deferred,
`designs/techdesign-track03-type-surface.md`).
```
::  :  ;  ,  .  ..  (  )  {  }  [  ]
=>  =  ==  !=  !  <  >  <=  >=  +  -  *  /  %
+=  -=  *=  /=  %=  <<  >>  &&  ||  &  |  ^  ~  ?  ??  ?.  @
```
`@` introduces an attribute (§6.9). A backtick-delimited `` `...` `` lexes as a
quasiquote literal (template payload; consumed by Phase-2 rules), and inside
one, `$name` is a hole. `attribute`, `comptime`, `rule`, `macro`, and `as`
(§4.1's `use ... as ...`) are **contextual** — ordinary identifiers except at
the declaration positions that give them meaning.

---

## 2. Types

### 2.1 Type expressions
```
TypeName                      // int, string, MyClass
TypeName<T1, T2>              // generic instantiation; nested (Array<Pair<T,U>>) allowed
T1 | T2                       // union type
(T1, T2) => R                 // function type
var / let                     // inference markers (not types; the static type is inferred)
```
`var` and `let` are both pure inference markers: the declared type is whatever the
initializer's type is. They differ only in mutability — `let` is sugar for `const var`
(§4.3b): a single-assignment inferred binding. `var` stays freely reassignable. Neither
is a type; `var`/`let` never appear in a type position beyond the declaration site.

### 2.2 Primitives (the object mask)
`int`, `string`, `bool`, `float`, `char` are **value types with a method shape**: stored
unboxed, literals type through them, and methods dispatch through the same member machinery as
classes (`this` inside a primitive method is the raw value). `void` is the absence of a value;
only usable as a return type.

`char` (Track 03 §1) holds **one Unicode scalar** (`0..0x10FFFF`, surrogates excluded),
unboxed, default `'\0'` (bare declaration → scalar 0). Literals are target-typed (§1.4);
methods and `std::charFromCode` are in §6.1. Comparisons (`== != < <= > >=`) are by scalar
value; there is deliberately **no arithmetic** on `char` (use `code()`, avoiding C's integer-
promotion pitfalls). `char` ships on the oracle, IR, emit-C++, **and LLVM** engines
(the `LV_CHAR` ABI addendum landed 2026-07-10 — `designs/deferal-char-block-abi.md`);
no ELF (X64Gen frozen, M4 deferred).

### 2.3 Unions and optionality
`int | string` is a closed tagged union. Assignability: a value of type `T` is assignable to a
union containing `T`.

**`T?` is sugar for `T | None`** (`None` = the unit absence type; there is no null — a
`None` value is just its union tag, no payload). Optional fields default to `None`; general
unions default to their first member's default. `None` never compares equal to a present
value (absent / present-empty / present-full stay distinguishable).

**Narrowing** (implemented): member access or calls on a union are a **compile error** until
narrowed. `x != None` / `x == None` and `x is T` narrow by **flow typing** — in `if`/`else`
branches, `while` bodies, ternary arms, and across `&&` (`s != None && s.length() > 0`).
Paths narrow too (`req.host != None` narrows `req.host`); assignment to a path (or its base)
invalidates its narrowing. Conditions require `bool` — no truthiness. `!` negates facts.

**`??`** — default-when-None: `host ?? "d"` types as the None-stripped union (the default
must match). **`?.`** — optional chaining: `a?.m()` / `a?.b` short-circuit to `None` (args
unevaluated) and type as `R | None`.

### 2.4 Reference vs value semantics
**Objects are references**: assignment and parameter passing share the instance. Primitives and
arrays are **values** (arrays are pure — §6.3).

### 2.5 Generics
Any scope-opening entity may declare type parameters: `class C<T>`, `R f<R>(R x)`, and methods
`U m<U>(...)`. Inference sources, in order: constructor/call argument types (including through
containers: `Array<U>` unifies with `Array<int>`), then the target type of the enclosing
initializer/return. Explicit `Name<T1,...>` is always available. Generics are **invariant**;
the raw form (`Array`) is compatible with any instantiation of the same head.

Singleton scopes (namespaces, class static sides) may use type parameters in **member
signatures** (bound per call) but may not declare **state** typed by them.

**Higher-kinded type parameters**: a type parameter may be a type constructor (`F` of kind
`* -> *`), applied as `F<A>`. Inference binds the constructor head by unification —
`F<A>` against `Array<int>` binds `F = Array, A = int` — and the head flows into the return
type, so container-preserving generic functions type precisely:

```
F<B> mapIt<F, A, B>(F<A> c, (A) => B fn) => c.map(fn);
Array<int> d = mapIt([1,2,3], (n) => n * 2);    // F=Array preserved -> Array
```

Bodies are duck-typed at instantiation (like C++ templates): `c.map(fn)` is checked leniently
and resolved at the call. When a result type argument can't be bound (e.g. from an opaque
lambda), the result is the raw head (compatible with any instantiation). HKT is an advanced,
gated idiom — prefer methods and interface bounds for everyday code.

Inside a generic callable body, the left operand of `::` may be one of that callable's
ordinary (`*`-kinded) type parameters. `T::member` is checked duck-style at the definition and
resolved separately for every concrete instantiation; labeled constructors, immediately-called
members, and callable members used as function values follow the same rule:

```
A decode<A>(A witness, int n) => A::FromInt(n);
```

The compiler emits one deduplicated concrete body per whole-program type tuple, so the operation
has the same runtime cost as the equivalent hand-written concrete function. If an instantiating
type lacks the selected member, the compile error names the concrete type and points to both the
`T::member` use and the call that instantiated it.

v1 bounds specialization depth at 32 and permits `T::` only for callable-level, `*`-kinded type
parameters. A class-level type parameter is rejected because raw generic-class widening erases
the tuple needed to select a copy. A generic instance method using `T::` is supported only when
it neither overrides nor is overridden; override-dispatched specialization is deferred.

---

## 3. Expressions

### 3.1 Precedence (loosest to tightest)
| level | operators | associativity |
|---|---|---|
| 1 | `=` `+=` `-=` `*=` `/=` `%=` | right |
| 2 | `?:` (ternary), `..` (range) | right |
| 3 | `\|\|` | left |
| 4 | `&&` | left |
| 5 | `\|` `&` `^` | left |
| 6 | `==` `!=` | left |
| 7 | `<` `>` `<=` `>=` | left |
| 8 | `<<` `>>` | left |
| 9 | `+` `-` | left |
| 10 | `*` `/` `%` | left |
| prefix | `!` `-` `~` | — |
| postfix | `.name` `::name` `(args)` `[index]` | left |

### 3.15 `match` — type/value dispatch
`match (subject) { pattern => body; ... }` where a pattern is a **type** (`Type =>`, narrows
the subject in that arm), a **value/range** (`0 =>`, `1..9 =>`), or **`else`**. First-match-
wins. Bodies are `=> expr;` or `=> { block }`; `match` is an expression (arms yield a value)
or a statement (no trailing `;`). Exhaustive over a closed union (no `else` needed); an open
hierarchy requires `else`. Lowers to the same `is`/`IsType` test as `catch` and `is` — one
type-dispatch path. (`match` is a reserved word.)

### 3.2 Primary expressions
Lambdas take an expression body (`(x) => x + 1`) **or a statement block**
(`(x) => { ...; return y; }`). Keywords (`get`/`set`/`is`/`in`/…) may be used as **member
names** after `.`/`::` and as method names after a return type — they are keywords only at
statement/declaration start.
```
42  3.14  "s"  true  false          // literals
name                                 // variable / member / function / type
this                                 // enclosing instance (or primitive receiver value)
(expr)                               // grouping
[e1, e2, ...]                        // array literal (ranges spread)
(p1, p2) => expr                     // lambda (params untyped; captures by closure)
await expr                           // suspension point — parks the current task (§6.6.67)
inject Type                          // explicit injection selector
a .. b                               // range value
```

**String interpolation:** `"...${expr}..."` (either quote style) desugars in
the parser to concatenation with `.toString()`:
`"code=${resp.status}!"` ≡ `"code=" + (resp.status).toString() + "!"`.
`\${` escapes a literal `${`; a bare `$` (no `{`) stays literal (no `$name`
shorthand — that syntax belongs to quasiquote holes, §6.9, and is untouched).
An empty hole (`${}`) or an unterminated one (`${` with no matching `}`) is a
compile error. A hole may contain any expression, including one with a
**nested string literal**, as long as it uses the **opposite quote style**
from the outer literal (`"${'has a } in it'}"` — the lexer has no concept of
interpolation, so a nested string using the *same* quote character still
ends the outer token right there, exactly as it would in any ordinary string).
A type without `toString()` fails the same way a hand-written `.toString()`
call on it would (§3.7).

### 3.3 Calls and construction
```
f(args)                    // function call; overload chosen by argument types
f(x, label: value)         // positional arguments first; named arguments may reorder
obj.method(args)           // method call; overload chosen by argument types
Type(args)                 // construction — NO `new` at the call site
Type::Label(args)          // constructor selected by label
NS::Type(args)             // construction of a namespaced class, reached by qualification
NS::Type::Label(args)      // ...combined with label selection
Base::Ctor(args)           // inside a constructor: base constructor applied to `this`
```
Constructor selection: candidates share the label; the overload is chosen by argument types
(most-specific; first-declared breaks ties). Generic type arguments are inferred from the
constructor arguments, then the target type.

At every call site, positional arguments must precede named arguments. A named argument binds
the parameter carrying that name; it may appear in any order among the named suffix. Functions,
methods, constructors, and attributes use the same spelling and binding rule.

A `::`-reached callable **without** a following `(...)` is not a call but a **method
reference** — a first-class function value (§3.4).

### 3.4 Member access and qualification
- `.` navigates an **instance**.
- `::` navigates the **base / static / non-instantiated** side: base classes
  (`this.Counter::value`), constructor labels (`Type::Label`), namespaces (`NS::name`), and
  class static sides. Inside a generic callable its left operand may also be a callable-level
  type parameter, resolved per concrete instantiation (§2.5). One operator, one meaning: "the
  non-instantiated version."
- A bare read that cannot be resolved by type — e.g. a `distinct`-collided member with no
  qualifier — is a **compile error** ("refuse to guess").

**Method references (LA-25).** A `::`-reached **callable** member in **value position** (not
immediately called) is a first-class function value — the same way a `::`-reached field read
yields the field's value (a member is a typed slot; some slot types are executable):

```
var f = NS::fn;                  // namespace function: its signature directly
b.handler = Controller::Login;   // instance method: UNBOUND — the receiver becomes
                                 //   the FIRST parameter: (Controller, Req) => Resp
var g = User::FromName;          // labeled constructor: (string) => User
Route('/login', AuthController::Login)   // as a call/constructor argument
```

- The reference's type spells as an **ordinary function type** (`(A, B) => R`) — assignable to
  fields/params/locals of that type and storable in containers like `Array<(A,B)=>R>`.
- **Resolution is by target function type.** An overloaded name resolves against the declared
  type of the slot being assigned/initialized, or the chosen overload's parameter type in
  argument position (deferred like a lambda argument, so it does not help pick the *outer*
  overload). Overloaded **with no target in context** (`var h = C::overloaded;`) is a compile
  error — annotate the target type. A **missing member** is a compile error at the reference
  site.
- **Dispatch** follows the ordinary receiver-dispatch rule, because the reference *is* the
  eta-expansion lambda `(C c, ...) => c.m(...)`: an interface reference (`IAnimal::speak`) and a
  concrete-class reference (`Animal::speak`) both dispatch on the runtime object — see §3.4a.
- **v1 limits:** a reference to a **generic** callable (`M::identity` where
  `R identity<R>(R x)`) is a compile error — its type parameters are unbound in value
  position. Each **evaluation** of a reference yields a **fresh** function value (like two
  identical hand-written lambdas), so identity comparison is not a "same handler" test —
  compare by key, or store the value once.

**Bound method references (F3).** A `.`-reached method in value position captures its
receiver and removes that receiver from the function type:

```
Editor editor = Editor();
var save = editor.save;                // equivalent to () => editor.save()
menu.onKey(this.onKeyDown);            // `this` may be captured directly
```

- The receiver must be a **bare local, parameter, or `this`**. An embedded expression such
  as `a.b.method`, `this.field.method`, or `make().method` is a compile error with a
  "bind the receiver to a local first" fix. This makes capture a one-time snapshot instead
  of silently re-evaluating an expression on every call.
- The captured value is the object reference: later mutation of that object is visible, but
  rebinding the original local does not retarget the closure.
- Overloads use the same target-function-type resolution as unbound references, including
  argument and typed-container contexts. A closure-valued field wins ordinary field-read
  resolution over a same-named method.
- Generic methods remain unsupported in value position. Each evaluation is fresh, and
  runtime override dispatch is exactly the dispatch of the eta-expansion lambda.

### 3.4a Method dispatch: runtime slot, statically devirtualized

An unqualified instance-method call dispatches on the receiver's **runtime** class — uniformly
for interface-typed *and* class-typed receivers (`designs/complete/techdesign-class-method-dispatch.md`).
A `Dog : Animal` overriding `speak()` runs `Dog::speak()` through any `Animal`-typed binding
(field, parameter, local, or a method reference, §3.4):

```
class Animal { string speak() => "..."; }
class Dog : Animal { string speak() => "Woof"; }
string callSpeak(Animal a) => a.speak();
callSpeak(Dog());        // "Woof" — the override runs, not the statically-named Animal::speak
```

The compiler **devirtualizes to a direct call** whenever the candidate set is provably closed —
no class anywhere in the whole-program gather (§12 of info.md) overrides the resolved method
below the receiver's static type — a whole-program optimization that can only change *how fast*
the call runs, never *what* runs. Qualified access (`this.Base::m()`), operators, constructor
selection, and static/namespace functions remain statically resolved (unaffected).

**Limit: overridden overload sets sharing an arity.** The runtime dispatch is by **name + arity**
(no type disambiguation). An overridden method that shares its `(name, arity)` with another
overload on the same receiver static type cannot be picked correctly at runtime and is a
**compile error** at the call site (give the overloads distinct arities/names, or qualify the
call explicitly):

```
class Animal { string speak(string s) => "a-str"; string speak(int n) => "a-int"; }
class Dog : Animal { string speak(int n) => "d-int"; }   // overrides speak(int); ambiguous with speak(string)
Animal a = Dog(); a.speak(5);   // compile error: shares its arity with another overload
```

Different-arity overridden overload sets (the common case) stay legal and dispatch correctly —
only same-arity siblings are rejected. The real fix (signature-aware runtime dispatch, carrying
the resolved parameter types into the by-name lookup) is roadmapped; it would benefit interface
dispatch identically, which has carried this same name+arity limitation since before this design.

### 3.5 Operators on objects
`a op b` resolves `(op)` on `a`'s class by the type of `b` (overloads supported). `(==)` must
return `bool`; `(!=)` derives automatically as `!(==)`. Defining an operator means declaring a
member with a symbolic selector (§4.4).

### 3.5b Integer operators
`<<` (shift left), `>>` (shift right, **arithmetic** — sign-extending, since
`int` is signed 64-bit), `^` (xor), and prefix `~` (complement) are defined
**only on `int`** — no shifts on `float`, no `^`/`~` on `bool` (use `!=`/`!`)
or `string`. A shift count outside `0..63` **throws `RuntimeException`**
("shift count out of range") rather than silently masking to 6 bits (the x86
default) or invoking C++'s undefined behavior. Resolution stays by type
(§1 of info.md): on an *object* left operand, `<<`/`>>` still dispatch the
`(<<)`/`(>>)` operator method (the stream transfer operators, §13/§6.6) —
the same rule that lets `+` be both int-add and a user `(+)`.
```
int x = 1 << 4;        // 16
int y = 256 >> 4;       // 16
int z = 5 ^ 3;          // 6
int w = ~0;             // -1
```

### 3.6 Indexing
`a[i]` dispatches to the `([])` get accessor of `a`'s class; `a[i] = v` to the `([])` set
accessor. On a **mutable object** the set accessor mutates in place. On a **pure array**,
`a[i] = v` is rebind-sugar: `a` is rebound to a new array with slot `i` replaced.

### 3.7 Strictness
- An **unknown name or function** is a compile error (`System` excepted until
  modeled; `console` IS modeled — §6.7).
- A generic construction whose type argument has **no inference source** is a compile error —
  provide a target type or a type-bearing argument (§2.5: inferred when recoverable, required
  when not).
- Runtime failures **throw catchable `RuntimeException`s**: index out of bounds, `Map.at` on a
  missing key, unresolvable call targets, missing operators on a class.

---

## 4. Declarations

### 4.1 Namespaces
```
namespace Name { ...declarations... }
```
Declaration-based (disk layout is irrelevant); the same namespace may be reopened and merges.
Reached by `NS::name`, or imported:
```
uses NS;                      // import ALL of NS's names into the enclosing scope
uses A::B;                    // nested path

use NS::name;                 // import ONE name (value, function, class, or
use NS::name as alias;        // nested namespace) — any decl kind, uniformly
use A::B;                     // a nested namespace itself: B::f() then works
```
An import is a declaration in whatever lexical scope it appears in; the top level of a
file is one such scope, so a top-of-file import covers exactly that file (not the whole
program) — imports are hoisted (visible throughout their scope regardless of position),
same as declarations generally. `use` binds one name, selectively, with optional `as`
renaming (collision-proof); `uses` dumps a whole namespace's names. Both are pure
compile-time resolution: an alias names the *same* slot (no runtime copy), so a write
through it is a write to the global — rejected at compile time exactly like a qualified
`NS::name = ...` (§3.7). Nearer declarations shadow imports; a `use` shadows a same-named
`uses`-dumped name in the same scope (specific beats bulk).

### 4.2 Classes and interfaces
```
class Name<T, U> : Base1, Base2 {  ...members...  }
class Name;                          // empty-body form
interface IName { int x; string f(); }
```
- **Multiple inheritance.** Same-named members collide only when the **type also matches**
  (§4.3); different-typed same-named members coexist (resolution by type).
- **Interfaces allocate nothing**: they declare required members (fields included); the
  implementing class's declaration is the single allocating site, so two interfaces requiring
  the same field never collide. Unsatisfied requirements are compile errors.
- **Interface method returns are covariant.** A method satisfies a requirement when its name
  and parameter canonicals match exactly and its declared class/interface return is assignable
  to the required return. `T` also satisfies required `T?`; other unions, functions, primitives,
  and `void` remain exact. This applies only to interface satisfaction—narrowing a return in a
  derived class does not override the base method. Requirements allocate no slot, so dispatch
  still reaches the implementing class's single method.
- Access: inline (`public int x;`) or sectional (`public:` ... `private:`).

### 4.2b Value structs (`struct`)
```
struct Point {
    int x;  int y;
    int dot() => x * x + y * y;
    mutating void translate(int dx, int dy) { x = x + dx; y = y + dy; }
}
```
A `struct` is a **value type** (the object mask generalized, §9 of info.md): copied on every
bind/pass/return/store (deep — nested structs copy, reference fields are shared), no identity,
and **final** (may implement interfaces, may not inherit implementation or be inherited from).
A method that writes `this` must be marked **`mutating`** (a plain method assigning a field is a
compile error); constructors and `set` accessors are mutating by definition. Reference `class`
is unchanged — pick `struct` for data (rows, coordinates), `class` for entities with identity.
Value types are what make a dense, unboxed `Array<Point>` / columnar / `mmap` layout possible.

### 4.2c Enums (`enum`)  (Track 03 §2)
```
enum Method { GET, HEAD, POST }                    // carrier: int, auto 0..n-1
enum Status : int { OK = 200, NotFound = 404, Teapot = 418 }   // explicit carriers
enum Gap : int { A, B = 10, C }                    // auto-after-explicit: A=0, B=10, C=11
```
An `enum` is a **value type** (copied, no identity, final) with a **closed** member set carried
by `int`. Members live on the **static side**, reached by `::` (the non-instantiated side, §3.4):
```
Method m = Method::GET;              // member access
m.code()                             // 0    — carrier value (int)
m.toString()                         // "GET" — member name
Method::fromCode(200)                // Method?  — None if no member's carrier matches
Method d;                            // bare declaration -> the first-declared member (GET)
```
- **Operators:** `==`/`!=` (and `<`, ordering) compare by carrier value. As a `Map`/`Set` key,
  an enum compares by value (contract C3).
- **`fromCode(int) -> Enum?`** returns `None` when no member carries that value.
- **Carriers:** `: int` is the only carrier in v1 (string carriers deferred,
  `designs/techdesign-track03-type-surface.md`). Members without an explicit value auto-increment
  from the previous member's carrier (`Gap` above); **duplicate carrier values are a compile
  error**.
- **`match` is exhaustive over the closed set** — every member covered means no `else` is
  needed, and omitting one is a compile error naming the missing members; an `else` arm is still
  permitted:
```
match (m) { Method::GET => ...; Method::HEAD => ...; Method::POST => ...; }   // exhaustive
match (s) { Status::OK => "ok"; else => "other"; }                           // else allowed
```
An enum **desugars** in the resolver to a value `struct` (a single `int $code` field) plus one
compiler-mangled `Enum$Member` const global per member and an `Enum$fromCode` free function;
`::`-member access and `match` arms resolve through two narrow rules gated on the enum's
registration. Because it lowers to structs + int + globals — no new value kind, no ABI tag —
`enum` is **full-coverage on all four engines including LLVM** (no ABI tag at all;
`char`/`Block` now also reach LLVM via their `LV_CHAR`/`LV_BLOCK` tags, but still
not ELF — X64Gen frozen).

### 4.3 Fields and `distinct`
```
public string label;
public distinct int value = 0;
```
Same **name + type** inherited from two bases = a collision. Default: **collapse** to one slot
(later base wins). `distinct` (on either side) keeps **separate per-source slots**, reachable
only by qualification: `this.Counter::value`. A bare read of a distinct-collided member is a
compile error.

Bare declaration **auto-constructs** (§3 of info.md): `string s;` is `""`, `Array<T> a;` is
`[]`, `int i;` is `0`. (Auto-construction of arbitrary object types: **[partial]** — currently
primitives and arrays.)

### 4.3b `const` declarations
```
const int maxRedirs = 50;                    // local: fixed at declaration
const var limit = 100;                       // composes with inference
const Array<string> args = std::sysArgs();   // namespace global: fixed at startup

class Session {
    public const string id = "s-1";          // field: a named compile-time constant
    const int SSL = 0x0800;                  // field: literals/operators over consts also fold
}

void handle(const Options o) { ... }         // parameter
for (const string a in args) { ... }         // per-iteration binding
```
`const` scopes a slot's write view to its initialization window; after the window
closes, only the read view remains. It is not a type (never appears in a type
position, never affects assignability/overload resolution/generics) and not
transitive (`const MyClass m = MyClass();` fixes the binding `m`; `m.field = 5;` is
still legal — deep immutability is the *value* axis's job: a `struct`/pure
`Array`/`Map`, or a `get`-only accessor).

The window per slot kind:
- **Locals** — the declaration initializer, exactly. `const int x;` with no
  initializer is a compile error (auto-construction would freeze the default
  forever). The initializer may be any runtime-computable expression.
- **Fields** — a `const` field is a **named compile-time constant** (techdesign-
  readonly, LA-28): it **must** have an initializer, and that initializer **must be a
  compile-time constant** — a literal, `None`, an array of those, a reference to
  another `const`/`comptime` value, or an arithmetic/bitwise operator over constant
  operands (`const int BOTH = A | B;`). **No constructor may ever assign a `const`
  field** — that capability belongs to `readonly` (§4.3c), the construction-time-
  fixed field. A field needing a value known only at construction time (an
  injected/runtime value) is `readonly`, never `const`.
- **Namespace / top-level globals** — the initializer, only, and the initializer may
  be any runtime-computable expression (unlike fields — a global has no distinct
  construction window for `readonly` to occupy, so `const` there keeps its full,
  unnarrowed meaning).

  **Initialization order.** Every global — top-level and namespace-scoped, bare or
  with an initializer — is **auto-constructed to its §3 default before any
  initializer or top-level statement runs**, so an initializer always sees a real
  slot (never an absent/zero one) and a mutation through it persists. Then, in
  order: (1) **namespace-scoped** initializers run (a startup phase — this is why a
  rule-injected `namespace Reg { bool $C = seed(...); }` seeds a registry before the
  program body executes); (2) **top-level** statements run in source order, and a
  top-level global's *explicit* initializer runs there, at its source position
  (preserving side-effect ordering with the surrounding statements). A namespace
  initializer therefore observes a top-level global at its **default**, not its
  top-level explicit value (that assignment is a body statement that runs after) —
  to seed a namespace global *from* a value, use another namespace-scoped
  global/`const` as the source or compute it in the initializer.
- **Parameters** — the call binding; reassignment in the body is an error.
- **`for (const T x in ...)`** — each iteration is a fresh binding.

Interactions all fall out of the one rule: compound assignment (`a op= b`) is a
write, same as `=`. `a[i] = v` on a pure array is rebind sugar (§3.6) — a write to
the slot; on a mutable object with a `([])` set accessor it mutates in place and
never touches the slot, so it's unaffected by a const binding. Calling a `mutating`
method through a const binding is an error (it needs a write view that doesn't
exist). A const binding's narrowing is never invalidated (nothing can assign it).
A `set` accessor may not be declared over a const field. An MI collision between a
const and non-const same-name+type slot is ambiguous — a compile error (resolve with
`distinct` or by matching the declarations).

For a field whose fixed value is known only at **construction time** (e.g. an
injected dependency), see `readonly` (§4.3c) — the construction-time-fixed sibling
of `const`'s compile-time-fixed slot.

### 4.3c `readonly` declarations (LA-28)
```
class AuthController : Controller {
    private readonly IUserService userService;      // field, constructor form
    new AuthController(IUserService userService) this.userService = userService;
}

class Session {
    readonly string id = generateId();               // field, initializer form (runtime-computed OK)
}
```
`readonly` is `const`'s construction-time-fixed counterpart: **instance fields
only** (a local/global/parameter/for-in binding has no construction window distinct
from its initializer, so `const` already covers them there). A `readonly` field's
write view is its **initializer**, *or* **any of the declaring class's own
constructors**, and it must be written **exactly once**:

- **Initializer form** (`readonly T x = v;`) — the initializer is the one write; no
  constructor may also assign `x` (that would be a second write — a compile error).
- **Constructor form** (`readonly T x;`, no initializer) — every constructor the
  class declares must assign `x` exactly once, definite-assignment style. A class
  with **zero** constructors and no initializer is a compile error (nothing could
  ever assign it). **v1 restriction:** definite assignment only recognizes
  **top-level statements** (direct children of the constructor body) — an
  exhaustive `if (c) { x = a; } else { x = b; }` is rejected in v1 (sound, not yet
  complete; a future CFG-aware pass may relax this).

Outside its write window (a non-ctor method, another class, after construction, or
a derived class reaching a base's `readonly` field directly instead of via
`Base::Ctor(...)`) a write is an error — the exact rule `const` fields used to
enforce, now serving `readonly`. Like `const`: not a type, not transitive (a
`readonly` field's own binding is fixed; the referenced object's fields still
mutate freely), narrowing is never invalidated, a `set` accessor may not be
declared over a `readonly` field, and an MI collision between a `readonly` and
non-`readonly` same-name+type slot is ambiguous (resolve with `distinct` or by
matching the declarations). A field may not be marked both `const` and `readonly`.

### 4.3d `weak` fields (F5)

```
class Component {
    weak IComponent? parent = None;
    weak readonly IComponent? fixedParent = None;
}
```

`weak` is an instance-field slot property, never a value qualifier. Its declared
type must be `T?`, where `T` is a reference `class` or `interface` (not a
`struct`, string, array, map, `Block`, or closure). A store accepts `T`, `T?`, or
`None` and does not retain the referent. A read returns a fresh `T?`: `None` once
the referent's last strong reference has been released, otherwise an ordinary
owned value. Copy a read to a local before narrowing it; a weak field path itself
is intentionally non-narrowable because every read performs a new liveness check.

Weakness belongs to the **slot**, never the value. Consequently a live weak read,
parameter, return value, or captured read is strong in the ordinary way. A weak
field copied through `spawn`, `Channel`, or `std::sysThreadTransfer` becomes
`None` on the destination side—the referent was not copied. `weak readonly` and
`distinct weak` are legal; `weak const` is not. Engine coverage: tree-walk, IR,
emit-C++, and LLVM; no frozen-ELF lane.

### 4.4 Methods, functions, operators
```
int f(int a, int b) { return a + b; }   // block body
void listen(int port = 80, string host = "localhost") { ... }
int f(int a, int b) => a + b;           // arrow body (=> IS return)
void g() console.writeln("x");          // any single statement is a body
void h();                                // empty body (interface req. / native intrinsic)
U remap<U>(U v) => v;                    // method-level type parameters
Counter (+)(int val) => ...;            // operator: symbolic selector
```
A **body is exactly one statement**. A **method** has `this` (instance side); a **function**
does not (namespaces, class static sides). Overloading is by argument types everywhere.
Named arguments and default parameter values apply uniformly to methods and functions.

### 4.5 Constructors
```
new ClassName() { ... }                 // 'new' marks a constructor
new AnyLabel(string s) { ... }          // the name is only a selection label
new Configured(int port = 80) { ... }   // constant default parameter value
```
No `new` at the call site. Inside a constructor, `Base::Ctor(args)` runs a base constructor
against `this` (derived class controls order). Label + argument types select among candidates.
Constructor arguments may be named and constructor parameters may declare defaults.

### 4.6 Accessors (get/set)
```
get value() => value;                    // read view over the 'value' slot
set value(int v) value = v * 2;          // write view
get ([])(int i) => cells[i];             // indexer (computed accessor)
set ([])(int i, int v) cells[i] = v;     // value parameter last
```
Parameterless accessors are **views over a backing slot** (no backing → discarded).
Parameterized accessors are **computed** (no backing needed). Inside an accessor body, the
owning slot's name is **raw access** (no recursion). Declaring only `get` → read-only; only
`set` → write-only.

### Argument binding

A call is resolved as one set of argument-to-parameter bindings:

1. Positional arguments bind from the first parameter onward.
2. Named arguments bind parameters by name; an unknown name or a second binding is an error.
3. Each omitted parameter is filled by its declared `= constant` default, otherwise by a
   matching lexical `bind`; a default takes precedence over ambient injection.
4. Supplied arguments are checked against their mapped parameter types. Most-specific wins;
   on equal type scores, the candidate using fewer defaults/injections wins; an exact tie is
   first-declared.

Defaults must be compile-time constants and cannot refer to another parameter or `this`.
Defaults on type-variable-typed parameters are not supported in v1. The checker rewrites every
successful call into a full positional argument list, so named/defaulted calls have no special
runtime calling convention.

### 4.7 Dependency injection
```
bind ILogger => ConsoleLogger();   // factory binding (body rule = method body rule)
bind ILogger { if (cfg) return A(); return B(); }
greet();                            // injection is implicit when unambiguous
greet(inject ILogger);              // explicit selector on collision
```
Block-scoped, lexically resolved, nearest-wins. Duplicate binding in one scope: error.

**Bind placement: enclose the call site, not the callee.** Injection resolves lexically at the
*injection site* (where the unfilled parameter is bound), not dynamically along the call chain —
a `bind` inside a function body only reaches calls made from inside that body. App-wide wiring
therefore lives at the outermost scope that encloses every call whose argument it should fill:
```
bind IEnv => FakeEnv(...);   // must enclose the CALL to main(), not main's own body
main();
```

#### Channel 1 — `use NS::T;` activates `T`'s namespace bind (system-binds.md)

A `use` of a type name also installs that type's **namespace bind**. A factory bind
(`bind T => ...;` / `bind T { ... }`) written at the **top level of a namespace body** is that
namespace's *exported* bind for `T`. `use NS::T;` (or `use NS::T as A;`), when `T` resolves to a
class or interface, installs `NS`'s bind for `T` — if one exists — into the scope the `use` is
written in, exactly as if `bind T => <that factory>;` (or `bind A => <that factory>;` under the
alias) were textually present there. The rules:

1. **No other path activates.** `uses NS;` (bulk import, including the implicit `uses std;`)
   never activates a bind — only a selective `use` of the type does. `use NS::fn;` of a
   non-type imports the name and activates nothing. A `use` of a type whose namespace has no
   top-level bind for it imports the name and activates nothing, silently.
2. **Textual beats activated, silently, same scope.** A hand-written `bind T => ...;` in the
   same scope as a `use`-activation of `T` wins — not the duplicate-bind hard error (that is
   reserved for two *textual* claims). This is what makes the test idiom below frictionless.
3. **Activated-vs-activated can't collide.** Bind keys are type-keyed, so two `use` statements
   can only install two binds for the same key by importing the same type twice — which
   dedupes to the one namespace bind.
4. **Nearest-wins is unchanged.** An activated bind participates in shadowing exactly like a
   textual bind at the same position.
5. **An alias changes the name only** — activation is identical under `as`.

```
use std::IEnv;                       // brings the name; activates the system bind file-wide
class FakeEnv : IEnv {
    Array<string> canned;
    new FakeEnv(Array<string> a) { canned = a; }
    Array<string> args() => canned;
    string? variable(string n) => None;
}
{
    bind IEnv => FakeEnv(["prog", "-v"]);   // block-level: shadows the activated root bind
    main();                                  // main's IEnv parameter fills with the fake
}
main();                                      // outside the block: the system bind again
```

**Capability interfaces** (`namespace std`, root-bound to the real system surface): `IEnv`
(`args()`, `variable(name) -> string?`), `IConsole` (`write`/`writeln`), `IClock` (`now()`),
`IFileSystem` (`open(path, mode)`, `exists(path)`), `INet` (`connect(host, port) -> TcpStream?`,
`listen(port)`). Each is a thin shim over the matching ambient global (`env::*`, `console`,
`std::sysNow`, `File`, `TcpStream`/`TcpListener`) — the shim is the only implementation, so
system and injected code share one behavior. The ambient globals remain reachable regardless;
the capability surface is a cheaper, disciplined *alternative*, not an enforced sandbox — see
§6.6.6a.

**comptime.** Binds are compile-time data, and ordinary (non-`comptime`) code that injects a
capability and later folds is unaffected. An injection written **directly inside** a
`comptime`-folded root (`comptime T x = (inject ICap)...;`, `comptime if (...)`) is denied at
the injection site itself — comptime folding runs before the checker's bind-scope pass exists,
so no bind (textual or `use`-activated) is ever in scope there — before any call could even
reach a native floor to hit the ordinary hermetic-comptime `sys*` deny.

**[planned] Binder objects (Channel 2):** `Bindings` values and `bind someBindings;` are not
implemented — deliberately deferred (system-binds.md §7): the many-scattered-binds aggregation
use case is owned by the metaprogramming splice mechanism (rule-generated ordinary `bind`
statements at a splice site) instead of a second aggregation mechanism, and the remaining
manual-multi-swap use case is already one atomic, collision-checked block of ordinary `bind`
statements. Its builder-entry syntax and compile-time/runtime boundary remain open design work
if ever revived; use lexical factory binds (plain, or `use`-activated) as shown above.

---

## 5. Statements

```
Type name = expr;      var name = expr;       // declaration (var/let infer)
Type name;                                     // auto-constructed
using Type name = expr;                        // deterministic cleanup: §5.2
expr;                                          // expression statement
{ stmt* }                                      // block
return expr;    => expr;                       // return (arrow form)
if (cond) stmt [else stmt]
while (cond) stmt
do stmt while (cond);                          // post-test: body runs before the first check
for (init; cond; step) stmt
for (T x in iterable) stmt                     // ranges, arrays, maps (Pair entries), any IIterable<T> (§6.4.8)
break;      continue;                          // loop control: §5.2
throw expr;                                    // expr must implement IException
try stmt catch (Type name?) stmt [catch ...]   // first assignable clause wins
bind ...;   uses NS;   use NS::name (as alias)?;   // imports: §4.1
;                                              // empty
```

### 5.1 Exceptions
`throw` is a control-transfer statement like `return`. The thrown value must implement
`IException`. Catch clauses select by the thrown value's dynamic type — first clause whose
declared type it is assignable to (subclass → base class → implemented interface); the binding
name is optional. Uncaught exceptions terminate execution with
`Uncaught <Class>: <message>`. There is no `finally` — scope-based cleanup (`using`, §5.2) covers
the common case; ordinary `try`/`catch` covers the rest.

### 5.2 Loop control and `using` (techdesign-02)

`break;` exits the nearest enclosing loop (`while`, `do`-`while`, C-style `for`, `for..in`).
`continue;` skips to the next iteration: for `while`/`do`-`while`, to the condition; for `for`,
to the step (then the condition); for `for..in`, to the next element. Both are unlabeled only —
they always target the *innermost* enclosing loop. Using either outside any loop is a compile
error. A lambda body is its own loop-nesting scope: a bare `break`/`continue` inside a lambda
never reaches a loop in the *enclosing* function — it is legal only if the lambda's own body has
a loop of its own. `match` is not a loop-nesting boundary: `break`/`continue` inside a `match`
arm that sits inside a loop targets that loop, not the match.

`do stmt while (cond);` runs the body once unconditionally, then loops while `cond` holds
(post-test, unlike `while`'s pre-test). `continue` inside a `do`-`while` body jumps to the
condition check, not back to the top of the body.

`using Type name = expr;` declares a scope-owned resource: `Type` must be a reference type
(not a `struct`) implementing `IDisposable` (`interface IDisposable { void close(); }`, prelude
— `File` implements it). The binding is implicitly `const` (a `using` slot may never be
reassigned) and legal only as a **direct statement of a block** — not a loop's one-statement
body, not a field, not a global, not a `for..in` binding (v1 restriction). `close()` runs
exactly once on **every** way the declaring block can be exited: falling off the end, `return`,
an uncaught `throw` unwinding past it, or a `break`/`continue` that leaves the block. Multiple
`using`s in one block close in **reverse declaration order** (last-in, first-closed) — the same
discipline C#'s `using` and Python's context managers use. If `close()` itself throws while
another exception is already unwinding, the new exception **replaces** the in-flight one (no
`finally`-style chaining); if the whole block exits normally and `close()` throws, that exception
propagates like any other. `using` composes with `const` (`using const File f = ...;`, a no-op
since `using` already implies it). **`close()` dispatches on the binding's runtime class**, so an
**interface-typed** `using` (`using IDisposable d = makeResource();`) runs the concrete class's
`close()` override — not the interface's empty requirement (which would silently skip teardown).
```
void copyLine(string path) {
    using File src = File(path, std::read);
    using File dst = File(path + ".bak", std::write);
    dst.writeln(src.readln());
}   // dst.close() runs, then src.close() — reverse declaration order, every exit edge
```

Standard hierarchy (in `namespace std`, implicitly imported — interfaces define catchability,
classes carry the payload):
`IException` (requires `string message`, `string toString()`); `Exception : IException`;
`IRuntimeException : IException`; `RuntimeException : Exception, IRuntimeException`;
`ILogicException : IException`; `LogicException : Exception, ILogicException`.

---

## 6. Standard library (prelude)

Written in the language over a minimal native-intrinsic core; automatically available.

### 6.1 Primitives
| type | methods |
|---|---|
| `int` | `abs()`, `max(int)`, `min(int)`, `toString()`, `pow(int) -> int` (square-and-multiply; negative exponent → `0`; overflow wraps, two's-complement), `clamp(int lo, int hi)` (`lo > hi` → `RuntimeException`), `sign() -> int` (`-1`/`0`/`1`), `toHex() -> string` (lowercase, no `0x` prefix, `-` for negatives), `toString(int radix) -> string` (`2..36`, else throws), `toFloat() -> float` (native; exact for `\|x\| < 2^53`) |
| `float` | `toString()`, `abs()`, `floor()`, `ceil()`, `round()` (native; half-away-from-zero, matches C `round`), `trunc()` (native), `sqrt()` (native; negative → NaN, IEEE, not a throw), `pow(float) -> float` (native), `toInt() -> int` (native; truncates; NaN/±inf/out-of-int64-range → `RuntimeException`, loud), `isNaN()`, `isInfinite()`, `bits() -> int` (native; raw IEEE-754 bit pattern), `canonEq(float) -> bool` (native; the canonical-relation primitive — `true` iff the two collapse to the same canonical form, so every NaN equals every NaN and `-0.0` equals `0.0`). Factory: **`float::fromBits(int) -> float`** (native free function; reinterprets the bit pattern). Constant: **`float::NaN`** (the one canonical NaN, a reachable value and `match` pattern) |
| `bool` | `toString()` |
| `char` | `code() -> int` (native; the Unicode scalar), `toString()` (native; UTF-8 encode), `isDigit()`, `isAlpha()`, `isUpper()`, `isLower()`, `isSpace()` (in-language over `code()`; ASCII ranges only — non-ASCII returns `false` in v1), `toUpper()`, `toLower()` (in-language, ASCII-only; a non-ASCII char returns itself unchanged). Factory: **`std::charFromCode(int) -> char`** (native free function — class static sides don't exist; out-of-range/surrogate → `RuntimeException`). |

**Engine coverage note (Track 06, `float.pow` only):** `int.pow` is in-language
and runs everywhere; `float.pow(float)` is native and is covered on the
oracle, IR, emit-C++, and LLVM backends. The frozen ELF backend still defers
it with a clean diagnostic (the zero-dep boundary; polynomial emission is a
later project). Every other method in both tables above (including
`floor`/`ceil`/`round`/`trunc`/`sqrt`/`toInt`/`toFloat`) is covered on all
five engines, verified byte-identical (`tests/corpus/math.ext`).

**Canonical float relation.** float scalars compare IEEE; derived equality,
hashing, ordering, and match — like all value contexts — compare canonically;
the two differ only on NaN. Concretely: a bare `x == y` on two floats is the
IEEE operator (`NaN != NaN`, `-0.0 == 0.0`), but a synthesized struct `(==)`,
a `Map` key comparison, and a `match` arm over a float all use `canonEq`, under
which every NaN equals every NaN and `float::NaN` is a reachable pattern. The
derived-vs-hand-written rule: **derived = canonical; hand-written = what you
wrote** — a struct's synthesized `(==)` compares float fields canonically, but
if you write your own `(==)` its float comparisons mean exactly the IEEE
operators you typed.

`string` relational operators (`<` `>` `<=` `>=`) are **lexicographic** —
byte-wise comparison of the character data, not identity or length; `""`
orders first, equal strings compare equal (bug.md #2). `\r` is a supported
escape (alongside `\n`, `\t`, `\\`, `\"`); `splitLines()` below is the
sanctioned way to strip it from CRLF input.

`string` — a native core (`length`, `charAt`, `subStr`, `indexOf`, `toInt`,
`toFloat`, `toUpper`, `toLower`, `trim`, `contains`, `startsWith`,
`endsWith`, `toString`) plus an in-language toolkit over it (Track 04):

| category | members |
|---|---|
| core (native) | `length()`, `charAt(int)`, `subStr(int start, int len)`, `indexOf(string)`, `toInt() -> int?`, `toFloat() -> float?`, `byteAt(int) -> int`, `toUpper()`, `toLower()`, `trim()`, `contains(string)`, `startsWith(string)`, `endsWith(string)`, `toString()` |
| char (Track 03) | `at(int i) -> char` (native; decodes the scalar **starting at byte offset `i`** — O(1), pairs with the byte-counted `length()`; a mid-sequence byte offset → `RuntimeException` "not a scalar boundary"), `chars() -> Array<char>` (native; full UTF-8 decode; invalid bytes decode to U+FFFD, never a throw — data is not a programming error) |
| search | `lastIndexOf(string)`, `indexOfFrom(string, int from)` (not an `indexOf` overload — a same-class overload would ride the arity-blind by-name fallback prelude bodies fall back to, bug.md #13), `count(string)` (`""` → 0) |
| split/join | `split(string sep)` (empty `sep` → array of 1-char strings; keeps empty segments), `splitLines()` (splits `\n`, trims one trailing `\r` per line) |
| transform | `replace(string from, string to)`, `padStart(int, string)`, `padEnd(int, string)`, `repeat(int)` (`<= 0` → `""`), `trimStart()`, `trimEnd()`, `removePrefix(string)`, `removeSuffix(string)` |
| queries | `isEmpty()`, `isBlank()` (`trim().isEmpty()`), `equalsIgnoreCase(string)` |

**`toInt()`/`toFloat()` are strict, optional-returning parses** (Track 04
M2 — `toInt()` was previously `atoll`-style, garbage silently became `0`;
this is a breaking change): optional leading `-`, digits only (no
surrounding space, no `+`), full-string consumption required; anything
else, or numeric overflow, is `None` rather than a guess. `toFloat()` is the
same shape over `strtod`, additionally rejecting non-finite results (`"inf"`
/`"nan"` text parses to `None`, not infinity). Narrow before use, same as
any `T?`: `int? p = s.toInt(); int n = p ?? 0;` or `if (p != None) { ... }`.
**Engine coverage note:** the frozen pure-ELF backend (`--emit-elf`,
`src/X64Gen.cpp` — kept as a reference/bootstrap seed, never extended, per
the portable-backend pivot) still runs the pre-Track-04 `toInt` and has no
`toFloat`; this divergence is accepted debt, not a bug (bug.md's toInt
migration note; `tests/corpus/strings_native/` deliberately excludes ELF's
test lanes for this reason). The oracle, IR, emit-C++, and LLVM backends
agree.

**`byteAt(int i)`** returns the raw byte value (`0..255`) at index `i`; out
of range throws `RuntimeException` (same wording as `Array.at`'s OOB, not
`None` — a byte position is either in the string or it's a bug, not an
absence). The reverse direction, **`std::byteToString(int b)`**, is a
`std` free function rather than a static method on `string` — the language
has no `static` keyword yet, so a `string::fromByte(int)` static-side
native has no plumbing to land on (Track 04 M3); `byteToString` throws the
same way for `b` outside `0..255`. Also excluded from the frozen ELF
backend's lanes, same as `toInt`/`toFloat`.

`reverse()` is deliberately not offered as a `string` method: a byte reverse is
wrong for UTF-8 content. With Track 03's `chars()` landed, the scalar-correct
idiom is `s.chars().reverse().joinToString("")`.

**Char engine coverage & back-compat (Track 03 §1).** `char`, `std::charFromCode`,
and `string.at`/`chars` ship on the **oracle, IR, emit-C++, and LLVM engines**
(the `LV_CHAR` ABI addendum landed 2026-07-10 — `designs/deferal-char-block-abi.md`):
`char` is a pure immediate (tag 10, `payload` = the scalar), its `code()`/literal/
`charFromCode` retag inline and its UTF-8 en/decode goes through `lvrt_char_to_string`/
`lvrt_str_at`/`lvrt_str_chars`. No ELF lane (X64Gen frozen, M4 deferred).
Char-literal target-typing (§1.4) is **expected-type-driven only**, so a
single-quoted literal compared against a *string* keeps string typing — existing
`ch == " "`-style code is unaffected. Where both `f(char)` and `f(string)`
overloads exist, a bare single-quoted literal argument selects **`string`**
(back-compat wins; reaching the `char` overload needs a `char`-typed expression).

### 6.1b `namespace math` (Track 06)

```
namespace math {
    const float pi = 3.141592653589793;
    const float e  = 2.718281828459045;
    float log(float x);   float log2(float x);   float exp(float x);
    float sin(float x);   float cos(float x);    float tan(float x);
    float atan2(float y, float x);
    int   min(int a, int b);      int   max(int a, int b);
    float min(float a, float b);  float max(float a, float b);
}
```

Accessed as `math::pi`, `math::log(x)`, etc., or via `uses math;` for bare
`pi`/`log(x)` in scope. `min`/`max` are in-language and overload by argument
type in the one namespace (an int pair and a float pair coexist); every
other member is a native free function backed by libm.

**Not `std::math`, despite the original design.** A qualified nested-
namespace path (`std::math::pi`) silently resolves to the wrong value on
the oracle and hard-errors on `--ir`/`--emit-cpp` — a real gap distinct
from the already-fixed bug.md #1/#2 (those covered block-scoped `uses`
imports, not qualified static paths into a nested namespace). `math` is a
**top-level** namespace instead — the design's own pre-registered fallback
for exactly this failure (`techdesign-06-stdlib-math.md` problem #3),
verified working on all five engines.

**Engine coverage note:** `pi`/`e`/`min`/`max` run everywhere. `log`/`log2`/
`exp`/`sin`/`cos`/`tan`/`atan2` are covered on the oracle, IR, emit-C++, and
LLVM backends (libm / LLVM intrinsics). On the frozen ELF backend they fail
with a clean coverage diagnostic (`native-elf backend: native floor function
'sin'`) rather than linking libm into that zero-dependency backend or
silently misbehaving. Covered by `tests/corpus/math_transcendental/`;
excluded only from the ELF test lanes.

### 6.2 `Range`
`a..b` — inclusive integer range. Fields `start`, `end`. Iterable in `for..in` (a counted
loop, contract C5); also `IIterable<int>` with an `iterator()` for protocol uniformity
(§6.4.8). Printable; spreads in array literals.

### 6.3 `Array<T>` — pure value semantics
Every method returns a **new** array; rebind to "change." `arr[i] = v` rebinds.
Planned efficiency: copy-on-write on the refcount (in-place when uniquely owned).

Construction: `[e1, e2, ...]` literal; `Array()` empty; `Array(int n, T fill)` sized+filled
(`T` inferred from the fill); bare declaration → `[]`.

`map`/`select`/`reduce` are **method-level generic** (Track 05 §1): the result
element type is inferred from the transform lambda's own return type, not
forced to stay `T` — `Array<string> r = a.map((n) => n.toString());` (`int`
→ `string`) type-checks and lowers correctly, including through chained maps.
This required a checker fix (lambda-last generic inference — value arguments
bind their type vars first, then each lambda argument's parameter types are
substituted in and its body's inferred return type binds the rest); see
`designs/complete/techdesign-05-stdlib-collections.md` §1 for the mechanism.

| category | members |
|---|---|
| core (native) | `length()`, `at(int)`, `add(T)` |
| indexer | `get ([])(int i)` |
| basics | `isEmpty()`, `first()`, `last()`, `firstOrNone()` → `T?`, `lastOrNone()` → `T?` |
| queries | `where(pred)` / `filter(pred)`, `any(pred)`, `all(pred)`, `count(pred)`, `contains(T)`, `indexOf(T)`, `indexWhere(pred)` (-1 miss), `find(pred)` → `T?` |
| transforms | `map<U>(fn)` / `select<U>(fn)`, `reduce<A>(seed, fn)`, `flatMap<U>(fn)`, `forEach(fn)`, `reverse()`, `take(int)`, `skip(int)`, `takeWhile(pred)`, `skipWhile(pred)`, `concat(Array<T>)`, `unique()` (dedup by `==`; **not** `distinct` — that's a reserved member-modifier keyword), `withIndex()` → `Array<Pair<int,T>>`, `groupBy<K>(fn)` → `Map<K, Array<T>>` |
| pure updates | `insertAt(int, T)`, `removeAt(int)`, `with(int i, T v)` (index-set, distinct overload from `Map.with`'s key-set — same vocabulary, no clash), `slice(int from, int len)` (throws on OOB, unlike `string.subStr`'s clamp) — all bounds errors throw `RuntimeException` |
| sorting | `sort((T,T)=>int cmp)` (**stable** merge sort — equal-key relative order preserved), `sortBy<K>(fn)` (duck-typed `<` on `K`; a `K` without `<` is an instantiation-time error), `minBy<K>(fn)` → `T?`, `maxBy<K>(fn)` → `T?` |
| relational | `join<U>(Array<U>, (T,U)=>bool)` → `Array<Pair<T,U>>`; `groupJoin<U>(...)` → `Array<Pair<T,Array<U>>>`; `zip<U>(Array<U>)` → `Array<Pair<T,U>>` (length = shorter of the two) |
| strings | `joinToString(string sep)`, `concatAll()` (native; O(total) — sum lengths, one allocation, memcpy each part; Track 04 M4). Declared on `Array<T>` generically (no specialization exists) but only meaningful, and only ever called, on an `Array<string>` — it's the engine behind `StringBuilder.toString()`. Excluded from the frozen ELF backend's lanes, same as `toInt`/`toFloat`/`byteAt`. |
| iteration / lazy | `iterator()` → `IIterator<T>` (protocol uniformity — §6.4.8; `for..in` over an array keeps its fast path), `asSeq()` → `Seq<T>` (the bridge into the lazy pipeline — §6.4.9) |

**Aggregates** (Track 05 §2) are free functions in `std`, not methods — `Array<T>`
has no specialization mechanism, so `int std::sum(Array<int>)` / `float
std::sum(Array<float>)` overload by argument type instead. `std::min`/`std::max`
return `T?` (`None` on empty); `std::average` always returns `float`.

### 6.4 `Pair<A, B>`
Fields `first`, `second`; constructor `Pair::Of(a, b)`. The element type of relational joins
and of Map iteration.

### 6.3.5 `StringBuilder` — a mutable accumulator (Track 04 M4)
```
class StringBuilder {
    StringBuilder add(string s);   // appends; returns `this` (chainable)
    StringBuilder (<<)(string s);  // sugar for add(s)
    int length();
    bool isEmpty();
    string toString();             // O(total) via Array<string>.concatAll()
}
```

A **class** (reference semantics — mutation across call sites is the point), unlike the
pure/immutable `Array`/`Map`. Backed by an `Array<string> parts` field: `parts = parts.add(s)`
is a **field self-append**, the same shape as the pure-array self-append pattern
(`a = a.add(x)`) but applied to a class field instead of a local. Making this the
sole-owner-at-call-time fast path required extending `Lower.cpp`'s COW-receiver handling
from locals to plain (accessor-free) class fields — general infrastructure, not
StringBuilder-specific, that benefits any class accumulating into a field this way. Verified
linear (not quadratic) on the emit-C++, LLVM, and frozen-ELF backends (100k `.add()` calls in
under 0.1s on each); the oracle and IR interpreter remain quadratic for self-append generally
— a separate, pre-existing gap in their shared native call path (bug.md), not something this
class's shape triggers. `(<<)` does not currently lower on the emit-C++ backend for
user-defined classes (bug.md) — use `.add(...)` there; `--run`/`--ir`/LLVM all support it.

### 6.4.4 Regex engine core (Track 10)

The regex core is implemented entirely in Leviathan prelude code and adds no natives. It
compiles byte-oriented patterns to a flat Thompson NFA (`Array<int>`), uses an ordered Pike VM
for leftmost-first captures, and uses a byte-equivalence-class lazy DFA for captureless boolean
queries. Literal, required-prefix, anchored, single-first-byte, and minimum-length prefilters are
selected at compile time. There is no backtracking, so matching remains linear in input length
times compiled-program size and user-supplied patterns cannot create a ReDoS path.

The engine syntax includes literals and escapes, `.`, ASCII classes and `\d`/`\w`/`\s`,
alternation, capturing/non-capturing/named groups, greedy and lazy quantifiers, anchors,
multiline mode, word boundaries, ASCII ignore-case, and dot-all. Backreferences and lookaround
are intentionally unsupported. Offsets and lengths are bytes.

`regex::compileProgram(pattern, flags) -> Array<int>` is the stable engine boundary used by the
public Regex library. It is pure and comptime-foldable; malformed constant patterns therefore
fail at their source line. The lower-level `programIsMatch`, `programFind`, `programCount`, and
batch hooks consume the compiled array. `programFind` returns capture slots as start/end integer
pairs (`0/1` is the whole match, then each group; `-1/-1` means non-participating). Most users
should use the public `Regex`/`Match` surface described by the companion library design rather
than this flattened boundary. `programPikeProbe(program, input) -> [matched(0/1), steps]` is a
**diagnostics-only** internal (not part of the public surface): it runs the Pike leg and reports
its deterministic work count, consumed by the `regex_pathological_linear` linearity gate
(techdesign-regex-linear-gate.md).

**Status:** landed in full, including the public surface — see §6.4.6. Most user code should
reach the engine only through `Regex`/`Match`/`Group`/`namespace regex`, not this flattened
boundary directly.

### 6.4.5 `Map<K, V>` — pure associative value
Insertion-ordered. Pure like Array: "changing" methods return a **new** map; `m[k] = v` is
rebind-sugar. Since `get`/`set` are keywords, the vocabulary is `at` / `with` / `without`.

Construction: `Map()` or bare declaration (`Map<string, int> m;`) → empty.

**Key equality (contract C3, Track 05 §4.2):** primitives compare **by
value**; `struct` keys compare **field-wise, recursively** (a struct IS its
fields — §9); class keys compare **by identity**. Landed in the tree-walk
oracle, bytecode interpreter, emit-C++, and LLVM backends; the frozen ELF
backend (`--emit-elf`) keeps the pre-C3 identity-only comparison for
struct/class keys permanently, by design (X64Gen is never extended past its
role as Track B's reference anchor) — primitive keys are identical on every
backend including ELF. **Engine caveat:** `.with()`/`.without()` are missing
from the LLVM and ELF backends' named-method dispatch entirely (bug.md #18)
— use the `m[k] = v` bracket-sugar form there instead of calling them by
name; the oracle, IR interpreter, and emit-C++ backends are unaffected.

| category | members |
|---|---|
| core (native) | `length()`, `at(K)`, `with(K, V)` → `Map<K,V>`, `without(K)` → `Map<K,V>`, `has(K)`, `keys()` → `Array<K>`, `values()` → `Array<V>` |
| indexer | `get ([])(K key) => at(key)`; `m[k] = v` rebinds |
| basics | `isEmpty()`, `atOrNone(K)` → `V?`, `atOr(K, V dflt)` → `V` |
| bulk | `entries()` → `Array<Pair<K,V>>`, `withAll(Map<K,V>)` (fold via bracket-sugar accumulate), `mapValues<U>((V)=>U)` → `Map<K,U>`, `whereEntries((K,V)=>bool)` → `Map<K,V>` |
| iteration | `for (Pair e in m)` — entries as `Pair<K, V>` (`e.first`, `e.second`); also `iterator()` → `IIterator<Pair<K,V>>` for protocol uniformity (§6.4.8), yielding the identical sequence in insertion order |

Printable: `console.writeln(m)` → `{a: 1, b: 2}`.

### 6.4.6 `Regex` — the public surface (Track 10, `techdesign-regex-library.md`)

The C#-shaped public API fronting the engine core (§6.4.4): a compiled `Regex` object,
plus `Match`/`Group` value types and `namespace regex` conveniences. All five public types
(`Regex`, `Match`, `Group`, `RegexOptions`, `RegexException`) are **top-level**, not
namespace-nested (qualified type names don't parse in type position yet, §12.6 of info.md).
Method/function names are camelCase (`isMatch`, not `Matches`); the first-match method is
`find`, not `match` (`match` is the pattern-dispatch keyword — one word must not mean two
things).

```
Regex userRe = Regex("^[a-z0-9_]{3,16}$");
bool ok = userRe.isMatch(name);

Regex kvRe = Regex("(?<key>\\w+)=(?<val>[^;]*)");
Match? m = kvRe.find(line);
if (m != None) { console.writeln(m.group("key")?.value ?? ""); }
```

**`Regex`** (reference semantics; share freely — it wraps a compiled `Array<int>` program):

| member | signature | notes |
|---|---|---|
| construct | `Regex(string pattern)`, `Regex(string, RegexOptions)`, `Regex(string, string flags)` | pattern-as-code: malformed **throws** `RegexException` |
| construct | `Regex::FromProgram(Array<int> program)` | O(1) wrap of an already-compiled program (comptime path below) |
| introspect | `pattern()`, `options()`, `groupCount()`, `groupNames() -> Array<string>` | declared `(?<name>…)` names, in declaration order |
| test | `isMatch(string)`, `isMatch(string, int from)` | |
| extract | `find(string) -> Match?`, `find(string, int from) -> Match?` | first match or `None` |
| extract | `matches(string) -> Array<Match>` | all non-overlapping matches |
| count | `count(string) -> int` | DFA-only; never materializes a `Match` |
| replace | `replace(string, string replacement[, int count])` | `$`-grammar below; replaces all by default |
| replace | `replace(string, (Match) => string fn[, int count])` | evaluator form |
| split | `split(string) -> Array<string>`, `split(string, int limit) -> Array<string>` | |
| batch | `isMatchAll(Array<string>) -> Array<bool>`, `countAll(Array<string>) -> Array<int>` | one warm engine across a column (DBMS shape) |

**`Match`** (a `struct` — a value, returned only on success):

| member | notes |
|---|---|
| `index`, `length`, `value` | byte offset/length and text of the whole match |
| `groups: Array<Group>` | `groups[0]` is the whole match (C# convention), then `1..n` |
| `group(int i) -> Group` | OOB throws `RuntimeException`, like `Array.at` |
| `group(string name) -> Group?` | `None` if no such declared name |

No `success` field: absence of a match is `None` (`find() -> Match?`) — there is already a word
for "no match" and it is not a flag on a hollow object.

**`Group`** (a `struct`): `matched: bool`, `index: int` (`-1` if `!matched`), `length: int`
(`0` if `!matched`), `value: string` (`""` if `!matched`). An unparticipating group inside a
*successful* match (an untaken alternation arm) is a real, reachable state — `matched` stays
`false` rather than the whole `Match` going absent.

**`RegexOptions`** (a `struct`, named-arg construction): `ignoreCase` (ASCII-only fold, v1),
`multiline` (`^`/`$` also match at `\n` boundaries), `dotAll` (`.` matches `\n` too — the
honest rename of C#'s confusing `Singleline`). `Regex(p, RegexOptions(multiline: true))`, or
the string-flags shorthand `Regex(p, "im")` / `Regex(p, "s")` — `i`/`m`/`s` in any order.

**`RegexException : Exception`** — `message` plus `int offset` (best-effort byte position in
the pattern, parsed from the underlying compile error; `-1` when a message carries no
offset). Thrown by every pattern-as-code path (constructors, the `namespace regex`
convenience functions below); catchable by contract as any `IException`.

**The `None`-vs-throw split (one rule):** pattern **as data** →
`regex::compile(pattern[, options|flags]) -> Regex?` — `None` on malformed, never a throw
(for patterns from config/user input). Pattern **as code** (a literal at the call site) —
constructors and the convenience functions below — **throws** `RegexException`: a malformed
literal is a programmer error and fails loud.

**`namespace regex` conveniences** (compile-per-call semantics, backed by a 16-entry LRU
pattern cache — correctness only depends on the cache being *present*, not on its eviction
order): `compile`, `isMatch`, `find`, `matches`, `replace`, `split`, `count` each have
`(s, pattern)`, `(s, pattern, RegexOptions)`, and `(s, pattern, string flags)` forms (and
`replace` additionally takes an evaluator lambda in place of a replacement string); plus
`escape(string) -> string` (quotes every metacharacter: `` \.^$|()[]{}*+? ``) and the engine's
own `compileProgram`/`programIsMatch`/… boundary (§6.4.4) for advanced/comptime use.

**Replacement-string grammar** (`replace`, parsed once per call before scanning): `$0`…`$99`
(group by number — greedy two-digit read, the longest valid group number wins), `${name}`
(named group), `$$` (literal `$`), a bare `$` before anything else or at the end is a literal
`$`. An unmatched group substitutes `""`. **A reference to a nonexistent group throws
`RegexException`** — replacement strings are code, and silent passthrough (C#/JS behavior)
is exactly the silent-distant footgun §16 of info.md bans.

> **Gotcha:** Leviathan's own string literals treat `"...${expr}..."` as interpolation
> (§3.2) — so a replacement template containing a named-group ref must escape it,
> `"\${name}"`, to deliver the literal three characters `${name}` to the regex engine
> rather than having the host language try to interpolate a variable named `name`.

**Semantics, pinned by the corpus (`tests/corpus/regex/`):**
1. **Leftmost-first, greedy-by-default** (Perl/C#/JS, not POSIX-longest) — falls out of
   the engine's Pike VM thread priority (§6.4.4).
2. **Empty-match advance:** a zero-length match at position `i` is reported, then the scan
   resumes at `i+1` (prevents the classic infinite loop in `matches`/`replace`/`split`).
3. **`split` includes captured-group text** in the output when the separator pattern
   captures (C# behavior, kept for porting fidelity); `split(s, limit)`: at most `limit`
   pieces, the remainder unsplit in the last piece.
4. **`replace` replaces all by default**; the `count` overloads bound it (differs from JS
   `String.replace`, matches C#).
5. **Byte-oriented v1:** offsets/lengths are bytes; `.` matches one **byte**, so a
   multi-byte UTF-8 scalar is not one `.` — see `string.chars()` (§9 of info.md) for the
   scalar view; codepoint classes are a deferred follow-up.
6. **`ignoreCase` is ASCII-only** in v1 (a compile-time fold in the engine).

**Comptime recipe** (a constant pattern compiles at **build** time; a malformed constant is a
build error at the pattern's source line, not a runtime surprise):

```
comptime Array<int> EMAIL_P = regex::compileProgram("^[\\w.+-]+@[\\w-]+\\.[\\w.]+$", "");
Regex emailRe = Regex::FromProgram(EMAIL_P);          // O(1) wrap, no parse at runtime
Array<bool> hits = emailRe.isMatchAll(column);        // batch: one warm engine per column
```

**Deferred** (named so they aren't re-litigated ad hoc): raw string literals (general
feature); a `(~)` match operator (blocked on bug.md #12); regex patterns in `match` arms;
a lazy `Seq<Match>` adapter; Unicode classes/folding; `escape`'s inverse (`unescape`);
`IgnorePatternWhitespace`; scalar-indexed offsets; columnar/`Block` scanning fused with
`where` (the DBMS endgame, arrives with dense string columns).

### 6.4.7 `Set<T>` — pure value semantics (Track 05 §4.1)
In-language over `Map<T, bool>`; zero natives. Insertion-ordered, same purity
model as Array/Map: every "changing" method returns a **new** Set. `with`/
`without` rebuild via bracket-sugar (never call `Map.with()`/`.without()` by
name — see the Map engine caveat above; Set is unaffected by it as a result,
and matches on all five backends including `--emit-elf`).

Construction: `Set()` empty; `Set(Array<T> items)` folds an array into a Set.

| category | members |
|---|---|
| basics | `length()`, `isEmpty()`, `has(T)` |
| pure updates | `with(T)` → `Set<T>`, `without(T)` → `Set<T>` |
| set algebra | `union(Set<T>)`, `intersect(Set<T>)`, `except(Set<T>)` — all pure, all `Set<T>` |
| conversion | `toArray()` → `Array<T>` (insertion order), `toString()` → `"{a, b, c}"` |

Key equality for `Set<T>` follows the same C3 rule as `Map` (it's built over
one) — `T` a struct compares field-wise; `T` a class compares by identity.

### 6.4.8 The iterator protocol (Track 07)
Two prelude interfaces make any type iterable by `for..in`:

```
interface IIterator<T> { bool hasNext(); T next(); }
interface IIterable<T> { IIterator<T> iterator(); }
```

`for (T x in e)`, when `e`'s static type implements `IIterable<T>`, desugars to

```
var __it = e.iterator();
while (__it.hasNext()) { T x = __it.next(); <body> }
```

`for (var x in e)` infers the loop variable from the `IIterable<T>` instantiation.
`break`/`continue` behave exactly as in the hand-written `while` (continue re-checks
`hasNext()`). A type that implements neither a built-in collection nor `IIterable<T>`
is a compile error naming the protocol.

**Dispatch order (contract C5, frozen).** The checker picks the loop's path
statically — there is no runtime probing:
1. a `Range` literal `a..b` → a counted loop (no object);
2. an `Array`/`Map`/`Range` value → the `IterLen`/`IterAt` fast path;
3. otherwise the protocol (`iterator()`/`hasNext()`/`next()` via ordinary dynamic
   dispatch — no new IR op, zero backend work).

Built-ins never reroute through the protocol (the fast paths are why arrays are
fast). `Array<T>`, `Map<K,V>`, and `Range` also *implement* `IIterable` (with
`ArrayIterator`/`MapIterator`/`RangeIterator`) purely for **uniformity** — so an
array or range can be passed where an `IIterable<T>` is wanted — but a `for..in`
over one still takes its fast path.

**`next()` past the end** is unspecified (iterators are driven by `hasNext()`); the
stdlib iterators throw `RuntimeException` (loud). **Invalidation:** stdlib iterators
are over pure values (an `Array` snapshot) and can never be invalidated; a user
iterator over a mutable collection is caveat-emptor, the same as any user code.

**Not iterable in v1:** strings (`s.chars()` returns an `Array` — explicit, avoids
the bytes-vs-scalars ambiguity) and `InStream<T>` (a pull on an empty live stream
throws — a for-loop over it is a foot-gun until blocking semantics exist).

### 6.4.9 `Seq<T>` — the lazy pipeline (Track 07)
A pure library over the protocol. **Arrays are eager; `Seq` is the opt-in lazy
form.** `array.asSeq()` is the bridge in.

| category | members |
|---|---|
| combinators (lazy) | `map<U>((T)=>U)` → `Seq<U>`, `where((T)=>bool)` → `Seq<T>`, `take(int)` → `Seq<T>`, `takeWhile((T)=>bool)` → `Seq<T>`, `skip(int)` → `Seq<T>` |
| terminals (pull) | `toArray()` → `Array<T>`, `firstOrNone()` → `T?` (short-circuits), `count()` → `int`, `forEach((T)=>void)`, `reduce<A>(A seed, (A,T)=>A)` → `A` |

**Laziness contract.** Nothing runs until a terminal (`toArray`/`firstOrNone`/
`count`/`forEach`/`reduce`) pulls; `map`/`where` functions run **at most once per
pulled element**; `firstOrNone` pulls exactly one matching element (the lazy
payoff). Terminal operations require a **finite** source — `take(n)` is the bound
for an unbounded one (there is no runtime guard, same stance as `while(true)`).

```
Array<int> r = nums.asSeq()
    .map((x) => x * x)
    .where((x) => x % 2 == 1)
    .take(3)
    .toArray();          // squares evaluated only for the 3 kept elements
```

**Implementation:** iterator-**composition** classes, not closures-holding-closures
(`MapSeq<T,U> : Seq<U>` wraps the source + fn; its iterator wraps the source's).
Closures capture by snapshot — wrong for a *stateful* cursor — so the wrappers hold
honest mutable cursor fields. Each `Seq` subclass holds its source and pulls it
through the **interface** type (`IIterable`/`IIterator`), never the class, so the
concrete subclass's `iterator()`/`next()` override runs (the language's dynamic
dispatch is interface-only).

### 6.5 `Promise<T>`
A plain library object — **no special language rules, no implicit wrapping**; a
promise-returning function must return an actual `Promise`. `new Promise(T value)` constructs a
resolved promise. `await` is the one privileged operation. (Async execution: **[planned]**.)

### 6.6 Streams — the system boundary
`StreamBuffer<T>` (a single-consumer **queue**; in-language, Array-backed today):
`push(v)`, `pull()`, `count()`, `isEmpty()`, `close()`. Typed views:
`InStream<T> : IDisposable` — `pull()`, `hasData()`, `subscribe((T) => void)`, `close()`;
`OutStream<T>` — `(<<)` (returns the stream: chainable);
`IOStream<T> : InStream<T>, OutStream<T>` — both ends over ONE collapsed buffer (the §13
diamond). `reader >>` extracts (== `pull()`). `subscribe` is a **standing pull**: it claims
the consumer end (later `pull` throws); broadcast is a library reshaping, not a stream
property. Pull on empty throws.

**Dispose / unsubscribe (SU-1, `designs/complete/techdesign-stream-unsubscribe.md`).**
`InStream<T>` is `IDisposable`, so a subscription is a resource: `using InStream<int> w =
signal::on(WINCH);` releases it on every scope-exit edge. `close()` is **idempotent and never
throws** (the `using` contract), runs an optional producer-attached teardown (a `signal::on`
stream's `close()` calls `signal::off`), then closes the backing buffer. On a closed
`StreamBuffer`, `push` is a **silent drop** (strict fanout cutoff — a closed consumer receives
zero further deliveries even mid-broadcast) and `pull`/`setHandler` throw the distinct
`"stream is closed"`. A plain in-memory `InStream` with no teardown attached still supports
`close()` (buffer close + no-op). For an `IOStream`, `close()` disposes the read-view
subscription and closes the shared buffer; subsequent `<<` pushes drop. Producer-side EOF
(loud push-after-close, `pullOrNone`, stream iteration) is deferred to streams-maturity (D-B).
Lanes: oracle/IR/LLVM full; emit-C++ compiles the in-memory surface; the signal stream itself
stays loop-bound-rejected on emit-C++ (unchanged).

**`signal::off(int sig, int subId)`** (free function) — the unsubscribe primitive `InStream`'s
teardown routes through; removes one subscriber from a signal's fanout and, when the **last**
subscriber leaves, `sysUnwatch`es the loop watch then `sysSignalClose`s the fd (the signal
returns to **default disposition** — a Linux-signalfd guarantee; on the self-pipe POSIX
fallback the mask unblocks but the handler stays installed). Idempotent; a one-shot program
that subscribes and disposes now **exits by loop drain** instead of the watch pinning the loop
forever.

### 6.6.5 `std::sys` — the syscall floor
`int sysWrite(int fd, string data)`; `string sysReadLine(int fd)` (returns `""` at end of
input — interim); `int sysOpen(string path, int flags)`; `int sysClose(int fd)`;
`string sysRead(int fd, int max)`; `int sysStat(string path, int field)` (0=exists,
1=size, 2=mtime, 3=isDir; -1 for missing paths — interim shape until a Block-backed
stat). `bool std::isDir(string path)` wraps field 3 alongside `fileExists`/`fileSize`/
`fileModified` (request-stat-isdir.md); one `stat(2)` word, correct even on an
unreadable directory where a `sysListDir(path) != None` probe misclassifies.
Track 08 F2/F3/F4/F6 add (see §6.6.58): `int sysMonotonic()`, `string sysRandom(int n)`,
`bool sysIsTty(int fd)`, `string? sysEnv(string key)`, `int sysMkdir(string path)`,
`int sysRemove(string path)`, `int sysRename(string from, string to)`,
`Array<string>? sysListDir(string path)`, `string? sysResolve(string host)`.
Track 08 F5/F7 add (see §6.6.57/§6.6.59): `int sysTcpConnectNb(string host, int port)`,
`int sysWatchWrite(int fd, (int) => void cb)`, `int sysConnectResult(int fd)`,
`Array<int> sysSpawn(string path, Array<string> args)`, `int sysPidfdOpen(int pid)`,
`int sysReap(int pid)`, `int sysKill(int pid, int sig)`.
Track 10 (threads, see §6.6.66) add: `int sysTcpListen(int port, bool reusePort)` (an
SO_REUSEPORT overload — LLVM/interpreters; the frozen ELF backend rejects the /2 form
loudly), `int sysCpuCount()` (online logical processors, ≥ 1; `std::cpuCount()` wraps it),
and `T sysThreadTransfer<T>(T value)` (the flatten/rebuild copy across a thread boundary).
LA-29 adds `int sysSocketBuffer(int fd, int sendBytes, int recvBytes)` (advisory socket
buffer sizing; oracle/IR/LLVM; ELF rejects like `sysTcpListen/2`; the first member of the
intent-named socket-option family — `techdesign-socket-options.md` §3 has the extension
recipe).
The only native seam for I/O, declared in-language;
collapses to `syscall(nr, ...)` + `Block` when the gated raw-memory features land.
sysWrite/sysReadLine work on every engine including native binaries; the file shims run on
the interpreters (native file I/O arrives with object coverage).

**Block I/O overloads — landed (Track 03 M4 I/O half, 2026-07-10).** The zero-copy
`Block` seam — `sysRead(fd, Block, max) -> int`, `sysWrite(fd, Block, off, len) -> int`,
`sysRecv(fd, Block, max) -> int?`, `sysSend(fd, Block, off, len) -> int`, and `File.read`/
`File.write` `Block` overloads — reads/recvs fill the `Block` from offset 0; writes/sends take
an explicit `[off, off+len)` window (every off/len bounds-checked, throws loud). The `Block`
forms are **arity-distinct** from the string forms, so overload resolution by argument type
selects, and every backend picks the right native by argument count. Coverage: **oracle + IR +
LLVM** (`lvrt_sys*_block` in `runtime/lv_runtime.c`); emit-C++ and the frozen ELF backend defer
the file/socket layer as they already do for the string forms. Only the ELF leg of M4 stays
deferred (`designs/deferal-char-block-abi.md`, `designs/complete/abi-addendum-lv-block.md` §6).

### 6.6.53 Process exit
`env::exit(int code)` terminates the program immediately and returns `code & 0xFF` to the
host process. It abandons pending event-loop work but runs the exit epilogue first, including
terminal raw-mode restoration. `env::setExitCode(int code)` records `code & 0xFF` for normal
completion, then lets execution continue and lets the implicit event loop drain. Multiple
`setExitCode` calls use the last value; `exit` does not return.

If an exception reaches top level uncaught, the program reports the usual `Uncaught ...`
message and exits with status `1`. A program that falls off the end without setting a code
exits `0`.

Both functions are process facts under `env`, alongside `env::args()` / `env::name()`. Their
native floor is `std::sysExit` / `std::sysSetExitCode`, so comptime code is denied by the
normal `sys*` hermeticity gate. The tree-walk and IR interpreters unwind cleanly so captured
stdout is flushed before the `leviathan` process exits. LLVM and emit-C++ support both forms;
the frozen pure ELF backend reports a clean deferral if either native is used.

### 6.6.58 System natives: env, time, dirs, tty, DNS (Track 08)
Track 08 (`designs/complete/techdesign-08-system-natives.md`) adds a family of floor natives
in `namespace std`. All are `sys*`-prefixed, so **comptime code is denied automatically** by
the hermeticity gate — a build never reads the clock, entropy, the filesystem, a tty, or DNS.
Optional returns carry the three-state fact (§9): `None` is distinct from `""` / `[]`.

- **`env::get(string key) -> string?`** — read an environment variable; `None` when unset
  (distinct from set-but-empty). The ergonomic wrapper over `std::sysEnv`, joining
  `env::args()` / `env::name()` / `env::exit()` under `env`.
- **`std::sysMonotonic() -> int`** — `CLOCK_MONOTONIC` milliseconds, for durations and
  deadlines (never jumps on a wall-clock adjust). Sibling of `std::sysNow()` (wall clock).
- **`std::sysRandom(int n) -> string`** — `n` cryptographically-random bytes carried in a
  string (the sys-floor convention until a `Block` overload lands). `n <= 0` → `""`;
  `n > 1MB` → `RuntimeException` (documented sanity bound).
- **`std::sysIsTty(int fd) -> bool`** — whether `fd` is a terminal.
- **`std::sysMkdir(string) -> int`**, **`std::sysRemove(string) -> int`** (unlinks a file,
  falls back to `rmdir` on a directory), **`std::sysRename(string, string) -> int`** — `0`
  on success, `-1` on failure (the `sysStat`/`sysOpen` scalar shape).
- **`std::sysListDir(string path) -> Array<string>?`** — entry names, with `.`/`..` omitted;
  `None` when `path` is not a directory / cannot be opened (distinct from an empty directory,
  which is `[]`).
- **`std::sysResolve(string host) -> string?`** — the first A record as a dotted-quad
  (`getaddrinfo`), or `None` on failure. Phase 1 (IPv4) — phase 2's in-language DNS client
  over a UDP floor is roadmap.

**Engine coverage.** All of the above run on the tree-walk (oracle) and IR interpreters — the
design's semantic reference, and the interpreter-first landing the track mandates.
`sysMonotonic` additionally runs on LLVM-native binaries (wired with the F5 socket floor —
deadline flows measure elapsed with it). The remaining natives on the compiled backends
(emit-C++, LLVM) and the frozen pure-ELF backend keep **clean coverage-errors** naming the
native (emit-C++ skips the system layer by design; LLVM's floor follows the `sysNow`
precedent when a consumer needs it; ELF/X64Gen is frozen). Using one of these natives on a
compiled backend is a clean compile-time diagnostic, never a miscompile.

### 6.6.59 Process — child processes (Track 08 F7)
`Process(path, args)` spawns a child by **explicit path** (no PATH search — the honest v1
boundary; a PATH-walking helper can be written in-language over `sysEnv`+`sysStat`), with
argv `[path] + args` and three pipes wearing the stream surface (pipes are fds — the socket
machinery takes them unchanged):

- `ok() -> bool` — spawn succeeded (pipes + fork; a **bad path is not a spawn failure**:
  the child's exec fails and its exit code arrives as `127`).
- `write(string)` — stdin, queue-and-drain like a socket send; `closeStdin()` — EOF is the
  protocol for `/bin/cat`-style children.
- `onStdout((string) => void)` / `onStderr((string) => void)` — chunk streams riding the
  ordinary read-watch loop.
- `exitCode() -> Promise<int>` — non-blocking reaping via a **pidfd watch** (no `SIGCHLD`
  handler, no blocking `waitpid`): resolves with the exit code (`128+signal` if
  signal-terminated, the shell convention; `127` on spawn failure), delivers any output
  still buffered in the pipes first, then closes every owned fd (pipes ×3 + pidfd) — a
  spawn-churn loop leaves the fd table at ±0. A program that never calls `exitCode()` keeps
  no reap machinery alive and may exit with live children (standard Unix: they are not
  reaped).
- `kill()` — `SIGTERM` via `sysKill(pid, 15)` (with `exitCode()` pending, the promise then
  resolves `143`).

Floor: `sysSpawn(path, args) -> Array<int>` (`[pid, stdinFd, stdoutFd, stderrFd]`, `[]` on
spawn failure), `sysPidfdOpen(pid)`, `sysReap(pid)` (`-1` still running, else the code),
`sysKill(pid, sig)` (`pid <= 0` refused at the floor — the `kill(2)` broadcast forms are
never exposed). All `sys*`-prefixed → comptime-denied automatically. Oracle + IR + **LLVM**
(G-LANG-2 process half, 2026-07-16: `runtime/lv_proc.c` over the `lv_plat_spawn/pidfd_open/
reap/kill` floor, `designs/techdesign-spawn-llvm.md`; Windows targets reject at compile time,
the threads precedent). emit-C++ still defers cleanly (deliberate system-layer policy).

### 6.6.54 Promises and await
`Promise<T>` (`resolve(v)`, `isReady()`, `get()`, `then(cb)`; construct `Promise()` pending
or `Promise(v)` pre-resolved) is a one-shot stream. `await expr` unwraps `Promise<T>` to `T`,
parking on the event loop until it resolves. No `async` keyword and no function coloring —
await is permitted anywhere. A function returning `Promise<T>` is the async form.

### 6.6.55 Timers and the event loop
A timer is a **system stream**: the runtime loop pushes tick numbers (1, 2, …) into a real
`StreamBuffer`, so `subscribe`/`pull` are the ordinary stream machinery.

`std::after(ms)` — one-shot; `std::every(ms)` — repeating. `Timer` members: `ticks() ->
InStream<int>`, `subscribe((int) => void)`, `cancel()`.

**Program lifetime (the implicit loop):** after top-level completes, the program keeps
running while live work remains (pending timers; sockets next) and exits when none does —
one-shots release themselves after firing; `cancel()` releases a repeating timer. Dispatch
is single-threaded: callbacks never race. Multiple due timers fire in (due-time,
creation) order. An uncaught exception in a callback stops the loop and reports as usual.
Floor: `sysTimerStart(delayMs, intervalMs, cb)` / `sysTimerCancel(id)`.

### 6.6.57 Sockets and HTTP
On the event loop, driven by fd read- and write-watches. `TcpStream` — `(<<)`/`send`,
`onData((string) => void)`, `onClose(() => void)`, `close()`; reads come from `sysRecv ->
string?` (`None` = peer closed). **`send` never silently short-writes** (Track 08 F5.6):
the fd is non-blocking, so a large payload's unsent tail is buffered and a write-watch
drains it as the kernel makes room; a fatal send (peer gone) drops the buffer and the read
side delivers the close. `TcpListener` — a **stream of connections**:
`connections((TcpStream) => void)`, `stop()`.

**Connect with a deadline (Track 08 F5):** `std::connectTimeout(host, port, ms, (int) =>
void cb)` — `cb` receives the connected fd (ready for `TcpStream`), or `-1` on refusal /
unreachability / a bad literal / the deadline. Entirely in-language over the floor trio:
`sysTcpConnectNb(host, port)` (non-blocking connect: the fd comes back even mid-handshake,
`-1` only on immediate failure), `sysWatchWrite(fd, cb)` (write-readiness watch — same
registry and `sysUnwatch` as read watches), `sysConnectResult(fd)` (`SO_ERROR` verdict:
`0` = connected). **IPv6:** a bare host literal containing `:` selects `AF_INET6` on
`sysTcpConnect`/`sysTcpConnectNb` (bracket forms like `[::1]:port` stay in URL code;
`sysTcpListen` remains v4 loopback — it takes no bind address yet).

Floor: `sysTcpConnect(host, port)`, `sysTcpListen(port)`, `sysAccept(fd)`,
`sysSend(fd, data)` (bytes written; `-1` retryable/would-block, `-2` fatal — and pipes take
the same call: `ENOTSOCK` falls back to `write(2)`), `sysRecv(fd, max) -> string?` (also
pipe-capable), `sysWatch(fd, cb)` / `sysWatchWrite(fd, cb)` / `sysUnwatch(id)`, and
`sysSocketBuffer(fd, sendBytes, recvBytes)` (advisory `SO_SNDBUF`/`SO_RCVBUF` sizing —
best-effort; the kernel clamps; a non-positive direction is left unchanged; `0`/`-1`). The
full socket floor including the F5 trio runs on the oracle, IR, **and LLVM-native** engines
(the write-watch landed in the compiled runtime's loop alongside the interpreters).

HTTP (in-language, over TCP streams; Track 09 F4 — framework-grade, zero new natives):

- **`HeaderMap`** — an ordered, case-insensitive multimap of `Header { name; value }`:
  `add`/`set` (replace-all-then-append), `first(name) -> string?`, `all(name) ->
  Array<string>`, `has`, `remove`, `entries() -> Array<Header>` (order + duplicates kept,
  so `Set-Cookie` survives), `render()`.
- **`HttpRequest`** — `method`/`path`/`version`/`body`/`headers` (a `HeaderMap`),
  `header(name) -> string`. `parse(raw)` for a buffered request and an incremental
  `feed(chunk) -> bool` state machine (head then body by `Content-Length`; returns true when
  complete; v1 does not pipeline).
- **`HttpResponse(status, body)`** — `headers` (`HeaderMap`), `withHeader(name, value)`,
  `reason()`, `render()` (computes `Content-Length` + `Connection`), and `parse(raw)` which
  **decodes a `Transfer-Encoding: chunked` body transparently**.
- **`ChunkedDecoder`** — a fragmentation-proof chunked-transfer decoder (`feed(chunk)`,
  `isDone`); `std::chunkEncode(data)` / `std::chunkEnd()` are the encoder side.
- **`HttpServer(port)`** — `handle((HttpRequest) => HttpResponse)`. Server-side **keep-alive**
  (a connection with neither side sending `Connection: close` is re-armed for the next
  request, bounded by 100/connection) and a **500 error path** (an uncaught throw in a handler
  becomes `500` + `Connection: close`, and the loop survives).
- **`HttpClient`** — `request(method, host, port, path, HeaderMap, body, cb)`, `get`/`post`
  sugar, and the await-able `fetch(host, port, path) -> Promise<HttpResponse>`.

Still text bodies until `Block`. Deferred (roadmap): client redirects, URL-string parsing,
request timeout, HTTP pipelining, client-side chunk-send, client connection pooling.

### 6.6.5x TLS + crypto (LA-2 — oracle + IR + LLVM; `designs/complete/techdesign-tls-crypto.md`)

**The fd IS the socket fd** (wrap-in-place, no new descriptor). After arming TLS over a
connected/accepted socket, `sysSend`/`sysRecv`/`sysClose` route through the session
transparently and `sysWatch*` keep polling the raw fd — so `TcpStream`, `HttpServer`, and
`HttpClient` gain TLS with **zero API change**. Backed by system OpenSSL (≥ 1.1.1) behind a
narrow provider seam; a build without OpenSSL ships a clean not-built provider (TLS calls
report `"TLS support not built into this runtime"`; plaintext programs are unaffected).

**Floor natives** (all `sys*` ⇒ comptime-denied): `sysTlsConnect(fd, host, alpn="",
caFile="", verifyMode=0)` / `sysTlsAccept(fd, certPath, keyPath, alpn="")` arm a session
(return the same fd, or `-1` with `sysTlsError(fd)` explaining); `sysTlsHandshake(fd)` steps
it (`0` done / `1` want-read / `2` want-write / `-1` failed); `sysTlsError`/`sysTlsAlpn(fd) ->
string`, `sysTlsVersion(fd) -> int` (`12`/`13`/`0`). **verifyMode:** `0` full (chain +
RFC 6125 hostname — `HttpClient`'s only mode), `1` chain-only, `2` encrypt-only (no
verification; greppable, never a default). `caFile ""` = system roots (plus the standard
`SSL_CERT_FILE`/`SSL_CERT_DIR` env), non-empty = an **additional** trust anchor. IP-literal
hosts send no SNI and verify IP SANs. **Posture (normative):** TLS 1.2 floor + 1.3 on, verify
ON by default, no renegotiation/compression; `SSLKEYLOGFILE` (env) enables NSS key-log lines,
off by default.

**Drives (loud, in-language):** `std::tlsConnect(fd, host, alpn, caFile, verifyMode, cb)` —
arm + drive + throw `RuntimeException("TLS handshake: <reason> …")` on verification failure,
`cb(fd)` on success; `std::tlsAccept(fd, cert, key, alpn, deadlineMs, cb)` — server-side, a
failed/slow client handshake drops the connection (`cb(-1)`), never unwinds the accept loop;
`std::tlsDrive(fd, cb)` — drive an already-armed fd. **HTTPS:** `HttpServer(port, cert, key)`
and `HttpClient.requestTls/getTls/postTls/fetchTls` (always full verification). The event loop
handles the two classic TLS/loop bugs — **want-direction inversion** (a mid-handshake read
that needs the fd writable, and vice versa) and **buffered-plaintext stall** (`SSL_has_pending`)
— with a poll-mask augmentation that is **bit-identical to the plaintext loop when no session
is live**.

**Crypto:** `std::sysRsaEncrypt(pubKeyPem, bytes, padding="oaep") -> string?` — RSA
public-key encrypt for auth key-transport (`"oaep"` = OAEP/SHA-1, `"pkcs1"` = PKCS#1 v1.5;
`None` on parse/capacity/encrypt failure). `std::sysRandom(int n) -> string` is guaranteed
**crypto-grade** (kernel CSPRNG: `getrandom` / `/dev/urandom` fallback) on every covered
engine including the LLVM leg. **Coverage:** oracle + IR + LLVM full; emit-C++ / frozen ELF
carry the existing clean deferral. Deferred: mTLS, session resumption, cert hot-reload,
cipher-policy surface, OCSP, Windows/macOS native providers, HTTP/2, `Block`-era AEAD.

### 6.6.66 Threads / workers (Track 10 — live on all three active engines)

Workers are the concurrency execution layer (`designs/complete/techdesign-threads-2.md`,
`-3.md`). The model is
**pure isolation with copy-always boundaries**: a worker captures its inputs by copy, runs,
and returns a result; every value that crosses a thread boundary crosses by **deep copy**
(flatten/rebuild), so a counted value lives on exactly one execution unit. Immutable
zero-copy sharing is a v2 optimization.

- **`Worker<T> std::spawn<T>(() => T body)`** — start a worker; the handle **is a
  `Promise<T>`** (`Worker<T> : Promise<T>`), so `await w` is the join (no second suspension
  surface). The body's captures are **snapshotted at the `spawn` call**, so mutating a
  captured original afterward is not observed by the worker.
- **`Channel<T>`** — a single-producer / single-consumer conduit. `new Channel(int capacity,
  int policy)` where `policy` is `std::overflowBlock()` / `overflowDrop()` / `overflowError()`
  (capacity comes *with* a policy; `grow` is rejected). `send(T)` copies the value in;
  `receive() -> Promise<T?>` resolves with the next item or **`None` once the channel is
  closed AND drained** (the receiver narrows with `!= None`); `close()`. `send` on a closed
  channel throws; a captured Channel handle is a **portal** (both units share the one
  endpoint).
- **Flattenability (§6).** What may cross a boundary: primitives, `char`, `None`, ranges,
  `struct` values, strings, pure `Array`/`Map` of flattenable elements, and statically-shaped
  `class` objects (deep, with shared substructure/cycles preserved). Rejected — a loud,
  catchable error naming the type: a **nested closure** (only the spawn body itself may cross
  as a closure), an **fd-/loop-bound carrier** (`TcpStream`/`TcpListener`/`Timer`/`Process`,
  and a **disposable `InStream`** — one whose `hasDispose` teardown reaches a live signalfd +
  loop watch, e.g. a `signal::on` subscription; a plain in-memory `InStream` still crosses —
  each worker opens its own), and a **`Block`**. (v1 residual, uniform across every carrier: a
  carrier reached through a bare *global* rather than a captured local is not caught — only a
  global *Promise* is re-scanned at the spawn call. Keep loop-bound carriers on their owning
  thread; pass a `Channel<T>`.)
- **`std::cpuCount()`** — online logical processors (≥ 1), for `HttpServer(port, workers:
  cpuCount())`-style sizing; `sysTcpListen(port, reusePort: true)` sets SO_REUSEPORT so N
  workers can each open a full accept loop on one port.

**Engine coverage.** The surface runs on all three active engines by construction, byte-for-
byte identically (one shared flatten/rebuild walk). On the **tree-walk oracle and IR
interpreter** a worker is a cooperative loop task. On the **LLVM backend** a worker is a
**true OS thread** (`designs/complete/techdesign-threads-3.md`): its own per-worker TLS heap /
arena / event loop, real `pthread`s, an `eventfd` join, and reap-time `munmap` of the worker's
regions — the result rebuilds back on the *spawner's* thread, so a `Worker`/`Promise`
continuation is only ever run by the thread that owns it. Values cross by a C flatten/rebuild
engine (a self-contained relocatable buffer with a seen-map for shared substructure and
cycles); `Channel<T>` on LLVM is a process-global lock-free SPSC ring plus two `eventfd`s. A
capability probe (`sysThreadStart` returns `-1` on the interpreters, a join fd on LLVM) drives
one prelude `spawn` over both legs. **Threads are POSIX-only in v1:** a `--target *windows*`
build rejects any `spawn`/`Channel` at compile time (`threads: unsupported on Windows (v1)` —
`LV_TLS` is deliberately non-TLS there); the **frozen ELF backend** rejects
`spawn`/`Channel`/`sysTcpListen/2` at compile time. **A `Worker<T>`/`Promise<T>` handle may
NOT cross a thread boundary** — a spawn body may not **reference** one, whether captured
through a local/container **or** through a bare top-level global (both reject identically at
the spawn call, before any worker starts), and sending one through a channel is likewise a
loud, catchable error naming the type ("pass a Channel"); `Channel<T>` is the one sanctioned
cross-worker conduit (a cross-thread `resolve` would run the promise's continuation on the
wrong thread). Pass a `Channel<T>`, or `await` the `Worker<T>` handle `spawn` returns. Output-determinism discipline for worker programs: **workers compute and
return/send; only the spawning thread prints, at join points** — never print from a worker
body (it would race stdout ordering under real threads). What `await` itself does — parking
the current **task**, resumption order, and failure at an await — is §6.6.67; `await w` on a
`Worker<T>` is that same machinery applied to the join.

### 6.6.67 Await and tasks (LA-30 — stackful suspension, live on oracle/IR/LLVM)

**`await expr`** — suspension point. If `expr` is not a promise-shaped object, `await`
yields it unchanged. If the promise is already settled, `await` proceeds **without
suspending** (no scheduling point — deterministic and free; the C# completed-task
rule, not JS's always-defer rule). Otherwise the current **task** suspends: other
tasks on the same thread — including new work the event loop dispatches — run while it
waits, and the task resumes after the promise settles, **in completion order** among
this thread's runnable tasks. Resumption order is FIFO per thread; it is **not** the
reverse-of-suspension (stack) order the pre-LA-30 pump implied, and programs must not
depend on either beyond what this paragraph states.

A task is the unit of suspension: the program top-level is a task, and every callback
the event loop dispatches (timer, watch, worker-join) runs as a task. Tasks are pinned
to their thread; `await` never moves work across threads (`Channel<T>`/`Worker<T>`
remain the only crossings — §6.6.66). Across any `await`, state on the current thread
may be observed and mutated by other tasks that ran meanwhile — single-threaded
interleaving is real even without data races; `await` marks exactly the points where
it can happen.

Because `await` on a settled promise never enters the scheduler, a loop of ready-awaits
is compute, not yielding — fairness among runnables starts at the first real suspension
(same as the pump; stated because the FIFO sentence above would otherwise imply a yield
that doesn't exist).

**Failure at an await.** A `Worker` that rejected rethrows its failure at the `await`
(catchable — §6.6.66, unchanged). An `await` whose promise can no longer be settled —
the thread's event loop has fully drained with the promise still pending — throws a
catchable `RuntimeException`: `await: event loop drained with promise unresolved`.
An uncaught throw inside a loop-dispatched callback terminates the program through
the standard uncaught path; it is **never delivered to an unrelated `await`**. The
canonical why — under the retired pump this program printed `caught: boom`, an
exception teleported into a handler that had nothing to do with the timer:

```
Promise<int> p = Promise();                  // nothing ever resolves p
std::sysTimerStart(5, 0, (n) => { throw RuntimeException("boom"); });
try { int x = await p; } catch (IException e) {
    console.writeln("caught: " + e.message);  // never receives the timer's boom
}
```

Under tasks the callback is its own task: the throw is program-uncaught
(`Uncaught RuntimeException: boom`, exit 1 — the exit-codes contract), and `await p`
subsequently drains with its own catchable throw above.

`await` inside hermetic comptime remains a compile error; `then(cb)` still runs
synchronously in the resolver's context, exactly as the prelude writes it. The old
pump remains reachable via `LANG_PUMP=1` (process-global escape hatch) until its
scheduled deletion; stats ride `LANG_TASK_STATS=1` (a `[tasks]` line on exit, the
`LANG_THREAD_STATS` convention).

**Coverage.** Oracle, IR, LLVM — full (per-task mmap'd stacks; 128 MiB virtual,
lazily committed, on the interpreters — fat C++ frames — vs 1 MiB compiled;
`LANG_TASK_STACK` overrides; overflow is a loud guard-page diagnostic).
emit-C++ — no async surface (unchanged deferral). ELF — frozen, not a target.
Windows — tasks await the win32 fiber leg; the wine lane is explicitly
`LANG_PUMP`-pinned until then (threads are POSIX-only in v1 anyway, §6.6.66).

### 6.6.68 Cancellation, timeouts, structured concurrency (LA-30 B2 — live on oracle/IR/LLVM)

**Cancellation is an exception delivered at park points, and only there.** A running
task is never preempted — the mark takes effect at its next `await`. `CancelledException
: Exception` (implements `ICancelledException : IException`) is an ordinary catchable
carrier, no second error channel:

```lev
interface ICancelledException : IException { }
class CancelledException : Exception, ICancelledException { }
```

A task may catch it and refuse cancellation (the same rethrow-point machinery as
Worker-failed and drained-loop above); an **uncaught** `CancelledException` in a
group-owned task is absorbed at the group boundary (cancellation is not a program
error) — an uncaught throw of any other type stays program-uncaught, unchanged.

**`TaskGroup`** — structured concurrency via the existing `using`/`IDisposable` rule
(§6.6.65), zero new syntax:

```lev
class TaskGroup : IDisposable {
    new TaskGroup();
    void run(() => void body);   // start a child task, owned by this group
    void cancelAll();            // mark every live child cancelled
    void close();                // cancelAll(), then join every child
}
```

`using TaskGroup g = TaskGroup();` runs `close()` on every exit edge of the block
(fall-off, return, throw, break — §5.2's guarantee): stragglers are cancelled and every
child is joined before the block's scope is left, so no task outlives its lexical
scope. `g.run` is a same-thread task (cheap, cancellable) — deliberately not `spawn`
(§6.6.66's thread-backed `Worker<T>`, parallel and uncancellable); the two never blur.
Cancellation delivery is masked while a task runs its own `close()` unwind (the shield
rule) so `using` nested under a cancelled task cannot livelock; a child that catches
`CancelledException` and refuses to park again leaves `close()`'s join waiting — loud,
not hung (a `[tasks] uncancellable=N` report on `LANG_TASK_STATS=1`), and the join still
completes once the refuser eventually parks or returns.

**`awaitTimeout`** — timeout as an outcome, not a failure:

```lev
T? awaitTimeout<T>(Promise<T> work, int ms);
```

Parks on `{work, a timer}`; returns `None` on timeout (the `T?`/union rule — expected
absence, no exception) or `work`'s value if it settles first. **v1 stops waiting; it
does not cancel `work`** — no `Promise → producer-task` map exists to reach back into.
A `Worker` (OS thread) is never cancelled by timeout either way — threads cannot be
wall-clock killed (§6.6.66). Compose with the enclosing group's `cancelAll()` for a
kill-switch shape: `int? r = awaitTimeout(p, 5000); if (r == None) g.cancelAll();`.

**Coverage.** Oracle, IR, LLVM — full, same substrate as §6.6.67 (task-struct cancel
mark, per-task shield-mask counter, a thread-local id→task registry — ids, never
handles, cross the native boundary). emit-C++ — no async surface (unchanged
deferral). ELF — frozen, not a target.

### 6.6.6 Files
`OpenMode` — flag values combined with the **`(|)` operator method** (not a union type):
`std::read | std::write | std::append | std::binary` (`binary` inert until `Block`).
Type-safe: `File` takes `OpenMode`, not `int`.

`File(path, mode)` opens on construction; failure throws
`FileException : Exception, IFileException`. Members: `open()`, `close()`, `isOpen()`,
`write(s)`, `writeln(s)`, `readln()` (text; `""` = end), `read(max)`, and stream views
`reader() -> FileInStream` (`pull()` / `>>` extract), `writer() -> FileOutStream` (`<<`).
Attributes: `exists()`, `size()`, `modified()` on File, and path-level
`std::fileExists/fileSize/fileModified/isDir(path)` (usable without opening). `File : IDisposable`
(§6.6.65) — `using File f = File(path, mode);` closes it deterministically on every scope exit
(techdesign-02 F3; landed, §19 #8 resolved). Planned: locking (with concurrency), binary +
`seek` (with `Block`).

### 6.6.65 `IDisposable` — deterministic cleanup (techdesign-02 F3)
```
interface IDisposable { void close(); }
```
The contract `using` (§5.2) requires. Prelude conformers: `File`, `TaskGroup` (§6.6.68),
and `InStream<T>` (§6.6, SU-1); user classes implement it the same way to opt a resource
into `using`.

Namespace-level **globals** (e.g. `std::read`, and `std::in/out` next) are initialized from
the prelude before the program runs. `std::read/write/append/binary` are `const`
(§4.3b): they're facts, not variables — `std::read = std::OpenMode(9);` is a
compile error (`cannot assign to const 'read'`), where it used to silently accept
and no-op the write.

### 6.6.6a Capability interfaces (`system-binds.md`)

Five thin, type-keyed alternatives to the ambient globals above, gated behind ordinary
`bind`/`inject` (§4.7) rather than a new mechanism. All in `namespace std`, so the implicit
`uses std;` makes the *names* visible everywhere — visibility never means provision; only an
explicit `use std::I...;` (Channel 1, §4.7) activates a capability's root bind.

```
interface IEnv        { Array<string> args(); string? variable(string name); }
interface IConsole    { void write(string s); void writeln(string s); void writeln(); }
interface IClock       { int now(); }                                   // epoch ms
interface IFileSystem { File open(string path, OpenMode mode); bool exists(string path); }
interface INet         { TcpStream? connect(string host, int port); TcpListener listen(int port); }
```

Each has one system implementation (`SystemEnv`, `SystemConsole`, `SystemClock`,
`SystemFileSystem`, `SystemNet`) — a stateless shim delegating to the matching ambient surface
(`env::args/get`, `console`, `std::sysNow`, `File`, `TcpStream`/`TcpListener`) — root-bound fresh
per injection (`bind IEnv => SystemEnv();`, ...), not a shared `const` global instance: an eager
global forces emit-C++ to mark the shim's whole class reachable unconditionally, which for
`IFileSystem`/`INet`/`IEnv` pulled in natives that backend doesn't implement and broke every
program's `--build`, not only ones using a capability. The shim is the *only* implementation
underneath either path, so injected and ambient code observe identical behavior regardless.
`IEnv.variable` reads `env::get` (`None` = unset, distinct from set-but-empty);
`INet.connect` returns `TcpStream?` (`None` on refusal, unlike a raw fd's silent no-op writes);
`IFileSystem`/`INet` gate *acquisition* (they hand back the real `File`/`TcpStream`), not the
streams' own I/O surface. A capability's contract is a **claim, not an enforced sandbox**: the
ambient globals remain reachable from anywhere regardless of what a signature says — the
guarantee is that the disciplined path (one injected parameter) is now cheaper than the ambient
one, not that the compiler forbids the latter.

**Fixed footgun (bug #70, `known_bugs_1.md`, fixed 2026-07-14):** a local/parameter whose name
shadowed an existing namespace (`env`, `math`, `term`, ...), dynamically dispatched (any
interface-typed value qualifies) and calling a method that namespace *also* declared with the
same arity, used to silently call the NAMESPACE function on `--ir`/native backends instead of
the actual receiver. `Lower.cpp`'s namespace-call fallback now checks the base name isn't a
local/parameter first, so an `IEnv`-typed value may be spelled `env` again.

### 6.7 `console` — a real object
`console` is a prelude global of class `Console` — resolved, type-checked, and
aliasable like any other value (the name is no longer a compiler special case):

```
class Console {
    void write<T>(T v);        // native: stringify + write to stdout
    void writeln<T>(T v);      // native: stringify + newline
    void writeln();            // native: bare newline
    Console (<<)(string s);    // transfer operator — chains: console << a << b
}
const Console console = Console();
```

`write`/`writeln` stringify any single value through the generic `T` (bound per
call, §2.5); `(<<)` gives it the out-stream operator surface (§6.6). Aliasing
works (`Console c = console; c.writeln(x)`); a formal `IOutStream` interface
awaits the streams unification. Inside `comptime` code, console output is
emitted during compilation (see §8). Engine note: `console << s` runs on the
oracle, IR, and pure-ELF engines; emit-C++ does not cover object transfer
operators yet (it reports its coverage error, like the stream types).

`console` is `const` (§4.3b): a fact declared once at prelude init, not a variable
— `console = Console();` is a compile error (`cannot assign to const 'console'`),
where it used to silently accept and no-op the write.

### 6.8 `System`
`System` remains an unmodeled builtin name (reserved).

---

## 6.9 Compile-time metaprogramming (§16.5, Phase 1)

The rule stage runs between resolve pass 1 and a full re-resolve/check, so
folded code is checked and compiled exactly like hand-written code on every
engine. Programs with no metaprogramming surface skip the stage entirely.

### Attributes (Layer A) — inert, typed annotations
```
attribute Route { string method; string path; }   // fields ARE the arguments
attribute Column { string name = ""; }             // defaulted -> bare @Column

@Route("GET", "/users")                            // positional, comptime-const
Array<User> list() => ...;
@App::Tag(3) void f() { }                          // qualified form
```
- `attribute Name { fields }` declares one; fields must be `int`/`float`/
  `bool`/`string` (they are the typed argument surface). Methods/ctors/
  accessors inside an attribute are errors.
- Attributes attach to classes, structs, interfaces, members, functions,
  globals, and namespaces. They do **nothing** until a rule reads them;
  arguments are type-checked against the fields and must be comptime-evaluable.
- **Scope (per-file):** an unqualified `@Name` resolves through the namespaces
  the *declaring file* imports — its own namespaces, its `uses` list, any
  namespace it draws a selective `use NS::name` from, and `std`. (A selective
  `use NS::x` opts the file into `NS` at namespace grain, exactly as it does for
  phantom-dep purposes — §4.1.) Ambiguity across imports is an error; qualify
  (`@Web::Route`).

### Rules (Layer B) — match a shape, inject quasiquoted code
```
namespace Web {
    interface IController { }
    attribute Route { string method; string path; }
    rule registerRoutes {
        match @Route(r) on method m in class C : IController
        inject `router.record($r.method, $r.path)` at bottom of C.constructor
    }
}
```
A rule is `match <shape> inject <template> at <anchor>`, declared in a
namespace. It fires on a declaration **only in files that import the rule's
namespace** (the same import graph that scopes attributes — a bulk `uses NS` or
a selective `use NS::name` both opt the file in) — read a file's imports to know
every rule that can touch it.

- **Match** binds by structural shape: `@Attr(bind)` (an attribute on the decl,
  binding its evaluated value), `on <kind> m` (the decl: `method`/`function`/
  `class`/`struct`/`field`/`constructor`/`interface`/`namespace`), and
  `in <kind> C : IFace` enclosers (an enclosing decl, optionally constrained to
  implement/extend a type — checked against the resolved base chain). `match one`
  requires at most one matching attribute.
- **Inject** splices a quasiquote template at a named anchor. **Holes** are
  `$name`: `$r.method` reifies a field of a bound attribute value to a literal;
  `$m` / `$C` splice a bound declaration's name (e.g. `this.$m` → the method's
  selector). A hole also reifies/splices in these positions:
  `$C.name` / `$m.name` reifies the matched decl's **simple name as a string
  literal** (the ergonomic form; no `[ $for x in [C] : $x.name ][0]` dance);
  a `$for`-bound `meta::Method`/`meta::Field` value used **in member-selector
  position** (`t.$m()` where `m` ranges over `C.methods`) splices its name as
  the selector — so a rule can both discover members and *invoke* them; and
  `$C` **in type position** (`$C t = $C();`) carries the matched class's
  identity through pass-2, so it resolves to the class even when the same
  injection declares a same-named value (e.g. `bool $C = …`, the type-position
  analogue of the call-position hygiene). Anchors (v1): `top`/`bottom of
  C.constructor` (a nullary constructor is synthesized if the class has none;
  `top` inserts after base constructor calls) and `member of C` (adds a member,
  erroring on a same-name same-type collision). `top`/`bottom of body`,
  `marker`, and `namespace N` anchors, plus `where` predicates, `$for` list
  splices, and expression macros (`macro name(e) => \`…\`;` / `name!(arg)`),
  all ship. **Body-replacing rules (Layer D, `rewrites` / `replace` / `$body`)
  also ship** — see below.
- **Additive + hygienic (by default):** ordinary rules only add code (no silent
  rewrites); the loud, explicitly-marked exception is a `rewrites` rule (below).
  A local a template declares is alpha-renamed to a fresh symbol, so injected
  code neither captures nor is captured by use-site names. Injected code goes
  through the normal resolver/checker/backends — it is **cost-identical** to
  hand-writing the same code (zero runtime reflection).
- `leviathan --rules file.ext` lists every rule firing; `leviathan --expand
  file.ext` re-emits the post-rules tree as **compilable source** (the output
  recompiles and runs identically — a verifiable artifact); `leviathan
  --ast-after-rules file.ext` gives the structural AST dump of the same tree.

### Comptime (Layer C) — run the language at compile time
```
comptime int TABLE = nextPrime(1000);      // var: init folds to a literal
comptime Array<int> EVENS = [1,2,3,4].where((x) => x % 2 == 0);
int y = 1 + comptime sumTo(10);            // expression form (folds rightward)
comptime if (TABLE > 100) { ... } else { ... }   // untaken branch not compiled
comptime console.writeln("[build] ...");   // compile-time log (real console)
```
- Evaluation runs on the tree-walk oracle, **hermetic**: the `std::sys*` floor
  is denied (no files/sockets/stdin/timers; `await` too) except writes to
  stdout/stderr — console works and emits *during compilation*. A capability
  injection (§4.7, §6.6.6a) written directly inside a comptime-folded root is
  denied even earlier, at the injection site — no bind is in scope during
  comptime folding at all (§4.7's comptime note).
- **Step-bounded**: a runaway comptime loop is a compile error
  (`--comptime-budget N` overrides the default ~100M steps); exhaustion is not
  catchable.
- Results reify as literals: `int`, `float`, `bool`, `string`, `None`, arrays
  of these, and value-`struct`s with a constructor matching their fields 1:1 by
  name and order (reified as a constructor call; reference classes stay
  non-reifiable). Later comptime sites see earlier `comptime` vars by name.
- `uses` inside a taken `comptime if` branch is supported: the branch's imports
  become visible to rule/attribute/macro scoping (the imports map is recomputed
  after the comptime fold). A macro call in a `comptime if` **condition** is an
  error (it would feed the imports map macro resolution itself needs).

#### `import()` — comptime file inclusion (LA-20)
```
comptime string tpl = import("views/links/index.html");
```
`std::import(path)` is an ordinary prelude function whose comptime evaluation
is intercepted (the exact inverse of the `sys*` gate: denied at comptime,
allowed at runtime — this one is allowed at comptime, and its ordinary body
just `throw`s at runtime). At compile time it reads the named file and folds
to its content as a comptime `string`; the file becomes a **declared build
input**, same determinism class as a `.lev` source.

- **Paths** are plain, project-relative, `/`-separated: no leading `/`, no
  `\`, no empty path, no `.`/`..` segment — checked lexically before any
  filesystem contact, so an escape-shaped path is refused outright rather
  than normalized.
- **Root resolution splits by build shape.** A `--plan` build resolves `rel`
  against the *importing file's own module's* declared `assets` table (built
  by trident from the manifest's `assets = [...]`, §12.8) — plain string
  equality, no path arithmetic; a dependency's `import()` sees only its own
  declared assets, never the consuming app's (or vice versa) — the same
  reach-is-declared discipline as phantom-dep checking. A bare single file
  (no `--plan`) resolves `rel` against the source file's own directory
  (project-of-one, no manifest needed).
- **Caching + determinism:** a path is read once per compile (cached by
  absolute path), so two comptime sites importing the same file observe
  identical content even if it changes mid-compile. Same inputs -> same fold
  -> same binary.
- **Runtime misuse is loud**: a program that reaches `import(...)` at runtime
  (there is nothing to intercept — `comptime_` is false) gets an ordinary,
  catchable `RuntimeException` naming the mistake, identical on every engine.
- A user's own `import` function (declared anywhere outside `namespace std`)
  is never hijacked — the intercept keys on the **prelude declaration's
  identity**, not the name, so shadowing is exactly as safe as any other
  std-shadow (§12.6).
- No file-size cap (the comptime step budget already bounds a runaway
  import-in-a-loop); `--assets` (below) makes what got read visible.

### Body-replacing rules (Layer D) — `rewrites` / `replace` / `$body`
```
namespace Perf {
    attribute Timed { }
    rule timed rewrites body of m {
        match @Timed on method m
        replace `
            ticks = ticks + 1;
            var r = $body;          // $body = the original body's value
            return r;
        `
    }
}
```
The one body-*replacing* capability, gated behind an explicit `rewrites body of
<bind>` header so it is greppable and its `--expand` diff is the loudest. A rule
is **additive (`inject`) XOR a rewriter (`replace`), never both** (M30).

- **`$body`** splices the *original* body back in — replacement is composition,
  not obliteration. **Statement position** (`$body;`) splices the original
  statements verbatim (their locals are not renamed — the body moves as one
  authored unit). **Expression position** (`var r = $body;`) is valid only when
  the original body is a single value expression — an arrow (`=> e`) or a block
  that is exactly `{ return e; }` — else M31 (capture it in statement position
  instead). `$body` must appear **exactly once** (M32: zero drops the body
  silently, more than once duplicates its side effects).
- **Ordering:** all additive `inject`s on a body apply first, then the single
  `replace` wraps the result — so `$body` captures "the method as written plus
  any additive prologue/epilogue."
- **Confluence (M33):** two `replace`s on one body do not compose (a conflict
  naming both rules); likewise two same-name+type `member of` injections, unless
  one is marked `distinct`.
- **`reentrant`** (`rule N reentrant { … }`): by default a rule never matches
  code another rule injected. `reentrant` opts a rule into re-triggering on
  rule-generated code, re-run to a fixpoint (or M34 if it doesn't converge in
  the round budget, default 8, `--reentrant-budget` overrides). Only `reentrant`
  rules re-trigger — the safe majority still sees the tree exactly once.

Expression macros (Layer D-lite) also ship (see Rules, above).

### Procedural macros (F4) — comptime code returns code
```
macro generated(string payload) comptime {
    var parts = ["0"];
    for (int i in 0..payload.length()) parts = parts.add("+ 1");
    return meta::parseExpr(parts.joinToString(" "));
}

int n = generated!(`four`);              // raw backtick payload -> "four"
```

`macro name(string payload) comptime <body>` is the procedural counterpart to
fixed-template expression macros. v1 takes exactly one comptime-evaluable
`string` and must return opaque `Ast`. `meta::parseExpr(string)` and
`meta::parseStmts(string)` are compile-time-only fragment parsers; their runtime
bodies throw. A procedural expression call requires `Ast(expr)`—returning
`Ast(stmts)` is a kind error.

Backticks are accepted as raw strings only in macro-call argument position;
they do not become general expressions. The body runs under the ordinary
hermetic comptime and step-budget rules (`import()` remains the one declared
file input). A same-named ordinary call inside the body is recursive comptime
execution and shares that budget. By contrast, generated `name!(...)` syntax is
rejected: generated output is never re-scanned for macro expansion (M24).

Every splice clones the carried tree and gives generated nodes the call-site
span. Parse failures name the macro call and render the generated fragment line
with an offset caret. Macro authors own generated-local hygiene in v1 and use
the reserved-by-convention `__<package>_...` prefix. `--expand` prints only the
post-expansion source; procedural bodies do not survive into the artifact.

---

## 6.10 `Block` — the fixed-length byte buffer (Track 03 §3, contract C4)

`Block` is the language's **gated mutable byte buffer** (info.md §16's "bazooka in a
marked room" — the gate is the type itself: nothing implicit converts to or from it). It is a
**reference type** (honestly mutable, shared by reference like any `class`), unlike the
pure/immutable `Array`/`Map`.

```
Block b = Block(4096);              // zeroed, fixed length; b.length() == 4096
Block c = Block::fromString(str);   // copy a string's bytes into a new Block
int  v  = b.byteAt(i);              // 0..255
b.setByte(i, v);                    // v must be 0..255 (else throws — not masked)
Block s = b.slice(off, len);        // ALIASING view (shares bytes with b)
string t = b.toString(off, len);    // copy len bytes -> string
int  u  = b.int32At(i); b.setInt32(i, u);   // little-endian; read sign-extends
int  w  = b.int64At(i); b.setInt64(i, w);   // little-endian
b.fill(off, len, v);                // native bulk byte fill
b.blit(dstOff, src, srcOff, len);   // native copy; memmove overlap semantics
bool same = b.equals(c);            // CONTENT equality (unlike `==`)
int first = b.mismatch(c, from);    // first differing index, or -1
```

| method | behavior |
|---|---|
| `Block(int size)` | construct a zeroed buffer of `size` bytes (fixed length) |
| `Block::fromString(string s)` | new `Block` copying `s`'s bytes (labeled constructor) |
| `length() -> int` | byte length |
| `byteAt(int i) -> int` | byte `0..255` at `i` |
| `setByte(int i, int value)` | store a byte; `value` outside `0..255` **throws** (loud, not masked) |
| `slice(int off, int len) -> Block` | **aliasing** view sharing storage — writes through the slice are visible in the parent (zero-copy, by design) |
| `toString(int off, int len) -> string` | copy `len` bytes from `off` into a new string |
| `int32At(int i) -> int` / `setInt32(int i, int value)` | little-endian 4-byte read (**sign-extended**) / write |
| `int64At(int i) -> int` / `setInt64(int i, int value)` | little-endian 8-byte read / write |
| `fill(int off, int len, int value)` | fill `[off, off+len)` with one byte; `value` must be `0..255` |
| `blit(int dstOff, Block src, int srcOff, int len)` | copy bytes with **memmove semantics**, including overlapping views of one root |
| `equals(Block other) -> bool` | compare full-view **content**; unequal lengths return `false` |
| `mismatch(Block other, int from) -> int` | first differing index at or after `from`, else `-1`; unequal lengths throw |

- **Every access is bounds-checked** and throws `RuntimeException` on out-of-range offset/length
  (§3.7 loudness) — including `setByte`'s `0..255` value range.
- **Equality has two deliberate meanings:** `b.equals(c)` compares byte content, while
  `b == c` remains ordinary `Block` reference identity. Neither operation changes the other.
- Empty bulk ranges are legal at the inclusive end offset. `mismatch` accepts
  `from == length()` and returns `-1`.
- **Print form:** `console.write(b)` renders `Block(len=N)`.
- **Engine coverage:** `Block` ships on the **oracle, IR, emit-C++, and LLVM engines**
  (the `LV_BLOCK` ABI addendum landed 2026-07-10 — `designs/deferal-char-block-abi.md`):
  a heap value (tag 11, body `{parentPtr, off, len, dataPtr}`) whose slice retains the
  **root** so shared bytes outlive any single view; `lvrt_block_*` in `lv_runtime.c`.
  No ELF lane (X64Gen frozen). The `sys*`/`File` `Block` I/O overloads (§6.6.5) also
  landed on oracle + IR + LLVM (2026-07-10). **Only the ELF leg of M4 stays deferred** —
  Block ARC on the frozen X64Gen backend.

---

## 6.11 JSON (Track 09 F1, `namespace json`)

The value model is a **class** (`JsonValue`) — the language has no type aliases, so a recursive
named union cannot be declared. `kind` is an int tag (`0` null, `1` bool, `2` number, `3`
string, `4` array, `5` object; a mechanical upgrade to `enum JsonKind` is a non-blocking rider).

```
JsonValue? doc = json::parse(text);          // None on malformed (parse is TOTAL, never throws)
if (doc != None) {
    string name = doc.at("user").at("name").asStr() ?? "?";   // loud navigation + typed access
    int? n = ...; // asBool()/asNum()/asStr()/asArray()/asObject() -> None on kind mismatch
    string out = doc.render();                // compact;  doc.renderPretty(2) indents
}
JsonValue v = JsonValue::ofObject(m);        // labeled "static" ctors: ofNull/ofBool/ofNum/
                                             //   ofStr/ofArray/ofObject
```

- **Accessors:** `isNull()`…`isObject()`; `asBool()->bool?` … `asObject()->Map<string,JsonValue>?`
  (None on mismatch); `at(int)` / `at(string)` throw on kind/bounds/missing (loud, §3.7);
  `atOrNone(key) -> JsonValue?`; `size()`.
- **Parser:** recursive-descent, full escape set incl. `\uXXXX` with surrogate-pair combining
  (a lone/unpaired surrogate → U+FFFD); numbers are IEEE doubles; depth cap 128; strict trailing
  garbage → None. `json::render(v)` is `v.render()`.
- **Number rendering** is `float.toString()`'s form (`42.000000`) for v1 — pinned by corpus,
  revisited when float formatting is addressed globally (§19).
- **Engine coverage:** oracle, IR, emit-C++, **and LLVM** (bug.md #30, a `Map<K,
  recursive-class>` corruption the object model triggered, was fixed 2026-07-10 —
  `designs/complete/techdesign-bug30-map-with-ownership.md`; JSON is full-coverage on
  every active engine). Not ELF — post-freeze `byteAt` (X64Gen frozen, never a target).
  Re-confirmed via Atlantis Track 03's serialization corpus (nested nested `Map<string,
  JsonValue>` objects, oracle/IR/LLVM byte-identical). Several still-open design docs
  (`designs/sonar/techdesign-08-theming-di.md` and others) cite bug #30 as a live reason
  to avoid JSON on LLVM — that premise is now stale and worth revisiting there.

## 6.12 `DateTime` & `Duration` (Track 09 F2)

UTC-only value **structs** over the public-domain Howard Hinnant civil↔days integer algorithm
(no tables; leap seconds ignored, Unix convention).

```
DateTime t  = DateTime::now();               // labeled ctors: now(), ofEpochMs(int)
DateTime u  = DateTime::ofEpochMs(784111777000);
int y = u.year(); u.month(); u.day(); u.hour(); u.minute(); u.second(); u.milli(); u.weekday();
string h = u.httpDate();                      // "Sun, 06 Nov 1994 08:49:37 GMT" (RFC 7231)
string i = u.iso8601();                       // "1994-11-06T08:49:37Z" (".mmm" if nonzero)
DateTime? p = datetime::parseHttpDate(h);     // None on malformed (the fallible parsers are
DateTime? q = datetime::parseIso8601(i);      //   datetime:: free functions — no `static` keyword)
Duration d = Duration::ofHours(1).plus(Duration::ofMinutes(2));   // ofMillis/Seconds/Minutes/Hours/Days
string ds  = d.toString();                    // "1h02m00s"
Duration span = t.minus(u);                   // DateTime.minus -> Duration; DateTime.plus(Duration)
```

`now()` reads `std::sysNow()` (wall-clock epoch ms — the one Track 08 clock native this track
depends on). Coverage: all four active engines (oracle, IR, emit-C++, LLVM; not frozen ELF).

## 6.13 Encoding & digests (Track 09 F3)

Byte-true text codecs and cryptographic digests, entirely in-language. Every **decoder is
total** — malformed input returns `None`, never throws (data errors are values, §12.6).

```
namespace encoding {
    string  base64Encode(string bytes);   string? base64Decode(string b64);
    string  base64UrlEncode(string bytes); string? base64UrlDecode(string s);   // JWT: no '=' pad
    string  percentEncode(string s);      string? percentDecode(string s);      // RFC 3986
    string  hexEncode(string bytes);      string? hexDecode(string s);
}
namespace digest {   // return RAW BYTES — compose with encoding::hexEncode / base64Encode
    string md5(string);  string sha1(string);  string sha256(string);
    string hmacSha256(string key, string msg);
}
string etag = encoding::hexEncode(digest::sha256(body));
```

Digests use the int64 mask idiom (words masked to 32 bits after each op); verified against RFC
1321/3174 + FIPS 180-4 + RFC 4231 vectors incl. the padding-boundary trap set. Slow-ish on the
interpreters (fine for cookies/etags). Coverage: oracle, IR, emit-C++, LLVM (not frozen ELF —
post-freeze `byteAt`/`byteToString`).

---

## 7. Compiler

```
leviathan --build prog file.ext  # source -> native executable, one step
leviathan file.ext             # parse + resolve + type-check, dump class shapes
leviathan --run file.ext       # ...then execute (tree-walk oracle)
leviathan --ir file.ext        # ...then lower to bytecode IR and execute that
leviathan --ir-verify file.ext # ...IR + ownership soundness verification (§15)
leviathan --mem-verify file.ext   # reachability oracle: cross-checks live/dead heap objects (§15)
leviathan --ownership file.ext # dump the ownership/escape analysis report
leviathan --emit-cpp file.ext  # native backend: emit C++ (compile with g++)
leviathan --emit-elf out file.ext  # PURE backend (FROZEN, reference-only): own x86-64 + ELF, zero deps; never extended — §7.3
leviathan --build out file.ext # source -> native executable in one step (emit-C++ + g++)
leviathan --ast file.ext       # dump the AST (pre-rule-stage)
leviathan --expand file.ext    # re-emit the post-rule-stage tree as compilable source (round-trips)
leviathan --ast-after-rules file.ext  # structural AST dump AFTER the rule stage (comptime folded)
leviathan --no-rules file.ext  # compile with the rule stage disabled
leviathan --comptime-budget N  # override the comptime step budget
leviathan --reentrant-budget N # override the reentrant-rule round cap (Layer D, default 8)
leviathan --resolve file.ext   # stop after resolution
leviathan --tokens file.ext    # dump the token stream
leviathan --plan build/plan.lvplan  # compile a whole project from trident's resolved build plan (§8)
leviathan --imports file.ext   # dump the file -> imports provenance map (§8)
leviathan --graph file.ext     # dump the `uses` include graph + build order (§8)
leviathan --namespaces file.ext         # list every namespace, the files that open it, and its members (§8)
leviathan --why <name> [in <file>] file.ext  # where a bare `name` resolves from; which candidate wins for a file (§8)
leviathan --lint-namespaces file.ext    # opt-in folder≈namespace convention check (off by default; §8)
leviathan --assets file.ext    # list every comptime import()-consumed asset (path, bytes, hash, module; LA-20)
```

Compilation is **whole-program**: all source gathers into one unit; namespace boundaries
persist. Diagnostics carry source spans and do not stop at the first error.

### 7.1 The two engines and the corpus

Per the two-variants architecture, one semantic IR underlies both execution paths. The
**tree-walking evaluator** is the semantics oracle; the **bytecode IR** (a register machine)
is the lowering target the future AOT backend will consume. Both share one runtime value
model and native cores, and `tests/corpus/` holds real programs with expected outputs — the
**entire corpus** must produce identical output on both engines (differential testing).

IR lowering covers the implemented language: classes (constructors with synthesized `$init`
field initialization, base-constructor calls, methods, accessors, indexers, operators with
`(!=)` derivation, `distinct` keys), closures (capture-by-snapshot closure conversion),
exceptions (per-function handler tables, unwinding across frames, catch by dynamic type
through bases and interfaces), collections (arrays/maps/ranges with the in-language stdlib
bodies themselves lowered to IR functions; native cores shared with the oracle), iteration,
streams, unions/optionality, and `await`/the event loop (an inline pump per `Await` site —
no separate coroutine machinery), plus all the scalar/control-flow core. `Lowerer::fail`
remains the fallback for any genuinely unrecognized construct, reporting `IR: not yet
lowerable` with a source span.

### 7.2 Ownership analysis (§15)

`--ownership` runs an interprocedural escape analysis over the IR, classifying every
allocation site: **scope-owned** (provably dies with its frame — the compiler can free it at
scope exit with zero runtime bookkeeping), **ownership-transferred by return** (the caller's
scope inherits the free obligation), or **refcount tier** (stored in objects/collections,
captured, thrown, or passed to retaining/unknown callees). `--ir-verify` executes the program
while checking the analysis empirically: every scope-owned allocation is verified dead at
frame exit (weak-reference liveness); any survivor is a soundness violation and fails the
run. Over the corpus, 85–90% of allocations are deterministic (scope-owned or transferred),
with zero violations — §15's claim, measured.

### 7.3 Native backends (AOT)

Three native backends consume the same IR; only entry-reachable functions are emitted, and
out-of-coverage constructs fail with a diagnostic:

- **LLVM** (when built against LLVM ≥ 18) — **the primary, portable AOT backend**
  (`designs/complete/techdesign-portable-backend.md`): `--emit-llvm` / `--native-obj out.o` (direct
  object emission via `TargetMachine`, no external `llc`), linking the portable runtime v2
  (`runtime/lv_runtime.c` + a platform floor). Reached full IR parity with the pure ELF
  backend at Gate G1 (2026-07-06) — objects, collections, closures, exceptions, the event
  loop, natives, async/await, sockets, HTTP — and, as of A-M6, the same per-frame arena
  tier the pure backend uses for scope-owned value-struct allocations (§7.2's escape
  analysis, shared verbatim). A `PassBuilder` module pipeline runs before object emission;
  `--opt-level 0|2` selects it (default 2, `0` for a debug build — faster codegen, easier
  to read under a debugger). `--build-native out` goes straight from source to a linked
  executable in one step.
- **Pure x86-64 / ELF** (`--emit-elf out`): the self-hosting-grade path — **our own machine-
  code emitter and ELF writer, no g++, no assembler, no linker, no libc.** It compiles the
  **whole language** (objects, collections, closures, exceptions, files, streams, event loop,
  timers, async/await, sockets, HTTP) to a statically-linked binary that talks to the kernel
  through the `syscall` instruction only ("not a dynamic executable"). Values are tagged
  16-byte `[tag][payload]` pairs; the runtime (allocator over `mmap`, dispatchers, exception
  unwinding, the event loop, sockets) is emitted as machine code. A stack-slot model (each IR
  register → a frame slot) keeps it simple; no register allocation. **Frozen** as of the
  portable-backend pivot — kept as the differential-testing anchor and the eventual
  zero-dependency bootstrap seed, but no longer extended.
- **emit-C++** (always available): `--emit-cpp` prints a freestanding C++ translation unit
  with an embedded runtime; compile with `g++ -O2`. Covers the whole language except the
  event-loop/system layer (that's the pure backend's and LLVM's domain).

**A concrete out-of-coverage example (Track 06):** `math::log`/`log2`/`exp`/`sin`/`cos`/
`tan`/`atan2` and `float.pow` are implemented on the oracle, IR, emit-C++, and LLVM
backends, but deliberately deferred on the frozen ELF backend. It fails the compile with a
clean diagnostic (`native-elf backend: ...`, `tests/run_elf.sh`'s SKIP grep pattern) rather
than linking libm into the frozen backend or silently producing a wrong answer. Every other
Track 06 method (`floor`/`ceil`/`round`/`trunc`/`sqrt`/`toInt`/`toFloat`/`int.pow`/
`math::pi`/`math::e`/`math::min`/`math::max`) is fully covered on all five engines.

All paths are differential-tested against the interpreters over a shared corpus (five-engine
agreement). Indicative wall-clock (this machine; `fib(30)`, recursive, no register
allocation on either native path): oracle 1.8s, IR 1.0s, emit-C++ 0.35s, **LLVM ~0.21s**,
pure ELF ~0.19s — LLVM is within ~10% of the hand-rolled ELF backend on a call/branch-heavy
recursive workload, after A-M6's inlined int/float `Arith` and `truth`/`Not` fast paths (the
alternative — a cross-`.o` `lvrt_arith`/`lvrt_throwing` call per operation/call-like op —
measurably dominated before those landed). Two further micros (`Array<int>` indexed writes
in a hot loop; a value-struct-returning allocator run 3M times): indexed writes are at
parity (LLVM ~0.45s vs ELF ~0.46s — both paths inline the COW fast case and the loop
condition equally well); the value-struct allocator remains LLVM's weakest case (~0.91s vs
ELF's ~0.67s, ~1.36x) even after A-M7 inlined the two remaining cross-`.o` calls on these
hot paths (`lvrt_throwing()` and the `lvrt_arena_save`/`lvrt_arena_restore` pair, both now a
direct load/store of the runtime's exported `lv_g_throwing`/`lv_g_arena_cursor` globals,
doc-2 §2.10) — a controlled A/B rebuild (same benchmark source, only the inlining changed)
measured indexed-store ~6% faster and the value-struct allocator ~8% faster than the pre-
A-M7 cross-`.o`-call baseline, closing part of the gap but not reaching the ≤1.1x target.
The residual gap is per-call overhead A-M7 did not target (every call still pays a real
`lvrt_retain` call per parameter, and the arena mark still round-trips through a frame
alloca rather than a register) plus the LTO lever A-M6 already identified — this machine
still has no `clang`, so compiling the runtime to bitcode and `Linker::linkModules`-ing it
in remains untried. Logged as an open follow-up (`designs/complete/techdesign-portable-backend.md` §12)
rather than spent unilaterally (a runtime-ABI change is a cross-track decision, not a
Track A one).

The **pure ELF** backend **reclaims heap** for both memory tiers (§15) and the heap is
**bounded, not just accounted**: scope-owned allocations (including provably-value-struct
copies) free through a per-frame arena; escaping allocations (objects, boxed arrays, closures,
maps, value-struct arrays, heap strings) free through retain/release over a real free-list allocator
(power-of-two size classes, blocks reused rather than the bump pointer growing forever). A
differential churn corpus (`tests/corpus/churn/`) is **13/13 green at +0 bytes on both ELF
and LLVM** (a 200k-iteration stress run holds at ~1KB escaping-tier peak on ELF; the LLVM
lane reaches the same 13-green/1-XFAIL profile as of A-M6's arena tier), with one shared
XFAIL (a dense-append edge, `dense_index_set.ext`) on both engines.
The last two gate-excluded kinds are now covered on both native backends: value structs
returned by value (freed by uniqueness via `Op::VFree` and, on LLVM, the arena, since a
value-struct pointer may point inline into a dense buffer) and heap-allocated strings
(concat/`subStr`/`toString`, prefixed via `halloc` and un-gated by a heap-address-range
check). Reference cycles remain the one asterisk.

**Coverage.** The **emit-C++** backend covers the whole language *except* the event-loop /
system features (sockets, timers, async, file/stdin I/O), which are inherently interpreter-
bound for now. It handles: the scalar core; the object core (classes, fields, constructors
incl. label ctors, methods, operators with `(!=)` derivation, get/set accessors, MI /
`distinct`); **collections** (arrays, maps, ranges, index get/set incl. `([])` accessors,
iteration, the native cores and the in-language LINQ-style methods); **closures**
(capture + `callclosure` dispatch); and **exceptions** (the interpreter's pending-throw
model — per-call checks, handler dispatch by dynamic type via a generated `issub`, uncaught
reporting). Dynamic dispatch that the checker didn't resolve goes through a generated
`callm` name-dispatcher. The corpus runs on native via emit-C++ (`corpus_native`), skipping
the system/loop programs with a printed notice. The **LLVM** backend now covers the same
ground as the pure ELF backend (see above) — the "scalar core only" state was an A-M2/A-M3
snapshot, superseded at Gate G1.

### 7.4 Columnar `Array<struct>` storage (struct-of-arrays)

An `Array<T>` whose element type `T` is a **columnar-eligible value struct** is stored
**column-major** (struct-of-arrays) on the native backends instead of row-major: all the
first fields, then all the second fields, and so on — one refcounted allocation, header +
per-field column sections, tag-free 8-byte payload columns (no per-row `classId`/`dyn`, no
per-slot tags). This is the default layout as of the columnar flip
(`designs/complete/techdesign-columnar-arrays.md`).

- **User-visible surface: none.** There is no new type, annotation, or syntax. `Array<T>`
  is the same pure value with the same method surface; only the physical layout changes,
  and value semantics make it unobservable (the differential corpus,
  `tests/corpus/columnar/`, proves oracle/IR/emit-C++/LLVM produce byte-identical output in
  both layouts).
- **Eligibility.** `T` is columnar iff it is a value `struct` with at least one field and
  **every** field is a plain scalar — `int`, `float`, `bool`, or `char` — with no `weak`,
  optional (`T?`), union, nested-struct, or reference field. Any ineligible struct (e.g. one
  with a `string` field) keeps the **row-major dense** layout, permanently and unchanged.
  Eligibility is decided statically; an empty array is boxed and flips to columnar on its
  first eligible-struct append.
- **The win — column-selective locality.** A direct-index scan of one field,
  `for (int i in 0..n-1) s = s + arr[i].hot;`, fuses to a single `ColGet` that reads only
  that field's contiguous column — no per-element gather, no allocation. Measured **~4.5×**
  faster than row-major on an 8-int struct at `-O2`, plus a **~2×** footprint reduction for
  all access patterns (`bench/columnar/`). The gain grows with struct width.
- **Where it does NOT win (v1).** Whole-struct consumption via `for (T x in arr)`, and
  closure pipelines (`map`/`where`/`reduce`), **gather** a fresh standalone struct per
  element (value semantics require the copy), which allocates — so those are *slower* than
  the row-major buffer-alias, ~1.8× on a narrow struct. Use direct-index field access for the
  locality win; pipeline/`for-in` fusion is the designated follow-up.
- **The flag.** Columnar is the default; `--no-columnar` restores row-major dense for the
  whole program (the escape hatch, retained one cycle). The frozen ELF backend
  (`--emit-elf`) is always row-major (no columnar lane — it is not a project target).
- **Semantics preserved.** `arr[i] = v` still copy-on-writes the whole buffer on the
  refcount (in place at rc 1, copy when shared); struct map-key equality stays field-wise;
  thread transfer deep-copies the columns as one scalars-only blob. `Array(n, fill)`
  constructs in one O(n) pass (`lvrt_arr_fill`).

---

## 8. Projects, manifests, and the package manager

A **project** generalizes the two-source prelude gather (§7) from 2 files to N: a manifest lists
sources and dependencies, all of it concatenates into one compilation buffer, and that buffer
runs the ordinary single-file pipeline — namespace semantics (§4.1) are unchanged.

### 8.0 Two tools: `trident` and `leviathan` (the toolchain split)

The toolchain is **two separate binaries**, following the cargo/rustc, MSBuild/csc, SPM/swiftc
norm (`designs/complete/techdesign-toolchain.md`):

- **`trident`** — the **package manager / build driver**, the front door users run. It owns the
  project manifest (`trident.toml`), resolves the dependency graph, and emits a fully-resolved
  **build plan** which it hands to the compiler.
- **`leviathan`** — a **pure compiler**. It compiles the source set a plan names. It does **not**
  parse the manifest, resolve dependencies, or know a registry exists.

The boundary is a **build plan** (default `build/plan.lvplan`): trident writes it, `leviathan
--plan <file>` reads it. The plan is machine-generated and never hand-authored — it lists every
resolved source (absolute path + `moduleId`/`origin`), the explicit entry mode, and the module
adjacency that drives phantom-dep enforcement. `leviathan` never opens a `trident.toml`.

```
trident build [dir]   # resolve trident.toml -> write plan -> leviathan --plan --build-native
trident run   [dir]   # ...compile and execute (tree-walk oracle)
trident check [dir]   # ...parse + resolve + type-check, no execution
trident emit-llvm [dir]   # ...emit LLVM IR
trident plan  [dir]   # resolve + write the plan only (no leviathan invocation)
trident --version     # trident's version, then leviathan's
```

Common flags: `--out <path>`, `--target <triple>`, `--opt-level <0|2>` (`--release` = `-O2`),
`--plan <path>` (override the plan location), `--leviathan <path>` (override compiler discovery).
`trident` locates `leviathan` via `--leviathan` → `$LEVIATHAN` → its own sibling directory →
`PATH`. Dependency-management subcommands (`add`/`remove`/`update`/`lock`/`fetch`/`why`) never
invoke the compiler — see `designs/techdesign-package-manager.md`.

### 8.1 The manifest (`trident.toml`)

**TOML**, fixed filename `trident.toml` (Cargo-style — not name-parameterized), discovered in the
project root. trident owns this format exclusively; a small hand-rolled reader keeps the toolchain
dependency-free (no external TOML library).

```toml
name    = "app"
entry   = "main"                # a function name, or a file ("run.lev")
sources = ["*.lev"]             # globs expand alphabetically; explicit lists too
assets  = ["views/**", "schema.sql"]   # comptime import() targets (LA-20); optional
version = "0.1.0"               # optional
out     = "app"                 # optional

[[dep]]                         # repeated array-of-tables; omit if no deps
path    = "jsonlib"
as      = "Json"
version = "1.0.0"
dev     = false
```

| field | meaning |
|---|---|
| `name` | project name (metadata; default output name) |
| `entry` | a **function** name (gather everything, call it) or a **file** name (that file's top-level statements drive the program). trident classifies which and records it in the plan — the compiler never sniffs the extension |
| `sources` | source paths relative to the manifest, glob-expandable (`"*.lev"`) |
| `assets` | declared build inputs for comptime `import()` (§6.9 LA-20): literals, globs, or `**` recursive globs (`"views/**"` — trees, unlike sources' flat globs); hashed by trident and carried in the plan. Absent = no assets. A glob matching zero files is a warning, not an error (a literal naming a missing file still is) |
| `[[dep]]` | dependency entries (§8.2); omit for none |
| `version`, `out` | optional metadata |

A bare file with no manifest is a **project of one** — `leviathan --imports`/`--graph`/
`--namespaces`/`--why` (§7) synthesize a one-file map so the same tooling applies uniformly.

The `--namespaces` and `--why` queries buy back the discoverability the by-name + path-decoupled
model (§ project system) otherwise costs: because a namespace is decoupled from its file path and
imports are by name, `--namespaces` is the symbol index (every namespace → its opening files and
members) and `--why <name> [in <file>]` answers "where could this name come from, and which
candidate wins here?" — the provenance a directory-coupled language reads off the path.

For teams who *want* the Go/Java folder-tidiness anyway, `--lint-namespaces` is an **opt-in** check
(off by default; a normal build never applies it): a project file that opens a namespace should sit
in a directory whose path, relative to the project root, is a case-insensitive suffix of that
namespace's `::`-segments — so `models/link.lev` opening `App::Models` matches ("models" ~ "Models"),
and a root file or a namespace-free composition root always passes. It prints per-file `ok`/`WARN`
lines and exits non-zero on any mismatch, so CI can enforce the convention without the language
imposing it. (Dependencies are never linted — their layout is their own.)

### 8.2 Dependencies (resolved by `trident`)

```toml
[[dep]]
path    = "jsonlib"
as      = "Json"
version = "1.0.0"
dev     = false
```

Dependency resolution is entirely `trident`'s job; `leviathan` only ever sees the resolved sources
the plan names.

- **`path`**: a **local directory** containing its own `trident.toml`, gathered recursively into
  the same whole-program compilation unit — a dependency is just more source (§1). A `path` that
  is not a local directory (e.g. `github.com/thaden0/json`) is a **VCS module**, requiring a
  `version`; VCS fetch, MVS version selection, a lockfile, and the content-addressed store live in
  `trident` per `designs/techdesign-package-manager.md`.
- **`as`**: aliases the dependency's exported namespaces into a synthesized local namespace, so
  `uses Json;` reaches a dep declared `as = "Json"` without knowing its internal namespace name.
  trident materializes the alias as a real source file under the build dir (the plan carries only
  on-disk paths).
- **`dev`**: a development-only dependency, excluded from the shippable-artifact modes (`build`,
  `run`, `emit-llvm`) but visible to `check`.
- **Phantom-dependency prevention**: a file may only `uses` a namespace belonging to the project
  itself or to a **direct** `[[dep]]` — reaching a transitive dependency's namespace without
  declaring it directly is a compile error (pnpm/Cargo-style strictness). trident decides the
  adjacency (plan `edge` rows); `leviathan` enforces it.
