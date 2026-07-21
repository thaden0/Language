#!/usr/bin/env python3
"""clonedet.py — winnowing-family (CPD/MOSS-style) clone detector + ratchet.

refactor_1 session 08. Fixed algorithm (do not tune):

  1. Per source file: strip `//` line comments, collapse whitespace runs;
     keep only lines with >= 4 significant chars. Two normalization modes:
     "exact" and "identifier-blind" (every C/C++ identifier token — keywords
     included — replaced by `$`; operators/punctuation/numbers kept).
  2. Slide a window of 8 significant lines; hash each window as md5 of the
     joined normalized lines.
  3. A window hash appearing in >= 2 different files (or >= 2 disjoint,
     non-overlapping sites within one file) is a clone window. Aggregate
     distinct clone-window counts per file-pair.
  4. Scan set: src/**/*.{cpp,hpp,c,h} recursively, EXCLUDING any path
     containing `X64` (the frozen backend is never read by tooling) and
     anything under a `build/` or `generated/` directory.

Modes:
  clonedet.py scan      human-readable per-pair table (identifier-blind),
                        sorted descending.
  clonedet.py baseline  deterministic JSON {"fileA|fileB": count} to stdout
                        (identifier-blind); redirect to tools/clone_baseline.json.
  clonedet.py check     recompute and compare against tools/clone_baseline.json;
                        exit nonzero iff any pair EXCEEDS its baseline
                        (missing entry = baseline 0). Decreases print a
                        re-baseline reminder but do not fail.

Determinism: file list, hash occurrence lists, and all output are sorted;
running `baseline` twice yields byte-identical JSON.
"""

import hashlib
import json
import os
import re
import sys

WINDOW = 8          # significant lines per window (fixed)
MIN_SIG_CHARS = 4   # minimum significant chars for a line to count (fixed)

SRC_EXTS = (".cpp", ".hpp", ".c", ".h")

_IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_LINE_COMMENT_RE = re.compile(r"//.*")
_WS_RE = re.compile(r"\s+")

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(REPO_ROOT, "src")
BASELINE_PATH = os.path.join(REPO_ROOT, "tools", "clone_baseline.json")


def collect_files():
    """Scan set, sorted, as repo-relative POSIX paths."""
    out = []
    for dirpath, dirnames, filenames in os.walk(SRC_DIR):
        # Prune excluded directories deterministically.
        dirnames[:] = sorted(
            d for d in dirnames
            if "X64" not in d and d not in ("build", "generated")
        )
        for name in sorted(filenames):
            if not name.endswith(SRC_EXTS):
                continue
            if "X64" in name:
                continue  # frozen backend: never read by tooling
            abspath = os.path.join(dirpath, name)
            rel = os.path.relpath(abspath, REPO_ROOT).replace(os.sep, "/")
            if "X64" in rel:
                continue
            out.append(rel)
    return sorted(out)


def normalize_lines(text, ident_blind):
    """Return the list of normalized significant lines for one file."""
    lines = []
    for raw in text.splitlines():
        line = _LINE_COMMENT_RE.sub("", raw)
        if ident_blind:
            line = _IDENT_RE.sub("$", line)
        line = _WS_RE.sub(" ", line).strip()
        if len(line) >= MIN_SIG_CHARS:
            lines.append(line)
    return lines


def window_hashes(lines):
    """Yield (index, md5hex) for each WINDOW-line window."""
    for i in range(len(lines) - WINDOW + 1):
        joined = "\n".join(lines[i:i + WINDOW])
        yield i, hashlib.md5(joined.encode("utf-8")).hexdigest()


def compute_pair_counts(ident_blind=True):
    """Return {(fileA, fileB): distinct-clone-window-count} with fileA <= fileB."""
    files = collect_files()
    occurrences = {}  # hash -> list of (relpath, window_start_index)
    for rel in files:
        abspath = os.path.join(REPO_ROOT, rel)
        with open(abspath, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        lines = normalize_lines(text, ident_blind)
        for idx, h in window_hashes(lines):
            occurrences.setdefault(h, []).append((rel, idx))

    pair_counts = {}
    for h in sorted(occurrences):
        occs = sorted(occurrences[h])
        by_file = {}
        for rel, idx in occs:
            by_file.setdefault(rel, []).append(idx)
        fnames = sorted(by_file)
        if len(fnames) >= 2:
            # Cross-file clone: one clone window per involved file pair.
            for i in range(len(fnames)):
                for j in range(i + 1, len(fnames)):
                    key = (fnames[i], fnames[j])
                    pair_counts[key] = pair_counts.get(key, 0) + 1
        for rel in fnames:
            idxs = sorted(by_file[rel])
            # >= 2 disjoint, non-overlapping sites within one file.
            if len(idxs) >= 2 and idxs[-1] - idxs[0] >= WINDOW:
                key = (rel, rel)
                pair_counts[key] = pair_counts.get(key, 0) + 1
    return pair_counts


def pair_key(a, b):
    return a + "|" + b


def counts_as_json_dict(pair_counts):
    return {pair_key(a, b): n for (a, b), n in pair_counts.items()}


def cmd_scan():
    pair_counts = compute_pair_counts(ident_blind=True)
    rows = sorted(pair_counts.items(), key=lambda kv: (-kv[1], kv[0]))
    if not rows:
        print("no clone windows found")
        return 0
    width = max(len(pair_key(a, b)) for (a, b), _ in rows)
    print(f"{'pair':<{width}}  windows (identifier-blind, {WINDOW}-line)")
    print("-" * (width + 40))
    for (a, b), n in rows:
        print(f"{pair_key(a, b):<{width}}  {n}")
    return 0


def cmd_baseline():
    pair_counts = compute_pair_counts(ident_blind=True)
    sys.stdout.write(
        json.dumps(counts_as_json_dict(pair_counts), sort_keys=True, indent=2)
    )
    sys.stdout.write("\n")
    return 0


def cmd_check():
    try:
        with open(BASELINE_PATH, "r", encoding="utf-8") as f:
            baseline = json.load(f)
    except OSError as e:
        print(f"clone-ratchet: cannot read baseline {BASELINE_PATH}: {e}",
              file=sys.stderr)
        return 2
    current = counts_as_json_dict(compute_pair_counts(ident_blind=True))

    offenders = []
    decreases = []
    for key in sorted(set(baseline) | set(current)):
        base = baseline.get(key, 0)
        cur = current.get(key, 0)
        if cur > base:
            offenders.append((key, base, cur))
        elif cur < base:
            decreases.append((key, base, cur))

    for key, base, cur in decreases:
        print(f"note: {key} improved ({base} -> {cur}); "
              f"re-baseline recommended (python3 tools/clonedet.py baseline "
              f"> tools/clone_baseline.json)")
    if offenders:
        print("clone-ratchet FAILED: new cross-file duplication exceeds baseline:")
        for key, base, cur in offenders:
            print(f"  {key}: baseline {base}, now {cur}")
        print("Move the shared logic into RuntimeCore / the owning layer "
              "instead of duplicating it.")
        return 1
    print(f"clone-ratchet OK: {len(current)} pair(s), none exceed baseline.")
    return 0


def main(argv):
    if len(argv) != 2 or argv[1] not in ("scan", "baseline", "check"):
        print("usage: clonedet.py {scan|baseline|check}", file=sys.stderr)
        return 2
    return {"scan": cmd_scan, "baseline": cmd_baseline, "check": cmd_check}[argv[1]]()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
