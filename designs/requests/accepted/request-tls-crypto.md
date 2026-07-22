# Request: TLS + Crypto Primitives (LA-2)

**From:** Atlantis framework (Tracks 01, 05, 07, 08). **Date:** 2026-07-06.
**Priority:** P0 for Atlantis 1.0 — an enterprise web framework that cannot serve HTTPS or
speak TLS to MySQL is not enterprise-grade. Sequenced need: MySQL TLS by ~AG-3/AG-4
(Oct–Nov), HTTPS serving by AG-8 (2027-02).

## 1. Requirement

- **R-1. TLS over an existing socket, both directions.** A seam that wraps an fd:
  - client: `sysTlsConnect(fd, host) -> int` (returns a TLS-fd; verifies cert chain +
    hostname against system roots; SNI sent),
  - server: `sysTlsAccept(fd, certPath, keyPath) -> int` (PEM cert chain + key),
  - after wrapping, the existing `sysSend`/`sysRecv`/`sysWatch` surface works unchanged on
    the TLS-fd (so `TcpStream`, `HttpServer`, `HttpClient`, and the MySQL driver gain TLS
    with **zero API change** — direction lives in the type, transport security in the fd).
- **R-2. TLS 1.2 + 1.3, ALPN optional** (needed later for HTTP/2; a `string alpn` arg that
  may be `""` is enough now).
- **R-3. RSA public-key encrypt (PKCS#1 OAEP or v1.5)** as a small standalone native:
  `sysRsaEncrypt(pubKeyPem, bytes) -> string?`. MySQL's `caching_sha2_password` full
  handshake without TLS encrypts the password with the server's RSA key — with R-1 *and*
  R-3, the driver covers every MySQL 8 auth configuration.
- **R-4. Secure random bytes**: `sysRandomBytes(n) -> string` (CSPRNG — session ids, CSRF
  tokens, salts). Track 08's `random` is not stated to be crypto-grade; this must be.

Not requested: password hashing (Atlantis implements PBKDF2-HMAC-SHA256 in-language over
Track 09's `hmacSha256`; a native argon2 fast-path can ride LA-12 later), symmetric-crypto
surface for user code (can come with `Block`).

## 2. Implementation-shape note (owner's call)

The portable pivot dropped the zero-dependency constraint (LLVM is the primary backend), so
**backing R-1 with a vendored/system TLS library behind the native seam** is consistent
with current architecture — same pattern as "C++ stands behind these declarations
temporarily, never beside them" (info.md §13). An in-language TLS 1.3 (over `Block` +
AES-GCM/X25519/HKDF natives) is the principled self-host-era destination; Atlantis only
needs the seam and does not care which stands behind it. The frozen ELF backend can simply
not support the TLS natives (framework targets LLVM for production — overview C8).

## 3. What Atlantis does with it

HTTPS serving (Track 01), `https://` in HttpClient + typed service clients and remote MCP
(Track 07), MySQL `caching_sha2_password` + `require_secure_transport` deployments
(Track 05 — designed in from day one per R4; activates when this lands), secure cookies +
CSRF tokens from R-4 (Track 08).

## 4. Acceptance

1. HTTPS loopback corpus: in-language client ↔ in-language server over `sysTlsConnect`/
   `sysTlsAccept`, request/response byte-identical to the plaintext twin.
2. Interop: `curl -k https://localhost:8443/` against an Atlantis hello app; in-language
   client fetches a real public HTTPS endpoint (cert verification exercised, bad-host
   rejected loudly).
3. MySQL 8 with default auth (`caching_sha2_password`) connects: (a) over TLS, (b) over
   plaintext via RSA password exchange (R-3).
4. `sysRandomBytes`: statistical smoke test + non-reuse across process runs.

## 5. Interim fallback (already designed in)

Plaintext HTTP behind a TLS-terminating reverse proxy; MySQL on a private network with
`mysql_native_password`. Both documented as interim in the affected track docs.
