# Deferral Register — HTTP & Streams Maturity: Client Pooling, `InStream` Iteration, ELF DNS

**Status:** D-B (DM-2/DM-3) LANDED 2026-07-19 — see §10. D-A and D-C's in-language
resolver (DM-4/DM-5) remain correctly deferred: neither trigger has fired (D-A needs
framework Phase A, ~Aug 24; D-C's resolver needs a `sysUdp*` floor decision). This
register is archived to `designs/complete/` with D-A/DM-5 still open by design — a
register tracks conditional work, and "complete" means its actionable-now items
landed, not that every trigger has fired (SU-1 precedent: landed and archived while
explicitly handing D-B onward). Re-open/re-read this file, don't re-derive, when
either trigger fires. Historical text below (re-grounded 2026-07-17, promoted out of
deferral the same day) is kept as the record; §10 is authoritative for what actually
landed.
**Date:** 2026-07-06 (register); 2026-07-17 (re-grounded).
**Depends on:** nothing to *read*; each resolution path names its own gate.
**Source:** techdesign-09 §4#5 (client pooling deferral), techdesign-07 §1
(`InStream` iteration deferral), techdesign-08 §6/§10-M7/§11 (DNS ELF deferral);
`docs/proposal-web-framework.md` §3/§7.2/§16 (the framework-era consumer all
three feed); overview §5 roadmap line ("ELF DNS resolver over an in-language
UDP client (08 phase 2)").
**Owns:** nothing. This register modifies no track and no code. Every resolution
path below lands in a region some other track owns (Track 07 prelude protocol,
Track 08 natives/RuntimeLoop, Track 09 prelude HTTP, portable Track B platform
floor) — when a trigger fires, the work belongs to that owner, coordinated, not
trespassed (the 09 §9 rule).

---

## 0. Why this document exists

Three logged deferrals share one shape: each is blocked **not by effort but by a
subsystem that has not matured yet**, and each was correctly deferred with a
one-line note in its owning track. One-line notes rot. This register expands
each into (a) the full deferment rationale, (b) the concrete resolution path,
(c) the trigger that unblocks it, and (d) the problems the eventual implementer
will hit — so that when the trigger fires, the work starts from a design, not
from archaeology.

The three are genuinely must-wait, not procrastination:

| id | deferral | deferred where (exact ref) | prerequisite subsystem | trigger |
|---|---|---|---|---|
| **D-A** | HTTP client keep-alive / connection pooling | techdesign-09 §4#5: "Client keep-alive/pooling: **deferred** (framework-era; one-connection-per-request is honest v1)"; §8 already promises an info.md §19 "client pooling" note | framework-grade HTTP framing (Track 09 M5/M6) + a loop-lifetime answer | Track 09 M6 landed **and** framework Phase A start (overview §5: ~Aug 24) |
| **D-B** | `InStream<T>` not `IIterable<T>` in v1 | techdesign-07 §1: "`InStream<T>`: **not** made IIterable in v1 (pull-on-empty throws; a for-loop over a live stream is a foot-gun until blocking semantics exist — noted deferral, revisit with the runtime-loop work)" | stream end-of-data signal + parking/blocking pull semantics on the runtime loop | Track 07 M2 landed **and** the runtime-loop suspension work (info.md async/await note: "true stackful/CPS suspension is a later optimization"; portable-pivot P3 window) |
| **D-C** | DNS resolution on ELF | techdesign-08 §6 F6: phase 1 `sysResolve` on oracle/IR/emit-C++, "**ELF: clean diagnostic** ('resolve: not on the pure backend yet')"; §10 M7 accept: "ELF diagnostic pinned"; §11 assigns reference §7.3 the ELF-coverage note (DNS deferral) | pre-pivot: an in-language UDP resolver (needs Track 03 `Block`). Post-pivot: the ELF lane is **frozen** — the gap is permanent there by policy | LLVM lane: Track 08 M7 itself (near-term rider, §4.3). In-language resolver: Track 03 `Block` unblocked + `sysUdp*` floor |

A fourth theme runs under all three: the **portable pivot** (2026-07-05; LLVM
primary, X64Gen/ELF frozen, zero-dep premise dropped) changed the ground under
D-C and softens nothing about D-A/D-B — §4 handles the currency correction
explicitly. **Nothing in this register proposes X64Gen work. The ELF DNS gap
stays deferred behind its diagnostic, permanently.**

**Re-grounding 2026-07-17** (verified against master; supersedes conflicting status text
below — the 2026-07-06 register is kept as the historical record):

- **D-C floor / DM-1 — LANDED.** `sysResolve` exists: prelude `string? sysResolve(string
  host)` (`Resolver.cpp:1091`), native at `RuntimeNatives.cpp:1846-1850` — §3.1's
  "does not exist yet anywhere in the tree" is superseded. It rode Track 08 (system
  natives F1–F7, landed in full 2026-07-10, LLVM lane included), i.e. the §3.3 near-term
  rider fired as planned. The ELF diagnostic stays permanent per policy. The **in-language
  resolver (DM-5)** is still unbuilt (`sysUdp*` verified absent 2026-07-17), but its nested
  blocker cleared — `Block` landed with Track 03 — so it gates only on the `sysUdp*` floor
  decision, demand-scheduled.
- **D-B — both trigger halves FIRED; partially landed under another track.** Track 07
  (protocol + `Seq`) landed 2026-07-08, and the "runtime-loop suspension question" was
  settled *stronger than the register hoped*: LA-30 true stackful suspension is the
  **default** on all active engines since 2026-07-12 (`await` parks a stackful task; the
  pump is a `LANG_PUMP=1` escape hatch), with cancellation/`TaskGroup` (B2) beside it. Then
  SU-1 (`designs/complete/techdesign-stream-unsubscribe.md`, landed 2026-07-15) delivered
  most of §2.3 step 1: `StreamBuffer.closed`/`close()` with pull-on-closed throwing the
  distinct `"stream is closed"` (`Resolver.cpp:2560/2568`), producer-attached teardown,
  `InStream : IDisposable`, `signal::off`. **Deltas to reconcile at pickup:** SU-1's close
  is dispose-shaped — push-after-close **silent-drops** where step 1 wanted a loud throw —
  and `pullOrNone` was not built. Remaining D-B work = `pullOrNone`, the waiter-promise
  blocking pull (step 2 — cleaner now on true suspension than the pump-await the register
  worried over), and the prelude-only `IIterable` flip (step 3) with the §2.3 contract
  corpus. §5#6's re-entrant-pumping fear is largely dissolved by real parking; re-verify,
  don't re-argue.
- **D-A — still demand-gated, but the trigger's framework half now exists.** Atlantis
  Tracks 02–04 (routing/controllers, serialization, DI/config) landed 2026-07-13 over
  Track 09's web foundations (2026-07-09) and LA-2 TLS. Pooling lands when the serving
  path demands it — unchanged in principle, materially nearer. §5#11's scheme-in-pool-key
  rule is live advice now that https exists.
- **Currency:** §2.1's "Track 07's log shows M2 not yet landed" is stale (landed
  2026-07-08); the §1.2/§2.2 pump-era loop framing predates LA-30 — the loop-lifetime
  rule itself (exit when no live work) still holds, and SU-1's `signal::off`
  exit-by-loop-drain re-proved the idle-work analysis this register's D-A reasoning
  leans on.

---

## 1. D-A — HTTP client keep-alive / connection pooling

### 1.1 Current state (grounded)

The prelude `HttpClient` (Resolver.cpp:1008–1025) is one-connection-per-request
*by construction*: `get()` calls `sysTcpConnect`, sends a request that
**hardcodes `Connection: close`** (Resolver.cpp:1011–1012), and the paired
`HttpResponseReader` (986–1006) accumulates bytes and parses **only in
`finish()`, on `onClose`** — the response is **EOF-framed**. Track 09 §4#5
rewrites this client (redirects, timeout, Content-Length/chunked) but keeps
one-connection-per-request and explicitly defers pooling.

### 1.2 Why it must wait (the rationale in full)

1. **Framing precedes reuse — hard technical ordering.** A connection can only
   be reused if the client knows where a response *ends* without the server
   closing the socket. Today's client cannot know: it parses at EOF. Reuse
   requires the Track 09 M5 machinery (incremental parse state machines,
   Content-Length consumption, chunked decoding with terminal chunk detection).
   Pooling before M5 is not "risky" — it is *impossible to implement correctly*.
2. **The loop-lifetime rule makes idle connections structurally hostile to
   one-shot programs.** The runtime's implicit-loop lifetime rule
   (RuntimeLoop.hpp:12–14, info.md "The event loop"): after top-level completes,
   the program runs while live work remains and **exits when none does**. An
   idle pooled connection with an armed read-watch (the natural way to detect
   server-side close) *is* live work — `hasWork()` stays true and **a CLI
   program like lcurl never exits**. Same for a repeating idle-sweep `Timer`.
   Inside a *server* this is moot (the listener watch pins the loop forever
   anyway) — which is precisely why the deferral note says **framework-era**:
   the first honest home for a pool is a program that already runs forever.
3. **Reuse correctness needs the server-side keep-alive work as its test
   oracle.** Track 09 §4#4 builds server keep-alive (bounded
   `maxRequestsPerConn = 100`, idle timer, the §2.3 every-path-unwatches
   invariant, churn-asserted). The client pool's loopback corpus needs that
   server to exist first — M6's "3 requests one connection" acceptance is the
   seed fixture.
