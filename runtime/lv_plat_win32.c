/* Track B — Windows platform floor (B-M5, doc-2 §5 table + §7).
 *
 * STATUS: IMPLEMENTED AND CROSS-VERIFIED. It is not in a default CMake target;
 * `runtime/build-triple.sh x86_64-pc-windows-gnu` (or another
 * `*-windows-*`/`*-mingw*` triple) selects it in place of lv_plat_posix.c.
 * The MinGW runtime archive, core corpus, and filesystem round trip compile
 * and execute under Wine as of 2026-07-19. Native-Windows release
 * certification remains a separate environment-specific gate.
 *
 * It implements the SAME lv_plat.h interface as lv_plat_posix.c, with the same
 * caller-visible contracts (portable open bits, nonblocking-accept -1==none,
 * recv 0==EOF, LV_POLL* translation incl. LV_POLLNVAL), so lv_runtime.c /
 * lv_loop.c are byte-identical across platforms — a new target is a new floor
 * file, nothing else (doc-2 §0.1).
 *
 * DESIGN DECISION (resolved here; docs left it implicit — see §10) — the fd
 * bridge. lv_plat.h's interface is a unified `int fd` space, but Windows has
 * three disjoint descriptor kinds: console HANDLEs (GetStdHandle), CRT file
 * fds (_open, small ints), and Winsock SOCKETs (UINT_PTR — 64-bit on Win64,
 * so NOT castable to `int` without truncation). Resolution:
 *   - console  : the fixed ints 0/1/2 → GetStdHandle (write/read special-case).
 *   - files    : CRT int fds from _open (always < LV_WIN_SOCK_BIAS).
 *   - sockets  : a small runtime-owned table; the fd handed back is
 *                LV_WIN_SOCK_BIAS + slot, so socket fds are disjoint from CRT
 *                fds and the 64-bit SOCKET is preserved losslessly in the
 *                table. write/read/close route by testing the bias; send/recv/
 *                poll/set_nonblock look the SOCKET up.
 * This keeps the `int fd` ABI intact on Windows with no change to any caller.
 */
#include "lv_plat.h"

#include <winsock2.h>   /* MUST precede windows.h */
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>     /* LA-2 §8: BCryptGenRandom (CSPRNG); links -lbcrypt */
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* ============================ socket fd table ============================= */
/* Socket fds are LV_WIN_SOCK_BIAS + slot. The bias sits far above any CRT fd
 * (which start at 3 and grow slowly), so `fd >= LV_WIN_SOCK_BIAS` cleanly
 * distinguishes a socket from a console/file fd in the shared entry points. */
#define LV_WIN_SOCK_BIAS 0x40000000

static SOCKET* g_socks = NULL;      /* slot -> SOCKET (INVALID_SOCKET == free) */
static int     g_socks_cap = 0;

static int lv_win_sock_register(SOCKET s) {
    for (int i = 0; i < g_socks_cap; i++) {
        if (g_socks[i] == INVALID_SOCKET) { g_socks[i] = s; return LV_WIN_SOCK_BIAS + i; }
    }
    int newcap = g_socks_cap ? g_socks_cap * 2 : 8;
    SOCKET* grown = (SOCKET*)realloc(g_socks, (size_t)newcap * sizeof(SOCKET));
    if (!grown) return -1;
    g_socks = grown;
    for (int i = g_socks_cap; i < newcap; i++) g_socks[i] = INVALID_SOCKET;
    int slot = g_socks_cap;
    g_socks_cap = newcap;
    g_socks[slot] = s;
    return LV_WIN_SOCK_BIAS + slot;
}

static SOCKET lv_win_sock_get(int fd) {
    int slot = fd - LV_WIN_SOCK_BIAS;
    if (slot < 0 || slot >= g_socks_cap) return INVALID_SOCKET;
    return g_socks[slot];
}

static void lv_win_sock_release(int fd) {
    int slot = fd - LV_WIN_SOCK_BIAS;
    if (slot >= 0 && slot < g_socks_cap) g_socks[slot] = INVALID_SOCKET;
}

/* Lazy one-time Winsock init. §7 suggests "WSAStartup in lv_rt_init"; doing it
 * lazily on first socket use keeps this floor self-contained (no lv_rt_init /
 * lv_plat.h change) so the pre-land stays inert. A future bring-up may hoist it
 * into an explicit lv_plat_init() hook if preferred. */
static void lv_win_ensure_winsock(void) {
    static int started = 0;
    if (!started) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        started = 1;
    }
}

/* ---- B-M1 slice --------------------------------------------------------- */

void* lv_plat_map(int64_t size) {
    void* p = VirtualAlloc(NULL, (SIZE_T)size, MEM_COMMIT | MEM_RESERVE,
                           PAGE_READWRITE);
    return p;   /* VirtualAlloc already returns NULL on failure */
}

void lv_plat_unmap(void* base, int64_t size) {
    (void)size;                       /* MEM_RELEASE requires a 0 size */
    VirtualFree(base, 0, MEM_RELEASE);
}

int64_t lv_plat_write(int fd, const void* buf, int64_t n) {
    HANDLE h;
    if (fd == 1)      h = GetStdHandle(STD_OUTPUT_HANDLE);
    else if (fd == 2) h = GetStdHandle(STD_ERROR_HANDLE);
    else if (fd < LV_WIN_SOCK_BIAS)
        return (int64_t)_write(fd, buf, (unsigned)n);   /* CRT file fd */
    else h = INVALID_HANDLE_VALUE;    /* write() is never called on sockets */
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD wrote = 0;
    if (!WriteFile(h, buf, (DWORD)n, &wrote, NULL)) return -1;
    return (int64_t)wrote;
}

