#include "runtime/RuntimeLoop.hpp"
#include "runtime/RuntimeValue.hpp"
#include "core/Ast.hpp"     // bug #3: walk a spawn body's AST to find its free variables
#include "lv_task.h"   // LA-30 B2 (doc 06 §4): sysTaskCancel/sysTaskShield registry calls
#include <unordered_set>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#ifdef __linux__
#include <sys/signalfd.h>
#endif
#include <cstring>
#include <cstdio>
#include <map>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <iostream>
#include <time.h>
#include <unistd.h>

extern char** environ;   // Track 08 F7: the child's envp (execve inherits ours)

// The native cores behind the prelude's empty-bodied methods. Shared by both
// execution engines. `err` non-empty => the engine raises a RuntimeException.

// argv for the interpreters — set by the driver before an engine runs
// (designs/argv.md §5.2). Default [""] so a bare `leviathan --run x.lev` (no
// `--`) still yields a length-1 array, never empty.
static std::vector<std::string> g_programArgs{""};
void setProgramArgs(std::vector<std::string> args) {
    if (args.empty()) args.push_back("");        // guarantee argv[0]
    g_programArgs = std::move(args);
}

// Terminal raw mode for the interpreters (designs/terminal-raw-mode.md §3.2).
// Direct termios, exactly as sysStat uses ::stat directly. Single saved slot.
// Restore is registered with atexit() on first enable so a --run/--ir program
// that leaves the terminal raw and then dies (uncaught, or leviathan exiting)
// never orphans the user's shell — the interpreter analogue of the compiled
// runtime's lvrt_term_shutdown epilogue (§3.4).
static struct termios g_ttySaved;
static bool g_ttyRaw = false;
static int  g_ttyFd  = -1;
static int termRestoreRc() {
    if (!g_ttyRaw) return 0;
    int rc = ::tcsetattr(g_ttyFd, TCSAFLUSH, &g_ttySaved) == 0 ? 0 : -1;
    g_ttyRaw = false;
    return rc;
}
static void termRestoreAtexit() { termRestoreRc(); }

// Raw mode unbuffers the engines' captured stdout (RuntimeValue.hpp). Declared
// down here because g_ttyRaw is the authority and lives in this file.
void interpEmitStdout(std::string& buf, const char* data, size_t len) {
    if (!g_ttyRaw) { buf.append(data, len); return; }
    if (!buf.empty()) {
        ssize_t n = ::write(1, buf.data(), buf.size()); (void)n;
        buf.clear();
    }
    ssize_t n = ::write(1, data, len); (void)n;
}

// Raw-mode safety handlers (designs/techdesign-terminal-floor.md §3): the
// interpreter analogue of the floor's lv_term_safety_handler. An external
// SIGTERM/HUP/INT/QUIT restores the terminal (single tcsetattr — async-signal-
// safe) then re-raises with default disposition (SA_RESETHAND reset us first),
// so a --run TUI killed mid-draw never orphans the shell. Disjoint from the
// scheduler's SIGSEGV reporter (issue #2). A signal the program subscribed to
// via signal::on is BLOCKED, so this handler never fires for it.
static void termSafetyHandler(int sig) {
    termRestoreRc();
    ::raise(sig);
}
static void termInstallSafety() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = termSafetyHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    const int sigs[4] = { SIGTERM, SIGHUP, SIGINT, SIGQUIT };
    for (int i = 0; i < 4; i++) ::sigaction(sigs[i], &sa, nullptr);
}

// Signals as streams for the interpreters (designs/techdesign-terminal-floor.md
// §3). Same direct-syscall approach as termRaw: signalfd on Linux, unavailable
// elsewhere (the interp is a Linux dev tool; the compiled floor carries the
// self-pipe fallback). Because --run/--ir run INSIDE the leviathan process,
// opened fds and the blocked mask OUTLIVE the program — interpSignalCleanup()
// (called at end of run) closes every fd and unblocks its signals so the next
// run, and leviathan itself, are left with a clean mask (M4, issue #5).
namespace {
struct SigFdRec { int fd; sigset_t mask; };
std::vector<SigFdRec> g_sigfds;

int interpSignalOpen(const std::vector<long long>& sigs) {
#ifdef __linux__
    if (sigs.empty()) return -1;
    sigset_t mask;
    sigemptyset(&mask);
    for (long long s : sigs) {
        if (s == SIGKILL || s == SIGSTOP) return -1;   // uncatchable
        sigaddset(&mask, (int)s);
    }
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) return -1;
    int fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (fd < 0) { sigprocmask(SIG_UNBLOCK, &mask, nullptr); return -1; }
    g_sigfds.push_back({fd, mask});
    return fd;
#else
    (void)sigs;
    return -1;
#endif
}
int interpSignalNext(int fd) {
#ifdef __linux__
    struct signalfd_siginfo si;
    ssize_t r = ::read(fd, &si, sizeof(si));
    if (r != (ssize_t)sizeof(si)) return -1;           // EAGAIN: none queued now
    return (int)si.ssi_signo;
#else
    (void)fd;
    return -1;
#endif
}
void interpSignalClose(int fd) {
    for (auto it = g_sigfds.begin(); it != g_sigfds.end(); ++it)
        if (it->fd == fd) {
            sigprocmask(SIG_UNBLOCK, &it->mask, nullptr);
            g_sigfds.erase(it);
            break;
        }
    ::close(fd);
}
}  // namespace
// Called from Eval::run / IrInterp::run at program end (M4): close every signal
// fd still open and unblock its signals, so the leviathan process (which
// outlives the program) never keeps a stale block or descriptor.
void interpSignalCleanup() {
    for (const SigFdRec& r : g_sigfds) {
        sigprocmask(SIG_UNBLOCK, &r.mask, nullptr);
        ::close(r.fd);
    }
    g_sigfds.clear();
}

static int termRaw(int fd) {
    if (g_ttyRaw) return 0;                            // idempotent; keep first save
    if (::tcgetattr(fd, &g_ttySaved) != 0) return -1;  // ENOTTY if not a tty
    static bool registered = false;
    if (!registered) { std::atexit(termRestoreAtexit); registered = true; }
    struct termios r = g_ttySaved;
    r.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    r.c_oflag &= ~(OPOST);
    r.c_cflag |= (CS8);
    r.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    r.c_cc[VMIN] = 1; r.c_cc[VTIME] = 0;
    if (::tcsetattr(fd, TCSAFLUSH, &r) != 0) return -1;
    g_ttyRaw = true; g_ttyFd = fd;
    termInstallSafety();                              // restore-on-external-kill (§3)
    return 0;
}

// Process exit for the interpreters (designs/exit-codes.md §4). --run/--ir run
// INSIDE the leviathan process and capture stdout in a sink flushed at the end
// (Eval::run/IrInterp::run's out_). A raw _exit mid-run would discard that
// buffer and skip the terminal restore — so sysExit must NOT _exit here. Instead
// it records a code + an exit-requested flag; the engine observes the flag after
// the native returns and unwinds CLEANLY back to run() (the same return path an
// uncaught throw uses, but non-error). run() then returns the captured output +
// the code and main.cpp flushes, then std::exit(code). Shared by both engines
// (they share nativeFreeCall); reset per run.
static int  g_interpExitCode = 0;
static bool g_interpExitRequested = false;
void interpResetExit()     { g_interpExitCode = 0; g_interpExitRequested = false; }
int  interpExitCode()      { return g_interpExitCode; }
bool interpExitRequested() { return g_interpExitRequested; }

namespace {
// Strict full-string int parse: optional leading '-', digits only, no
// surrounding space. Overflow (beyond long long) -> false (None).
bool strictParseInt(const std::string& s, long long& out) {
    if (s.empty()) return false;
    size_t i = s[0] == '-' ? 1 : 0;
    if (i == s.size()) return false;               // "-" alone
    for (size_t j = i; j < s.size(); ++j)
        if (!std::isdigit((unsigned char)s[j])) return false;
    errno = 0;
    char* end = nullptr;
    long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno == ERANGE || end != s.c_str() + s.size()) return false;
    out = v;
    return true;
}
// Strict full-string float parse: strtod, full consumption, finite result.
// No surrounding space (strtod would otherwise skip leading whitespace).
bool strictParseFloat(const std::string& s, double& out) {
    if (s.empty() || std::isspace((unsigned char)s.front()) ||
        std::isspace((unsigned char)s.back()))
        return false;
    errno = 0;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || !std::isfinite(v)) return false;
    out = v;
    return true;
}
// Track 08 F5: fill a sockaddr for host:port. IPv6 when the literal contains
// ':' (design §5.5: the floor takes BARE addresses only — bracket forms like
// [::1]:port stay in URL code). Returns false on an unparseable literal.
bool fillSockAddr(const std::string& host, long long port,
                  sockaddr_storage& ss, socklen_t& len) {
    // Numeric-literal fast path (no resolver call): IPv6 when it contains ':'.
    if (host.find(':') != std::string::npos) {
        auto* a6 = (sockaddr_in6*)&ss;
        a6->sin6_family = AF_INET6;
        a6->sin6_port = htons((uint16_t)port);
        if (::inet_pton(AF_INET6, host.c_str(), &a6->sin6_addr) == 1) {
            len = sizeof(sockaddr_in6);
            return true;
        }
    } else {
        auto* a4 = (sockaddr_in*)&ss;
        a4->sin_family = AF_INET;
        a4->sin_port = htons((uint16_t)port);
        if (::inet_pton(AF_INET, host.c_str(), &a4->sin_addr) == 1) {
            len = sizeof(sockaddr_in);
            return true;
        }
    }
    // Not a numeric literal — resolve the hostname (bug.md #72). The connect
    // floor previously accepted ONLY numeric IPs, so an HTTP(S) request to a
    // bare host like "www.google.com" failed with -1 even though the resolver
    // (getaddrinfo, already used by sysResolve) was available. Resolve here so
    // every caller — and every engine — connects to hostnames identically.
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0) return false;
    bool ok = false;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_addr && (size_t)ai->ai_addrlen <= sizeof ss) {
            std::memcpy(&ss, ai->ai_addr, ai->ai_addrlen);
            len = (socklen_t)ai->ai_addrlen;
            if (ai->ai_family == AF_INET)
                ((sockaddr_in*)&ss)->sin_port = htons((uint16_t)port);
            else if (ai->ai_family == AF_INET6)
                ((sockaddr_in6*)&ss)->sin6_port = htons((uint16_t)port);
            ok = true;
            break;
        }
    }
    ::freeaddrinfo(res);
    return ok;
}
}  // namespace

// ===========================================================================
//  TLS + crypto (LA-2 — designs/complete/techdesign-tls-crypto.md)
//
//  Interpreter (oracle + IR) implementation of the provider contract in
//  runtime/lv_tls.h — the SAME behavioral contract the compiled runtime's
//  lv_tls_openssl.c implements, differentially pinned by the TLS corpus (§2.4).
//  The fd IS the socket fd (wrap-in-place, §2.1): a session side-table keyed by
//  fd routes sysSend/sysRecv/sysClose through OpenSSL; sysWatch* keep polling
//  the raw fd, which is exactly right for TLS-over-TCP. The event loop reaches
//  session state through runtimeTlsWants/runtimeTlsPending (bottom of this
//  block), which return 0/false when no session exists — the §2.3 no-TLS
//  bit-identity guarantee, held by construction.
// ===========================================================================
#ifdef HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/evp.h>

