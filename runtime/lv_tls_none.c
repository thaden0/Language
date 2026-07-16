/* LA-2 — the not-built TLS provider (designs/complete/techdesign-tls-crypto.md
 * §2.4 / §5.2). Compiled into liblvrt.a when OpenSSL is NOT found for the target
 * (cross triples without target-OpenSSL, or LVRT_TLS=off). Every entry returns
 * the clean not-supported result, so a program that never touches TLS links and
 * runs as a plaintext binary, and one that does gets a loud, greppable failure
 * rather than a silent wrong answer (§10 #10). is/wants/pending return 0 — the
 * §2.3 no-TLS bit-identity guarantee holds trivially (there are never sessions). */
#include "lv_tls.h"

static const char* kNotBuilt = "TLS support not built into this runtime";

int lv_tls_client_start(int fd, const char* host, const char* alpn,
                        const char* caFile, int verifyMode) {
    (void)fd; (void)host; (void)alpn; (void)caFile; (void)verifyMode; return -1;
}
int lv_tls_server_start(int fd, const char* certPath, const char* keyPath,
                        const char* alpn) {
    (void)fd; (void)certPath; (void)keyPath; (void)alpn; return -1;
}
int     lv_tls_handshake(int fd)                  { (void)fd; return -1; }
int64_t lv_tls_send(int fd, const void* p, int64_t n) { (void)fd; (void)p; (void)n; return -2; }
int64_t lv_tls_recv(int fd, void* p, int64_t n)   { (void)fd; (void)p; (void)n; return -2; }
int     lv_tls_close(int fd)                      { (void)fd; return 0; }
int     lv_tls_is(int fd)                         { (void)fd; return 0; }
int     lv_tls_wants(int fd)                      { (void)fd; return 0; }
int     lv_tls_pending(int fd)                    { (void)fd; return 0; }
const char* lv_tls_error(int fd)                  { (void)fd; return kNotBuilt; }
const char* lv_tls_alpn(int fd)                   { (void)fd; return ""; }
int     lv_tls_version(int fd)                    { (void)fd; return 0; }
int64_t lv_rsa_encrypt(const char* pem, int64_t pemLen, const void* in, int64_t inLen,
                       void* out, int64_t outCap, int pad) {
    (void)pem; (void)pemLen; (void)in; (void)inLen; (void)out; (void)outCap; (void)pad;
    return -1;
}