int64_t lv_plat_read(int fd, void* buf, int64_t n) {
    HANDLE h;
    if (fd == 0)      h = GetStdHandle(STD_INPUT_HANDLE);
    else if (fd < LV_WIN_SOCK_BIAS)
        return (int64_t)_read(fd, buf, (unsigned)n);     /* CRT file fd */
    else h = INVALID_HANDLE_VALUE;    /* read() is never called on sockets */
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)n, &got, NULL)) return -1;
    return (int64_t)got;
}

int64_t lv_plat_now_ns(void) {
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    /* split to avoid int64 overflow of counter*1e9 on long uptimes */
    int64_t sec  = c.QuadPart / freq.QuadPart;
    int64_t rem  = c.QuadPart % freq.QuadPart;
    return sec * 1000000000LL + rem * 1000000000LL / freq.QuadPart;
}

int64_t lv_plat_now_realtime_ms(void) {
    /* FILETIME is 100ns ticks since 1601-01-01; shift to the Unix epoch. */
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    int64_t t = ((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return t / 10000LL - 11644473600000LL;
}

void lv_plat_exit(int code) {
    ExitProcess((UINT)code);
}

/* ---- B-M3 slice: files, sockets, poll ----------------------------------- */

int lv_plat_open(const char* path, int bits, int mode) {
    /* Bit convention and flag derivation mirror lv_plat_posix.c EXACTLY so
     * file semantics are byte-identical; _O_BINARY forces raw bytes (no CRLF
     * text translation — §7's console/newline rule for corpus parity). Win32
     * accepts '/' so the corpus's forward slashes need no rewriting. */
    int rd = bits & 1, wr = bits & 2, ap = bits & 4;
    int flags;
    if (rd && wr) flags = _O_RDWR | _O_CREAT;
    else if (wr) flags = _O_WRONLY | _O_CREAT | _O_TRUNC;
    else flags = _O_RDONLY;
    if (ap) flags = (flags & ~_O_TRUNC) | _O_APPEND | _O_CREAT | (rd ? _O_RDWR : _O_WRONLY);
    flags |= _O_BINARY;
    (void)mode;
    return _open(path, flags, _S_IREAD | _S_IWRITE);
}

/* A pty master is a bridge socket (designs/pty/ 03 D-W1), so closing it must
 * also run the D-W5 teardown: the socket close IS step 1 (it breaks the conin
 * pump's recv), and the hook below does steps 2-4. Defined in the ConPTY
 * section; a no-op for every ordinary socket. */
static void lv_win_pty_on_close(int fd);

void lv_plat_close(int fd) {
    if (fd >= LV_WIN_SOCK_BIAS) {
        SOCKET s = lv_win_sock_get(fd);
        if (s != INVALID_SOCKET) closesocket(s);   /* D-W5 step 1 */
        lv_win_sock_release(fd);
        lv_win_pty_on_close(fd);                   /* D-W5 steps 2-4, if a pty */
    } else {
        _close(fd);
    }
}

/* ---- terminal raw mode (designs/terminal-raw-mode.md §4). Windows console
 * analogue of the POSIX termios path: clear line/echo/processed input and
 * enable virtual-terminal input so ^C etc. arrive as bytes. Single saved slot,
 * g_raw_active authoritative for restore (fd advisory). Same contract as POSIX
 * so lv_runtime.c is byte-identical across platforms. */
static DWORD g_win_saved_mode = 0;
static int   g_win_raw_active = 0;
int lv_plat_term_raw(int fd) {
    (void)fd;
    if (g_win_raw_active) return 0;
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD mode;
    if (!GetConsoleMode(h, &mode)) return -1;      /* not a console */
    g_win_saved_mode = mode;
    mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    if (!SetConsoleMode(h, mode)) return -1;
    g_win_raw_active = 1;
    return 0;
}
int lv_plat_term_restore(int fd) {
    (void)fd;
    if (!g_win_raw_active) return 0;
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    int rc = SetConsoleMode(h, g_win_saved_mode) ? 0 : -1;
    g_win_raw_active = 0;
    return rc;
}
int lv_plat_term_israw(void) { return g_win_raw_active; }

/* Terminal size (designs/techdesign-terminal-floor.md §2): the console window
 * extent (srWindow), not the scroll-back buffer size. -1 if not a console. */
int lv_plat_term_size(int fd, int* rows, int* cols) {
    (void)fd;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return -1;
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(h, &csbi)) return -1;
    int r = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    int c = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    if (r <= 0 || c <= 0) return -1;
    *rows = r; *cols = c;
    return 0;
}

/* Signals-as-streams: Windows v1 stub, always-fail (POSIX-first, the
 * spawn/Channel Windows-reject precedent — §3 lane matrix). */
int  lv_plat_signal_open(const int* sigs, int n) { (void)sigs; (void)n; return -1; }
int  lv_plat_signal_next(int fd) { (void)fd; return -1; }
void lv_plat_signal_close(int fd) { (void)fd; }

/* pipes-spawn floor: no Windows story in v1 — the spawn/Channel Windows-reject
 * precedent. LlvmGen rejects sysSpawn/sysPidfdOpen for a Windows target at
 * compile time (techdesign-spawn-llvm.md D4, narrowed to those two by
 * designs/pty/ 03 §7); these stubs keep the archive linking and return the
 * frozen failure sentinels if ever reached. lv_plat_reap/lv_plat_kill are NOT
 * stubs any more — they are the registry-backed bodies in the ConPTY section
 * below (D-W3), which is what let the codegen reject narrow. */
int lv_plat_spawn(const char* path, char* const argv[], int fds[3]) {
    (void)path; (void)argv; (void)fds; return -1;
}
/* Stays -1 on Windows by ruling (D-W2): there is no pollable exit fd here, so
 * the Pty prelude takes its existing 20 ms poll-reap fallback — zero new
 * prelude code. Exit still *arrives* promptly as bridge-socket EOF. */
int lv_plat_pidfd_open(int pid) { (void)pid; return -1; }

/* ======================= ConPTY floor (designs/pty/ 03) ===================
 * The Windows pty lane. Four pieces, in order below:
 *   1. the CreatePseudoConsole probe (D-P8: pre-1809 degrades at RUNTIME to
 *      -1, which the language sees as the ordinary [] spawn failure — never a
 *      compile-time reject, the same binary must run on both);
 *   2. lv_win_socketpair — Windows has no socketpair(2), so the bridge pair is
 *      built the classic way (loopback listener on port 0, connect, accept);
 *   3. the pid -> {hProcess, HPCON, bridge} registry (D-W3) that backs
 *      reap/kill/resize/close — reap NEVER goes to the OS by pid (K7/W2: the
 *      registry holds the HANDLE, so pid reuse cannot alias a stranger);
 *   4. the bridge itself (D-W1): ConPTY's anonymous pipes cannot be WSAPoll'd,
 *      so two floor-internal pump threads move bytes between those pipes and a
 *      loopback socketpair whose loop-side end IS the master fd the language
 *      sees (registered in the socket table above, so lv_plat_poll/send/recv
 *      work on it with ZERO changes). NO language code ever runs on a pump
 *      thread — the signal self-pipe sanction, lv_plat.h.
 * Exit detection is bridge EOF + poll-reap (D-W2); teardown ordering is D-W5
 * (loop socket, then ClosePseudoConsole from the CALLING thread, then join). */

/* MinGW's headers declare neither HPCON nor the three ConPTY entry points (we
 * GetProcAddress them anyway, per the probe), and PROC_THREAD_ATTRIBUTE_
 * PSEUDOCONSOLE is likewise absent: ProcThreadAttributeValue(22,F,T,F). */
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

typedef void* LvHPCON;   /* HPCON is an opaque handle; never dereferenced */
typedef HRESULT (WINAPI* LvCreatePseudoConsoleFn)(COORD, HANDLE, HANDLE,
                                                  DWORD, LvHPCON*);
typedef HRESULT (WINAPI* LvResizePseudoConsoleFn)(LvHPCON, COORD);
typedef void    (WINAPI* LvClosePseudoConsoleFn)(LvHPCON);

static LvCreatePseudoConsoleFn g_conpty_create = NULL;
static LvResizePseudoConsoleFn g_conpty_resize = NULL;
static LvClosePseudoConsoleFn  g_conpty_close  = NULL;
static int g_conpty_probed = 0;

/* One cached GetProcAddress sweep. LV_PTY_NO_CONPTY=1 forces the pre-1809
 * answer — the §6.6 test hook for the runtime-degrade path (an env probe, not
 * a new exported symbol: the header surface stays additive-constant only). */
static int lv_win_conpty_available(void) {
    if (!g_conpty_probed) {
        g_conpty_probed = 1;
        const char* off = getenv("LV_PTY_NO_CONPTY");
        if (!(off && off[0] && off[0] != '0')) {
            HMODULE k32 = GetModuleHandleA("kernel32.dll");
            if (k32) {
                g_conpty_create = (LvCreatePseudoConsoleFn)(void*)
                    GetProcAddress(k32, "CreatePseudoConsole");
                g_conpty_resize = (LvResizePseudoConsoleFn)(void*)
                    GetProcAddress(k32, "ResizePseudoConsole");
                g_conpty_close  = (LvClosePseudoConsoleFn)(void*)
                    GetProcAddress(k32, "ClosePseudoConsole");
            }
        }
    }
    return g_conpty_create && g_conpty_resize && g_conpty_close;
}

/* The socketpair(2) stand-in. The listener is bound to 127.0.0.1:0 and dropped
 * the moment the pair exists; the accepted peer is checked against the
 * connector's own local address so a racing local process cannot slip into the
 * bridge (a loopback listener is reachable by anything on the machine). */
static int lv_win_socketpair(SOCKET* out_a, SOCKET* out_b) {
    lv_win_ensure_winsock();
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == INVALID_SOCKET) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    int alen = (int)sizeof addr;
    if (bind(ls, (struct sockaddr*)&addr, alen) == SOCKET_ERROR ||
        listen(ls, 1) == SOCKET_ERROR ||
        getsockname(ls, (struct sockaddr*)&addr, &alen) == SOCKET_ERROR) {
        closesocket(ls); return -1;
    }
    SOCKET a = socket(AF_INET, SOCK_STREAM, 0);
    if (a == INVALID_SOCKET) { closesocket(ls); return -1; }
    if (connect(a, (struct sockaddr*)&addr, alen) == SOCKET_ERROR) {
        closesocket(a); closesocket(ls); return -1;
    }
    struct sockaddr_in mine, theirs;
    int mlen = (int)sizeof mine, tlen = (int)sizeof theirs;
    memset(&mine, 0, sizeof mine); memset(&theirs, 0, sizeof theirs);
    SOCKET b = accept(ls, (struct sockaddr*)&theirs, &tlen);
    closesocket(ls);                       /* the pair is made; drop the door */
    if (b == INVALID_SOCKET) { closesocket(a); return -1; }
    if (getsockname(a, (struct sockaddr*)&mine, &mlen) == SOCKET_ERROR ||
        theirs.sin_port != mine.sin_port ||
        theirs.sin_addr.s_addr != mine.sin_addr.s_addr) {
        closesocket(a); closesocket(b); return -1;   /* not our connection */
    }
    *out_a = a; *out_b = b;
    return 0;
}

