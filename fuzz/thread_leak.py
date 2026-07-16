#!/usr/bin/env python3
"""The leak/reap/fd net for true OS threads (Track 10 M6, techdesign-threads-3 §9).

The corpus lanes (tests/corpus/threads_native/) prove worker output is
byte-identical to the serial oracle. They cannot prove the true-thread runtime
does not *leak* — a program can print the right sum while leaking a transfer
buffer per message, an eventfd per worker, or a whole worker heap that is never
unmapped. This harness is that missing half, the thread twin of churn_leak.py.

Each program in fuzz/thread_churn/ churns @N@ workers/messages through the
flatten/rebuild + pthread/eventfd machinery, parameterized by the hole `@N@`. We
compile each via the LLVM backend, link the runtime v2 (incl. lv_thread.c), and
run it at two magnitudes of N with LANG_THREAD_STATS=1, reading two meters the
runtime prints to stderr at exit:

    [heap] escaping-tier peak=<P> live-at-exit=<L> bytes
    [threads] spawns=<S> reaps=<R> transfer-outstanding=<O>

The spec the true-thread runtime must satisfy (§9):

  1. live-at-exit does NOT grow with N (each worker's heap is unmapped at reap;
     every transfer buffer is freed at rebuild) — the same flatness churn_leak.py
     asserts, now across the thread boundary.
  2. reaps == spawns: every worker was joined and its regions unmapped (no leaked
     thread, no leaked worker heap).
  3. transfer-outstanding == 0: every malloc'd flatten buffer was freed.
  4. rebuild-equality: the LLVM output equals the tree-walk oracle's — every
     worker rebuilt byte-for-byte the same graph (diamonds/cycles included).
  5. fd flatness (§9.4): re-run under a low RLIMIT_NOFILE. A leaked join eventfd
     per worker would exhaust the fd table (workers are sequential, so a *closed*
     eventfd is reused and never exhausts); completion at N > the limit proves
     the eventfds are retired at reap. This replaces the design's
     sysListDir('/proc/self/fd') sampling, which has no LLVM lowering.

Throughput (msgs/sec / workers/sec) is timed at large N and RECORDED (not gated)
for the implementation log — problem #6's benchmark the v2 zero-copy tier is
argued from.

Usage:
    fuzz/thread_leak.py --runtime runtime/lv_runtime.c runtime/lv_plat_posix.c \\
                        runtime/lv_loop.c runtime/lv_thread.c runtime/lv_entry.c
    fuzz/thread_leak.py --small 200 --large 2000 --runtime <sources...>
"""

import argparse
import os
import re
import resource
import subprocess
import sys
import tempfile
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_LEVIATHAN = os.path.join(REPO_ROOT, "build", "leviathan")
DEFAULT_CORPUS = os.path.join(REPO_ROOT, "fuzz", "thread_churn")
TIMEOUT_S = 120

HEAP_RE = re.compile(r"\[heap\] escaping-tier peak=(\d+) live-at-exit=(\d+) bytes")
THREADS_RE = re.compile(r"\[threads\] spawns=(\d+) reaps=(\d+) transfer-outstanding=(-?\d+)")
RESULT_RE = re.compile(r"result=(-?\d+)")

# A correct runtime leaves only the N-independent root set live, so live-at-exit
# is essentially identical across N. A small absolute slack absorbs constant
# differences; anything above it is a leak that scales with N.
TOLERANCE_BYTES = 8192
# fd-flatness probe: a file-descriptor ceiling far below `large`, so a per-worker
# eventfd leak exhausts it (sequential workers reuse a *closed* eventfd, so a
# non-leaking run never approaches it).
FD_LIMIT = 64
FD_PROBE_N = 200


def sh(cmd, timeout=TIMEOUT_S, env=None, preexec_fn=None):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                          env=env, preexec_fn=preexec_fn)


def compile_llvm(leviathan, src, work, rtobjs):
    """Emit an object via the LLVM backend and link it + the runtime v2 (with
    -lpthread). Returns (binpath, error)."""
    objp = os.path.join(work, "prog.o")
    binp = os.path.join(work, "prog")
    p = sh([leviathan, "--native-obj", objp, src])
    if p.returncode != 0:
        return None, f"native-obj failed (rc={p.returncode}): {p.stderr.strip()[:400]}"
    p = sh(["cc", "-O2", "-o", binp, objp] + list(rtobjs) + ["-lm", "-lpthread"])
    if p.returncode != 0:
        return None, f"llvm link failed (rc={p.returncode}): {p.stderr.strip()[:400]}"
    os.chmod(binp, 0o755)
    return binp, None


