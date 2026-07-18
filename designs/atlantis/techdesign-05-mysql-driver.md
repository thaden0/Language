# Atlantis Track 05 — MySQL Driver (atlantis-mysql)

**Status:** draft for owner review. **Date:** 2026-07-06.
**Depends on:** Track 09 digests (§3.2 `digest::sha1/sha256`), overview contract **C3**
(data-access seam — this track implements it) + **C6** (`DbConfig`),
`designs/request-tls-crypto.md` (LA-2 — designed-in per R4, activates when it lands).
**Owns:** `packages/atlantis-mysql/` (namespace `MySql`), exclusively.
**Gate:** AG-3 (2026-10-05) — connect, auth, query/execute/transactions with `?` params,
pooling, against real MySQL 8.

---

## 0. Mission, scope, non-goals

**Mission.** An enterprise-grade MySQL 8 client written entirely in Leviathan over
`TcpStream` — the proof that the language's byte-in-string idiom, event loop, and
`Promise`/`await` model can carry a real binary wire protocol, and the template every
future driver package copies (R8).

**Scope.**
1. Wire fundamentals: packet framing + 16MB splitting, length-encoded ints/strings,
   fragmentation-proof incremental reassembly (H-4 / Track 09 §4 discipline), capability
   negotiation, OK/ERR/EOF parsing, utf8mb4.
2. Handshake + auth: `mysql_native_password` (interim path, R4) and
   `caching_sha2_password` fast + full paths (first-class, per `request-tls-crypto.md`),
   AuthSwitchRequest/AuthMoreData flows.
3. Text protocol (`COM_QUERY`) with column-type → `DbValue` mapping.
4. Prepared statements (binary protocol) — **the** mechanism behind C3's `?` params.
5. Exact C3 implementation: `MySql::Driver`, `Connection`, `ResultSet`/`Row`,
   `ExecResult`, transactions, `MySqlException` family with capability interfaces.
6. Connection pool (`IDbPool`) on the single-threaded loop, worker-ready in shape (LA-1).
7. Fixture-corpus + Docker acceptance testing.

**Non-goals.** NO ORM — entities, change tracking, query builder are Track 06's (it
consumes only C3, tested against a `FakeDriver`). NO other databases. NO replication/
binlog protocol. NO `LOAD DATA LOCAL INFILE` (a server-initiated file read — security
hazard; the `0xFB` response is rejected loudly). NO `CLIENT_MULTI_STATEMENTS` (stacked
queries stay impossible even if params are misused — defense in depth). NO compression
protocol, NO server-side cursors in v1.

**R8 extensibility check (what C3 demands of a future `atlantis-postgres`).** Recorded
here so the seam is validated against a second implementation *on paper* now:
(a) `connect(DbConfig)` — Postgres's StartupMessage consumes the same five C6 fields;
(b) C3 pins `?` as THE portable placeholder — Postgres uses `$1..$n`, so *drivers own
placeholder translation* (a lexer that respects quotes/comments — same scanner this track
builds in §4.4 to count params); (c) `DbValue` text-protocol mapping is identical in
shape; (d) the capability-interface exception amendment (§5.4) is what lets Track 06
catch `IDuplicateKeyException` driver-agnostically; (e) the pool (§6) is written against
`IDbDriver`/`IDbConnection` only — a future minor can lift it into `Atlantis::Data`
unchanged. Nothing in C3 is MySQL-shaped; the seam holds.

---

## 1. Wire fundamentals

### 1.1 Byte-in-string ground rules

Until `Block` (LA-3), packets are byte-clean strings: `byteAt(i) -> 0..255` reads,
`std::byteToString(b)` writes, `StringBuilder` accumulates, `subStr` slices. String
`length()` is bytes — exactly what a wire protocol wants. utf8mb4 column data passes
through untouched (bytes in, bytes out). **P-probe P1 (§8) validates byte-cleanliness
through a TcpStream loopback before any feature work** — if sockets mangle NUL/high
bytes, this whole track STOPs on a bug.md filing.

### 1.2 Little-endian primitives (`MySql::Wire`)

Sensitive kernel — implemented here, verbatim, so every consumer shares one correct copy:

```
namespace MySql::Wire {
    int u8 (string b, int off) => b.byteAt(off);
    int u16(string b, int off) => b.byteAt(off) | (b.byteAt(off + 1) << 8);
    int u24(string b, int off) => u16(b, off) | (b.byteAt(off + 2) << 16);
    int u32(string b, int off) => u24(b, off) | (b.byteAt(off + 3) << 24);
    int u64(string b, int off) {                       // lenenc 0xFE payloads
        if (b.byteAt(off + 7) >= 0x80) { throw MySqlProtocolException(
            "u64 exceeds int64 range"); }              // lengths never legitimately do
        return u32(b, off) | (u32(b, off + 4) << 32);
    }
    string wU8 (int v) => std::byteToString(v & 0xFF);
    string wU16(int v) => wU8(v) + wU8(v >> 8);
    string wU24(int v) => wU16(v) + wU8(v >> 16);
    string wU32(int v) => wU24(v) + wU8(v >> 24);

    // Length-encoded integer: < 0xFB inline; FC=2B, FD=3B, FE=8B. 0xFB is NOT a
    // lenenc int — it is NULL (row context) / LOCAL INFILE marker (response context);
    // callers check the first byte BEFORE calling readLenenc.
    struct LenEnc { int value; int size; }
    LenEnc readLenenc(string b, int off) {
        int first = b.byteAt(off);
        if (first < 0xFB)  { return LenEnc(first, 1); }
        if (first == 0xFC) { return LenEnc(u16(b, off + 1), 3); }
        if (first == 0xFD) { return LenEnc(u24(b, off + 1), 4); }
        if (first == 0xFE) { return LenEnc(u64(b, off + 1), 9); }
        throw MySqlProtocolException("0xFB/0xFF is not a lenenc int");
    }
    string writeLenenc(int v) {
        if (v < 0xFB)      { return wU8(v); }
        if (v <= 0xFFFF)   { return std::byteToString(0xFC) + wU16(v); }
        if (v <= 0xFFFFFF) { return std::byteToString(0xFD) + wU24(v); }
        return std::byteToString(0xFE) + wU32(v) + wU32(v >> 32);
    }
    // lenenc string = lenenc length + raw bytes; nul-terminated reader for handshake
}
```

