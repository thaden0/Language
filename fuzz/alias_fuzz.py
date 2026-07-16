#!/usr/bin/env python3
"""Aliasing-heavy differential fuzzer for the copy-on-write / refcount paths.

Generates random .ext programs that stress exactly the aliasing shapes the
fixed corpus does not cover -- random sequences over a few Array<int> and
object (Holder) locals of:

    let            Array<int> a3 = [1, 2];     / Holder o1 = Holder();
    alias          a1 = a0;                    / Holder o2 = o1;
    append         a0 = a0.add(v);             (the COW hot path)
    store-field    o0.f = a1;
    load-field     a2 = o0.f;
    field-append   o0.f = o0.f.add(v);
    pass-to-fn     a0 = fnA0(a0);  fnH0(o0);   (randomly generated helpers)
    drop           a1 = [];  o0 = Holder();
    scoped alias   if (true) { Array<int> t = a0; ... }   (ref dropped at exit)
    loop append    for (int i in 1..k) a0 = a0.add(i);

Every generated program is fully deterministic; interleaved and final
console.writeln observations pin down the values. Each program runs through
three engines and any disagreement is flagged:

    --run        tree-walk oracle (reference semantics)
    --emit-elf   pure x86-64/ELF backend (own emitter, no libc)
    --emit-cpp   C++ backend, compiled with g++ -O2

Failures (mismatch, engine crash, emit error) are saved as full repro dirs
under fuzz/failures/seed-<N>/. Re-run a single seed with:

    fuzz/alias_fuzz.py --seed <N> --count 1

Usage:
    fuzz/alias_fuzz.py                     # 100 programs, seeds 1..100
    fuzz/alias_fuzz.py --count 500 --ops 60
    fuzz/alias_fuzz.py --seed 42 --count 1 --print-program
"""

import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_LEVIATHAN = os.path.join(REPO_ROOT, "build", "leviathan")
DEFAULT_FAILURES = os.path.join(REPO_ROOT, "fuzz", "failures")
TIMEOUT_S = 20


# ---------------------------------------------------------------- generator

