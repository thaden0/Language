# TLS test material — **TEST ONLY, NEVER FOR PRODUCTION**

⚠️ These keys and certificates are checked into the repository in the clear and
exist **solely** to exercise the TLS/crypto corpus (`designs/complete/techdesign-tls-crypto.md`,
LA-2). They protect nothing. Do not deploy, do not trust, do not reuse.

| file | role |
|---|---|
| `ca-cert.pem` / `ca-key.pem` | the test root CA (10-year) — corpus clients trust it via the `caFile` arg |
| `server-cert.pem` / `server-key.pem` | leaf cert + key, CN `localhost`, SANs `localhost` / `127.0.0.1` / `::1` |
| `server-chain.pem` | leaf + CA concatenated (what `SSL_CTX_use_certificate_chain_file` loads) |
| `rsa-pub.pem` / `rsa-priv.pem` | a 2048-bit RSA keypair for the `sysRsaEncrypt` OAEP/PKCS#1 round-trip proof |
| `server-big-cert.pem` / `server-big-key.pem` | **TEST ONLY** — an oversized (~19.6 KB DER) self-signed leaf for the LA-29 want-write inversion test (`techdesign-socket-options.md` §7) |

Validity is 10 years so the corpus does not rot; regenerate any time with the
commands below (run from this directory).

```sh
# Test CA (10-year)
openssl req -x509 -newkey rsa:2048 -nodes -keyout ca-key.pem -out ca-cert.pem \
  -days 3650 -subj "/O=Leviathan TEST ONLY/CN=Leviathan Test CA"

# Leaf key + CSR
openssl req -newkey rsa:2048 -nodes -keyout server-key.pem -out server.csr \
  -subj "/O=Leviathan TEST ONLY/CN=localhost"

# SAN extension
cat > san.ext <<'EOF'
subjectAltName = DNS:localhost, IP:127.0.0.1, IP:::1
basicConstraints = CA:FALSE
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
EOF

# Sign the leaf (10-year)
openssl x509 -req -in server.csr -CA ca-cert.pem -CAkey ca-key.pem -CAcreateserial \
  -out server-cert.pem -days 3650 -extfile san.ext
cat server-cert.pem ca-cert.pem > server-chain.pem

# RSA keypair for sysRsaEncrypt
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out rsa-priv.pem
openssl pkey -in rsa-priv.pem -pubout -out rsa-pub.pem

rm -f server.csr san.ext

# LA-29: an oversized (~16 KB flight) self-signed leaf, padded via a long SAN
# list, to force a TLS Certificate flight larger than any clamped tiny-buffer
# window (want-write inversion test; the client uses verifyMode 2, so it need
# not chain to the test CA). Regenerate any time; 10-year validity.
{ printf '[req]\ndistinguished_name=dn\nx509_extensions=v3\nprompt=no\n[dn]\nO=Leviathan TEST ONLY\nCN=localhost\n[v3]\nsubjectAltName=@san\n[san]\n'
  i=1; while [ $i -le 900 ]; do printf 'DNS.%d=h%d.leviathan.test\n' "$i" "$i"; i=$((i+1)); done
} > big.cnf
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
  -keyout server-big-key.pem -out server-big-cert.pem -config big.cnf
rm -f big.cnf
# CONFIRM the flight is big enough: this is the DER cert the server sends.
openssl x509 -in server-big-cert.pem -outform DER | wc -c   # expect >= 12000
```