def run_llvm(binp, timed=False, fd_limit=None):
    """Run the linked binary with LANG_THREAD_STATS=1. Returns a dict of parsed
    meters + stdout + elapsed, or {'error': ...}."""
    env = dict(os.environ, LANG_THREAD_STATS="1")
    preexec = None
    if fd_limit is not None:
        def preexec():
            resource.setrlimit(resource.RLIMIT_NOFILE, (fd_limit, fd_limit))
        preexec = preexec
    t0 = time.monotonic()
    try:
        p = sh([binp], env=env, preexec_fn=preexec)
    except subprocess.TimeoutExpired:
        return {"error": "binary timed out (possible deadlock in the ring/join)"}
    elapsed = time.monotonic() - t0
    if p.returncode < 0:
        return {"error": f"binary killed by signal {-p.returncode}: {p.stderr.strip()[:200]}"}
    if p.returncode != 0:
        return {"error": f"binary exited {p.returncode}: {p.stderr.strip()[:300]}"}
    heap = HEAP_RE.search(p.stderr)
    thr = THREADS_RE.search(p.stderr)
    res = RESULT_RE.search(p.stdout)
    if not heap:
        return {"error": f"no [heap] meter (stderr: {p.stderr.strip()[:200]!r})"}
    if not thr:
        return {"error": f"no [threads] meter (LANG_THREAD_STATS): {p.stderr.strip()[:200]!r}"}
    return {
        "live": int(heap.group(2)),
        "spawns": int(thr.group(1)), "reaps": int(thr.group(2)),
        "outstanding": int(thr.group(3)),
        "result": res.group(1) if res else None,
        "stdout": p.stdout, "elapsed": elapsed,
    }


def run_oracle(leviathan, src):
    p = sh([leviathan, "--run", src])
    if p.returncode != 0:
        return None, f"oracle exited {p.returncode}: {p.stderr.strip()[:200]}"
    m = RESULT_RE.search(p.stdout)
    return (m.group(1) if m else p.stdout.strip()), None


def write_prog(template, n, work, name):
    src = os.path.join(work, name)
    with open(src, "w") as f:
        f.write(template.replace("@N@", str(n)))
    return src


