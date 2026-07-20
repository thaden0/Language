# Tech Design — HTTP Streaming Responses (LA-HTTP-STREAM)

**Status:** READY FOR IMPLEMENTATION — design only; the feature is not landed.
**Date:** 2026-07-19.
**Complexity:** Sonnet. This is an in-language standard-library and test change; it adds no
native, ABI, ownership-lowering, TLS, or backend-specific code.
**Implements:**
`designs/requests/accepted/request-http-streaming-response.md`.
**Primary consumer:** Atlantis Track 01's `ChunkedBody` / `mkStreaming` seam in
`packages/atlantis/src/kernel/seam.lev`.
**Depends on:** the landed Track 09 HTTP stack, `TcpStream` queue-and-drain writes,
`sysWatchWrite`, LA-30 stackful suspension, and the existing chunk encoder.

---

## 0. Decision summary

Implement request shape **A: one `HttpResponse`, two body modes**.

```lev
HttpResponse::ofStream(
    int status,
    HeaderMap headers,
    (ChunkedSink) => void writer
)
```

`HttpConnection` sends an authoritative HTTP/1.1 head with
`Transfer-Encoding: chunked`, then invokes the writer with a live `ChunkedSink`. The
writer may return while the sink remains open; later timer/subscription callbacks may
write and eventually call `end()`. The buffered `HttpResponse(status, body)` path remains
source-compatible.

Five decisions are locked:

1. **No connection escape hatch and no second handler type.** Streaming is a body mode of
   `HttpResponse`, preserving `handle((HttpRequest) => HttpResponse)` and Atlantis C2.
2. **Chunk framing belongs to `ChunkedSink`.** Applications write payload bytes, never raw
   chunk sizes or terminators. Empty writes are no-ops; only `end()` emits the terminal
   zero chunk.
3. **A transport-drain seam is part of the work.** `TcpStream.flush() -> bool` parks until
   all queued bytes are sent (`true`) or the stream closes/fails (`false`). Every streaming
   write flushes before returning. Without this, `TcpStream.pending` can still grow to the
   whole file and `end()` followed by `close()` can truncate queued bytes.
4. **The connection is not reusable until the terminal chunk has drained.** A finite
   streamed response may keep alive, but request parsing is re-armed only after
   `0\r\n\r\n` is on the wire. Premature peer close aborts the sink and fires its
   `onClose` callback exactly once.
5. **One in-language implementation serves oracle, IR, and LLVM.** No changes under
   `Runtime*`, `runtime/`, `LlvmGen.cpp`, or the frozen X64/ELF backend are needed.

### 0.1 Requirement map

| Request criterion | Design answer |
|---|---|
| Head now, body later | `HttpResponse::ofStream` + `HttpConnection.startStream`, §4.2 |
| Writer returns before completion | connection-owned live `ChunkedSink`, §3.3/§4.2 |
| SSE disconnect cleanup | read-watch EOF routes to sink `onClose` exactly once, §4.4 |
| 64 KiB large-file streaming and responsive peer request | drain-backed `write` + Atlantis cooperative yield, §3.2/§5.2 |
| Oracle, IR, LLVM | prelude-only implementation + three-engine integration lane, §7 |

---

## 1. Research findings

### 1.1 The existing write path is asynchronous but not observable

`src/Resolver.cpp`'s `TcpStream.send` tries `sysSend` once, stores a short-write tail in
one string, and registers `sysWatchWrite`; another `send` while draining concatenates more
data:

```lev
void send(string s) {
    if (fd < 0) return;
    if (draining) { pending = pending + s; return; }
    int n = std::sysSend(fd, s);
    if (n == 0 - 2) return;
    if (n < 0) n = 0;
    if (n < s.length()) {
        pending = s.subStr(n, s.length() - n);
        draining = true;
        drainId = std::sysWatchWrite(fd, (ready) => self.drain());
    }
}
```

That is sufficient for small buffered responses, but it has two consequences for this
request:

- a producer that reads 64 KiB, calls `send`, yields one timer tick, and repeats can still
  append faster than the socket drains, materializing the response in `pending`; and
- `HttpConnection` currently calls `conn << resp.render()` and immediately closes a
  non-keep-alive connection. `TcpStream.close()` cancels the write watch and discards the
  unsent tail.

The design therefore needs completion/backpressure, not another encoder.

