/* LA-2 — the OpenSSL TLS/crypto provider (designs/complete/techdesign-tls-crypto.md
 * §2.4 / §4). The compiled-runtime twin of src/RuntimeNatives.cpp's interpreter
 * TLS block: ONE behavioral contract, two implementations, differentially pinned
 * by the TLS corpus. Compiled into liblvrt.a when OpenSSL (>= 1.1.1) is found for
 * the target; otherwise lv_tls_none.c takes its place.
 *
 * State is LV_TLS (thread-local), matching lv_loop.c's per-thread watch registries
 * (§10 #11: sessions are fd-affine, fds are loop-affine, so the table shards per
 * thread). The security posture (§4) is normative: TLS 1.2 floor + 1.3, verify ON
 * by default with RFC 6125 hostname, no renegotiation/compression, SIG_IGN once. */
#include "lv_tls.h"
#include "lv_abi.h"   /* LV_TLS thread-local qualifier */

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/evp.h>

#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct TlsSession {
    SSL* ssl;
    int  want;          /* 0 none | 1 read | 2 write — poll augmentation (§2.3 #1) */
    int  server;
    int  established;
    char error[512];    /* per-fd latest detail (§10 #6) */
} TlsSession;

/* Session table keyed by fd (grown to max-fd-seen; NULL when the fd is plaintext). */
static LV_TLS TlsSession** g_tls;
static LV_TLS int          g_tlsCap;
static LV_TLS char         g_procErr[512];   /* last setup error before a session exists */
static LV_TLS int          g_sigpipeIgn;

/* Server ctx cache keyed by cert|key|alpn (parse once, SSL_new per conn, §3). A
 * small linear table — a server pins one or two cert/key pairs. */
typedef struct CtxEntry { char key[1024]; SSL_CTX* ctx; char* alpnWire; int alpnLen; } CtxEntry;
static LV_TLS CtxEntry* g_serverCtx;
static LV_TLS int       g_serverCtxN, g_serverCtxCap;

static TlsSession* tls_get(int fd) {
    if (fd < 0 || fd >= g_tlsCap) return NULL;
    return g_tls[fd];
}
static void tls_put(int fd, TlsSession* s) {
    if (fd < 0) return;
    if (fd >= g_tlsCap) {
        int nc = g_tlsCap ? g_tlsCap : 16;
        while (nc <= fd) nc *= 2;
        g_tls = realloc(g_tls, (size_t)nc * sizeof *g_tls);
        for (int i = g_tlsCap; i < nc; i++) g_tls[i] = NULL;
        g_tlsCap = nc;
    }
    g_tls[fd] = s;
}

static void tls_sigpipe_once(void) {
    if (!g_sigpipeIgn) { signal(SIGPIPE, SIG_IGN); g_sigpipeIgn = 1; }   /* §4 #6 */
}

/* §4 #7: drain the OpenSSL error queue into dst (never leave a stale queue). */
static void tls_drain_err(char* dst, size_t cap, const char* prefix) {
    size_t used = 0;
    dst[0] = '\0';
    if (prefix) { snprintf(dst, cap, "%s", prefix); used = strlen(dst); }
    unsigned long e;
    int first = (used == 0);
    while ((e = ERR_get_error()) != 0) {
        char b[256]; ERR_error_string_n(e, b, sizeof b);
        int m = snprintf(dst + used, cap - used, "%s%s", first ? "" : "; ", b);
        if (m > 0) used += (size_t)m;
        first = 0;
        if (used >= cap - 1) break;
    }
    if (dst[0] == '\0' || (prefix && strcmp(dst, prefix) == 0))
        snprintf(dst + used, cap - used, "unknown error");
}

static int tls_update_want(TlsSession* s, int sslErr) {
    if (sslErr == SSL_ERROR_WANT_READ)  { s->want = 1; return 1; }
    if (sslErr == SSL_ERROR_WANT_WRITE) { s->want = 2; return 2; }
    s->want = 0;
    return 0;
}

static int tls_is_ip(const char* h) {
    unsigned char buf[16];
    return inet_pton(AF_INET, h, buf) == 1 || inet_pton(AF_INET6, h, buf) == 1;
}

