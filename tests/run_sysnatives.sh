#!/usr/bin/env bash
# Track 08 system-natives differential (designs/techdesign-08-system-natives.md
# §10, M3/M4/M7). The environment-dependent halves that the golden corpus file
# (corpus/sys_natives.lev) deliberately leaves out: env PATH, DNS localhost, and
# a filesystem round-trip — asserted identical on oracle, IR, and LLVM-native
# now that the F3 directory floor is complete. Plus the two invariants the
# design calls for: comptime hermeticity (every sys* native denied) and honest
# per-backend coverage (emit-C++ still defers the system layer cleanly).
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
dllvm="$work/llvm"; mkdir -p "$dllvm"
if "$bin" --build-native "$work/fs_llvm" "$work/fs.lev" >/dev/null 2>&1; then
  check "fs   llvm-native" "$FS_EXPECT" "$(LEV_FS_BASE="$dllvm" "$work/fs_llvm" 2>/dev/null)"
else
  echo "FAIL fs llvm-native (build failed)"; fail=1
fi

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
  # isDir remains independently pinned through sysStat on all three engines.
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
  # distinguishes an unreadable directory from a successful empty listing.
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
  if "$bin" --build-native "$work/isdir_list_llvm" "$work/isdir_list.lev" >/dev/null 2>&1; then
    check "isDir locked-list llvm" "$LOCKLIST_EXPECT" "$(LEV_ISDIR_BASE="$d3" "$work/isdir_list_llvm" 2>/dev/null)"
  else
    echo "FAIL isDir locked-list llvm (build failed)"; fail=1
  fi
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

# --- 5. compiled backend coverage is explicit (never miscompile) -------------
# emit-C++ keeps its clean system-layer coverage error. LLVM covers the clock
# and F3 filesystem family; the focused filesystem executable above pins F3.
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
# compiled runtime, and fd-table churn hygiene probed directly through
# sysOpen's lowest-available fd number. sysListDir is covered independently
# by the F3 differential above. Stdout alone is compared; [heap] is stderr.
if "$bin" --build-native "$work/kill_llvm" "$work/kill.lev" >/dev/null 2>&1; then
  check "kill llvm-native" "$K_EXPECT" "$("$work/kill_llvm" 2>/dev/null)"
else
  echo "FAIL kill llvm-native (build failed)"; fail=1
fi

