# Refactor 1 — Session 04: Rules + Parser TU Splits (sonnet)

> Goal: same pure-code-motion treatment as session 03, applied to
> `Rules.cpp` (2,864 lines, ~153 fns) and `Parser.cpp` (2,351 lines,
> ~257 fns). No behavior change. Date: 2026-07-23. Depends on: 01.
> Parallel with: 02, 03, 05.

## Files owned by this session

- `src/Rules.cpp` (shrink), `src/RulesInternal.hpp`, `src/RulesExpand.cpp`,
  `src/RulesClone.cpp` (fill)
- `src/Parser.cpp` (shrink), `src/ParserInternal.hpp`, `src/ParserExpr.cpp`,
  `src/ParserMeta.cpp` (fill)
- `src/Rules.hpp`, `src/Parser.hpp` (comment-only edits)

## Split mapping — Rules

| Destination | What moves |
|---|---|
| `RulesExpand.cpp` | declaration-level expansion: `collectRules`, `orderRules`, `matchAndExpandRule`, `runReentrantFixpoint`, splice-site indexing (`indexSpliceSites`, named anchors) |
| `RulesClone.cpp` | the hygienic AST cloner: `cloneStmt`, `cloneExpr`, `cloneType`, `collectTemplateLocals` and rename machinery |
| `Rules.cpp` (stays) | phase 1: `foldExpr`, `foldParamDefaults`, `processAttrs`, `resolveAttr`, `evalAttrArgs`, `reify`, `buildMetaValue`, engine driver (`run`) |

## Split mapping — Parser

| Destination | What moves |
|---|---|
| `ParserExpr.cpp` | expression side: `parseExpr(minBP)`, `parseUnary`, `parsePostfix`, `parsePrimary`, lambda disambiguation (`typedParamAhead`, `looksLikeLambda`, `skipTypeLA`) |
| `ParserMeta.cpp` | metaprogramming grammar: `parseRule`, `parseMacroDecl`, `parseAttributeDecl`, `parseForSpliceStmt`, `parseForkSpliceStmt`, all `parse*Fragment` |
| `Parser.cpp` (stays) | statements/declarations (`parseClass`, `parseEnum`, `parseNamespace`, `parseBind`, `parseUses`/`parseUse`, …), `synchronize()`, token cursor plumbing |

## Method

Identical to session 03's: enumerate definitions, assign per table
(ambiguous → STOP), move one destination per step, compile each step.
Helper placement rule as in 03: single-user statics move with their user;
multi-user statics (~17 in Rules.cpp, ~11 in Parser.cpp) become
`rulesDetail::` / `parserDetail::` functions declared in the respective
`*Internal.hpp`. Headers get `// defined in X.cpp` grouping comments only.

## Validation

- Full clean build, zero new warnings; `ctest -j4` green (flake policy per
  overview). Metaprogramming coverage matters here: the run must include the
  rules/macro/attribute and `--expand` test groups.
- `git diff` pure-motion review as in 03.
- No resulting TU exceeds ~1,500 lines.

## Ending state (fixed)

`Rules` implemented across 3 TUs + internal header; `Parser` across 3 TUs +
internal header; behavior identical.

## STOP-and-escalate

Same triggers as session 03. Additionally: if moving the cloner exposes a
hidden dependency on fold-phase file-statics that cannot be expressed through
`RulesInternal.hpp` without changing semantics, STOP and escalate.
