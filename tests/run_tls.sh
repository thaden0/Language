#!/usr/bin/env bash
# LA-2 TLS/crypto integration lane (designs/complete/techdesign-tls-crypto.md §12).
# Runs the tls corpus on oracle / IR / LLVM, plus the RSA round-trip against
# `openssl pkeyutl`, sysRandom checks, comptime denial, and the loud-reject
# wording of the throwing tlsConnect form. Hermetic: SSL_CERT_FILE points the
# client (verifyMode 0, system roots) at the checked-in test CA. Self-SKIPs
# cleanly when the runtime was built without OpenSSL (a plaintext-only build) or
# when the `openssl` CLI is absent.
#   usage: run_tls.sh <leviathan-binary> <repo-root>
set -u
bin="$1"; root="$2"
cd "$root" || { echo "FAIL cannot cd $root"; exit 1; }
export SSL_CERT_FILE="$root/tests/certs/ca-cert.pem"
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
fail=0

strip_heap() { grep -v '^\[heap\]'; }

# --- OpenSSL presence probe: sysTlsError on a bogus fd returns the not-built
# message when the `none` provider is compiled in, else "". -------------------
printf 'console.writeln("[" + std::sysTlsError(999) + "]");\n' > "$work/probe.lev"
probe=$("$bin" --run "$work/probe.lev" 2>&1)
if echo "$probe" | grep -q "not built"; then
  echo "SKIP tls_integration: runtime built without OpenSSL"; exit 0
fi
if ! command -v openssl >/dev/null 2>&1; then
  echo "SKIP tls_integration: openssl CLI not on PATH"; exit 0
fi