/* ---- registry (D-W3 / §4) ------------------------------------------------
 * Same growth idiom as the socket table. Touched ONLY from the loop thread
 * (spawn/reap/kill/resize/close all originate there); the pumps own just their
 * handles and their socket end, so no lock is needed beyond the teardown join.
 * Slot lifetime: allocated at spawn; the bridge half (pumps, pipes, HPCON,
 * loop fd) is released by lv_plat_close's D-W5 teardown, the process half
 * (hProcess/hThread) at reap-success; the slot frees when both are done. A
 * program that never reaps leaks exactly one HANDLE per pty — the same honest
 * stance POSIX takes about unreaped children. */
typedef struct LvWinPty {
    int     used;
    int     pid;            /* 0 once reaped/collected */
    HANDLE  hProcess, hThread;
    LvHPCON hPC;
    int     loopFd;         /* socket-table fd handed to the language; -1 torn */
    SOCKET  pumpSide;       /* the pumps' end of the bridge */
    HANDLE  inW, outR;      /* the parent's ends of ConPTY's pipes */
    HANDLE  pumps[2];       /* 0 = conout, 1 = conin */
    int     torn;           /* bridge already torn down (D-W5 ran) */
} LvWinPty;

static LvWinPty* g_ptys = NULL;
static int       g_ptys_cap = 0;