4. **One-connection-per-request is honest, not lazy.** It is slower but has no
   stale-connection races, no response desync, no retry semantics, and no
   lifetime asterisks. v1's job (per Track 09) is a *correct* client; the pool
   is a performance feature for the framework's serving path — where
   `proposal-web-framework.md` §16.1's DB driver ("Postgres wire ... over the
   existing TCP streams") and §7.2's query layer (round-trip deferred to the
   terminal op — each terminal is a wire exchange wanting a warm connection)
   will want the same pool shape. Designing the pool once, against real
   framework demand, beats designing it speculatively now. (The proposal even
   catalogs the failure mode of getting this wrong early: §3.1's FastAPI
   "holding a DB connection across a request lifecycle" footgun.)

### 1.3 Resolution path — `HttpConnectionPool` (design sketch)

In-language, prelude (or framework package once Trident hosts it), zero natives,
zero compiler changes — the 09 discipline. Shape:

```
class PooledConn {
    TcpStream stream; string host; int port;
    int requestsServed = 0; DateTime lastUsed;
}
class HttpConnectionPool {
    Map<string, Array<PooledConn>> idle;   // key: host + ":" + port (no TLS yet — scheme joins the key when TLS lands)
    int maxIdlePerHost = 4;
    Duration idleTimeout = Duration::ofSeconds(30);
    int maxRequestsPerConn = 100;          // mirror the server's bound (09 §4#4)

    TcpStream? checkout(string host, int port);   // pops idle (lazy-expiring stale ones), else None => caller dials
    void checkin(PooledConn c, bool reusable);    // reusable=false => close; true => push, evicting over maxIdle
    void closeAll();                              // program-end / drop hygiene
}
```

The load-bearing decisions:

- **No idle read-watch, no idle timer — lazy expiry only (v1).** Staleness is
  detected at `checkout` (age > idleTimeout → close+drop, try next) and by the
  retry rule below. This costs nothing in RuntimeLoop changes and — decisively —
  **cannot pin the loop**: an idle pool is inert, so one-shot programs exit
  naturally. The price: idle fds linger until next use or `closeAll`, bounded by
  `maxIdlePerHost × hosts`. Documented, acceptable. (An "idle-tier work the loop
  ignores for `hasWork()`" is the *better* long-term answer but is a RuntimeLoop
  semantic change — Track 08 / portable Track B owner territory; see STOP §7.)