/* "h2,http/1.1" -> wire ALPN (\x02h2\x08http/1.1). Caller frees. NULL if empty. */
static char* tls_alpn_wire(const char* csv, int* outLen) {
    *outLen = 0;
    if (!csv || !*csv) return NULL;
    size_t n = strlen(csv);
    char* w = malloc(n + 1);
    int wl = 0;
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && csv[j] != ',') j++;
        size_t plen = j - i;
        if (plen > 0 && plen < 256) { w[wl++] = (char)plen; memcpy(w + wl, csv + i, plen); wl += (int)plen; }
        i = (j < n) ? j + 1 : j;
    }
    *outLen = wl;
    return w;
}

/* Server ALPN select: first server proto the client also offered; no overlap ->
 * fatal no_application_protocol alert (RFC 7301 §3.2). arg = CtxEntry*. */
static int tls_alpn_select_cb(SSL* ssl, const unsigned char** out, unsigned char* outlen,
                              const unsigned char* in, unsigned int inlen, void* arg) {
    (void)ssl;
    CtxEntry* e = (CtxEntry*)arg;
    if (!e || !e->alpnWire || e->alpnLen == 0) return SSL_TLSEXT_ERR_NOACK;
    const unsigned char* srv = (const unsigned char*)e->alpnWire;
    int srvlen = e->alpnLen;
    for (int i = 0; i + 1 <= srvlen; ) {
        unsigned char l = srv[i];
        if (i + 1 + l > srvlen) break;
        const unsigned char* sp = srv + i + 1;
        for (unsigned int k = 0; k + 1 <= inlen; ) {
            unsigned char cl = in[k];
            if (k + 1 + cl > inlen) break;
            if (cl == l && memcmp(in + k + 1, sp, l) == 0) {
                *out = sp; *outlen = l; return SSL_TLSEXT_ERR_OK;
            }
            k += 1u + cl;
        }
        i += 1 + l;
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

/* §4 #9: NSS key-log lines only when SSLKEYLOGFILE is set (off by default). */
static void tls_keylog_cb(const SSL* ssl, const char* line) {
    (void)ssl;
    const char* path = getenv("SSLKEYLOGFILE");
    if (!path || !*path) return;
    FILE* f = fopen(path, "a");
    if (f) { fprintf(f, "%s\n", line); fclose(f); }
}
static void tls_keylog_maybe(SSL_CTX* ctx) {
    if (getenv("SSLKEYLOGFILE")) SSL_CTX_set_keylog_callback(ctx, tls_keylog_cb);
}

static void tls_harden_ctx(SSL_CTX* ctx) {
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);                 /* §4 #1 */
    SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_COMPRESSION); /* §4 #3 */
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE |
                          SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);         /* §4 #5 */
}

int lv_tls_client_start(int fd, const char* host, const char* alpn,
                        const char* caFile, int verifyMode) {
    tls_sigpipe_once();
    if (lv_tls_is(fd)) { snprintf(g_procErr, sizeof g_procErr, "fd already TLS"); return -1; }
    if ((!host || !*host) && verifyMode != 2) {
        snprintf(g_procErr, sizeof g_procErr, "empty host requires verification mode 2");
        return -1;
    }
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { tls_drain_err(g_procErr, sizeof g_procErr, "SSL_CTX_new: "); return -1; }
    tls_harden_ctx(ctx);
    SSL_CTX_set_default_verify_paths(ctx);                             /* system roots + env */
    if (caFile && *caFile && SSL_CTX_load_verify_locations(ctx, caFile, NULL) != 1) {
        tls_drain_err(g_procErr, sizeof g_procErr, "load caFile: "); SSL_CTX_free(ctx); return -1;
    }
    tls_keylog_maybe(ctx);
    if (alpn && *alpn) {
        int wl = 0; char* w = tls_alpn_wire(alpn, &wl);
        if (w) { SSL_CTX_set_alpn_protos(ctx, (const unsigned char*)w, (unsigned)wl); free(w); }
    }
    SSL* ssl = SSL_new(ctx);
    SSL_CTX_free(ctx);                                                 /* SSL holds its own ref */
    if (!ssl) { tls_drain_err(g_procErr, sizeof g_procErr, "SSL_new: "); return -1; }
    SSL_set_fd(ssl, fd);
    SSL_set_connect_state(ssl);
    SSL_set_verify(ssl, verifyMode == 2 ? SSL_VERIFY_NONE : SSL_VERIFY_PEER, NULL);
    if (host && *host) {
        int ip = tls_is_ip(host);
        if (!ip) SSL_set_tlsext_host_name(ssl, host);                 /* SNI (never for IP) */
        if (verifyMode == 0) {                                        /* full: chain + identity */
            if (ip) X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), host);
            else    SSL_set1_host(ssl, host);
        }
    }
    TlsSession* s = calloc(1, sizeof *s);
    s->ssl = ssl; s->server = 0;
    tls_put(fd, s);
    ERR_clear_error();                                                /* §2.2 opportunistic step */
    int r = SSL_do_handshake(ssl);
    if (r == 1) s->established = 1;
    else tls_update_want(s, SSL_get_error(ssl, r));
    return fd;
}

