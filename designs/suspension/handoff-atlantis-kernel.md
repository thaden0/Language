# LA-30 → Atlantis kernel handoff (filed at the M5 flip, 2026-07-12)

Per `designs/complete/techdesign-05-semantics-and-flip.md` §6 and the
disjoint-ownership convention: LA-30 does **not** edit
`designs/atlantis/techdesign-01-kernel.md`. This file is the handoff list for
the Atlantis track to fold in on its own schedule. Context: `await` is now a
true suspension point (stackful tasks, per-thread FIFO scheduler) on all
three active engines — normative contract in reference.md §6.6.67; the pump
is retired to a `LANG_PUMP=1` escape hatch pending deletion (M6).

1. **Kernel §8.1 — "MANDATORY `await nextTick()` between chunks" relaxes to
   latency hygiene.** A handler that computes without yielding no longer
   starves the whole loop from inside someone else's await — it delays only
   the *other tasks on its own thread*. Chunking is still good latency
   practice; it is no longer a correctness rule the kernel must legislate.

2. **Kernel §7 (~line 621) — "a wall-clock kill switch is impossible until
   true workers" now has its real dependency named.** Every task has an
   identity and a parked/runnable state — exactly the hook cancellation
   needs. The kill switch arrives with LA-30 B2
   (`designs/suspension/techdesign-06-b2-cancellation.md`, gated on its own
   green-light), not with "true workers".

3. **Kernel §8.4 — the "no coloring" claim can now cite
   reference.md §6.6.67.** The uncolored `await` survived the pump→tasks
   swap unchanged, which is the strongest evidence the claim will hold.

4. **Kernel §5.4 — `awaitTimeout` stays "stops waiting, can't stop the
   work" until B2.** Unchanged text; the footnote it deserves is that B2
   (cancellation with unwind, G12) is what gives it teeth.

5. **New, favorable fact the kernel design predates:** failure routing is
   now precise (C2 — an uncaught handler throw is program-uncaught, never
   surfacing at an unrelated pumping await) and a drained-loop await is loud
   (C3 — catchable `await: event loop drained with promise unresolved`).
   The kernel's error-path story can rely on both.

6. **Caveat to carry:** a `spawn` body referencing a bare `Promise<T>` — whether
   captured or through a top-level global — is rejected loudly at the spawn call
   (bug.md #35 fixed, reject route A: the global path used to bypass the A-1
   flatten guard and drain silently on LLVM; it now rejects byte-identically on
   every engine). Kernel code should keep to `Worker<T>` joins and `Channel<T>` —
   which it already does by design.