def check_program(path, leviathan, small, large, tolerance, rtobjs):
    name = os.path.basename(path)
    with open(path) as f:
        template = f.read()
    if "@N@" not in template:
        return name, False, "template has no @N@ hole", None

    with tempfile.TemporaryDirectory(prefix=f"thchurn-{name}-") as work:
        # small N: compile, run LLVM + oracle, assert reap/outstanding + differential.
        src_s = write_prog(template, small, work, "small.lev")
        binp, err = compile_llvm(leviathan, src_s, work, rtobjs)
        if err:
            return name, False, f"N={small}: {err}", None
        r_s = run_llvm(binp)
        if "error" in r_s:
            return name, False, f"N={small}: {r_s['error']}", None
        oracle, oerr = run_oracle(leviathan, src_s)
        if oerr:
            return name, False, f"N={small} oracle: {oerr}", None
        if r_s["result"] != oracle:
            return name, False, (f"N={small}: LLVM result={r_s['result']} != oracle "
                                 f"result={oracle} — rebuild disagreement (a flatten bug)"), None
        if r_s["reaps"] != r_s["spawns"]:
            return name, False, (f"N={small}: reaps={r_s['reaps']} != spawns={r_s['spawns']} "
                                 f"— a worker was not joined/unmapped"), None
        if r_s["outstanding"] != 0:
            return name, False, (f"N={small}: transfer-outstanding={r_s['outstanding']} != 0 "
                                 f"— leaked flatten buffer(s)"), None

        # large N: flatness + reap/outstanding again + throughput.
        src_l = write_prog(template, large, work, "large.lev")
        binp_l, err = compile_llvm(leviathan, src_l, work, rtobjs)
        if err:
            return name, False, f"N={large}: {err}", None
        r_l = run_llvm(binp_l, timed=True)
        if "error" in r_l:
            return name, False, f"N={large}: {r_l['error']}", None
        if r_l["reaps"] != r_l["spawns"]:
            return name, False, (f"N={large}: reaps={r_l['reaps']} != spawns={r_l['spawns']}"), None
        if r_l["outstanding"] != 0:
            return name, False, (f"N={large}: transfer-outstanding={r_l['outstanding']} != 0"), None

        # fd flatness: re-run under a low RLIMIT_NOFILE (§9.4 replacement).
        src_f = write_prog(template, FD_PROBE_N, work, "fd.lev")
        binp_f, err = compile_llvm(leviathan, src_f, work, rtobjs)
        if err:
            return name, False, f"fd probe: {err}", None
        r_f = run_llvm(binp_f, fd_limit=FD_LIMIT)
        if "error" in r_f:
            return name, False, (f"fd flatness: N={FD_PROBE_N} under RLIMIT_NOFILE={FD_LIMIT} "
                                 f"failed — a leaked eventfd per worker exhausts the table: "
                                 f"{r_f['error']}"), None

    growth = r_l["live"] - r_s["live"]
    ips = large / r_l["elapsed"] if r_l["elapsed"] > 0 else 0.0
    facts = (f"live-at-exit: N={small}->{r_s['live']}B, N={large}->{r_l['live']}B "
             f"(growth {growth:+d}B); reaps==spawns={r_l['spawns']}, outstanding=0; "
             f"result matches oracle; fd-flat under RLIMIT={FD_LIMIT} @ N={FD_PROBE_N}; "
             f"~{ips:,.0f} iters/sec @ N={large}")
    if growth > tolerance:
        return name, False, facts + f"\n    LEAK: live-at-exit grew {growth}B > {tolerance}B", ips
    return name, True, facts + f"  [flat within {tolerance}B]", ips


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--leviathan", default=DEFAULT_LEVIATHAN)
    ap.add_argument("--corpus", default=DEFAULT_CORPUS)
    ap.add_argument("--small", type=int, default=200)
    ap.add_argument("--large", type=int, default=2000)
    ap.add_argument("--tolerance", type=int, default=TOLERANCE_BYTES)
    ap.add_argument("--runtime", nargs="+", required=True,
                    help="runtime v2 .c sources (incl. lv_thread.c) to compile+link")
    args = ap.parse_args()

    if not os.path.exists(args.leviathan):
        print(f"error: leviathan not found at {args.leviathan}", file=sys.stderr)
        return 2
    if args.large <= args.small:
        print("error: --large must exceed --small", file=sys.stderr)
        return 2

    rt_dir = tempfile.mkdtemp(prefix="thchurn-rt-")
    rtobjs = []
    for src in args.runtime:
        obj = os.path.join(rt_dir, os.path.basename(src)[:-2] + ".o")
        p = sh(["cc", "-O2", "-c", "-o", obj, src])
        if p.returncode != 0:
            print(f"error: runtime compile failed ({src}): {p.stderr.strip()[:400]}",
                  file=sys.stderr)
            return 2
        rtobjs.append(obj)

    templates = sorted(f for f in os.listdir(args.corpus) if f.endswith(".lev"))
    if not templates:
        print(f"error: no .lev templates in {args.corpus}", file=sys.stderr)
        return 2

    print(f"thread-leak net: sweeping N in {{{args.small}, {args.large}}} over "
          f"{len(templates)} program(s); live-at-exit flat, reaps==spawns, "
          f"transfer-outstanding==0, fd-flat")
    fails = 0
    for t in templates:
        name, ok, detail, _ = check_program(
            os.path.join(args.corpus, t), args.leviathan,
            args.small, args.large, args.tolerance, rtobjs)
        print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}")
        if not ok:
            fails += 1

    if fails:
        print(f"\n{fails}/{len(templates)} thread-churn program(s) FAIL: the "
              f"true-thread runtime leaks or mis-reaps.")
    else:
        print(f"\nall {len(templates)} thread-churn program(s) hold flat: no leaked "
              f"worker heaps, transfer buffers, or eventfds as N grows.")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