namespace {

struct TlsSession {
    SSL* ssl = nullptr;
    int  want = 0;          // 0 none | 1 read | 2 write — poll augmentation (§2.3 #1)
    bool server = false;
    bool established = false;
    std::string error;      // per-fd latest detail (§10 #6)
};

// Session table keyed by fd (grown to max-fd-seen; null when the fd is plaintext).
std::vector<TlsSession*> g_tls;
std::string g_tlsProcErr;              // last setup error before a session exists
bool g_sigpipeIgn = false;             // §4 #6: SIG_IGN once at first session
std::map<std::string, SSL_CTX*> g_serverCtx;    // key cert|key|alpn -> cached ctx (§3, per-conn SSL_new)
std::map<SSL_CTX*, std::string> g_serverAlpn;   // wire ALPN list per server ctx (select cb arg)

TlsSession* tlsGet(int fd) {
    if (fd < 0 || fd >= (int)g_tls.size()) return nullptr;
    return g_tls[fd];
}
void tlsPut(int fd, TlsSession* s) {
    if (fd < 0) return;
    if (fd >= (int)g_tls.size()) g_tls.resize(fd + 1, nullptr);
    g_tls[fd] = s;
}
bool tlsIsFd(int fd) { return tlsGet(fd) != nullptr; }

void tlsSigpipeOnce() {
    if (!g_sigpipeIgn) { ::signal(SIGPIPE, SIG_IGN); g_sigpipeIgn = true; }
}

// §4 #7: drain the OpenSSL error queue into dst (never leave a stale queue —
// enforced by construction, one clear-before / drain-after per SSL call).
void tlsDrainErr(std::string& dst, const char* prefix) {
    std::string acc = prefix ? prefix : "";
    bool first = acc.empty();
    unsigned long e;
    while ((e = ERR_get_error()) != 0) {
        char b[256]; ERR_error_string_n(e, b, sizeof(b));
        if (!first) acc += "; ";
        acc += b; first = false;
    }
    if (acc.empty() || (prefix && acc == prefix)) acc += "unknown error";
    dst = acc;
}

// §2.3 #1: record want-direction from the last SSL_get_error verdict.
int tlsUpdateWant(TlsSession* s, int sslErr) {
    if (sslErr == SSL_ERROR_WANT_READ)  { s->want = 1; return 1; }
    if (sslErr == SSL_ERROR_WANT_WRITE) { s->want = 2; return 2; }
    s->want = 0;
    return 0;
}

// A bare IP literal (v4/v6) sends no SNI (RFC 6066) and verifies IP SANs (§3).
bool tlsIsIp(const std::string& h) {
    unsigned char buf[16];
    return ::inet_pton(AF_INET, h.c_str(), buf) == 1 ||
           ::inet_pton(AF_INET6, h.c_str(), buf) == 1;
}

// "h2,http/1.1" -> wire ALPN (\x02 h2 \x08 http/1.1). "" -> empty.
std::string tlsAlpnWire(const std::string& csv) {
    std::string w;
    size_t i = 0;
    while (i < csv.size()) {
        size_t j = csv.find(',', i);
        if (j == std::string::npos) j = csv.size();
        std::string p = csv.substr(i, j - i);
        if (!p.empty() && p.size() < 256) { w.push_back((char)p.size()); w += p; }
        i = j + 1;
    }
    return w;
}

// Server ALPN select: first server proto the client also offered; no overlap ->
// fatal no_application_protocol alert (RFC 7301 §3.2).
int tlsAlpnSelectCb(SSL*, const unsigned char** out, unsigned char* outlen,
                    const unsigned char* in, unsigned int inlen, void* arg) {
    std::string* wire = (std::string*)arg;
    if (!wire || wire->empty()) return SSL_TLSEXT_ERR_NOACK;
    const unsigned char* srv = (const unsigned char*)wire->data();
    size_t srvlen = wire->size();
    for (size_t i = 0; i + 1 <= srvlen; ) {
        unsigned char l = srv[i];
        if (i + 1 + l > srvlen) break;
        const unsigned char* sp = srv + i + 1;
        for (size_t k = 0; k + 1 <= inlen; ) {
            unsigned char cl = in[k];
            if (k + 1 + cl > inlen) break;
            if (cl == l && std::memcmp(in + k + 1, sp, l) == 0) {
                *out = sp; *outlen = l; return SSL_TLSEXT_ERR_OK;
            }
            k += 1 + cl;
        }
        i += 1 + l;
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

// §4 #9: NSS key-log lines only when SSLKEYLOGFILE is set (off by default).
void tlsKeylogCb(const SSL*, const char* line) {
    const char* path = ::getenv("SSLKEYLOGFILE");
    if (!path || !*path) return;
    if (FILE* f = ::fopen(path, "a")) { ::fprintf(f, "%s\n", line); ::fclose(f); }
}
void tlsKeylogMaybe(SSL_CTX* ctx) {
    if (::getenv("SSLKEYLOGFILE")) SSL_CTX_set_keylog_callback(ctx, tlsKeylogCb);
}

// Common context hardening (§4 #1/#3/#5), shared by client + server.
void tlsHardenCtx(SSL_CTX* ctx) {
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);                 // 1.2 floor, 1.3 on
    SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE |
                          SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
}

// §3: wrap a connected client socket. Returns fd (armed) or -1 (g_tlsProcErr set).
int tlsClientStart(int fd, const std::string& host, const std::string& alpnCsv,
                   const std::string& caFile, int verifyMode) {
    tlsSigpipeOnce();
    if (tlsIsFd(fd)) { g_tlsProcErr = "fd already TLS"; return -1; }
    if (host.empty() && verifyMode != 2) {
        g_tlsProcErr = "empty host requires verification mode 2"; return -1;
    }
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { tlsDrainErr(g_tlsProcErr, "SSL_CTX_new: "); return -1; }
    tlsHardenCtx(ctx);
    SSL_CTX_set_default_verify_paths(ctx);                             // system roots + env
    if (!caFile.empty() &&
        SSL_CTX_load_verify_locations(ctx, caFile.c_str(), nullptr) != 1) {
        tlsDrainErr(g_tlsProcErr, "load caFile: "); SSL_CTX_free(ctx); return -1;
    }
    tlsKeylogMaybe(ctx);
    if (!alpnCsv.empty()) {
        std::string w = tlsAlpnWire(alpnCsv);
        SSL_CTX_set_alpn_protos(ctx, (const unsigned char*)w.data(), (unsigned)w.size());
    }
    SSL* ssl = SSL_new(ctx);
    SSL_CTX_free(ctx);                                                 // SSL holds its own ref
    if (!ssl) { tlsDrainErr(g_tlsProcErr, "SSL_new: "); return -1; }
    SSL_set_fd(ssl, fd);
    SSL_set_connect_state(ssl);
    SSL_set_verify(ssl, verifyMode == 2 ? SSL_VERIFY_NONE : SSL_VERIFY_PEER, nullptr);
    if (!host.empty()) {
        bool ip = tlsIsIp(host);
        if (!ip) SSL_set_tlsext_host_name(ssl, host.c_str());         // SNI (never for IP)
        if (verifyMode == 0) {                                        // full: chain + identity (§4 #2)
            if (ip) X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), host.c_str());
            else    SSL_set1_host(ssl, host.c_str());
        }
        // verifyMode 1 (verify-ca): chain verified, identity NOT checked.
    }
    TlsSession* s = new TlsSession();
    s->ssl = ssl; s->server = false;
    tlsPut(fd, s);
    ERR_clear_error();                                                // §2.2 opportunistic step
    int r = SSL_do_handshake(ssl);
    if (r == 1) s->established = true;
    else tlsUpdateWant(s, SSL_get_error(ssl, r));
    return fd;
}

// §3: wrap an accepted server socket. ctx cached per (cert,key,alpn).
int tlsServerStart(int fd, const std::string& cert, const std::string& key,
                   const std::string& alpnCsv) {
    tlsSigpipeOnce();
    if (tlsIsFd(fd)) { g_tlsProcErr = "fd already TLS"; return -1; }
    std::string ckey = cert + "\x1f" + key + "\x1f" + alpnCsv;
    SSL_CTX* ctx;
    auto it = g_serverCtx.find(ckey);
    if (it != g_serverCtx.end()) {
        ctx = it->second;
    } else {
        ctx = SSL_CTX_new(TLS_server_method());
        if (!ctx) { tlsDrainErr(g_tlsProcErr, "SSL_CTX_new: "); return -1; }
        tlsHardenCtx(ctx);
        if (SSL_CTX_use_certificate_chain_file(ctx, cert.c_str()) != 1) {
            tlsDrainErr(g_tlsProcErr, "certificate: "); SSL_CTX_free(ctx); return -1;
        }
        if (SSL_CTX_use_PrivateKey_file(ctx, key.c_str(), SSL_FILETYPE_PEM) != 1) {
            tlsDrainErr(g_tlsProcErr, "private key: "); SSL_CTX_free(ctx); return -1;
        }
        if (SSL_CTX_check_private_key(ctx) != 1) {
            tlsDrainErr(g_tlsProcErr, "key/cert mismatch: "); SSL_CTX_free(ctx); return -1;
        }
        tlsKeylogMaybe(ctx);
        std::string w = tlsAlpnWire(alpnCsv);
        if (!w.empty()) {
            g_serverAlpn[ctx] = w;                                     // stable map-node addr
            SSL_CTX_set_alpn_select_cb(ctx, tlsAlpnSelectCb, &g_serverAlpn[ctx]);
        }
        g_serverCtx[ckey] = ctx;
    }
    SSL* ssl = SSL_new(ctx);
    if (!ssl) { tlsDrainErr(g_tlsProcErr, "SSL_new: "); return -1; }
    SSL_set_fd(ssl, fd);
    SSL_set_accept_state(ssl);
    TlsSession* s = new TlsSession();
    s->ssl = ssl; s->server = true;
    tlsPut(fd, s);
    ERR_clear_error();
    int r = SSL_do_handshake(ssl);
    if (r == 1) s->established = true;
    else tlsUpdateWant(s, SSL_get_error(ssl, r));
    return fd;
}

// §2.2: one handshake step. 0 done | 1 want-read | 2 want-write | -1 failed
// (error recorded — verify reason preferred, else the drained queue).
int tlsHandshake(int fd) {
    TlsSession* s = tlsGet(fd);
    if (!s) return -1;
    ERR_clear_error();
    int r = SSL_do_handshake(s->ssl);
    if (r == 1) { s->established = true; s->want = 0; return 0; }
    int w = tlsUpdateWant(s, SSL_get_error(s->ssl, r));
    if (w) return w;
    long vr = SSL_get_verify_result(s->ssl);
    if (vr != X509_V_OK)
        s->error = std::string("certificate verify failed: ") +
                   X509_verify_cert_error_string(vr);
    else
        tlsDrainErr(s->error, "");
    return -1;
}

// §2.1 send: n written | -1 retryable (want-*) | -2 fatal.
long tlsSend(int fd, const void* p, long n) {
    TlsSession* s = tlsGet(fd);
    if (!s) return -2;
    ERR_clear_error();
    size_t written = 0;
    int r = SSL_write_ex(s->ssl, p, (size_t)(n < 0 ? 0 : n), &written);
    if (r == 1) { s->want = 0; return (long)written; }
    int e = SSL_get_error(s->ssl, r);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) { tlsUpdateWant(s, e); return -1; }
    tlsDrainErr(s->error, ""); s->want = 0;
    return -2;
}

// §2.1 recv: n | 0 clean-EOF (or truncation) | -1 would-block | -2 fatal.
long tlsRecv(int fd, void* p, long n) {
    TlsSession* s = tlsGet(fd);
    if (!s) return -2;
    ERR_clear_error();
    size_t got = 0;
    int r = SSL_read_ex(s->ssl, p, (size_t)(n < 0 ? 0 : n), &got);
    if (r == 1) { s->want = 0; return (long)got; }
    int e = SSL_get_error(s->ssl, r);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) { tlsUpdateWant(s, e); return -1; }
    s->want = 0;
    if (e == SSL_ERROR_ZERO_RETURN) return 0;                         // clean close_notify
    if (e == SSL_ERROR_SYSCALL && ERR_peek_error() == 0) {           // EOF without close_notify
        s->error = "truncated"; return 0;                            // §4 #11 (still EOF)
    }
    tlsDrainErr(s->error, "");
    return -2;                                                       // fatal
}

// §2.1 close: single best-effort close_notify + free; clears the slot BEFORE the
// caller's close(2) so fd-number reuse can never leak a session (§10 #8).
bool tlsClose(int fd) {
    TlsSession* s = tlsGet(fd);
    if (!s) return false;
    ERR_clear_error();
    SSL_shutdown(s->ssl);                                            // never retried/blocked on
    SSL_free(s->ssl);
    delete s;
    tlsPut(fd, nullptr);
    return true;
}

std::string tlsError(int fd) {
    TlsSession* s = tlsGet(fd);
    if (s && !s->error.empty()) return s->error;
    return g_tlsProcErr;
}
std::string tlsAlpn(int fd) {
    TlsSession* s = tlsGet(fd);
    if (!s) return "";
    const unsigned char* p = nullptr; unsigned int len = 0;
    SSL_get0_alpn_selected(s->ssl, &p, &len);
    return (p && len) ? std::string((const char*)p, len) : "";
}
int tlsVersion(int fd) {
    TlsSession* s = tlsGet(fd);
    if (!s || !s->established) return 0;
    int v = SSL_version(s->ssl);
    if (v == TLS1_3_VERSION) return 13;
    if (v == TLS1_2_VERSION) return 12;
    return 0;
}

