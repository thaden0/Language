/* Track B — POSIX platform floor (Linux/macOS). B-M1 implements the
 * minimal slice the allocator/ARC/exceptions need; B-M3 fills in the rest
 * of lv_plat.h's declared interface (files/sockets/poll) alongside the
 * event loop.
 */
#include "lv_plat.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/signalfd.h>
#endif

void* lv_plat_map(int64_t size) {
    void* p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

void lv_plat_unmap(void* base, int64_t size) {
    munmap(base, (size_t)size);
}

int64_t lv_plat_write(int fd, const void* buf, int64_t n) {
    return (int64_t)write(fd, buf, (size_t)n);
}

int64_t lv_plat_read(int fd, void* buf, int64_t n) {
    return (int64_t)read(fd, buf, (size_t)n);
}

int64_t lv_plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int64_t lv_plat_now_realtime_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

void lv_plat_exit(int code) {
    _exit(code);
}

/* ---- B-M3 slice: files, sockets, poll. Behavior extracted from
 * src/RuntimeNatives.cpp (the tree-walk/IR oracle) so the sys* natives
 * built on this floor are byte-identical to it — never invented. ---- */

/* `bits` is the portable convention (1=read 2=write 4=append, matching
 * RuntimeNatives.cpp's sysOpen) — the O_* translation happens here so no
 * OS-specific flag value ever appears outside a lv_plat_*.c file. */
int lv_plat_open(const char* path, int bits, int mode) {
    int rd = bits & 1, wr = bits & 2, ap = bits & 4;
    int flags;
    if (rd && wr) flags = O_RDWR | O_CREAT;
    else if (wr) flags = O_WRONLY | O_CREAT | O_TRUNC;
    else flags = O_RDONLY;
    if (ap) flags = (flags & ~O_TRUNC) | O_APPEND | O_CREAT | (rd ? O_RDWR : O_WRONLY);
    return open(path, flags, mode);
}

void lv_plat_close(int fd) {
    close(fd);
}

/* ---- terminal raw mode (designs/terminal-raw-mode.md §3.1). Single saved slot:
 * a process drives one controlling terminal (multi-fd raw is out of scope,
 * documented). g_raw_fd is authoritative for restore, so the exit epilogue can
 * pass any fd. All state stays in this POSIX floor file — no termios escapes. */
static struct termios g_saved_termios;
static int g_raw_active = 0;
static int g_raw_fd = -1;

/* Raw-mode safety handlers (designs/techdesign-terminal-floor.md §3): a TUI
 * killed externally (SIGTERM/HUP/INT/QUIT) must put the terminal back before it
 * dies, or it orphans a raw shell. The ENTIRE handler body is restore + re-raise
 * default — async-signal-safe (a single tcsetattr, then raise). SA_RESETHAND
 * resets us to SIG_DFL before entry, so raise() dies with the correct status.
 * Disjoint from lv_task.c's SIGSEGV crash reporter (issue #2) — these four
 * never touch SEGV. Composes with lvrt_term_shutdown's idempotent restore
 * (issue #4). If a program instead subscribes via signal::on(TERM), that fd
 * BLOCKS the signal and the handler simply never fires — the program owns it. */
static void lv_term_safety_handler(int sig) {
    lv_plat_term_restore(0);           /* single tcsetattr — async-signal-safe */
    raise(sig);                        /* SA_RESETHAND already reset to SIG_DFL */
}
static void lv_term_install_safety(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = lv_term_safety_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;        /* fire once, then default disposition */
    const int sigs[4] = { SIGTERM, SIGHUP, SIGINT, SIGQUIT };
    for (int i = 0; i < 4; i++) sigaction(sigs[i], &sa, NULL);
}

int lv_plat_term_raw(int fd) {
    if (g_raw_active) return 0;                    /* idempotent; keep the first save */
    if (tcgetattr(fd, &g_saved_termios) != 0) return -1;   /* ENOTTY if not a tty */
    struct termios r = g_saved_termios;
    r.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);   /* IXON: ^S/^Q flow off */
    r.c_oflag &= ~(OPOST);                                    /* no \n -> \r\n xlate */
    r.c_cflag |= (CS8);
    r.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);            /* no echo/line/^C^Z */
    r.c_cc[VMIN] = 1; r.c_cc[VTIME] = 0;                      /* block for >= 1 byte */
    if (tcsetattr(fd, TCSAFLUSH, &r) != 0) return -1;
    g_raw_active = 1; g_raw_fd = fd;
    lv_term_install_safety();          /* restore-on-external-kill (§3) */
    return 0;
}