static SSL_CTX* server_ctx_get(const char* cert, const char* key, const char* alpn) {
    char k[1024];
    snprintf(k, sizeof k, "%s\x1f%s\x1f%s", cert ? cert : "", key ? key : "", alpn ? alpn : "");
    for (int i = 0; i < g_serverCtxN; i++)
        if (strcmp(g_serverCtx[i].key, k) == 0) return g_serverCtx[i].ctx;

    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) { tls_drain_err(g_procErr, sizeof g_procErr, "SSL_CTX_new: "); return NULL; }
    tls_harden_ctx(ctx);
    if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1) {
        tls_drain_err(g_procErr, sizeof g_procErr, "certificate: "); SSL_CTX_free(ctx); return NULL;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1) {
        tls_drain_err(g_procErr, sizeof g_procErr, "private key: "); SSL_CTX_free(ctx); return NULL;
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        tls_drain_err(g_procErr, sizeof g_procErr, "key/cert mismatch: "); SSL_CTX_free(ctx); return NULL;
    }
    tls_keylog_maybe(ctx);
    if (g_serverCtxN == g_serverCtxCap) {
        g_serverCtxCap = g_serverCtxCap ? g_serverCtxCap * 2 : 4;
        g_serverCtx = realloc(g_serverCtx, (size_t)g_serverCtxCap * sizeof *g_serverCtx);
    }
    CtxEntry* e = &g_serverCtx[g_serverCtxN++];
    snprintf(e->key, sizeof e->key, "%s", k);
    e->ctx = ctx;
    e->alpnWire = tls_alpn_wire(alpn, &e->alpnLen);
    if (e->alpnWire) SSL_CTX_set_alpn_select_cb(ctx, tls_alpn_select_cb, e);
    return ctx;
}

int lv_tls_server_start(int fd, const char* certPath, const char* keyPath, const char* alpn) {
    tls_sigpipe_once();
    if (lv_tls_is(fd)) { snprintf(g_procErr, sizeof g_procErr, "fd already TLS"); return -1; }
    SSL_CTX* ctx = server_ctx_get(certPath, keyPath, alpn);
    if (!ctx) return -1;
    SSL* ssl = SSL_new(ctx);
    if (!ssl) { tls_drain_err(g_procErr, sizeof g_procErr, "SSL_new: "); return -1; }
    SSL_set_fd(ssl, fd);
    SSL_set_accept_state(ssl);
    TlsSession* s = calloc(1, sizeof *s);
    s->ssl = ssl; s->server = 1;
    tls_put(fd, s);
    ERR_clear_error();
    int r = SSL_do_handshake(ssl);
    if (r == 1) s->established = 1;
    else tls_update_want(s, SSL_get_error(ssl, r));
    return fd;
}

int lv_tls_handshake(int fd) {
    TlsSession* s = tls_get(fd);
    if (!s) return -1;
    ERR_clear_error();
    int r = SSL_do_handshake(s->ssl);
    if (r == 1) { s->established = 1; s->want = 0; return 0; }
    int w = tls_update_want(s, SSL_get_error(s->ssl, r));
    if (w) return w;
    long vr = SSL_get_verify_result(s->ssl);
    if (vr != X509_V_OK)
        snprintf(s->error, sizeof s->error, "certificate verify failed: %s",
                 X509_verify_cert_error_string(vr));
    else
        tls_drain_err(s->error, sizeof s->error, "");
    return -1;
}

int64_t lv_tls_send(int fd, const void* p, int64_t n) {
    TlsSession* s = tls_get(fd);
    if (!s) return -2;
    ERR_clear_error();
    size_t written = 0;
    int r = SSL_write_ex(s->ssl, p, (size_t)(n < 0 ? 0 : n), &written);
    if (r == 1) { s->want = 0; return (int64_t)written; }
    int e = SSL_get_error(s->ssl, r);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) { tls_update_want(s, e); return -1; }
    tls_drain_err(s->error, sizeof s->error, ""); s->want = 0;
    return -2;
}