// §7: RSA public-key encrypt (modern EVP path). pad 0=OAEP(SHA-1) 1=PKCS#1 v1.5.
bool tlsRsaEncrypt(const std::string& pem, const std::string& in, int pad,
                   std::string& out, std::string& err) {
    BIO* bio = BIO_new_mem_buf(pem.data(), (int)pem.size());
    if (!bio) { err = "rsa: allocation failed"; return false; }
    EVP_PKEY* pk = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pk) { tlsDrainErr(err, "rsa: bad public key: "); return false; }
    EVP_PKEY_CTX* c = EVP_PKEY_CTX_new(pk, nullptr);
    bool ok = false;
    if (c && EVP_PKEY_encrypt_init(c) == 1 &&
        EVP_PKEY_CTX_set_rsa_padding(c, pad == 1 ? RSA_PKCS1_PADDING
                                                 : RSA_PKCS1_OAEP_PADDING) == 1) {
        size_t outlen = 0;
        const unsigned char* mp = (const unsigned char*)in.data();
        if (EVP_PKEY_encrypt(c, nullptr, &outlen, mp, in.size()) == 1) {
            out.resize(outlen);
            if (EVP_PKEY_encrypt(c, (unsigned char*)out.data(), &outlen, mp, in.size()) == 1) {
                out.resize(outlen); ok = true;
            }
        }
    }
    if (!ok) tlsDrainErr(err, "rsa: encrypt failed: ");
    if (c) EVP_PKEY_CTX_free(c);
    EVP_PKEY_free(pk);
    return ok;
}

}  // namespace (TLS internals)

// Loop queries (§2.3) — external linkage, declared in RuntimeLoop.hpp. No
// session => 0/false, so the poll set is bit-identical with zero TLS live.
int  runtimeTlsWants(int fd)   { TlsSession* s = tlsGet(fd); return s ? s->want : 0; }
bool runtimeTlsPending(int fd) { TlsSession* s = tlsGet(fd);
                                 return s && s->ssl && SSL_has_pending(s->ssl); }

#else  // !HAVE_OPENSSL — clean not-built stubs; plaintext fds take every path.
static const char* kTlsNotBuilt = "TLS support not built into this runtime";
static bool tlsIsFd(int)                     { return false; }
static long tlsSend(int, const void*, long)  { return -2; }
static long tlsRecv(int, void*, long)        { return -2; }
static bool tlsClose(int)                    { return false; }
int  runtimeTlsWants(int)                    { return 0; }
bool runtimeTlsPending(int)                  { return false; }
#endif // HAVE_OPENSSL

bool nativeCall(std::string_view cls, const std::string& m, const Value& self,
                std::vector<Value>& args, Value& out, std::string& err,
                bool cowReceiver) {
    if (cls == "string") {
        const std::string& s = self.s;
        auto argS = [&](size_t i) { return i < args.size() ? args[i].s : std::string(); };
        auto argI = [&](size_t i) { return i < args.size() ? args[i].i : 0; };
        if (m == "length")   { out = vint((long long)s.size()); return true; }
        if (m == "isEmpty")  { out = vbool(s.empty()); return true; }
        if (m == "toUpper")  { std::string r = s; for (char& c : r) c = (char)std::toupper((unsigned char)c); out = vstr(r); return true; }
        if (m == "toLower")  { std::string r = s; for (char& c : r) c = (char)std::tolower((unsigned char)c); out = vstr(r); return true; }
        if (m == "charAt")   { long long i = argI(0); out = vstr(i >= 0 && i < (long long)s.size() ? std::string(1, s[(size_t)i]) : ""); return true; }
        if (m == "subStr")   { long long a = argI(0), n = argI(1); out = vstr(a >= 0 && a <= (long long)s.size() ? s.substr((size_t)a, (size_t)n) : ""); return true; }
        if (m == "contains") { out = vbool(s.find(argS(0)) != std::string::npos); return true; }
        if (m == "indexOf")  { auto p = s.find(argS(0)); out = vint(p == std::string::npos ? -1 : (long long)p); return true; }
        if (m == "startsWith") { const std::string p = argS(0); out = vbool(s.rfind(p, 0) == 0); return true; }
        if (m == "endsWith") { const std::string p = argS(0); out = vbool(p.size() <= s.size() && s.compare(s.size() - p.size(), p.size(), p) == 0); return true; }
        if (m == "trim")     { size_t a = s.find_first_not_of(" \t\n\r"); size_t b = s.find_last_not_of(" \t\n\r"); out = vstr(a == std::string::npos ? "" : s.substr(a, b - a + 1)); return true; }
        if (m == "toInt")    { long long v; out = strictParseInt(s, v) ? vint(v) : vnone(); return true; }
        if (m == "toFloat")  { double v; out = strictParseFloat(s, v) ? vfloat(v) : vnone(); return true; }
        if (m == "byteAt") {
            long long i = argI(0);
            if (i < 0 || i >= (long long)s.size()) {
                err = "index " + std::to_string(i) + " out of bounds (length " +
                      std::to_string(s.size()) + ")";   // matches Array.at's wording exactly
                out = vvoid();
                return true;
            }
            out = vint((long long)(unsigned char)s[(size_t)i]);
            return true;
        }
        // Track 03 §1 (C1): decode the scalar starting at byte offset i (O(1)).
        // OOB or a mid-sequence offset throws (loud); valid lead byte decodes.
        if (m == "at") {
            long long i = argI(0);
            if (i < 0 || i >= (long long)s.size()) {
                err = "index " + std::to_string(i) + " out of bounds (length " +
                      std::to_string(s.size()) + ")";
                out = vvoid(); return true;
            }
            size_t len; bool boundary;
            long long cp = utf8DecodeAt(s, (size_t)i, len, boundary);
            if (!boundary) {
                err = "byte offset " + std::to_string(i) + " is not a scalar boundary";
                out = vvoid(); return true;
            }
            out = vchar(cp); return true;
        }
        // Full UTF-8 decode to Array<char>; invalid bytes -> U+FFFD (never throw).
        if (m == "chars") {
            std::vector<Value> cs;
            size_t i = 0;
            while (i < s.size()) {
                size_t len; bool boundary;
                long long cp = utf8DecodeAt(s, i, len, boundary);
                cs.push_back(vchar(cp));
                i += len;
            }
            out = varr(std::move(cs)); return true;
        }
    } else if (cls == "char") {
        // Track 03 §1: char natives (the classification/case helpers are
        // in-language over these two — see the prelude `class char`).
        if (m == "code")     { out = vint(self.i); return true; }
        if (m == "toString") { out = vstr(utf8Encode(self.i)); return true; }
    } else if (cls == "Block") {
        // Track 03 §3: byte-buffer natives. Every access is bounds-checked and
        // throws on OOB (loud). Writes mutate the shared backing store in place,
        // so a slice's write is visible through its parent (aliasing, §3.1).
        if (!self.block || !self.block->bytes) { err = "null block"; out = vvoid(); return true; }
        BlockData& bd = *self.block;
        std::vector<uint8_t>& bytes = *bd.bytes;
        auto argI = [&](size_t i) { return i < args.size() ? args[i].i : 0; };
        auto oob = [&](long long i) {
            err = "index " + std::to_string(i) + " out of bounds (length " +
                  std::to_string(bd.len) + ")";
            out = vvoid(); return true;
        };
        if (m == "length") { out = vint((long long)bd.len); return true; }
        if (m == "byteAt") {
            long long i = argI(0);
            if (i < 0 || i >= (long long)bd.len) return oob(i);
            out = vint((long long)bytes[bd.off + (size_t)i]); return true;
        }
        if (m == "setByte") {
            long long i = argI(0), v = argI(1);
            if (i < 0 || i >= (long long)bd.len) return oob(i);
            if (v < 0 || v > 255) {
                err = "byte value " + std::to_string(v) + " out of range 0..255";
                out = vvoid(); return true;
            }
            bytes[bd.off + (size_t)i] = (uint8_t)v; out = vvoid(); return true;
        }
        if (m == "slice") {
            long long o = argI(0), l = argI(1);
            if (o < 0 || l < 0 || o + l > (long long)bd.len) return oob(o);
            auto nb = std::make_shared<BlockData>();
            nb->bytes = bd.bytes;                        // SHARE the store (aliasing view)
            nb->off = bd.off + (size_t)o; nb->len = (size_t)l;
            out = vblock(nb); return true;
        }
        if (m == "toString") {
            long long o = argI(0), l = argI(1);
            if (o < 0 || l < 0 || o + l > (long long)bd.len) return oob(o);
            out = vstr(std::string(bytes.begin() + bd.off + o, bytes.begin() + bd.off + o + l));
            return true;
        }
        if (m == "int32At") {
            long long i = argI(0);
            if (i < 0 || i + 4 > (long long)bd.len) return oob(i);
            uint32_t u = 0;
            for (int k = 0; k < 4; ++k) u |= (uint32_t)bytes[bd.off + i + k] << (8 * k);
            out = vint((long long)(int32_t)u); return true;   // little-endian, sign-extended
        }
        if (m == "setInt32") {
            long long i = argI(0), v = argI(1);
            if (i < 0 || i + 4 > (long long)bd.len) return oob(i);
            uint32_t u = (uint32_t)v;
            for (int k = 0; k < 4; ++k) bytes[bd.off + i + k] = (uint8_t)(u >> (8 * k));
            out = vvoid(); return true;
        }
        if (m == "int64At") {
            long long i = argI(0);
            if (i < 0 || i + 8 > (long long)bd.len) return oob(i);
            uint64_t u = 0;
            for (int k = 0; k < 8; ++k) u |= (uint64_t)bytes[bd.off + i + k] << (8 * k);
            out = vint((long long)u); return true;
        }
        if (m == "setInt64") {
            long long i = argI(0), v = argI(1);
            if (i < 0 || i + 8 > (long long)bd.len) return oob(i);
            uint64_t u = (uint64_t)v;
            for (int k = 0; k < 8; ++k) bytes[bd.off + i + k] = (uint8_t)(u >> (8 * k));
            out = vvoid(); return true;
        }
        if (m == "fill") {
            long long o = argI(0), l = argI(1), v = argI(2);
            if (o < 0 || l < 0 || o > (long long)bd.len || l > (long long)bd.len - o) return oob(o);
            if (v < 0 || v > 255) {
                err = "byte value " + std::to_string(v) + " out of range 0..255";
                out = vvoid(); return true;
            }
            if (l) std::memset(bytes.data() + bd.off + (size_t)o, (int)v, (size_t)l);
            out = vvoid(); return true;
        }
        if (m == "blit") {
            long long d = argI(0), s = argI(2), l = argI(3);
            if (d < 0 || l < 0 || d > (long long)bd.len || l > (long long)bd.len - d) return oob(d);
            if (args.size() < 2 || !args[1].block || !args[1].block->bytes) {
                err = "null block"; out = vvoid(); return true;
            }
            BlockData& sb = *args[1].block;
            if (s < 0 || s > (long long)sb.len || l > (long long)sb.len - s) {
                err = "index " + std::to_string(s) + " out of bounds (length " +
                      std::to_string(sb.len) + ")";
                out = vvoid(); return true;
            }
            if (l) std::memmove(bytes.data() + bd.off + (size_t)d,
                                sb.bytes->data() + sb.off + (size_t)s, (size_t)l);
            out = vvoid(); return true;
        }
        if (m == "equals") {
            if (args.empty() || !args[0].block || !args[0].block->bytes) {
                err = "null block"; out = vvoid(); return true;
            }
            BlockData& ob = *args[0].block;
            bool equal = bd.len == ob.len &&
                (bd.len == 0 || std::memcmp(bytes.data() + bd.off,
                                             ob.bytes->data() + ob.off, bd.len) == 0);
            out = vbool(equal); return true;
        }
        if (m == "mismatch") {
            if (args.empty() || !args[0].block || !args[0].block->bytes) {
                err = "null block"; out = vvoid(); return true;
            }
            BlockData& ob = *args[0].block;
            if (bd.len != ob.len) {
                err = "Block.mismatch requires equal lengths"; out = vvoid(); return true;
            }
            long long from = argI(1);
            if (from < 0 || from > (long long)bd.len) return oob(from);
            size_t i = (size_t)from;
            while (i + 64 <= bd.len &&
                   std::memcmp(bytes.data() + bd.off + i, ob.bytes->data() + ob.off + i, 64) == 0) i += 64;
            while (i < bd.len && bytes[bd.off + i] == (*ob.bytes)[ob.off + i]) i++;
            out = vint(i == bd.len ? -1 : (long long)i); return true;
        }
    } else if (cls == "Array") {
        static const std::vector<Value> kEmpty;
        const std::vector<Value>& a = self.arr ? *self.arr : kEmpty;
        if (m == "length") { out = vint((long long)a.size()); return true; }
        if (m == "at") {
            long long i = args.empty() ? 0 : args[0].i;
            if (i < 0 || i >= (long long)a.size()) {
                err = "index " + std::to_string(i) + " out of bounds (length " +
                      std::to_string(a.size()) + ")";
                out = vvoid();
                return true;
            }
            out = a[(size_t)i];
            return true;
        }
        if (m == "add" && cowReceiver && self.arr) {
            // bug.md #15: the IR lowerer transferred a semantically unique
            // receiver into this call. Its interpreter marshaling creates
            // incidental shared_ptr copies, so use_count() here cannot express
            // that uniqueness; the pre-marshaling signal can. Mutating this
            // buffer is observationally the same pure result because the
            // caller cleared its source slot and no program-visible alias exists.
            if (!args.empty()) self.arr->push_back(args[0]);
            out = self;
            return true;
        }
        if (m == "add") {                                 // pure: returns a new array
            auto v = std::make_shared<std::vector<Value>>(a);
            if (!args.empty()) v->push_back(args[0]);
            Value r; r.kind = VKind::Array; r.arr = v; out = r; return true;
        }
        if (m == "concatAll") {                           // O(total): sum, reserve, append
            size_t total = 0;
            for (const Value& v : a) total += v.s.size();
            std::string r; r.reserve(total);
            for (const Value& v : a) r += v.s;
            out = vstr(std::move(r));
            return true;
        }
    } else if (cls == "Map") {
        static const std::vector<std::pair<Value, Value>> kNoEntries;
        const auto& entries = self.map ? *self.map : kNoEntries;
        if (m == "length") { out = vint((long long)entries.size()); return true; }
        if (m == "at") {
            for (const auto& [k, v] : entries)
                if (!args.empty() && keyEquals(k, args[0])) { out = v; return true; }
            err = "key not found: " + (args.empty() ? "<none>" : valueToString(args[0]));
            out = vvoid();
            return true;
        }
        if (m == "has") {
            bool found = false;
            for (const auto& [k, v] : entries)
                if (!args.empty() && keyEquals(k, args[0])) { found = true; break; }
            out = vbool(found); return true;
        }
        if (m == "with") {                                // pure add/update
            auto nv = std::make_shared<std::vector<std::pair<Value, Value>>>(entries);
            bool replaced = false;
            if (args.size() == 2)
                for (auto& [k, v] : *nv)
                    if (keyEquals(k, args[0])) { v = args[1]; replaced = true; break; }
            if (!replaced && args.size() == 2) nv->push_back({args[0], args[1]});
            Value r; r.kind = VKind::Map; r.map = nv; out = r; return true;
        }
        if (m == "without") {                             // pure remove
            auto nv = std::make_shared<std::vector<std::pair<Value, Value>>>();
            for (const auto& e : entries)
                if (args.empty() || !keyEquals(e.first, args[0])) nv->push_back(e);
            Value r; r.kind = VKind::Map; r.map = nv; out = r; return true;
        }
        if (m == "keys" || m == "values") {
            auto vec = std::make_shared<std::vector<Value>>();
            for (const auto& [k, v] : entries) vec->push_back(m == "keys" ? k : v);
            Value r; r.kind = VKind::Array; r.arr = vec; out = r; return true;
        }
    } else if (cls == "int") {
        if (m == "toString") { out = vstr(std::to_string(self.i)); return true; }
        if (m == "toFloat")  { out = vfloat((double)self.i); return true; }
    } else if (cls == "bool") {
        if (m == "toString") { out = vstr(self.b ? "true" : "false"); return true; }
    } else if (cls == "float") {
        if (m == "toString") { out = vstr(std::to_string(self.f)); return true; }
        if (m == "floor") { out = vfloat(std::floor(self.f)); return true; }
        if (m == "ceil")  { out = vfloat(std::ceil(self.f)); return true; }
        if (m == "round") { out = vfloat(std::round(self.f)); return true; }   // half-away-from-zero
        if (m == "trunc") { out = vfloat(std::trunc(self.f)); return true; }
        if (m == "sqrt")  { out = vfloat(std::sqrt(self.f)); return true; }    // negative -> NaN (IEEE)
        if (m == "pow")   { out = vfloat(std::pow(self.f, args.empty() ? 0.0 : args[0].f)); return true; }
        // struct-equality design §8: bit reinterpret (never a cast through
        // long — memcpy only, §3.2 warning); canonEq routes through the ONE
        // canon (lv_canon, RuntimeValue.hpp) — hash-consistency law §3.3.
        if (m == "bits") {
            long long b; std::memcpy(&b, &self.f, 8);
            out = vint(b); return true;
        }
        if (m == "canonEq") {
            out = vbool(lv_canon(self.f) == lv_canon(args.empty() ? 0.0 : args[0].f));
            return true;
        }
        if (m == "toInt") {
            // Truncation; NaN/±inf/out-of-int64-range -> RuntimeException
            // (loud, not UB — §1.2 of the design). int64 range as doubles:
            // [-2^63, 2^63) — the upper bound is exclusive since 2^63 itself
            // overflows int64 (max representable is 2^63-1).
            if (!std::isfinite(self.f) || self.f < -9223372036854775808.0 ||
                self.f >= 9223372036854775808.0) {
                err = "float is not finite or out of int64 range for toInt()";
                out = vvoid();
                return true;
            }
            out = vint((long long)self.f);
            return true;
        }
    }
    return false;
}