int lv_plat_term_restore(int fd) {
    (void)fd;                                      /* advisory; g_raw_fd is authoritative */
    if (!g_raw_active) return 0;
    int rc = tcsetattr(g_raw_fd, TCSAFLUSH, &g_saved_termios);
    g_raw_active = 0;
    return rc;
}

int lv_plat_term_israw(void) { return g_raw_active; }

/* ---- terminal size (designs/techdesign-terminal-floor.md §2). ioctl on any
 * fd; a 0x0 report (some multiplexers) is treated as failure per the ruling,
 * so the caller drops to its cursor-report / 24x80 fallback rather than sizing
 * a UI to nothing. No state; safe to call repeatedly. */
int lv_plat_term_size(int fd, int* rows, int* cols) {
    struct winsize ws;
    if (ioctl(fd, TIOCGWINSZ, &ws) != 0) return -1;   /* ENOTTY on a pipe */
    if (ws.ws_row == 0 || ws.ws_col == 0) return -1;  /* 0x0 = failure (§2/§5) */
    *rows = (int)ws.ws_row;
    *cols = (int)ws.ws_col;
    return 0;
}

/* ---- signals as streams (designs/techdesign-terminal-floor.md §3). A signal
 * is a readable fd; language code never runs in signal context. Linux uses
 * signalfd; other POSIX targets fall back to a self-pipe whose handler body is
 * exactly write(pipe_wr,&signo,1) (issue #3: a full pipe drops — coalescing is
 * correct, so the write failure is ignored ON PURPOSE, not "fixed"). A tiny
 * registry records each fd's blocked set so close() can unblock exactly it —
 * the prelude keeps one fd per signal number, so the sets stay disjoint (§3,
 * issue #1: workers spawned after this inherit the mask via pthread_create). */
#define LV_SIG_MAX_FDS 64
static struct { int fd; sigset_t mask; } g_sigfds[LV_SIG_MAX_FDS];
static int g_sigfd_count = 0;

static int lv_sig_reject(const int* sigs, int n) {
    for (int i = 0; i < n; i++)
        if (sigs[i] == SIGKILL || sigs[i] == SIGSTOP) return 1;   /* uncatchable */
    return 0;
}
static void lv_sig_register(int fd, const sigset_t* mask) {
    if (g_sigfd_count < LV_SIG_MAX_FDS) {
        g_sigfds[g_sigfd_count].fd = fd;
        g_sigfds[g_sigfd_count].mask = *mask;
        g_sigfd_count++;
    }
}

#ifdef __linux__
int lv_plat_signal_open(const int* sigs, int n) {
    if (n <= 0 || lv_sig_reject(sigs, n)) return -1;
    sigset_t mask;
    sigemptyset(&mask);
    for (int i = 0; i < n; i++) sigaddset(&mask, sigs[i]);
    /* Block first (issue #1): a blocked, generated signal stays pending and is
     * delivered through the fd instead of a default-disposition kill. */
    if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) return -1;
    int fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (fd < 0) { sigprocmask(SIG_UNBLOCK, &mask, NULL); return -1; }
    lv_sig_register(fd, &mask);
    return fd;
}
int lv_plat_signal_next(int fd) {
    struct signalfd_siginfo si;
    ssize_t r = read(fd, &si, sizeof(si));
    if (r != (ssize_t)sizeof(si)) return -1;          /* EAGAIN: none queued now */
    return (int)si.ssi_signo;
}
#else
/* Self-pipe fallback (macOS / non-signalfd POSIX). One write end per signo,
 * kept in a global the async-signal-safe handler indexes by signo. */
#include <limits.h>
#ifndef NSIG
#define NSIG 65
#endif
static volatile int g_selfpipe_wr[NSIG];
static int g_selfpipe_inited = 0;
static void lv_selfpipe_handler(int signo) {
    unsigned char b = (unsigned char)signo;
    int wr = (signo >= 0 && signo < NSIG) ? g_selfpipe_wr[signo] : -1;
    if (wr >= 0) { ssize_t w = write(wr, &b, 1); (void)w; }   /* full pipe: drop (issue #3) */
}
int lv_plat_signal_open(const int* sigs, int n) {
    if (n <= 0 || lv_sig_reject(sigs, n)) return -1;
    if (!g_selfpipe_inited) {
        for (int i = 0; i < NSIG; i++) g_selfpipe_wr[i] = -1;
        g_selfpipe_inited = 1;
    }
    int p[2];
    if (pipe(p) != 0) return -1;
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    sigset_t mask;
    sigemptyset(&mask);
    for (int i = 0; i < n; i++) {
        int s = sigs[i];
        if (s < 0 || s >= NSIG) { close(p[0]); close(p[1]); return -1; }
        g_selfpipe_wr[s] = p[1];
        sigaddset(&mask, s);
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = lv_selfpipe_handler;
        sa.sa_flags = SA_RESTART;
        sigfillset(&sa.sa_mask);
        sigaction(s, &sa, NULL);
    }
    /* The read end IS the stream fd; the write end is owned by the handler for
     * the process lifetime (v1 does not reclaim it on close — signal handlers
     * are process-global anyway, and this fallback is the non-Linux path; the
     * Linux signalfd path above has no such write end). */
    lv_sig_register(p[0], &mask);
    return p[0];
}
int lv_plat_signal_next(int fd) {
    unsigned char b;
    ssize_t r = read(fd, &b, 1);
    if (r != 1) return -1;
    return (int)b;
}
#endif