- **Retry-once-on-stale.** The classic race: server closes an idle connection
  exactly as we reuse it. Rule: if a **reused** connection dies (EOF/reset)
  before *any* response byte arrives, retry the request **once** on a fresh
  connection — and only for idempotent methods (GET/HEAD/PUT/DELETE per RFC
  7231 §4.2.2); POST surfaces the error to the caller. Fresh connections never
  auto-retry (a real network error is real).
- **Check-in eligibility is strict.** Return to pool only if: response framing
  completed *exactly* (Content-Length fully consumed or chunked terminal chunk
  + trailers read), neither side sent `Connection: close`, HTTP/1.1, body not
  abandoned mid-stream by the consumer, and `requestsServed <
  maxRequestsPerConn`. Anything else → close. Leftover unread bytes on a
  checked-in connection are a desync bug, never a tolerable state (§5#1).
- **No HTTP/1.1 pipelining, ever, on the client.** Mirror the server's
  buffered-but-serial stance (09 §4#4). Real-world clients abandoned pipelining
  (head-of-line blocking, broken intermediaries); concurrency demand is met by
  *multiple pooled connections*, and the real answer is HTTP/2 multiplexing —
  a roadmap item far behind TLS. Pinned as a non-goal so nobody "helpfully"
  adds it.
- **Framing special cases ride M5's rules:** HEAD responses carry no body
  despite Content-Length; 204/304 carry no body; 1xx are interim. The pool adds
  no parsing — it *consumes* the M5 state machine's "response complete" edge.

### 1.4 Trigger

**Track 09 M6 accepted** (real HttpClient + server keep-alive corpus green)
**and** framework Phase A underway (overview §5 gate ~Aug 24). Suggested
landing: with the framework's HTTP-client consumer or the §16.1 DB driver,
whichever demands it first — realistically **Sep 2026**. Until then the deferral
note in techdesign-09 stands as written.

---

## 2. D-B — `InStream<T>` is not `IIterable<T>`

### 2.1 Current state (grounded)

Track 07 defines the protocol (`IIterator<T>`/`IIterable<T>`, `for..in`
desugar to `hasNext()`/`next()`), and its §1 explicitly excludes `InStream`.
The substrate says why (Resolver.cpp:1147–1191):

- `StreamBuffer.pull()` **throws** on empty ("stream is empty", 1166–1167) and
  throws if a subscriber has claimed the consumer end (1166).
- `InStream.hasData()` (1189) means "buffered *now*" — it cannot distinguish
  *nothing-yet* from *ended*. **The substrate has no end-of-stream signal at
  all**: `StreamBuffer` has no close/EOF state (`FileInStream.pull` fakes it
  with `"" = end of input (interim)`, Resolver.cpp:1054; `TcpStream` signals
  close *beside* the stream via `onClose`, not through it).
- `subscribe` claims the consumer end exclusively (1172–1175) — the queue is
  single-consumer by contract.

Track 07's log (2026-07-06) shows M2 not yet landed (P1/P2 probe fixes only) —
the interfaces are not in the prelude yet. This deferral is thus *ahead of* its
own prerequisite; it fires two gates deep.

### 2.2 Why it must wait (the rationale in full)

The naive bridge — `hasNext() => hasData()`, `next() => pull()` — is wrong in
every branch, which is exactly what the "foot-gun" note means:

1. **Three states collapse to two.** A live stream has data / nothing-now /
   ended (the very trichotomy info.md's sockets section celebrates `string?`
   for). `hasData() == false` conflates the last two: a `for..in` over a
   Timer-fed or socket-fed stream would **silently terminate the moment the
   buffer is momentarily empty** — near-always immediately, since the producer
   runs on loop dispatch *after* top-level. The loop wouldn't be wrong loudly;
   it would be wrong *quietly*, the worst kind.
2. **Polling is not an option.** `while (!hasData()) {}` never yields to the
   loop — the producer's callback can never run; a spin here is a livelock, not
   a wait. Correct waiting must *park on the event loop* (dispatch other work
   while blocked) — which is precisely the machinery `await` has and `pull`
   does not, yet. info.md pins the intended end-state: "await and stream-pull
   are the same suspension, two surfaces" — today's throw-on-empty `pull` is
   the interim, not the design.
3. **The suspension substrate is itself interim.** `await` today is
   pump-until-ready (info.md: "The interpreters pump-until-ready; true
   stackful/CPS suspension is a later optimization, needed for the native
   backend"). Re-entrant pumping (an `await`/park inside a loop-dispatched
   callback nests dispatch) is a live semantic question the runtime-loop work
   must settle **before** the language hands users a construct — `for..in` —
   that parks implicitly and invisibly. An explicit `await` at least *looks*
   like a suspension point; a for-loop header does not.
4. **Consumer-end exclusivity needs a ruling surfaced.** `iterator()` on a
   stream must claim the consumer end (like `subscribe`), else two iterators
   silently steal elements from each other. That is a contract decision worth
   making once, deliberately, alongside close semantics.

Deferring was correct: shipping `IIterable` on `InStream` in v1 would have
required either wrong semantics (1–2) or new runtime-loop machinery mid-track
(3) — the latter being Track 08/Track B owned surface (07 §7's "coordinate,
don't trespass" applies).

### 2.3 Resolution path (when blocking semantics land)

Three steps, deliberately ordered so the final flip is **prelude-only**:

1. **Substrate: end-of-stream.** `StreamBuffer<T>` gains `bool closed` +
   `void close()`; `push` after close throws (loud); `pull` on
   empty-and-closed throws a *distinct* end-of-stream error; producers adopt it
   (`TcpStream` closes its buffer from the same path that fires `onClose`;
   `FileInStream` retires the `"" = EOF` interim — a wart this fixes for free).
   Beside it, `T? pullOrNone()` — the honest *non*-blocking pull (None =
   nothing-now) for code that wants to poll explicitly.
2. **Blocking pull = await on a waiter promise.** When empty-and-open and a
   puller wants data, the buffer creates an internal one-shot
   `Promise` resolved by the next `push`/`close`; blocking `pull`/`hasNext`
   `await` it. This *reuses the existing await machinery* — a promise is
   already a one-shot stream; no new native, no new IR op (if the shape demands
   either, that is a STOP, §7). Whether pump-until-ready await is *sufficient*
   or true suspension must land first is exactly the "revisit with the
   runtime-loop work" question — the trigger below.
3. **The flip (prelude-only):**
   ```
   class InStream<T> : IIterable<T> {
       ...
       IIterator<T> iterator() => StreamIterator(this);   // claims the consumer end, like subscribe
   }
   class StreamIterator<T> : IIterator<T> {
       InStream<T> s;
       bool hasNext() { /* park until data-or-closed */ return s.hasData(); }
       T next() => s.pull();
   }
   ```
   Because Track 07's dispatch is static-by-type through contract C5 (Range →
   builtin fast paths → protocol) and the protocol path lowers to plain
   `CallDyn`, adding `: IIterable<T>` to a prelude class needs **zero checker,
   Eval, Lower, or backend work**. That is the payoff of doing D-B in this
   order.

Contract points to pin in the corpus: `iterator()` then `pull()` → error
(consumer claimed); `subscribe` then `iterator()` → error; `for..in` over a
Timer-fed stream of 5 ticks then `close()` → exactly 5 iterations, loop exits;
`Seq` over a stream inherits Track 07 §5#4's "terminals require finite sources"
verbatim (an unclosed stream is an infinite source — `take(n)` is the bound).

### 2.4 Trigger

**Track 07 M2/M3 landed** (protocol + `Seq` exist) **and** the runtime-loop
suspension question is settled — the natural window is the portable pivot's P3
("runtime in Leviathan", gate G3 2026-10-16, where loop semantics get
re-authored in-language anyway). Earlier only if an owner ruling declares
pump-until-ready await sufficient for parking pulls on all engines (it must
hold on `--build-native`; runtime v2's loop in `runtime/lv_loop.c` already
mirrors RuntimeLoop's registries, so the waiter-promise shape has a home
there). Realistically **Oct 2026**.

---

## 3. D-C — DNS resolution on ELF

### 3.1 Current state (grounded)

`sysResolve` does not exist yet anywhere in the tree (grep: no hits in `src/`)
— Track 08 M7 (window Jul 20 – Aug 7) will create it. The design of record
(techdesign-08 §6 F6) is two-phase:

- **Phase 1:** `std::sysResolve(string host) -> string?` via `getaddrinfo`
  (A record, first result, dotted quad) on oracle/IR/emit-C++; **ELF: clean
  diagnostic** ("resolve: not on the pure backend yet") — "the zero-dep backend
  has no libc resolver, and faking it badly would betray the engine's whole
  premise." M7 acceptance pins the diagnostic.
- **Phase 2 (roadmap; overview §5 names it):** `sysUdpSend/sysUdpRecv` floor +
  an **in-language DNS client** (parse `/etc/resolv.conf` via File, build the
  query packet via `Block`, one UDP round-trip) — "which then runs on ELF
  because it is just language code over the floor." Depends on Track 03 `Block`.

Meanwhile lcurl's floor has **no DNS at all** (curl-design.md §2 table:
`sysTcpConnect` does `inet_pton` — "no DNS, IPv4 literals only") — so this
deferral currently gates *every* engine's hostname support, not just ELF's.

### 3.2 Why it must wait — and how the portable pivot changed the answer

The original rationale was the **zero-dep boundary**: pure ELF links nothing,
`getaddrinfo` *is* libc (and transitively NSS — genuinely unfake-able via raw
syscalls; there is no `resolve(2)`). Hand-rolling a resolver *in the backend's
emitted machine code* would be enormous and off-premise; hence the honest
diagnostic plus the elegant Phase-2 plan: put UDP in the floor, write the
resolver in Leviathan, and ELF gets DNS "for free" as language code.

The **portable pivot (2026-07-05)** rewrote the ground truth:

- **X64Gen/ELF is frozen** — never extended, kept as-is. The ELF half of
  Phase 2 (emitting UDP syscalls in X64Gen) is now *forbidden work*, not future
  work. **The ELF DNS gap therefore stays deferred behind its diagnostic
  permanently.** This register exists partly to say that out loud so no one
  "finishes" Phase 2 on the frozen lane. (Precedent: Track 06's ELF
  transcendentals, same shape — deferred with diagnostic, overtaken by the
  pivot.)
- **LLVM is the primary backend** and its runtime v2 *already links libc*
  (`runtime/lv_plat_posix.c` uses BSD sockets, `connect`, `poll` — the
  zero-dep premise is dropped per the pivot memo). So the pivot **dissolves**
  the original blocker on the lane that matters: `getaddrinfo` at the platform
  floor is squarely within runtime v2's sanctioned dependency envelope.
- techdesign-08 §6's phase-1 coverage list ("oracle/IR/emit-C++") **predates
  the pivot reaching parity** (G1 functional parity met 2026-07-06) — same
  staleness class as techdesign-03's currency note about its "LLVM:
  uncovered-report policy" line. The framework proposal's §16.1 "the framework
  targets the ELF/interpreter engines" is stale the same way (targets LLVM
  now). Currency correction below.

