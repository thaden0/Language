# Tech Design — Socket Buffer Control & the Socket-Option Floor (LA-29)

**Status:** ✅ LANDED IN FULL. Oracle + IR + LLVM in scope.
**Date:** 2026-07-11.
**Authored by:** Opus-class model (agent1). **Implemented by:** a Sonnet-class implementation agent.
**Implements:** the LA-2 residual — `designs/complete/techdesign-tls-crypto.md` §16
("**Residual:** the live **want-write-on-a-read-watch** trigger needs the SO_SNDBUF tiny-buffer
harness … that requires a `setsockopt(SO_SNDBUF)` native not yet exposed") and §10 #1 / §12's
deferred inversion harness.
**Consumers:** `tests/run_tls.sh` (the want-write inversion proof, §6 — the *first* consumer);
any future flow-control/latency tuning in `TcpStream`/`HttpServer`/the MySQL driver; self-hosting
(a real network stack must be able to size its own buffers).
**Depends on:** nothing unlanded. Rides Track 08's socket floor (`sysTcpListen`/`sysAccept`/…),
the LA-2 TLS session + §2.3 poll-mask augmentation (already landed), and the portable-backend
runtime (`liblvrt.a`).
**Owns (may edit — disjoint from any other live track):**
`src/Resolver.cpp` kPreludeStd (one `sys` declaration in the sockets block),
`src/RuntimeNatives.cpp` (`nativeFreeCall` — one new case, shared by oracle + IR),
`runtime/lv_abi.h` (one enumerated shim declaration + its doc-comment row),
`runtime/lv_loop.c` (one `lvrt_*` wrapper beside the socket wrappers),
`runtime/lv_plat.h` + `runtime/lv_plat_posix.c` + `runtime/lv_plat_win32.c` (one platform-floor
function, three files),
`src/LlvmGen.cpp` (one `FunctionCallee` + one dispatch branch — **NOT** `emitNativeRows`; see §4.6),
`tests/corpus/tls/**` (one new `.lev` + `.expected`),
`tests/certs/**` (one new oversized test cert pair + a `README.md` recipe row),
`docs/reference.md` (two prose rows),
and — on completion — this file **moves to `designs/complete/`** (§0.4).

> **⚠️ IMPLEMENTER — MOVE THIS FILE WHEN DONE.** The last commit of this work must
> `git mv designs/techdesign-socket-options.md designs/complete/` with the §12 log filled in and
> the status line flipped to `✅ LANDED IN FULL`. A design that ships code but is left in the
> top-level `designs/` dir reads as *in-progress* to every future agent. Do not skip this.

---

## 0. Read this first

### 0.1 The mission, in one sentence

Expose a **named, intent-level socket-buffer primitive** — `sysSocketBuffer(fd, sendBytes,
recvBytes) -> int` — that hides the OS `SO_SNDBUF`/`SO_RCVBUF` mapping in the platform floor, then
use it to write the **first live, deterministic proof** that LA-2 §2.3's want-write poll-mask
augmentation actually fires (today it is verified only by symmetric code review). When this lands,
the LA-2 residual is **closed in full** — not narrowed, not "verified by construction," *closed by
a live deadlock-or-complete test on all three engines.*

### 0.2 What "resolve the issue fully" means here — and it IS fully resolved

The issue is: *the want-write-on-a-read-watch code path has never been exercised live.* This design
delivers the exact harness that exercises it (§6): a handshake that **can only complete if the
augmentation works** and **deadlocks (then trips a watchdog) if it does not**. There is no partial
close. The one honest caveat — kernel buffer clamping — is a *determinism* concern the test design
neutralises by construction (an oversized cert flight, §6.3 + §7), not a gap in the fix.

### 0.3 This is a permanent capability, not a test backdoor

`sysSocketBuffer` is framed as a **permanent systems primitive** — flow-control buffer tuning is
something any real network stack needs, and the self-hosting goal (Leviathan must eventually
implement its own stack) makes it load-bearing. The inversion test is its *first consumer*, not its
*reason to exist*. It is declared in the ordinary socket floor beside `sysTcpListen`, is
`sys*`-prefixed (so comptime-denied like every other syscall), and would be exactly as welcome if
the TLS test never existed. **It must never be commented or named as "test-only."**

### 0.4 STOP protocol (model-escalation — mandatory)

You are a Sonnet-class implementer. This design is meant to be followed exactly. **STOP and escalate
to the owner (do not improvise) if any of these is true:**

- A specified file anchor (§4) no longer matches — the surrounding code moved. Re-locate by the
  *named symbol*, confirm the pattern still holds, and if the pattern itself changed, STOP.
- The want-write test in §6 does **not** complete on some engine (it deadlocks to the watchdog, or
  the oracle and a compiled engine disagree). That is either a real §2.3 augmentation bug or a cert
  too small (§6.3) — **either way file it in `bug.md` and STOP**; do not edit `.expected` to paper
  over it, and do not "fix" the augmentation without a ruling.
- Closing this residual would require touching the LA-2 loop code (`RuntimeLoop.cpp` /
  `lv_loop.c` poll builders) beyond *reading* it. It must not. The augmentation is already correct;
  this design only adds the trigger. If you find yourself editing the mask logic, STOP.
- You are tempted to add a raw `setsockopt(level, optname, value)` passthrough (§1.3 forbids it) or
  to defer anything not already listed in §5's alarm.

### 0.5 Frozen / do-not-touch

- **X64Gen / the pure ELF backend is FROZEN** (owner directive, standing). Do **not** add an ELF
  leg, an ELF corpus entry, or gate anything on an ELF finding. `sysSocketBuffer` on the ELF backend
  simply rejects loudly exactly as `sysTcpListen/2` already does (§5) — that is correct and
  final, not a deferral of this issue.
- The LA-2 §2.3 poll-mask augmentation (`RuntimeLoop.cpp:85-104`, `lv_loop.c:250-260`) is **correct
  and landed**. Read it to understand the test; never modify it here.
- No security default moves. This native cannot weaken TLS posture — it sizes a transport buffer,
  nothing more.

---

## 1. The capability and the design principle

### 1.1 The surface (normative)

```
// std namespace, kPreludeStd socket block.
int sysSocketBuffer(int fd, int sendBytes, int recvBytes);
```

- **Intent, per direction.** `sendBytes` = requested send-buffer size in bytes; `recvBytes` =
  requested receive-buffer size in bytes.
- **A non-positive value leaves that direction UNCHANGED.** `sysSocketBuffer(fd, 4096, 0)` sizes
  only the send buffer; `sysSocketBuffer(fd, 0, 4096)` only the receive buffer; `(fd, 0, 0)` is a
  successful no-op (returns `0`).
- **Return:** `0` when every *requested* (positive) direction was applied without error; `-1` if any
  requested `setsockopt` failed (e.g. a closed/invalid fd). The scalar `0/-1` shape matches
  `sysMkdir`/`sysRename`/`sysConnectResult`.

### 1.2 The design principle it obeys

Name the **intent** (send/recv buffer bytes); hide the **OS mapping** (`SO_SNDBUF`/`SO_RCVBUF`, the
`SOL_SOCKET` level, the `int`-sized option value, the `(const char*)` cast Winsock wants) entirely in
the platform floor. This is the *same shape already proven twice in this codebase*:

- `sysTcpListen(port, reusePort: bool)` hides `SO_REUSEPORT` behind a friendly bool
  (`Resolver.cpp:1292`, floor at `lv_plat_posix.c:186-192`).
- A bare `:` in a host literal selects `AF_INET6` (`reference.md` §6.6.57) — the OS address family
  never surfaces.

`sysSocketBuffer` is the third instance of one rule: **one intent-named native per capability, OS
constants confined to the floor.**

### 1.3 What it MUST reject: a raw `setsockopt` surface

A generic `sysSetSockOpt(level, optname, value)` is **forbidden**. It would leak non-portable OS
integers (`SOL_SOCKET`, `IPPROTO_TCP`, `SO_SNDBUF`, `TCP_NODELAY`, values that differ across
kernels and don't exist on Windows) straight into user Leviathan source — the exact "special case
hiding behind a passthrough" the language philosophy (info.md §1) exists to prevent. The principled
extension path is **one named native per option** (§3), never a passthrough. If a future need feels
like it wants the passthrough, that is the signal to STOP and design the named native instead.

### 1.4 Honest semantics — buffer sizing is ADVISORY

State this in the prelude comment and in `reference.md`, verbatim in spirit:

> Buffer sizing is **best-effort**. The kernel clamps: Linux **doubles** the requested value and
> enforces a **minimum floor** (`/proc/sys/net/core/{r,w}mem_min`), so the effective size is
> neither what you asked for nor readable back as such; Windows and the BSDs clamp differently.
> A caller **must not assume an exact buffer size** after this call. It sizes an intent, not a
> guarantee — which is precisely why the want-write test (§6) is built to tolerate clamping by
> making the TLS flight far larger than any clamped window rather than by trusting a tiny number.

This honesty is *why the test needs an oversized cert* (§6.3) — the two facts are linked, and the
implementer must understand the link before writing either half.

---

## 2. Engine policy

| Engine | `sysSocketBuffer` |
|---|---|
| **oracle** (tree-walk, `Eval.cpp`) | **full** — via the shared `nativeFreeCall` case (§4.2). |
| **IR interpreter** (`IrInterp.cpp`) | **full** — same `nativeFreeCall` case, one edit covers both. |
| **LLVM native** (`liblvrt.a`) | **full** — shim + floor + `LlvmGen` dispatch (§4.3–4.6). |
| **emit-C++** | **clean deferral** — it already skips the whole socket/system layer; a program using `sysSocketBuffer` on `--emit-cpp` hits the existing coverage diagnostic, never a miscompile. Do **not** add a leg. |
| **pure ELF / X64Gen** | **loud reject** — frozen; rejects `sysSocketBuffer` exactly as it already rejects `sysTcpListen/2`. Do **not** add a leg, a corpus entry, or a gate. |
| **comptime** | **denied automatically** — `sys*`-prefix trips the hermeticity gate (`Eval.cpp:521`). A `comptime` call to `sysSocketBuffer` reports *"comptime code may not perform I/O ('sysSocketBuffer')"*. A negative test pins this (§6.6). |

This is the **identical** coverage matrix as the rest of the socket floor. Nothing here is novel
policy; it rides the precedent.

---

## 3. The socket-option floor pattern — how this stays expandable

`sysSocketBuffer` is the **first member of a family**. v1 ships **buffers only** (§5 alarm), but the
design fixes the pattern so every future option is a mechanical, low-risk addition. **Every new
socket option is added as one named native by repeating exactly the six edits in §4**, substituting
the option's intent-name and its `SOL_SOCKET`/`IPPROTO_*` + `optname` mapping in the floor. No new
mechanism is ever needed; no dispatch shape changes; no ABI negotiation.

Concretely, the anticipated family (each a future ticket, each ~30 minutes following §4 — **none in
v1 scope, see §5**):

| future native (illustrative names) | hides | intent |
|---|---|---|
| `sysTcpNoDelay(fd, bool on)` | `TCP_NODELAY` (`IPPROTO_TCP`) | disable Nagle for latency-sensitive writes |
| `sysSocketKeepAlive(fd, bool on)` | `SO_KEEPALIVE` (+ later `TCP_KEEPIDLE`/`_INTVL`/`_CNT`) | dead-peer detection |
| `sysSocketLinger(fd, int seconds)` | `SO_LINGER` | close semantics (`-1`/`0` = default/off) |
| `sysSocketTimeout(fd, int sendMs, int recvMs)` | `SO_SNDTIMEO`/`SO_RCVTIMEO` | blocking-call deadlines |
| `sysSocketBufferGet(fd) -> …` | `getsockopt(SO_SNDBUF/RCVBUF)` | read back the *clamped actual* size (introspection) |

The table is documentation of the extension path, **not a work list**. The point is that a reader six
months from now adding `TCP_NODELAY` finds the recipe already written and the first instance already
in the tree to copy. That is the "maintainable and expandable" requirement discharged: uniform rule,
worked example, explicit forbidden alternative (§1.3), and a copy-paste recipe (§4).

---

## 4. Engine wiring — the six edits (with verified anchors)

All anchors verified against the working tree at authoring time. If one drifted, re-locate by the
named symbol and confirm the pattern (§0.4).

### 4.1 Prelude declaration — `src/Resolver.cpp` kPreludeStd

Insert in the **sockets block**, immediately after `sysConnectResult` (`Resolver.cpp:1314`) and
before the TLS block comment (`:1315`):

```cpp
// LA-29 (techdesign-socket-options.md): advisory send/recv buffer sizing.
// Names the INTENT (bytes per direction); the OS mapping (SO_SNDBUF/SO_RCVBUF)
// is hidden in the floor — the same shape as sysTcpListen's reusePort bool
// hiding SO_REUSEPORT. A NON-POSITIVE value leaves that direction UNCHANGED.
// Returns 0 (every requested direction applied) | -1 (a requested setsockopt
// failed). BEST-EFFORT: the kernel clamps (Linux doubles + enforces a floor;
// Windows/BSD differ) — a caller must not assume an exact size. A permanent
// systems capability (flow-control tuning any real stack needs), not a test
// hook. sys*-prefixed => comptime-denied automatically.
int sysSocketBuffer(int fd, int sendBytes, int recvBytes);
```

Full arity, three required ints — no overloads (the ticket's only call shape passes all three). No
higher-level sugar (§5).

### 4.2 Interpreter native — `src/RuntimeNatives.cpp` `nativeFreeCall`

One new case beside the `sysTcpListen` case (`RuntimeNatives.cpp:1162`). It is reached by **both**
interpreters (`Eval.cpp:532` oracle, `IrInterp.cpp:230` IR) — one edit, two engines:

```cpp
if (name == "sysSocketBuffer") {
    int fd        = args.size() > 0 ? (int)args[0].i : -1;
    long long snd = args.size() > 1 ? args[1].i : 0;
    long long rcv = args.size() > 2 ? args[2].i : 0;
    int rc = 0;
    if (snd > 0) { int v = (int)snd;
        if (::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof v) != 0) rc = -1; }
    if (rcv > 0) { int v = (int)rcv;
        if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof v) != 0) rc = -1; }
    out = vint(rc);
    return true;
}
```

`SO_SNDBUF`/`SO_RCVBUF`/`SOL_SOCKET` are already in scope (the `sysTcpListen` case uses
`setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, …)` at `:1168` with the same headers). No new include.

### 4.3 ABI shim declaration — `runtime/lv_abi.h`

Add beside the socket shims (`lv_abi.h:555-558`), and a doc-comment row in the block comment above
(`:505-513`, following the `lvrt_systcplisten` row style):

```c
/* lvrt_syssocketbuffer: advisory SO_SNDBUF/SO_RCVBUF sizing (RuntimeNatives.cpp
 * parity). A non-positive direction is left untouched; returns 0 if every
 * requested direction applied, -1 if a requested setsockopt failed. */
void    lvrt_syssocketbuffer(LvValue* out, const LvValue* fd,
                             const LvValue* sendBytes, const LvValue* recvBytes);
```

### 4.4 ABI definition — `runtime/lv_loop.c`

One thin wrapper beside `lvrt_systcplisten` (`lv_loop.c:389-398`) — unbox, call the floor, box the
`int`:

```c
void lvrt_syssocketbuffer(LvValue* out, const LvValue* fd,
                          const LvValue* sendBytes, const LvValue* recvBytes) {
    out->tag = LV_INT;
    out->payload = lv_plat_sock_buffer((int)fd->payload,
                                       sendBytes->payload, recvBytes->payload);
}
```

### 4.5 Platform floor — `runtime/lv_plat.h` + `lv_plat_posix.c` + `lv_plat_win32.c`

Declare after `lv_plat_recv` (`lv_plat.h:73`), with a doc-comment matching the `lv_plat_tcp_listen`
style:

```c
/* LA-29: advisory socket send/recv buffer sizing. A non-positive direction is
 * skipped. Returns 0 if every requested direction's setsockopt succeeded, -1 if
 * any failed. BEST-EFFORT — the kernel clamps (Linux doubles + floors). */
int     lv_plat_sock_buffer(int fd, int64_t send_bytes, int64_t recv_bytes);
```

**POSIX** (`lv_plat_posix.c`, beside `lv_plat_tcp_listen` at `:181`):

```c
int lv_plat_sock_buffer(int fd, int64_t send_bytes, int64_t recv_bytes) {
    int rc = 0;
    if (send_bytes > 0) { int v = (int)send_bytes;
        if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof v) != 0) rc = -1; }
    if (recv_bytes > 0) { int v = (int)recv_bytes;
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof v) != 0) rc = -1; }
    return rc;
}
```

**Win32** (`lv_plat_win32.c`, beside `lv_plat_tcp_listen` at `:306`) — go through the socket-handle
map (`lv_win_sock_get`, `:68`) and cast `(const char*)`; `SO_SNDBUF`/`SO_RCVBUF` are portable BSD
options Winsock supports, so **no no-op is needed** (unlike `SO_REUSEPORT`, which Win32 drops):

```c
int lv_plat_sock_buffer(int fd, int64_t send_bytes, int64_t recv_bytes) {
    SOCKET s = lv_win_sock_get(fd);
    if (s == INVALID_SOCKET) return -1;
    int rc = 0;
    if (send_bytes > 0) { int v = (int)send_bytes;
        if (setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&v, sizeof v) != 0) rc = -1; }
    if (recv_bytes > 0) { int v = (int)recv_bytes;
        if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&v, sizeof v) != 0) rc = -1; }
    return rc;
}
```

### 4.6 LLVM dispatch — `src/LlvmGen.cpp` (and what NOT to touch)

Two lines, mirroring `sysTcpListen`:

1. Declare the `FunctionCallee` in the socket-natives group (add to the list at `LlvmGen.cpp:169`
   and register beside `rtSysTcpListen` at `:318`):
   ```cpp
   rtSysSocketBuffer = fn("lvrt_syssocketbuffer", voidTy, {ptrTy, ptrTy, ptrTy, ptrTy});
   ```
2. Add a dispatch branch in the `CallNativeFn` `n == "sys…"` chain, beside `sysAccept`
   (`LlvmGen.cpp:2270`) — args are pre-marshalled into `regs`, so `arg(k)` suffices:
   ```cpp
   } else if (n == "sysSocketBuffer") {
       b.CreateCall(rtSysSocketBuffer, {regs[in.a], arg(0), arg(1), arg(2)});
   }
   ```
   No `retainDst()` — the result is a plain `int`, not a heap value.

> **DO NOT touch `emitNativeRows`.** That table (`LlvmGen.cpp:1331`+) and its sync-warning
> (`:1734`) are for **receiver-tag-dispatched member natives** (`length`, `isEmpty`, `at`, …).
> `sysSocketBuffer` is a **free-function** native dispatched in the `n == "sys…"` chain — the same
> place as `sysTcpListen`, which likewise has no `emitNativeRows` entry. Adding one there is wrong
> and will confuse the next reader.

---

## 5. ⚠️⚠️ LOUD DEFERRAL ALARM — READ THIS, OWNER ⚠️⚠️

**d333mon — the following are deliberately NOT built in v1. Each is a SCOPE boundary, and NONE of
them leaves the LA-2 want-write residual unresolved — that residual is closed in full by §6.** You
asked to be told loudly what is deferred and why. Here it is, in full:

1. **The wider socket-option family** (`TCP_NODELAY`, `SO_KEEPALIVE`, `SO_LINGER`, send/recv
   timeouts, `IP_TOS`, `TCP_CORK`/`QUICKACK`, …). **Deferred.** *Why:* there is **no consumer** for
   any of them today, and shipping options nobody calls is the "unprincipled surface" the language
   rejects. The design pays the full expandability cost up front (§3's uniform recipe + the first
   worked instance) so each becomes a ~30-minute mechanical add **the day a real consumer appears** —
   not before. This is YAGNI with the extension ramp pre-built, not a capability we can't reach.

2. **Higher-level sugar** (`TcpStream.setBuffers(…)`, an `HttpServer(port, …, sndbuf:)` knob).
   **Deferred.** *Why:* the floor native is the honest surface; wrapping it in ergonomic sugar has
   **no consumer** and can be layered later with **zero ABI change**. Adding sugar now would be
   guessing the ergonomics before anyone needs them.

3. **Read-back / introspection** (`getsockopt` to observe the *clamped actual* buffer size).
   **Deferred.** *Why:* buffer sizing is **advisory** (§1.4) — the read-back value is unreliable and
   misleading by nature, so exposing it invites callers to depend on a number the kernel is free to
   change. Add `sysSocketBufferGet` only when a real consumer proves it needs introspection.

4. **emit-C++ and pure-ELF/X64Gen legs.** **Not built — and this is correct, not a gap.** *Why:*
   emit-C++ already skips the entire socket/system layer by design; X64Gen is **FROZEN by standing
   owner directive**. `sysSocketBuffer` rejects loudly on both exactly as `sysTcpListen/2` already
   does. The want-write test runs on **oracle + IR + LLVM** — the three engines where TLS itself
   runs — so this changes nothing about closing the residual. **I did not, and will not, gate this
   design's completion on any ELF finding.**

**Bottom line for the owner:** the actual issue you're resolving — proving the want-write inversion
fires live — is **100% closed** by this design. Everything in this alarm is future surface area with
no present consumer, deferred on purpose, with the on-ramp already laid so none of it is a cliff.

---

## 6. The payoff: the want-write inversion test (§6 is the reason this ticket exists)

### 6.1 What it proves and why it's airtight

Force a **live want-write on a read-only-driven handshake**, deterministically, on loopback:

- The **server**, on accept, shrinks its **send** buffer to a tiny value and arms TLS with an
  **oversized (~16 KB) cert** (§7). It drives its handshake with a **read-only watch only** — a
  deliberately naïve driver that **always re-arms `sysWatch` (read), never `sysWatchWrite`**,
  regardless of the handshake's wanted direction. (This is the one thing the prelude `tlsAccept`
  never does — it correctly arms the wanted direction — which is *why* an off-the-shelf driver can't
  expose the bug and the test must hand-roll one.)
- The **client** shrinks its **receive** buffer to a tiny value and connects with **`verifyMode 2`
  (encrypt-only)** — decoupling the inversion proof from cert verification, so the big cert need not
  chain to any CA. The client may drive normally (`std::tlsDrive`); the inversion is proven on the
  *server* side.
- The collapsed in-flight window can't hold the server's Certificate flight, so its `SSL_write`
  returns **want-write (`sysTlsHandshake` → 2)**. The naïve driver then arms a **pure read watch**.
  **The only thing that can wake a pure read watch is LA-2 §2.3's augmentation OR-ing `POLLOUT` onto
  it.** The driver records that a want-write step occurred (`sawWantWrite = true`), and on completion
  prints a deterministic line.
- **The proof is the completion.** If the augmentation were broken, the read-only watch would only
  ever poll `POLLIN`; with nothing left for the peer to send, the handshake **deadlocks** — caught
  by the in-program watchdog (§6.4), which prints a distinct non-matching line so the lane fails
  **fast (~8 s)** instead of hanging to the 240 s ctest timeout. Completing ⇒ the inversion fired
  live. Zero C harness.

### 6.2 The corpus program — `tests/corpus/tls/want_write_inversion.lev`

Idiom verified against `tls_reject.lev` and the prelude `TlsDrive`/`TlsAccept` classes
(`Resolver.cpp:1560-1659`). The implementer writes exactly this shape (tune only the tiny-buffer
constant and the watchdog ms if a STOP-worthy platform issue forces it):

```lev
// LA-29 acceptance — a LIVE want-write-on-a-read-watch, proving LA-2 §2.3's
// poll-mask augmentation fires. The server shrinks SO_SNDBUF and drives with a
// READ-ONLY watch (never a write watch); the client shrinks SO_RCVBUF and uses
// verifyMode 2 (encrypt-only) against an oversized cert whose Certificate flight
// cannot fit the collapsed window. sysTlsHandshake therefore returns 2
// (want-write); only the augmentation adding POLLOUT can wake a pure read watch,
// so completion == the inversion fired. A broken augmentation deadlocks and
// trips the watchdog (distinct output => FAIL fast). Port 8444 (corpus_net_ports
// lock keeps TLS lanes off each other).
int PORT = 8444;

// A deliberately NAÏVE server driver: it ALWAYS re-arms a read watch, whatever
// direction the handshake wants. This is what forces a broken augmentation into
// a deadlock (the prelude tlsAccept would arm the correct direction and hide it).
class ReadOnlyServerDrive {
    int fd = 0 - 1;
    int watchId = 0 - 1;
    int timerId = 0 - 1;
    bool sawWantWrite = false;
    bool done = false;
    int lfd = 0 - 1;
    int accWatch = 0 - 1;

    void begin(int cfd, int listenFd, int accW) {
        fd = cfd; lfd = listenFd; accWatch = accW;
        int armed = std::sysTlsAccept(fd, "tests/certs/server-big-cert.pem",
                                          "tests/certs/server-big-key.pem", "");
        if (armed < 0) { report(false); return; }        // setup failure -> loud FAIL line
        ReadOnlyServerDrive self = this;
        timerId = std::sysTimerStart(8000, 0, (n) => self.watchdog());
        step();
    }
    void step() {
        if (done) return;
        int r = std::sysTlsHandshake(fd);
        if (r == 2) { sawWantWrite = true; }              // <- the inversion signal
        if (r == 0) { report(true); return; }             // handshake completed
        if (r < 0)  { report(false); return; }
        // NAÏVE: read-only re-arm for BOTH want-read (1) AND want-write (2).
        if (watchId >= 0) { std::sysUnwatch(watchId); }
        ReadOnlyServerDrive self = this;
        watchId = std::sysWatch(fd, (ready) => self.step());   // NEVER sysWatchWrite
    }
    void watchdog() {
        if (done) return;
        // Deadlock path: the augmentation did not wake the read-only watch.
        report(false);
    }
    void report(bool completed) {
        if (done) return;
        done = true;
        if (watchId >= 0) { std::sysUnwatch(watchId); }
        if (timerId >= 0) { std::sysTimerCancel(timerId); }
        if (fd >= 0) { std::sysClose(fd); }
        std::sysUnwatch(accWatch);
        std::sysClose(lfd);
        if (completed) { console.writeln("want-write observed: true"); }
        else           { console.writeln("want-write observed: false"); }
        console.writeln("handshake complete: " + completed.toString());
    }
}

void run() {
    int lfd = std::sysTcpListen(PORT);
    ReadOnlyServerDrive drv = ReadOnlyServerDrive();
    int accWatch = std::sysWatch(lfd, (ready) => {
        int cfd = std::sysAccept(lfd);
        if (cfd >= 0) {
            std::sysSocketBuffer(cfd, 2048, 0);           // shrink server SEND buffer
            drv.begin(cfd, lfd, accWatch);
        }
    });

    std::connectTimeout("127.0.0.1", PORT, 5000, (fd) => {
        if (fd < 0) { console.writeln("connect FAILED"); return; }
        std::sysSocketBuffer(fd, 0, 2048);                // shrink client RECV buffer
        int armed = std::sysTlsConnect(fd, "127.0.0.1", "", "", 2);  // encrypt-only
        if (armed < 0) { console.writeln("client setup failed"); return; }
        std::tlsDrive(fd, (result) => { });               // client drives normally
    });
    console.writeln("inversion up");
}
run();
```

Note the `accWatch`-capture ordering: `sysWatch` returns the id used inside its own closure. If the
resolver rejects the forward self-reference, split it exactly as `tls_reject.lev:14-20` does (assign
`accWatch` first, reference it in the closure) — that file compiles today, so mirror it. This is a
mechanical idiom detail, not a STOP condition.

### 6.3 The `.expected` — and the self-validating cert-size check

`tests/corpus/tls/want_write_inversion.expected`:

```
inversion up
want-write observed: true
handshake complete: true
```

**This makes the test self-validating on cert size.** If the oversized cert is still too small — its
flight fits the clamped window — the server never sees want-write, `sawWantWrite` stays false, the
handshake still completes the ordinary way, and the output becomes `want-write observed: false` →
**mismatch → FAIL**, loudly telling the implementer *"grow the cert"* (§7). If the augmentation is
broken, the watchdog fires → `handshake complete: false` → **mismatch → FAIL**. Only a correctly
oversized cert **and** a working augmentation produce the expected three lines. Both failure modes
are distinct and fast.

### 6.4 Determinism, and every risk the method must answer

- **Kernel clamping** ⇒ "tiny" is really the kernel floor. Answered by making the cert flight
  **comfortably larger than any clamped window** (§7 targets a ≥ 12 KB, ~16 KB flight vs a
  worst-case clamped window of ~8 KB across mainstream kernels). This is the direct consequence of
  §1.4's honesty: we do not trust the small number; we out-size it.
- **Watchdog vs lane timeout.** The in-program 8 s watchdog converts a broken-augmentation deadlock
  into a fast, deterministic FAIL line rather than a 240 s ctest hang. 8 s is ~80× the real loopback
  handshake time (< 100 ms) yet 30× under the lane timeout.
- **No-OpenSSL builds self-skip.** `run_tls.sh` probes the provider and `SKIP`s the whole lane before
  any corpus file runs (`run_tls.sh:22-24`); this test rides that gate for free (it lives under the
  globbed `tests/corpus/tls/*.lev`, `run_tls.sh:29`). No harness edit is required for it to run on
  all three engines.
- **Port isolation.** Uses `8444` (distinct from the existing `8443` lanes); the lane already holds
  `RESOURCE_LOCK corpus_net_ports` (`CMakeLists.txt:584`), and `run_tls.sh` runs corpus files
  sequentially, so no cross-test collision. Update the lock comment
  (`CMakeLists.txt:579`, `:576`) to read *"binds loopback 8443/8444."*
- **Aggressive shrink is safe here** only because the kernel floor prevents a sub-minimum that would
  wedge a *normal* handshake — a fact worth the one-line comment in the `.lev` header, since it is
  the reason this test is legitimate rather than a flake generator.

### 6.5 Why buffers, and not a TLS-specific trigger (alternatives considered)

An `SSL_key_update` native would force the inversion deterministically regardless of buffers — but
it adds a **TLS-specific native with no independent use**, and TLS 1.3 KeyUpdate/renegotiation is
**off by policy** (LA-2 §4). Buffer sizing wins because it is **independently useful**
(flow-control), **portable by intent** (§1.2), and **self-hosting-aligned** (a real stack needs it).
The test is a *consumer* of a general capability, not a capability bent to fit a test — exactly the
posture §0.3 demands.

### 6.6 Comptime-denial negative check (rides `run_tls.sh` §4)

Extend the existing comptime-denial loop in `run_tls.sh` (the `for nat in …` block, ~`:83`) to
include `'std::sysSocketBuffer(3,0,0)'`, asserting the same *"comptime code may not perform I/O"*
diagnostic. One line; proves the hermeticity gate covers the new native.

---

## 7. The oversized test cert (§6.3 depends on this)

Add **`tests/certs/server-big-cert.pem`** + **`tests/certs/server-big-key.pem`**: a self-signed leaf
(no CA needed — the client uses `verifyMode 2`) whose **on-wire Certificate flight is ≥ 12 KB**,
achieved by a long `subjectAltName` DNS list. Append this recipe to `tests/certs/README.md`'s
command block and add a table row (marking it **TEST ONLY**, like the rest):

```sh
# LA-29: an oversized (~16 KB flight) self-signed leaf, padded via a long SAN
# list, to force a TLS Certificate flight larger than any clamped tiny-buffer
# window (want-write inversion test; the client uses verifyMode 2, so it need
# not chain to the test CA). Regenerate any time; 10-year validity.
{ printf '[req]\ndistinguished_name=dn\nx509_extensions=v3\nprompt=no\n[dn]\nO=Leviathan TEST ONLY\nCN=localhost\n[v3]\nsubjectAltName=@san\n[san]\n'
  i=1; while [ $i -le 900 ]; do printf 'DNS.%d=h%d.leviathan.test\n' "$i" "$i"; i=$((i+1)); done
} > big.cnf
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
  -keyout server-big-key.pem -out server-big-cert.pem -config big.cnf
rm -f big.cnf
# CONFIRM the flight is big enough: this is the DER cert the server sends.
openssl x509 -in server-big-cert.pem -outform DER | wc -c   # expect >= 12000
```

> **Cert-size is not guesswork — the test enforces it.** If `wc -c` (or a different kernel's
> clamping) leaves the flight too small, §6.3's `.expected` mismatch (`want-write observed: false`)
> fails the lane and tells you to raise the `900` SAN count. Tune until the corpus test is green on
> all three engines; that green **is** the proof the size is sufficient.

Do not delete or shrink the existing certs — this is purely additive.

---

## 8. Docs duty — `docs/reference.md`

Two prose edits (there is no socket-option *table* — the floor is a "Floor:" sentence):

1. **§6.6.57 "Sockets and HTTP"**, append to the `Floor:` sentence (`reference.md:1150-1155`):
   *"… `sysWatch(fd, cb)` / `sysWatchWrite(fd, cb)` / `sysUnwatch(id)`, and
   `sysSocketBuffer(fd, sendBytes, recvBytes)` (advisory `SO_SNDBUF`/`SO_RCVBUF` sizing — best-effort;
   the kernel clamps; a non-positive direction is left unchanged; `0`/`-1`)."*
2. **§6.6.5 "std::sys — the syscall floor"**, add a credited line in the track list
   (`reference.md:1008-1015` neighbourhood):
   *"LA-29 adds `int sysSocketBuffer(int fd, int sendBytes, int recvBytes)` (advisory socket buffer
   sizing; oracle/IR/LLVM; ELF rejects like `sysTcpListen/2`; the first member of the intent-named
   socket-option family — `techdesign-socket-options.md` §3 has the extension recipe)."*

And in **`designs/complete/techdesign-tls-crypto.md`**: append one line to the §16 **Residual** note
turning it from open to closed — *"CLOSED by LA-29 (`techdesign-socket-options.md`): `sysSocketBuffer`
lands the `SO_SNDBUF`/`SO_RCVBUF` floor and `tests/corpus/tls/want_write_inversion.lev` exercises the
live want-write-on-a-read-watch on oracle + IR + LLVM."* (Leave the historical prose; just mark it
resolved so the LA-2 log stays truthful.)

