#!/usr/bin/env bash
# Track 08 system-natives differential (designs/techdesign-08-system-natives.md
# §10, M3/M4/M7). The environment-dependent halves that the golden corpus file
# (corpus/sys_natives.lev) deliberately leaves out: env PATH, DNS localhost, and
# a filesystem round-trip — each asserted identical on the two interpreters
# (oracle + IR, the design's semantic reference). Plus the two invariants the
# design calls for: comptime hermeticity (every sys* native denied) and clean
# deferral on the compiled backends (never a miscompile).
#
# usage: run_sysnatives.sh <leviathan-binary>
set -u
bin="$1"
fail=0
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

check() {   # check <label> <expected> <actual>
  if [ "$3" != "$2" ]; then
    echo "FAIL $1"; diff <(echo "$2") <(echo "$3") | head -12; fail=1
  else
    echo "ok   $1"
  fi
}

# --- 1. env (F1): PATH is set; an absent variable is None -------------------
cat > "$work/env.lev" <<'EOF'
console.writeln((env::get("PATH") != None).toString());
console.writeln((env::get("LEVIATHAN_DEFINITELY_UNSET_9Z") == None).toString());
EOF
ENV_EXPECT=$'true\ntrue'
check "env  --run" "$ENV_EXPECT" "$("$bin" --run "$work/env.lev" 2>&1)"
check "env  --ir"  "$ENV_EXPECT" "$("$bin" --ir  "$work/env.lev" 2>&1)"

# --- 2. DNS (F6 phase 1): localhost resolves to the loopback address --------
cat > "$work/dns.lev" <<'EOF'
console.writeln(std::sysResolve("localhost") ?? "NONE");
console.writeln((std::sysResolve("no.such.host.invalid.") == None).toString());
EOF
DNS_EXPECT=$'127.0.0.1\ntrue'
check "dns  --run" "$DNS_EXPECT" "$("$bin" --run "$work/dns.lev" 2>&1)"
check "dns  --ir"  "$DNS_EXPECT" "$("$bin" --ir  "$work/dns.lev" 2>&1)"

# --- 3. dirs (F3): mkdir / write / list / rename / remove round-trip ---------
# The temp dir is passed in via an env var read with env::get, so the .lev body
# carries no absolute path (keeps the program identical across runs).
cat > "$work/fs.lev" <<'EOF'
void run(string base) {
    console.writeln("mkdir=" + std::sysMkdir(base + "/d").toString());
    int fd = std::sysOpen(base + "/d/a.txt", 2);
    std::sysWrite(fd, "hi");
    std::sysClose(fd);
    Array<string>? es = std::sysListDir(base + "/d");
    if (es != None) {
        console.writeln("count=" + es.length().toString());
        console.writeln("has_a=" + es.contains("a.txt").toString());
    }
    console.writeln("rename=" + std::sysRename(base + "/d/a.txt", base + "/d/b.txt").toString());
    console.writeln("rmFile=" + std::sysRemove(base + "/d/b.txt").toString());
    console.writeln("rmDir=" + std::sysRemove(base + "/d").toString());
    console.writeln("gone=" + (std::sysListDir(base + "/d") == None).toString());
}
string? base = env::get("LEV_FS_BASE");
run(base ?? ".");
EOF
FS_EXPECT=$'mkdir=0\ncount=1\nhas_a=true\nrename=0\nrmFile=0\nrmDir=0\ngone=true'
d1="$work/run"; mkdir -p "$d1"
check "fs   --run" "$FS_EXPECT" "$(LEV_FS_BASE="$d1" "$bin" --run "$work/fs.lev" 2>&1)"
d2="$work/ir"; mkdir -p "$d2"
check "fs   --ir"  "$FS_EXPECT" "$(LEV_FS_BASE="$d2" "$bin" --ir  "$work/fs.lev" 2>&1)"

# --- 3b. isDir (request-stat-isdir.md): dir vs file vs missing, and the edge
# the request was filed for — an unreadable (mode 000) directory is still
# correctly classified as a directory, where the old sysListDir(path)!=None
# probe fails to opendir() it and misclassifies it as a file. stat(2) only
# needs search permission on the PARENT path components, not the target, so
# this holds even though the directory itself is locked down. Skipped when
# run as root: root ignores the mode-000 restriction, so the edge can't be
# observed (permissions don't restrict root's opendir either).
if [ "$(id -u)" != "0" ]; then
  d3="$work/isdir"; mkdir -p "$d3/sub"; : > "$d3/f.txt"; mkdir -p "$d3/locked"
  chmod 000 "$d3/locked"
  # isDir alone (LLVM-coverable: sysStat is on the LLVM floor, sysListDir is not).
  cat > "$work/isdir.lev" <<'EOF'
