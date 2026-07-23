# Summary: LLVM native codegen for `sysListDir`/`sysRemove`/`sysRename`

Track 08's directory/fs-metadata natives (`designs/complete/techdesign-08-system-natives.md`
F3) landed on the tree-walk oracle and IR interpreter (`src/RuntimeNatives.cpp`) but were never
wired into the LLVM native backend. `sysStat`/`sysMkdir` — the other two members of the same
F3 family — DID get LLVM support (`src/LlvmGen.cpp`'s native dispatch has `rtSysStat`/
`rtSysMkdir` cases, backed by `lvrt_sysstat`/`lvrt_sysmkdir` in `runtime/lv_runtime.c`); the
other three simply never made the same trip. Sonar DOM techdesign-07 (`FileDialog`, a real
directory-listing file picker) is the first feature to actually need `sysListDir` at runtime,
and hit the gap building a native binary: `LLVM backend: native floor function 'sysRemove'`
(the "generic unimplemented native" fail every un-lowered native produces, `src/LlvmGen.cpp:2864`
/ `src/X64Gen.cpp:3860`).

## Request Details

Any package that lists a directory, deletes/renames a file, or builds a native binary that
does so is blocked today — not a corner case, but the ordinary shape of a file-manager UI, an
installer, a build tool, or (concretely, now) a TUI file-open dialog. The fix is mechanical
plumbing, not new design: the interpreter-side behavior (`src/RuntimeNatives.cpp`'s
`sysListDir`/`sysRemove`/`sysRename` handlers, quoted below) is the exact spec to port; `sysMkdir`
sitting one function above each of them in the same file is the direct porting template
(`lvrt_sysmkdir` in `lv_runtime.c` + the `rtSysMkdir` dispatch case in `LlvmGen.cpp`).

The one new wrinkle relative to `sysStat`/`sysMkdir`: `sysListDir` returns `Array<string>?`
(an optional heap array), not a bare scalar int. Its LLVM lowering needs to build an
`Array<string>` value and set the ARC retain the existing `retainDst()` helper already applies
to other fresh-heap-value natives (`sysReadLine`, `sysRead`'s string form, `sysArgs` — all three
already in the dispatch table as a `retainDst()`-following-`CreateCall()` pair) plus the `None`
case (directory unreadable/nonexistent) — likely a null/sentinel array the checker's `?`
handling already expects from other optional-returning natives, if one exists; if not, this is
the one piece of the three that needs an actual (small) design decision rather than pure
plumbing.

## Requested Specific Feature

```
runtime/lv_runtime.c:
    void lvrt_sysremove(LvValue* out, const LvValue* path);
    void lvrt_sysrename(LvValue* out, const LvValue* from, const LvValue* to);
    void lvrt_syslistdir(LvValue* out, const LvValue* path);   // Array<string>? result

src/LlvmGen.cpp (native dispatch, alongside the existing sysStat/sysMkdir cases):
    else if (n == "sysRemove")  { b.CreateCall(rtSysRemove,  {regs[in.a], arg(0)}); }
    else if (n == "sysRename")  { b.CreateCall(rtSysRename,  {regs[in.a], arg(0), arg(1)}); }
    else if (n == "sysListDir") { b.CreateCall(rtSysListDir, {regs[in.a], arg(0)}); retainDst(); }
```

Same three natives for the frozen pure-ELF backend (`src/X64Gen.cpp`) if that backend is meant
to carry the rest of the F3 family too — `sysStat`/`sysMkdir`'s own ELF coverage status (present
or deferred) is the precedent to match; not investigated as part of filing this ticket.

## Known Warnings

- `sysListDir`'s `Array<string>?` return is the one piece here that isn't pure plumbing (see
  above) — resolve the `None`-representation question against however another
  optional/nullable-returning native already lowers on LLVM (if one exists) before writing the
  `Array<string>` construction code, rather than inventing a new convention.
- `sysRemove`'s fallback-to-`rmdir`-on-`EISDIR`/`EPERM` behavior (a file-vs-directory
  disambiguation baked into the interpreter implementation, not a separate native) must be
  preserved byte-for-byte in the C port — a differential test comparing all three engines on
  both a file and a directory target is the cheap way to pin it.
- Errno-to-return-code mapping (`0`/`-1`) is already the established `sysStat`/`sysMkdir` shape;
  reuse it rather than introducing a new sentinel convention for these three.

## Acceptance Criteria

1. `--build-native`/`--build` on a package calling `sysListDir`, `sysRemove`, or `sysRename`
   succeeds (no "native floor function" fail) and links.
2. A differential corpus case (list a directory, remove a file, remove a directory, rename a
   file, list a nonexistent path) is byte-identical across oracle, IR, and LLVM.
3. `moby/tests/dom-dialogs/` (techdesign-07) — currently the first real caller, failing on
   the LLVM lane only, oracle+IR green — passes on LLVM once this lands; no changes needed on
   the Sonar DOM side (`FileDialog` already calls only the documented `std::sysListDir`/
   `std::isDir`/`std::fileExists` surface).

## Interim Fallback

None workable for a real directory-listing feature — `sysListDir` has no alternative native
path. `moby/tests/dom-dialogs/` ships with its LLVM lane left failing
(`runtests.sh` reports `FAIL dom-dialogs (llvm codegen)`), documented in techdesign-07's
implementation log rather than worked around, since trimming the feature to avoid the native
would defeat the point of a real file dialog. Oracle and IR are unaffected and both pass
byte-identical.
