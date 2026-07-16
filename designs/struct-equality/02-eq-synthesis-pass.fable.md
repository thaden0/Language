# Packet 02 — the `(==)` synthesis pass (Model: **Fable**)

The heart of the design (§5.5): materialize the derived struct `(==)` as a
**real generated method decl** through the same channel `desugarEnums` uses.
This is the single Fable packet — everything downstream (checker gate,
fallback extermination, canon leg) hangs off decisions made here. Read design
§5 in full before writing code.

## Goal

A new Resolver pass, `synthesizeStructEquality(Program&)`, that for every
*user-program* value struct with **no explicit `(==)`**:

- classifies every field by the §5.2 comparability ladder;
- if all fields are comparable → generates and splices a
  `bool (==)(T other)` member whose body is the field-wise conjunction;
- if not → generates nothing and records *why* (struct name, first bad
  field name, bad-kind description) in Program-level metadata for the
  checker (packet 03) to report.

The pass itself emits **no diagnostics** — non-comparability only becomes an
error at a `==`/`!=` use site (design §5.1: the gate fires at the compare,
not at the declaration).

## Where the pass runs

Inside `Resolver::run` (`Resolver.cpp:6048`), immediately after
`desugarEnums(program)` and after symbols are gathered — the classification
needs symbol lookup (is this Named type a struct? a class? an enum?), so the
correct anchor is after `gatherInto(program.items, sema_.global)`
(`Resolver.cpp:6062`) and **before** shapes are built (`buildShape` loop at
`Resolver.cpp:6112`). Appending a member to `cls->decl->body` before
`buildShape` is sufficient for it to become an ordinary `"=="` slot
(`slotOf`, `Resolver.cpp:5620/5628`) — verify the gather step does not
itself enumerate members (it gathers top-level decls into scopes; member
slots come from `buildShape`). If gather DOES touch members, run the pass
between `desugarEnums` and gather and do symbol classification lazily via a
name→decl map you build yourself — but verify first; do not guess.

## The comparability ladder (§5.2) — classification rules

Classify from the field's `TypeRef` (syntax) + symbol lookup:

| Field type | Verdict | Generated compare |
|---|---|---|
| `int`, `bool`, `char`, `string` | comparable | `this.f == other.f` |
| `float` | comparable | `this.f == other.f` — **IEEE for now, deliberately** (see "Float deviation" below) |
| enum (desugared struct) | comparable | `this.f == other.f` (its own synthesized `(==)`) |
| value struct | comparable iff that struct is (recursive) | `this.f == other.f` |
| reference class / interface | comparable | `this.f == other.f` (identity — checker branch `Checker.cpp:2060-2065` types it; engines keep the class-identity path) |
| `T?` / union | comparable iff every non-`None` member is | `this.f == other.f` (None tag-compare already works end-to-end: `Checker.cpp:2069-2070` types it, `RuntimeValue.hpp:357-361` + `Eval.cpp:1059-1061` execute it) |
| `Array<...>`, `Map<...>`, `Block`, function types (`Fn`/arrow TypeRefs), `Ast`, `Promise`, `Channel` | **not comparable** | gate fires at use site |
| anything unresolvable | **not comparable** (fail closed, never silently) | gate fires |

Recursive struct classification: memoize verdicts per `Symbol*`; guard
cycles (value structs can't cycle by value — infinite size is already
rejected — but a defensive in-progress mark that yields "not comparable"
on re-entry protects against error-recovery states).

### Float deviation (deliberate, temporary)

Design M1 says "ladder minus floats", but excluding floats in this packet
would make float-field structs a compile error until packet 05 — a
regression window over today's behavior. Instead: float fields synthesize
with plain `==` here (bit-for-bit what `keyEquals` does today), and
**packet 05 flips exactly that leg to `canonEq`**. End state identical to
the design; no broken interval. Leave a `// packet 05 flips this to canonEq`
comment at the generation site.

## Generation mechanics (mirror `desugarEnums`, `Resolver.cpp:5990-6016`)

1. Build source text for a host struct wrapper so the parser has a member
   context:
   ```
   struct __eq_T {
       bool (==)(T other) => this.f1 == other.f1 && this.f2 == other.f2 && ...;
   }
   ```
   Zero-field struct → `=> true;`. Long bodies: `&&`-chain is fine (design
   §5.3 — first mismatch short-circuits; all legs pure).
2. `program.synthFiles.push_back(SourceFile{"<eq T>", std::move(src)})`,
   lex+parse `.back()` with a local `DiagnosticSink dummy`; on parse error →
   `sink_.error(cls->decl->span, "internal: struct 'T' (==) synthesis failed to parse")`
   (should be unreachable; keep it loud like `Resolver.cpp:6018`).