void run(string base) {
    console.writeln("dir=" + std::isDir(base + "/sub").toString());
    console.writeln("file=" + std::isDir(base + "/f.txt").toString());
    console.writeln("missing=" + std::isDir(base + "/nope").toString());
    console.writeln("locked=" + std::isDir(base + "/locked").toString());
}
string? base = env::get("LEV_ISDIR_BASE");
run(base ?? ".");
EOF
  ISDIR_EXPECT=$'dir=true\nfile=false\nmissing=false\nlocked=true'
  check "isDir --run" "$ISDIR_EXPECT" "$(LEV_ISDIR_BASE="$d3" "$bin" --run "$work/isdir.lev" 2>&1)"
  check "isDir --ir"  "$ISDIR_EXPECT" "$(LEV_ISDIR_BASE="$d3" "$bin" --ir  "$work/isdir.lev" 2>&1)"
  if "$bin" --build-native "$work/isdir_llvm" "$work/isdir.lev" >/dev/null 2>&1; then
    check "isDir llvm" "$ISDIR_EXPECT" "$(LEV_ISDIR_BASE="$d3" "$work/isdir_llvm" 2>/dev/null)"
  else
    echo "FAIL llvm (expected isDir to compile natively)"; fail=1
  fi
  # The actual regression the request cites: sysListDir(locked)!=None
  # misclassifies an unreadable directory as a file. Interpreters only
  # (sysListDir is not on the LLVM/emit-C++ floor).
  cat > "$work/isdir_list.lev" <<'EOF'
void run(string base) {
    console.writeln("locked_list=" + (std::sysListDir(base + "/locked") == None).toString());
}
string? base = env::get("LEV_ISDIR_BASE");
run(base ?? ".");
EOF
  LOCKLIST_EXPECT='locked_list=true'
  check "isDir locked-list --run" "$LOCKLIST_EXPECT" "$(LEV_ISDIR_BASE="$d3" "$bin" --run "$work/isdir_list.lev" 2>&1)"
  check "isDir locked-list --ir"  "$LOCKLIST_EXPECT" "$(LEV_ISDIR_BASE="$d3" "$bin" --ir  "$work/isdir_list.lev" 2>&1)"
  chmod 755 "$d3/locked"   # restore so the EXIT trap's rm -rf can't be blocked
fi

# --- 4. comptime hermeticity: every sys* native is denied at build time ------
printf 'comptime int N = std::sysMonotonic();\nconsole.writeln(N.toString());\n' > "$work/ct.lev"
ct_out=$("$bin" --run "$work/ct.lev" 2>&1); ct_rc=$?
if [ $ct_rc -ne 0 ] && echo "$ct_out" | grep -q "comptime code may not perform I/O"; then
  echo "ok   comptime hermeticity (sysMonotonic denied)"
else
  echo "FAIL comptime hermeticity (expected denial, got rc=$ct_rc): $ct_out"; fail=1
fi

# --- 5. compiled backends defer cleanly (never miscompile) -------------------
# emit-C++ and LLVM keep clean coverage-errors for these floor natives (the
# design's stated policy); assert the diagnostic names the native, non-zero rc.
printf 'console.writeln((std::sysMonotonic() >= 0).toString());\n' > "$work/mc.lev"
cpp_out=$("$bin" --build "$work/mc_cpp" "$work/mc.lev" 2>&1); cpp_rc=$?
if [ $cpp_rc -ne 0 ] && echo "$cpp_out" | grep -q "sysMonotonic"; then
  echo "ok   emit-cpp (clean deferral)"
else
  echo "FAIL emit-cpp (expected clean sysMonotonic deferral, got rc=$cpp_rc): $cpp_out"; fail=1
fi
# LLVM covers sysMonotonic since the F5 landing (lvrt_sysmonotonic rides the
# floor's existing lv_plat_now_ns — deadline flows measure elapsed with it):
# the same program now compiles and runs there.
if "$bin" --build-native "$work/mc_llvm" "$work/mc.lev" >/dev/null 2>&1; then
  check "llvm sysMonotonic (covered)" "true" "$("$work/mc_llvm" 2>&1 | head -1)"
else
  echo "FAIL llvm (expected sysMonotonic to compile natively)"; fail=1
fi

