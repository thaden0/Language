/* Track B — the platform floor. The ONLY place the runtime touches an OS
 * (doc-2 §0.1 item 4). lv_runtime.c / lv_loop.c must never call an OS
 * primitive directly — everything routes through here so a new target
 * (macOS, Windows, aarch64) is a new lv_plat_*.c file, nothing else.
 *
 * The interface below is complete per doc-2 §5's table; B-M1 implements
 * only the minimal slice needed by the allocator/ARC/exceptions (map/unmap,
 * write/read, now_ns, exit) in lv_plat_posix.c. The rest are declared now
 * (per doc-2 §3 step 1: "the floor *interface* is complete in §5;
 * implement the slice you need") and defined when B-M3 lands the event
 * loop and net natives.
 */
#ifndef LV_PLAT_H
#define LV_PLAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- B-M1 slice (implemented) ---- */
void*   lv_plat_map(int64_t size);              /* anonymous RW mapping, NULL on failure */
void    lv_plat_unmap(void* base, int64_t size);
int64_t lv_plat_write(int fd, const void* buf, int64_t n);
int64_t lv_plat_read(int fd, void* buf, int64_t n);
int64_t lv_plat_now_ns(void);                   /* monotonic clock */
int64_t lv_plat_now_realtime_ms(void);          /* wall clock, epoch ms (Track 08 C6) */
void    lv_plat_exit(int code);

/* ---- B-M3 slice ----
 * open: flagBits 1=read 2=write 4=append (matches RuntimeNatives.cpp's
 * sysOpen bit convention exactly, so the sys* natives built on top are
 * byte-identical to the oracle). stat_size: -1 if the path doesn't exist.
 * tcp_connect/listen: -1 on any failure, no partial fd leaked (mirrors
 * RuntimeNatives.cpp sysTcpConnect/sysTcpListen: socket-create, bind/
 * connect, listen all closed-and--1 on the first failure). connect takes
 * IPv6 when the bare literal contains ':' (Track 08 F5.5 — brackets stay
 * in URL code). accept: -1 if none pending (matches nonblocking accept()
 * semantics); the returned client fd is already set non-blocking.
 * send: bytes written, -1 retryable (would-block/EINTR, nothing written),
 * -2 fatal (peer gone) — the split the prelude's queue-and-drain needs
 * (Track 08 F5.6; matches RuntimeNatives.cpp sysSend exactly).
 * recv: raw recv() result — 0 (peer closed), <0 (would-block/
 * error), >0 (n bytes) — the sys native layer, not this floor, maps 0/<0
 * to the None/empty-string language-level distinction (§2.7). */
int     lv_plat_open(const char* path, int bits, int mode);
void    lv_plat_close(int fd);

/* ---- terminal raw mode (designs/terminal-raw-mode.md) ----
 * The termios/console-mode struct stays inside the platform floor (the LvPollFd
 * opacity rule above); the language only ever sees "go raw" / "restore".
 * lv_plat_term_raw: save the current terminal state ONCE and switch fd to
 *   character-at-a-time, no-echo, no-signal, no-flow-control, no-output-post-
 *   processing (VMIN=1/VTIME=0). Returns 0 ok, -1 if fd is not a tty or the
 *   ioctl fails. Idempotent: a second call while already raw is a no-op success
 *   (the ORIGINAL saved state is never clobbered).
 * lv_plat_term_restore: restore the state saved by lv_plat_term_raw. No-op (0)
 *   if raw mode was never entered. The fd argument is ADVISORY — the fd captured
 *   by lv_plat_term_raw is authoritative — so the exit epilogue (which does not
 *   track the fd) can call restore with any value. A single tcsetattr, no
 *   allocation: async-signal-safe, safe from a fatal path. */
int     lv_plat_term_raw(int fd);
int     lv_plat_term_restore(int fd);

/* ---- terminal floor completion (designs/techdesign-terminal-floor.md) ----
 * lv_plat_term_size: fill *rows/*cols with fd's terminal window size. Returns
 * 0 on success, -1 on failure — not a tty, the query fails, OR a 0x0 report
 * (some multiplexers answer 0x0; the design treats it as failure, §2/§5).
 * POSIX ioctl(TIOCGWINSZ); Win32 GetConsoleScreenBufferInfo (srWindow extent).
 * lv_plat_term_israw: 1 if lv_plat_term_raw is currently active, else 0 — the
 * winsize cursor-report fallback is guarded on it (cooked mode can't read the
 * \x1b[6n reply unbuffered, §2). */
int     lv_plat_term_size(int fd, int* rows, int* cols);
int     lv_plat_term_israw(void);

/* ---- signals as streams (designs/techdesign-terminal-floor.md §3) ----
 * Ruling (signals.md, normative): language code NEVER runs in signal context.
 * A signal is a readable system stream. lv_plat_signal_open blocks each signal
 * number in `sigs` in the process mask and returns a readable fd yielding one
 * queued signo per lv_plat_signal_next call (Linux signalfd primary; portable
 * self-pipe fallback whose handler body is exactly write(pipe_wr,&signo,1)).
 * SIGKILL/SIGSTOP in the set -> -1 (they cannot be caught). Returns the fd
 * (>=0) or -1 on failure. lv_plat_signal_next: the next pending signo, or -1
 * when none is queued right now (the fd is non-blocking, so a caller drains in
 * a loop and a SIGWINCH storm coalesces). lv_plat_signal_close: close the fd
 * and unblock exactly the signals it carried (the prelude keeps one fd per
 * signal number, so the sets are disjoint). Windows v1: always-fail stub
 * (POSIX-first, the spawn/Channel Windows-reject precedent). */