### 3.3 Resolution path

**Near-term (rider on Track 08 M7 — the currency correction):** phase-1
`sysResolve` must cover `--build-native` too, via the platform floor, or the
*primary backend* ships with the gap the frozen one has. Shape (the one
sensitive interface; implementation belongs to M7's implementer / Track B
owner):

- `runtime/lv_plat.h`: `int lv_plat_resolve(const char* host, char* out, int outCap);`
  — 0 on success (dotted quad written to `out`), -1 on failure.
- `runtime/lv_plat_posix.c`: `getaddrinfo` with `ai_family = AF_INET`,
  `ai_socktype = SOCK_STREAM`, first result via `inet_ntop`, `freeaddrinfo` on
  every path. `lv_plat_win32.c`: identical call surface over Winsock's
  `getaddrinfo` (post-`WSAStartup`, which `lv_rt_init` already does per
  portable-backend-2 §B-M3 notes).
- Runtime v2 native binding + the interpreter native in `RuntimeNatives.cpp`
  share the `string?` contract (None = resolution failure — the C2 recipe).
  **ELF: the pinned diagnostic, unchanged.** Comptime deny-list gets
  `sysResolve` in the same commit (techdesign-08 §9#6's checklist rule —
  hermeticity breaks silently otherwise).

**Long-term (Phase 2, re-scoped):** the in-language resolver survives the pivot
with a *better* justification than ELF coverage — two, in fact:

1. **`getaddrinfo` blocks.** Real-world lookups take up to seconds; on the
   single-threaded loop that freezes everything — the same disease as the
   blocking connect (curl-design §2.9), and unfixable at the floor without
   threads (`getaddrinfo_a` is glibc-only; the language has no threads). The
   in-language UDP resolver is **async for free** — the query rides a UDP fd
   watch on the existing loop like every other socket.
2. **Portable P3 ("runtime in Leviathan", G3: no C above the plat layer).** A
   Leviathan resolver over a `sysUdp*` floor is exactly the direction of
   travel; `getaddrinfo`-in-the-floor remains as the plat-layer fallback shim.

Re-scoped Phase 2, v1 boundaries (each documented, each a deliberate cut):
check `/etc/hosts` first (it is just a File read + parse); `/etc/resolv.conf`
first nameserver only; A records only (AAAA when IPv6 URL work demands it);
5s timeout via a `Timer`; truncated (TC-bit) responses → `None` (TCP fallback
is a roadmap note); no search-domain walking. Verified against a local fixture
resolver in the test script (never live DNS in the corpus — determinism).
New corpus files are **`.lev`** (house rule 2026-07-06 — never new `.ext`, even
though the neighboring corpus predates the rule).

### 3.4 Trigger

- **LLVM-lane floor:** Track 08 M7 execution itself (its window: Jul 20 –
  Aug 7). No new gate — a scope-currency rider on an already-scheduled
  milestone.
- **In-language resolver:** `sysUdpSend/sysUdpRecv` floor (small Track 08-shaped
  follow-up) **and** Track 03 `Block` — which is currently **STOPPED** on the
  statics ruling (repo-root `/this_bug.mg` handoff, commit affa8a4). Phase 2
  therefore has a nested blocker and cannot be scheduled until that ruling
  lands. Realistic window: **Sep–Oct 2026**.
- **ELF:** no trigger. Frozen. The diagnostic is the permanent answer.

---

## 4. Dependencies & trigger milestones (the register's contract)

| id | prerequisite that must mature | concrete trigger event | then the work lands in (owner surface) |
|---|---|---|---|
| D-A | HTTP framing (Track 09 M5) + real client + server keep-alive oracle (M6); framework demand | 09 M6 accepted + framework Phase A start (~Aug 24) | prelude HTTP region (Track 09 owner) / framework package |
| D-B | protocol in prelude (07 M2/M3); stream close semantics; loop parking ruling | 07 M3 accepted + suspension ruling (natural window: portable P3, G3 2026-10-16) | `StreamBuffer`/`InStream` prelude + (if pump-await insufficient) RuntimeLoop/`lv_loop.c` (Track 08 / Track B owners) |
| D-C floor | Track 08 M7 (creates `sysResolve`) | M7 execution (Jul 20 – Aug 7 window) | `RuntimeNatives.cpp` + `lv_plat.h`/`lv_plat_posix.c`/`lv_plat_win32.c` (Track 08 + Track B owners) |
| D-C resolver | `sysUdp*` floor + Track 03 `Block` (STOPPED: statics ruling) | statics ruling lands → Track 03 resumes → `Block` accepted | prelude `namespace dns` (in-language) |
| D-C ELF | — (frozen lane) | none — permanent deferral, diagnostic pinned | — |

---

## 5. Foreseeable problems & solution strategies