# --- 6. connectTimeout (F5): success / refused / blackhole ------------------
# success: a live local listener. refused: a port nothing listens on (SO_ERROR
# path, settles fast). blackhole: TEST-NET-3 — the SYN either blackholes (the
# timer path fires at the 400ms deadline) or the sandbox reports unreachable
# (immediate -1); both observables are "cb(-1), well under 5s", so the
# assertion is stable with or without a network route.
cat > "$work/ct5.lev" <<'EOF'
void step3() {
    int t0 = std::sysMonotonic();
    connectTimeout("203.0.113.1", 81, 400, (fd) => {
        int dt = std::sysMonotonic() - t0;
        console.writeln("blackhole: " + (fd < 0).toString() + " fast=" + (dt < 5000).toString());
    });
}
void step2() {
    connectTimeout("127.0.0.1", 47513, 2000, (fd) => {
        console.writeln("refused: " + (fd < 0).toString());
        step3();
    });
}
TcpListener server = TcpListener(8095);
server.connections((conn) => {
    conn.onData((data) => {
        conn << "echo:" + data;
        conn.close();
    });
});
connectTimeout("127.0.0.1", 8095, 2000, (fd) => {
    console.writeln("connected: " + (fd >= 0).toString());
    TcpStream c = TcpStream(fd);
    c.onData((reply) => {
        console.writeln("reply: " + reply);
        c.close();
        server.stop();
        step2();
    });
    c.send("ping");
});
EOF
CT_EXPECT=$'connected: true\nreply: echo:ping\nrefused: true\nblackhole: true fast=true'
check "connectTimeout --run" "$CT_EXPECT" "$("$bin" --run "$work/ct5.lev" 2>&1)"
check "connectTimeout --ir"  "$CT_EXPECT" "$("$bin" --ir  "$work/ct5.lev" 2>&1)"

# --- 7. TcpStream.send queue-and-drain (F5.6): 512KB in one send ------------
# Far beyond the socket buffer, so the tail must ride the write-watch drain;
# the server counts bytes — byte-exact or the drain is broken. Also runs as a
# native LLVM binary: the F5 floor is covered there (write-watch + nb-connect
# landed in lv_loop.c/lv_plat_* alongside the interpreters).
cat > "$work/drain.lev" <<'EOF'
int total = 0;
void run() {
    string big = "x";
    for (int i in 1..19) { big = big + big; }        // 2^19 = 524288
    int expected = big.length();
    TcpListener server = TcpListener(8096);
    server.connections((conn) => {
        conn.onData((data) => {
            total = total + data.length();
            if (total >= expected) {
                console.writeln("got=" + total.toString() + " want=" + expected.toString());
                conn.close();
                server.stop();
            }
        });
    });
    int fd = std::sysTcpConnect("127.0.0.1", 8096);
    TcpStream client = TcpStream(fd);
    client << big;
}
run();
EOF
DR_EXPECT='got=524288 want=524288'
check "drain --run" "$DR_EXPECT" "$("$bin" --run "$work/drain.lev" 2>&1 | grep '^got=')"
check "drain --ir"  "$DR_EXPECT" "$("$bin" --ir  "$work/drain.lev" 2>&1 | grep '^got=')"
if "$bin" --build-native "$work/drain_llvm" "$work/drain.lev" >/dev/null 2>&1; then
  check "drain llvm-native" "$DR_EXPECT" "$("$work/drain_llvm" 2>&1 | grep '^got=')"
else
  echo "FAIL drain llvm-native (build failed)"; fail=1
fi

# --- 8. spawn (F7): kill -> 128+SIGTERM, and fd-table churn hygiene ----------
cat > "$work/kill.lev" <<'EOF'
Process p = Process("/bin/cat", []);     // stdin never closed: runs forever
p.exitCode().then((code) => console.writeln("killed: " + code.toString()));
after(100).subscribe((n) => p.kill());
EOF
K_EXPECT='killed: 143'
check "kill --run" "$K_EXPECT" "$("$bin" --run "$work/kill.lev" 2>&1)"
check "kill --ir"  "$K_EXPECT" "$("$bin" --ir  "$work/kill.lev" 2>&1)"