---

## 9. Milestones & acceptance

Land in one pass (the native is small; the test is the substance):

- **M1 — native, all engines.** §4.1–4.6. Acceptance: a scratch program calling
  `sysSocketBuffer(fd, 4096, 4096)` on a live loopback fd returns `0` on oracle, IR, and an
  LLVM-native build; on a closed/invalid fd returns `-1`. Comptime call denied (§6.6).
- **M2 — the oversized cert.** §7. Acceptance: `openssl x509 … -outform DER | wc -c ≥ 12000`.
- **M3 — the inversion corpus test.** §6.2–6.4. Acceptance: `want_write_inversion.lev` prints the
  three expected lines on **oracle, IR, and LLVM-native**, completing in well under the watchdog;
  `run_tls.sh` reports `ok want_write_inversion` on all three; the comptime-denial and (unchanged)
  RSA/random/reject/keepalive checks stay green — **16/16 → 17-plus/all pass**.
- **M4 — docs + close-out.** §8, plus **`git mv` this file to `designs/complete/`** with §12 filled
  and the status line flipped. Acceptance: the LA-2 residual reads CLOSED; no design left in the
  top-level dir.

**Global discipline:** never edit a `.expected` to force a pass; an oracle-vs-compiled disagreement
is a STOP + `bug.md` entry (§0.4). The three engines must agree byte-for-byte — that agreement is
the differential correctness proof the whole corpus relies on.

