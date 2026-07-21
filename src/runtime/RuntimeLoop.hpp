#pragma once
#include "runtime/RuntimeValue.hpp"
#include <chrono>
#include <vector>

// ---------------------------------------------------------------------------
//  The event loop's work registry (§13: system events drive streams).
//
//  Shared by both engines: the registry owns the pending work — timers and
//  fd read-watches. Each ENGINE drives dispatch: it drains due batches and
//  invokes the stored callbacks with its own closure-call machinery. The
//  program-lifetime rule: after top-level completes, the engine keeps running
//  while the registry has live work (Node-style implicit loop); the program
//  exits when nothing remains.
//
//  fd watches are the socket substrate: a watch fires (with the fd as its
//  argument) whenever its descriptor is read-ready. The in-language callback
//  then does the accept/recv and pushes into a stream — the loop stays dumb
//  ("this fd is ready, run that"), all protocol logic lives in the language.
// ---------------------------------------------------------------------------

struct LoopCallback {
    Value callback;   // a closure value
    Value argument;   // tick number (timer) or fd (watch)
};

// TLS loop-integration queries (LA-2, techdesign-tls-crypto.md §2.3). Defined in
// RuntimeNatives.cpp beside the session table. With zero TLS sessions live these
// return 0/false for every fd, so nextBatch()'s poll set, timeout, and readiness
// are BIT-IDENTICAL to the pre-TLS loop — the §2.3 no-TLS guarantee, the
// regression firewall for the whole sockets/http/async corpus.
//   wants(fd):   0 none | 1 read | 2 write — the direction OpenSSL needs NEXT on
//                this fd (mid-handshake / TLS 1.3 KeyUpdate can invert it).
//   pending(fd): true when SSL has buffered plaintext ready but the socket polls
//                idle (a big TLS record already drained into OpenSSL's buffer).
int  runtimeTlsWants(int fd);
bool runtimeTlsPending(int fd);

class RuntimeLoop {
public:
    static RuntimeLoop& instance();

    void reset();     // each program run starts fresh

    // intervalMs == 0 => one-shot. Returns the timer id.
    long long addTimer(long long delayMs, long long intervalMs, Value callback);
    void cancelTimer(long long id);

    // Watch a descriptor for read-readiness; the callback receives the fd.
    long long addWatch(int fd, Value callback);
    // Watch a descriptor for WRITE-readiness (Track 08 F5: connect-progress
    // and send-drain). Same id space and cancel path as read watches; when no
    // write watch is registered the poll set is bit-identical to the
    // read-only form (problem #2's "existing programs provably unaffected").
    long long addWriteWatch(int fd, Value callback);
    void cancelWatch(long long id);

    bool hasWork() const;

    // Blocks until timers are due or a watched fd is ready, then returns the
    // callbacks to run (ready fds in fd order, then due timers in due/creation
    // order). Repeating timers re-arm; one-shots are removed. Empty only when
    // there is no work at all.
    std::vector<LoopCallback> nextBatch();

private:
    struct Timer {
        long long id;
        std::chrono::steady_clock::time_point due;
        long long intervalMs;   // 0 = one-shot
        long long ticks = 0;
        Value callback;
    };
    struct Watch {
        long long id;
        int fd;
        bool write = false;   // false: read-readiness; true: write-readiness
        Value callback;
    };
    std::vector<Timer> timers_;
    std::vector<Watch> watches_;
    long long nextId_ = 1;
};