### 1.3 Packet framing + 16MB splitting

Header = 3-byte LE payload length + 1-byte sequence id. Payload cap `0xFFFFFF`.
**Sending:** slice the logical payload into `0xFFFFFF`-byte packets, sequence id
incrementing per slice; a payload of exactly `n * 0xFFFFFF` bytes MUST be followed by an
empty packet (length 0) — the classic trap, encoded in a fixture. **Receiving:** a packet
with length `0xFFFFFF` concatenates with successors until one with length `< 0xFFFFFF`
arrives; the join is one logical packet. Sequence ids: reset to 0 at each command; every
packet either direction increments; mismatch → `MySqlProtocolException` (fail loud —
desync is unrecoverable, connection is marked broken §6.4).

### 1.4 Incremental reassembly (the H-4 state machine)

`onData` chunks arrive at arbitrary granularity — mid-header, mid-payload, many-packets-
at-once. Exactly Track 09 §4's parse-feed discipline: a state machine fed chunks, never
assuming boundaries. Sensitive kernel, implemented here:

```
namespace MySql {
    class PacketAssembler {
        string buf; int pos;              // cursor, compacted every feed (§9#6)
        int state;                        // Wire::ST_HEADER | ST_PAYLOAD
        int need; int seq;                // from header
        StringBuilder joined;             // 16MB-continuation accumulator
        bool joining;
        (int, string) => void sink;       // (seq of LAST frame, logical payload)

        new (( int, string) => void sink) { this.sink = sink; this.reset(); }

        void feed(string chunk) {
            // compact-then-append: leftover is bounded by one frame (< 16MB + 4)
            if (this.pos == this.buf.length()) { this.buf = chunk; }
            else { this.buf = this.buf.subStr(this.pos,
                       this.buf.length() - this.pos) + chunk; }
            this.pos = 0;
            bool progress = true;
            while (progress) {
                progress = false;
                int avail = this.buf.length() - this.pos;
                if (this.state == Wire::ST_HEADER && avail >= 4) {
                    this.need = Wire::u24(this.buf, this.pos);
                    this.seq  = Wire::u8(this.buf, this.pos + 3);
                    this.pos = this.pos + 4;
                    this.state = Wire::ST_PAYLOAD; progress = true;
                } else if (this.state == Wire::ST_PAYLOAD && avail >= this.need) {
                    string payload = this.buf.subStr(this.pos, this.need);
                    this.pos = this.pos + this.need;
                    this.state = Wire::ST_HEADER; progress = true;
                    if (this.need == 0xFFFFFF) {            // continuation
                        this.joined << payload; this.joining = true;
                    } else if (this.joining) {
                        this.joined << payload;
                        string whole = this.joined.toString();
                        this.joined = StringBuilder(); this.joining = false;
                        this.sink(this.seq, whole);
                    } else { this.sink(this.seq, payload); }
                }
            }
        }
        void reset() { this.buf = ""; this.pos = 0; this.joined = StringBuilder();
                       this.joining = false; this.state = Wire::ST_HEADER; }
    }
}
```

The `Connection` (§5) owns one assembler; its sink dispatches on the connection's *phase*
state machine (HANDSHAKE → AUTH → READY → columns/rows per command). Zero-length payload
mid-`ST_PAYLOAD` (need == 0) falls through correctly (avail >= 0 always).

### 1.5 Capability flags (no `enum` — LA-8 fallback: namespace consts)

```
namespace MySql::Caps {
    const int LONG_PASSWORD   = 0x00000001;  const int CONNECT_WITH_DB = 0x00000008;
    const int PROTOCOL_41     = 0x00000200;  const int SSL             = 0x00000800;
    const int TRANSACTIONS    = 0x00002000;  const int SECURE_CONNECTION = 0x00008000;
    const int PLUGIN_AUTH     = 0x00080000;  const int PLUGIN_AUTH_LENENC = 0x00200000;
    const int DEPRECATE_EOF   = 0x01000000;
}
```

We **require** the server to advertise `PROTOCOL_41`, `PLUGIN_AUTH`, `SECURE_CONNECTION`
(else `MySqlProtocolException` — pre-4.1 servers are out of scope). We **request**
those three plus `CONNECT_WITH_DB`, `TRANSACTIONS`, `PLUGIN_AUTH_LENENC`, and
`DEPRECATE_EOF` *if offered* (both result-set terminations are implemented; fixtures
cover both). `SSL` is requested iff `tlsMode != "disabled"` **and** the LA-2 natives
exist (§2.4). Multi-statements/multi-results deliberately not requested (§0).

