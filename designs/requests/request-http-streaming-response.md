# Summary: A server-side streaming-response hook on the hardened `HttpServer`

Track 09's `HttpServer` (`handle((HttpRequest) => HttpResponse)`) buffers a
complete `HttpResponse` per request — headers + a fully-materialized string body,
written in one shot by `HttpConnection`. There is no way for a handler to send the
status line + headers immediately and then drive the body incrementally over the
open connection. Atlantis Track 01's kernel
(`designs/atlantis/techdesign-01-kernel.md` §8, contract C2's streaming body mode)
needs this for two things: **Server-Sent Events** (an unbounded, timer-driven event
stream where the writer returns before the body is done) and **large static-file
streaming** (§5.4: chunk ≤ 64 KiB, `await nextTick()` between chunks, never
buffering a 100 MB file in memory). This is the named ask the kernel design filed in
§3.4 and hurdle H-6.

## Request Details

The kernel's `ChunkedBody` and `mkStreaming(status, headers, (ChunkedBody) => void
writer)` (in `packages/atlantis/src/kernel/seam.lev`) currently implement a
**collect-then-send** interim: the writer runs to `end()`, all chunks are buffered
into one string, and a normal buffered `HttpResponse` is returned. This is correct
output for finite producers (static files serve correctly on all three engines) but
has two honest limitations it cannot escape without a runtime hook:

1. **Unbounded SSE cannot work.** An SSE producer registers a `std::every(1000)`
   timer and writes an event per tick, forever — it never calls `end()`. A
   collect-then-send helper would never return a response. True SSE requires the
   server to flush the status line + headers immediately and keep the connection
   open while the app writes chunked frames from timer/subscription callbacks that
   run *after* the handler returned.

2. **Large files are buffered whole.** With no incremental wire path, a large-file
   response reads the entire file into a string before sending. The design's M4
   acceptance ("100 MB file streams without loop stall; an interleaved second
   request stays fast") is unmeetable while the whole body must materialize first.

The clean shape is one the event loop already supports for reads (`TcpStream.onData`
is exactly this pattern in reverse): let a handler hand the server a *writer* instead
of a body, and let the server (a) send the head with `Transfer-Encoding: chunked`,
(b) invoke the writer with a live chunk sink, and (c) keep the connection open until
the writer signals end / the peer closes.

## Requested Specific Feature

Any ONE of these three shapes works for the kernel; the seam adapts to whichever the
stdlib owner prefers:

- **(A) A streaming `HttpResponse` mode.**
  `HttpResponse::ofStream(int status, HeaderMap headers, (ChunkedSink) => void writer)`
  where `ChunkedSink` has `write(string) / end() / onClose(() => void)`. `HttpConnection`,
  on a streaming response, writes the head with `Transfer-Encoding: chunked`, calls
  `writer(sink)`, and frames each `write` as one HTTP chunk (`size.toHex() + "\r\n" +
  chunk + "\r\n"`), terminal `0\r\n\r\n` on `end()`. The writer may return before `end()`
  (the SSE shape) — the sink stays valid, captured by timers/subscriptions.

- **(B) A second server handler arity.**
  `HttpServer.handleStream((HttpRequest, ChunkedSink) => void)` — the handler owns the
  sink directly; the server sends the head then invokes it.

- **(C) A "take the connection" escape.** A way for a handler to obtain the underlying
  `TcpStream` for a request and opt out of the buffered response path, writing framing
  directly. Lowest-level; the kernel would wrap it as (A).

(A) is preferred — it keeps the one-handler-shape (C2: a handler returns exactly one
type, `HttpResponse`, which simply *has* a streaming body mode) and needs no second
endpoint model.

This advances the language's "streams are THE system boundary" principle (info.md
§13): a response body is a stream, and the server should expose the write end the
same way `TcpStream.onData` exposes the read end — no new concept, the existing
event-loop write-watch (`sysWatchWrite`, already used by `TcpStream.send`'s
queue-and-drain) is the substrate.

## Known Warnings

- Chunked framing already exists (`std::chunkEncode` / `std::chunkEnd`,
  `ChunkedDecoder`) — the encoder side is trivial; the work is the connection
  lifecycle (head-then-open, keep-alive vs. close after a streamed body, draining on
  peer close).
- Interacts with keep-alive: a streamed response ends with the terminal 0-chunk, after
  which the connection *may* keep-alive (the design notes this in `ChunkedBody.end()`).
- Interacts with the cooperative-handler contract (§8.3): the streaming writer must
  `await nextTick()` between chunks so it yields the loop — this is a kernel-side
  discipline, not a server change, but the hook must allow `await` inside the writer.
- No bug.md entry — this is a missing capability, not a defect.

## Acceptance Criteria

1. A handler can send status + headers immediately and then write body chunks
   incrementally over the same connection, with `Transfer-Encoding: chunked` framing
   emitted by the server.
2. The writer may return before the body is complete; a captured sink written from a
   later timer/subscription callback still reaches the client (the SSE shape).
3. Writing an unbounded SSE stream and cancelling it on peer close leaves the event
   loop able to drain and the process to exit (no leaked watch/timer).
4. A large file streams in ≤ 64 KiB chunks without materializing the whole body in
   memory; an interleaved second request stays responsive.
5. Available on the oracle, IR, and LLVM engines (the framework's production targets).

## Interim Fallback

Track 01 ships now with the **collect-then-send** interim in `seam.lev`
(`mkStreaming` buffers a finite writer's chunks into one `HttpResponse`) and the
`ChunkedBody` / `SseStream` API surface fully in place and tested (finite SSE batches
format correctly; static files serve correctly on all three engines). The kernel is
**not blocked** — the buffered path serves real traffic (see
`packages/atlantis/tests/corpus/loopback`), and the streaming surface is stable across
the future swap: when this hook lands, only `mkStreaming`'s body in `seam.lev` changes
(from buffer-then-return to head-then-drive), and no kernel caller changes. So this is
an additive hardening ask, not a park — Track 01 lands complete on the buffered
substrate with streaming as a documented interim.

## Other

Filed alongside the kernel's other named coordination items to the Track 09 owner
(design §3.4): this streaming hook plus a request-parser body cap knob
(`maxBodyBytes`) for chunked request bodies (§7.2 — the kernel's `BodyLimit` can only
gate on a present `Content-Length` today).