static LvWinPty* lv_win_pty_alloc(void) {
    LvWinPty* e = NULL;
    for (int i = 0; i < g_ptys_cap && !e; i++)
        if (!g_ptys[i].used) e = &g_ptys[i];
    if (!e) {
        int newcap = g_ptys_cap ? g_ptys_cap * 2 : 4;
        LvWinPty* grown = (LvWinPty*)realloc(g_ptys, (size_t)newcap * sizeof(LvWinPty));
        if (!grown) return NULL;
        g_ptys = grown;
        memset(&g_ptys[g_ptys_cap], 0,
               (size_t)(newcap - g_ptys_cap) * sizeof(LvWinPty));
        e = &g_ptys[g_ptys_cap];
        g_ptys_cap = newcap;
    }
    memset(e, 0, sizeof *e);
    e->used = 1;                /* claimed immediately: the half-built slot is
                                 * never handed out twice by a re-entrant path */
    e->pumpSide = INVALID_SOCKET;
    e->loopFd = -1;
    return e;
}

static LvWinPty* lv_win_pty_by_pid(int pid) {
    for (int i = 0; i < g_ptys_cap; i++)
        if (g_ptys[i].used && g_ptys[i].pid == pid && pid > 0) return &g_ptys[i];
    return NULL;
}

static LvWinPty* lv_win_pty_by_fd(int fd) {
    for (int i = 0; i < g_ptys_cap; i++)
        if (g_ptys[i].used && !g_ptys[i].torn && g_ptys[i].loopFd == fd)
            return &g_ptys[i];
    return NULL;
}

static void lv_win_pty_maybe_free(LvWinPty* e) {
    if (e->torn && !e->hProcess) e->used = 0;
}

/* ---- the pumps (D-W1 / §3) ----------------------------------------------
 * Fixed 4 KiB stack buffers, no allocator use, no callbacks — auditable in
 * isolation. The conout pump NEVER pauses while the child lives (a full
 * ConPTY buffer blocks the child); when the loop stops reading, the socket
 * buffer fills, send blocks here, and the child blocks — the POSIX
 * full-master analog, surfaced identically (D-P9). */
typedef struct LvPumpArg { SOCKET sock; HANDLE pipe; } LvPumpArg;

static DWORD WINAPI lv_win_pty_conout(LPVOID param) {
    LvPumpArg a = *(LvPumpArg*)param;
    free(param);
    char buf[4096];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(a.pipe, buf, (DWORD)sizeof buf, &got, NULL) || got == 0)
            break;                       /* pipe broke: child/ConPTY is gone */
        DWORD off = 0;
        while (off < got) {
            int wrote = send(a.sock, buf + off, (int)(got - off), 0);
            if (wrote <= 0) goto done;   /* loop side closed */
            off += (DWORD)wrote;
        }
    }
done:
    shutdown(a.sock, SD_SEND);           /* loop-side recv -> 0 == EOF (D-W2) */
    return 0;
}

static DWORD WINAPI lv_win_pty_conin(LPVOID param) {
    LvPumpArg a = *(LvPumpArg*)param;
    free(param);
    char buf[4096];
    for (;;) {
        int got = recv(a.sock, buf, (int)sizeof buf, 0);
        if (got <= 0) break;             /* loop side closed */
        int off = 0;
        while (off < got) {
            DWORD wrote = 0;
            if (!WriteFile(a.pipe, buf + off, (DWORD)(got - off), &wrote, NULL) ||
                wrote == 0)
                return 0;                /* ConPTY input gone */
            off += (int)wrote;
        }
    }
    return 0;
}

static HANDLE lv_win_pump_start(LPTHREAD_START_ROUTINE body, SOCKET s, HANDLE pipe) {
    LvPumpArg* a = (LvPumpArg*)malloc(sizeof *a);
    if (!a) return NULL;
    a->sock = s; a->pipe = pipe;
    HANDLE t = CreateThread(NULL, 64 * 1024, body, a, 0, NULL);
    if (!t) free(a);
    return t;
}

/* D-W5 steps 2-4. The loop-side socket is closed by the CALLER before this
 * runs (step 1), which is what makes the conin pump's recv fail.
 * ClosePseudoConsole is called from HERE — the calling thread — never from the
 * conout pump, which must keep draining until the pipe breaks on its own; that
 * is the pre-24H2 deadlock rule (node-pty #415 / terminal #14160). Closing the
 * pseudoconsole terminates the attached tree: the documented SIGHUP analogy. */
