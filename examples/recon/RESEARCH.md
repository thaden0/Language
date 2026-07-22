# Recon — Research Dossier

**A terminal REST API client (Postman-in-the-terminal), written in Leviathan on the Moby TUI framework.**

**Status:** research only. No design yet. **Date:** 2026-07-14.
**Audience:** the designer of `examples/recon`. You (the designer) will **not** have access to
`info.md`, `docs/reference.md`, or the `designs/moby/**` documents — so this dossier reproduces
**every** language fact, framework surface, library API, domain-model detail, and known-bug
footgun you need to design Recon end-to-end. When this document quotes a signature, it was
verified against the actual prelude / framework source this session (file:line where it matters).

The dossier is organized as:

1. What Recon is (product scope & the honest constraints)
2. The Leviathan language — everything you'll write in
3. The Moby framework — the UI substrate (what exists, what doesn't)
4. The networking / JSON / crypto / file library surface (the engine under the hood)
5. The Postman collection & environment formats (the data model to import)
6. Cookies, sessions, auth, redirects, timing — the REST-client mechanics
7. Compiler & framework footguns you MUST design around (the landmine map)
8. Packaging, running, testing (`trident`, engine lanes)
9. A consolidated "capabilities vs. gaps" table and open design questions

---

# 1. What Recon is

Recon is a keyboard-driven terminal application that does what Postman / Insomnia / Bruno /
`httpie` do, but as a TUI:

- **Import & browse Postman Collections** (v2.1 and v2.0 JSON) as a navigable tree of
  folders and requests.
- **Build & send HTTP/HTTPS requests**: method, URL (with `{{variable}}` interpolation), query
  params, headers, body (raw / JSON / url-encoded / form-data), and auth.
- **Environments & variables**: Postman environment files + a variable-resolution scope chain,
  with `{{var}}` substitution across URL, headers, and body.
- **Response viewer**: status, timing, size, headers, cookies, and a pretty-printed body
  (JSON pretty-print is first-class).
- **Sessions & cookies**: a cookie jar that captures `Set-Cookie` from responses and re-attaches
  matching cookies on subsequent requests (domain/path/expiry/secure aware).
- **Auth helpers**: Basic, Bearer, API-Key; Digest and simple OAuth2 client-credentials are
  stretch goals (all buildable from the crypto/encoding primitives in §4).
- **History**: a log of sent requests + responses, persisted to disk.
- **Persistence**: collections, environments, cookies, and history saved as JSON files.

### The honest constraints (read these before designing anything)

These are the hard facts that shape the whole design. They are elaborated in §4 and §7; stated
up front because they change the product surface:

1. **Postman pre-request / test scripts are JavaScript. Leviathan has no JS engine.** Recon
   **cannot execute** them. You must decide the story: import & display them read-only, and offer
   a **native Recon assertion/extraction layer** as the replacement (a small declarative form:
   "status == 200", "jsonpath `$.token` → env var `token`"). Do **not** promise to run `pm.test(...)`.
   This is the single biggest fidelity gap versus Postman.
2. **The HTTP client does no URL parsing.** It takes `(host, port, path)` explicitly. Recon must
   parse `scheme://host:port/path?query#frag` itself (a small, well-understood parser — §6.1).
3. **The HTTP client does not follow redirects, has no request timeout, and does no connection
   pooling.** Recon implements redirect-following (read `Location`, re-issue, cap the count) and
   timeouts (`awaitTimeout`) itself. See §6.4/§6.6.
4. **HTTP bodies are text only.** Binary request/response bodies (`Block`-backed) are not wired
   into the HTTP client yet ("still text bodies until Block"). File upload / binary download is
   therefore text-limited in v1. Design multipart assembly as text.
5. **No gzip/deflate/br decompression exists in the language.** Servers that send
   `Content-Encoding: gzip` will hand you bytes you cannot inflate. **Mitigation: always send
   `Accept-Encoding: identity`** so servers return plaintext. Document this.
6. **There is no multi-line text *editor* component in Moby** (see §3). `Input` is single-line;
   `Text` is a read-only multi-line label (word-wrap, scrolls inside a `ContentBox`). A request
   **body editor** and an editable-response surface must be designed around this — either build a
   new editor component, or edit bodies through an external-file / line-oriented workflow, or a
   modal single-line-per-field form. This is a real design decision, not a footnote.
7. **The practical run lane is the tree-walk oracle and the IR interpreter** (`trident run` /
   `--ir`). The compiled LLVM lane currently segfaults when a *drawing* component paints through
   the container hierarchy (a P0 compiler bug, §7). Networking + the event loop themselves run on
   the interpreters and LLVM; it's the Moby *paint* path that is interpreter-only today. Design
   for the interpreter lane; treat LLVM as a future target.

Everything below gives you the tools to do all of this.

---

# 2. The Leviathan language

Leviathan is a statically-typed, object-oriented language (C++/C#/TypeScript/Haskell lineage).
Source files are **`.lev`** (never `.ext` — that extension is legacy and forbidden for new files).
The compiler binary is `leviathan`; the package manager / build driver is `trident` (§8).

Guiding philosophy you'll feel constantly: **resolution is by type, everywhere** (overloads,
fields, operators, assignment targets, `catch`, `match` all resolve on type); **explicit over
implicit**; **no null** (absence is the `None` value); **safe by default, danger behind an
explicit gate**.

## 2.1 Lexical & literals

```lev
// line comment
/* block comment */
```

- Identifiers: `[A-Za-z_][A-Za-z0-9_]*`. Internal word boundaries capitalize each stem
  (`subString`, `toUpper`, not `substring`).
- Integers: decimal, `0xFF` hex, `0b1010` binary; `_` digit separators (`1_000_000`). `int` is
  **signed 64-bit**.
- Floats: `3.14`.
- Strings: `"hello"` or `'hello'` — **both quote styles lex to `string`**. Escapes: `\n \t \r \0`
  and `\xNN` (exactly two hex digits → that byte). Strings are **byte-clean** (an embedded NUL
  survives; does not terminate).
- **String interpolation:** `"code=${expr}!"` desugars in the parser to
  `"code=" + (expr).toString() + "!"`. A bare `$` (no `{`) stays literal. **Gotcha for Recon:**
  because `${` is interpolation, a literal string containing Postman's `{{var}}` is fine, but a
  string containing `${` needs `\${`. Recon parses `{{var}}` itself (double braces), which the
  lexer leaves alone — no conflict.
- Booleans: `true`, `false`.
- Arrays: `[1, 2, 3]`, `[]`, `[1..5]` (range spreads to `[1,2,3,4,5]`).
- Ranges: `1..10` inclusive, first-class, iterable, spreadable.
- `char` literals: a single-quoted literal **re-types to `char`** only when the expected type is
  `char` (declaration, comparison against a `char`, `char` return). Otherwise `'a'` is a `string`.
  **Footgun: a bare `'x'` in call-argument position does NOT retype to `char` (bug #50)** — bind
  to a `char` local first, or compare via `.code()`.

## 2.2 Types

Primitives (value types with method shapes, unboxed): `int`, `string`, `bool`, `float`, `char`.
`void` = no value (return type only). Primitive type names are **not keywords** — they resolve to
prelude declarations.

- **`var` / `let`** are inference markers, not types (`let` = `const var`). The value has a fixed
  static type; only its spelling is inferred.
- **`any` does not exist** (rejected by design). Use a **union** `int | string` (closed, tagged).
- **Optionality:** `T?` is exactly `T | None`. **There is no null.** `None` is a unit value.
  Optional fields default to `None`. `None` never `==` a present value.
- **Unions & narrowing:** member access/calls on a union are a compile error **until narrowed**.
  Narrow with `x != None` / `x == None` / `x is T` — flow-typing through `if`/`else`, `while`,
  ternary arms, `&&` chains. Paths narrow (`req.host != None` narrows `req.host`); assignment
  invalidates. **Conditions require `bool` — no truthiness** (`if (host)` is illegal; write
  `if (host != None)`). `??` = default-when-None (`host ?? "d"`); `?.` = optional chaining
  (`a?.m()` short-circuits to `None`).

Example you'll use constantly:
```lev
int? maybe = s.toInt();          // strict parse: None if not a clean integer
int n = maybe ?? 0;              // or:  if (maybe != None) { ... use maybe as int ... }
```

### Value structs vs reference classes

```lev
struct Point { int x; int y; int dot() => x*x + y*y;
               mutating void shift(int dx) { x = x + dx; } }   // 'mutating' to write this
class Session { ... }                                          // reference identity
```

- `struct` = **value**: copied on bind/pass/return/store (deep — nested structs copy, reference
  fields shared), no identity, **final** (may implement interfaces, cannot inherit/be inherited).
  A method that writes `this` must be `mutating`.
- `class` = **reference**: shared handle, identity, multiple inheritance allowed.
- **Rule of thumb for Recon:** geometry/config bundles → `struct`; entities with identity and
  mutation-across-call-sites (the collection tree, the cookie jar, the app state, UI components)
  → `class`. **But watch bug #41/#49 (§7): do not put a `struct` with an enum field into an
  `Array` element, and do not put a `struct` as a `Map` *value* on a class field — both corrupt on
  compiled backends. Use a `class`, or index a `Map<K,int>` into parallel primitive arrays.**

### Enums

```lev
enum Method : int { GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS }   // int-carried, auto 0..n
enum BodyMode { Raw, UrlEncoded, FormData, GraphQL, NoBody }
```

- Value type, closed set, carried by `int`. Members on the static side (`Method::GET`).
  `m.code()` → carrier int; `m.toString()` → member name; `Method::fromCode(int) -> Method?`.
  `==`/`!=`/`<` compare by carrier. Bare `Method m;` = first member.
- `match` over an enum is **exhaustive** (omit a member → compile error; `else` allowed).
- Full coverage on all engines including LLVM (desugars to struct + int, no ABI tag).

### Generics

Any scope-opening entity may carry type params: `class Box<T>`, `R f<R>(R x)`, method `U m<U>(...)`.
Inferred from arguments (through containers: `Array<U>` unifies `Array<int>`) or target type;
explicit `Name<T,...>` always available. **Invariant** (`Array<int>` ≠ `Array<string>`); the raw
form (`Array`) is compatible with any instantiation.

## 2.3 Classes, members, dispatch

```lev
class MyClass {
    public string name;                 // inline access modifier
    public:                             // sectional (until next section)
        int count = 0;
        new MyClass() { name = "x"; }   // constructor: marked by 'new'; name is a label only
        new Named(string s) { name = s; }        // overload / labeled ctor
        int add(int a, int b) => a + b;          // '=>' IS 'return'; body is ONE statement
        void log() console.writeln(name);        // any single statement is a body
    private:
        int secret() { return 42; }
}
```

- **Construction has no `new` at the call site:** `MyClass m = MyClass();` or
  `MyClass m = MyClass::Named("x");` (labeled). Bare `MyClass m;` auto-constructs.
- **A body is exactly one statement** — a block `{ }`, `=> expr;`, a bare statement, or `;`.
- **Members are typed slots; some are executable.** Fields and methods are the same kind of thing.
- **Accessors (get/set) are views over a slot:**
  ```lev
  get value() => value;              // read view
  set value(int v) value = v;        // write view (inside, 'value' is raw slot — non-recursive)
  get ([])(int i) => cells[i];       // indexer
  set ([])(int i, int v) cells[i] = v;
  ```
- **Operators are methods with symbolic selectors:** `MyClass (+)(int v) => ...;`,
  `bool (==)(MyClass o) => ...;` (`!=` derives from `==`). `<<`/`>>` are transfer operators
  (arrowhead points at the destination): `stream << value`, `reader >> target`.
- **Multiple inheritance** with `distinct`: two bases with a same-name **same-type** member
  collide → default **collapse** to one slot; `distinct` keeps them separate (reach via
  `this.Base::member`). Different-typed same-name members never collide.
- **Interfaces allocate nothing** and may require fields *and* methods:
  ```lev
  interface IException { string message; string toString(); }
  ```
  Two interfaces requiring the same field don't collide (the class's one declaration satisfies
  both). Interface method returns are **covariant** (an impl may return a subtype).
- **Dispatch:** an unqualified instance-method call dispatches on the receiver's **runtime** class
  (uniform for class- and interface-typed receivers), devirtualized when the candidate set is
  provably closed. **Limit:** an overridden method that shares its `(name, arity)` with another
  overload on the same static type is a **compile error** at the call site — keep overridden
  methods single-arity per name, or qualify.

## 2.4 Named args & defaults

```lev
void listen(int port = 80, string host = "localhost") { ... }
listen(host: "0.0.0.0", port: 8080);     // positional first, then named in any order
```
Compile-time normalization only (no runtime calling-convention change). Defaults must be
compile-time constants.

## 2.5 Statements & control flow

- `if/else`, `while`, `do..while` (post-test), C-style `for (init; cond; step)`.
- **`for (T x in iterable)`** — ranges, arrays, maps (`for (Pair e in map)` → `e.first`/`e.second`),
  any `IIterable<T>`.
- Compound assignment `+= -= *= /= %=`. `break;` / `continue;` (unlabeled, innermost loop).
- **`using Type name = expr;`** — deterministic cleanup: `Type` implements `IDisposable`
  (`void close()`); `close()` runs on **every** block-exit edge (fall-through, return, throw,
  break/continue), reverse declaration order. `File` implements `IDisposable`. Use it for files
  and any resource with teardown.

## 2.6 Exceptions

```lev
try { throw RuntimeException("boom"); }
catch (IRuntimeException e) { console.writeln(e.message); }   // catch by contract (interface)
catch (IException e) { ... }                                  // first assignable clause wins
```
- A thrown value must implement `IException`. No `finally` (`using` covers cleanup).
- Standard hierarchy (in `namespace std`, implicitly imported): `IException` → `Exception`;
  `RuntimeException : Exception, IRuntimeException`; `LogicException : Exception, ILogicException`.
- Uncaught → `Uncaught <Class>: <message>`, exit 1.
- **Built-in runtime failures throw catchable `RuntimeException`s** (array OOB, missing `Map` key
  via `at`, unresolvable calls, missing operators). Use `Map.has(k)` / `Map.atOrNone(k)` and
  `Array.firstOrNone()` to avoid throwing on absence.

## 2.7 Pattern matching

```lev
string describe(IShape s) => match (s) { Circle => "circle"; Square => "square"; else => "shape"; };
string sign(int n) => match (n) { 0 => "zero"; 1..9 => "small"; else => "big"; };
```
Patterns: a **type** (narrows the subject in that arm), a **value/range**, or `else`. Exhaustive
over a closed union/enum (no `else` needed); open hierarchy requires `else`.

## 2.8 Namespaces & imports

- **Declaration-based** namespaces (disk layout irrelevant): `namespace Recon { ... }`. May be
  reopened across files and merges.
- `uses NS;` imports all of a namespace's names; `use NS::name (as alias)?;` imports one.
  `NS::name` qualification always works. Imports are **lexically scoped** (a top-of-file import
  covers that file; a block-level one covers that block); hoisted within their scope.
- **Inside `namespace NS`, write a namespace global with a BARE name (`x = v;`), never
  `NS::x = v;`** — the qualified-write form has a lowering hazard (cheat-sheet footgun).

## 2.9 Collections & core library (the vocabulary you'll lean on)

### `Array<T>` — pure value semantics

Every method returns a **new** array; "change" by rebinding. `arr[i] = v` is rebind-sugar
(mutates in place when uniquely owned — COW on the refcount). `a = a.add(x)` is the append idiom.

Core (native): `length()`, `at(int)`, `add(T)`. Indexer `arr[i]`. Then a large in-language surface:
- basics: `isEmpty()`, `first()`, `last()`, `firstOrNone() -> T?`, `lastOrNone() -> T?`
- queries: `where(pred)`/`filter(pred)`, `any(pred)`, `all(pred)`, `count(pred)`, `contains(T)`,
  `indexOf(T)`, `indexWhere(pred)` (-1 miss), `find(pred) -> T?`
- transforms: `map<U>(fn)`/`select<U>(fn)`, `reduce<A>(seed, fn)`, `flatMap<U>(fn)`, `forEach(fn)`,
  `reverse()`, `take(int)`, `skip(int)`, `takeWhile`, `skipWhile`, `concat(Array<T>)`, `unique()`,
  `withIndex() -> Array<Pair<int,T>>`, `groupBy<K>(fn) -> Map<K, Array<T>>`
- pure updates: `insertAt(int, T)`, `removeAt(int)`, `with(int i, T v)`, `slice(int from, int len)`
  (throws on OOB)
- sorting: `sort((T,T)=>int cmp)` (stable), `sortBy<K>(fn)`, `minBy<K>(fn)`, `maxBy<K>(fn)`
- strings: `joinToString(string sep)`
- lazy: `asSeq() -> Seq<T>` (lazy pipeline: `map`/`where`/`take`/`firstOrNone` run nothing until a
  terminal pulls)

Free functions in `std`: `std::sum`, `std::min`, `std::max` (`-> T?`), `std::average` (`-> float`).

### `Map<K, V>` — pure associative value, **insertion-ordered**

Because `get`/`set` are keywords, the vocabulary is `at`/`with`/`without`:
- native: `length()`, `at(K)` (throws on missing), `with(K,V) -> Map`, `without(K) -> Map`,
  `has(K)`, `keys() -> Array<K>`, `values() -> Array<V>`
- indexer: `m[k]` reads (== `at`); `m[k] = v` rebinds (insert/update)
- basics: `isEmpty()`, `atOrNone(K) -> V?`, `atOr(K, V dflt) -> V`
- bulk: `entries() -> Array<Pair<K,V>>`, `withAll(Map)`, `mapValues<U>(fn)`, `whereEntries(fn)`
- iterate: `for (Pair e in m)` → `e.first`, `e.second`

**Critical map footguns (see §7):**
- **`m.with(...)`/`.without(...)` by name are broken on LLVM/ELF (bug #18) — always use the
  `m[k] = v` bracket-sugar form.**
- **A `Map<K, Struct>` stored as a class field corrupts on LLVM at 3+ entries (bug #49).** Do not
  store structs as map values on a class field. Store `Map<K, int>` indexing into parallel arrays,
  or use a reference `class` value.
- Key equality: primitives by value, `struct` keys field-wise recursive, class keys by identity.

### `Set<T>`, `Pair<A,B>`, `Range`, `Seq<T>` — all present (see §2.9 vocab). `Pair::Of(a,b)`,
fields `first`/`second`.

### `StringBuilder` — mutable accumulator

```lev
StringBuilder sb = StringBuilder();
sb.add("a").add("b");            // chainable; (<<) works on interpreters
string out = sb.toString();      // O(total)
```
**Caveat:** `(<<)` on user classes doesn't lower on emit-C++; and `StringBuilder.toString()` uses
the `concatAll` native absent on the frozen ELF backend. For Recon (interpreter lane) `StringBuilder`
is fine; for hot HTTP header building the prelude deliberately uses plain `+` for ELF portability —
you can use `StringBuilder` freely since Recon targets the interpreters.

### `string` surface (byte-counted!)

Core (native): `length()` (**bytes**), `charAt(int)`, `subStr(int start, int len)`, `indexOf(string)`,
`toInt() -> int?`, `toFloat() -> float?`, `byteAt(int) -> int`, `toUpper()`, `toLower()`, `trim()`,
`contains(string)`, `startsWith(string)`, `endsWith(string)`, `toString()`.
Toolkit: `lastIndexOf`, `indexOfFrom(s, from)`, `count(s)`, `split(sep) -> Array<string>`,
`splitLines()` (splits `\n`, trims one trailing `\r`), `replace(from, to)`, `padStart/padEnd`,
`repeat(n)`, `trimStart/trimEnd`, `removePrefix/removeSuffix`, `isEmpty`, `isBlank`,
`equalsIgnoreCase`. Relational `< > <= >=` are lexicographic (bytewise).
- **`toInt()`/`toFloat()` are strict**: optional leading `-`, digits only, full-string consumed,
  else `None` (not a silent 0). Great for parsing status codes / ports.
- **Scalar view** (for cursor math / correct Unicode): `s.chars() -> Array<char>` (full UTF-8
  decode, invalid → U+FFFD), `s.at(i) -> char` (decode scalar at byte offset i). `length()`/
  `subStr`/`indexOf` stay **byte-counted**.
- `reverse()` is deliberately absent (byte reverse breaks UTF-8) — use `s.chars().reverse().joinToString("")`.

### `console`

`console.writeln(x)`, `console.write(x)` (stringify any value via generic `write<T>`), `console << x`.
**But: R16 rule — no `console.write` while a Moby app is running** (it corrupts the screen). Route
diagnostics through `Moby::log(string)` + the debug facility instead (§3).

---

# 3. The Moby framework (the UI substrate)

Moby is the enterprise TUI framework: a **retained-mode** component tree (build once, mutate in
place, repaint by damage), flex/grid/dock/stack layout, a `Block`-backed cell surface, MI-mixin
capability composition, DI theming, attribute rules, and a compile-time `moby!` template layer.
It ships as a **trident package** (`moby/`, namespace `Moby`) — Recon depends on it. It is
**checked user code** (full checker coverage, narrowing, `T?` all work — the prelude's narrowing
caveats do NOT apply to package code).

**Engine lane matrix (important):** run lanes are oracle / IR / **LLVM**; **emit-C++ compiles the
whole package except `App.run()`** (no event loop there). BUT a live compiler P0 (bug #67) makes
components that *draw* segfault on LLVM through container→child interface dispatch, so **treat the
tree-walk oracle and the IR interpreter as Recon's real run lanes** (`trident run`). Networking and
the event loop run on the interpreters fine.

## 3.1 What's implemented (you can use these today)

All under `namespace Moby`. The full contract signatures are in §3.4–§3.9; here's the inventory.

**Core (T01):** `Component` (base), `Container`, the mixins `Focusable`/`Scrollable`/`Styleable`/
`Bordered`, `Surface` (cell buffer), geometry structs, damage sweep, `MobyException`.

**Layout (T02):** `FlexLayout`, `GridLayout`, `DockLayout`, `StackLayout` (all `ILayoutStrategy`).
Container default layout is `FlexLayout(Axis::Vertical)`.

**Events / input (T03):** event classes `KeyEvent`/`MouseEvent`/`PasteEvent`, the ANSI/UTF-8
`InputDecoder` (arrows, function keys, mouse SGR, bracketed paste, Alt-prefix), `FocusRing`
(tab traversal), `Keymap` (chord grammar `[C-][M-][S-]key`, `^X` ≡ `C-X`).

**Basic components (T04):** `Text` (word-wrap, read-only), `ContentBar` (3-segment status bar),
`Input` (**single-line** editor: cursor/scroll/mask/paste/validation), `Button`, `CheckBox`,
`RadioGroup`, `ProgressBar`, `Spinner`. Interfaces `ISingleLine`/`IMultiLine`/`IValidatable`.

**Composite / data (T05):** `ContentBox` (scrolling bordered region), `SplitBox` (2-pane split),
`GridBox`, `Tabs`, `ListView`, `TableView`, `TreeView` — all **virtualized** over source interfaces
(`IListSource`/`ITableSource`/`ITreeSource`). These are your primary building blocks for Recon.

**Templates (T06):** the `moby!(\`<Tag .../>\`)` compile-time macro (expands to construction code;
`--expand` shows it). Attribute-driven wiring. Optional — you can build the tree by hand instead.

**Theming/DI (T08):** `Theme : ITheme`, in-language TOML theme files, `bind ITheme` DI,
runtime theme switching. Four built-in themes (Default/Dark/Light/HighContrast).

**Run loop / terminal (T09):** `App` (the root container + run loop), `AnsiRenderer`,
`TerminalSession`, `StdinSource`, timers (`App.every`), overlay stack, resize handling,
`Moby::log` + frame stats.

**Reactivity (T11):** `@Moby::Reactive` field attribute → bare writes update bound widgets in a
`moby!` template (global fan-out). Optional.

## 3.2 What is NOT implemented (design around these)

- **No multi-line text *editor*.** `Input` is single-line. `Text` is read-only, wraps, and is
  scrollable only when placed inside a `ContentBox`. **There is no `TextBox` component** (it's in
  the original sketch's capability matrix but was never assigned to a track that built it). For
  Recon's **request-body editor** and any editable multi-line surface you must: (a) design/build a
  new multi-line editor component (biggest effort), or (b) use a line-array + single-line `Input`
  per line workflow, or (c) edit bodies via `$EDITOR`/external file, or (d) accept single-line
  bodies + file-loaded bodies for v1. **Decide this early.** For *displaying* a response body,
  `ContentBox` + `Text` (wrap) works read-only.
- **`Modal`, `BarMenu`/`Menu`/`MenuItem`, `DebugOverlay` are NOT implemented** (T05 M5/M6 were
  gated on T09, which now exists, but the components themselves haven't been built). The overlay
  substrate exists in `App` (`pushOverlay`/`popOverlay` — minimal), but there is no `Modal` class.
  If Recon wants dialogs (environment editor, save-as, confirm), you either build them over the
  raw overlay stack + a `Container`/`Bordered`/`Focusable` composition yourself, or design a
  non-modal in-pane workflow. **Do not assume `Moby::Modal` / `Moby::alert` / `Moby::confirm`
  exist.**
- **No `@Shortcut`/`@Timer`/`@Validator` attribute rules yet** (T07 designed, not implemented).
  Bind chords directly through `App.keymap().bind(...)` instead; register timers via `App.every`.
- No 256-color/truecolor (16-color SGR only — cell format is internal so it can widen later), no
  RTL, no grapheme clusters (cell math is scalar + `glyphWidth`), no Windows legacy conhost.

## 3.3 Rulings & conventions you must follow (R-series)

- **R1 — events are classes** (mutable `handled` flag). `KeyEvent`/`MouseEvent`/`PasteEvent`.
- **R3 — no `App::Current()`.** The running app is a namespace global reached by `Moby::app()`
  (throws `MobyException` if none). Inside `namespace Moby` it's written bare.
- **R4 — bit-sets are namespace consts, not enums:** `Moby::Attr::Bold`, `Moby::Mod::Ctrl`
  (OR-able ints). `Moby::Unbounded` = the layout "no max" sentinel.
- **R5 — mixins derive from `Component`; diamonds collapse.** A leaf `class ContentBox : Container,
  Scrollable, Bordered` gets ONE collapsed `Component` core. **Base-list order is significant:
  core-most first, decorators last** (collapse keeps the later base's implementation).
- **R6 — fluent chains are leaf-typed.** Mixin bases expose `void setX(...)` setters that
  invalidate; leaf components add thin fluent wrappers returning their own type.
- **R7 — detach discipline.** `remove()`/`clear()`/overlay-dismiss/teardown MUST null child
  `parent` links and fire `onDetach()` (the leak-fix path, since weak refs aren't the default —
  cycles between `parent` and `children` would otherwise leak). Framework `Container.remove/clear`
  already do this; if you build custom containers, replicate it.
- **R10 — theme keys are dotted strings** (`"input.focused.border"`); TOML themes; **never JSON on
  LLVM** (bug #30, mostly moot on the interpreter lane but keep themes in TOML anyway).
- **R11 — keymap wins at capture**: global chords (`^S`) beat a focused `Input`; unmodified
  printable keys reach the focused component.
- **R12 — handler registration returns an int token; removal by token** (`offKey(t)`). Method refs
  have no identity, so remove-by-value is impossible.
- **R13 — overlays are an App-level stack**; the top overlay owns input exclusively (modal-ish).
- **R15 — Moby is a checked package** (full narrowing/`T?` — no prelude caveats).
- **R16 — no `console.write` during a running app.** Use `Moby::log(string)` (a 200-entry ring
  buffer) + a debug facility. A `MOBY_LOG_STDERR=1` env tee exists for headless debugging.

## 3.4 Core value types & interfaces (contracts C2/C3/C5)

Geometry & style (structs):
```lev
struct Point  { int x; int y; }
struct Size   { int w; int h; }
struct Insets { int top; int right; int bottom; int left; new Uniform(int n){...}
                Insets plus(Insets o); int h(); int v(); }
struct Rect {
    Point origin; Size size;
    int x(); int y(); int w(); int h();
    int right(); int bottom();          // exclusive
    bool isEmpty();
    bool contains(int px, int py); bool contains(Point p);
    Rect intersect(Rect o); Rect cover(Rect o); Rect inset(Insets i); Rect shift(int dx,int dy);
}
struct Style { Color fg; Color bg; int attrs;
               Style withFg(Color c); Style withBg(Color c); Style withAttrs(int a);
               Style over(Style base); }        // 'this' wins where fg/bg != Default; attrs OR
struct Constraint {
    int min; int max; int flex; int concrete;
    new Constraint();                    // size-to-content
    new Fixed(int n); new Flex(int weight); new Bounded(int mn,int mx,int weight);
    bool isConcrete();
}
```

Enums (subset you'll use): `Color` (Default=-1, then Black..BrightWhite), `BorderStyle`
(NoBorder/Single/Double/Rounded/Heavy/Dashed — note **`NoBorder`, not `None`**), `KeyCode`
(Char/Enter/Escape/Tab/BackTab/Backspace/Delete/Insert/Up/Down/Left/Right/Home/End/PageUp/PageDown/
F1..F12), `MouseButton`, `MouseKind`, `Overflow` (Clip/Scroll/Wrap), `Align` (Start/Center/End/
Stretch), `Axis` (Horizontal/Vertical), `Dock` (Top/Bottom/Left/Right/Fill).

Event classes:
```lev
class KeyEvent { KeyCode code; char ch; int mods; bool handled = false;
                 bool isChar(char c); bool isCtrl(char c); bool matches(string chord); }
class MouseEvent { MouseKind kind; MouseButton button; int x; int y; int mods; bool handled=false; }
class PasteEvent { string text; bool handled = false; }
```

Core interfaces:
```lev
interface IComponent {
    Rect box; Dock dock; int gridRow; int gridCol; int gridRowSpan; int gridColSpan;
    Constraint widthConstraint(); Constraint heightConstraint();
    void measure(Size avail); Size desired(); void arrange(Rect assigned);
    void paint(Surface s);
    void invalidate(); void invalidateLayout(); bool dirty(); bool subtreeDamaged();
    IComponent? parent(); void setParent(IComponent? p);
    bool visible(); void onAttach(); void onDetach();
}
interface IContainer {
    IContainer add(IComponent c); IContainer remove(IComponent c); IContainer clear();
    Array<IComponent> children(); IContainer setLayout(ILayoutStrategy l); ILayoutStrategy layout();
}
interface ILayoutStrategy {
    Size measure(Array<IComponent> kids, Size avail);
    void arrange(Array<IComponent> kids, Rect content);
}
interface IRenderer { Size size(); void acquire(bool alt,bool mouse,bool paste); void release();
                      void present(Surface s); void setCursor(Point? pos); void bell(); }
interface IInputSource { void start((string)=>void onBytes); void stop(); }
interface ITheme { Style style(string key); bool has(string key); }
interface IFocusPolicy { Focusable? first(IComponent root); Focusable? next(IComponent root, Focusable c);
                         Focusable? prev(IComponent root, Focusable c); }
interface IValidatable { bool validate(); string validationMessage(); }
interface ISingleLine { }  interface IMultiLine { }
```

## 3.5 Component & Container base (C7/C8)

```lev
class Component {
    Rect box; Insets padding; Dock dock; int gridRow; int gridCol; ...
    Constraint widthConstraint(); Constraint heightConstraint();
    void setWidth(Constraint c); void setHeight(Constraint c);      // invalidateLayout
    void measure(Size avail);                    // template: clamp(contentDesired(avail))
    Size contentDesired(Size avail) => Size(0,0); // THE leaf sizing hook
    Size desired(); void arrange(Rect assigned);
    void paint(Surface s);                        // background -> content -> chrome
    void paintContent(Surface s) { }              // leaf hook (draw here)
    void paintChrome(Surface s) { }               // Bordered overrides
    void invalidate(); void invalidateLayout(); bool dirty(); bool subtreeDamaged();
    IComponent? parent(); void setParent(IComponent? p);
    bool visible(); void setVisible(bool v);
    void onAttach() { } void onDetach() { }
    int onKey((KeyEvent)=>void h); void offKey(int t);
    int onKeyCapture((KeyEvent)=>void h); void offKeyCapture(int t);
    int onMouse((MouseEvent)=>void h); void offMouse(int t);
    int onPaste((PasteEvent)=>void h); void offPaste(int t);
}
class Container : Component {
    IContainer add(IComponent c);       // setParent(this), onAttach, invalidateLayout
    IContainer remove(IComponent c);    // detach discipline (R7)
    IContainer clear(); Array<IComponent> children();
    IContainer setLayout(ILayoutStrategy l); ILayoutStrategy layout();
    Rect contentRect();                 // box inset by chrome()
}
class Focusable : Component { bool focused; void setTabStop(bool); bool isTabStop();
    int onFocusChange((bool)=>void h); void offFocusChange(int t); Point? cursorPos() => None; }
class Scrollable : Component { int scrollX; int scrollY; Size contentSize;
    void scrollTo(int x,int y); void scrollBy(int dx,int dy); }
class Styleable  : Component { void setStyle(string key, Style s); Style resolve(string key, ITheme t); }
class Bordered   : Component { BorderStyle border = BorderStyle::NoBorder; string title = "";
    void setBorder(BorderStyle b); void setTitle(string t); }
```

**Custom component pattern:** subclass `Component` (or a mixin combo), override `contentDesired`
for sizing and `paintContent(Surface s)` for drawing, register handlers in the constructor with
`this.` explicit receivers (see bug #53, §7), and call `invalidate()` on state change.

## 3.6 Surface (drawing API, C6)

```lev
class Surface {
    int width; int height;
    void put(int x, int y, char c, Style s);              // one cell, clip-aware, wide-glyph heal
    void writeText(int x, int y, string text, Style s);   // scalar-decodes, clips, wide-aware
    void fill(Rect r, char c, Style s); void clear(Style s);
    void pushClip(Rect r); void popClip();
}
```
Use `writeText` to draw strings. **Footgun (bug #67, LLVM only):** drawing through container→child
interface dispatch segfaults on LLVM — a non-issue on the interpreter lane Recon targets, but do
not build Recon expecting the LLVM lane to paint. Also **bug: `pushClip` + 2+ writes segfaults on
LLVM** — again interpreter-lane only.

## 3.7 App & run loop (C9)

```lev
class App : Container {
    new App();
    App title(string t); App altScreen(bool on = true); App mouse(bool on = false);
    App bracketedPaste(bool on = true); App fpsCap(int fps = 60);
    App onResize((Size)=>void h);
    void run();                                   // blocks until quit()
    void quit(); void quitWith(int code);
    Size screen();                                // w = cols, h = rows
    Keymap keymap();
    void focus(Focusable f); Focusable? focused();
    void pushOverlay(IComponent c); void popOverlay();
    int every(int intervalMs, ()=>void cb); void cancelEvery(int token);
    void requestFrame(); void pumpOnce();         // pumpOnce is the test hook
}
class Keymap {
    int bind(string chord, ()=>void action);      // duplicate chord in one keymap throws
    void unbind(int token); bool handle(KeyEvent e);
}
```
- **Frame phases (frozen order):** input → layout → sweep → paint → present → cursor. Tree
  mutations from handlers take effect next frame. Idle app blocks on watches/timers, zero CPU.
- **Chord grammar:** `[C-][M-][S-]key`, `^X` ≡ `C-X`; key = printable char or KeyCode name
  (`Enter`, `Tab`, `F5`). Examples: `"^S"`, `"M-x"`, `"C-M-Left"`, `"S-Tab"`.
- **`Moby::app()`** returns the running `App` (throws if none). `Moby::log(string)` for diagnostics.
- **The run loop rides the language event loop.** `App.run()` awaits internally; your socket
  callbacks, timers, and `await`s all run on the same single-threaded dispatcher. This is exactly
  why Recon's HTTP-in-the-background model works: send a request, its response callback fires on
  the loop, you mutate state + `invalidate()`, and the next frame paints it.

## 3.8 Theming & DI (C10)

```lev
class Theme : ITheme { new Theme(); new FromToml(string toml); Theme set(string key, Style s);
                       Style style(string key); bool has(string key); void setTheme(Theme other);
                       Array<string> keys(); }        // Theme::Default()/Dark()/Light()/HighContrast()
// composition root:
bind ITheme => Theme::Dark();
bind IRenderer => AnsiRenderer();
bind IInputSource => StdinSource();
```
`bind`/`inject` is compile-time DI, block-scoped, nearest-wins. **Bind interfaces/reference classes
only — struct binds are rejected by the checker** (so a `Style` is passed, never bound). Components
resolve theme styles via `Styleable.resolve(key, theme)`. **Note:** the landed T09 wires
renderer/input via test setters (`__useRenderer`/`__useInput`) rather than pure DI in places —
follow the running examples' pattern when you get to the composition root.

## 3.9 The `moby!` template layer (optional)

```lev
var view = moby!(`<ContentBox title="Files" border={BorderStyle::Single}>
                       <ListView id="files" flex="1"/>
                   </ContentBox>`);
```
Expands at compile time to construction code (`--expand` shows the exact `set*`/`add` chain).
`id="name"` binds the node to the enclosing class's declared field `name`. `on:event={handler}`
wires a handler. `$for`/`$if` for repetition/conditionals. External templates via
`comptime string tpl = import("views/x.moby");` + `assets = ["views/**"]` in the manifest.
**You do not have to use templates** — building the tree with explicit `add()`/`set*()` calls is
equally valid and often clearer for a data-driven app like Recon. If you use templates, note the
`char`-literal-at-comptime bug (#58) and the imported-symbol-in-IIFE bug (#57) are worked around in
the engine already.

## 3.10 The components you'll actually assemble Recon from

- **`TreeView`** (collection sidebar): consumes `ITreeSource { int rootCount(); TreeNodeId rootAt(int);
  int childCount(TreeNodeId); TreeNodeId childAt(TreeNodeId, int); string labelAt(TreeNodeId);
  bool hasChildren(TreeNodeId); }` where `struct TreeNodeId { int id; }` is an opaque handle the
  source owns. Recon implements a source over the parsed collection tree. Virtualized (paints only
  the viewport), keyboard nav (Up/Down/PageUp/Home/End, Right expand, Left collapse, Enter/Space),
  `on:select(TreeNodeId)`, `on:expand(TreeNodeId, bool)` (fires **before** rebuild — the lazy-load
  hook), `refresh()` after mutation.
- **`ListView`** (history, environment list): `IListSource { int count(); string itemAt(int); }`;
  `ArrayListSource` wraps an `Array<string>`; `ListView.items(Array<string>)` sugar. Selection,
  scroll, `on:select`/`on:activate`, `multi` mode.
- **`TableView`** (headers, query params, cookies): `ITableSource { int rowCount(); int colCount();
    string cellAt(int r,int c); }`; `struct TableColumn { string title; Constraint width; Align align; }`;
  sorting is delegated (`on:sort(col, asc)` — the view never sorts, the source does + `refresh()`).
- **`Tabs`** (request sections Params/Headers/Body/Auth; response sections Body/Headers/Cookies):
  `Tabs add(string tabLabel, IComponent c)`, `select(int)`, `on:select(int)`.
- **`SplitBox`** (sidebar | main, or request | response): exactly 2 children, `axis`, `ratioPct`,
  draggable divider, `on:resize`.
- **`ContentBox`** (scrolling bordered regions, response body viewer via a `Text` child),
  **`GridBox`** (forms), **`ContentBar`** (status bar: method+url+status+timing),
  **`Input`** (URL bar, single-line fields), **`Button`**, **`CheckBox`**, **`RadioGroup`**,
  **`ProgressBar`**/**`Spinner`** (in-flight indicator).

**A plausible Recon layout skeleton** (design freely; this is orientation, not prescription):
```
App (FlexLayout Vertical)
 ├─ ContentBar               (top: env selector, global status)
 ├─ SplitBox (Horizontal)
 │   ├─ TreeView             (collections sidebar)
 │   └─ SplitBox (Vertical)
 │       ├─ Container        (request builder: URL Input + method + Tabs[Params|Headers|Body|Auth])
 │       └─ Tabs             (response: Body[ContentBox+Text] | Headers[TableView] | Cookies[TableView])
 └─ ContentBar               (bottom: keybind hints / timing / size)
```

---

# 4. The networking / data library surface (the engine)

All of this is in the prelude (`namespace std` unless noted), verified this session against
`src/Resolver.cpp`. This is what powers Recon's actual request sending.

## 4.1 HTTP client (the core of Recon)

```lev
class HttpClient {
    // plaintext
    void request(string method, string host, int port, string path,
                 HeaderMap headers, string body, (HttpResponse)=>void onResp);
    void get (string host, int port, string path, (HttpResponse)=>void onResp);
    void post(string host, int port, string path, string body, (HttpResponse)=>void onResp);
    Promise<HttpResponse> fetch(string host, int port, string path);     // await-able (GET only)
    // HTTPS (always full cert verification, TLS 1.2 floor + 1.3)
    void requestTls(string method, string host, int port, string path,
                    HeaderMap headers, string body, (HttpResponse)=>void onResp);
    void getTls (string host, int port, string path, (HttpResponse)=>void onResp);
    void postTls(string host, int port, string path, string body, (HttpResponse)=>void onResp);
    Promise<HttpResponse> fetchTls(string host, int port, string path);
}
```

**Behavioral facts (verified):**
- **No URL parsing** — you pass `(host, port, path)`. Recon parses the URL (§6.1) and picks the
  `Tls` variant iff scheme is `https`. Default port: 80 (http) / 443 (https).
- **Sends `Connection: close`** and reads until the peer closes, then parses. One connection per
  request (no pooling).
- **`request`/`requestTls` re-derive `Host`, `Connection`, and `Content-Length` themselves** and
  **drop** any user-supplied header of those names — so don't try to override `Host` or
  `Content-Length` through the `HeaderMap` (they're authoritative). Everything else you put in the
  `HeaderMap` is sent verbatim.
- **`fetch`/`fetchTls` are GET-only convenience** returning `Promise<HttpResponse>`. For anything
  with a method/body/headers you use `request`/`requestTls` with the callback and wrap it in a
  `Promise` yourself if you want to `await` (trivial: `Promise<HttpResponse> p = Promise();
  client.request(..., (r) => p.resolve(r)); HttpResponse resp = await p;`).
- **HTTPS verification is always full** (chain + RFC 6125 hostname). A verification failure throws
  a loud named `RuntimeException` **before** the first byte. There is no "insecure/skip-verify"
  mode exposed through `HttpClient` (the lower `std::tlsConnect(fd, host, alpn, caFile,
  verifyMode, cb)` has `verifyMode` 0/1/2 and a `caFile` for a custom CA if Recon ever needs
  self-signed support — but that's below `HttpClient`, you'd build a custom send path).
- **TLS availability:** built against system OpenSSL (≥1.1.1). A runtime built without OpenSSL
  reports `"TLS support not built into this runtime"` on any TLS call; plaintext still works.
  Coverage: oracle + IR + LLVM.
- **Host resolution:** `sysTcpConnect(host, port)` resolves the hostname (getaddrinfo, IPv4 A
  record; a host literal containing `:` selects IPv6). `std::sysResolve(host) -> string?` exists if
  you want to resolve separately.
- **Timing:** measure with `std::sysMonotonic()` (CLOCK_MONOTONIC ms) around the send/callback.
- **Request timeout:** none built in. Use `awaitTimeout<T>(Promise<T>, ms) -> T?` (§4.7) around the
  fetch promise — returns `None` on timeout. Note it stops *waiting* but does not cancel the
  underlying socket work; pair with your own connection close if you need a hard kill.

### HeaderMap (verified)
```lev
struct Header { string name = ""; string value = ""; }
class HeaderMap {
    HeaderMap add(string name, string value);       // append (duplicates kept, order kept)
    HeaderMap set(string name, string value);       // replace-all-then-append
    bool has(string name);
    string firstOr(string name, string dflt);       // narrowing-free internal form
    string? first(string name);                     // None if absent
    Array<string> all(string name);                 // all values (for Set-Cookie!)
    HeaderMap remove(string name);
    Array<Header> entries();                        // order + duplicates preserved
    int length(); string render();                  // "Name: value\r\n" per entry
}
```
Case-insensitive name matching. **`entries()` / `all(name)` preserve duplicate `Set-Cookie`
lines in order** — this is exactly what Recon's cookie jar needs.

### HttpResponse (verified)
```lev
class HttpResponse {
    int status = 200; string reasonText; string body; HeaderMap headers; bool keepAlive;
    new HttpResponse(int s, string b);
    HttpResponse withHeader(string name, string value);
    string reason();                    // status text (built-in table for common codes)
    string render();
    void parse(string raw);             // parses status line + headers + body;
                                        // DECODES a Transfer-Encoding: chunked body transparently
}
```
So when your callback fires, `resp.status` / `resp.headers` / `resp.body` are ready; chunked bodies
are already de-chunked. `resp.headers.all("Set-Cookie")` gives you the cookie lines.

### HttpRequest (server-side / parsing; you mostly won't need it as a client)
Has `method/path/version/body/headers`, `parse(raw)`, incremental `feed(chunk) -> bool`,
`header(name) -> string`. Recon builds requests by calling `HttpClient.request(...)` directly, not
by constructing `HttpRequest`. `HttpServer` exists too (not needed for a client, but available if
Recon ever wants a mock server for testing).

### TcpStream / lower floor (if you need custom control)
```lev
class TcpStream {
    new TcpStream(int fd); int rawFd();
    void send(string s); void onData((string)=>void cb); void onClose(()=>void cb); void close();
}
int std::sysTcpConnect(string host, int port);     // -1 on failure
void std::connectTimeout(string host, int port, int ms, (int)=>void cb);   // cb(fd) or cb(-1)
```
You'll almost always stay at the `HttpClient` level. `connectTimeout` is available if you want a
connect deadline distinct from a total-request deadline.

## 4.2 JSON (parse collections AND response bodies)

```lev
namespace json { JsonValue? parse(string s);  string render(JsonValue v); }
class JsonValue {
    int kind;    // 0 null, 1 bool, 2 number, 3 string, 4 array, 5 object
    new ofNull(); new ofBool(bool); new ofNum(float); new ofStr(string);
    new ofArray(Array<JsonValue>); new ofObject(Map<string,JsonValue>);
    bool isNull(); bool isBool(); bool isNum(); bool isStr(); bool isArray(); bool isObject();
    bool? asBool(); float? asNum(); string? asStr();          // None on kind mismatch
    Array<JsonValue>? asArray(); Map<string,JsonValue>? asObject();
    JsonValue at(int i);            // array index — throws on kind/bounds
    JsonValue at(string key);       // object key   — throws on kind/missing
    JsonValue? atOrNone(string key);// object key   — None if absent (USE THIS for optional fields)
    int size();                     // array length or object field count
    string render();                // compact
    string renderPretty(int indent);// pretty-print (indent spaces) — the response pretty-printer
}
```
- **`json::parse` is total** — returns `None` on malformed input, never throws. So a bad collection
  file or non-JSON response body yields `None`; handle it.
- Navigation: `at(...)` is loud (throws on missing/kind mismatch); **`atOrNone(key)`** is the safe
  optional-object-field accessor. `asStr()`/`asNum()`/etc. return `None` on kind mismatch.
- **Number rendering quirk:** numbers render in float form (`42.000000`), not `42`. When displaying
  a JSON number you'll want to trim trailing zeros yourself for a clean view. `asNum()` gives you
  the `float`; convert to `int` via `.toInt()` when you know it's integral.
- Parser details: recursive descent, full escape set incl. `\uXXXX` with surrogate-pair combining,
  depth cap 128, strict trailing-garbage → `None`.
- **Coverage is full on every active engine including LLVM** (a historical `Map<K, recursive-class>`
  corruption, bug #30, was fixed) — but Recon runs on the interpreter lane anyway.

This is your workhorse for **both** importing Postman JSON and rendering response bodies. To build
JSON for export (saving a collection/environment/cookie file), construct `JsonValue::ofObject(...)`
trees and call `.render()` / `.renderPretty(2)`.

## 4.3 Encoding (auth, query params, base64)

```lev
namespace encoding {
    string  base64Encode(string bytes);   string? base64Decode(string b64);
    string  base64UrlEncode(string bytes); string? base64UrlDecode(string s);   // JWT-style, no '=' pad
    string  percentEncode(string s);      string? percentDecode(string s);      // RFC 3986
    string  hexEncode(string bytes);      string? hexDecode(string s);
}
```
- **Basic auth:** `"Basic " + encoding::base64Encode(user + ":" + pass)`.
- **URL query building / `x-www-form-urlencoded` bodies:** `encoding::percentEncode(key) + "=" +
  encoding::percentEncode(value)`, joined by `&`.
- **JWT / bearer token decoding for display:** `base64UrlDecode` on the token segments.
- Every decoder is **total** (malformed → `None`).

## 4.4 Digests / HMAC (Digest auth, AWS/HMAC signing, content hashing)

```lev
namespace digest {   // return RAW BYTES — compose with encoding::hexEncode / base64Encode
    string md5(string);  string sha1(string);  string sha256(string);
    string hmacSha256(string key, string msg);
}
```
- **Digest auth** (RFC 2617) is buildable: `md5` of the A1/A2/response strings, `hexEncode` the
  results. All the pieces are here.
- **HMAC-SHA256** for OAuth1 / AWS Signature v4 style signing if you go that far.
- **RSA:** `std::sysRsaEncrypt(pubKeyPem, bytes, padding="oaep") -> string?` exists (for
  key-transport). `std::sysRandom(int n) -> string` is crypto-grade (nonces).

## 4.5 Regex (URL/variable parsing, response search, assertions)

```lev
Regex re = Regex("(?<key>\\w+)=(?<val>[^;]*)");        // throws RegexException on a bad pattern
bool ok = re.isMatch(s);
Match? m = re.find(s);        if (m != None) { string v = m.group("key")?.value ?? ""; }
Array<Match> all = re.matches(s);
string out = re.replace(s, "$1-$2");                   // $0..$99, ${name}, $$
Array<string> parts = re.split(s);
namespace regex { ... compile/isMatch/find/matches/replace/split/count/escape ... }  // convenience
```
- Byte-oriented, **linear-time** (Thompson/Pike/lazy-DFA), **no backtracking/backrefs/lookaround**
  (so no ReDoS). Offsets/lengths are bytes.
- `Match` (a struct): `index`, `length`, `value`, `groups: Array<Group>`, `group(int)`,
  `group(string) -> Group?`. `Group`: `matched`, `index`, `length`, `value`.
- **Pattern-as-data vs pattern-as-code:** `regex::compile(p) -> Regex?` returns `None` on a bad
  pattern (use for user-supplied patterns); the `Regex(...)` constructor **throws** (use for
  literals). Recon should use `regex::compile` for any pattern typed by the user.
- **Gotcha:** a replacement template with `${name}` must be written `"\${name}"` in Leviathan
  source (else the language interpolates it). And there's an **overload-scoring bug (#34)**: within
  a class, declare lambda-taking overloads **before** string-taking same-name overloads.
- Cookie-header parsing, `Set-Cookie` attribute splitting, and `{{var}}` interpolation can all be
  done with regex, but simple `indexOf`/`subStr` string scanning is often clearer and avoids the
  regex footguns — your call.

## 4.6 Date/Time (history timestamps, cookie expiry)

```lev
struct DateTime {                     // UTC-only value struct
    new now();                        // reads std::sysNow() (wall-clock epoch ms)
    new ofEpochMs(int e);
    int year(); int month(); int day(); int hour(); int minute(); int second(); int milli(); int weekday();
    string httpDate();                // "Sun, 06 Nov 1994 08:49:37 GMT" (RFC 7231) — cookie Expires!
    string iso8601();                 // "1994-11-06T08:49:37Z"
    DateTime plus(Duration d); DateTime minus(Duration d); Duration minus(DateTime u);
}
struct Duration { new ofMillis(int); new ofSeconds(int); new ofMinutes(int); new ofHours(int);
                  new ofDays(int); Duration plus(Duration); string toString(); }  // "1h02m00s"
namespace datetime { DateTime? parseHttpDate(string); DateTime? parseIso8601(string); }
```
- **Cookie `Expires`** is an RFC-7231 HTTP-date — `datetime::parseHttpDate` parses it, and
  `DateTime::now()` compares against it for expiry. **`Max-Age`** is seconds-from-now.
- **History timestamps:** `DateTime::now().iso8601()`.
- Clock native: `std::sysNow()` (wall clock ms), `std::sysMonotonic()` (monotonic ms, for durations
  — use this for request timing, not `sysNow`).

## 4.7 Async / await / timers / concurrency

- **`await expr`** parks the current task until a `Promise<T>` resolves, yielding `T`. No function
  coloring (any function may `await`; a function is "async" simply by returning a `Promise<T>`).
- **`Promise<T>`**: `new Promise()` (pending) / `new Promise(v)` (resolved); `resolve(v)`,
  `isReady()`, `get()`, `then(cb)`.
- **Timers:** `std::after(ms)` / `std::every(ms)` return a `Timer` (`ticks() -> InStream<int>`,
  `subscribe`, `cancel`). In Moby, use `App.every(ms, cb)` / `App.cancelEvery(token)`.
- **`awaitTimeout<T>(Promise<T> work, int ms) -> T?`** — `None` on timeout (your request-timeout
  primitive). **`TaskGroup`** (structured concurrency via `using`) and `CancelledException` exist if
  you parallelize (e.g. a "run folder" batch). **`std::spawn`/`Channel<T>`/`Worker<T>`** are true
  OS threads on LLVM — but for a single-user TUI you almost certainly stay single-threaded on the
  event loop. **Do not `await` a bare global `Promise` across a `spawn` (bug #35).**
- The event loop keeps the program alive while any timer/watch/socket is pending, and exits when
  none remain — so a Recon request in flight keeps the loop running until the response arrives.

## 4.8 Files (load/save collections, environments, cookies, history)

```lev
namespace std {
    const OpenMode read; const OpenMode write; const OpenMode append; const OpenMode binary;
    bool fileExists(string path); int fileSize(string path); int fileModified(string path);
}
class File : IDisposable {
    new File(string path, OpenMode mode);      // opens on construction; throws FileException on failure
    void open(); void close(); bool isOpen();
    bool exists(); int size(); int modified();
    void write(string s); void writeln(string s);
    string readln();                            // one line; "" at EOF
    string read(int max);                       // up to max bytes
}
```
- **Read a whole file:** `using File f = File(path, std::read); string text = f.read(f.size());`
  (or loop `readln()` accumulating). `f.size()` gives the byte length up front. Wrap in `using` so
  it always closes.
- **Write:** `using File f = File(path, std::write); f.write(json::render(doc));`.
- Combine modes with `|`: `std::read | std::write`. `binary` is inert (text reads only) until
  `Block` I/O lands — fine, Recon's data files are text/JSON.
- **Directory ops** (for a config dir): `std::sysMkdir(path) -> int`, `std::sysListDir(path) ->
  Array<string>?`, `std::sysRemove`, `std::sysRename`. `env::get("HOME") -> string?` for the config
  location (e.g. `~/.config/recon/`).

## 4.9 Process / env / exit

```lev
namespace env { Array<string> args(); string name(); string? get(string key);
                void exit(int code); void setExitCode(int code); }
```
- CLI args: `env::args()` (e.g. `recon collection.json` to open a collection on launch).
- Config paths / editor: `env::get("HOME")`, `env::get("EDITOR")`, `env::get("NO_COLOR")`.
- Exit codes: `env::exit(code)` (immediate) / `env::setExitCode(code)` (on normal drain). Uncaught
  exception → exit 1. Moby's `App` runs terminal teardown before exit via `using`.

---

# 5. The Postman data model (what to import)

This is the external domain knowledge the designer needs — Recon must read these formats. All are
JSON, so `json::parse` handles the tokenizing; Recon walks the `JsonValue` tree.

## 5.1 Collection Format v2.1.0 (the primary target)

Top-level object:
```json
{
  "info": {
    "_postman_id": "uuid",
    "name": "My API",
    "description": "optional string OR { content, type }",
    "schema": "https://schema.getpostman.com/json/collection/v2.1.0/collection.json"
  },
  "item": [ <item-or-folder>, ... ],
  "auth": <auth-object>,              // collection-level default auth (optional)
  "event": [ <event>, ... ],          // collection-level pre-request/test scripts (optional)
  "variable": [ <variable>, ... ]     // collection-level variables (optional)
}
```

**`item`** is a recursive array. An element is **either a folder or a request**:

Folder:
```json
{ "name": "Users", "description": "...", "item": [ ... ], "auth": {...}, "event": [ ... ] }
```
(Distinguishing rule: **if it has an `item` array, it's a folder; if it has a `request`, it's a
request.**)

Request item:
```json
{
  "name": "Get user",
  "request": <request-object>,
  "response": [ <saved-example>, ... ],   // saved example responses (optional)
  "event": [ <event>, ... ]               // per-request scripts (optional)
}
```

**`request` object:**
```json
{
  "method": "GET",
  "header": [ { "key": "Accept", "value": "application/json", "disabled": false, "description": "" }, ... ],
  "url": <url-object-or-string>,
  "body": <body-object>,
  "auth": <auth-object>,
  "description": "..."
}
```
`request` may itself be a bare string (just a URL) in minimal collections — handle that.

**`url` (v2.1 is an object; v2.0 is often a plain string — support both):**
```json
{
  "raw": "https://{{base}}/users/:id?active=true#frag",
  "protocol": "https",
  "host": [ "{{base}}" ],                 // array of dot-segments, join with "."
  "port": "8080",
  "path": [ "users", ":id" ],             // array of slash-segments; ":id" is a path variable
  "query": [ { "key": "active", "value": "true", "disabled": false }, ... ],
  "hash": "frag",
  "variable": [ { "key": "id", "value": "42" }, ... ]   // path-variable values
}
```
**Recommended:** trust `url.raw` as the source of truth (it already contains `{{vars}}`), interpolate
variables into it, then parse the *resolved* string with Recon's own URL parser (§6.1). The
structured fields are a convenience/fallback. If `url` is a plain string, that string *is* `raw`.

**`body` object (mode-tagged):**
```json
{ "mode": "raw", "raw": "{\"a\":1}",
  "options": { "raw": { "language": "json" } } }                 // raw text (json/xml/text/...)
{ "mode": "urlencoded", "urlencoded": [ {"key","value","disabled"} ] }   // form-urlencoded
{ "mode": "formdata",   "formdata": [ {"key","value","type":"text"|"file","src","disabled"} ] }  // multipart
{ "mode": "graphql",    "graphql": { "query": "...", "variables": "..." } }
{ "mode": "file",       "file": { "src": "/path" } }             // binary file upload
```
- `raw` → send as-is; set `Content-Type` from `options.raw.language` (json → `application/json`).
- `urlencoded` → build `k=v&k2=v2` with `percentEncode`, `Content-Type: application/x-www-form-urlencoded`.
- `formdata` → assemble a `multipart/form-data` body with a boundary (text parts fine; `file`
  parts are **limited** — bodies are text-only, so binary file upload is out of scope for v1, or
  reads a text file and inlines it). `Content-Type: multipart/form-data; boundary=...`.
- `graphql` → `Content-Type: application/json`, body `{"query": ..., "variables": ...}`.
- `file` → binary upload — **not supported in v1** (text bodies only); surface it as "unsupported"
  rather than silently dropping.

**`auth` object (`type` + a nested config array of `{key, value, type}`):**
```json
{ "type": "bearer",  "bearer":  [ { "key": "token",    "value": "{{token}}" } ] }
{ "type": "basic",   "basic":   [ { "key": "username", "value": "{{u}}" }, { "key": "password", "value": "{{p}}" } ] }
{ "type": "apikey",  "apikey":  [ { "key": "key", "value": "X-Api-Key" }, { "key": "value", "value": "{{k}}" }, { "key": "in", "value": "header"|"query" } ] }
{ "type": "digest",  "digest":  [ ... username/password/realm/nonce/... ] }
{ "type": "oauth2",  "oauth2":  [ ... ] }
{ "type": "noauth" }
```
Auth cascades: request auth overrides folder auth overrides collection auth. `type: "noauth"`
disables an inherited one. Recon should resolve the effective auth per request. **Recon can fully
implement `noauth`/`basic`/`bearer`/`apikey`; `digest` is buildable via `digest::md5` (§4.4);
`oauth1`/`oauth2`/`hawk`/`ntlm`/`awsv4` are stretch/omit — surface unsupported types honestly.**

**`event` object (scripts — the JS you cannot run):**
```json
{ "listen": "prerequest" | "test",
  "script": { "type": "text/javascript", "exec": [ "line1", "line2", ... ] } }
```
`exec` is an array of JS source lines (join with `\n`). **Recon cannot execute these.** Import them
so they're visible/editable-as-text, and offer the native assertion/extraction layer as the
substitute (see §6.7). Common patterns you *could* pattern-match if ambitious:
`pm.environment.set("k", ...)`, `pm.test("...", () => pm.response.to.have.status(200))`,
`pm.expect(...)` — but a robust design treats scripts as opaque text plus a native alternative.

**`variable` object:** `{ "key": "base", "value": "api.example.com", "type": "string", "disabled": false }`.

## 5.2 Environment format (separate `*.postman_environment.json` files)

```json
{
  "id": "uuid",
  "name": "Production",
  "values": [ { "key": "base", "value": "api.example.com", "enabled": true, "type": "default"|"secret" }, ... ],
  "_postman_variable_scope": "environment",
  "_postman_exported_at": "2026-...",
  "_postman_exported_using": "Postman/..."
}
```
Note: environment uses **`enabled`**, while collection variables use **`disabled`** — mind the
polarity. `type: "secret"` values should be masked in the UI.

Globals file is the same shape with `"_postman_variable_scope": "globals"`.

## 5.3 Variable resolution (`{{var}}`) — the scope chain

Postman resolves `{{name}}` by precedence (**most specific wins**):
```
local (data/iteration) > environment > collection > global
```
Recon's model: keep a `Map<string,string>` (or ordered list) per scope, resolve `{{name}}` by
walking the chain. Recon adds/edits these in the UI. Interpolation applies to **URL, headers, body,
and auth values**. **Nested/recursive resolution** (`{{a}}` whose value contains `{{b}}`) is
supported by Postman — decide whether to resolve recursively (with a depth cap to avoid loops) or
one-pass. Also Postman has **dynamic variables** (`{{$guid}}`, `{{$timestamp}}`, `{{$randomInt}}`,
`{{$isoTimestamp}}`, ...) — implement the common ones (`$guid` via `std::sysRandom`+hexEncode,
`$timestamp` via `sysNow()/1000`, `$isoTimestamp` via `DateTime::now().iso8601()`, `$randomInt`)
and leave the rest literal.

## 5.4 Version differences to handle
- **v2.0 vs v2.1:** in v2.0 `url` is frequently a plain **string**; in v2.1 it's an **object** with
  `raw`. Support both (string ⇒ treat as `raw`).
- **v1 (legacy)** is a different, flat shape (`requests` array with `folders`) — you can ignore v1
  or detect it via `info.schema` and refuse with a clear message.
- Detect version via `info.schema` (contains `v2.1.0` / `v2.0.0`). Be liberal in what you accept.

## 5.5 Export (round-trip)
Recon should be able to **write** collections/environments back as v2.1 JSON so users can re-import
elsewhere. Build the `JsonValue` tree and `renderPretty(2)`. Preserve unknown fields where feasible
(store the original `JsonValue` alongside the parsed model so export is lossless-ish) — a real
design decision (fully-parsed model vs. keep-the-raw-tree). Given Recon can't run scripts, at
minimum preserve `event` blocks verbatim so exported collections keep their scripts.

---

# 6. REST-client mechanics (the behaviors to build)

These are the algorithms Recon must implement on top of §4, because the language/library gives
primitives, not the full client.

## 6.1 URL parsing (you must write this)
Parse `scheme://host[:port]/path[?query][#frag]`:
1. Split scheme at `"://"`. Default scheme `http` if absent (or require it).
2. The authority is up to the next `/`, `?`, or `#`. Split host:port at the **last** `:` in the
   authority (mind IPv6 `[::1]:port` — bracket form). Default port 80 (http) / 443 (https).
3. Path is from the first `/` to `?`/`#` (default `/`).
4. Query is between `?` and `#`; split on `&`, each `k=v` (percent-decode for display, keep raw for
   sending). Recon may let the user edit query params as a table and rebuild the query string.
5. Fragment after `#` is not sent to the server (client-only).
Handle `{{var}}` interpolation **before** parsing (resolve variables → concrete URL → parse). Note
the built-in `HttpClient` needs `(host, port, path)` where `path` **includes** the query string
(e.g. `/users?active=true`).

## 6.2 Sending a request (end-to-end)
1. Resolve effective auth (request > folder > collection), variables, and headers.
2. Build the `HeaderMap`: user headers + auth header + cookie header (§6.5) + `Accept-Encoding:
   identity` (avoid gzip, §1.5) + content-type/body-derived headers. Don't set `Host`/
   `Content-Length` (client owns them).
3. Build the body string per body mode (§5.1).
4. Parse the URL; choose `request` vs `requestTls` by scheme.
5. Record `sysMonotonic()` start; call `client.request(method, host, port, pathWithQuery, headers,
   body, (resp) => {...})` (wrap in a `Promise` + `awaitTimeout` if you want a deadline).
6. In the callback: compute elapsed = `sysMonotonic() - start`; capture `Set-Cookie` into the jar
   (§6.5); if status is 3xx and you're following redirects, re-issue (§6.4); else update the
   response model + `invalidate()` the response panes; append to history.

## 6.3 Method/body support
Support GET/POST/PUT/PATCH/DELETE/HEAD/OPTIONS via `request(method, ...)`. `HEAD` responses have no
body; `OPTIONS` may return an empty body. Bodies apply to POST/PUT/PATCH (and technically DELETE).

## 6.4 Redirect following (you must write this)
The client does NOT follow redirects. On a 301/302/303/307/308 with a `Location` header:
- 301/302/303 → typically re-issue as **GET**, drop the body (303 always GET; 301/302 GET by common
  practice for non-GET). 307/308 → **preserve** method and body.
- Resolve `Location` against the current URL (it may be relative).
- Re-apply cookies for the new host; carry auth per your policy (usually drop auth on cross-host
  redirect for safety).
- **Cap the redirect count** (e.g. 10) and surface the chain to the user. Make following optional
  (a per-request / global toggle, Postman-style).

## 6.5 Cookies & sessions (you must write this — no cookie jar exists)
There is no cookie jar in the language; build one over `HeaderMap`:
- **Capture:** after each response, `resp.headers.all("Set-Cookie")` gives every cookie line.
  Parse each: `name=value; Domain=...; Path=...; Expires=... | Max-Age=...; Secure; HttpOnly;
  SameSite=...`. Split on `;`, first pair is `name=value`, rest are attributes.
- **Store** (`class CookieJar`): key cookies by (domain, path, name). Default domain = request host;
  default path = request path's directory. Track `secure`, `httpOnly`, expiry (`Expires` via
  `datetime::parseHttpDate`, or `Max-Age` seconds via `DateTime::now().plus(Duration::ofSeconds)`).
  A `Set-Cookie` with `Max-Age=0` or a past `Expires` **deletes** the cookie.
- **Attach:** before a request, collect cookies whose domain matches (host == domain or host ends
  with `.domain`), path is a prefix of the request path, not expired, and (`secure` ⇒ https). Emit
  one `Cookie: n1=v1; n2=v2` header.
- **Sessions:** a "session" in Recon = a named cookie jar + optionally an active environment. Let
  users have multiple sessions (e.g. logged-in-as-admin vs. anonymous), switch between them, and
  persist each jar to disk (JSON). This is how you get Postman's "session cookies" + more.
- **Persistence:** serialize the jar to JSON (domain/path/name/value/attrs) and reload on launch.
  Distinguish session cookies (no expiry — optionally cleared on exit) from persistent ones.

## 6.6 Timeouts & cancellation
Wrap the fetch in `awaitTimeout(promise, ms)` for a soft deadline (`None` = timed out). For a hard
cancel, keep the `TcpStream`/fd and `close()` it. Provide a global default timeout + per-request
override (Postman-style). Show a `Spinner`/`ProgressBar` while in flight; let `Esc` cancel.

## 6.7 The scripting substitute (Recon's native test/extract layer)
Since Postman JS scripts can't run, design a **native, declarative** replacement that covers the
90% use case:
- **Assertions** on a response: status equals/in-range; header present/equals/matches; body contains
  / matches regex; JSON path equals/exists/type. A JSON path evaluator over `JsonValue` (a small
  `$.a.b[0].c` walker) is easy to build. Show pass/fail per request (green/red) + a run summary.
- **Extractions** into variables: "JSON path `$.token` → env var `token`", "header `Location` → var
  `next`", "regex group → var". This replaces `pm.environment.set(...)` and enables chained
  requests (login → capture token → use `{{token}}` in the next request). This is the feature that
  makes Recon a real workflow tool despite no JS.
- **Pre-request** equivalents: variable-setting rules and dynamic variables (§5.3) evaluated before
  send. Keep it declarative.
- Store these as Recon-native metadata (its own JSON schema), and **preserve** the original Postman
  `event` scripts verbatim for round-trip export.

## 6.8 Response viewing
- **Status line:** code + reason + elapsed ms + body size (bytes). `resp.status`, `resp.reason()`,
  timing from §6.2, `resp.body.length()`.
- **Body:** if `Content-Type` is JSON (or the body parses via `json::parse`), pretty-print with
  `renderPretty(2)` in a scrollable `ContentBox`+`Text`; else show raw. Trim JSON number float
  artifacts (`42.000000` → `42`) for readability. XML/HTML → raw (no pretty-printer for those).
- **Headers:** a `TableView` (name | value) from `resp.headers.entries()`.
- **Cookies:** a `TableView` of what this response set + what's in the jar for this host.
- **Search** within a body (regex or substring) is a nice-to-have.

## 6.9 The typical feature checklist (parity targets)
Send request; collections tree; environments + variable interpolation; history; response
inspector (status/time/size/headers/body/cookies); cookie jar/sessions; Basic/Bearer/API-Key auth;
save/load/export collections & environments; redirect following; timeouts; request duplication;
keyboard-first navigation; import Postman collection & environment files; native assertions +
variable extraction (script substitute); optional: curl/code export, request search, folder run
(send all requests in a folder), diff of responses.

---

# 7. Footgun map — compiler & framework bugs you MUST design around

These are real, currently-open defects (or hard language rules) that have bitten every Moby track.
Design so Recon never hits them. Numbers are `bug.md` entries as of this research.

**Language / codegen (affect any Recon code):**
- **#53 (P0-ish) — bare-`this` receiver in a stored lambda:** a lambda that calls a **sibling
  instance method with a bare receiver** (`doThing()` instead of `this.doThing()`), once stored
  (e.g. in `onKey`) and invoked from another call frame, **silently drops the call on IR and
  segfaults on native/LLVM.** **Rule: in every handler/lambda, call instance methods with an
  explicit `this.method()`.** Field reads/writes in the lambda are fine bare.
- **#50 — char literal in call-argument position doesn't retype to `char`.** Bind `char c = 'x';`
  first, or compare via `.code()`. (`isChar('x')` won't work; `isChar(c)` will.)
- **#51 — nullable function type `((T)=>R)?` fails to parse.** Don't declare an optional
  function-typed field; use a non-nullable field + a `bool has...` flag (as `Input`'s validator does).
- **#52 — calling an array-index result directly (`arr[i]()`) fails to lower on IR/native.** Bind
  `var f = arr[i]; f();`.
- **#18 — `Map.with(...)`/`.without(...)` by name broken on LLVM/ELF.** Use `m[k] = v` bracket sugar
  always. (On the interpreter lane the named forms work, but write bracket-sugar for portability.)
- **#49 — `Map<K, Struct>` as a class field corrupts on LLVM at 3+ entries.** Never store a struct
  as a map value on a class field. Use a `class` value, or `Map<K,int>` indexing parallel arrays.
- **#41 — `Array<struct-with-enum-field>` goes stale after unrelated heap activity** (emit-C++/LLVM).
  If you need an array of records whose fields include an enum, **make the element a `class`, not a
  `struct`.** (Moby's `Chord`/`TableColumn`/`TreeRow` are classes for exactly this reason.)
- **#34 — overload scoring: a lambda literal wrongly scores as applicable to a `string` param.**
  Within a class, **declare lambda-taking overloads before string-taking same-name overloads.**
- **#30 — `Map<K, recursive-class>` corruption** was fixed for JSON on LLVM; still, keep Recon's
  config in TOML/JSON files and rely on the fixed `json` path (interpreter lane is unaffected).
- **#35 — don't `await` a bare global `Promise` across a `spawn`.** Irrelevant if Recon stays
  single-threaded (recommended).
- **Language rules that read like bugs but are by design:** no truthiness (`if (x != None)`, never
  `if (x)`); overridden same-arity overloads are a compile error (keep overridden methods
  single-arity per name); struct-copy-is-a-copy (`var r = box; r.origin.x = 5;` mutates a *copy* —
  reassign `box = box.shift(...)` instead); qualified namespace-global writes don't lower (write
  bare inside the namespace).

**Moby / paint (affect the LLVM lane only — Recon runs on interpreters, so these are "don't rely
on LLVM for the UI" notes):**
- **#67 (P0) — a drawing component painted through container→child interface dispatch segfaults on
  LLVM** (a nested `Size.w` read miscompiles). This is why **Recon's real run lane is the tree-walk
  oracle / IR interpreter.** Networking + the loop are LLVM-clean; the widget paint pipeline is not.
- **`pushClip` + 2+ writes segfaults on LLVM;** char-literal-in-ternary misevaluates on native;
  `Container.paint`'s inherited children-loop paints nothing for a `Container`+second-mixin leaf on
  oracle/IR too (Moby's framework leaves already work around this by redeclaring `paint()` — you
  inherit the fix by using the shipped components; only relevant if you build **custom** container
  subclasses, in which case redeclare `paint()`/`arrange()`/`contentRect()` on the leaf).
- **#68 — `env::get` (sysEnv) fails LLVM codegen as a non-inlined package call.** Read env vars in
  Recon's own `main()` (where it inlines) and pass values down, rather than deep inside a package.
- **#57/#58 — inside a comptime macro:** imported symbols don't resolve inside a bare IIFE; char
  literals don't retype at comptime. Only relevant if you write your own `moby!`-style macros
  (you don't need to).

**The meta-rule:** build and differential-test on the **oracle** (`trident run`) as you go; rebuild
before trusting any native/LLVM divergence (stale `build/` binaries have caused false alarms). Keep
Recon's data structures to: **classes** for anything with identity/mutation, **`Map<K,int>` +
parallel arrays** (or class values) instead of `Map<K,struct>`, **classes not structs** for array
elements containing enums, **bracket-sugar `m[k]=v`** for map writes, and **explicit `this.`** in
every lambda.

---

# 8. Packaging, building, running, testing

## 8.1 Package layout & manifest

Recon lives in `examples/recon/` as a **trident package** that depends on the Moby package.
Model the manifest on the Atlantis example (verified format):

```toml
# examples/recon/trident.toml
name    = "recon"
entry   = "main"                       # a function name, OR a file ("main.lev")
sources = ["src/*.lev"]                # globs expand alphabetically
# assets = ["views/**"]                # only if you use external .moby templates / bundled files

[[dep]]
path    = "../../moby"                # local-path dep: a directory with its own trident.toml
as      = "Moby"                      # reach it via `uses Moby;`
version = "0.2.0"
```
- **`entry`** is either a function name (gather everything, call it) or a file whose top-level
  statements drive the program. `trident` records which; the compiler never sniffs.
- **`sources`** are globs relative to the manifest; files reopening a namespace merge.
- **`[[dep]] path`** is a local directory (recursively gathered into the same whole-program unit);
  **`as`** aliases the dep's namespaces so `uses Moby;` works. Phantom-dep prevention: a file may
  only `uses` a namespace from the project or a **direct** dep.
- Moby itself has `name = "moby"`, so the dep `path` is `../../moby`. Confirm Moby's exported
  namespace is `Moby` (it is) and use `as = "Moby"`.

## 8.2 Build & run

```
trident run   [dir]     # resolve manifest → plan → compile → execute on the tree-walk ORACLE
trident build [dir]     # → native executable (emit-C++ + g++)   [App.run() unavailable here]
trident check [dir]     # parse + resolve + type-check only, no execution
leviathan --ir file     # run the IR interpreter (the other reliable Recon lane)
leviathan --expand file # show post-macro source (for moby! debugging)
```
- **`trident run` (oracle) is your primary lane.** The IR interpreter (`--ir`) is the second.
  Both run the full Moby paint path and the event loop + sockets.
- **emit-C++/`trident build`** compiles the whole package **except `App.run()`** (no event loop) —
  so a compiled Recon binary can't run the interactive loop. Not a target for v1.
- **LLVM** runs the loop and networking but currently segfaults on component paint (#67) — not a
  target until that's fixed.
- **A single bare file** (no manifest) is a "project of one"; but Recon should be a real package
  with a manifest + `[[dep]]` on Moby.

## 8.3 Testing

House style is **corpus tests**: a program whose stdout matches an `.expected` file, exit 0. Moby
provides testing seams you can reuse:
- **`TestRenderer : IRenderer`** — records a cell grid; `snapshot() -> string` (two-channel text +
  style format), `textOnly()`. Bind it instead of `AnsiRenderer` for headless UI tests.
- **`ScriptedInput : IInputSource`** — feed byte scripts / chords; drive frames with
  **`App.pumpOnce()`** (one input→layout→paint→present pass) for deterministic tests.
- For **networking tests**, `HttpServer` (in-language, over TCP) lets you run a mock server + client
  in one event loop (the hermetic loopback pattern the prelude's own HTTP tests use) — ideal for
  testing Recon's request/redirect/cookie logic without the network.
- Unit-test pure logic (URL parser, cookie parser, variable interpolation, collection importer,
  JSON-path evaluator) as plain print-and-expect programs — these are the highest-value tests and
  need no UI.
- Differential doctrine: run tests on the oracle and IR; byte-identical output. (LLVM excluded for
  UI paint per #67.)

## 8.4 Config & data-file locations
- Suggested: `env::get("HOME")` + `/.config/recon/` (create with `std::sysMkdir`), holding
  `sessions/*.json` (cookie jars), `environments/*.json`, `history.json`, `settings.json`. Imported
  collections can live anywhere the user points at (via `env::args()` or a file-open flow).

---

# 9. Capabilities vs. gaps — the consolidated table

| Recon feature | Language/framework support | What Recon must build | Notes |
|---|---|---|---|
| Import Postman collection (v2.1/v2.0) | `json::parse` (total, full JSON) | tree-walk of `JsonValue`; v2.0 URL-as-string handling | §5, §4.2 |
| Import environment/globals | `json::parse` | same; mind `enabled` vs `disabled` polarity | §5.2 |
| Collection tree UI | `TreeView` + `ITreeSource` | a source over the parsed model | §3.10 |
| Send HTTP request | `HttpClient.request` | URL parse; header/body build | §4.1, §6.1–6.2 |
| Send HTTPS | `HttpClient.requestTls` (full verify always) | pick Tls variant by scheme | no skip-verify via HttpClient; use `std::tlsConnect` verifyMode for custom CA |
| URL parsing | — (none) | full parser | §6.1 |
| Query params (edit as table) | `TableView`, `percentEncode` | build/rebuild query string | §4.3 |
| Request body: raw/json/urlencoded/graphql | strings, `percentEncode` | body assembler per mode | §5.1 |
| Request body: multipart text parts | strings | boundary assembly | binary parts out of scope |
| Request body: binary/file upload | — (text-only bodies) | surface as unsupported | §1.4 |
| Multi-line body editor | — (**no TextBox**) | design a solution | §3.2 — a real decision |
| Variables `{{var}}` + scopes | strings, maybe regex | interpolation + scope chain | §5.3 |
| Dynamic vars (`$guid`, `$timestamp`) | `sysRandom`, `sysNow`, `DateTime` | evaluate common ones | §5.3 |
| Redirects | — (client won't follow) | follow + cap + resolve Location | §6.4 |
| Timeouts | `awaitTimeout` | wrap fetch; optional hard-close fd | §4.7, §6.6 |
| gzip response bodies | — (no inflate) | send `Accept-Encoding: identity` | §1.5 |
| Cookies / cookie jar | `HeaderMap.all("Set-Cookie")`, `DateTime`/`datetime` | full jar: capture/store/attach/expire | §6.5 — none exists |
| Sessions | File I/O + jar | named jar + env, persist | §6.5 |
| Auth: Basic | `base64Encode` | header build | §4.3 |
| Auth: Bearer / API-Key | strings | header/query build | trivial |
| Auth: Digest | `digest::md5`, `hexEncode` | RFC 2617 flow | buildable |
| Auth: OAuth2/1, AWSv4, NTLM, Hawk | `hmacSha256`, `sysRandom`, `sysRsaEncrypt` | signing flows | stretch/omit; surface unsupported |
| Response inspector | `ContentBox`+`Text`, `TableView`, `renderPretty` | wiring + JSON prettify | §6.8; trim float artifacts |
| JSON pretty-print | `JsonValue.renderPretty(int)` | — | number float form quirk |
| Timing / size | `sysMonotonic`, `body.length()` | measure around callback | §4.1 |
| History | File I/O, `DateTime.iso8601` | log + persist + ListView | §6.8 |
| Save/export collections & envs | `JsonValue.ofObject/...`, `render(Pretty)` | model→JSON serializer | §5.5; preserve `event` blocks |
| Scripts (pre-request/test JS) | — (**no JS engine**) | native assertion/extraction layer | §1.1, §6.7 — the big substitute |
| Assertions / JSON path | `JsonValue`, `Regex` | assertion DSL + `$.a.b[0]` walker | §6.7 |
| Chained requests (token capture) | above | extraction → variable | §6.7 |
| Dialogs (env editor, save-as, confirm) | overlay stack in `App` (minimal) | build over raw overlays (**no `Modal`**) | §3.2 |
| Menus / command palette | — (**no BarMenu/Menu**) | keymap-driven or custom | §3.2 |
| Keyboard nav | `Keymap`, `FocusRing`, component keys | bind chords via `App.keymap()` | §3.7; R11 |
| Theming | `Theme`, built-ins, TOML | pick/ship a theme | §3.8 |
| curl/code export | strings | formatter | optional |
| Folder run (batch) | `TaskGroup`/loop | sequence sends + summary | optional; single-threaded fine |

## 9.1 Open design questions to resolve

1. **The body editor.** No multi-line editor exists. Choose: build a `TextArea` component
   (Focusable+Scrollable, line-array model, cursor math via `glyphWidth` — meaningful effort, but
   the most Postman-like), vs. external `$EDITOR` handoff, vs. file-loaded + single-line-form-only
   bodies for v1. This gates the whole "edit request" UX.
2. **The script substitute.** How much of the native assertion/extraction layer ships in v1, and
   what its own persisted schema is. This is Recon's identity move (§6.7).
3. **Model fidelity vs. round-trip.** Fully-parsed model (clean, but lossy on export) vs.
   keep-the-raw-`JsonValue` alongside (lossless-ish export, esp. for `event` scripts). Recommend
   keeping the raw tree for export fidelity.
4. **Dialogs without `Modal`.** Decide whether to build a minimal modal over `App`'s overlay stack
   or do everything in-pane (a "command bar" / inline editor style suits a keyboard TUI well).
5. **Insecure TLS.** `HttpClient` always verifies. If users need self-signed endpoints, decide
   whether to build a custom send path over `std::tlsConnect(..., verifyMode, caFile, ...)` (a
   per-request "insecure" or "custom CA" toggle) — non-trivial but doable.
6. **Binary bodies / large downloads.** Text-only bodies cap this. Decide the honest v1 boundary
   (e.g. "text responses only; binary shows size + a note").
7. **Streaming/SSE/websockets.** Out of scope (the client reads until close). State it.

---

## Appendix A — Minimal end-to-end request sketch (orientation only, not a design)

Illustrative Leviathan showing the shapes fit together (interpreter lane). This is to prove the
pieces compose; the designer decides the actual structure.

```lev
uses Moby;

// resolve URL -> (scheme, host, port, pathWithQuery) via Recon's own parser (§6.1)
class Url { string scheme; string host; int port; string path; }   // class: identity, mutation

Promise<HttpResponse> sendReq(HttpClient client, string method, Url u,
                              HeaderMap headers, string body) {
    Promise<HttpResponse> p = Promise();
    if (u.scheme == "https") {
        client.requestTls(method, u.host, u.port, u.path, headers, body,
                          (resp) => p.resolve(resp));    // note: explicit lambda, no bare-this call
    } else {
        client.request(method, u.host, u.port, u.path, headers, body,
                       (resp) => p.resolve(resp));
    }
    return p;
}

// inside an async context on the event loop:
// int t0 = std::sysMonotonic();
// HttpResponse? r = awaitTimeout(sendReq(client, "GET", url, hdrs, ""), 15000);
// if (r != None) {
//     int elapsed = std::sysMonotonic() - t0;
//     Array<string> setCookies = r.headers.all("Set-Cookie");   // feed the jar
//     string pretty = json::parse(r.body) != None ? json::parse(r.body).renderPretty(2) : r.body;
//     // ... update response model, invalidate() the panes ...
// }
```

## Appendix B — Quick reference: namespaces & where things live

- **`std`** (implicit `uses std;`): `HttpClient`, `HeaderMap`, `HttpResponse`, `HttpRequest`,
  `TcpStream`, `File`, `OpenMode`, `read/write/append`, `Promise`, `after`/`every`, `spawn`,
  `Channel`, `awaitTimeout`, `TaskGroup`, `sysNow`/`sysMonotonic`/`sysRandom`/`sysResolve`/
  `sysTcpConnect`/`connectTimeout`/`tlsConnect`, `Exception`/`RuntimeException`, `Array`/`Map`/`Set`
  aggregates, `StringBuilder`.
- **`json`**: `parse`, `render`; `JsonValue` (top-level class).
- **`encoding`**: `base64*`, `percent*`, `hex*`.
- **`digest`**: `md5`, `sha1`, `sha256`, `hmacSha256`.
- **`regex`**: `compile`/`isMatch`/`find`/`matches`/`replace`/`split`/`count`/`escape`; `Regex`,
  `Match`, `Group`, `RegexOptions`, `RegexException` (top-level classes).
- **`datetime`**: `parseHttpDate`, `parseIso8601`; `DateTime`, `Duration` (top-level structs).
- **`env`**: `args`, `name`, `get`, `exit`, `setExitCode`.
- **`math`**: `pi`, `e`, `log`/`exp`/`sin`/... , `min`/`max`.
- **`Moby`** (the dep, via `uses Moby;`): everything in §3 — `App`, `Component`, `Container`,
  `Text`/`Input`/`Button`/`CheckBox`/`RadioGroup`/`ProgressBar`/`Spinner`/`ContentBar`,
  `ContentBox`/`SplitBox`/`GridBox`/`Tabs`/`ListView`/`TableView`/`TreeView`, layout strategies,
  `Theme`, `Keymap`, event classes, geometry structs, `Moby::log`/`Moby::app()`/`Moby::Attr`/
  `Moby::Mod`. **Not present:** `TextBox`, `Modal`, `BarMenu`/`Menu`, `DebugOverlay`,
  `@Shortcut`/`@Timer`/`@Validator`.

---

## Migration note (Moby DOM D06, 2026-07-19): framework `TextArea` now exists

The framework gained a general `Moby::TextArea` (`moby/src/components/textarea.lev`, tech design
`designs/moby/dom/techdesign-06-textarea.md`). It closes every gap the local
`examples/recon/src/ui/textarea.lev` skeleton left open: cell-width cursor math (CJK/emoji-correct
`cellAt`/`scalarAtCell`), auto-scroll on both axes, a sticky desired-column for vertical moves,
PageUp/PageDown, word jumps, doc-home/end, an optional line-number gutter, readOnly gating, wide-glyph
edge clipping, per-keystroke `on:change`/`on:cursor` events, an intrinsic validator, and DOM `<textarea>`
tag/attr/`value`-channel integration.

**Proposal (owner-approved change, NOT done here as a side effect):** Recon swaps its local `TextArea`
for the framework one. Blocking difference to reconcile first: Recon's editor carries `^F` JSON
pretty-print (`formatJson`) and a `rawLanguage`/`dirtyFlag` protocol its request/response panes read.
The framework component has neither by design (syntax/format concerns are Recon-domain). The clean path
is a thin `Recon` subclass over `Moby::TextArea` that re-adds `^F` + the dirty flag via `on:change`,
rather than keeping the parallel skeleton. Left for a dedicated change.

---

*End of research dossier. Everything needed to design Recon — language, framework, library,
Postman formats, REST-client mechanics, and the footgun map — is contained above. The designer
should not need any other file.*