int     lv_plat_signal_open(const int* sigs, int n);
int     lv_plat_signal_next(int fd);
void    lv_plat_signal_close(int fd);

int64_t lv_plat_stat_size(const char* path);
int64_t lv_plat_stat_mtime(const char* path);   /* epoch seconds; -1 if absent */
int     lv_plat_stat_isdir(const char* path);   /* 1 dir / 0 not-dir / -1 absent */
int     lv_plat_mkdir(const char* path);         /* 0 ok / -1 fail (mode 0755) */
int     lv_plat_tcp_connect(const char* ip, int port);
int     lv_plat_tcp_listen(const char* ip, int port, int backlog, int reuse_port);
int     lv_plat_cpu_count(void);
int     lv_plat_accept(int fd);
int64_t lv_plat_send(int fd, const void* buf, int64_t n);
int64_t lv_plat_recv(int fd, void* buf, int64_t n);
/* LA-29: advisory socket send/recv buffer sizing. A non-positive direction is
 * skipped. Returns 0 if every requested direction's setsockopt succeeded, -1 if
 * any failed. BEST-EFFORT — the kernel clamps (Linux doubles + floors). */
int     lv_plat_sock_buffer(int fd, int64_t send_bytes, int64_t recv_bytes);
void    lv_plat_set_nonblock(int fd);
/* LA-2 §8: fill `buf` with n crypto-grade random bytes from the kernel CSPRNG
 * (getrandom / BCryptGenRandom / /dev/urandom fallback). Returns n on success,
 * -1 on failure. The compiled twin of the oracle's sysRandom (getrandom). */
int64_t lv_plat_random(void* buf, int64_t n);
/* Track 08 F5: the connect-timeout floor. connect_nb returns the fd even
 * mid-handshake (EINPROGRESS/WSAEWOULDBLOCK); -1 only on an immediate
 * failure (bad literal, no fds). Completion is observed by polling the fd
 * for LV_POLLOUT and reading connect_result: 0 = connected, else the
 * errno-shaped failure (-1 if the query itself failed). Win32 note (B-H8):
 * pre-2004 WSAPoll misses failed async connects — the in-language
 * connectTimeout then settles at its deadline instead of instantly, a
 * graceful degradation, never a hang (the timer always fires). */
int     lv_plat_tcp_connect_nb(const char* ip, int port);
int     lv_plat_connect_result(int fd);

/* --- process floor (G-LANG-2, techdesign-spawn-llvm.md) --------------------
 * lv_plat_spawn: argv is NUL-terminated (argv[0] = path, no PATH search).
 *   On success returns pid > 0 and writes the parent pipe ends to fds[0..2]
 *   (stdin-write, stdout-read, stderr-read), all O_CLOEXEC + O_NONBLOCK.
 *   Returns -1 on pipe/fork failure (exec failure is the child's 127 instead).
 * lv_plat_pidfd_open: pollable fd, read-ready when pid exits; -1 unavailable.
 * lv_plat_reap: -1 still running / not ours; 0..255 exited; 128+sig signaled.
 *   Never blocks.
 * lv_plat_kill: 0/-1. Callers refuse pid <= 0 above this line; the floor
 *   refuses it again (broadcast forms never reach kill(2)).                  */
int     lv_plat_spawn(const char* path, char* const argv[], int fds[3]);
int     lv_plat_pidfd_open(int pid);
int     lv_plat_reap(int pid);
int     lv_plat_kill(int pid, int sig);

/* poll: a platform-opaque record so a future Win32 floor can back it with
 * WSAPOLLFD (whose SOCKET fd type differs in width from POSIX's int)
 * without touching any caller. events/revents use LV_POLLIN/LV_POLLOUT,
 * not the raw POLLIN/POLLOUT bit values (B-H8: WSAPoll's bits don't match
 * poll()'s 1:1 either, so a caller-visible translation layer is required
 * regardless of platform). revents ready-for-read also covers HUP/ERR
 * (mirrors src/RuntimeLoop.cpp's `POLLIN|POLLHUP|POLLERR` readiness
 * check — a half-closed peer must still wake the watch); ready-for-WRITE
 * covers HUP/ERR the same way (a refused non-blocking connect surfaces as
 * ERR/HUP and must wake the write watch so sysConnectResult can rule). LV_POLLNVAL is
 * reported when the fd was closed under the poll (POSIX POLLNVAL; the
 * Win32 floor maps WSAPoll's invalid-socket report): the loop must treat
 * it as a wake AND retire the watch — ignoring it makes poll() return
 * instantly with an event nobody consumes, a silent 100%-CPU busy spin
 * (found 2026-07-05 maintenance pass; RuntimeLoop.cpp had the identical
 * gap, fixed the same day; X64Gen's frozen 0x19 mask still has it). */
enum { LV_POLLIN = 1, LV_POLLOUT = 2, LV_POLLNVAL = 4 };
typedef struct LvPollFd { int fd; int16_t events; int16_t revents; } LvPollFd;
int     lv_plat_poll(LvPollFd* fds, int nfds, int timeout_ms);   /* -1 on error */

#ifdef __cplusplus
}
#endif

#endif /* LV_PLAT_H */