cat > "$work/churn.lev" <<'EOF'
// problem #4's acceptance: sequential spawn churn leaves the fd table
// exactly where it started (pipes x3 + pidfd all closed on reap). Extended
// with pty rounds (designs/pty/ doc 01 §5, K8): master + pidfd never leak.
int fdCount() {
    Array<string>? es = std::sysListDir("/proc/self/fd");
    int n = 0 - 1;
    if (es != None) { n = es.length(); }
    return n;
}
int before = 0 - 1;
int rounds = 0;
void spinPty() {
    if (rounds >= 16) {
        console.writeln("churn: " + (fdCount() == before).toString());
        return;
    }
    rounds = rounds + 1;
    Pty p = Pty::Deterministic("/bin/echo", ["r" + rounds.toString()], 24, 80);
    p.onData((s) => {});
    p.exitCode().then((code) => spinPty());
}
void spin() {
    if (rounds >= 8) {
        spinPty();
        return;
    }
    rounds = rounds + 1;
    Process p = Process("/bin/echo", ["r" + rounds.toString()]);
    p.onStdout((s) => {});
    p.exitCode().then((code) => spin());
}
before = fdCount();
spin();
EOF
CH_EXPECT='churn: true'
check "spawn-churn --run" "$CH_EXPECT" "$("$bin" --run "$work/churn.lev" 2>&1)"
check "spawn-churn --ir"  "$CH_EXPECT" "$("$bin" --ir  "$work/churn.lev" 2>&1)"

# LLVM legs (G-LANG-2, techdesign-spawn-llvm.md §7.3): kill -> 143 on the
# compiled runtime, and fd-table churn hygiene probed via sysOpen's
# lowest-available fd number (sysListDir is not on the LLVM floor). Stdout
# compared alone — the LLVM binary's [heap] meter is stderr.
if "$bin" --build-native "$work/kill_llvm" "$work/kill.lev" >/dev/null 2>&1; then
  check "kill llvm-native" "$K_EXPECT" "$("$work/kill_llvm" 2>/dev/null)"
else
  echo "FAIL kill llvm-native (build failed)"; fail=1
fi

cat > "$work/churn_llvm.lev" <<'EOF'
// problem #4's acceptance on the compiled runtime: 8 spawn/reap rounds leave
// the lowest free fd number unchanged (pipes x3 + pidfd all closed on reap).
int fdProbe() {
    int fd = std::sysOpen("/dev/null", 1);
    std::sysClose(fd);
    return fd;
}
int before = 0 - 1;
int rounds = 0;
void spin() {
    if (rounds >= 8) {
        console.writeln("churn: " + (fdProbe() == before).toString());
        return;
    }
    rounds = rounds + 1;
    Process p = Process("/bin/echo", ["r" + rounds.toString()]);
    p.onStdout((s) => {});
    p.exitCode().then((code) => spin());
}
before = fdProbe();
spin();
EOF
if "$bin" --build-native "$work/churn_llvm" "$work/churn_llvm.lev" >/dev/null 2>&1; then
  check "spawn-churn llvm-native" "$CH_EXPECT" "$("$work/churn_llvm" 2>/dev/null)"
else
  echo "FAIL spawn-churn llvm-native (build failed)"; fail=1
fi

# --- 9. comptime hermeticity covers the F5/F7 natives too --------------------
printf 'comptime int N = std::sysSpawn("/bin/true", []).length();\nconsole.writeln(N.toString());\n' > "$work/ct_spawn.lev"
cts_out=$("$bin" --run "$work/ct_spawn.lev" 2>&1); cts_rc=$?
if [ $cts_rc -ne 0 ] && echo "$cts_out" | grep -q "comptime code may not perform I/O"; then
  echo "ok   comptime hermeticity (sysSpawn denied)"
else
  echo "FAIL comptime hermeticity (expected sysSpawn denial, got rc=$cts_rc): $cts_out"; fail=1
fi

# --- 10. spawn floor: emit-C++ defers cleanly; LLVM covers it ----------------
# emit-C++ keeps its deliberate system-layer deferral (Track 08 stubs policy):
# the failure must be a coverage error naming a sys native, never a miscompile.
# LLVM covers the spawn floor since G-LANG-2 landed (techdesign-spawn-llvm.md)
# — flipped from "defers" to "covered", the F5/sysMonotonic precedent.
printf 'Process p = Process("/bin/echo", ["x"]);\np.exitCode().then((c) => console.writeln(c.toString()));\n' > "$work/sp.lev"
sp_cpp=$("$bin" --build "$work/sp_cpp" "$work/sp.lev" 2>&1); sp_cpp_rc=$?
if [ $sp_cpp_rc -ne 0 ] && echo "$sp_cpp" | grep -q "native.*'sys"; then
  echo "ok   emit-cpp (clean spawn deferral)"
else
  echo "FAIL emit-cpp (expected clean spawn deferral, got rc=$sp_cpp_rc): $sp_cpp"; fail=1
fi
if "$bin" --build-native "$work/sp_llvm" "$work/sp.lev" >/dev/null 2>&1; then
  check "llvm spawn (covered)" "0" "$("$work/sp_llvm" 2>/dev/null)"
else
  echo "FAIL llvm (expected sysSpawn to compile natively)"; fail=1
fi

echo "sys-natives differential done"
exit $fail
