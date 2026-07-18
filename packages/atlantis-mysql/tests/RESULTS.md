# atlantis-mysql — test results (techdesign-05)

Every case is verified byte-identical across the **tree-walk oracle** (`--run`), the
**IR interpreter** (`--run --ir`), and the **LLVM native binary** (`--build-native`).
emit-C++/ELF are out of scope for framework testing (C8). The `[heap]` accounting line
LLVM prints on **stderr** is ignored by the runner.

Run the whole suite:

```
./packages/atlantis-mysql/tests/runtests.sh            # uses build/leviathan, build/trident
```

| case | what it proves | oracle | IR | LLVM |
|---|---|---|---|---|
| `wire` | P3 lenenc edges: encode/decode round-trip at 0/250/251/252/65535/65536/0xFFFFFF/0x1000000; `u32` high-bit packing; `u64` int64-range guard throws | green | green | green |
| `protocol` | **P2** native + caching_sha2 scramble vectors (vs Python hashlib); HandshakeV10 parse (20-byte nonce, caps, plugin, PROTOCOL_41/DEPRECATE_EOF); OK/ERR(1062→`IDuplicateKeyException`) parse; EOF `<9` disambiguation; **P4** `PacketAssembler` identical output whole-buffer / byte-at-a-time / 3-byte chunks | green | green | green |
| `prepared` | binary row decode: signed LONG, DOUBLE (in-language IEEE-754), VAR_STRING, NULL bitmap 2-bit offset, FLOAT32, **unsigned BIGINT → lossless decimal string**, signed BIGINT −1; `COM_STMT_EXECUTE` body shape | green | green | green |
| `loopback` | **end-to-end** against an in-language fake MySQL 8 server over a real socket: connect + `mysql_native_password` auth + SET NAMES → READY; `COM_QUERY` text protocol with typed mapping (int/string/float/null); prepared INSERT (params → `ExecResult`); prepared SELECT (binary rows via EXECUTE FSM); transaction begin/execute/commit; duplicate-key **typed** catch by capability interface; PING; clean loop exit | green | green | green |
| `pool` | pool churn: 5 acquires over `maxConnections=2` reusing idle; `withConnection` one-liner; `drain()` closes all and the loop exits | green | green | green |

## Milestone coverage (design §10)

- **M0** probes: P1 (byte-safe socket) implicitly via the loopback round-trip; **P2** (scramble
  vectors) in `protocol`; **P3** (lenenc edges) in `wire`; **P4** (assembler fragmentation) in
  `protocol`; **P6** (exception-through-`await`) settled ✓ — a throw after an internal `await`
  propagates the **typed** exception synchronously to the caller's `await` (capability
  interfaces survive), so no outcome-union interim was needed. P5 (buffer-cursor perf spike)
  not run here (perf-only).
- **M1** wire core (Wire, `PacketAssembler`, OK/ERR/EOF, error family) ✓
- **M2** handshake + `mysql_native_password` + PING/QUIT ✓ (loopback)
- **M3** `COM_QUERY` text protocol + type mapping + error family ✓
- **M4** C3 conformance (Connection / ResultSet / Row / ExecResult) + transactions + Driver ✓
- **M5** prepared statements (PREPARE/EXECUTE/CLOSE, binary rows, stmt cache) ✓
- **M6** pool (acquire/release/waiters/ping/eviction/drain) ✓
- **M7** caching_sha2 full path + TLS/RSA routes: **implemented** behind `Options.tlsMode`
  (dead by default; the fast-path scramble is P2-validated). Live acceptance vs a real
  MySQL 8.4 `caching_sha2` + TLS server (§7.2 second CI job) is **not run here** — no server
  is available in this environment, and the routes activate by config per §2.4.

## Not run here (needs infrastructure)

- **§7.2 live Docker acceptance** (`acceptance.lev` vs `mysql:8.0`) — no Docker/MySQL in
  this environment. The loopback fake server exercises the same wire surface end-to-end on
  all three engines as a stand-in; a real-server job drops in unchanged (same C3 calls).

## Compiler issues found (filed)

- **#82 [P1]** cross-package nested-namespace `const int` reads as 0 when fully-qualified
  (silent) — worked around with `uses …;` + bare names.
- **#83 [P1]** implementing a dependency's interface needs bare (uses-imported) member types,
  and `uses` must appear in every source file of the package — worked around throughout.