class Gen:
    """Generates one deterministic aliasing-heavy program from a seed."""

    def __init__(self, seed, n_ops):
        self.rng = random.Random(seed)
        self.n_ops = n_ops
        self.arrays = []      # live Array<int> local names
        self.holders = []     # live Holder local names
        self.next_id = 0
        self.body = []        # statements inside run()

    def fresh(self, prefix):
        self.next_id += 1
        return f"{prefix}{self.next_id}"

    def val(self):
        return self.rng.randint(0, 99)

    def pick_array(self):
        return self.rng.choice(self.arrays)

    def pick_holder(self):
        return self.rng.choice(self.holders)

    def emit(self, stmt):
        self.body.append("    " + stmt)

    # -- helper functions (randomized per program) --------------------------

    def gen_array_helper(self, name):
        """Array<int> name(Array<int> xs) with 1-3 random ops on xs."""
        lines = [f"Array<int> {name}(Array<int> xs) {{"]
        for _ in range(self.rng.randint(1, 3)):
            k = self.rng.randrange(3)
            if k == 0:
                lines.append(f"    xs = xs.add({self.val()});")
            elif k == 1:
                # alias the parameter, append to the alias: xs must not change
                t = self.fresh("t")
                lines.append(f"    Array<int> {t} = xs;")
                lines.append(f"    {t} = {t}.add({self.val()});")
            else:
                lines.append(f"    for (int i{self.next_id} in 1..{self.rng.randint(1, 3)}) "
                             f"xs = xs.add(i{self.next_id});")
                self.next_id += 1
        lines.append("    return xs;")
        lines.append("}")
        return "\n".join(lines)

    def gen_holder_helper(self, name):
        """void name(Holder h) with 1-3 random ops on h (visible to caller)."""
        lines = [f"void {name}(Holder h) {{"]
        for _ in range(self.rng.randint(1, 3)):
            k = self.rng.randrange(4)
            if k == 0:
                lines.append(f"    h.f = h.f.add({self.val()});")
            elif k == 1:
                lines.append(f"    h.tag = h.tag + {self.val()};")
            elif k == 2:
                lines.append(f"    h.f = [{self.val()}];")
            else:
                # load the field into a local alias, append there: h.f unchanged
                t = self.fresh("t")
                lines.append(f"    Array<int> {t} = h.f;")
                lines.append(f"    {t} = {t}.add({self.val()});")
        lines.append("}")
        return "\n".join(lines)

    # -- observations --------------------------------------------------------

    def observe_array(self, a):
        if self.rng.random() < 0.7:
            self.emit(f'console.writeln({a}.joinToString(","));')
        else:
            self.emit(f"console.writeln({a}.length());")

    def observe_holder(self, o):
        if self.rng.random() < 0.7:
            self.emit(f'console.writeln({o}.f.joinToString(","));')
        else:
            self.emit(f"console.writeln({o}.tag);")

    def maybe_observe(self):
        if self.rng.random() < 0.35:
            if self.holders and self.rng.random() < 0.4:
                self.observe_holder(self.pick_holder())
            else:
                self.observe_array(self.pick_array())

    # -- ops -----------------------------------------------------------------

    def op_new_array(self):
        a = self.fresh("a")
        n = self.rng.randint(0, 3)
        if n == 0:
            self.emit(f"Array<int> {a};")
        else:
            elems = ", ".join(str(self.val()) for _ in range(n))
            self.emit(f"Array<int> {a} = [{elems}];")
        self.arrays.append(a)

    def op_alias_new(self):
        a = self.fresh("a")
        self.emit(f"Array<int> {a} = {self.pick_array()};")
        self.arrays.append(a)

    def op_alias_assign(self):
        x, y = self.pick_array(), self.pick_array()
        if x != y:
            self.emit(f"{x} = {y};")

    def op_append(self):
        a = self.pick_array()
        self.emit(f"{a} = {a}.add({self.val()});")

    def op_new_holder(self):
        o = self.fresh("o")
        self.emit(f"Holder {o} = Holder();")
        self.holders.append(o)

    def op_alias_holder(self):
        o = self.fresh("o")
        self.emit(f"Holder {o} = {self.pick_holder()};")
        self.holders.append(o)

    def op_store_field(self):
        self.emit(f"{self.pick_holder()}.f = {self.pick_array()};")

    def op_load_field(self):
        o = self.pick_holder()
        if self.rng.random() < 0.5:
            a = self.fresh("a")
            self.emit(f"Array<int> {a} = {o}.f;")
            self.arrays.append(a)
        else:
            self.emit(f"{self.pick_array()} = {o}.f;")

    def op_field_append(self):
        o = self.pick_holder()
        self.emit(f"{o}.f = {o}.f.add({self.val()});")

    def op_call_array_fn(self, fns):
        a = self.pick_array()
        self.emit(f"{a} = {self.rng.choice(fns)}({a});")

    def op_call_holder_fn(self, fns):
        self.emit(f"{self.rng.choice(fns)}({self.pick_holder()});")

    def op_drop_array(self):
        self.emit(f"{self.pick_array()} = [];")

    def op_drop_holder(self):
        self.emit(f"{self.pick_holder()} = Holder();")

    def op_scoped_alias(self):
        # temp alias whose reference dies at block exit; the next append on
        # the source may then take the unique in-place path
        src = self.pick_array()
        t = self.fresh("t")
        self.emit("if (true) {")
        self.emit(f"    Array<int> {t} = {src};")
        for _ in range(self.rng.randint(1, 2)):
            self.emit(f"    {t} = {t}.add({self.val()});")
        self.emit(f'    console.writeln({t}.joinToString(","));')
        self.emit("}")
        self.emit(f"{src} = {src}.add({self.val()});")

    def op_loop_append(self):
        i = f"i{self.next_id}"
        self.next_id += 1
        k = self.rng.randint(1, 4)
        if self.holders and self.rng.random() < 0.4:
            o = self.pick_holder()
            self.emit(f"for (int {i} in 1..{k}) {o}.f = {o}.f.add({i});")
        else:
            a = self.pick_array()
            self.emit(f"for (int {i} in 1..{k}) {a} = {a}.add({i});")

    def op_tag(self):
        o = self.pick_holder()
        self.emit(f"{o}.tag = {o}.tag + {self.val()};")

    # -- program -------------------------------------------------------------

    def generate(self):
        array_fns = [self.fresh("fnA") for _ in range(2)]
        holder_fns = [self.fresh("fnH") for _ in range(2)]
        helpers = [self.gen_array_helper(f) for f in array_fns] + \
                  [self.gen_holder_helper(f) for f in holder_fns]

        # seed pools so every op has an operand
        self.op_new_array()
        self.op_new_holder()

        ops = [
            (self.op_new_array, 4), (self.op_alias_new, 8),
            (self.op_alias_assign, 8), (self.op_append, 14),
            (self.op_new_holder, 3), (self.op_alias_holder, 6),
            (self.op_store_field, 8), (self.op_load_field, 7),
            (self.op_field_append, 8),
            (lambda: self.op_call_array_fn(array_fns), 6),
            (lambda: self.op_call_holder_fn(holder_fns), 5),
            (self.op_drop_array, 4), (self.op_drop_holder, 3),
            (self.op_scoped_alias, 5), (self.op_loop_append, 5),
            (self.op_tag, 3),
        ]
        weighted = [op for op, w in ops for _ in range(w)]

        for _ in range(self.n_ops):
            self.rng.choice(weighted)()
            self.maybe_observe()

        # final dump: every live local, labeled, in a fixed order
        self.emit('console.writeln("=== final ===");')
        for a in self.arrays:
            self.emit(f'console.writeln("{a}");')
            self.emit(f'console.writeln({a}.joinToString(","));')
        for o in self.holders:
            self.emit(f'console.writeln("{o}.f");')
            self.emit(f'console.writeln({o}.f.joinToString(","));')
            self.emit(f'console.writeln({o}.tag);')

        return "\n".join(
            ["// generated by fuzz/alias_fuzz.py",
             "class Holder {",
             "    Array<int> f;",
             "    int tag = 0;",
             "}",
             ""]
            + helpers
            + ["", "void run() {"]
            + self.body
            + ["}", "run();", ""])