### 1.2 HTTP has one buffered response transition today

Current `HttpResponse.render()` always emits `Content-Length`, and `HttpConnection.feed()`
always treats handler return as a complete response:

```lev
resp.keepAlive = wantKeep;
conn << resp.render();
if (wantKeep) {
    served = served + 1;
    req = HttpRequest();
} else {
    this.closeConn();
}
```

There is no “head committed, body open” state. The request parser also remains armed while
a handler is awaiting or a response is being written; because `HttpRequest.feed()` returns
`true` forever after completion, extra bytes could re-enter the handler. The documented v1
server is buffered-but-serial and does not support pipelining, so this design adds an
explicit `responding` gate and closes on non-empty pipelined bytes rather than processing a
second request concurrently. An empty `sysRecv` would-block delivery remains a no-op; it is
not evidence of pipelining.

### 1.3 The protocol primitives already exist

The prelude already provides:

```lev
string chunkEncode(string data) =>
    data.length().toHex() + "\r\n" + data + "\r\n";
string chunkEnd() => "0\r\n\r\n";
```

`TcpStream.onData` retains a read watch and reports peer EOF through `onClose`; the same
watch can detect disconnect while a streamed response remains open. `sysWatchWrite` and
LA-30 suspension already work on oracle, IR, and LLVM. There is no missing runtime floor.

### 1.4 Atlantis has the correct caller-facing seam, but it buffers

`packages/atlantis/src/kernel/seam.lev`'s `ChunkedBody` stores one concatenated string and
`mkStreaming` runs the writer to completion before constructing a normal response. The
public kernel callers already use the target shape, so the integration is an adapter swap:

```lev
HttpResponse mkStreaming(..., (ChunkedBody) => void writer) {
    // today: writer -> ChunkedBody.buffer -> HttpResponse(status, wholeBody)
    // target: HttpResponse::ofStream(..., live std::ChunkedSink adapter)
}
```

The large-file implementation currently omits the design's `await nextTick()` because it
is collecting synchronously. That yield must be restored when the real sink lands.

### 1.5 HTTP/1.1 constraints that govern the implementation

The current standards are unambiguous:

- A sender must not combine `Content-Length` and `Transfer-Encoding`; responses to HEAD and
  responses with 1xx, 204, or 304 status have no message body regardless of framing
  headers ([RFC 9112 §§6.2–6.3](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.2)).