### 1.6 OK / ERR / EOF

- **ERR** — header `0xFF`: `u16` errno, `#` marker + 5-byte sqlstate, message → §5.4.
- **OK** — header `0x00` (or `0xFE` with `DEPRECATE_EOF`, payload ≥ 7): lenenc
  affectedRows, lenenc lastInsertId, `u16` status flags, `u16` warnings.
- **EOF** (non-deprecated mode) — header `0xFE` **and payload length < 9**. The `< 9`
  rule is load-bearing: a row whose first cell is a lenenc 8-byte int also starts with
  `0xFE` but its payload is ≥ 9 bytes. Pinned in fixtures.
- Status flag `SERVER_MORE_RESULTS_EXISTS (0x0008)` → protocol exception (we never
  request multi-results). `SERVER_STATUS_IN_TRANS (0x0001)` is tracked for §5.3 sanity.

### 1.7 Charset

Handshake response charset byte = **45 (`utf8mb4_general_ci`)** — a one-byte field, so
the 8.0 default 255 also fits, but 45 is accepted everywhere; then `SET NAMES utf8mb4`
issued as the first post-auth command (belt and braces; also normalizes collation
surprises behind one greppable statement).

---

## 2. Handshake & authentication

### 2.1 HandshakeV10 parse

First packet after TCP connect (note: it may instead be ERR — host blocked; handle
before assuming handshake). Fields consumed: protocol version (must be 10), server
version string (nul-terminated, logged), thread id, auth-plugin-data part 1 (8 bytes),
capabilities (lower 16 + upper 16), charset, status, auth-plugin-data length, part 2
(`max(13, len - 8)` bytes — **the 21st byte is a NUL terminator, NOT nonce**; the
scramble nonce is exactly 20 bytes = 8 + 12), auth plugin name (nul-terminated).

### 2.2 Scrambles (sensitive kernel — exact code, verified by P-probe P2)

```
namespace MySql::Auth {
    string xorBytes(string a, string b) {           // |a| == |b| — caller guarantees
        StringBuilder out = StringBuilder();
        int i = 0;
        while (i < a.length()) {
            out << std::byteToString(a.byteAt(i) ^ b.byteAt(i));
            i = i + 1;
        }
        return out.toString();
    }
    // mysql_native_password: XOR(SHA1(pw), SHA1(nonce + SHA1(SHA1(pw))))  — 20 bytes
    string nativeScramble(string password, string nonce20) {
        string s1 = digest::sha1(password);
        string s2 = digest::sha1(s1);
        return xorBytes(s1, digest::sha1(nonce20 + s2));
    }
    // caching_sha2_password fast path:
    //   XOR(SHA256(pw), SHA256(SHA256(SHA256(pw)) + nonce))               — 32 bytes
    string sha2Scramble(string password, string nonce20) {
        string d1 = digest::sha256(password);
        string d2 = digest::sha256(d1);
        return xorBytes(d1, digest::sha256(d2 + nonce20));
    }
}
```

Empty password: send a zero-length auth response (no scramble) — both plugins.
`digest::sha1/sha256` return raw bytes (Track 09 §3.2) — no hex round-trips anywhere.

### 2.3 HandshakeResponse41 and the auth state machine

Response packet (seq continues from handshake): `u32` client flags (§1.5), `u32` max
packet size (16MB), charset 45, 23 zero bytes, user nul-terminated, auth response as
lenenc string (`PLUGIN_AUTH_LENENC`), database nul-terminated (`CONNECT_WITH_DB`),
plugin name nul-terminated. Then the auth phase is a small FSM — every transition
fixture-tested:

- **OK** → connected (→ `SET NAMES utf8mb4` → READY).
- **ERR** → `MySqlAuthException` (1045 et al., §5.4).
- **AuthSwitchRequest** (`0xFE`, payload ≥ 9 in auth phase; plugin name + new 20-byte
  nonce): recompute with the *named* plugin's scramble against the *new* nonce, send as
  a bare payload packet. Old-auth switch (`0xFE`, payload 1) → loud unsupported error.
- **AuthMoreData** (`0x01` header) — `caching_sha2_password` only:
  - `0x03` (fast-auth success) → an OK follows; done.
  - `0x04` (full auth required) → §2.4.

### 2.4 caching_sha2_password full path (designed NOW, activates with LA-2 — R4)

Fast path (§2.2 scramble) works over plaintext when the server has the account's
verifier cached. Cache-miss (`0x04`) requires the cleartext password to reach the
server safely — two routes, both behind `request-tls-crypto.md` seams:

1. **TLS route (preferred).** During §2.3, before sending HandshakeResponse41: if
   `tlsMode != "disabled"` and server offered `SSL`, send an SSLRequest packet (the
   first 4+4+1+23 bytes of the response, `SSL` flag set), then
   `sysTlsConnect(fd, host)` (LA-2 R-1) wraps the fd — TcpStream continues unchanged
   on the TLS-fd. On full-auth `0x04`: send `password + NUL` in the clear-inside-TLS.
2. **RSA route (plaintext links).** On `0x04`, send `0x02` (request public key);
   server answers AuthMoreData carrying a PEM public key; send
   `sysRsaEncrypt(pem, xorBytes(password + NUL, repeatTo(nonce20, len)))` (LA-2 R-3;
   nonce is tiled to password length). Optional pinning: `serverPublicKeyPath` option
   skips the network fetch (defeats MITM key swap on plaintext).