# ------------------------------------------------------------------ runners

def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True,
                          timeout=TIMEOUT_S, **kw)


def run_engines(leviathan, src, work):
    """Run one program through oracle / ELF / C++. Returns (results, errors):
    results maps engine -> (rc, stdout); errors maps engine -> message for
    engines that failed before the program could run."""
    results, errors = {}, {}

    try:
        p = sh([leviathan, "--run", src])
        results["run"] = (p.returncode, p.stdout)
        if p.returncode != 0:
            errors["run"] = f"oracle exited {p.returncode}: {p.stderr.strip()[:500]}"
    except subprocess.TimeoutExpired:
        errors["run"] = "oracle timeout"

    elf_bin = os.path.join(work, "prog_elf")
    try:
        p = sh([leviathan, "--emit-elf", elf_bin, src])
        if p.returncode != 0:
            errors["elf"] = f"emit-elf failed: {p.stderr.strip()[:500]}"
        else:
            os.chmod(elf_bin, 0o755)
            p = sh([elf_bin])
            results["elf"] = (p.returncode, p.stdout)
    except subprocess.TimeoutExpired:
        errors["elf"] = "elf timeout"

    cpp_src = os.path.join(work, "prog.cpp")
    cpp_bin = os.path.join(work, "prog_cpp")
    try:
        p = sh([leviathan, "--emit-cpp", src])
        if p.returncode != 0:
            errors["cpp"] = f"emit-cpp failed: {p.stderr.strip()[:500]}"
        else:
            with open(cpp_src, "w") as f:
                f.write(p.stdout)
            g = sh(["g++", "-O2", "-o", cpp_bin, cpp_src])
            if g.returncode != 0:
                errors["cpp"] = f"g++ failed: {g.stderr.strip()[:500]}"
            else:
                p = sh([cpp_bin])
                results["cpp"] = (p.returncode, p.stdout)
    except subprocess.TimeoutExpired:
        errors["cpp"] = "cpp timeout"

    return results, errors