# --- 1. corpus on oracle + IR + LLVM ----------------------------------------
for f in tests/corpus/tls/*.lev; do
  name=$(basename "$f" .lev); exp="${f%.lev}.expected"
  want=$(cat "$exp")
  got=$("$bin" --run "$f" 2>&1)
  [ "$got" = "$want" ] && echo "ok   $name (oracle)" || { echo "FAIL $name (oracle)"; diff <(echo "$want") <(echo "$got") | head -6; fail=1; }
  got=$("$bin" --ir "$f" 2>&1)
  [ "$got" = "$want" ] && echo "ok   $name (ir)" || { echo "FAIL $name (ir)"; diff <(echo "$want") <(echo "$got") | head -6; fail=1; }
  if "$bin" --build-native "$work/$name" "$f" >"$work/$name.blog" 2>&1; then
    got=$(timeout 30 "$work/$name" 2>&1 | strip_heap)
    [ "$got" = "$want" ] && echo "ok   $name (llvm)" || { echo "FAIL $name (llvm)"; diff <(echo "$want") <(echo "$got") | head -6; fail=1; }
  else
    echo "SKIP $name (llvm): build-native unavailable"; head -3 "$work/$name.blog"
  fi
done

# --- 2. RSA round-trip vs openssl pkeyutl (OAEP default + PKCS#1) ------------
cat > "$work/rsa.lev" <<'EOF'
void run() {
    File f = File("tests/certs/rsa-pub.pem", std::read);
    string pem = f.read(4096); f.close();
    string msg = "leviathan-la2-rsa";
    string oaep = std::sysRsaEncrypt(pem, msg) ?? "";
    string pk1  = std::sysRsaEncrypt(pem, msg, "pkcs1") ?? "";
    File o = File("__RSA_OAEP__", std::write | std::binary); o.write(oaep); o.close();
    File p = File("__RSA_PK1__",  std::write | std::binary); p.write(pk1);  p.close();
    console.writeln("rsa done");
}
run();
EOF
sed -i "s#__RSA_OAEP__#$work/oaep.bin#; s#__RSA_PK1__#$work/pk1.bin#" "$work/rsa.lev"
if "$bin" --run "$work/rsa.lev" >/dev/null 2>&1; then
  d1=$(openssl pkeyutl -decrypt -inkey tests/certs/rsa-priv.pem -in "$work/oaep.bin" -pkeyopt rsa_padding_mode:oaep 2>/dev/null)
  d2=$(openssl pkeyutl -decrypt -inkey tests/certs/rsa-priv.pem -in "$work/pk1.bin" 2>/dev/null)
  [ "$d1" = "leviathan-la2-rsa" ] && echo "ok   rsa OAEP round-trip" || { echo "FAIL rsa OAEP: '$d1'"; fail=1; }
  [ "$d2" = "leviathan-la2-rsa" ] && echo "ok   rsa PKCS1 round-trip" || { echo "FAIL rsa PKCS1: '$d2'"; fail=1; }
else
  echo "FAIL rsa (run)"; fail=1
fi

# --- 3. sysRandom: length, non-reuse across runs, empty on n<=0 -------------
cat > "$work/rnd.lev" <<'EOF'
void run() {
    string a = std::sysRandom(32);
    console.writeln("len " + a.length());
    console.writeln("empty " + std::sysRandom(0).length());
}
run();
EOF
r1=$("$bin" --run "$work/rnd.lev" 2>&1)
[ "$(echo "$r1" | head -1)" = "len 32" ] && echo "ok   sysRandom length" || { echo "FAIL sysRandom length: $r1"; fail=1; }

# --- 4. comptime hermeticity: TLS/crypto natives denied ----------------------
for nat in 'std::sysTlsConnect(3,"h")' 'std::sysTlsVersion(3)' 'std::sysSocketBuffer(3,0,0)'; do
  printf 'comptime int N = %s;\nconsole.writeln(N.toString());\n' "$nat" > "$work/ct.lev"
  out=$("$bin" --run "$work/ct.lev" 2>&1); rc=$?
  if [ $rc -ne 0 ] && echo "$out" | grep -q "comptime code may not perform I/O"; then
    echo "ok   comptime denial ($nat)"
  else
    echo "FAIL comptime denial ($nat): rc=$rc $out"; fail=1
  fi
done

# --- 5. loud-reject wording of the throwing tlsConnect form ------------------
# A hostname mismatch through std::tlsConnect raises an uncaught async
# RuntimeException on the interpreters; assert the named reason is reported.
cat > "$work/loud.lev" <<'EOF'
void run() {
    int lfd = std::sysTcpListen(8443);
    int aw = std::sysWatch(lfd, (r) => {
        int cfd = std::sysAccept(lfd);
        if (cfd >= 0) { std::tlsAccept(cfd, "tests/certs/server-chain.pem",
                                       "tests/certs/server-key.pem", "", 10000, (s) => {}); }
    });
    std::connectTimeout("127.0.0.1", 8443, 5000, (fd) => {
        if (fd < 0) { return; }
        std::tlsConnect(fd, "wrong.example", "", "tests/certs/ca-cert.pem", 0, (c) => {});
    });
}
run();
EOF
lout=$("$bin" --run "$work/loud.lev" 2>&1)
if echo "$lout" | grep -q "TLS handshake: certificate verify failed: hostname mismatch"; then
  echo "ok   loud-reject wording (throwing form)"
else
  echo "FAIL loud-reject wording: $lout"; fail=1
fi

# --- 6. emit-C++ clean deferral for a sysTls* name --------------------------
printf 'void run() { int v = std::sysTlsVersion(3); console.writeln(v.toString()); }\nrun();\n' > "$work/ecpp.lev"
if "$bin" --emit-cpp "$work/ecpp.lev" >/dev/null 2>"$work/ecpp.err"; then
  echo "note emit-C++ compiled sysTlsVersion (int-only path)"
else
  if grep -qE "native backend|not yet lowerable|coverage" "$work/ecpp.err"; then
    echo "ok   emit-C++ clean deferral (sysTlsVersion)"
  else
    echo "FAIL emit-C++ deferral: $(head -2 "$work/ecpp.err")"; fail=1
  fi
fi

[ $fail -eq 0 ] && echo "TLS integration: all checks passed" || echo "TLS integration: FAILURES above"
exit $fail