// ====================================================================
//  Track 10 — flatten/rebuild across a thread boundary (techdesign-threads-2
//  §4.4). On the single-heap interpreters "flatten then rebuild into the
//  receiver heap" is one deep copy: a fresh, independently-owned value graph.
//  A seen-map keyed on shared_ptr identity makes shared substructure and cycles
//  copy ONCE (a diamond in -> a diamond out; the walk terminates on cycles).
//  Values that cannot cross a boundary in v1 (§6) set `err` and the caller
//  raises a catchable RuntimeException naming the offending type. The two
//  engines share this walk, so their post-copy graphs — and thus all observable
//  output — are identical by construction (the acceptance-#5 differential).
// ====================================================================

// The known system fd-/loop-bound classes: an fd is bound to the loop that
// watches it, so it cannot be handed to another worker (each worker opens its
// own — the SO_REUSEPORT / share-nothing shape, §6). Detected by class name
// here (the interpreter backstop); the compiler's flattenable predicate is the
// type-directed front line.
static bool lvThreadNonTransferable(std::string_view cls) {
    return cls == "TcpStream" || cls == "TcpListener" || cls == "Timer" ||
           cls == "Process" || cls == "TcpConnector";
}

// Portal handles (§6): a Channel<T> names a two-thread conduit, not transferable
// data. It is *relocated*, not copied — on the single-heap interpreters that
// means shared (returned as-is) so a worker handed a handle feeds the SAME
// endpoint the peer does (R-4's whole point). (On LLVM the endpoint record lives
// in the transfer allocator; §3.4/§6.)
static bool lvThreadIsPortal(Symbol* cls) {
    if (!cls) return false;
    if (cls->name == "Channel") return true;
    if (cls->decl)
        for (const auto& base : cls->decl->bases)
            if (lvThreadIsPortal(base->resolvedSymbol)) return true;
    return false;
}

// A-1 (techdesign-threads-3 §1): a Worker<T>/Promise<T> handle may NOT cross a
// thread boundary. Promise.resolve invokes the stored continuation inline, so a
// worker resolving a spawner-owned promise would run a spawner-heap closure on
// the worker's thread against non-atomic refcounts — the exact D1' race. It
// stayed latent under M1 only because the serial engines have one thread; the
// rule is uniform across ALL engines so dev behavior never diverges from native.
static bool lvThreadIsPromiseDerived(Symbol* cls) {
    if (!cls) return false;
    if (cls->name == "Promise") return true;
    if (cls->decl)
        for (const auto& base : cls->decl->bases)
            if (lvThreadIsPromiseDerived(base->resolvedSymbol)) return true;
    return false;
}

// bug #81 (SU-1): an InStream is loop-bound *conditionally* — a plain in-memory
// stream is ordinary flattenable data and MUST still cross a boundary, but one
// carrying a producer-attached teardown (`hasDispose == true` — e.g. the
// `signal::on` subscription, whose `onDispose` reaches a live signalfd + loop
// watch via `signal::off`) is as loop-bound as a TcpStream/Timer and must
// reject. So it cannot go on the name-keyed `lvThreadNonTransferable` list;
// the gate is the object's own `hasDispose` field. (A disposable stream's
// `onDispose` closure would already trip the nested-closure reject with a
// generic message; this reject fires first and names the type, and stays
// correct even if the teardown is ever represented as something other than a
// closure.)
static bool lvThreadIsInStream(Symbol* cls) {
    if (!cls) return false;
    if (cls->name == "InStream") return true;
    if (cls->decl)
        for (const auto& base : cls->decl->bases)
            if (lvThreadIsInStream(base->resolvedSymbol)) return true;
    return false;
}
static bool lvThreadIsDisposableStream(const std::shared_ptr<Object>& obj) {
    if (!obj || !lvThreadIsInStream(obj->cls)) return false;
    auto it = obj->fields.find("hasDispose");
    return it != obj->fields.end() && it->second.kind == VKind::Bool && it->second.b;
}

// bug #35 (reject route A): the read-only mirror of lvThreadCopy's Promise
// reject, applied to a program global's CURRENT value at the spawn call (no
// copy). A spawn body may reference a Promise via a bare GLOBAL rather than a
// captured local; the capture walk (lvThreadCopy) only sees captures, so both
// interpreters re-scan the globals a spawn body references here. Scoped to
// Promise-derived objects reached through object fields / array elements / map
// entries — the #35 shape, matching the LLVM runtime's lvrt_value_has_promise.
// Channel portals are relocatable (shared) and skipped; closures/Blocks in a
// global are out of scope (nested lambdas of the BODY are walked by the
// engines' name analysis, not values sitting in a global). Cycle-safe via a
// pointer seen-set. Returns true + sets `err` (byte-identical to the capture
// reject) on the first hit.
static bool lvThreadValueHasPromiseRec(const Value& v, std::string& err,
                                       std::unordered_set<const void*>& seen) {
    switch (v.kind) {
        case VKind::Object: {
            if (!v.obj) return false;
            if (v.obj->cls && lvThreadIsPortal(v.obj->cls)) return false;   // Channel: relocatable
            if (v.obj->cls && lvThreadIsPromiseDerived(v.obj->cls)) {
                err = "value of type " + std::string(v.obj->cls->name) +
                      " cannot cross a thread boundary (v1); pass a Channel";
                return true;
            }
            if (!seen.insert(v.obj.get()).second) return false;
            for (const auto& [k, fv] : v.obj->fields)
                if (lvThreadValueHasPromiseRec(fv, err, seen)) return true;
            return false;
        }
        case VKind::Array: {
            if (!v.arr) return false;
            if (!seen.insert(v.arr.get()).second) return false;
            for (const Value& el : *v.arr)
                if (lvThreadValueHasPromiseRec(el, err, seen)) return true;
            return false;
        }
        case VKind::Map: {
            if (!v.map) return false;
            if (!seen.insert(v.map.get()).second) return false;
            for (const auto& [k, val] : *v.map) {
                if (lvThreadValueHasPromiseRec(k, err, seen)) return true;
                if (lvThreadValueHasPromiseRec(val, err, seen)) return true;
            }
            return false;
        }
        default:
            return false;   // immediates, strings, closures, blocks: out of scope
    }
}
bool lvThreadValueHasPromise(const Value& v, std::string& err) {
    std::unordered_set<const void*> seen;
    return lvThreadValueHasPromiseRec(v, err, seen);
}