def first_diff(a, b):
    la, lb = a.splitlines(), b.splitlines()
    for i in range(max(len(la), len(lb))):
        x = la[i] if i < len(la) else "<missing>"
        y = lb[i] if i < len(lb) else "<missing>"
        if x != y:
            return f"line {i + 1}: oracle={x!r} vs {y!r}"
    return "outputs equal"


def check_seed(seed, n_ops, leviathan, failures_dir):
    """Returns (seed, verdict, detail). verdict: 'ok' | 'fail'."""
    program = Gen(seed, n_ops).generate()
    with tempfile.TemporaryDirectory(prefix=f"aliasfuzz-{seed}-") as work:
        src = os.path.join(work, "prog.ext")
        with open(src, "w") as f:
            f.write(program)
        results, errors = run_engines(leviathan, src, work)

        problems = [f"[{eng}] {msg}" for eng, msg in errors.items()]
        if "run" in results and results["run"][0] == 0:
            rc0, out0 = results["run"]
            for eng in ("elf", "cpp"):
                if eng not in results:
                    continue
                rc, out = results[eng]
                if out != out0:
                    problems.append(f"[{eng}] OUTPUT MISMATCH: {first_diff(out0, out)}")
                elif rc != rc0:
                    problems.append(f"[{eng}] exit code {rc} vs oracle {rc0}")

        if not problems:
            return seed, "ok", ""

        # save a full repro
        repro = os.path.join(failures_dir, f"seed-{seed}")
        os.makedirs(repro, exist_ok=True)
        with open(os.path.join(repro, "prog.ext"), "w") as f:
            f.write(program)
        for eng, (rc, out) in results.items():
            with open(os.path.join(repro, f"{eng}.out"), "w") as f:
                f.write(out)
        with open(os.path.join(repro, "report.txt"), "w") as f:
            f.write(f"seed {seed} (--ops {n_ops})\n" + "\n".join(problems) + "\n")
        return seed, "fail", "; ".join(problems)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--leviathan", default=DEFAULT_LEVIATHAN, help="path to the leviathan binary")
    ap.add_argument("--seed", type=int, default=1, help="first seed (default 1)")
    ap.add_argument("--count", type=int, default=100, help="number of programs")
    ap.add_argument("--ops", type=int, default=40, help="ops per program")
    ap.add_argument("--failures", default=DEFAULT_FAILURES,
                    help="directory for failure repros")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--print-program", action="store_true",
                    help="print the generated program(s) and exit")
    args = ap.parse_args()

    if args.print_program:
        for seed in range(args.seed, args.seed + args.count):
            print(Gen(seed, args.ops).generate())
        return 0

    if not os.path.exists(args.leviathan):
        print(f"error: leviathan binary not found at {args.leviathan} "
              f"(build first: cmake --build build -j)", file=sys.stderr)
        return 2
    if shutil.which("g++") is None:
        print("error: g++ not found (needed for the --emit-cpp leg)", file=sys.stderr)
        return 2

    seeds = range(args.seed, args.seed + args.count)
    fails = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futs = [pool.submit(check_seed, s, args.ops, args.leviathan, args.failures)
                for s in seeds]
        for fut in futs:
            seed, verdict, detail = fut.result()
            if verdict == "fail":
                fails += 1
                print(f"FAIL seed {seed}: {detail}")
                print(f"     repro: {os.path.join(args.failures, f'seed-{seed}')}/")
    print(f"{args.count} program(s) fuzzed (seeds {args.seed}.."
          f"{args.seed + args.count - 1}, {args.ops} ops each): "
          f"{args.count - fails} agree, {fails} FAIL")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
