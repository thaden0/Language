/* LA-2 — the TLS/crypto provider seam (designs/complete/techdesign-tls-crypto.md
 * §2.4). INTERNAL to the runtime — NOT part of lv_abi.h (generated code never
 * calls these; only lv_loop.c / lv_runtime.c do). Exactly ONE provider is
 * compiled into liblvrt.a: lv_tls_openssl.c when OpenSSL is found for the target,
 * else lv_tls_none.c (every call returns not-supported/-1; is/wants/pending
 * return 0; error returns the not-built message) — the same selected-at-build
 * pattern as lv_plat_posix.c vs lv_plat_win32.c.
 *
 * The fd IS the socket fd (wrap-in-place, §2.1): a session side-table keyed by
 * fd routes send/recv/close; sysWatch* keep polling the raw fd. The loop reaches
 * session state through wants/pending, which return 0/false when no session
 * exists — the §2.3 no-TLS bit-identity guarantee. This mirrors the interpreter
 * implementation in src/RuntimeNatives.cpp; the corpus differentially pins both. */
#ifndef LV_TLS_H
#define LV_TLS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int         lv_tls_client_start(int fd, const char* host, const char* alpn,
                                const char* caFile, int verifyMode);
int         lv_tls_server_start(int fd, const char* certPath, const char* keyPath,
                                const char* alpn);
int         lv_tls_handshake(int fd);          /* 0 done | 1 want-read | 2 want-write | -1 */
int64_t     lv_tls_send(int fd, const void* p, int64_t n);   /* n | -1 retry | -2 fatal   */
int64_t     lv_tls_recv(int fd, void* p, int64_t n);         /* n | 0 clean-EOF | -1 | -2 */
int         lv_tls_close(int fd);              /* close_notify best-effort + free; 1=was-tls */
int         lv_tls_is(int fd);                 /* fast: table[fd] != NULL                   */
int         lv_tls_wants(int fd);              /* 0 | 1 read | 2 write (poll augmentation)  */
int         lv_tls_pending(int fd);            /* buffered plaintext ready (§2.3 #2)        */
const char* lv_tls_error(int fd);              /* per-fd error, else process-last, else ""  */
const char* lv_tls_alpn(int fd);               /* negotiated protocol or ""                 */
int         lv_tls_version(int fd);            /* 12 | 13 | 0                               */
int64_t     lv_rsa_encrypt(const char* pem, int64_t pemLen, const void* in, int64_t inLen,
                           void* out, int64_t outCap, int pad /*0=oaep 1=pkcs1*/);

#ifdef __cplusplus
}
#endif

#endif /* LV_TLS_H */