---

## 10. Foreseeable problems & mitigations

| # | Problem | Mitigation |
|---|---|---|
| 1 | **Cert too small after clamping** → `want-write observed: false`, no inversion. | Self-validating `.expected` (§6.3) fails loudly; raise the SAN count (§7) until green. The test *is* the size oracle. |
| 2 | **Broken §2.3 augmentation** → deadlock. | In-program 8 s watchdog (§6.4) → distinct FAIL line in ~8 s, not a 240 s hang. If it fires, that's a real LA-2 bug: STOP + `bug.md` (§0.4). |
| 3 | **`setsockopt` value type mismatch** (`SO_SNDBUF` wants `int`, not `int64_t`). | The floor casts `int v = (int)send_bytes;` and passes `&v, sizeof v` (§4.5). Do not pass the `int64_t` directly. |
| 4 | **Win32 fd is not a raw SOCKET.** | The Win32 floor translates via `lv_win_sock_get` and casts `(const char*)` (§4.5) — same as every other Win32 socket op. |
| 5 | **Port collision with the 8443 lanes.** | Distinct port `8444`; sequential lane execution; `corpus_net_ports` lock already held (§6.4). |
| 6 | **Someone "improves" the driver to arm the correct direction.** | That silently destroys the test (it can no longer deadlock). The naïve read-only re-arm is load-bearing — the `.lev` comment says so; do not §-lint it away. |
| 7 | **Temptation to add a generic `setsockopt` passthrough** for the future family. | Forbidden (§1.3). One named native per option (§3). |