**Interim deployment stance (R4, stated for operators):** until LA-2 lands, supported
production config is `mysql_native_password` accounts over a **private network** (same
host / VPC / unix-socket-proxied), `tlsMode = "disabled"`, and MySQL ≤ 8.0 images or 8.x
with native-password enabled per-user. `caching_sha2_password` *fast path* may succeed
on plaintext when cached, but the driver treats it as best-effort and reports `0x04`
without LA-2 as `MySqlAuthException("full auth requires TLS or RSA — see
request-tls-crypto.md")` — never silently downgrades security. All of §2.4 ships as
dead-until-LA-2 code paths behind `tlsMode`; activation is a config flip, not a rewrite.

### 2.5 Config surface

C6 `DbConfig` (host/port/user/password/database) is the C3-mandated input to
`Driver.connect`. Driver-specific knobs ride the Driver, not DbConfig (frozen contract):
`MySql::Options { string tlsMode = "disabled";  // disabled | preferred | required
string serverPublicKeyPath = ""; int maxResultBytes = 67108864; }`, given at
`Driver(Options opts)` construction; `Driver()` = defaults.

---

## 3. Text protocol (COM_QUERY)

### 3.1 Result FSM

`COM_QUERY` = `0x03` + SQL bytes. Response first packet: ERR | OK (no result set) |
`0xFB` (LOCAL INFILE → rejected, §0) | lenenc column count. Then per-connection phase
walks: `COLUMNS(n)` → [EOF unless DEPRECATE_EOF] → `ROWS` → EOF/OK terminator (§1.6).
Column definition (ColumnDefinition41) fields we keep: name (lenenc str #5), charset,
column length, **type byte**, **flags** (`UNSIGNED = 0x20`, `BINARY = 0x80`), decimals —
rest skipped positionally. Rows: one lenenc string per column; first byte `0xFB` = NULL.

### 3.2 Type mapping (column type byte → `DbValue`)

| Type codes | → DbValue |
|---|---|
| TINY 1, SHORT 2, LONG 3, LONGLONG 8, INT24 9, YEAR 13 | `int` via `toInt()`; **unsigned LONGLONG > int64::max falls back to `string`** (lossless, documented) |
| BIT 16 with length 1 | `bool` (`\x01` → true) — matches `BOOL`-ish schemas; longer BIT → `string` raw |
| FLOAT 4, DOUBLE 5, DECIMAL 0, NEWDECIMAL 246 | `float` via `toFloat()` (DECIMAL precision caveat documented — exact-decimal type is a Track 06/LA follow-up) |
| NULL 6 / any `0xFB` cell | `None` |
| DATE 10, TIME 11, DATETIME 12, TIMESTAMP 7 | `string` verbatim (`"2026-07-06 12:00:00"`) v1; helpers `MySql::Dates::toDateTime(string) -> DateTime?` / `fromDateTime` bridge to Track 09 §2 types |
| everything else (VARCHAR/STRING/BLOB/JSON/ENUM/SET/GEOMETRY…) | `string` (byte-clean; BLOBs work today via byte-in-string) |

The mapping lives in one function `DbValue mapText(int type, int flags, string? cell)`
— the single place Track 06 reads to know what it gets.

### 3.3 Dispatch rule

`query/execute(sql, params)`: **empty `params` → COM_QUERY text protocol; non-empty →
prepared-statement binary protocol (§4).** Client-side escaping/interpolation of params
into SQL text is NOT an acceptable substitute and does not exist in this codebase —
C3's "params always" + AG-3's params acceptance are satisfied only by server-side
binding. (Stated per track charter; a reviewer finding string-spliced SQL anywhere in
`atlantis-mysql` should treat it as a defect of the highest severity.)

---

## 4. Prepared statements (binary protocol)

### 4.1 COM_STMT_PREPARE / EXECUTE / CLOSE

- `COM_STMT_PREPARE (0x16)` + SQL with `?` placeholders → `0x00`, `u32` stmtId,
  `u16` numColumns, `u16` numParams, filler, `u16` warnings; then numParams param
  definitions [+EOF], numColumns column definitions [+EOF]. Definitions are cached with
  the statement.
- `COM_STMT_EXECUTE (0x17)`: `u32` stmtId, flags `0x00` (no cursor), `u32` iteration
  count = 1, **null bitmap** `(numParams + 7) / 8` bytes (bit i = param i is None),
  new-params-bound flag `0x01`, then per-param `[type byte, flags byte]` pairs
  (`0x80` in flags byte = unsigned), then values LE-packed.
- `COM_STMT_CLOSE (0x19)`: `u32` stmtId — fire-and-forget, no response (sequence
  bookkeeping: it still resets its own sequence).

### 4.2 Parameter encoding (from `DbValue`)

| DbValue | wire type | encoding |
|---|---|---|
| `int` | LONGLONG (8) | 8-byte LE two's-complement |
| `bool` | TINY (1) | 1 byte 0/1 |
| `string` | VAR_STRING (253) | lenenc string (byte-clean — BLOBs ride through) |
| `float` | VAR_STRING (253) | textual render, server coerces — v1 dodge that avoids in-language IEEE754 *encode*; upgrade to DOUBLE(5) when `Block` gives cheap bit-packing (LA-3), noted in log |
| `None` | any | bit set in null bitmap; type byte still emitted (NULL 6) |

### 4.3 Binary row decoding

Row packet: `0x00` header, null bitmap of `(numColumns + 7 + 2) / 8` bytes with a
**2-bit offset** (the protocol's oddest corner — fixture-pinned), then values by column
type: ints fixed-width LE (signedness from column flags), FLOAT/DOUBLE as IEEE 754
bits decoded **in-language** (sign/exponent/mantissa extraction over int64, value =
`(1 + frac / 2^52) * 2^(exp - 1023)` via float arithmetic; subnormals scaled from
`2^-1022`; NaN/Inf → protocol exception — MySQL columns cannot store them), DATE/
DATETIME/TIME as length-prefixed component structs (rendered to the same string shapes
as §3.2 so `DbValue` is protocol-agnostic), strings/BLOB/DECIMAL as lenenc strings.
`mapBinary(colDef, bytes)` mirrors `mapText` — same output contract, one fixture corpus
asserts text-vs-binary row equality for identical queries (the strongest cross-check
this track has).

### 4.4 Statement cache

Per connection: `Map<string, PreparedStmt>` keyed by exact SQL text, LRU-capped at 256;
eviction sends `COM_STMT_CLOSE`. Server restarts invalidate ids → errno 1243 (unknown
statement) triggers one transparent re-prepare + retry, then surfaces. The cache also
stores the param count, validated against `params.length()` before EXECUTE (mismatch →
`MySqlException` with a clear message — the most common app-developer error).

---

## 5. Implementing C3 exactly

### 5.1 Surface (package `atlantis-mysql`, depends on `atlantis` for C3 types)

```
namespace MySql {
    class Driver : IDbDriver {
        new () { … }  new (Options opts) { … }
        Promise<IDbConnection> connect(DbConfig cfg);   // TCP → §2 FSM → READY
    }
    class Connection : IDbConnection {                  // one in-flight command;
        Promise<ResultSet>  query  (string sql, Array<DbValue> params);  // §3.3 dispatch
        Promise<ExecResult> execute(string sql, Array<DbValue> params);
        Promise<ITransaction> begin();                  // default isolation
        Promise<ITransaction> beginWith(string isolation);  // "REPEATABLE READ" | …
        void close();                                   // COM_QUIT best-effort + socket
    }
}
```

Commands serialize per connection: a second `query()` while one is in flight queues
behind it (FIFO) — a safety net; the pool normally hands out exclusive connections.
`ResultSet { Array<string> columns; Array<Row> rows; }` per C3; `Row` wraps
`Array<DbValue>` + a shared column-index map: `DbValue at(int i)`,
`DbValue byName(string name)` (unknown name → throw — typo'd column names must not
read as SQL NULLs), plus narrowing sugar `int? intAt(string)`, `string? strAt(string)`.
`ExecResult { int affectedRows; int lastInsertId; }` straight from OK packets.

### 5.2 Async plumbing on the loop

`connect/query/execute` return pending `Promise`s resolved from the `onData`-driven
FSM — no blocking anywhere; `await` composes naturally (overview collapse #1).
**Failure delivery:** the design assumes an exception thrown in the driver's resumed
continuation propagates to the caller's `await` (i.e., promise rejection semantics).
This is *undocumented* in reference.md §6.6.54 — **P-probe P6 settles it before M1**;
if it does not hold, that is a P0 language ask (filed as `request-promise-rejection.md`,
LA register addition) — a DB driver without an error path to its caller is half a
driver. Interim shape if needed: internal `QueryOutcome` union awaited inside driver
wrapper functions — but the C3 signatures stay put either way (R4: design assumes the
ask lands).

### 5.3 Transactions

```
class Tx : ITransaction {                 // ITransaction : IDisposable (C3)
    Promise<void> commit();               // "COMMIT"   → OK; idempotence guarded
    Promise<void> rollback();             // "ROLLBACK" → OK
    void close();                         // using-exit: rollback if neither ran
}
```

`begin()` issues `BEGIN` (after optional `SET TRANSACTION ISOLATION LEVEL …` for
`beginWith` — applies to the *next* transaction only, exactly what we want). The Tx
holds its Connection; `close()` without commit → rollback (RAII: `using ITransaction tx
= await conn.begin();` is exception-safe by construction — no `finally` in the
language, `using` is the model). Commit/rollback after completion → throw. The
connection tracks `SERVER_STATUS_IN_TRANS` from OK packets as a sanity cross-check
(mismatch logged, not fatal). A pooled connection released while IN_TRANS is
rolled back by the pool (§6.4) — leaked transactions must not poison the next tenant.

### 5.4 Error mapping (MySqlException family)

```
namespace MySql {
    interface IMySqlException : IException { int errno; string sqlstate; }
    class MySqlException            : Exception, IMySqlException { … }        // base: any ERR
    class MySqlProtocolException    : MySqlException { … }  // framing/desync (breaks conn)
    class MySqlAuthException        : MySqlException { … }  // 1044/1045/1698/3159
    class MySqlDuplicateKeyException: MySqlException { … }  // 1062/1586/1859; sqlstate 23xxx
    class MySqlDeadlockException    : MySqlException { … }  // 1213; sqlstate 40001 (transient)
    class MySqlLockTimeoutException : MySqlException { … }  // 1205 (transient)
    class MySqlConnectionLostException : MySqlException { … } // socket EOF/reset mid-command
}
```

Classification: errno first, sqlstate class as fallback (23xxx → duplicate/integrity,
40001 → deadlock-transient). **C4-compatible capability interfaces:** catch-by-interface
is the framework's one error model, and Track 06 must catch these *without* depending on
`atlantis-mysql`. Therefore this track proposes an **additive C3 amendment** (escalation
to the overview owner, not a drive-by — overview §3 freeze): `Atlantis::Data` gains
`interface IDbException : IException`, `IDuplicateKeyException`, `IDeadlockException`,
`IConnectionLostException`, `ITransientDbException` (retry-safe marker), which the
classes above additionally implement (multiple inheritance earning its keep, same move
as C4). Until ruled: interfaces ship in `MySql` and Track 06 is told; the flip is
additive. Connection-lost and protocol exceptions mark the connection broken (§6.4).

---

## 6. Connection pool (`IDbPool`)

### 6.1 Shape

```
class Pool : IDbPool {                       // single-loop; no locks needed (yet)
    new (IDbDriver driver, DbConfig cfg, PoolOptions opts) { … }
    Promise<IDbConnection> acquire();        // resolves with a PooledConnection
    void release(IDbConnection c);
    Promise<void> drain();                   // graceful shutdown: quit all, refuse new
}
struct PoolOptions { int maxConnections = 10; int minIdle = 0;
    int idleTimeoutMs = 600000; int pingAfterIdleMs = 30000;
    int acquireTimeoutMs = 30000; int maxWaiters = 1024; }
```

Written against `IDbDriver`/`IDbConnection` **only** — zero MySQL types — so it is
lift-ready into `Atlantis::Data` for driver reuse (§0, R8; flagged to overview owner).

### 6.2 acquire/release on the event loop

`acquire()`: idle connection available → (if idle > `pingAfterIdleMs`: `COM_PING`
first; failure → evict, try next) → resolve. None idle and total < max → 
`driver.connect(cfg)` → resolve. At max → FIFO waiter queue (each waiter carries a
pending Promise + a `std::after(acquireTimeoutMs)` timer → timeout throws
`PoolTimeoutException` at the awaiter; queue > `maxWaiters` → immediate throw —
back-pressure, not unbounded memory). `release()`: broken → discard, and if waiters
exist and total < max, spawn a replacement connect; IN_TRANS → `ROLLBACK` first (§5.3);
else hand directly to the oldest waiter (no idle round-trip) or push to the idle list
timestamped. A `std::every(30s)` reaper closes idle > `idleTimeoutMs` down to `minIdle`
(reaper is cancelled by `drain()` — the loop-exit lifetime invariant, Track 09 §6#4).

### 6.3 using-friendly acquire

`acquire()` resolves a `PooledConnection : IDbConnection` wrapper delegating
query/execute/begin, whose `close()` **releases to the pool** instead of closing the
socket — so the blessed pattern is exactly the language's RAII:

```
using IDbConnection c = await pool.acquire();
ResultSet rs = await c.query("SELECT id, name FROM users WHERE id = ?", [id]);
```

Double-release is idempotent (wrapper flag). A convenience
`Promise<T> withConnection<T>((IDbConnection) => Promise<T> fn)` wraps
acquire/using/release for one-liners.

### 6.4 Health & broken-connection eviction

Any `MySqlProtocolException`/`MySqlConnectionLostException`/socket `onClose` marks the
`Connection.broken` flag; pending command promises get `MySqlConnectionLostException`;
the pool never re-issues a broken connection. `COM_PING (0x0E)` is the liveness probe
(on acquire-after-idle and available as `pool.pingAll()` for ops).

### 6.5 Worker-readiness (LA-1 note, per request-threads.md)

Threads land later; the pool is **worker-safe in shape now**: no ambient globals — the
pool is constructed in the composition root and reaches consumers via `bind IDbPool =>
…`. When LA-1 lands: **one Pool per worker loop** (per-worker sub-pools;
`maxConnections` becomes per-worker), connections NEVER cross workers (no locking
needed, matching request-threads.md's stated plan). A Pool instance shared across
workers is a defect; this is asserted in the doc comment on the class.

---

## 7. Testing

### 7.1 Recorded-hex fixture corpus (the lcurl method)

`packages/atlantis-mysql/tests/fixtures/*.hex` — hand-annotated hex dumps of every
packet type: HandshakeV10 (8.0-native, 8.0-caching_sha2, +AuthSwitch, +AuthMoreData
0x03/0x04), OK/ERR/EOF (both DEPRECATE_EOF modes), COM_QUERY result sets (NULL cells,
every §3.2 type row, empty set, 0xFE-leading-cell trap), prepared prepare/execute/rows
(null-bitmap offsets, 16-column bitmap edge), 16MB-split logical packet + the
exactly-0xFFFFFF empty-follower trap. Capture source: a tiny in-language recording
proxy (`tests/record_proxy.lev`: TcpListener ↔ TcpStream to real MySQL, hex-logging both
directions) — fixtures are *recorded reality*, not hand-derived. Every fixture replays
through `PacketAssembler`+FSM at multiple granularities — whole-buffer, byte-at-a-time,
split-inside-header, split-at-lenenc-boundary, random seeds — asserting identical
outcomes (Track 09 M5's proven discipline). Runs on oracle + IR + LLVM (C8).

### 7.2 Live acceptance (AG-3)

```
docker run --rm -d --name atlantis-mysql-test \
  -e MYSQL_ROOT_PASSWORD=rootsecret -e MYSQL_DATABASE=atlantis_test \
  -e MYSQL_USER=atlantis -e MYSQL_PASSWORD=atlantis-pass \
  -p 127.0.0.1:3306:3306 \
  mysql:8.0 --default-authentication-plugin=mysql_native_password
```

(`mysql:8.0` pinned for the interim path — 8.4 removed that flag; a second job on
`mysql:8.4` with default `caching_sha2_password` + TLS is added the day LA-2 lands,
per request-tls-crypto.md acceptance #3.) Acceptance program `tests/acceptance.lev`:
connect → SET NAMES → DDL (CREATE TABLE) → execute INSERT with params (lastInsertId
asserted) → query with params (every mapped type round-tripped) → duplicate-key path →
transaction commit/rollback pair → deadlock pair (two connections, crossed updates) →
pool churn (50 sequential acquires over max 5) → drain, loop exits clean (+0B ethos).

---

## 8. P-probes (run before feature work; failures → bug.md / LA, never hack)

- **P1 — binary-safe socket round-trip:** loopback TcpListener echoes; client sends all
  256 byte values (incl. 0x00, 0xFF) in one string and in 1-byte sends; asserts byteAt
  equality both directions. THE gate for byte-in-string over sockets.
- **P2 — scramble vector:** `nativeScramble("secret", nonce)` and
  `sha2Scramble("secret", nonce)` for a fixed 20-byte nonce against expected bytes
  precomputed with Python hashlib (recorded in the probe file) — pins both XOR
  pipelines and digest raw-bytes composition.
- **P3 — lenenc edges:** encode/decode round-trip at 0, 250, 251, 252, 65535, 65536,
  0xFFFFFF, 0x1000000, 2^53, 2^63-1; `u64` high-bit guard throws; `0xFB` handled only
  by callers. Also pins int64 `<<`-packing behavior for bytes ≥ 0x80.
- **P4 — assembler fragmentation:** one fixture through every granularity of §7.1
  before the FSM exists on top.
- **P5 — buffer-cursor perf spike:** 10MB of 1KB chunks through `PacketAssembler.feed`
  on `--ir`; expectation-setting number logged (compaction is O(leftover) per feed).
- **P6 — exception-through-await:** async fn throws after an internal await; caller
  `try { await it(); } catch (IException e)` — must catch. Undocumented semantics
  (§5.2); result decides whether `request-promise-rejection.md` is filed as P0.

## 9. Foreseeable problems & strategies

| # | Problem | Strategy |
|---|---|---|
| 1 | **Auth plugin negotiation surprises** (server default differs from user's plugin; AuthSwitch mid-handshake; 8.4 disabling native password) | Auth is an explicit FSM (§2.3) with fixtures for every transition incl. AuthSwitch to each plugin; unknown plugin name → loud `MySqlAuthException` naming the plugin and pointing at request-tls-crypto.md; 8.0 pinned for interim CI |
| 2 | **16MB split rows** (huge BLOB cells; exactly-0xFFFFFF trap; memory doubling during join) | Continuation handled inside `PacketAssembler` (§1.4) below all protocol logic; empty-follower fixture; `maxResultBytes` guard throws before OOM |
| 3 | **Event-loop starvation on big result sets** (H-3) | Rows parse per-`onData`-chunk (~64KB) — the loop breathes between chunks naturally; `maxResultBytes` caps buffering; **streaming API `queryEach(sql, params, (Row) => void)` is the designed v1.1 seam** (needs TcpStream flow-control pause/resume — raised as a P2 language ask candidate, else rows-outrun-consumer just buffers) |
| 4 | **`0xFE` triple meaning** (EOF vs DEPRECATE_EOF-OK vs AuthSwitch vs 8-byte-lenenc row) | Disambiguation is phase + payload-length rules pinned in §1.6/§2.3, each with a dedicated fixture (incl. the ≥9-byte row-starting-with-0xFE trap) |
| 5 | **Promise rejection semantics undocumented** | P6 before M1; failure → P0 ticket + interim outcome-union inside the driver only (C3 signatures unchanged) — §5.2 |
| 6 | **Quadratic buffering** (string concat per chunk on huge results) | Compact-then-append cursor keeps leftover ≤ one frame; P5 measures; if IR numbers are ugly, an `Array<string>` segment list inside the assembler is a contained rewrite |
| 7 | **Unsigned BIGINT / DECIMAL don't fit int/float** | Lossless string fallback (§3.2), documented in the mapping table Track 06 reads; exact-decimal is a future ask, never silent precision loss on ints |
| 8 | **Sequence-id desync** (usually a driver bug — lost packet, miscounted split) | Verified on every packet; mismatch = protocol exception + connection marked broken; never "resync by guessing" |
| 9 | **IEEE754 double decode in-language** (binary FLOAT/DOUBLE rows) | §4.3 exponent/mantissa arithmetic + text-vs-binary equality corpus (same query both protocols); params dodge encode entirely v1 (§4.2) |
| 10 | **Server restart invalidates statement cache** | errno 1243 → single transparent re-prepare (§4.4); pooled conns fail fast via ping-after-idle (§6.2) |

## 10. Milestones & acceptance (AG-3 = 2026-10-05; text protocol first, then prepared)

| M | Deliverable | Accept | Target |
|---|---|---|---|
| M0 | Probes P1–P6 + verdicts logged; tickets filed if needed | all probes green or escalated | Aug 24–28 |
| M1 | Wire core: Wire ns, PacketAssembler, OK/ERR/EOF, fixture harness | §7.1 framing fixtures at all granularities, 3 engines | Sep 1–8 |
| M2 | Handshake + mysql_native_password + PING/QUIT (needs Track 09 digests, landed Aug 21) | connect/auth/ping against Docker (§7.2); auth FSM fixtures | Sep 8–15 |
| M3 | COM_QUERY end-to-end: columns, rows, type mapping, error mapping | typed round-trip corpus + fixture set; error family unit tests | Sep 15–22 |
| M4 | C3 conformance: Connection/ResultSet/Row/ExecResult + transactions | acceptance.lev through tx pair; Track 06 handed the surface | Sep 22–26 |
| M5 | Prepared statements: PREPARE/EXECUTE/CLOSE, binary rows, stmt cache | `?` params acceptance (THE AG-3 requirement); text-vs-binary equality corpus | Sep 26–Oct 3 |
| M6 | Pool: acquire/release/waiters/ping/eviction/drain | pool churn + broken-conn corpus; **AG-3 acceptance run, all green** | Oct 3–5 |
| M7 | caching_sha2 full path + TLS + mysql:8.4 CI job | request-tls-crypto.md acceptance #3 | when LA-2 lands |

## 11. STOP conditions

- **P1 fails** (strings not byte-clean through sockets) → bug.md with repro; nothing in
  this track proceeds.
- **P6 fails** and the outcome-union interim can't deliver exceptions to awaiting
  callers → file `request-promise-rejection.md`, STOP driver error-path work.
- Track 09 digests not landed by Sep 8 → escalate (H-1); never vendor a digest copy.
- Any need to touch `src/**`, `tools/**`, `runtime/**`, the prelude, or to create
  `.ext` files → STOP (overview §0.4 b/f).
- Any pressure to substitute client-side escaping for server-side prepared params
  (e.g., prepared-statement schedule slip near AG-3) → STOP and escalate; §3.3 is
  charter, the gate moves before the mechanism does.
- C3/C6 shape change needed (incl. the §5.4 capability-interface amendment) →
  escalation to overview owner; frozen contracts don't move by driver fiat.

## 12. Implementation log (append-only)

- 2026-07-06 — Design authored. Open with owner: (1) §5.4 additive C3 amendment
  (`Atlantis::Data` exception capability interfaces); (2) P6 promise-rejection
  semantics — probe first, ticket if needed; (3) pool lift into `Atlantis::Data` as a
  later minor (R8). Awaiting review.

- 2026-07-17 — **M1–M6 implemented and verified on oracle + IR + LLVM.** Package
  `packages/atlantis-mysql/` (`MySql` namespace); C3 seam added to the `atlantis`
  package as `Atlantis::Data` (`packages/atlantis/src/data/data.lev`) with the §5.4
  capability interfaces (the amendment is treated as approved per overview §"C3 amendment
  approved (Track 05)").
  - **P6 SETTLED (the load-bearing probe):** a `throw` after an internal `await`
    propagates the **typed** exception synchronously to the caller's `await` — capability
    interfaces (`IDuplicateKeyException` &c.) survive the crossing. So the FSM never throws
    in the socket sink (a throw there hits the loop); it resolves the command promise with
    an error-carrying `CommandResult`, and the public wrappers `await` it, re-`throw` the
    typed exception, and `return Promise(value)`. `request-promise-rejection.md` NOT needed.
  - **P2/P3/P4** green (scramble vectors vs Python hashlib; lenenc edges; assembler at
    every granularity). See `tests/RESULTS.md`.
  - **End-to-end validated** against an in-language fake MySQL 8 server over a real socket
    (`tests/loopback`, `tests/pool`): connect + `mysql_native_password` + SET NAMES,
    text `COM_QUERY` with typed mapping, prepared INSERT/SELECT (binary rows incl.
    in-language IEEE-754), transactions, duplicate-key typed error, PING, pool churn + drain.
  - **M5** binary codec: unsigned BIGINT → lossless decimal string; FLOAT/DOUBLE decoded
    in-language; text-vs-binary output contract shared via `mapText`/`decodeBinaryRow`.
  - **M7** caching_sha2 full path (§2.4): fast path is P2-validated; the TLS route
    (SSLRequest + `std::tlsConnect` wrap-in-place) and RSA route (`sysRsaEncrypt` over the
    nonce-tiled password) are IMPLEMENTED behind `Options.tlsMode` (dead by default, LA-2
    now landed). **Not live-tested** — no MySQL 8.4 `caching_sha2`/TLS server in the build
    env; §7.2 second CI job drops in unchanged.
  - **Live Docker acceptance (§7.2)** not run — no Docker/MySQL available here; the
    loopback fake server stands in on all three engines.
  - **Two compiler defects found + filed** (`known_bugs_1.md`, `docs/footguns.md`):
    **#82** cross-package nested-namespace `const int` reads as 0 when fully-qualified
    (silent); **#83** implementing a dependency's interface requires bare (uses-imported)
    member types and `uses` must appear in every package source file. Both worked around
    throughout the package (bare C3 types + `uses Atlantis::Data;` in all src files); no
    `src/**`/prelude edits were made (§11 respected).