void lv_plat_signal_close(int fd) {
    for (int i = 0; i < g_sigfd_count; i++) {
        if (g_sigfds[i].fd == fd) {
            sigprocmask(SIG_UNBLOCK, &g_sigfds[i].mask, NULL);
            g_sigfds[i] = g_sigfds[--g_sigfd_count];   /* swap-remove */
            break;
        }
    }
    close(fd);
}

int64_t lv_plat_stat_size(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

int64_t lv_plat_stat_mtime(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int64_t)st.st_mtime;
}

/* request-stat-isdir.md: 1 dir / 0 not-dir / -1 absent. stat(2) only needs
 * search permission on the parent path components, not on the target itself,
 * so this correctly classifies an unreadable (mode 000) directory as a
 * directory where an opendir()-based probe would fail and misclassify it. */
int lv_plat_stat_isdir(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

/* Create a directory (mode 0755). 0 on success, -1 on any failure — exact
 * byte-parity with the oracle (RuntimeNatives.cpp sysMkdir: mkdir==0?0:-1,
 * so an already-existing path reports -1 too). */
int lv_plat_mkdir(const char* path) {
    return mkdir(path, 0755) == 0 ? 0 : -1;
}

static void lv_set_nonblock_fd(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

void lv_plat_set_nonblock(int fd) {
    lv_set_nonblock_fd(fd);
}

/* IPv6 when the bare literal contains ':' (Track 08 F5.5 — mirrors
 * RuntimeNatives.cpp's fillSockAddr; brackets stay in URL code). */
static int lv_fill_sockaddr(const char* ip, int port,
                            struct sockaddr_storage* ss, socklen_t* len) {
    if (strchr(ip, ':')) {
        struct sockaddr_in6* a6 = (struct sockaddr_in6*)ss;
        a6->sin6_family = AF_INET6;
        a6->sin6_port = htons((uint16_t)port);
        if (inet_pton(AF_INET6, ip, &a6->sin6_addr) != 1) return 0;
        *len = sizeof *a6;
        return 1;
    }
    struct sockaddr_in* a4 = (struct sockaddr_in*)ss;
    a4->sin_family = AF_INET;
    a4->sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &a4->sin_addr) != 1) return 0;
    *len = sizeof *a4;
    return 1;
}

int lv_plat_tcp_connect(const char* ip, int port) {
    struct sockaddr_storage ss;
    socklen_t slen = 0;
    memset(&ss, 0, sizeof ss);
    if (!lv_fill_sockaddr(ip, port, &ss, &slen)) return -1;
    int fd = socket(((struct sockaddr*)&ss)->sa_family, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr*)&ss, slen) < 0) { close(fd); return -1; }
    lv_set_nonblock_fd(fd);        /* subsequent I/O is pumped by the loop */
    return fd;
}

int lv_plat_tcp_connect_nb(const char* ip, int port) {
    struct sockaddr_storage ss;
    socklen_t slen = 0;
    memset(&ss, 0, sizeof ss);
    if (!lv_fill_sockaddr(ip, port, &ss, &slen)) return -1;
    int fd = socket(((struct sockaddr*)&ss)->sa_family, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    lv_set_nonblock_fd(fd);        /* BEFORE connect — the whole point */
    if (connect(fd, (struct sockaddr*)&ss, slen) < 0 && errno != EINPROGRESS) {
        close(fd); return -1;
    }
    return fd;
}

int lv_plat_connect_result(int fd) {
    int soerr = 0;
    socklen_t sl = sizeof soerr;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0) return -1;
    return soerr;
}