static void lv_win_pty_teardown(LvWinPty* e) {
    if (e->torn) return;
    e->torn = 1;
    e->loopFd = -1;
    if (e->hPC && g_conpty_close) g_conpty_close(e->hPC);
    e->hPC = NULL;
    for (int i = 0; i < 2; i++) {
        if (!e->pumps[i]) continue;
        WaitForSingleObject(e->pumps[i], 2000);   /* bounded join (W3) */
        CloseHandle(e->pumps[i]);
        e->pumps[i] = NULL;
    }
    if (e->pumpSide != INVALID_SOCKET) { closesocket(e->pumpSide); e->pumpSide = INVALID_SOCKET; }
    if (e->inW)  { CloseHandle(e->inW);  e->inW  = NULL; }
    if (e->outR) { CloseHandle(e->outR); e->outR = NULL; }
    lv_win_pty_maybe_free(e);
}

static void lv_win_pty_on_close(int fd) {
    LvWinPty* e = lv_win_pty_by_fd(fd);
    if (e) lv_win_pty_teardown(e);
}

/* ---- argv -> command line (§2, the CommandLineToArgvW inverse) -----------
 * Windows takes ONE command-line string. The MSVCRT rules: quote an argument
 * that is empty or holds whitespace/quotes; inside quotes, a run of
 * backslashes is doubled only when it precedes a quote (or the closing quote),
 * and an embedded quote is backslash-escaped. lpApplicationName carries the
 * path separately, so the no-PATH-search contract is preserved regardless of
 * what argv[0] says. */
static int lv_win_cmd_push(char** buf, size_t* len, size_t* cap, const char* s, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t next = *cap ? *cap : 256;
        while (next < *len + n + 1) {
            if (next > SIZE_MAX / 2) return -1;
            next *= 2;
        }
        char* grown = (char*)realloc(*buf, next);
        if (!grown) return -1;
        *buf = grown; *cap = next;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

static char* lv_win_build_cmdline(char* const argv[]) {
    char* buf = NULL; size_t len = 0, cap = 0;
    if (lv_win_cmd_push(&buf, &len, &cap, "", 0) != 0) return NULL;
    for (int i = 0; argv && argv[i]; i++) {
        const char* a = argv[i];
        if (i && lv_win_cmd_push(&buf, &len, &cap, " ", 1) != 0) goto fail;
        int needq = (a[0] == '\0');
        for (const char* p = a; *p && !needq; p++)
            if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\v' || *p == '"')
                needq = 1;
        if (!needq) {
            if (lv_win_cmd_push(&buf, &len, &cap, a, strlen(a)) != 0) goto fail;
            continue;
        }
        if (lv_win_cmd_push(&buf, &len, &cap, "\"", 1) != 0) goto fail;
        for (const char* p = a;; p++) {
            size_t slashes = 0;
            while (*p == '\\') { slashes++; p++; }
            if (*p == '\0') {
                for (size_t s = 0; s < slashes * 2; s++)
                    if (lv_win_cmd_push(&buf, &len, &cap, "\\", 1) != 0) goto fail;
                break;
            }
            if (*p == '"') {
                for (size_t s = 0; s < slashes * 2 + 1; s++)
                    if (lv_win_cmd_push(&buf, &len, &cap, "\\", 1) != 0) goto fail;
            } else {
                for (size_t s = 0; s < slashes; s++)
                    if (lv_win_cmd_push(&buf, &len, &cap, "\\", 1) != 0) goto fail;
            }
            if (lv_win_cmd_push(&buf, &len, &cap, p, 1) != 0) goto fail;
        }
        if (lv_win_cmd_push(&buf, &len, &cap, "\"", 1) != 0) goto fail;
    }
    return buf;
fail:
    free(buf);
    return NULL;
}

/* UTF-8 in, UTF-16 out (R§B.2: the bytes on the bridge and in the command line
 * are UTF-8; no codepage translation anywhere in this floor). CreateProcessW,
 * not A, so a non-ASCII path or argument survives the trip. */
static wchar_t* lv_win_widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t* w = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) { free(w); return NULL; }
    return w;
}