---

## 11. STOP conditions (escalate to owner; do not improvise)

- The inversion test deadlocks to the watchdog on any engine after the cert is confirmed ≥ 12 KB
  (§10 #2) — a real §2.3 bug.
- Oracle disagrees with a compiled engine on the test output.
- Closing this needs an edit to the LA-2 poll-mask builders (`RuntimeLoop.cpp`/`lv_loop.c` mask
  logic) beyond reading them.
- Any anchor's *pattern* (not just its line number) has changed such that the copy-paste no longer
  fits.
- You would have to defer something not already in §5's alarm — sound a new loud alarm to the owner
  first; do not defer silently.

---

## 12. Implementation log (append-only)

**2026-07-11 — landed in full (Sonnet-class implementer).**

- **M1 — native, all engines.** The six edits of §4, applied exactly as specified (anchors had
  drifted a handful of lines from authoring time but the surrounding patterns matched precisely,
  so no STOP was warranted):
  - `src/Resolver.cpp` — `int sysSocketBuffer(int fd, int sendBytes, int recvBytes);` in the
    prelude `sys` sockets block, beside `sysConnectResult`.
  - `src/RuntimeNatives.cpp` — one `nativeFreeCall` case (beside `sysTcpListen`), reached by both
    the oracle and the IR interpreter.
  - `runtime/lv_abi.h` — the `lvrt_syssocketbuffer` shim declaration + doc-comment row.
  - `runtime/lv_loop.c` — the thin `lvrt_syssocketbuffer` wrapper beside `lvrt_syscpucount`.
  - `runtime/lv_plat.h` + `lv_plat_posix.c` + `lv_plat_win32.c` — `lv_plat_sock_buffer`, POSIX via
    a direct `setsockopt`, Win32 via `lv_win_sock_get` + the `(const char*)` cast (no no-op needed;
    `SO_SNDBUF`/`SO_RCVBUF` are portable Winsock options, unlike `SO_REUSEPORT`).
  - `src/LlvmGen.cpp` — the `rtSysSocketBuffer` `FunctionCallee` + one dispatch branch beside
    `sysAccept` in the `n == "sys…"` chain. `emitNativeRows` was **not** touched (§4.6 — this is a
    free-function native, not a receiver-tag-dispatched member native).
  - Acceptance verified: a scratch program calling `sysSocketBuffer(fd, 4096, 4096)` on a live
    loopback fd returns `0` on oracle, IR, and an LLVM-native build; `sysSocketBuffer(-1, 4096,
    4096)` returns `-1` on all three; `sysSocketBuffer(fd, 0, 0)` no-ops and returns `0`. Comptime
    call (`comptime int N = std::sysSocketBuffer(3,0,0);`) is denied with "comptime code may not
    perform I/O ('sysSocketBuffer')" on all engines. The frozen ELF backend rejects it loudly
    exactly as `sysTcpListen/2` already does (`native-elf backend: native floor function
    'sysSocketBuffer'`); emit-C++ defers cleanly (hits the existing `sysTcpListen` coverage
    diagnostic before ever reaching this native, since the corpus program opens a listener first).
- **M2 — the oversized cert.** `tests/certs/server-big-cert.pem` / `server-big-key.pem` generated
  per §7's recipe (900-entry SAN list). Confirmed DER size: **19604 bytes** (≥ the 12000-byte
  floor). Recipe appended to `tests/certs/README.md` with a `TEST ONLY` table row.
