# Track W — HARD 01: TLS-model wasm branch

**[HARD]** — edits `src/LlvmGen.cpp`. **Status:** scheduled (W-M1). **Context:**
`techdesign-02-backend-column.md` §3. **Depends on:** W-M0 spike verdict.

## The edit

`LlvmGen.cpp:3343-3355` pins the TLS model by object format (ELF→InitialExec,
COFF→non-TLS) and has **no wasm case** — a wasm triple silently falls to GeneralDynamic.
Add one explicit branch: for `Triple::isWasm()`, pin **LocalExec** (single-threaded v1;
wasm TLS without shared memory lowers to plain globals, and LocalExec is the honest
single-instance model).

One `else if`. Nothing else in the function. No refactor of the existing branches.

## Constraints

- Minimal diff; no drive-by changes anywhere in `LlvmGen.cpp`.
- Revisited only when the deferred Workers leg opens (`techdesign-04-async-jspi.md` §7).

## Verification

- Four-lane engine differential (oracle/IR/emit-C++/LLVM-native) on the full suite —
  byte-identical, since native triples must be untouched by the new branch.
- Wasm lane: pure-compute corpus compiles and links without TLS reloc errors.
- Any divergence → STOP (overview §8 #5), do not pin around it.