3. Lift the single `StmtKind::Member` out of the parsed wrapper class's
   `body` and `push_back` it into the real struct's
   `const_cast<Stmt*>(cls->decl)->body` (precedent:
   `RuleEngine::injectMember`, `Rules.cpp:2455`). Mark it (see below).
4. Record metadata on `Program` (new, alongside `enumDesugars` in
   `Ast.hpp:508`):
   ```cpp
   struct StructEqSynth {
       std::string_view structName;   // backed like synthNames
       bool synthesized;              // false => gate info below is set
       std::string badField;          // first non-comparable field
       std::string badKindNote;       // e.g. "a function value", "an Array"
   };
   std::vector<StructEqSynth> structEqSynths;
   ```
   The checker keys off this by struct symbol name (packet 03). Message
   vocabulary should let packet 03 produce exactly the design §5.1 shape.

## Field-name capture

Generate `this.f == other.f` with the field's real name. Struct fields are
`StmtKind::Member` entries in `decl->body` with `callable == false`, not
ctors/accessors (`Ast.hpp:360-393`; skip `isCtor`/`isGet`/`isSet`). Declaration
order = body order (design §5.3). Skip nothing else — a field you cannot
classify is "not comparable", never "skipped".

## Idempotency & the two-pass resolver (CRITICAL)

`Resolver::run` executes twice when the rule stage changes the program
(`main.cpp:466-472`), over the SAME `Program`. Rules can inject fields into
structs between the passes (`injectMember`). Therefore:

- Add a `bool isSynthEq = false;` flag to `Stmt` (`Ast.hpp`, near the other
  member bools).
- The pass FIRST erases any `isSynthEq` member from every struct body and
  clears `program.structEqSynths`, THEN regenerates from the current field
  list. This makes the pass idempotent AND pass-2-correct (a rule-injected
  field lands in the regenerated body; a rule-injected explicit `(==)`
  suppresses synthesis).
- A struct that declares any explicit `(==)` (symbolic selector text
  `"=="`) gets **no synthesis** — this also covers desugared enums
  (`Resolver.cpp:6000` gives them one) and makes re-runs naturally skip
  already-explicit types.

Scope: user program items only (recurse into `StmtKind::Namespace` bodies,
same walk shape as `desugarEnums`); never prelude classes (`parsePrelude`'s
program is separate — don't touch it).

## Warnings / footguns

- **string_view lifetime**: every name/text in the spliced AST must point
  into `program.synthFiles`/`synthNames` (deques — stable). Never into a
  local `std::string`.
- **`--expand` visibility** (design §5.5): synthesized members will show up
  in `--expand-ast`/`--expand` output. That's desired. Check the expand
  round-trip test (`corpus_meta_expand_roundtrip`) still passes — the source
  printer must be able to print a symbolic `(==)` member (it already prints
  enum desugars' one... but those are spliced as whole structs; verify the
  member prints inside a user struct).
- **Generic structs**: if the struct has type params, `T` in the generated
  header must be spelled with them. Find how enum/ordinary code canonicalizes
  a struct's self-type; if generics make the source form ambiguous, restrict
  v1 synthesis to non-generic structs ONLY IF generic value structs +
  `==` have no existing corpus coverage (check first) — and record the
  restriction in the packet commit message and `structEqSynths` gate note.
- **Do not** re-implement any field-walk in engines here. This packet only
  adds the pass + metadata + flag. Engine fallbacks die in packet 04, so
  after this packet both mechanisms coexist: dispatch finds the generated
  method FIRST (`findMethod` precedes the fallback in all four engines), the
  fallback becomes dead for comparable structs. That ordering is what keeps
  this packet safely committable on its own.
- Structs compared in **comptime** code (RuleEngine oracle) run between
  pass 1 and pass 2 — synthesis in pass 1 means the method already exists
  there. Fine. But if a *rule-generated struct* is compared *inside comptime
  of the same run*, it has no synthesized `(==)` until pass 2; the old
  fallback still exists until packet 04, so behavior is unchanged in this
  packet. Packet 04 revisits this edge.

## Acceptance

- Full `ctest` green, including packet 01's cluster (now exercising the
  generated method for the no-explicit-`(==)` pins — verify via `--expand`
  on `struct_eq_basic.lev` that the `(==)` member appears).
- Four-lane byte-identical on the equality cluster.
- New checker unit tests NOT yet (no diagnostic yet — packet 03).
- Add one green pin: `struct_eq_expand_visible` is NOT needed as a pin;
  instead manually confirm `build/leviathan --expand-ast` shows the member
  and note it in the commit message.

Commit: `Struct equality packet 02 (M1): synthesize derived (==) via the generated-decl channel`.