// bug #3: collect every identifier a lambda body references (a conservative
// SUPERSET of its free variables — member names and shadowing binders are
// harmlessly included since they simply won't match an env slot). Used to copy
// ONLY the referenced captures across a thread boundary, matching the LLVM
// backend's minimal capture list, so an in-scope-but-UNREFERENCED
// non-flattenable local (a Worker/Timer/Block/nested closure) no longer forces
// the spawn body to reject. A referenced non-flattenable still rejects (its
// name IS collected -> it IS copied -> the flatten walk hits it), keeping the
// interpreters' behavior identical to the LLVM backend's.
static void lvCollectStmtNames(const Stmt* s, std::unordered_set<std::string_view>& names);
static void lvCollectExprNames(const Expr* e, std::unordered_set<std::string_view>& names) {
    if (!e) return;
    if (e->kind == ExprKind::Name || e->kind == ExprKind::Member)
        if (!e->text.empty()) names.insert(e->text);
    lvCollectExprNames(e->a.get(), names);
    lvCollectExprNames(e->b.get(), names);
    lvCollectExprNames(e->c.get(), names);
    for (const ExprPtr& x : e->list) lvCollectExprNames(x.get(), names);
    for (const MatchArm& arm : e->arms) {
        lvCollectExprNames(arm.value.get(), names);
        lvCollectExprNames(arm.bodyExpr.get(), names);
        lvCollectStmtNames(arm.bodyBlock.get(), names);
    }
    for (const Param& p : e->params) lvCollectExprNames(p.defaultValue.get(), names);
    lvCollectStmtNames(e->block.get(), names);
}
static void lvCollectStmtNames(const Stmt* s, std::unordered_set<std::string_view>& names) {
    if (!s) return;
    lvCollectExprNames(s->expr.get(), names);
    lvCollectExprNames(s->init.get(), names);
    lvCollectExprNames(s->forStep.get(), names);
    for (const StmtPtr& b : s->body) lvCollectStmtNames(b.get(), names);
    lvCollectStmtNames(s->thenBranch.get(), names);
    lvCollectStmtNames(s->elseBranch.get(), names);
    lvCollectStmtNames(s->forInit.get(), names);
    lvCollectStmtNames(s->memberBody.get(), names);
    for (const CatchClause& c : s->catches) lvCollectStmtNames(c.body.get(), names);
}

static Value lvThreadCopy(const Value& v, bool rootClosureOk,
                          std::unordered_map<const void*, Value>& seen,
                          std::string& err) {
    if (!err.empty()) return Value{};
    switch (v.kind) {
        case VKind::Void: case VKind::Int: case VKind::Float: case VKind::Bool:
        case VKind::String: case VKind::Char: case VKind::None:
            return v;                       // immediate / by-value: already deep
        case VKind::Object: {
            if (!v.obj) return v;
            if (v.obj->cls && lvThreadIsPortal(v.obj->cls))
                return v;                   // Channel portal: relocate (share), don't copy
            if (v.obj->cls && lvThreadIsPromiseDerived(v.obj->cls)) {
                err = "value of type " + std::string(v.obj->cls->name) +
                      " cannot cross a thread boundary (v1); pass a Channel";
                return Value{};
            }
            if (v.obj->cls && lvThreadNonTransferable(v.obj->cls->name)) {
                err = "value of type " + std::string(v.obj->cls->name) +
                      " cannot cross a thread boundary (v1)";
                return Value{};
            }
            if (lvThreadIsDisposableStream(v.obj)) {         // bug #81
                err = "value of type " + std::string(v.obj->cls->name) +
                      " cannot cross a thread boundary (v1)";
                return Value{};
            }
            auto it = seen.find(v.obj.get());
            if (it != seen.end()) return it->second;
            auto o = std::make_shared<Object>();
            o->cls = v.obj->cls;
            Value r = v; r.obj = o;
            seen[v.obj.get()] = r;          // register BEFORE recursing (cycles)
            for (const auto& [k, fv] : v.obj->fields) {
                o->fields[k] = lvThreadCopy(fv, false, seen, err);
                if (!err.empty()) return Value{};
            }
            return r;
        }
        case VKind::Array: {
            if (!v.arr) return v;
            auto it = seen.find(v.arr.get());
            if (it != seen.end()) return it->second;
            auto a = std::make_shared<std::vector<Value>>();
            Value r = v; r.arr = a;
            seen[v.arr.get()] = r;
            a->reserve(v.arr->size());
            for (const Value& el : *v.arr) {
                a->push_back(lvThreadCopy(el, false, seen, err));
                if (!err.empty()) return Value{};
            }
            return r;
        }
        case VKind::Map: {
            if (!v.map) return v;
            auto it = seen.find(v.map.get());
            if (it != seen.end()) return it->second;
            auto m = std::make_shared<std::vector<std::pair<Value, Value>>>();
            Value r = v; r.map = m;
            seen[v.map.get()] = r;
            m->reserve(v.map->size());
            for (const auto& [k, val] : *v.map) {
                Value ck = lvThreadCopy(k, false, seen, err);
                if (!err.empty()) return Value{};
                Value cv = lvThreadCopy(val, false, seen, err);
                if (!err.empty()) return Value{};
                m->push_back({ck, cv});
            }
            return r;
        }
        case VKind::Closure: {
            if (!v.closure) return v;
            // Only the spawn body itself may cross as a closure (§6): a captured
            // closure could reach arbitrarily far (loop registries, sockets), so
            // flattening one is a covert graph export.
            if (!rootClosureOk) {
                err = "a closure cannot cross a thread boundary (v1)";
                return Value{};
            }
            auto it = seen.find(v.closure.get());
            if (it != seen.end()) return it->second;
            auto c = std::make_shared<Closure>();
            c->lambda = v.closure->lambda;        // AST is immutable — shared
            c->thisClass = v.closure->thisClass;
            Value r = v; r.closure = c;
            seen[v.closure.get()] = r;
            // Snapshot the captured environment — this is acceptance #1: a
            // mutation of the captured original after spawn is not observed,
            // because the worker runs against this copy. bug #3: copy only the
            // variables the body actually references (its free set), so an
            // unreferenced non-flattenable local merely in lexical scope does
            // not force a reject — matching the LLVM backend's minimal capture.
            // Only the tree-walk oracle keeps the source-AST lambda on the
            // closure; the IR interpreter's closures carry a lowered fnIndex
            // (lambda == nullptr) whose captures are already the minimal set the
            // IR builder pinned, so the free-name filter only applies — and is
            // only sound — when the AST body is available.
            std::unordered_set<std::string_view> freeNames;
            bool haveFree = v.closure->lambda != nullptr;
            if (haveFree) lvCollectExprNames(v.closure->lambda, freeNames);
            for (const auto& scope : v.closure->env) {
                c->env.emplace_back();
                for (const auto& [nm, cv] : scope) {
                    if (haveFree && !freeNames.count(nm)) continue;
                    Value copied = lvThreadCopy(cv, false, seen, err);
                    if (!err.empty()) return Value{};
                    c->env.back()[nm] = copied;
                }
            }
            if (v.closure->thisObj) {
                Value tv; tv.kind = VKind::Object; tv.obj = v.closure->thisObj;
                Value ct = lvThreadCopy(tv, false, seen, err);
                if (!err.empty()) return Value{};
                c->thisObj = ct.obj;
            }
            return r;
        }
        case VKind::Block:
            // A Block is a mutable byte buffer / aliasing view, loop- and
            // fd-bound in practice; it does not cross a boundary in v1 (§6).
            err = "a Block cannot cross a thread boundary (v1)";
            return Value{};
        case VKind::Ast:
            err = "an Ast value is compile-time-only";
            return Value{};
    }
    return v;
}