int lv_plat_pty_spawn(const char* path, char* const argv[],
                      int rows, int cols, int flags, int* master) {
    /* flags bit0 (deterministic termios) is ACCEPTED AND IGNORED: there is no
     * termios here, ConPTY owns the cooked behavior. Determinism on Windows
     * comes from the behavioral test lane (doc 03 §6), never byte goldens —
     * documented, not hidden. */
    (void)flags;
    if (!path || !path[0] || rows <= 0 || cols <= 0 || !master) return -1;
    if (!lv_win_conpty_available()) return -1;   /* pre-1809: D-P8 degrade */
    lv_win_ensure_winsock();

    LvWinPty* e = lv_win_pty_alloc();
    if (!e) return -1;

    HANDLE inR = NULL, inW = NULL, outR = NULL, outW = NULL;
    if (!CreatePipe(&inR, &inW, NULL, 0)) { e->used = 0; return -1; }
    if (!CreatePipe(&outR, &outW, NULL, 0)) {
        CloseHandle(inR); CloseHandle(inW); e->used = 0; return -1;
    }
    COORD size;
    size.X = (SHORT)cols; size.Y = (SHORT)rows;
    LvHPCON hPC = NULL;
    HRESULT hr = g_conpty_create(size, inR, outW, 0, &hPC);   /* flags 0 ALWAYS:
        PSEUDOCONSOLE_INHERIT_CURSOR's cursor-query handshake deadlocks an
        unanswering host (pitfall #13) — banned by D-W5. */
    CloseHandle(inR); CloseHandle(outW);   /* ConPTY owns those ends now; the
        parent's copies MUST go or the child never sees EOF (the POSIX
        close-the-slave analog). */
    if (FAILED(hr) || !hPC) {
        CloseHandle(inW); CloseHandle(outR); e->used = 0; return -1;
    }

    /* 0xc0000142 startup-race rule: hPC is validated before CreateProcess and
     * nothing is torn down between the two except through the paths below. */
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attrSize);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs =
        (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attrSize);
    wchar_t* wpath = lv_win_widen(path);
    char*    cmd   = lv_win_build_cmdline(argv);
    wchar_t* wcmd  = cmd ? lv_win_widen(cmd) : NULL;
    free(cmd);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    int ok = attrs && wpath && wcmd &&
             InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
             UpdateProcThreadAttribute(attrs, 0,
                                       PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                       hPC, sizeof hPC, NULL, NULL);
    if (ok) {
        STARTUPINFOEXW si;
        memset(&si, 0, sizeof si);
        si.StartupInfo.cb = sizeof si;
        si.lpAttributeList = attrs;
        ok = CreateProcessW(wpath, wcmd, NULL, NULL, FALSE,
                            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                            NULL, NULL, &si.StartupInfo, &pi) ? 1 : 0;
        DeleteProcThreadAttributeList(attrs);
    }
    free(attrs); free(wpath); free(wcmd);
    if (!ok) {
        g_conpty_close(hPC);               /* no child exists: safe to close */
        CloseHandle(inW); CloseHandle(outR);
        e->used = 0;
        return -1;
    }

    /* The bridge (D-W1). Its loop-side end is registered in the socket table,
     * so it IS the master fd — WSAPoll/send/recv need no change at all. */
    SOCKET loopSide = INVALID_SOCKET, pumpSide = INVALID_SOCKET;
    int fd = -1;
    if (lv_win_socketpair(&loopSide, &pumpSide) == 0) {
        fd = lv_win_sock_register(loopSide);
        if (fd < 0) { closesocket(loopSide); closesocket(pumpSide); }
    }
    if (fd < 0) {
        TerminateProcess(pi.hProcess, LV_PTY_KILLED);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        g_conpty_close(hPC);
        CloseHandle(inW); CloseHandle(outR);
        e->used = 0;
        return -1;
    }
    u_long nb = 1;
    ioctlsocket(loopSide, FIONBIO, &nb);   /* the O_NONBLOCK master contract */

    e->pumps[0] = lv_win_pump_start(lv_win_pty_conout, pumpSide, outR);
    e->pumps[1] = lv_win_pump_start(lv_win_pty_conin,  pumpSide, inW);
    e->used = 1;
    e->pid = (int)pi.dwProcessId;
    e->hProcess = pi.hProcess;
    e->hThread  = pi.hThread;
    e->hPC = hPC;
    e->loopFd = fd;
    e->pumpSide = pumpSide;
    e->inW = inW;
    e->outR = outR;
    if (!e->pumps[0] || !e->pumps[1]) {    /* thread creation failed: unwind
        through the SAME D-W5 sequence a normal close takes — a half-built
        bridge is torn down exactly like a live one, never ad hoc. */
        lv_plat_close(fd);
        TerminateProcess(e->hProcess, LV_PTY_KILLED);
        CloseHandle(e->hProcess); e->hProcess = NULL;
        CloseHandle(e->hThread);  e->hThread  = NULL;
        e->pid = 0;
        lv_win_pty_maybe_free(e);
        return -1;
    }
    *master = fd;
    return e->pid;
}

int lv_plat_pty_resize(int master, int rows, int cols) {
    if (rows <= 0 || cols <= 0) return -1;
    LvWinPty* e = lv_win_pty_by_fd(master);
    if (!e || !e->hPC || !g_conpty_resize) return -1;
    COORD size;
    size.X = (SHORT)cols; size.Y = (SHORT)rows;
    return SUCCEEDED(g_conpty_resize(e->hPC, size)) ? 0 : -1;
}

/* D-W3. Ruled on the WAIT, never on the code value — that is what dodges the
 * STILL_ACTIVE(259) ambiguity. A registry miss is -1: "not ours", the same
 * answer POSIX's waitid gives for a non-child. Killed children report
 * LV_PTY_KILLED naturally, because that is the code lv_plat_kill terminates
 * with (D-W4) — never a fabricated 128+sig. */
int lv_plat_reap(int pid) {
    if (pid <= 0) return -1;
    LvWinPty* e = lv_win_pty_by_pid(pid);
    if (!e || !e->hProcess) return -1;
    if (WaitForSingleObject(e->hProcess, 0) != WAIT_OBJECT_0) return -1;
    DWORD code = 0;
    if (!GetExitCodeProcess(e->hProcess, &code)) code = 0;
    CloseHandle(e->hProcess); e->hProcess = NULL;
    if (e->hThread) { CloseHandle(e->hThread); e->hThread = NULL; }
    e->pid = 0;
    lv_win_pty_maybe_free(e);
    return (int)(code & 0xFF);
}

int lv_plat_kill(int pid, int sig) {
    (void)sig;                 /* no signals here: terminate-or-nothing (D-W3) */
    if (pid <= 0) return -1;
    LvWinPty* e = lv_win_pty_by_pid(pid);
    if (!e || !e->hProcess) return -1;
    return TerminateProcess(e->hProcess, LV_PTY_KILLED) ? 0 : -1;
}

int64_t lv_plat_stat_size(const char* path) {
    /* Pure Win32 (no CRT _stat64 struct-name ambiguity across CRTs); also
     * correct for files > 4 GB. -1 if the path does not exist (§5 contract). */
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return -1;
    return ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;
}

int64_t lv_plat_stat_mtime(const char* path) {
    /* Epoch seconds; -1 if the path does not exist — parity with the POSIX
     * floor's st_mtime (added at Track A's A-M5, doc-2 §10 2026-07-06 note).
     * FILETIME is 100 ns ticks since 1601-01-01; the Unix epoch offset is
     * 11644473600 seconds. */
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return -1;
    int64_t ticks = ((int64_t)fad.ftLastWriteTime.dwHighDateTime << 32)
                  | (int64_t)fad.ftLastWriteTime.dwLowDateTime;
    return ticks / 10000000LL - 11644473600LL;
}

