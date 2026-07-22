#pragma once
#include "core/Diagnostic.hpp"
#include "ir/Ir.hpp"
#include <string>

// The LLVM backend: translates the IR to LLVM IR via the C++ API and emits a
// native object file directly (TargetMachine — no external llc). Values cross
// into the runtime as `LvValue* {tag, payload}` pointers per the ABI contract
// in docs/techdesign-portable-backend-2.md §2 (Track B). Generated code calls
// the runtime's `lvrt_*` C functions; today that means the temporary local
// stub at tests/support/llvm_stub_runtime.c (see docs/techdesign-portable-
// backend.md A-M2) until Track B's liblvrt.a lands — a pure relink at that
// point, since the ABI is shared.
//
// Coverage is being brought to parity with the pure x86-64/ELF backend
// (docs/techdesign-portable-backend.md); out-of-coverage ops fail with a
// diagnostic. Link a binary with:
//   leviathan --native-obj out.o prog.ext && cc out.o tests/support/llvm_stub_runtime.c -o prog
class LlvmGen {
public:
    LlvmGen(const IrModule& mod, DiagnosticSink& sink) : mod_(mod), sink_(sink) {}

    // Textual LLVM IR (--emit-llvm), or "" on failure.
    std::string emitIr();

    // Native object file (--native-obj). Returns false on failure.
    // `triple`: LLVM target triple for cross emission (Track B doc §6 B-M4,
    // e.g. "aarch64-linux-gnu"); empty = the host's default triple.
    // `optLevel`: 0 or 2 (§9 A-M6 hurdle H-12) — a PassBuilder module
    // pipeline at that level runs before object emission; default 2, 0 for
    // a debug build (faster codegen, easier to read in a debugger).
    bool emitObject(const std::string& path, const std::string& triple = "",
                    int optLevel = 2);

private:
    const IrModule& mod_;
    DiagnosticSink& sink_;
};