bool nativeFreeCall(const std::string& name, std::vector<Value>& args, Value& out,
                    std::string& err, std::string* sink) {
    // Track 10: the one boundary-crossing intrinsic behind prelude `spawn`/join.
    // `sysThreadTransfer(v)` is the flatten/rebuild walk above — on the
    // interpreters, one deep copy on the single heap. It serves every crossing
    // (§4.4's "one mechanism, four call sites"): the spawn-body capture, the
    // join result, and (M4) a channel message. The root value may be the spawn
    // body closure; a closure nested deeper in the graph is rejected (§6).
    if (name == "sysThreadTransfer") {
        std::unordered_map<const void*, Value> seen;
        out = lvThreadCopy(args.empty() ? Value{} : args[0], /*rootClosureOk=*/true, seen, err);
        return true;
    }
    // Track 10 (techdesign-threads-3 §2): the capability-probe seam. On the
    // serial reference engines there are no true threads, so sysThreadStart is a
    // pure probe that returns -1 — the prelude `spawn` then takes the cooperative
    // loop-task leg. sysThreadResult is never reached here (the -1 path registers
    // no watch); it exists only so the true-thread `else` branch of `spawn`
    // lowers on the IR interpreter. Both are the real work only on LLVM (§5).
    if (name == "sysThreadStart") { out = vint(-1); return true; }
    if (name == "sysThreadResult") { out = vvoid(); return true; }
    // LA-30 B2 (doc 06 §4): the id-only task natives — pure lv_task registry
    // calls, engine-agnostic, shared by both interpreters. The parking/spawning
    // trio (sysTaskRun/sysTaskJoinAll/sysAwaitAny2) needs the engine (closure
    // spawn + S3/G7 park discipline) and is intercepted engine-side BEFORE this
    // dispatcher. Under LANG_PUMP the registry is empty and there is no current
    // task, so both are honest no-ops here (the trio raises there instead).
    if (name == "sysTaskCancel") {
        out = vint(args.empty() ? 0 : lv_task_cancel((uint64_t)args[0].i));
        return true;
    }
    if (name == "sysTaskShield") {
        lv_task_shield(args.empty() ? 0 : (int)args[0].i);
        out = vint(0);
        return true;
    }
    // Track 10 M3d: Channel<T> native ring is the LLVM leg. On the serial engines
    // sysChannelNew returns -1 (the probe: no native ring here), so the prelude
    // Channel runs its landed in-process queue and never calls send/receive/close
    // (those exist only so the id>=0 branch lowers on the IR interpreter).
    if (name == "sysChannelNew") { out = vint(-1); return true; }
    if (name == "sysChannelSend") { out = vint(0); return true; }
    if (name == "sysChannelReceive") { out = vnone(); return true; }
    if (name == "sysChannelClose") { out = vint(0); return true; }
    // Track 06 std::math transcendentals: libm, full precision, every engine
    // that isn't the zero-dep ELF backend (which defers this group with a
    // diagnostic — see X64Gen.cpp/LlvmGen.cpp's CallDyn/CallNativeFn handling).
    if (name == "log")    { out = vfloat(std::log(args.empty() ? 0.0 : args[0].f)); return true; }
    if (name == "log2")   { out = vfloat(std::log2(args.empty() ? 0.0 : args[0].f)); return true; }
    if (name == "exp")    { out = vfloat(std::exp(args.empty() ? 0.0 : args[0].f)); return true; }
    if (name == "sin")    { out = vfloat(std::sin(args.empty() ? 0.0 : args[0].f)); return true; }
    if (name == "cos")    { out = vfloat(std::cos(args.empty() ? 0.0 : args[0].f)); return true; }
    if (name == "tan")    { out = vfloat(std::tan(args.empty() ? 0.0 : args[0].f)); return true; }
    if (name == "atan2")  {
        out = vfloat(std::atan2(args.empty() ? 0.0 : args[0].f,
                                args.size() > 1 ? args[1].f : 0.0));
        return true;
    }
    if (name == "byteToString") {
        long long b = args.empty() ? 0 : args[0].i;
        if (b < 0 || b > 255) {
            err = "byte value " + std::to_string(b) + " out of range 0..255";
            out = vvoid();
            return true;
        }
        out = vstr(std::string(1, (char)(unsigned char)b));
        return true;
    }
    if (name == "floatFromBits") {
        // struct-equality design §8: int64 bits -> double, memcpy only (no
        // cast-through-long UB). Surface spelling `float::fromBits(int)` routes
        // here via the checker (mirrors Enum::fromCode); `math::floatFromBits`
        // is the same native reachable directly.
        long long b = args.empty() ? 0 : args[0].i;
        double d; std::memcpy(&d, &b, 8);
        out = vfloat(d);
        return true;
    }
    if (name == "charFromCode") {
        long long c = args.empty() ? 0 : args[0].i;
        if (c < 0 || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
            err = "code point " + std::to_string(c) +
                  " is not a valid Unicode scalar (0..0x10FFFF minus surrogates)";
            out = vvoid();
            return true;
        }
        out = vchar(c);
        return true;
    }
    if (name == "sysWrite") {
        long long fd = args.size() > 0 ? args[0].i : 1;
        if (args.size() > 1 && args[1].kind == VKind::Block) {   // Track 03 M4: (fd, b, off, len)
            if (!args[1].block || !args[1].block->bytes) { err = "null block"; out = vvoid(); return true; }
            BlockData& bd = *args[1].block;
            long long off = args.size() > 2 ? args[2].i : 0;
            long long len = args.size() > 3 ? args[3].i : 0;
            if (off < 0 || len < 0 || off + len > (long long)bd.len) {
                err = "block window [" + std::to_string(off) + "," + std::to_string(off + len) +
                      ") out of bounds (length " + std::to_string(bd.len) + ")";
                out = vvoid(); return true;
            }
            const char* p = (const char*)bd.bytes->data() + bd.off + off;
            if ((fd == 1 || fd == 2) && sink) interpEmitStdout(*sink, p, (size_t)len);
            else { ssize_t n = write((int)fd, p, (size_t)len); (void)n; }
            out = vint(len); return true;
        }
        const std::string& data = args.size() > 1 ? args[1].s : out.s;
        if ((fd == 1 || fd == 2) && sink) {
            interpEmitStdout(*sink, data);     // engine captures — unless raw mode
        } else {
            ssize_t n = write((int)fd, data.data(), data.size());
            (void)n;
        }
        out = vint((long long)data.size());
        return true;
    }
    if (name == "sysReadLine") {
        long long fd = args.empty() ? 0 : args[0].i;
        std::string line;
        if (fd == 0) {
            if (!std::getline(std::cin, line)) line = "";
        } else {
            char c;
            while (::read((int)fd, &c, 1) == 1 && c != '\n') line.push_back(c);
        }
        out = vstr(line);                             // "" = end of input (interim)
        return true;
    }
    if (name == "sysOpen") {
        const std::string& path = args.size() > 0 ? args[0].s : out.s;
        long long bits = args.size() > 1 ? args[1].i : 1;
        int flags;
        bool rd = bits & 1, wr = bits & 2, ap = bits & 4;
        if (rd && wr) flags = O_RDWR | O_CREAT;
        else if (wr) flags = O_WRONLY | O_CREAT | O_TRUNC;
        else flags = O_RDONLY;
        if (ap) flags = (flags & ~O_TRUNC) | O_APPEND | O_CREAT | (rd ? O_RDWR : O_WRONLY);
        out = vint(::open(path.c_str(), flags, 0644));
        return true;
    }
    if (name == "sysClose") {
        // TLS session (if any) first: one best-effort close_notify + free, table
        // slot cleared BEFORE close(2) so fd-number reuse can't leak it (§10 #8).
        // No-op on a plaintext fd. This is TcpStream.close()'s single funnel.
        int fd = (int)(args.empty() ? -1 : args[0].i);
        tlsClose(fd);
        out = vint(::close(fd));
        return true;
    }
    // Track 08 F2 (contract C6): wall-clock epoch milliseconds. Track 09
    // DateTime.now() is its one consumer today. CLOCK_REALTIME per the design.
    if (name == "sysNow") {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        out = vint((long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
        return true;
    }
    if (name == "sysTimerStart") {
        long long delay = args.size() > 0 ? args[0].i : 0;
        long long interval = args.size() > 1 ? args[1].i : 0;
        Value cb = args.size() > 2 ? args[2] : Value{};
        out = vint(RuntimeLoop::instance().addTimer(delay, interval, std::move(cb)));
        return true;
    }
    if (name == "sysTimerCancel") {
        RuntimeLoop::instance().cancelTimer(args.empty() ? -1 : args[0].i);
        out = vint(0);
        return true;
    }
    if (name == "sysStat") {
        // field: 0 = exists (0/1), 1 = size in bytes, 2 = mtime (epoch seconds),
        // 3 = isDir (1 dir / 0 not-dir / -1 missing, request-stat-isdir.md).
        // Returns -1 for size/mtime/isDir of a missing path. Interim shape
        // until a Block-backed stat can fill a real structure.
        const std::string& path = args.size() > 0 ? args[0].s : out.s;
        long long field = args.size() > 1 ? args[1].i : 0;
        struct stat st;
        bool ok = ::stat(path.c_str(), &st) == 0;
        if (field == 0) out = vint(ok ? 1 : 0);
        else if (!ok) out = vint(-1);
        else if (field == 1) out = vint((long long)st.st_size);
        else if (field == 2) out = vint((long long)st.st_mtime);
        else if (field == 3) out = vint(S_ISDIR(st.st_mode) ? 1 : 0);
        else out = vint(-1);
        return true;
    }
    if (name == "sysArgs") {
        // The process argv (designs/argv.md §5.2): argv[0] + the `--` tail the
        // driver stashed. Bytes verbatim (strings are byte strings). Length is
        // always >= 1. Compiled backends bypass this; here it's the registry.
        std::vector<Value> out_args;
        out_args.reserve(g_programArgs.size());
        for (const std::string& a : g_programArgs) out_args.push_back(vstr(a));
        out = varr(std::move(out_args));
        return true;
    }
    if (name == "sysTermRaw") {
        out = vint(termRaw(args.empty() ? 0 : (int)args[0].i));
        return true;
    }
    if (name == "sysTermRestore") {
        (void)args;                              // fd advisory; the saved fd is restored
        out = vint(termRestoreRc());
        return true;
    }
    if (name == "sysWinSize") {                  // §2: (fd, field) -> rows|cols|-1
        int fd = (int)(args.size() > 0 ? args[0].i : 1);
        long long field = args.size() > 1 ? args[1].i : 0;
        struct winsize ws;
        if (::ioctl(fd, TIOCGWINSZ, &ws) != 0 || ws.ws_row == 0 || ws.ws_col == 0)
            out = vint(-1);
        else
            out = vint(field == 0 ? ws.ws_row : (field == 1 ? ws.ws_col : -1));
        return true;
    }
    if (name == "sysTermIsRaw") {                // §2: raw-state query (fallback guard)
        (void)args;                              // fd advisory; g_ttyRaw is authoritative
        out = vbool(g_ttyRaw);
        return true;
    }
    if (name == "sysSignalOpen") {               // §3: Array<int> -> readable fd
        std::vector<long long> sigs;
        if (!args.empty() && args[0].kind == VKind::Array && args[0].arr)
            for (const Value& v : *args[0].arr) sigs.push_back(v.i);
        out = vint(interpSignalOpen(sigs));
        return true;
    }
    if (name == "sysSignalNext") {               // §3: fd -> signo, -1 = none now
        out = vint(interpSignalNext((int)(args.empty() ? -1 : args[0].i)));
        return true;
    }
    if (name == "sysSignalClose") {              // §3: close + unblock
        interpSignalClose((int)(args.empty() ? -1 : args[0].i));
        out = vint(0);
        return true;
    }
    if (name == "sysSetExitCode") {
        // Record the code for normal completion (designs/exit-codes.md §4). Does
        // NOT stop execution — the loop drains, then run() returns this code.
        // 8-bit masked so every engine agrees (§5).
        g_interpExitCode = (int)((args.empty() ? 0 : args[0].i) & 0xFF);
        out = vint(0);
        return true;
    }
    if (name == "sysExit") {
        // Exit now (§4): record code + request exit. No raw _exit (would drop
        // the captured sink); the engine unwinds cleanly on seeing the flag.
        g_interpExitCode = (int)((args.empty() ? 0 : args[0].i) & 0xFF);
        g_interpExitRequested = true;
        out = vint(0);
        return true;
    }
    if (name == "sysRead") {
        long long fd = args.empty() ? 0 : args[0].i;
        if (args.size() > 1 && args[1].kind == VKind::Block) {   // Track 03 M4: (fd, b, max) -> int
            if (!args[1].block || !args[1].block->bytes) { err = "null block"; out = vvoid(); return true; }
            BlockData& bd = *args[1].block;
            long long max = args.size() > 2 ? args[2].i : 0;
            if (max < 0) max = 0;
            if (max > (long long)bd.len) max = (long long)bd.len;
            char* p = (char*)bd.bytes->data() + bd.off;
            ssize_t n = max > 0 ? ::read((int)fd, p, (size_t)max) : 0;
            out = vint(n > 0 ? (long long)n : 0); return true;
        }
        long long max = args.size() > 1 ? args[1].i : 0;
        std::string buf(max > 0 ? (size_t)max : 0, '\0');
        ssize_t n = max > 0 ? ::read((int)fd, buf.data(), (size_t)max) : 0;
        buf.resize(n > 0 ? (size_t)n : 0);
        out = vstr(buf);
        return true;
    }

    // --- sockets (the network is just more streams) ---------------------------
    if (name == "sysTcpConnect") {
        // IPv6 when the host literal contains ':' (F5.5 — fillSockAddr).
        const std::string& host = args.size() > 0 ? args[0].s : std::string();
        long long port = args.size() > 1 ? args[1].i : 0;
        sockaddr_storage ss{}; socklen_t slen = 0;
        if (!fillSockAddr(host, port, ss, slen)) { out = vint(-1); return true; }
        int fd = ::socket(((sockaddr*)&ss)->sa_family, SOCK_STREAM, 0);
        if (fd < 0) { out = vint(-1); return true; }
        if (::connect(fd, (sockaddr*)&ss, slen) < 0) {
            ::close(fd); out = vint(-1); return true;
        }
        int fl = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);      // response read via the loop
        out = vint(fd);
        return true;
    }
    // Track 08 F5: the connect-timeout floor. Non-blocking socket, connect
    // returns immediately — the fd is handed back even mid-handshake
    // (EINPROGRESS); -1 only on an immediate failure (bad literal, no fds).
    // Completion is observed via sysWatchWrite + sysConnectResult; the prelude
    // connectTimeout composes the three with a Timer, entirely in-language.
    if (name == "sysTcpConnectNb") {
        const std::string& host = args.size() > 0 ? args[0].s : std::string();
        long long port = args.size() > 1 ? args[1].i : 0;
        sockaddr_storage ss{}; socklen_t slen = 0;
        if (!fillSockAddr(host, port, ss, slen)) { out = vint(-1); return true; }
        int fd = ::socket(((sockaddr*)&ss)->sa_family, SOCK_STREAM, 0);
        if (fd < 0) { out = vint(-1); return true; }
        int fl = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);       // BEFORE connect — the point
        if (::connect(fd, (sockaddr*)&ss, slen) < 0 && errno != EINPROGRESS) {
            ::close(fd); out = vint(-1); return true;
        }
        out = vint(fd);
        return true;
    }
    // Post-writability verdict: getsockopt(SO_ERROR). 0 = connected; else the
    // errno-shaped failure (ECONNREFUSED etc.); -1 if the query itself failed.
    if (name == "sysConnectResult") {
        int fd = (int)(args.empty() ? -1 : args[0].i);
        int soerr = 0; socklen_t sl = sizeof(soerr);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0) soerr = -1;
        out = vint(soerr);
        return true;
    }
    if (name == "sysTcpListen") {
        long long port = args.empty() ? 0 : args[0].i;
        bool reusePort = args.size() > 1 && args[1].i != 0;
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { out = vint(-1); return true; }
        int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
        if (reusePort && ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)) != 0) {
            ::close(fd); out = vint(-1); return true;
        }