int64_t lv_tls_recv(int fd, void* p, int64_t n) {
    TlsSession* s = tls_get(fd);
    if (!s) return -2;
    ERR_clear_error();
    size_t got = 0;
    int r = SSL_read_ex(s->ssl, p, (size_t)(n < 0 ? 0 : n), &got);
    if (r == 1) { s->want = 0; return (int64_t)got; }
    int e = SSL_get_error(s->ssl, r);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) { tls_update_want(s, e); return -1; }
    s->want = 0;
    if (e == SSL_ERROR_ZERO_RETURN) return 0;                         /* clean close_notify */
    if (e == SSL_ERROR_SYSCALL && ERR_peek_error() == 0) {           /* EOF without close_notify */
        snprintf(s->error, sizeof s->error, "truncated"); return 0;  /* §4 #11 (still EOF) */
    }
    tls_drain_err(s->error, sizeof s->error, "");
    return -2;
}

int lv_tls_close(int fd) {
    TlsSession* s = tls_get(fd);
    if (!s) return 0;
    ERR_clear_error();
    SSL_shutdown(s->ssl);                                            /* single best-effort */
    SSL_free(s->ssl);
    free(s);
    tls_put(fd, NULL);
    return 1;
}

int lv_tls_is(int fd)   { return tls_get(fd) != NULL; }
int lv_tls_wants(int fd){ TlsSession* s = tls_get(fd); return s ? s->want : 0; }
int lv_tls_pending(int fd){ TlsSession* s = tls_get(fd); return (s && s->ssl && SSL_has_pending(s->ssl)) ? 1 : 0; }

const char* lv_tls_error(int fd) {
    TlsSession* s = tls_get(fd);
    if (s && s->error[0]) return s->error;
    return g_procErr;
}
const char* lv_tls_alpn(int fd) {
    TlsSession* s = tls_get(fd);
    if (!s) return "";
    const unsigned char* p = NULL; unsigned int len = 0;
    SSL_get0_alpn_selected(s->ssl, &p, &len);
    if (!p || !len) return "";
    static LV_TLS char buf[256];
    unsigned int m = len < sizeof buf - 1 ? len : (unsigned)(sizeof buf - 1);
    memcpy(buf, p, m); buf[m] = '\0';
    return buf;
}
int lv_tls_version(int fd) {
    TlsSession* s = tls_get(fd);
    if (!s || !s->established) return 0;
    int v = SSL_version(s->ssl);
    if (v == TLS1_3_VERSION) return 13;
    if (v == TLS1_2_VERSION) return 12;
    return 0;
}

/* §7: RSA public-key encrypt (modern EVP path). Returns ciphertext length written
 * into out (<= outCap), or -1 on parse/capacity/encrypt failure. pad 0=OAEP 1=v1.5. */
int64_t lv_rsa_encrypt(const char* pem, int64_t pemLen, const void* in, int64_t inLen,
                       void* out, int64_t outCap, int pad) {
    BIO* bio = BIO_new_mem_buf(pem, (int)pemLen);
    if (!bio) return -1;
    EVP_PKEY* pk = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
    BIO_free(bio);
    if (!pk) return -1;
    EVP_PKEY_CTX* c = EVP_PKEY_CTX_new(pk, NULL);
    int64_t rv = -1;
    if (c && EVP_PKEY_encrypt_init(c) == 1 &&
        EVP_PKEY_CTX_set_rsa_padding(c, pad == 1 ? RSA_PKCS1_PADDING
                                                 : RSA_PKCS1_OAEP_PADDING) == 1) {
        size_t outlen = 0;
        if (EVP_PKEY_encrypt(c, NULL, &outlen, (const unsigned char*)in, (size_t)inLen) == 1 &&
            (int64_t)outlen <= outCap &&
            EVP_PKEY_encrypt(c, (unsigned char*)out, &outlen, (const unsigned char*)in, (size_t)inLen) == 1) {
            rv = (int64_t)outlen;
        }
    }
    if (c) EVP_PKEY_CTX_free(c);
    EVP_PKEY_free(pk);
    return rv;
}