cat > "$work/churn_llvm.lev" <<'EOF'
// problem #4's acceptance on the compiled runtime: 8 spawn/reap rounds then 8
// pty rounds leave the lowest free fd number unchanged (pipes x3 / the pty
// master, plus the pidfd, all closed on reap).
int fdProbe() {
    int fd = std::sysOpen("/dev/null", 1);
    std::sysClose(fd);
    return fd;
}
int before = 0 - 1;
int rounds = 0;
// designs/pty/ 02 §6.3 (K8): 8 pty rounds on the compiled runtime too — the
// master and the pidfd must both be released on reap, or the probe drifts up.
void spinPty() {
    if (rounds >= 16) {
        console.writeln("churn: " + (fdProbe() == before).toString());
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

# --- 11. pty floor: three-lane behavior + emit-C++ defers cleanly -----------
# designs/pty/ 02 §6.3-§6.4. The kill->143 encoding on a session-leader child,
# the pre-exec TIOCSWINSZ seed, and the frozen VEOF ("\x04") round trip, all
# asserted on the COMPILED runtime as well as the two interpreters — the whole
# point of G-PTY2 is that the three lanes cannot diverge.
cat > "$work/pty.lev" <<'EOF'
string acc = "";
void stepVeof() {
    Pty c = Pty::Deterministic("/bin/cat", [], 24, 80);
    c.onData((s) => { acc = acc + s; });
    c.write("ping\n");
    c.write("\x04");                    // VEOF: the frozen pty EOF protocol
    c.exitCode().then((code) => {
        console.writeln("veof: " + acc.length().toString() + " " + code.toString());
    });
}
void stepKill() {
    Pty k = Pty::Deterministic("/bin/cat", [], 24, 80);
    k.onData((s) => {});
    k.kill();
    k.exitCode().then((code) => {
        console.writeln("kill: " + code.toString());
        acc = "";
        stepVeof();
    });
}
Pty w = Pty::Deterministic("/bin/stty", ["size"], 24, 80);
w.onData((s) => { acc = acc + s; });
w.exitCode().then((code) => {
    console.writeln("winsize: " + acc.trim());   // trim the ONLCR CR
    stepKill();
});
EOF
P_EXPECT='winsize: 24 80
kill: 143
veof: 6 0'
check "pty --run" "$P_EXPECT" "$("$bin" --run "$work/pty.lev" 2>/dev/null)"
check "pty --ir"  "$P_EXPECT" "$("$bin" --ir  "$work/pty.lev" 2>/dev/null)"
if "$bin" --build-native "$work/pty_llvm" "$work/pty.lev" >/dev/null 2>&1; then
  check "pty llvm-native" "$P_EXPECT" "$("$work/pty_llvm" 2>/dev/null)"
else
  echo "FAIL pty llvm-native (build failed)"; fail=1
fi

# emit-C++ keeps its deliberate system-layer deferral for the pty floor too:
# a clean coverage error naming a sys native, never a miscompile (§6.4).
printf 'Pty p = Pty::Deterministic("/bin/echo", ["x"], 24, 80);\np.exitCode().then((c) => console.writeln(c.toString()));\n' > "$work/pty_cpp.lev"
pty_cpp=$("$bin" --build "$work/pty_cpp" "$work/pty_cpp.lev" 2>&1); pty_cpp_rc=$?
if [ $pty_cpp_rc -ne 0 ] && echo "$pty_cpp" | grep -q "native.*'sys"; then
  echo "ok   emit-cpp (clean pty deferral)"
else
  echo "FAIL emit-cpp (expected clean pty deferral, got rc=$pty_cpp_rc): $pty_cpp"; fail=1
fi

# --- 12. the Windows codegen gating split (designs/pty/ 03 §7, D-P10) -------
# S3 narrowed the process-floor Windows reject: sysReap/sysKill gained real
# registry-backed win32 bodies (D-W3) and now LOWER on a Windows triple, while
# sysSpawn/sysPidfdOpen keep the frozen reject (pipes-spawn on Windows is still
# future work). sysPtySpawn/sysPtyResize have lowered everywhere since S2 —
# their Windows story is a RUNTIME degrade (D-P8), never a compile error.
# --native-obj stops after object emission, so this needs no MinGW toolchain.
WIN_TRIPLE=x86_64-pc-windows-gnu
win_reject() {   # win_reject <label> <program>
  printf '%s\n' "$2" > "$work/wingate.lev"
  out=$("$bin" --native-obj "$work/wingate.o" --target "$WIN_TRIPLE" "$work/wingate.lev" 2>&1)
  if [ $? -ne 0 ] && echo "$out" | grep -q "process spawn: unsupported on Windows"; then
    echo "ok   win-gate $1 (rejects)"
  else
    echo "FAIL win-gate $1 (expected the frozen Windows reject): $out"; fail=1
  fi
}
win_lowers() {   # win_lowers <label> <program>
  printf '%s\n' "$2" > "$work/wingate.lev"
  if out=$("$bin" --native-obj "$work/wingate.o" --target "$WIN_TRIPLE" "$work/wingate.lev" 2>&1); then
    echo "ok   win-gate $1 (lowers)"
  else
    echo "FAIL win-gate $1 (expected Windows lowering): $out"; fail=1
  fi
}
win_reject "sysSpawn"      'console.writeln(std::sysSpawn("/bin/true", []).length().toString());'
win_reject "sysPidfdOpen"  'console.writeln(std::sysPidfdOpen(1).toString());'
win_lowers "sysReap/sysKill" 'console.writeln(std::sysReap(999999).toString());
console.writeln(std::sysKill(999999, 15).toString());'
win_lowers "sysPty*"         'console.writeln(std::sysPtySpawn("C:\\x.exe", [], 24, 80, 0).length().toString());
console.writeln(std::sysPtyResize(3, 30, 100).toString());'

echo "sys-natives differential done"
exit $fail