#else
        if (reusePort) { ::close(fd); out = vint(-1); return true; }
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons((uint16_t)port);
        if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0 || ::listen(fd, 16) < 0) {
            ::close(fd); out = vint(-1); return true;
        }
        int fl = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        out = vint(fd);
        return true;
    }
    // LA-29: advisory SO_SNDBUF/SO_RCVBUF sizing. A non-positive direction is
    // left unchanged; returns 0 if every requested direction applied, -1 if a
    // requested setsockopt failed.
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
    if (name == "sysCpuCount") {
        long n = ::sysconf(_SC_NPROCESSORS_ONLN);
        out = vint(n > 0 ? n : 1);
        return true;
    }
    if (name == "sysAccept") {
        int lfd = (int)(args.empty() ? -1 : args[0].i);
        int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd >= 0) {
            int fl = ::fcntl(cfd, F_GETFL, 0);
            ::fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
        }
        out = vint(cfd);                            // -1 if none pending
        return true;
    }
    if (name == "sysSend") {
        // Bytes written; -1 = retryable (would-block/EINTR, nothing written);
        // -2 = fatal (peer/pipe gone). The -1/-2 split is what the prelude
        // queue-and-drain needs — a write-watch keeps firing on a dead fd, so
        // "retry" and "give up" must be distinguishable at the floor. F7 rider:
        // ENOTSOCK falls back to write(2) — a Process stdin pipe takes the
        // same call (pipes are fds; SIGPIPE is ignored at spawn, EPIPE -> -2).
        int fd = (int)(args.size() > 0 ? args[0].i : -1);
        if (args.size() > 1 && args[1].kind == VKind::Block) {   // Track 03 M4: (fd, b, off, len)
            if (!args[1].block || !args[1].block->bytes) { err = "null block"; out = vvoid(); return true; }
            BlockData& bd = *args[1].block;
            long long off = args.size() > 2 ? args[2].i : 0;
            long long len = args.size() > 3 ? args[3].i : 0;
            if (off < 0 || len < 0 || off + len > (long long)bd.len) {
                err = "block window [" + std::to_string(off) + "," + std::to_string(off + len) +
                      ") out of bounds (length " + std::to_string(bd.len) + ")";
                out = vvoid(); return true;
            }
            const char* p = (const char*)bd.bytes->data() + bd.off + off;
            if (tlsIsFd(fd)) { out = vint((long long)tlsSend(fd, p, (long)len)); return true; }
            ssize_t bn = ::send(fd, p, (size_t)len, MSG_NOSIGNAL);
            if (bn < 0 && errno == ENOTSOCK) bn = ::write(fd, p, (size_t)len);
            if (bn < 0) bn = (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? -1 : -2;
            out = vint((long long)bn); return true;
        }
        const std::string& data = args.size() > 1 ? args[1].s : std::string();
        if (tlsIsFd(fd)) { out = vint((long long)tlsSend(fd, data.data(), (long)data.size())); return true; }
        ssize_t n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        if (n < 0 && errno == ENOTSOCK)
            n = ::write(fd, data.data(), data.size());
        if (n < 0)
            n = (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? -1 : -2;
        out = vint((long long)n);
        return true;
    }
    if (name == "sysRecv") {
        // string? : None = peer closed (EOF); "" = nothing available now
        // (EAGAIN); non-empty = data. The three states narrowing was built for.
        // ENOTSOCK falls back to read(2) with identical semantics — Process
        // stdout/stderr pipes ride the same three-state recv (F7).
        int fd = (int)(args.size() > 0 ? args[0].i : -1);
        if (args.size() > 1 && args[1].kind == VKind::Block) {   // Track 03 M4: (fd, b, max) -> int?
            if (!args[1].block || !args[1].block->bytes) { err = "null block"; out = vvoid(); return true; }
            BlockData& bd = *args[1].block;
            long long bmax = args.size() > 2 ? args[2].i : (long long)bd.len;
            if (bmax < 0) bmax = 0;
            if (bmax > (long long)bd.len) bmax = (long long)bd.len;
            char* p = (char*)bd.bytes->data() + bd.off;
            if (tlsIsFd(fd)) {
                long r = tlsRecv(fd, p, (long)bmax);
                if (r > 0) out = vint(r);                        // data
                else if (r == -1) out = vint(0);                // would-block -> 0
                else out = vnone();                             // clean-EOF / fatal -> None
                return true;
            }
            ssize_t bn = ::recv(fd, p, (size_t)bmax, 0);
            if (bn < 0 && errno == ENOTSOCK) {
                bn = ::read(fd, p, (size_t)bmax);
                // pty master, child gone: Linux says -1/EIO where macOS/BSD say
                // 0 — both ARE the orderly close (CPython bpo-26228; designs/
                // pty/ D-P4). Without this arm the read-watch busy-spins
                // forever on a dead pty.
                if (bn < 0 && errno == EIO) bn = 0;
            }
            if (bn == 0) { out = vnone(); return true; }         // orderly close -> None
            if (bn < 0) { out = vint(0); return true; }          // would-block -> 0
            out = vint((long long)bn); return true;
        }
        long long max = args.size() > 1 ? args[1].i : 4096;
        std::string buf(max > 0 ? (size_t)max : 4096, '\0');
        if (tlsIsFd(fd)) {
            long r = tlsRecv(fd, buf.data(), (long)buf.size());
            if (r > 0) { buf.resize((size_t)r); out = vstr(buf); }  // data
            else if (r == -1) out = vstr("");                      // would-block
            else out = vnone();                                    // clean-EOF / fatal
            return true;
        }
        ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n < 0 && errno == ENOTSOCK) {
            n = ::read(fd, buf.data(), buf.size());
            // pty master, child gone: Linux says -1/EIO where macOS/BSD say 0 — both
            // ARE the orderly close (CPython bpo-26228; designs/pty/ D-P4). Without
            // this arm the read-watch busy-spins forever on a dead pty.
            if (n < 0 && errno == EIO) n = 0;
        }
        if (n == 0) { out = vnone(); return true; }             // orderly close
        if (n < 0) { out = vstr(""); return true; }             // would-block
        buf.resize((size_t)n);
        out = vstr(buf);
        return true;
    }
    if (name == "sysWatch") {                        // register an fd read-watch
        int fd = (int)(args.size() > 0 ? args[0].i : -1);
        Value cb = args.size() > 1 ? args[1] : Value{};
        out = vint(RuntimeLoop::instance().addWatch(fd, std::move(cb)));
        return true;
    }
    if (name == "sysWatchWrite") {                   // register an fd write-watch
        int fd = (int)(args.size() > 0 ? args[0].i : -1);
        Value cb = args.size() > 1 ? args[1] : Value{};
        out = vint(RuntimeLoop::instance().addWriteWatch(fd, std::move(cb)));
        return true;
    }
    if (name == "sysUnwatch") {
        RuntimeLoop::instance().cancelWatch(args.empty() ? -1 : args[0].i);
        out = vint(0);
        return true;
    }

    // --- TLS + crypto (LA-2, techdesign-tls-crypto.md §3) --------------------
    // The fd IS the socket fd (wrap-in-place): these arm/step/query the session
    // side-table; send/recv/close routing lives in those cases below. Without
    // OpenSSL each returns the clean not-built result (a program that never
    // touches TLS is unaffected — §10 #10).
    if (name == "sysTlsConnect") {
#ifdef HAVE_OPENSSL
        int fd = (int)(args.size() > 0 ? args[0].i : -1);
        const std::string& host = args.size() > 1 ? args[1].s : std::string();
        const std::string& alpn = args.size() > 2 ? args[2].s : std::string();
        const std::string& caf  = args.size() > 3 ? args[3].s : std::string();
        int vm = (int)(args.size() > 4 ? args[4].i : 0);
        out = vint(tlsClientStart(fd, host, alpn, caf, vm));
#else
        (void)args; out = vint(-1);
#endif
        return true;
    }
    if (name == "sysTlsAccept") {
#ifdef HAVE_OPENSSL
        int fd = (int)(args.size() > 0 ? args[0].i : -1);
        const std::string& cert = args.size() > 1 ? args[1].s : std::string();
        const std::string& key  = args.size() > 2 ? args[2].s : std::string();
        const std::string& alpn = args.size() > 3 ? args[3].s : std::string();
        out = vint(tlsServerStart(fd, cert, key, alpn));
#else
        (void)args; out = vint(-1);
#endif
        return true;
    }
    if (name == "sysTlsHandshake") {
#ifdef HAVE_OPENSSL
        out = vint(tlsHandshake((int)(args.empty() ? -1 : args[0].i)));
#else
        (void)args; out = vint(-1);
#endif
        return true;
    }
    if (name == "sysTlsError") {
#ifdef HAVE_OPENSSL
        out = vstr(tlsError((int)(args.empty() ? -1 : args[0].i)));
#else
        (void)args; out = vstr(kTlsNotBuilt);
#endif
        return true;
    }
    if (name == "sysTlsAlpn") {
#ifdef HAVE_OPENSSL
        out = vstr(tlsAlpn((int)(args.empty() ? -1 : args[0].i)));
#else
        (void)args; out = vstr("");
#endif
        return true;
    }
    if (name == "sysTlsVersion") {
#ifdef HAVE_OPENSSL
        out = vint(tlsVersion((int)(args.empty() ? -1 : args[0].i)));
#else
        (void)args; out = vint(0);
#endif
        return true;
    }
    if (name == "sysRsaEncrypt") {
#ifdef HAVE_OPENSSL
        const std::string& pem = args.size() > 0 ? args[0].s : std::string();
        const std::string& in  = args.size() > 1 ? args[1].s : std::string();
        const std::string  pad = args.size() > 2 ? args[2].s : std::string("oaep");
        std::string ct, rerr;
        if (tlsRsaEncrypt(pem, in, pad == "pkcs1" ? 1 : 0, ct, rerr)) out = vstr(std::move(ct));
        else out = vnone();
#else
        (void)args; out = vnone();
#endif
        return true;
    }

    // --- Track 08 F2: monotonic clock + random ------------------------------
    // sysMonotonic: CLOCK_MONOTONIC milliseconds, for durations/deadlines
    // (never jumps on a wall-clock adjust). Siblings sysNow (wall clock) above.
    if (name == "sysMonotonic") {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        out = vint((long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
        return true;
    }
    // sysRandom(n): n cryptographically-random bytes carried in a string (the
    // sys-floor convention until a Block overload lands, Track 03 M4). n<=0 ->
    // "" (nothing requested); n>1MB -> RuntimeException (documented sanity
    // bound, design F2). getrandom loops on short reads / EINTR.
    if (name == "sysRandom") {
        long long n = args.empty() ? 0 : args[0].i;
        if (n <= 0) { out = vstr(""); return true; }
        if (n > (1LL << 20)) {
            err = "sysRandom: requested " + std::to_string(n) +
                  " bytes exceeds the 1MB sanity bound";
            out = vvoid(); return true;
        }
        std::string buf((size_t)n, '\0');
        size_t got = 0;
        while (got < (size_t)n) {
            ssize_t r = ::getrandom(&buf[got], (size_t)n - got, 0);
            if (r < 0) {
                if (errno == EINTR) continue;
                err = "sysRandom: getrandom failed";
                out = vvoid(); return true;
            }
            got += (size_t)r;
        }
        out = vstr(std::move(buf));
        return true;
    }

    // --- Track 08 F4: isatty -----------------------------------------------
    if (name == "sysIsTty") {
        long long fd = args.empty() ? 0 : args[0].i;
        out = vbool(::isatty((int)fd) == 1);
        return true;
    }

    // --- Track 08 F1: env lookup (argv/exit already landed) -----------------
    // sysEnv(key) -> string?  None = unset (distinct from set-but-empty, the
    // three-state fact §9's None exists for). Optional-return per the C2 recipe.
    if (name == "sysEnv") {
        const std::string& key = args.size() > 0 ? args[0].s : out.s;
        const char* v = ::getenv(key.c_str());
        out = v ? vstr(std::string(v)) : vnone();
        return true;
    }

    // --- Track 08 F3: dirs & fs metadata -----------------------------------
    // Scalar 0/-1 results, the sysStat/sysOpen shape. mkdir mode 0755.
    if (name == "sysMkdir") {
        const std::string& path = args.size() > 0 ? args[0].s : out.s;
        out = vint(::mkdir(path.c_str(), 0755) == 0 ? 0 : -1);
        return true;
    }
    // sysRemove: unlink a file; a directory reports EISDIR (EPERM on some
    // systems), so fall back to rmdir (design F3). -1 if neither succeeds.
    if (name == "sysRemove") {
        const std::string& path = args.size() > 0 ? args[0].s : out.s;
        if (::unlink(path.c_str()) == 0) { out = vint(0); return true; }
        if (errno == EISDIR || errno == EPERM) {
            out = vint(::rmdir(path.c_str()) == 0 ? 0 : -1); return true;
        }
        out = vint(-1);
        return true;
    }
    if (name == "sysRename") {
        const std::string& from = args.size() > 0 ? args[0].s : std::string();
        const std::string& to   = args.size() > 1 ? args[1].s : std::string();
        out = vint(::rename(from.c_str(), to.c_str()) == 0 ? 0 : -1);
        return true;
    }
    // sysListDir(path) -> Array<string>?  entry names, no "."/"..". None =
    // not a directory / can't open (distinguishable from an empty directory,
    // which is [] — the three-state fact, design F3).
    if (name == "sysListDir") {
        const std::string& path = args.size() > 0 ? args[0].s : out.s;
        DIR* d = ::opendir(path.c_str());
        if (!d) { out = vnone(); return true; }
        std::vector<Value> names;
        struct dirent* e;
        while ((e = ::readdir(d)) != nullptr) {
            std::string nm = e->d_name;
            if (nm == "." || nm == "..") continue;
            names.push_back(vstr(nm));
        }
        ::closedir(d);
        out = varr(std::move(names));
        return true;
    }

    // --- Track 08 F6 phase 1: DNS resolution -------------------------------
    // sysResolve(host) -> string?  first A record as a dotted-quad, or None on
    // failure. getaddrinfo (libc resolver) on every interpreter; the pure ELF
    // backend has no libc resolver and defers with a diagnostic (design F6 —
    // phase 2's in-language DNS client over a UDP floor closes that gap).
    if (name == "sysResolve") {
        const std::string& host = args.size() > 0 ? args[0].s : out.s;
        struct addrinfo hints{};
        hints.ai_family = AF_INET;                 // A record (IPv4), phase 1
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = nullptr;
        int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &res);
        if (rc != 0 || !res) {
            if (res) ::freeaddrinfo(res);
            out = vnone(); return true;
        }
        char ip[INET_ADDRSTRLEN] = {0};
        sockaddr_in* sa = (sockaddr_in*)res->ai_addr;
        ::inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
        ::freeaddrinfo(res);
        out = vstr(std::string(ip));
        return true;
    }

    // --- Track 08 F7: spawn (the agentic primitive) --------------------------
    // sysSpawn(path, args) -> Array<int> : [pid, stdinFd, stdoutFd, stderrFd],
    // or [] on SPAWN failure (pipes/fork — exec failure is not spawn failure:
    // the child _exit(127)s and the code arrives via sysReap, design §7).
    // No PATH search (explicit path — the honest v1 boundary). Child discipline
    // per problem #3: argv is built BEFORE fork; between fork and exec the
    // child only dup2s and execs, and a failed exec _exit(127)s immediately —
    // never returns into two live interpreters. Parent ends are O_CLOEXEC (a
    // later sibling won't inherit them) and non-blocking (reads ride the event
    // loop; stdin writes queue-and-drain instead of stalling it).
    if (name == "sysSpawn") {
        const std::string& path = args.size() > 0 ? args[0].s : std::string();
        std::vector<Value> fail;                     // [] = spawn failure
        if (path.empty()) { out = varr(std::move(fail)); return true; }
        // SIGPIPE off, once: a write to a pipe whose child died must surface
        // as EPIPE (-2 from sysSend), not kill the leviathan process. SIG_IGN
        // is a disposition, not a handler — the design's no-signal-handler
        // rule (§12) is about SIGCHLD-style reentrancy, which this is not.
        static bool sigpipeIgnored = false;
        if (!sigpipeIgnored) { ::signal(SIGPIPE, SIG_IGN); sigpipeIgnored = true; }
        std::vector<std::string> argvStore;
        argvStore.push_back(path);                   // argv[0] = the path
        if (args.size() > 1 && args[1].arr)
            for (const Value& a : *args[1].arr) argvStore.push_back(a.s);
        std::vector<char*> argv;
        argv.reserve(argvStore.size() + 1);
        for (std::string& s : argvStore) argv.push_back(s.data());
        argv.push_back(nullptr);
        int inP[2], outP[2], errP[2];
        if (::pipe2(inP, O_CLOEXEC) != 0) { out = varr(std::move(fail)); return true; }
        if (::pipe2(outP, O_CLOEXEC) != 0) {
            ::close(inP[0]); ::close(inP[1]);
            out = varr(std::move(fail)); return true;
        }
        if (::pipe2(errP, O_CLOEXEC) != 0) {
            ::close(inP[0]); ::close(inP[1]); ::close(outP[0]); ::close(outP[1]);
            out = varr(std::move(fail)); return true;
        }
        pid_t pid = ::fork();
        if (pid < 0) {
            ::close(inP[0]); ::close(inP[1]); ::close(outP[0]); ::close(outP[1]);
            ::close(errP[0]); ::close(errP[1]);
            out = varr(std::move(fail)); return true;
        }
        if (pid == 0) {
            // Child: dup2 clears O_CLOEXEC on the std fds; every other pipe
            // end closes itself at exec. Nothing here allocates.
            ::dup2(inP[0], 0);
            ::dup2(outP[1], 1);
            ::dup2(errP[1], 2);
            ::execve(path.c_str(), argv.data(), environ);
            ::_exit(127);
        }
        ::close(inP[0]); ::close(outP[1]); ::close(errP[1]);
        for (int fd : {inP[1], outP[0], errP[0]}) {
            int fl = ::fcntl(fd, F_GETFL, 0);
            ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        }
        std::vector<Value> r{vint(pid), vint(inP[1]), vint(outP[0]), vint(errP[0])};
        out = varr(std::move(r));
        return true;
    }
    // sysPidfdOpen(pid) -> a pollable fd that reads ready when the process
    // exits (pidfd_open, the design's no-SIGCHLD reaping route); -1 on failure.
    if (name == "sysPidfdOpen") {
        long long pid = args.empty() ? -1 : args[0].i;
        if (pid <= 0) { out = vint(-1); return true; }
        out = vint((long long)::syscall(SYS_pidfd_open, (pid_t)pid, 0));
        return true;
    }
    // sysReap(pid) -> the child's exit code via waitid(WNOHANG): -1 = still
    // running (or not our child); 0..255 = exited; 128+signal if terminated by
    // a signal (the shell convention). Never blocks — the loop stays live.
    if (name == "sysReap") {
        long long pid = args.empty() ? -1 : args[0].i;
        if (pid <= 0) { out = vint(-1); return true; }
        siginfo_t si;
        si.si_pid = 0;
        if (::waitid(P_PID, (id_t)pid, &si, WEXITED | WNOHANG) != 0 || si.si_pid == 0) {
            out = vint(-1); return true;
        }
        out = vint(si.si_code == CLD_EXITED ? (long long)(si.si_status & 0xFF)
                                            : 128 + (long long)si.si_status);
        return true;
    }
    // sysKill(pid, sig) -> 0/-1. pid <= 0 is refused at the floor: the kill(2)
    // group/broadcast forms (0, -1, -pgid) are never exposed to programs.
    if (name == "sysKill") {
        long long pid = args.size() > 0 ? args[0].i : -1;
        long long sig = args.size() > 1 ? args[1].i : SIGTERM;
        if (pid <= 0 || sig < 0) { out = vint(-1); return true; }
        out = vint(::kill((pid_t)pid, (int)sig) == 0 ? 0 : -1);
        return true;
    }

    // --- G-LANG-2 terminal half: pty floor (designs/pty/) ---------------------
    // sysPtySpawn(path, args, rows, cols, flags) -> [pid, masterFd] | [] on
    // failure. ONE master fd, read+write (a pty fuses stdout/stderr). flags bit0
    // = deterministic termios profile (D-P3). Child discipline is D-P2: ALL
    // non-async-signal-safe work (openpt/grant/unlock/ptsname/open/tcsetattr/
    // TIOCSWINSZ) happens here in the parent BEFORE fork; the child body is
    // setsid/TIOCSCTTY/dup2/close/execve/_exit only.
    if (name == "sysPtySpawn") {
        const std::string& path = args.size() > 0 ? args[0].s : std::string();
        long long rows = args.size() > 2 ? args[2].i : 0;
        long long cols = args.size() > 3 ? args[3].i : 0;
        long long flags = args.size() > 4 ? args[4].i : 0;
        std::vector<Value> fail;
        if (path.empty() || rows <= 0 || cols <= 0) { out = varr(std::move(fail)); return true; }
        static bool sigpipeIgnored2 = false;   // disposition, not a handler (F7 note)
        if (!sigpipeIgnored2) { ::signal(SIGPIPE, SIG_IGN); sigpipeIgnored2 = true; }

        std::vector<std::string> argvStore;              // argv BEFORE fork (D7/D-P2)
        argvStore.push_back(path);
        if (args.size() > 1 && args[1].arr)
            for (const Value& a : *args[1].arr) argvStore.push_back(a.s);
        std::vector<char*> argv;
        argv.reserve(argvStore.size() + 1);
        for (std::string& s : argvStore) argv.push_back(s.data());
        argv.push_back(nullptr);

        int mfd = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);   // atomic cloexec (R§A.6)
        if (mfd < 0) { out = varr(std::move(fail)); return true; }
        char sname[64];
        if (::grantpt(mfd) != 0 || ::unlockpt(mfd) != 0 ||
            ::ptsname_r(mfd, sname, sizeof sname) != 0) {
            ::close(mfd); out = varr(std::move(fail)); return true;
        }
        int sfd = ::open(sname, O_RDWR | O_NOCTTY);      // NOT cloexec: child keeps it past exec
        if (sfd < 0) { ::close(mfd); out = varr(std::move(fail)); return true; }

        // D-P3: stamp the frozen profile on the SLAVE, parent-side, pre-fork.
        struct termios tio;
        std::memset(&tio, 0, sizeof tio);
        tio.c_iflag = ICRNL | IXON;
        tio.c_oflag = OPOST | ONLCR;
        tio.c_cflag = CS8 | CREAD;
        tio.c_lflag = ISIG | ICANON | ECHOE | ECHOK | IEXTEN;
        if ((flags & 1) == 0) tio.c_lflag |= ECHO | ECHOE | ECHOK;  // DEFAULT profile
        else tio.c_lflag &= ~(tcflag_t)(ECHOE | ECHOK);             // DETERMINISTIC: echo family off
        tio.c_cc[VMIN] = 1; tio.c_cc[VTIME] = 0;
        // control chars: explicit seeds so the lanes never drift with libc
        // defaults; VEOF=4 freezes the documented write("\x04") EOF protocol.
        tio.c_cc[VEOF] = 4; tio.c_cc[VINTR] = 3; tio.c_cc[VSUSP] = 26;
        tio.c_cc[VERASE] = 0x7f; tio.c_cc[VKILL] = 21; tio.c_cc[VQUIT] = 28;
        ::cfsetispeed(&tio, B38400); ::cfsetospeed(&tio, B38400);
        struct winsize ws; std::memset(&ws, 0, sizeof ws);
        ws.ws_row = (unsigned short)rows; ws.ws_col = (unsigned short)cols;
        if (::tcsetattr(sfd, TCSANOW, &tio) != 0 || ::ioctl(sfd, TIOCSWINSZ, &ws) != 0) {
            ::close(sfd); ::close(mfd); out = varr(std::move(fail)); return true;
        }

        pid_t pid = ::fork();
        if (pid < 0) { ::close(sfd); ::close(mfd); out = varr(std::move(fail)); return true; }
        if (pid == 0) {                                  // async-signal-safe ONLY (D-P2)
            ::setsid();                                  // session leader FIRST (R pitfall #8)
            ::ioctl(sfd, TIOCSCTTY, 0);                  // slave = controlling tty
            ::dup2(sfd, 0); ::dup2(sfd, 1); ::dup2(sfd, 2);
            if (sfd > 2) ::close(sfd);
            ::execve(path.c_str(), argv.data(), environ);
            ::_exit(127);
        }
        ::close(sfd);                    // parent keeps ONLY the master (R pitfall #7)
        int fl = ::fcntl(mfd, F_GETFL, 0);
        ::fcntl(mfd, F_SETFL, fl | O_NONBLOCK);          // rides the loop like every fd
        std::vector<Value> r{vint(pid), vint(mfd)};
        out = varr(std::move(r));
        return true;
    }
    // sysPtyResize(masterFd, rows, cols) -> 0/-1. TIOCSWINSZ on the master; the
    // KERNEL SIGWINCHes the child's foreground group — never signal by hand (R§A.4).
    if (name == "sysPtyResize") {
        int fd = (int)(args.size() > 0 ? args[0].i : -1);
        long long rows = args.size() > 1 ? args[1].i : 0;
        long long cols = args.size() > 2 ? args[2].i : 0;
        if (fd < 0 || rows <= 0 || cols <= 0) { out = vint(-1); return true; }
        struct winsize ws; std::memset(&ws, 0, sizeof ws);
        ws.ws_row = (unsigned short)rows; ws.ws_col = (unsigned short)cols;
        out = vint(::ioctl(fd, TIOCSWINSZ, &ws) == 0 ? 0 : -1);
        return true;
    }
    return false;
}