- Chunked coding is `size CRLF`, payload, `CRLF`, followed eventually by a zero-sized chunk
  and a final empty line. It exists specifically to carry an unknown-length stream while
  retaining connection persistence
  ([RFC 9112 §7.1](https://www.rfc-editor.org/rfc/rfc9112.html#section-7.1)).
- A chunked response is incomplete if the zero-sized terminal chunk is not received
  ([RFC 9112 §8](https://www.rfc-editor.org/rfc/rfc9112.html#section-8)).
- A persistent connection is safe to reuse only after the message's self-defined length is
  complete; both peers must consume the full message before reuse
  ([RFC 9112 §9.3](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.3)).
- Implementations should monitor persistent connections for closure so resources are
  reclaimed promptly
  ([RFC 9112 §9.5](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.5)).
- Browser SSE uses `text/event-stream`, is UTF-8, and reconnects after a lost connection;
  those representation semantics remain Atlantis's concern, while this design supplies
  the open HTTP body transport
  ([WHATWG Server-Sent Events](https://html.spec.whatwg.org/multipage/server-sent-events.html#server-sent-events)).

### 1.6 Feasibility verdict

No implementation blocker was found. The feature is standard-library state management over
landed primitives. The only necessary widening beyond the request's literal API is
`TcpStream.flush()` so “incremental” is true at the transport queue and connection close is
ordered after output.

---

## 2. Public surface

### 2.1 `TcpStream.flush`

```lev
class TcpStream {
    // Existing send/(<<)/onData/onClose/close remain.

    // Parks while this stream has an unsent tail.
    // true  = every byte queued before completion reached sysSend successfully.
    // false = local close, peer/fatal write failure, or an already-invalid stream.
    bool flush();
}
```

This is useful beyond HTTP (process stdin and future streaming clients need the same
backpressure fact), but it does not replace `send` or add function coloring. A fast-path
flush returns `true` immediately. A waiting flush uses an ordinary `Promise<bool>` and
LA-30's existing park/resume semantics.

`flush` is a queue barrier, not a durability or peer-ack guarantee: it means the bytes were
accepted by the transport floor, exactly the fact required before reusing or locally closing
the descriptor.

### 2.2 `ChunkedSink`

```lev
class ChunkedSink {
    ChunkedSink write(string data);       // parks until this framed chunk drains
    ChunkedSink (<<)(string data);        // delegates to write; chainable
    void end();                           // idempotent; drains exactly one 0-chunk
    void onClose(() => void cb);          // premature transport close/abort, once
    bool isClosed();                      // ended OR transport-aborted
}
```

Normative behavior:

- `write("")` is a no-op. It must never call `chunkEncode("")`, because that spelling is
  the terminal zero chunk.
- A non-empty `write(data)` emits exactly one `std::chunkEncode(data)` frame and does not
  return until the transport queue drains or fails.
- Calls are serialized in arrival order inside the sink, so callbacks cannot interleave
  size lines, payloads, or the terminator.
- `end()` enters the same serialization gate, emits exactly one `std::chunkEnd()`, waits
  for its drain, and then completes the HTTP response.
- `end()` after end, `write()` after end, and either call after abort are no-ops. This
  matches the project's stale-`TcpStream` and closed-`StreamBuffer` cutoff rule.
- `onClose` means premature transport termination, including a peer EOF or a server abort
  after the head was committed. It does not fire for a successful local `end()`. If it is
  registered after an abort, it runs immediately. One callback slot matches
  `TcpStream.onClose` and Atlantis's existing `ChunkedBody`.
- A writer must not create an unbounded number of concurrent calls. The sink serializes
  overlap for wire correctness, but each parked caller necessarily retains its own payload.
  The intended producer is one cooperative file loop or one bounded timer/subscription
  callback stream.

### 2.3 Streaming response constructor

```lev
class HttpResponse {
    new HttpResponse(int status, string body);  // unchanged buffered mode

    new ofStream(int status, HeaderMap headers,
                 (ChunkedSink) => void writer);

    bool isStreaming();
}
```

Internally, store the writer in a zero-or-one
`Array<(ChunkedSink) => void> streamWriters = []`, not a nullable or uninitialized function
field. Buffered construction leaves the array empty; `ofStream` adds exactly one writer;
`isStreaming()` is `streamWriters.length() == 1`. Function values in arrays are already
landed and exercised by Atlantis, and this avoids inventing a sentinel callable.

Usage:

```lev
HeaderMap h = HeaderMap();
h.set("Content-Type", "text/event-stream");
h.set("Cache-Control", "no-cache");

return HttpResponse::ofStream(200, h, (sink) => {
    Timer ticks = std::every(1000);
    ticks.subscribe((n) => sink.write("data: " + n + "\n\n"));
    sink.onClose(() => ticks.cancel());
    // Return now. The captured sink remains live.
});
```

`HttpResponse.render()` remains the buffered renderer and throws a local
`RuntimeException` if called directly on a streaming response. Only `HttpConnection` can
drive a streaming writer because only it owns request method, connection policy, peer-close
events, and keep-alive re-entry.

No `Content-Length` overload is added. A known-length response should remain buffered or be
served by a future fixed-length file/body API; this request is specifically unknown-length,
incremental output.

---

## 3. Transport design

### 3.1 Flush waiters on `TcpStream`

Add these in-language fields beside `pending` / `draining`:

```lev
Array<Promise<bool>> flushWaiters = [];
bool writeHealthy = true;
```

Conceptual implementation:

```lev
bool flush() {
    if (fd < 0 || !writeHealthy) return false;
    if (!draining) return true;
    Promise<bool> p = Promise();
    flushWaiters = flushWaiters.add(p);
    return await p;
}

void settleFlush(bool ok) {
    Array<Promise<bool>> ws = flushWaiters;
    flushWaiters = [];
    for (Promise<bool> p in ws) p.resolve(ok);
}
```

The concrete patch must route every terminal write transition through one helper:

| Transition | Existing action | New waiter result |
|---|---|---|
| immediate full send | no drain watch | later `flush()` fast-path `true` |
| short send then tail fully drains | clear pending + unwatch | settle `true` |
| `sysSend == -2` immediate or during drain | drop pending | mark unhealthy, settle `false` |
| local/peer `close()` | unwatch + close fd | settle `false` before invalidating fd; detach stored read/close callbacks |
| spurious writable wake / `-1` | keep watch | no settlement |

Snapshot-and-clear waiters before resolving them; `Promise.resolve` schedules parked tasks,
and a resumed task may call `send`/`flush` again. No callback may iterate a mutable waiter
array in place.

The ordinary `send` API stays queueing and source-compatible. Only callers that opt into
`flush` receive backpressure.

`TcpStream.close()` must also overwrite `onChunk` / `onClosed` with no-op closures after
unwatching (and clear `hasCloseCb`). Today those stored callbacks can retain the owning
`HttpConnection` after the fd is gone. Streaming makes repeated long-lived connection churn
an acceptance path, so leaving that connection → stream → callback → connection cycle is
not acceptable.

### 3.2 Bounded single-producer memory

`ChunkedSink.write` performs:

1. acquire its FIFO write turn;
2. queue one framed chunk through `TcpStream.send`;
3. call `flush()` and park if needed;
4. release the turn; or abort the sink if flush returned `false`.

For Atlantis's single file-loop producer, the next `File.read(65536)` cannot occur until
the prior frame has left `TcpStream.pending`. Application memory is therefore bounded by
one payload, one framed/copy tail, and fixed state; it cannot become the full file. Kernel
socket buffers are outside the managed heap and remain bounded by the OS.

`await nextTick()` is still mandatory after each file chunk. `flush()` may take its fast
path on a writable loopback socket, so it is backpressure, not a universal fairness yield.

### 3.3 Sink retention and teardown

`HttpConnection` retains the active sink while a streaming response is open. The writer may
also retain it in a timer/subscription closure. On successful end or abort:

- detach the sink from `HttpConnection`;
- clear its internal writer-turn waiters;
- overwrite internal emit/end/abort hooks and the user close callback with no-op closures;
- invalidate further writes; and
- on abort only, deliver the user close callback once.

This teardown is an ARC requirement, not cosmetic. The implementation must not leave the
connection → sink → closure → connection graph live. Use an explicit zero-or-one
`Array<ChunkedSink>` (or an equally backend-neutral concrete holder) for the active sink,
then clear it on teardown; do not introduce a weak-field dependency into the frozen ELF
HTTP path. The churn test in §7.6 pins this.

---

## 4. HTTP connection lifecycle

### 4.1 State machine

`HttpConnection` gains an explicit response phase:

| State | Accept request bytes? | Active output | Exit |
|---|---:|---|---|
| `reading` | yes | none | full request → `responding` |
| `responding` | no; extra bytes are unsupported pipelining → close | handler or buffered flush | buffered done → `reading`/`closed`; stream head done → `streaming` |
| `streaming` | no; extra bytes → close | live `ChunkedSink` | terminal drained → `reading`/`closed`; EOF/error → `closed` |
| `closed` | no | none | terminal |

Set `responding = true` before invoking the handler. A handler may itself park; another read
callback must not re-enter it while it is suspended. This also makes the existing
“no HTTP pipelining in v1” statement honest: non-empty bytes received before the prior
response is complete close the connection instead of being dropped or dispatched
concurrently. Ignore an empty would-block chunk before applying this gate.

### 4.2 Starting a streamed response

After the handler returns successfully:

1. Compute `wantKeep` with the existing request `Connection: close` and per-connection
   request-cap policy. Increment `served` only after the response completes.
2. Make server framing authoritative. Render the status line and caller headers while
   dropping caller-supplied `Content-Length`, `Transfer-Encoding`, and `Connection`.
   Add exactly `Transfer-Encoding: chunked` and the computed `Connection` value.
3. Create and retain `ChunkedSink`; connect its successful-end and abort hooks to this
   `HttpConnection`.
4. Queue the head and `flush()` it. This is the commit point. If it fails, abort without
   invoking the writer.
5. Mark the writer invocation active and invoke it under `try/catch`. It may park, call
   `end`, or return while still open.
6. A synchronous/awaiting writer that calls `end()` defers keep-alive re-entry until the
   writer itself returns successfully. This prevents `end(); throw ...` from reusing the
   connection before the post-commit failure is observed. A writer that returned earlier
   (the SSE shape) completes immediately when a later callback calls `end()`.

### 4.3 Buffered responses also use the drain barrier

For a normal `HttpResponse`:

```lev
conn.send(resp.render());
bool sent = conn.flush();
if (!sent) closeConn();
else if (wantKeep) rearmRequest();
else closeConn();
```

This is required infrastructure for streaming and fixes the same truncation race on large
buffered responses. It does not alter their wire bytes or handler surface.

The common buffered renderer also filters a user `Transfer-Encoding` field before adding
its authoritative `Content-Length`. The current combination of caller TE plus server CL is
invalid and can create framing ambiguity; no valid existing response depends on it.

### 4.4 Peer close and producer cancellation

`HttpConnection.start()` registers both callbacks once:

```lev
conn.onData((chunk) => self.feed(chunk));
conn.onClose(() => self.peerClosed());
```

`peerClosed()` is idempotent. During streaming it first marks the sink aborted and invokes
its `onClose` callback, then retires connection state. A typical SSE callback cancels its
repeating timer/subscription; after the listener is stopped, no watch or timer remains to
pin the event loop.

If EOF arrives while `write()` or `end()` is parked in `flush`, `TcpStream.close()` resolves
the waiter `false`; the sink is already aborted when the task resumes, so its remaining work
is a no-op. No stale fd write can reach a recycled descriptor.

`closeConn()` must distinguish normal streamed completion from abort: detach a successfully
ended sink before locally closing a `Connection: close` transport, so a normal end does not
misfire the producer's premature-close callback.

### 4.5 Successful end and keep-alive

After the terminal frame drains:

- **keep-alive:** detach/retire the sink, increment `served`, allocate a fresh
  `HttpRequest`, clear `responding`, and return to `reading` on the same read watch;
- **connection close:** detach/retire the sink, then close the `TcpStream`; and
- **peer already closed / send failed:** abort idempotently, with no re-arm.

The terminal chunk is a self-defined message boundary, so serial keep-alive is safe. Client
pipelining remains out of scope and is rejected while `responding`/`streaming`.

### 4.6 Bodyless responses and protocol version

For a HEAD request or status 1xx/204/304, RFC 9112 says the response ends after its header
section. If a handler returns `ofStream` in one of those cases:

- emit a head with neither `Content-Length` nor `Transfer-Encoding` added by the server;
- do not invoke the writer;
- complete/reuse/close through the ordinary drained-head transition.

This avoids an invalid chunk body and avoids running a body producer whose representation
cannot be sent. A successful CONNECT tunnel is not implemented by `HttpServer`; returning a
streaming 2xx CONNECT response is a pre-commit application error and follows the 500/close
path.

Chunked transfer coding is an HTTP/1.1 facility. This server does not implement an HTTP/1.0
close-delimited streaming variant or HTTP/2 framing. If `req.version` is not exactly
`HTTP/1.1`, a requested streaming response is replaced before commit with
`505 HTTP Version Not Supported`, an empty buffered body, and `Connection: close`; add the
505 reason mapping to `HttpResponse.reason()`. The writer is not invoked.

### 4.7 Exceptions

- Handler or validation throw **before** the head commit: preserve today's buffered
  `500 Internal Server Error`, `Connection: close`, and keep the accept loop alive.
- Initial writer invocation (including an `await` still on that call stack) throws **after**
  commit: a second HTTP response is impossible. Abort the connection, notify sink close
  cleanup, and keep the server/accept loop alive. The client correctly observes an
  incomplete chunked response (no zero chunk).
- A separately registered future timer/subscription callback that throws is no longer on
  the writer call stack; it follows the language's ordinary uncaught-callback semantics.
- `onClose` callback throw follows existing event-loop callback semantics: it is an uncaught
  program error. The HTTP layer must not silently swallow application cleanup failures.

---

## 5. Atlantis seam swap

### 5.1 Preserve all kernel callers

Keep the public `Atlantis::Http::ChunkedBody` type and `mkStreaming` signature. Change the
class from a collector into a thin adapter over `std::ChunkedSink`:

```lev
class ChunkedBody {
    ChunkedSink sink;
    new ChunkedBody(ChunkedSink s) { this.sink = s; }

    ChunkedBody write(string chunk) { this.sink.write(chunk); return this; }
    ChunkedBody (<<)(string chunk) { this.sink.write(chunk); return this; }
    void end() { this.sink.end(); }
    void onClose(() => void cb) { this.sink.onClose(cb); }
    bool isClosed() => this.sink.isClosed();
}
```

`mkStreaming` converts its pair array to a `HeaderMap`, captures the caller writer in an
ordinary local (the repository's lambda discipline), and returns `HttpResponse::ofStream`:

```lev
HttpResponse mkStreaming(int status, Array<Pair<string, string>> hs,
                         (ChunkedBody) => void writer) {
    HeaderMap headers = HeaderMap();
    for (Pair p in hs) headers.add(p.first, p.second);
    var w = writer;
    return HttpResponse::ofStream(status, headers, (sink) => {
        w(ChunkedBody(sink));
    });
}
```

Delete collector-only fields/methods (`buffer`, `collected`, `isEnded`). No route,
middleware, `SseStream`, or application handler call site changes. The one Atlantis corpus
case that directly constructs a disconnected `ChunkedBody()` only to inspect
`collected()` is rewritten as a real streaming loopback assertion; a live wire sink cannot
be meaningfully constructed without its owning response connection.

### 5.2 Large static files

Restore the design's cooperative loop in
`packages/atlantis/src/kernel/static_files.lev`:

```lev
string chunk = f.read(65536);
while (chunk != "" && !out.isClosed()) {
    out.write(chunk);          // transport backpressure; may park
    await nextTick();          // fairness even when flush fast-paths
    chunk = f.read(65536);
}
if (!out.isClosed()) out.end();
```

The loop never holds more than one 64 KiB file chunk. A disconnect stops further reads.
Small files, HEAD, ETag, and 304 paths remain buffered/unchanged.

### 5.3 SSE

`SseStream` remains source-identical. Its usage contract is now real rather than interim:

```lev
Timer t = std::every(1000);
t.subscribe((n) => sse.event("tick " + n));
out.onClose(() => t.cancel());
```

An SSE producer that also ends locally is responsible for cancelling its own repeating
source before/with `end`; `onClose` is the premature-disconnect hook, not a general finally.

---

## 6. File ownership and exclusions

### 6.1 May edit

- `src/Resolver.cpp`
  - `TcpStream` flush state;
  - `ChunkedSink`;
  - `HttpResponse` body mode/head rendering; and
  - `HttpConnection` lifecycle.
- `tests/corpus/http_streaming/**` — focused pure/wire programs and expected output.
- `tests/run_http_streaming.sh` — coordinated three-engine integration/stress lane.
- `CMakeLists.txt` — tests, timeout, and `RESOURCE_LOCK corpus_net_ports`.
- `docs/reference.md` and `info.md` — public HTTP/stream semantics.
- `packages/atlantis/src/kernel/seam.lev` and
  `packages/atlantis/src/kernel/static_files.lev` — adapter + cooperative file loop.
- `packages/atlantis/tests/**` / `packages/atlantis/tests/RESULTS.md` — consumer acceptance.

### 6.2 Do not touch

- `src/RuntimeNatives.cpp`, `src/RuntimeLoop.*`, `runtime/**`, `src/LlvmGen.cpp`: the
  substrate already exists and this design adds no native.
- `src/X64*`: frozen backend.
- TLS provider/session code: TLS remains an fd property under `TcpStream`; the same sink
  works unchanged on an HTTPS `HttpServer`.
- `HttpClient` streaming/pooling: its current one-request, close-delimited collector is not
  a server-response hook and remains a separate roadmap item.
- HTTP/2, WebSockets/CONNECT takeover, trailers, request-body caps, pipelining, sendfile,
  and `Block`-body APIs.

---

## 7. Verification design

### 7.1 Test harness shape

Add one consolidated `http_streaming_integration` CTest entry invoking
`tests/run_http_streaming.sh`. It runs the same loopback programs on:

1. `leviathan --run` (oracle),
2. `leviathan --ir`, and
3. `leviathan --build-native` (LLVM + `liblvrt.a`).

The test holds `RESOURCE_LOCK corpus_net_ports` and a finite watchdog. It uses temporary
ports/files under `mktemp -d`; no real network and no checked-in 100 MB fixture. Expected
wire output is generated first from the oracle and checked in through the normal corpus
discipline, never hand-edited.

Emit-C++ and frozen ELF are not acceptance engines for sockets. Their pre-existing clean
coverage behavior must remain unchanged.

### 7.2 Head/framing corpus

A raw `TcpStream` client proves:

- the head arrives before a delayed first producer tick;
- exactly one `Transfer-Encoding: chunked`, no `Content-Length`, and the computed
  `Connection` header;
- caller-supplied CL/TE/Connection values cannot create duplicates;
- payloads `"a"`, `"bc"`, and binary bytes frame as `1`, `2`, and the byte length;
- `write("")` emits nothing;
- `end()` emits exactly `0\r\n\r\n`; double end and write-after-end add no bytes.

### 7.3 Return-before-end / SSE corpus

The handler registers a timer and returns without ending. The client receives multiple
later frames on the same response. It then closes the socket; `onClose` increments a
one-shot proof counter, cancels the timer, and stops the listener. The process must exit by
loop drain before the watchdog on all three engines.

Also register `onClose` after forcing an abort and prove immediate one-time delivery. This
pins the close-before-registration race.

### 7.4 Keep-alive and error corpus

- Complete a finite streamed response, then send a normal buffered request on the same
  connection. The second handler must run only after the first terminal chunk drains.
- Request `Connection: close`; verify the terminal chunk is complete before EOF.
- Send pipelined bytes while the stream is open; verify clean connection close and no
  second handler invocation.
- Throw in the writer after the head; verify EOF without a second status line or terminal
  chunk, then prove a new connection is still served.
- Cover HEAD and 204/304 streaming responses: head only, writer not invoked, no chunk body.
- Request streaming over HTTP/1.0: one 505/close response, writer not invoked.

### 7.5 100 MB / fairness / memory proof

The shell creates a deterministic sparse 100 MB file. Use two coordinated subcases:

1. an instrumented `mkStreaming` producer exposes its completed-chunk counter through a
   second endpoint while client A initially stops reading; and
2. Atlantis `StaticFiles` serves the real file through the same adapter while a timed client
   B requests a small endpoint on a second connection.

In both subcases B must complete before A resumes.

Assertions:

- B completes inside the watchdog while A's writer is parked on transport backpressure;
- the instrumented producer's counter observed by B is well below all 1600 chunks while A
  is paused, proving the complete body was not read/queued;
- after A resumes, it receives exactly 100 MB after dechunking and the expected digest;
- every application chunk is at most 65536 bytes; and
- server/listener/client shutdown drains cleanly.

This is stronger and less platform-fragile than an absolute RSS threshold: it pins the
causal invariant that the producer cannot read the next file chunk until the prior transport
queue drains.

### 7.6 Regression and ARC gates

Run at minimum:

```sh
cmake --build build --target leviathan lvrt
ctest --test-dir build --output-on-failure -R \
  'http_streaming|corpus_http_parse|corpus_(treewalk|ir|llvm_full)|tls_integration'
```

The implementation should narrow this regex if the local build lacks optional TLS/LLVM,
but an available target must not be silently skipped.

Add a repeated open → two chunks → end and open → peer-abort churn program to the existing
heap-trailer/churn discipline. Live-at-exit must stay flat (`+0B`) across increasing N,
pinning sink/connection closure teardown.

Atlantis acceptance reruns its kernel/static/loopback corpus on oracle, IR, and LLVM, and
updates `packages/atlantis/tests/RESULTS.md` from “collect-then-send interim” to the landed
streaming result.

---

## 8. Implementation sessions

### S1 — transport drain barrier

**Scope:** `TcpStream.flush`, waiter settlement, fatal/close paths, focused socket tests.

**Gate:** immediate, short-write, peer-close, fatal-write, and multiple-waiter cases settle
exactly once on oracle/IR/LLVM; existing socket/process/TLS queue-and-drain tests remain
green.

### S2 — response mode and connection state machine

**Scope:** `ChunkedSink`, `HttpResponse::ofStream`, authoritative head rendering,
`HttpConnection` responding/streaming states, peer-close and keep-alive transitions.

**Gate:** §§7.2–7.4 green on all three engines; no runtime/backend files changed.

### S3 — Atlantis swap, stress, and documentation

**Scope:** seam adapter, static-file yield/close check, 100 MB fixture, docs and results.

**Gate:** §7.5 + Atlantis corpus + focused regression set green. Only after every gate passes
does implementation archive this design under `designs/complete/`.

---

## 9. Risk register

| # | Risk | Mitigation / proof |
|---:|---|---|
| 1 | “Streaming” still buffers the whole body in `TcpStream.pending` | `write` waits on `flush`; paused-reader counter proof, §7.5 |
| 2 | `end()` closes before pending bytes drain | terminal frame uses the same drain barrier; close/rearm only afterward |
| 3 | Head and first chunk reorder | head is flushed before writer invocation |
| 4 | Concurrent timer callbacks interleave chunk frames | sink FIFO serialization gate; terminal uses same gate |
| 5 | Peer disconnect leaves an SSE timer/watch live | read EOF → sink abort → immediate/one-shot `onClose`; drain corpus |
| 6 | End and peer EOF race double-callback/reuse | idempotent sink + connection transitions; flush false after close |
| 7 | Keep-alive parses request 2 before response 1 ends | explicit `responding`/`streaming` gate; rearm only after terminal drain |
| 8 | User adds CL beside TE (framing ambiguity/smuggling hazard) | server strips CL/TE/Connection and writes exactly one authoritative framing set |
| 9 | Empty payload accidentally terminates stream | `write("")` no-op; only `end` may call `chunkEnd` |
| 10 | Writer throws after head and server emits a second response | post-commit throw aborts transport; incomplete response test |
| 11 | HEAD/204/304 sends illegal chunk body | bodyless transition skips writer and TE/body |
| 12 | ARC cycle retains every completed connection/sink | explicit callback/holder detachment + increasing-N `+0B` churn gate |
| 13 | `flush` fast path is mistaken for fairness | Atlantis retains mandatory `await nextTick()` between 64 KiB chunks |
| 14 | HTTPS behaves differently | same `TcpStream` path over TLS-fd; focused TLS regression remains green |
| 15 | HTTP/1.0 client receives chunk syntax it cannot parse | pre-commit 505/close; no writer invocation |

---

## 10. Acceptance checklist

- [ ] `HttpResponse::ofStream` preserves the single handler/result model.
- [ ] Head is observable before delayed body output.
- [ ] Later callbacks can write through a sink retained after writer return.
- [ ] Framing is authoritative and RFC-correct, including empty writes and one terminal.
- [ ] Transport backpressure prevents a serial file producer from queueing the whole file.
- [ ] Peer close fires sink cleanup once and leaves no watch/timer leak.
- [ ] Finite streaming supports serial keep-alive only after terminal drain.
- [ ] Connection-close streaming drains the terminator before fd close.
- [ ] Post-commit writer failures abort only that connection; the server remains live.
- [ ] Atlantis streams a 100 MB file in at most 64 KiB chunks while a second request stays
      responsive.
- [ ] Oracle, IR, and LLVM integration lanes pass with identical semantic output.
- [ ] `docs/reference.md`, `info.md`, and Atlantis results describe the landed behavior.
- [ ] Focused HTTP/socket/TLS regressions and ARC churn gates pass.

---

## 11. STOP conditions

Stop and revise this design rather than improvising if any of these occurs:

1. `await` inside the ordinary-return `flush`/`write` call chain does not park and resume
   correctly on one of oracle, IR, or LLVM.
2. A `Promise<bool>` waiter array or retained writer/sink closure requires backend-specific
   lowering or a new runtime op.
3. `TcpStream.flush` cannot distinguish successful drain from close/fatal send without a
   native contract change.
4. The sink/connection graph cannot be explicitly broken with backend-neutral callback and
   holder teardown to pass the churn gate.
5. Meeting the feature requires exposing the raw `TcpStream` or adding a second server
   handler/result model.
6. Any proposed fix touches the frozen X64 backend or weakens existing TLS/socket semantics.

If none triggers, this remains a Sonnet-class, prelude-only implementation.

---

## 12. Delivery policy

The request is accepted when this design is authored, but this design remains active in
`designs/` until implementation and every acceptance gate pass. Implementation must then:

1. update `docs/reference.md` and `info.md`;
2. move this file to `designs/complete/` only after the feature is verified;
3. stage and commit the owned changes;
4. pull `origin master`, stopping for any code conflict that cannot be safely resolved; and
5. rerun the appropriate focused suite after the pull, then push to `origin master`.