/* request-stat-isdir.md: 1 dir / 0 not-dir / -1 absent — parity with the
 * POSIX floor's S_ISDIR probe. */
int lv_plat_stat_isdir(const char* path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return -1;
    return (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
}

/* Create a directory. 0 on success, -1 on any failure (incl. already-exists) —
 * parity with the POSIX floor / oracle sysMkdir. */
int lv_plat_mkdir(const char* path) {
    return CreateDirectoryA(path, NULL) ? 0 : -1;
}

/* LLVM filesystem/directory parity. This floor follows the repository's
 * existing ANSI-path convention; migrating the entire floor to UTF-16 is a
 * separate compatibility change. */
static void lv_dir_discard(char** names, size_t count) {
    if (names) {
        for (size_t i = 0; i < count; i++) free(names[i]);
        free(names);
    }
}

static int lv_dir_append(char*** names, size_t* count, size_t* capacity,
                         const char* name) {
    if ((uint64_t)*count >= (uint64_t)INT64_MAX) return -1;
    if (*count == *capacity) {
        size_t next;
        if (*capacity == 0) {
            next = 16;
        } else {
            if (*capacity > SIZE_MAX / 2) return -1;
            next = *capacity * 2;
        }
        if (next > SIZE_MAX / sizeof(char*)) return -1;
        char** grown = (char**)realloc(*names, next * sizeof(char*));
        if (!grown) return -1;
        *names = grown;
        *capacity = next;
    }

    size_t len = strlen(name);
    if (len == SIZE_MAX) return -1;
    char* copy = (char*)malloc(len + 1);
    if (!copy) return -1;
    memcpy(copy, name, len + 1);
    (*names)[(*count)++] = copy;
    return 0;
}

int lv_plat_remove(const char* path) {
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return -1;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY)
        return RemoveDirectoryA(path) ? 0 : -1;
    return DeleteFileA(path) ? 0 : -1;
}

int lv_plat_rename(const char* from, const char* to) {
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}

int lv_plat_listdir(const char* path, LvDirEntries* out) {
    if (!out) return -1;
    out->names = NULL;
    out->count = 0;

    size_t path_len = strlen(path);
    if (path_len > SIZE_MAX - 3) return -1;
    char* pattern = (char*)malloc(path_len + 3);
    if (!pattern) return -1;
    memcpy(pattern, path, path_len);
    pattern[path_len] = '\\';
    pattern[path_len + 1] = '*';
    pattern[path_len + 2] = '\0';

    WIN32_FIND_DATAA data;
    HANDLE search = FindFirstFileA(pattern, &data);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) return -1;

    char** names = NULL;
    size_t count = 0;
    size_t capacity = 0;
    int append_failed = 0;
    do {
        if (strcmp(data.cFileName, ".") == 0 ||
            strcmp(data.cFileName, "..") == 0)
            continue;
        if (lv_dir_append(&names, &count, &capacity, data.cFileName) != 0) {
            append_failed = 1;
            break;
        }
    } while (FindNextFileA(search, &data));

    DWORD iteration_error = append_failed ? ERROR_NOT_ENOUGH_MEMORY : GetLastError();
    BOOL closed = FindClose(search);
    if (append_failed || iteration_error != ERROR_NO_MORE_FILES || !closed) {
        lv_dir_discard(names, count);
        return -1;
    }

    out->names = names;
    out->count = (int64_t)count;
    return 0;
}

void lv_plat_listdir_free(LvDirEntries* entries) {
    if (!entries) return;
    size_t count = entries->count > 0 ? (size_t)entries->count : 0;
    lv_dir_discard(entries->names, count);
    entries->names = NULL;
    entries->count = 0;
}

void lv_plat_set_nonblock(int fd) {
    SOCKET s = lv_win_sock_get(fd);
    if (s == INVALID_SOCKET) return;
    u_long yes = 1;
    ioctlsocket(s, FIONBIO, &yes);
}

/* IPv6 when the bare literal contains ':' (Track 08 F5.5 — same rule as the
 * POSIX floor; brackets stay in URL code). */
static int lv_win_fill_sockaddr(const char* ip, int port,
                                struct sockaddr_storage* ss, int* len) {
    if (strchr(ip, ':')) {
        struct sockaddr_in6* a6 = (struct sockaddr_in6*)ss;
        a6->sin6_family = AF_INET6;
        a6->sin6_port = htons((uint16_t)port);
        if (inet_pton(AF_INET6, ip, &a6->sin6_addr) != 1) return 0;
        *len = (int)sizeof *a6;
        return 1;
    }
    struct sockaddr_in* a4 = (struct sockaddr_in*)ss;
    a4->sin_family = AF_INET;
    a4->sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &a4->sin_addr) != 1) return 0;
    *len = (int)sizeof *a4;
    return 1;
}