| # | problem | strategy |
|---|---|---|
| 1 | **Connection-reuse desync** (D-A) — one unconsumed body byte on a checked-in connection corrupts every later response on it (request smuggling's benign cousin). | Strict check-in eligibility (§1.3): reuse only on *exact* framing completion; abandoned/aborted bodies → close, never pool. Corpus: a response with trailing junk after Content-Length must poison-pill the connection (asserted closed). |
| 2 | **Stale-idle race** (D-A) — server closes an idle connection as it is reused. | Retry-once-on-stale, idempotent methods only, only if zero response bytes arrived (§1.3). Pinned by a loopback fixture whose server closes after each response *without* `Connection: close`. |
| 3 | **Pipelining temptation** (D-A) — "the connection is idle while the body downloads, send the next request!" | Documented non-goal with the reason (HoL blocking, broken-intermediary history); concurrency = more pooled connections; the real fix is HTTP/2, behind TLS on the roadmap. Mirror of the server's buffered-but-serial stance so client and server tell one story. |
| 4 | **Idle pool pins the event loop** (D-A) — idle watches/timers are `hasWork()` = program never exits (the lcurl case). | v1: inert pool — no idle watches, no sweep timer; lazy expiry at checkout + `closeAll` (§1.3). If framework-era demand wants proactive close-detection, that is a RuntimeLoop "idle-tier work" semantic → STOP, owner ruling (§7). |
| 5 | **Pull-on-empty foot-gun ships anyway** (D-B) — someone bridges `hasNext() => hasData()` "temporarily". | The register's §2.2 is the argument to point at; the corpus pin when D-B lands (Timer-fed loop must deliver *all* ticks, which the naive bridge fails immediately) makes the wrong bridge unlandable. Until then: `InStream` simply isn't `IIterable`; the compile error names the protocol (07 §1's improved message). |
| 6 | **Re-entrant loop pumping** (D-B) — parking inside a loop-dispatched callback nests dispatch: ordering surprises, unbounded stack nesting under repetition. | This is *the* reason D-B waits for the runtime-loop work rather than landing on pump-await unexamined. The suspension ruling must state the nesting contract (allowed-documented / detected-and-thrown / true suspension). Churn corpus: park-inside-callback program, +0B, clean exit. |
| 7 | **Producer never closes** (D-B) — `for..in` parks forever; `toArray()` on a live stream never returns. | Same stance as `while(true)` (Track 07 §5#4 verbatim): documented contract — "terminals and exhaustive loops require closed/finite sources; `take(n)` is the bound." No runtime guard. |
| 8 | **Blocking `getaddrinfo` freezes the loop** (D-C floor) — seconds-long lookups stall timers, sockets, everything (curl-design §2.9's disease, second host). | v1: documented honestly (same as the blocking connect precedent). Real fix is the in-language async resolver (§3.3) — the strongest single argument for Phase 2. Do not thread, do not `getaddrinfo_a` (glibc-only). |
| 9 | **DNS on zero-dep ELF "finished" by an eager implementer** (D-C) — Phase 2's original text says ELF gets DNS via the floor, which would mean new X64Gen UDP-syscall emission. | The pivot forbids it: X64Gen frozen, no new work, ever — this register's §3.2 exists to supersede the stale reading. The diagnostic is pinned by M7 acceptance and stays. Reference §7.3's ELF coverage note records it as permanent, not pending. |
| 10 | **In-language resolver correctness edges** (D-C) — /etc/hosts precedence, TC-bit truncation, compression-pointer parsing in answers, malformed packets. | v1 scope cuts documented (§3.3); compression pointers must be handled (answers use them universally — `Block` cursor walk, bounded hops → malformed = `None`); fixture resolver in the test script exercises: hosts-hit, plain answer, compressed answer, NXDOMAIN, timeout, truncated. Malformed data is a value (`None`), never an exception (§12.6 stance). |
| 11 | **Pool key collision once TLS lands** (D-A) — http and https to the same host:port must never share a socket. | Key includes scheme from day one (§1.3 note) even while only `http` exists — one string concat now avoids a security-shaped bug later. |

---

## 6. Recommended solutions (the register's rulings-to-request, in one place)

1. **D-A:** build `HttpConnectionPool` as §1.3 sketches — inert-idle v1,
   retry-once-on-stale, strict check-in, no pipelining — as a framework-era
   deliverable after Track 09 M6. Request no RuntimeLoop changes for v1.
2. **D-B:** land close-semantics + `pullOrNone` first (small, independently
   valuable — fixes the `FileInStream` EOF wart), then the waiter-promise
   blocking pull *behind the suspension ruling*, then the prelude-only
   `IIterable` flip. Never bridge via `hasData()`.
3. **D-C:** attach the LLVM-lane floor rider to Track 08 M7 now (scope
   currency, not new scope); keep the ELF diagnostic permanently; schedule the
   in-language resolver only after the Track 03 statics ruling unblocks
   `Block`. The resolver's justification is async + P3 alignment, no longer
   ELF coverage.

---

## 7. Milestones & timeline (conditional — each keyed to its trigger, dates are planning targets)

| M | deliverable | when (trigger) | accept |
|---|---|---|---|
| DM-1 | D-C floor rider: `sysResolve` incl. `--build-native` via `lv_plat_resolve`; ELF diagnostic pinned; comptime deny-list | **LANDED** (rode Track 08, 2026-07-10) | M7's resolve probe green on oracle/IR/emit-C++ **and** llvm lane; ELF diagnostic text asserted; deny-list grep clean |
| DM-2 | D-B step 1: `StreamBuffer.close()` + `pullOrNone` + producer adoption (TcpStream/FileInStream) | **substantially LANDED via SU-1, 2026-07-15** (deltas: push-after-close silent-drops, `pullOrNone` unbuilt — see re-grounding) | `stream_close.lev`: push-after-close throws; pull-on-empty-closed distinct error; File EOF via close not `""`; churn +0B |
| DM-3 | D-B steps 2–3: waiter-promise blocking pull + `InStream : IIterable` flip (prelude-only) | **UNBLOCKED** — LA-30 true suspension default since 2026-07-12; schedulable now | `stream_iter.lev`: Timer-fed `for..in` delivers all ticks then exits at close; iterator/subscribe/pull exclusivity errors pinned; park-inside-callback churn (+0B) per §5#6; all non-frozen engines byte-identical |
| DM-4 | D-A: `HttpConnectionPool` per §1.3 | framework exists (Atlantis T02–T04 landed 2026-07-13); demand-scheduled | loopback corpus: N sequential requests over ≤ maxIdle connections; stale-reuse retry pinned (§5#2); desync poison-pill (§5#1); one-shot CLI program exits (no loop pinning); `pool_churn.lev` +0B |
| DM-5 | D-C phase 2: `sysUdp*` floor + in-language `namespace dns` resolver | `Block` landed (Track 03); gates only on the `sysUdp*` floor decision; demand-scheduled | fixture-resolver test matrix (§5#10); lcurl resolves a hostname end-to-end on oracle/IR/llvm; ELF unchanged (diagnostic) |

No dates in this table override an owning track's own timeline; where a trigger
slips, the milestone slips with it (the register is trigger-relative by design).

## 8. STOP conditions

- **Any X64Gen/ELF work for any item** — frozen lane, no exceptions; the DNS
  diagnostic and the ELF iteration/pool gaps are permanent-by-policy. If a
  design doc (including techdesign-08 §6 phase 2's pre-pivot text) appears to
  ask for it, this register supersedes: STOP, do not implement.
- **D-A wants RuntimeLoop semantics** (idle-tier work, hasWork carve-outs) —
  loop semantics belong to Track 08's owner / portable Track B; escalate with
  the concrete demand, don't improvise mid-pool.
- **D-B's blocking pull demands a new IR op, a runtime type probe, or true
  CPS on the native lane** — that is portable-pivot architecture (echoes
  Track 07 §7); STOP with the probe matrix.
- **D-C phase 2 attempted while the Track 03 statics ruling is open** —
  `Block` is blocked; escalate the `/this_bug.mg` handoff rather than
  improvising a byte-buffer substitute.
- **Any item pulled forward without its trigger** — the deferrals are
  load-bearing; landing them early against immature substrate re-creates the
  exact foot-guns the original notes avoided.

## 9. Reference-doc duty

When each lands (not before): info.md §19 gets the "client pooling" entry
Track 09 §8 already promised (DM-4 closes it); reference §6.6.57 gains the pool
contract; reference §6.4.8/§6.6.54 gain stream-iteration + close semantics
(DM-2/DM-3); reference §7.3's ELF coverage note is *re-worded* from "not yet"
to "permanent (frozen lane)" for DNS at DM-1. Flag (do not edit here):
proposal-web-framework §16.1's "framework targets the ELF/interpreter engines"
is stale post-pivot — LLVM-primary; owner of the next proposal refresh should
pick it up.

## 10. Implementation log

**2026-07-19 — DM-2 deltas + DM-3 LANDED (D-B), oracle/IR/LLVM.** Scope: only the
items whose trigger had actually fired per §0's re-grounding — DM-1 was already
landed (Track 08), DM-4 (D-A) and DM-5 (D-C resolver) remain demand-gated with no
new trigger event, so neither was touched (§8 STOP: "any item pulled forward
without its trigger" — not pulled forward here).

**DM-2 deltas (`pullOrNone`, drain-before-EOF).** `StreamBuffer<T>` (prelude,
`src/Resolver.cpp`) gains `T? pullOrNone()` (the honest non-blocking pull SU-1's
hand-off deferred — §8 of `techdesign-stream-unsubscribe.md`) and a `pullRaw()`
internal to the new iterator path. Resolved the one real semantic question the
hand-off left open: SU-1's `pull()` throws `"stream is closed"` **unconditionally**
once closed, even with items still buffered (pinned by
`tests/corpus/floor/unsub_inmem.lev`'s `IMEXP` string in
`tests/run_terminal_floor.sh`) — that is a **consumer-abandon** contract ("the
closer said done, drop everything") and is left **byte-for-byte unchanged**.
`pullOrNone()`/`pullRaw()`/the new `waitForData()` are the **producer-EOF**
siblings the hand-off predicted: they drain whatever is already queued before
recognizing end-of-stream, because a producer's natural `push(lastItem); close();`
must still deliver `lastItem` (found via a genuine crash in early testing — see
below — not a hypothetical). Two different, deliberately-diverging rules on one
`closed` flag, exactly as SU-1 §8 sanctioned ("it may split loudness/drain
behavior by role... but must keep `close()` idempotent and never-throwing").
`push`-after-close stays a **silent drop** (SU-1's behavior, unchanged) — loud
push-after-close was optional per the hand-off, not required by any DM-3
acceptance criterion, and changing it broadly risked an uncaught exception from
inside a loop-dispatched callback (Timer/signal/DOM producers) for no concrete
win; recorded as a deliberate scope cut, not an oversight.

**DM-3 (waiter-promise blocking pull + `IIterable` flip).** `InStream<T> :
IDisposable, IIterable<T>`. `iterator()`/`subscribe()` are mutually-exclusive
standing claims on the consumer end (whichever runs first wins; the loser throws
naming what claimed it). `StreamIterator<T> : IIterator<T>` holds the
`StreamBuffer` directly (not the `InStream`) so `next()` can drain via the
claim-bypassing `pullRaw()` while external `pull()`/`subscribe()` calls still see
the exclusivity error. `hasNext()` awaits `StreamBuffer.waitForData()` — a
`Promise<bool>` resolved by the next `push` (true) or `close` (false); empty
waiters array otherwise, exactly the design's §2.3 step-2 sketch, reusing the
`Channel.receive()` waiter-list pattern already in the prelude. Zero new native,
zero new IR op, as promised — `for..in`/`.iterator()` dispatch through the
existing decl-less-by-name `CallDyn` protocol path (techdesign-07 §2.1 C5),
confirmed zero checker/Eval/Lower cost by testing (`Foo : IA, IB` — two
interfaces on one class — compiles and dispatches correctly; no prior example of
this shape existed in the prelude to point to). `InStream.asSeq() => StreamSeq(this)`
additionally joins the `Seq<T>` lazy pipeline (`.map/.where/.take/...`), mirroring
`Array.asSeq()` — not in the design's literal sketch but a one-line, free
consequence of `Seq`'s existing generic-over-`IIterable` design, and needed to
make the design's own "Seq over a stream inherits finite-sources" contract point
concretely testable.

**Real bug caught and fixed during implementation (not shipped).** The first cut
of `waitForData()`/`pullRaw()` checked `closed` before `isEmpty()` (matching
`pull()`'s order) — this **crashed** a completely ordinary producer pattern:
`b.push(lastItem); b.close();` from the same callback, because `Promise.resolve()`
does not resume a native-parked `await`er synchronously (resumption is deferred),
so by the time the woken `hasNext()`→`next()` call actually ran, `closed` was
already `true` and `pullRaw()` threw even though `lastItem` was sitting right
there, un-consumed. Root-caused with a minimal repro
(`iter 1 got=1`/`iter 2 got=1`/`iter 3 got=1` expected, got an uncaught
`"stream is closed"` instead), fixed by making `pullRaw`/`pullOrNone`/
`waitForData` check items **before** closed (§2.3's "pull on empty-and-closed"
phrasing, taken literally) while leaving `pull()` itself on the old order. Pinned
by `tests/corpus/floor/stream_iter.lev`'s "drainOnClose" case (§7 below).

**CGen.cpp regression found and fixed (the one non-prelude touch).** Adding
`iterator()` to `InStream` broke emit-C++ compilation of *any* program that
merely constructs a stream and never iterates it (`tests/corpus/floor/usr1.lev`
and `unsub_inmem.lev` both started failing with a generic "construct not
covered" error instead of their expected diagnostics). Root cause: CGen's
reachability walk marks **every** member of an instantiated class reachable
unconditionally (vtable completeness for by-name dispatch), so `InStream`'s mere
construction (e.g. via `signal::on`) pulled in `iterator()`, which constructs
`StreamIterator`, whose `hasNext()` uses `await` — an `Op::Await` CGen has no
case for (this backend has no async surface at all, permanently,
`designs/suspension/techdesign-04-emitcpp-leg.md` §1) — turning provably-dead
code into a hard compile error, the *same* failure shape SU-1 hit and fixed for
`TaskGroup::close`/`sysTaskCancel`, but on a **different** reachability path
(the eager per-instantiated-class member sweep, not the by-name-CallDyn one
SU-1's `isPossibleRecv` gates). Confirmed by `git stash`-ing back to pre-D-B
`Resolver.cpp` (leak/regression absent) and by a targeted experiment (stubbing
`hasNext()` to skip `await` made the regression disappear, confirming
`Op::Await` reachability as the exact trigger) before touching `src/CGen.cpp` at
all. Fix: `iterator`/`hasNext`/`next` are, per the prelude's own existing
comments (bug.md #27/#28), *always* dispatched via decl-less by-name `CallDyn` —
never a resolved decl — so the already-existing, already-safe
`markByName`/`isPossibleRecv`/`byNameSweep` machinery is a complete, correct path
for them on its own; excluded `InStream`/`StreamSeq.iterator` and
`StreamIterator.hasNext/next` from **both** the eager per-instantiated-class
sweep and the `methodFns` by-name index, so they're reachable *only* when a
program genuinely, by-name, calls them — which correctly still gets the honest
emit-C++ rejection when a program actually tries to iterate a stream there
(verified: `stream_iter.lev` rejects with `sysTimerStart`, not a generic error).
Cost, documented: a hypothetical cpp program that calls `.iterator()`/`for..in`
on a stream gets a silent no-op miss (the pre-existing `callm`-miss fallback)
rather than a clean diagnostic — an acceptable trade given the alternative was
breaking every unrelated stream-constructing program on this backend, and
iteration was never going to work there regardless (no async surface).

**Pre-existing, unrelated bug found (not fixed — filed).** Churn-testing the
design's own §5#6 "park-inside-callback" contract via
`fuzz/task_churn/park_inside_callback.lev` surfaced a real LLVM leak
(~128B/iteration). Bisected with `git stash` to the pre-D-B tree (still leaks)
and reduced to a repro with **no stream code at all** — a plain class field
`Array<T>` mutated by `.add()`/`.skip()` across two separate method calls leaks
on `--engine llvm` regardless of streams, iteration, or `await`. Filed as
`known_bugs_2.md` **#90 [P2]** (output correct on every engine, resource-only);
the churn file carries an `XFAIL-LLVM` marker citing it, per
`fuzz/churn_leak.py`'s established convention, so it converges to a guarded PASS
automatically once #90 is fixed rather than gating this design on an unrelated
defect.

**Tests.** `tests/corpus/floor/stream_iter.lev` (new): Timer-fed `for..in`
delivers all 5 ticks then exits at `close()` (§2.3's literal contract point,
verified the *process* also exits — no loop pinning); `iterator()`-then-`pull()`
and `subscribe()`-then-`iterator()` exclusivity errors; producer-EOF drain
(push 3, close, `for..in` still yields all 3); `asSeq().take(3).toArray()` over
a live Timer stream. Wired into `tests/run_terminal_floor.sh` as a new
deterministic (non-PTY) section: `--run`/`--ir` byte-identical, `--build-native`
byte-identical modulo the `[heap]` line, `--build` (emit-C++) cleanly rejected
on `sysTimerStart` (not the generic-construct regression). `fuzz/task_churn/
park_inside_callback.lev` (new, XFAIL-LLVM per #90): real park/wake through the
task scheduler inside a loop-dispatched Timer callback, N iterations, asserts
(once #90 is fixed) that the waiter `Promise`, the Timer's closure, and the
parked task's own stack all return to the allocator. Full regression sweep run:
`terminal_floor` (all green, was 2 pre-existing-in-branch FAILs before the CGen
fix, both now pass), `corpus_core_{treewalk,native,ir,llvm}`,
`corpus_tasks_{treewalk,ir,llvm}`, `corpus_wasm` (covers the DOM event stream,
a third `StreamBuffer` producer beside Timer/signal, unaffected),
`corpus_churn_leak_tasks` — all pass (XFAIL counted as non-failing).

**Reference docs.** `docs/reference.md` §6.4.8 (iterator protocol: `InStream`
now in the `IIterable` list, "not iterable" note for `InStream` removed) and §6.6
(streams: `pullOrNone`, `iterator`/`asSeq`, exclusivity, the drain-before-EOF
rule, engine lanes) updated. `info.md` §13 landing-log entry added (mirroring
SU-1's own entry style) plus a stale-text fix (pull-on-empty no longer says
"will block once the runtime loop exists" — it exists, and does).

**Not done, correctly.** D-A (`HttpConnectionPool`) and D-C's in-language
`namespace dns` resolver (DM-5): both remain deferred behind unfired triggers
per §0/§4 of this register. Nothing in `Resolver.cpp`'s HTTP client region or
DNS-adjacent code was touched.