int lv_plat_sock_buffer(int fd, int64_t send_bytes, int64_t recv_bytes) {
    int rc = 0;
    if (send_bytes > 0) { int v = (int)send_bytes;
        if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof v) != 0) rc = -1; }
    if (recv_bytes > 0) { int v = (int)recv_bytes;
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof v) != 0) rc = -1; }
    return rc;
}

int lv_plat_tcp_listen(const char* ip, int port, int backlog, int reuse_port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
#ifdef SO_REUSEPORT
    if (reuse_port && setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof yes) != 0) {
        close(fd); return -1;
    }
#else
    if (reuse_port) { close(fd); return -1; }
#endif
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (ip && ip[0]) {
        if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) { close(fd); return -1; }
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    if (bind(fd, (struct sockaddr*)&addr, sizeof addr) < 0 ||
        listen(fd, backlog) < 0) {
        close(fd); return -1;
    }
    lv_set_nonblock_fd(fd);
    return fd;
}

int lv_plat_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

int lv_plat_accept(int fd) {
    int cfd = accept(fd, NULL, NULL);
    if (cfd >= 0) lv_set_nonblock_fd(cfd);
    return cfd;                    /* -1 (EAGAIN/EWOULDBLOCK) => none pending */
}

int64_t lv_plat_send(int fd, const void* buf, int64_t n) {
    /* -1 retryable / -2 fatal (lv_plat.h contract; RuntimeNatives.cpp
     * parity). ENOTSOCK falls back to write(2) — pipes take the same call. */
    int64_t r = (int64_t)send(fd, buf, (size_t)n, MSG_NOSIGNAL);
    if (r < 0 && errno == ENOTSOCK)
        r = (int64_t)write(fd, buf, (size_t)n);
    if (r < 0)
        r = (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? -1 : -2;
    return r;
}

int64_t lv_plat_recv(int fd, void* buf, int64_t n) {
    int64_t r = (int64_t)recv(fd, buf, (size_t)n, 0);
    if (r < 0 && errno == ENOTSOCK)
        r = (int64_t)read(fd, buf, (size_t)n);
    return r;
}

/* LA-2 §8: kernel CSPRNG — getrandom(2) (blocking pool, short-read looped) via
 * glibc's <sys/random.h> wrapper (portable across arches, unlike a raw syscall
 * number); /dev/urandom read-loop as a fallback where getrandom is unavailable
 * (older kernels / getentropy-only platforms). Fills exactly n bytes or returns
 * -1. Byte-matches the oracle's sysRandom (getrandom). */
int64_t lv_plat_random(void* buf, int64_t n) {
    unsigned char* p = (unsigned char*)buf;
    int64_t got = 0;
#if defined(__linux__) || defined(__GLIBC__)
    while (got < n) {
        ssize_t r = getrandom(p + got, (size_t)(n - got), 0);
        if (r < 0) { if (errno == EINTR) continue; break; }   /* fall through to urandom */
        got += r;
    }
    if (got >= n) return got;
#endif
    int fd = open("/dev/urandom", O_RDONLY);                  /* fallback */
    if (fd < 0) return -1;
    while (got < n) {
        int64_t r = (int64_t)read(fd, p + got, (size_t)(n - got));
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; close(fd); return -1; }
        got += r;
    }
    close(fd);
    return got;
}

int lv_plat_poll(LvPollFd* fds, int nfds, int timeout_ms) {
    struct pollfd pfds[nfds > 0 ? nfds : 1];   /* VLA: nfds is loop-owned, not external input */
    for (int i = 0; i < nfds; i++) {
        pfds[i].fd = fds[i].fd;
        pfds[i].events = 0;
        if (fds[i].events & LV_POLLIN)  pfds[i].events |= POLLIN;
        if (fds[i].events & LV_POLLOUT) pfds[i].events |= POLLOUT;
        pfds[i].revents = 0;
    }
    int n = poll(pfds, (nfds_t)nfds, timeout_ms);
    if (n <= 0) {
        for (int i = 0; i < nfds; i++) fds[i].revents = 0;
        return n;
    }
    int ready = 0;
    for (int i = 0; i < nfds; i++) {
        int16_t rv = 0;
        if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) rv |= LV_POLLIN;
        /* ERR/HUP wake write watches too: a refused non-blocking connect
         * reports them (sometimes without POLLOUT) and the watch must fire
         * so sysConnectResult can deliver the verdict (lv_plat.h note). */
        if (pfds[i].revents & (POLLOUT | POLLHUP | POLLERR)) rv |= LV_POLLOUT;
        if (pfds[i].revents & POLLNVAL) rv |= LV_POLLIN | LV_POLLOUT | LV_POLLNVAL;
        fds[i].revents = rv;
        if (rv) ready++;
    }
    return ready;
}
