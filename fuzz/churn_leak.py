#!/usr/bin/env python3
"""The leak half of the ARC verification net.

The aliasing fuzzer (fuzz/alias_fuzz.py) catches *wrong output*. It cannot
catch a *leak*: a program can print the right thing while never freeing the
escaping-tier objects it dropped. This harness is that missing half.

Each program in tests/corpus/churn/ allocates and drops N escaping objects
(arrays / objects / closures) in a loop, parameterized by a template hole
`@N@`. We compile each at several magnitudes of N via the pure x86-64/ELF
backend, run the binary, and read the escaping-tier accounting meter the
backend prints to stderr at exit:

    [heap] escaping-tier peak=<P> live-at-exit=<L> bytes

The spec the escaping-tier ARC must satisfy:

  * live-at-exit must NOT grow with N. The dropped objects are unreachable;
    the ARC must reclaim them, leaving only the N-independent root set live.

Cross-check (attribution): we also run `--mem-verify` at each N and read the
reachability oracle's root set --

    [mem] <total> heap allocation(s), peak <P> live concurrently, <R> reachable at exit (the root set).

The oracle's root set R is CONSTANT in N (the semantic truth: only a fixed
handful of objects is genuinely reachable at exit). So when live-at-exit
scales with N while R stays flat, the leak is unambiguously an escaping-tier
ARC failure -- not genuinely-reachable memory. The failure message says so.

Expected-red templates (future ARC work targets) carry an XFAIL marker: a
template whose FIRST line contains `XFAIL` (e.g. `// XFAIL: <reason>`) is run
and reported exactly like the others -- oracle differential, mem-verify, the
works -- but a failure is reported as `XFAIL` and does not fail the suite. An
XFAIL that passes is reported as `XPASS` (also non-failing): that is the ARC
agent's cue to flip the marker off, promoting it to a guarded PASS.

Usage:
    fuzz/churn_leak.py                       # default sweep, all churn programs
    fuzz/churn_leak.py --small 100 --large 800
    fuzz/churn_leak.py --corpus tests/corpus/churn --leviathan build/leviathan
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_LEVIATHAN = os.path.join(REPO_ROOT, "build", "leviathan")
DEFAULT_CORPUS = os.path.join(REPO_ROOT, "tests", "corpus", "churn")
TIMEOUT_S = 60

METER_RE = re.compile(r"\[heap\] escaping-tier peak=(\d+) live-at-exit=(\d+) bytes")
ROOTSET_RE = re.compile(r"\[mem\].*?(\d+) reachable at exit \(the root set\)")

# A correct ARC leaves only the N-independent root set live, so live-at-exit is
# essentially identical across N. We allow a small absolute slack for any
# constant differences, and flag anything above it as a leak that scales with N.
TOLERANCE_BYTES = 4096


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True,
                          timeout=TIMEOUT_S, **kw)


def measure_live(leviathan, template, n, work, engine="elf", rtobjs=None, flags=None):
    """Compile the program (with @N@ -> n) via the chosen backend, run it, and
    return (live_bytes, stdout, error). live_bytes/stdout are None on error.

    engine=="elf" emits a standalone ELF via --emit-elf; engine=="llvm" emits an
    object via --native-obj and links it against the runtime-v2 objects (rtobjs)
    — both print the identical `[heap] ... live-at-exit=` meter to stderr (the
    LLVM backend's meter is emitted by lv_main, X64Gen-parity).

    A binary killed by a signal (negative returncode) is reported as a likely
    over-release: once recursive release is wired, a double-free / use-after-free
    on a shared or nested node crashes here, and nested graphs are exactly what
    exercises that path."""
    src = os.path.join(work, "prog.ext")
    with open(src, "w") as f:
        f.write(template.replace("@N@", str(n)))
    binp = os.path.join(work, "prog")
    flags = flags or []
    if engine == "llvm":
        objp = os.path.join(work, "prog.o")
        try:
            p = sh([leviathan] + flags + ["--native-obj", objp, src])
        except subprocess.TimeoutExpired:
            return None, None, "native-obj timed out"
        if p.returncode != 0:
            return None, None, f"native-obj failed (rc={p.returncode}): {p.stderr.strip()[:400]}"
        try:
            p = sh(["cc", "-O2", "-o", binp, objp] + list(rtobjs or []) + ["-lm"])
        except subprocess.TimeoutExpired:
            return None, None, "llvm link timed out"
        if p.returncode != 0:
            return None, None, f"llvm link failed (rc={p.returncode}): {p.stderr.strip()[:400]}"
    else:
        try:
            p = sh([leviathan] + flags + ["--emit-elf", binp, src])
        except subprocess.TimeoutExpired:
            return None, None, "emit-elf timed out"
        if p.returncode != 0:
            return None, None, f"emit-elf failed (rc={p.returncode}): {p.stderr.strip()[:400]}"
    os.chmod(binp, 0o755)
    try:
        p = sh([binp])
    except subprocess.TimeoutExpired:
        return None, None, "binary timed out (possible cycle in recursive release)"
    if p.returncode < 0:
        return None, None, (f"binary killed by signal {-p.returncode} -- likely "
                            f"OVER-RELEASE (double-free / use-after-free) in "
                            f"recursive release: {p.stderr.strip()[:200]}")
    if p.returncode != 0:
        return None, None, f"binary exited {p.returncode}: {p.stderr.strip()[:400]}"
    m = METER_RE.search(p.stderr)
    if not m:
        return None, None, ("no escaping-tier meter line on stderr "
                            f"(got: {p.stderr.strip()[:200]!r}) -- is the accounting "
                            "meter built? (ARC net #1, commit e4792af)")
    return int(m.group(2)), p.stdout, None


def run_oracle(leviathan, template, n, work):
    """Run the tree-walk oracle (--run) for the differential; returns
    (stdout, error)."""
    src = os.path.join(work, "oracle.ext")
    with open(src, "w") as f:
        f.write(template.replace("@N@", str(n)))
    try:
        p = sh([leviathan, "--run", src])
    except subprocess.TimeoutExpired:
        return None, "oracle timed out"
    if p.returncode != 0:
        return None, f"oracle exited {p.returncode}: {p.stderr.strip()[:200]}"
    return p.stdout, None


def measure_rootset(leviathan, template, n, work, flags=None):
    """Run --mem-verify (with @N@ -> n), return (rootset, error)."""
    src = os.path.join(work, "mv.ext")
    with open(src, "w") as f:
        f.write(template.replace("@N@", str(n)))
    try:
        p = sh([leviathan] + (flags or []) + ["--mem-verify", src])
    except subprocess.TimeoutExpired:
        return None, "mem-verify timed out"
    text = p.stdout + p.stderr
    m = ROOTSET_RE.search(text)
    if not m:
        return None, f"could not parse root set from --mem-verify: {text.strip()[:200]!r}"
    return int(m.group(1)), None


def is_xfail(template, engine="elf"):
    """A template whose first line contains XFAIL is an expected-red target: it
    runs and reports like any other, but does not fail the suite. `XFAIL-LLVM`
    is expected-red on the LLVM engine ONLY; `XFAIL-ELF` is the symmetric
    marker for frozen-backend-only debt. Unqualified `XFAIL` applies to both."""
    first = template.split("\n", 1)[0]
    if "XFAIL-LLVM" in first:
        return engine == "llvm"
    if "XFAIL-ELF" in first:
        return engine == "elf"
    return "XFAIL" in first


def check_program(path, leviathan, small, large, tolerance, engine="elf", rtobjs=None, flags=None):
    """Returns (name, ok, detail). ok is True/False; detail is the message."""
    name = os.path.basename(path)
    with open(path) as f:
        template = f.read()
    if "@N@" not in template:
        return name, False, "template has no @N@ hole to parameterize N"

    with tempfile.TemporaryDirectory(prefix=f"churn-{name}-") as work:
        live_s, nat_out, err = measure_live(leviathan, template, small, work, engine, rtobjs, flags)
        if err:
            return name, False, f"N={small}: {err}"
        # Over-release differential: the compiled binary's output must match the
        # tree-walk oracle's. A double-free that recycles a still-referenced
        # block corrupts data (not always a crash), and that shows up here.
        oracle_out, oerr = run_oracle(leviathan, template, small, work)
        if oerr:
            return name, False, f"N={small} oracle: {oerr}"
        if nat_out != oracle_out:
            return name, False, (
                f"N={small}: {engine} output disagrees with the oracle -- likely "
                f"OVER-RELEASE corrupting live data (a freed-then-reused block).\n"
                f"    oracle={oracle_out.strip()[:80]!r} {engine}={nat_out.strip()[:80]!r}")
        live_l, _, err = measure_live(leviathan, template, large, work, engine, rtobjs, flags)
        if err:
            return name, False, f"N={large}: {err}"
        root_s, err = measure_rootset(leviathan, template, small, work, flags)
        if err:
            return name, False, f"N={small} mem-verify: {err}"
        root_l, err = measure_rootset(leviathan, template, large, work, flags)
        if err:
            return name, False, f"N={large} mem-verify: {err}"

    growth = live_l - live_s
    per_obj = growth / (large - small) if large != small else 0.0
    root_const = root_s == root_l
    facts = (f"live-at-exit: N={small} -> {live_s}B,  N={large} -> {live_l}B "
             f"(growth {growth:+d}B, ~{per_obj:.1f}B per churned iteration); "
             f"oracle root set: N={small} -> {root_s}, N={large} -> {root_l}; "
             f"ELF output == oracle")

    if growth <= tolerance:
        return name, True, facts + f"  [flat within {tolerance}B tolerance]"

    if root_const:
        why = (f"the oracle root set is CONSTANT ({root_s} objects reachable at "
               f"exit, independent of N), so these bytes are NOT genuinely "
               f"reachable -- the escaping-tier ARC is not reclaiming the "
               f"{large - small} extra dropped objects between N={small} and "
               f"N={large}. This is the escaping-tier free obligation (§15).")
    else:
        why = (f"the oracle root set itself changed with N ({root_s} -> {root_l}); "
               f"the churn program is not dropping what it should -- fix the "
               f"corpus program before trusting the leak signal.")
    return name, False, facts + "\n    LEAK: " + why


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--leviathan", default=DEFAULT_LEVIATHAN, help="path to the leviathan binary")
    ap.add_argument("--corpus", default=DEFAULT_CORPUS,
                    help="directory of churn .ext templates")
    ap.add_argument("--small", type=int, default=100, help="small N (default 100)")
    ap.add_argument("--large", type=int, default=800, help="large N (default 800)")
    ap.add_argument("--tolerance", type=int, default=TOLERANCE_BYTES,
                    help=f"max allowed live-at-exit growth in bytes (default {TOLERANCE_BYTES})")
    ap.add_argument("--engine", choices=["elf", "llvm"], default="elf",
                    help="backend under test: elf (--emit-elf) or llvm (--native-obj + link)")
    ap.add_argument("--runtime", nargs="+", default=[],
                    help="runtime-v2 .c sources to compile+link for --engine llvm")
    ap.add_argument("--compile-flag", action="append", default=[], dest="compile_flag",
                    help="extra flag passed to leviathan at compile time (repeatable), "
                         "e.g. --compile-flag --columnar for the columnar churn family")
    args = ap.parse_args()

    if not os.path.exists(args.leviathan):
        print(f"error: leviathan binary not found at {args.leviathan} "
              f"(build first: cmake --build build -j)", file=sys.stderr)
        return 2
    if args.large <= args.small:
        print("error: --large must exceed --small", file=sys.stderr)
        return 2

    # The llvm engine links the runtime-v2 objects; compile them ONCE (as C) and
    # reuse across every program+N, mirroring tests/run_native_llvm.sh.
    rtobjs = None
    rt_dir = None
    if args.engine == "llvm":
        if not args.runtime:
            print("error: --engine llvm requires --runtime <sources...>", file=sys.stderr)
            return 2
        rt_dir = tempfile.mkdtemp(prefix="churn-rt-")
        rtobjs = []
        for src in args.runtime:
            obj = os.path.join(rt_dir, os.path.basename(src)[:-2] + ".o")
            p = sh(["cc", "-O2", "-c", "-o", obj, src])
            if p.returncode != 0:
                print(f"error: runtime compile failed ({src}): {p.stderr.strip()[:400]}",
                      file=sys.stderr)
                return 2
            rtobjs.append(obj)

    templates = sorted(f for f in os.listdir(args.corpus) if f.endswith((".ext", ".lev")))
    if not templates:
        print(f"error: no .ext/.lev templates in {args.corpus}", file=sys.stderr)
        return 2

    print(f"churn-leak net [{args.engine}]: sweeping N in {{{args.small}, {args.large}}} "
          f"over {len(templates)} program(s); escaping-tier live-at-exit must stay flat")
    fails = 0
    xfails = 0
    xpasses = 0
    for t in templates:
        path = os.path.join(args.corpus, t)
        with open(path) as f:
            expected_red = is_xfail(f.read(), args.engine)
        name, ok, detail = check_program(
            path, args.leviathan, args.small, args.large, args.tolerance,
            args.engine, rtobjs, args.compile_flag)
        if expected_red:
            tag = "XPASS" if ok else "XFAIL"
            if ok:
                xpasses += 1
            else:
                xfails += 1
        else:
            tag = "PASS" if ok else "FAIL"
            if not ok:
                fails += 1
        print(f"{tag} {name}: {detail}")

    notes = []
    if xfails:
        notes.append(f"{xfails} expected-red XFAIL (future ARC targets)")
    if xpasses:
        notes.append(f"{xpasses} XPASS (fixed! flip the XFAIL marker off)")
    suffix = f"  [{'; '.join(notes)}]" if notes else ""
    checked = len(templates) - xfails - xpasses
    if fails:
        print(f"\n{fails}/{checked} churn program(s) LEAK: the escaping-tier "
              f"ARC (§15) is not reclaiming dropped objects.{suffix}")
    else:
        print(f"\nall {checked} guarded churn program(s) hold live-at-exit flat "
              f"as N grows -- escaping-tier ARC reclaims the churn.{suffix}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
