# Threads corpus — serial reference engines (oracle + IR)

Track 10 (`designs/complete/techdesign-threads-2.md`, `-3.md`).
`spawn`/`Worker<T>`/join and the flatten/rebuild copy boundary, run on the
**serial** engines. Workers are cooperative loop tasks here — the tree-walk
oracle and IR interpreter are the semantic reference the LLVM true-thread run
matches byte-for-byte. The true-thread (LLVM) counterparts of the shared programs
live in the sibling `threads_native/` directory (M3 landed); this directory keeps
the serial-only lane, plus `probe.lev` (the `sysThreadStart` = -1 capability
probe, meaningful only where there are no true threads).

Output-determinism discipline (§8): **workers compute and return/send; only the
spawning thread writes, at join points.** Never print from a worker body — that
would race stdout ordering once M3 runs these on real threads.

`native_floor.lev` (the `sysTcpListen(port, reusePort)` overload and
`std::cpuCount()`) lives in the sibling `threads/` dir, which runs on all three
active engines.