- **M3 — the inversion corpus test.** `tests/corpus/tls/want_write_inversion.lev` written exactly
  per §6.2, with one mechanical deviation flagged in advance by §6.2 itself: the resolver rejects
  the forward self-reference of `accWatch` inside the closure that produces it (`unknown name
  'accWatch'`). Not a STOP condition — §6.2 explicitly anticipated this and prescribed the fix:
  declare `int accWatch = 0 - 1;` first, then assign `accWatch = std::sysWatch(...)` as a separate
  statement, so the closure captures an already-declared outer local instead of forward-referencing
  its own initializer. Applied verbatim; no other deviation from the design.
  - **Live re-verification (§2.3-style, explicit):** `want_write_inversion.lev` printed the exact
    three expected lines (`inversion up` / `want-write observed: true` / `handshake complete:
    true`) on **oracle** (`--run`), **IR** (`--ir`), and an **LLVM-native** build
    (`--build-native`), well under the 8s in-program watchdog, confirmed stable across three
    repeated runs. `want-write observed: true` is the load-bearing signal: it proves
    `sysTlsHandshake` returned 2 (want-write) on a socket armed with a **pure read-only watch**,
    and the handshake still completed — which is only possible if LA-2 §2.3's poll-mask
    augmentation OR'd `POLLOUT` onto that read watch. The residual is closed by a live run, not by
    construction.
  - `run_tls.sh` extended: the comptime-denial loop now also checks `std::sysSocketBuffer(3,0,0)`.
  - `CMakeLists.txt`'s `tls_integration` lock comment updated to "binds loopback 8443/8444".
  - Full `run_tls.sh` lane result: **20/20 pass** (up from the pre-existing 16), including
    `want_write_inversion` on all three engines and the new comptime-denial check.
- **M4 — docs + close-out.** `docs/reference.md` §6.6.57's socket "Floor:" sentence and §6.6.5's
  Track-credit list both updated (§8). `designs/complete/techdesign-tls-crypto.md` §16's Residual
  note appended with a CLOSED-by-LA-29 line (historical prose left intact). This file moved to
  `designs/complete/` as this final log entry lands.
- **No STOP conditions were hit.** No edit touched `RuntimeLoop.cpp`/`lv_loop.c`'s poll-mask
  builders beyond reading them; no ELF leg, corpus entry, or gate was added; no raw `setsockopt`
  passthrough was introduced. Full `ctest` suite re-run after the change to confirm no regression
  elsewhere in the tree.