int lv_plat_tcp_connect(const char* ip, int port) {
    lv_win_ensure_winsock();
    struct sockaddr_storage ss;
    int slen = 0;
    memset(&ss, 0, sizeof ss);
    if (!lv_win_fill_sockaddr(ip, port, &ss, &slen)) return -1;
    SOCKET s = socket(((struct sockaddr*)&ss)->sa_family, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return -1;
    /* Blocking connect (mirrors lv_plat_posix.c): it reports failure
     * synchronously, side-stepping B-H8 (WSAPoll's connect-failure blind spot
     * only bites the *async* connect path — see connect_nb below). */
    if (connect(s, (struct sockaddr*)&ss, slen) == SOCKET_ERROR) {
        closesocket(s); return -1;
    }
    u_long yes = 1;
    ioctlsocket(s, FIONBIO, &yes);    /* subsequent I/O is pumped by the loop */
    return lv_win_sock_register(s);
}

int lv_plat_tcp_connect_nb(const char* ip, int port) {
    lv_win_ensure_winsock();
    struct sockaddr_storage ss;
    int slen = 0;
    memset(&ss, 0, sizeof ss);
    if (!lv_win_fill_sockaddr(ip, port, &ss, &slen)) return -1;
    SOCKET s = socket(((struct sockaddr*)&ss)->sa_family, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return -1;
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);     /* BEFORE connect — the whole point */
    if (connect(s, (struct sockaddr*)&ss, slen) == SOCKET_ERROR &&
        WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(s); return -1;
    }
    /* B-H8 rider: pre-2004 WSAPoll can miss a FAILED async connect; the
     * in-language connectTimeout then settles at its deadline (timer path)
     * instead of instantly. Documented degradation, never a hang. */
    return lv_win_sock_register(s);
}

int lv_plat_connect_result(int fd) {
    SOCKET s = lv_win_sock_get(fd);
    if (s == INVALID_SOCKET) return -1;
    int soerr = 0;
    int sl = (int)sizeof soerr;
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &sl) != 0) return -1;
    return soerr;
}

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

int lv_plat_tcp_listen(const char* ip, int port, int backlog, int reuse_port) {
    (void)reuse_port;
    lv_win_ensure_winsock();
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return -1;
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (ip && ip[0]) {
        if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) { closesocket(s); return -1; }
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    if (bind(s, (struct sockaddr*)&addr, sizeof addr) == SOCKET_ERROR ||
        listen(s, backlog) == SOCKET_ERROR) {
        closesocket(s); return -1;
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    return lv_win_sock_register(s);
}

int lv_plat_cpu_count(void) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors ? (int)info.dwNumberOfProcessors : 1;
}

int lv_plat_accept(int fd) {
    SOCKET ls = lv_win_sock_get(fd);
    if (ls == INVALID_SOCKET) return -1;
    SOCKET cs = accept(ls, NULL, NULL);
    if (cs == INVALID_SOCKET) return -1;   /* WSAEWOULDBLOCK => none pending */
    u_long nb = 1;
    ioctlsocket(cs, FIONBIO, &nb);
    return lv_win_sock_register(cs);
}

int64_t lv_plat_send(int fd, const void* buf, int64_t n) {
    SOCKET s = lv_win_sock_get(fd);
    if (s == INVALID_SOCKET) return -2;               /* dead handle: fatal */
    /* No MSG_NOSIGNAL: Windows raises no SIGPIPE, a dead peer just errors.
     * -1 retryable / -2 fatal per the lv_plat.h contract. */
    int64_t r = (int64_t)send(s, (const char*)buf, (int)n, 0);
    if (r < 0) {
        int e = WSAGetLastError();
        r = (e == WSAEWOULDBLOCK || e == WSAEINTR) ? -1 : -2;
    }
    return r;
}

int64_t lv_plat_recv(int fd, void* buf, int64_t n) {
    SOCKET s = lv_win_sock_get(fd);
    if (s == INVALID_SOCKET) return -1;
    return (int64_t)recv(s, (char*)buf, (int)n, 0);   /* 0 == EOF, <0 == error */
}

/* LA-2 §8: the Windows CSPRNG — BCryptGenRandom with the system-preferred RNG
 * (the getrandom analogue). Fills exactly n bytes or returns -1. Needs -lbcrypt,
 * which build-triple.sh writes into lvrt.link for windows triples. */
int64_t lv_plat_random(void* buf, int64_t n) {
    return BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)n,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? n : -1;
}

int lv_plat_poll(LvPollFd* fds, int nfds, int timeout_ms) {
    if (nfds <= 0) {
        /* no watches: WSAPoll(nfds=0) is not a portable sleep on Windows, so
         * use Sleep() to match lv_plat_posix.c's poll(NULL,0,timeout) sleep. */
        if (timeout_ms > 0) Sleep((DWORD)timeout_ms);
        return 0;
    }
    WSAPOLLFD* pfds = (WSAPOLLFD*)malloc((size_t)nfds * sizeof(WSAPOLLFD));
    if (!pfds) return -1;
    for (int i = 0; i < nfds; i++) {
        pfds[i].fd = lv_win_sock_get(fds[i].fd);
        pfds[i].events = 0;
        if (fds[i].events & LV_POLLIN)  pfds[i].events |= POLLRDNORM;
        if (fds[i].events & LV_POLLOUT) pfds[i].events |= POLLWRNORM;
        pfds[i].revents = 0;
    }
    int n = WSAPoll(pfds, (ULONG)nfds, timeout_ms);
    if (n <= 0) {
        for (int i = 0; i < nfds; i++) fds[i].revents = 0;
        free(pfds);
        return n;
    }
    int ready = 0;
    for (int i = 0; i < nfds; i++) {
        int16_t rv = 0;
        if (pfds[i].revents & (POLLRDNORM | POLLHUP | POLLERR)) rv |= LV_POLLIN;
        /* ERR/HUP wake write watches too (refused async connect — lv_plat.h
         * note; same mapping as the POSIX floor). */
        if (pfds[i].revents & (POLLWRNORM | POLLHUP | POLLERR)) rv |= LV_POLLOUT;
        /* POLLNVAL == WSAPoll's invalid-socket report (fd closed under the
         * poll): wake once AND retire, per lv_plat.h's LV_POLLNVAL contract
         * (the 2026-07-05 busy-spin fix). Same mapping as the POSIX floor. */
        if (pfds[i].revents & POLLNVAL) rv |= LV_POLLIN | LV_POLLOUT | LV_POLLNVAL;
        fds[i].revents = rv;
        if (rv) ready++;
    }
    free(pfds);
    return ready;
}
