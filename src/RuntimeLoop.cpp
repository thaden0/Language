#include "RuntimeLoop.hpp"
#include <algorithm>
#include <poll.h>
#include <thread>

RuntimeLoop& RuntimeLoop::instance() {
    static RuntimeLoop loop;
    return loop;
}

void RuntimeLoop::reset() {
    timers_.clear();
    watches_.clear();
    nextId_ = 1;
}

long long RuntimeLoop::addTimer(long long delayMs, long long intervalMs, Value callback) {
    Timer t;
    t.id = nextId_++;
    t.due = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
    t.intervalMs = intervalMs;
    t.callback = std::move(callback);
    timers_.push_back(std::move(t));
    return timers_.back().id;
}

void RuntimeLoop::cancelTimer(long long id) {
    for (auto it = timers_.begin(); it != timers_.end(); ++it)
        if (it->id == id) { timers_.erase(it); return; }
}

long long RuntimeLoop::addWatch(int fd, Value callback) {
    Watch w{nextId_++, fd, false, std::move(callback)};
    watches_.push_back(std::move(w));
    return watches_.back().id;
}

long long RuntimeLoop::addWriteWatch(int fd, Value callback) {
    Watch w{nextId_++, fd, true, std::move(callback)};
    watches_.push_back(std::move(w));
    return watches_.back().id;
}

void RuntimeLoop::cancelWatch(long long id) {
    for (auto it = watches_.begin(); it != watches_.end(); ++it)
        if (it->id == id) { watches_.erase(it); return; }
}

bool RuntimeLoop::hasWork() const {
    return !timers_.empty() || !watches_.empty();
}

std::vector<LoopCallback> RuntimeLoop::nextBatch() {
    std::vector<LoopCallback> batch;
    if (timers_.empty() && watches_.empty()) return batch;

    // Compute the poll timeout from the earliest timer (or block if none).
    int timeoutMs = -1;
    if (!timers_.empty()) {
        auto earliest = timers_[0].due;
        for (const Timer& t : timers_)
            if (t.due < earliest) earliest = t.due;
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(earliest - now)
                      .count();
        timeoutMs = ms < 0 ? 0 : (int)ms;
    }

    // §2.3 #2 (TLS buffered-plaintext stall): SSL_read can drain a whole TLS
    // record into OpenSSL's buffer while pump() takes only one 4096-byte chunk —
    // the socket then polls idle with plaintext still undelivered. Scan read
    // watches for buffered plaintext BEFORE polling: any hit forces a
    // non-blocking poll (timeout 0) and marks that watch ready regardless of the
    // poll result. With zero TLS sessions runtimeTlsPending is always false, so
    // pendingIds stays empty and timeoutMs is untouched — the §2.3 bit-identity
    // guarantee holds by construction.
    std::vector<long long> pendingIds;
    if (!watches_.empty()) {
        for (const Watch& w : watches_)
            if (!w.write && runtimeTlsPending(w.fd)) pendingIds.push_back(w.id);
        if (!pendingIds.empty()) timeoutMs = 0;
    }

    // Poll the watched fds (if any) up to the timer deadline.
    if (!watches_.empty()) {
        std::vector<pollfd> pfds;
        std::vector<short>  masks;      // the direction(s) each watch waits on
        pfds.reserve(watches_.size());
        masks.reserve(watches_.size());
        for (const Watch& w : watches_) {
            // §2.3 #1 (want-direction inversion): the base direction is the
            // watch's own (read->POLLIN, write->POLLOUT). If this fd's TLS
            // session currently needs the OTHER direction to make progress — a
            // read that needs the fd writable, or a write that needs it
            // readable, mid-handshake or on a TLS 1.3 KeyUpdate — also poll for,
            // and fire on, that direction. runtimeTlsWants is 0 (mask unchanged)
            // whenever the fd has no live session, so the no-TLS mask is
            // bit-identical to the pre-TLS `w.write ? POLLOUT : POLLIN`.
            short mask = w.write ? POLLOUT : POLLIN;
            switch (runtimeTlsWants(w.fd)) {
                case 1: mask |= POLLIN;  break;   // session wants the fd readable
                case 2: mask |= POLLOUT; break;   // session wants the fd writable
                default: break;                   // 0: no session / no pending want
            }
            pfds.push_back({w.fd, mask, 0});
            masks.push_back(mask);
        }
        int n = ::poll(pfds.data(), (nfds_t)pfds.size(), timeoutMs);
        if (n > 0 || !pendingIds.empty()) {
            // Snapshot ready ids first: callbacks may add/cancel watches.
            // POLLNVAL (the fd was closed under the watch) also wakes it —
            // otherwise poll() returns instantly with an event this mask
            // ignores and the loop busy-spins forever. The watch is then
            // auto-cancelled after batching: a dead descriptor can never
            // become readable, so a POLLNVAL watch fires exactly once and
            // is dropped (the callback observes the failure via recv).
            std::vector<long long> readyIds;
            std::vector<long long> deadIds;
            for (size_t i = 0; i < pfds.size(); ++i) {
                // Fire when ANY watched direction (base or want-augmented) is
                // ready, or on the error/hang-up family: a refused connect
                // surfaces as POLLERR|POLLHUP and sysConnectResult reads the
                // verdict. The callback re-drives send/recv, which advances the
                // handshake or delivers data.
                if (pfds[i].revents & (masks[i] | POLLHUP | POLLERR | POLLNVAL))
                    readyIds.push_back(watches_[i].id);
                if (pfds[i].revents & POLLNVAL)
                    deadIds.push_back(watches_[i].id);
            }
            // §2.3 #2: buffered-plaintext watches fire even when poll saw
            // nothing (the socket is idle but OpenSSL has data). Dedup against
            // watches poll already reported ready.
            for (long long id : pendingIds) {
                bool dup = false;
                for (long long r : readyIds) if (r == id) { dup = true; break; }
                if (!dup) readyIds.push_back(id);
            }
            for (long long id : readyIds)
                for (const Watch& w : watches_)
                    if (w.id == id) { batch.push_back({w.callback, vint(w.fd)}); break; }
            for (long long id : deadIds) cancelWatch(id);
        }
    } else if (timeoutMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
    }

    // Collect due timers in (due, creation-id) order.
    auto now = std::chrono::steady_clock::now();
    std::vector<Timer*> due;
    for (Timer& t : timers_)
        if (t.due <= now) due.push_back(&t);
    std::stable_sort(due.begin(), due.end(), [](const Timer* a, const Timer* b) {
        if (a->due != b->due) return a->due < b->due;
        return a->id < b->id;
    });
    std::vector<long long> expired;
    for (Timer* t : due) {
        ++t->ticks;
        batch.push_back({t->callback, vint(t->ticks)});
        if (t->intervalMs > 0) t->due += std::chrono::milliseconds(t->intervalMs);
        else expired.push_back(t->id);
    }
    for (long long id : expired) cancelTimer(id);
    return batch;
}
