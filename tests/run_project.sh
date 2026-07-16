#!/usr/bin/env bash
# Multi-file project runner: for every project under a directory (each a folder
# with a manifest), resolve it through trident into a build plan (§3.3,
# techdesign-toolchain.md), then build that plan through the oracle, the IR
# interpreter, and the pure ELF backend, diffing each engine's stdout against
# expected.txt. All three must agree (the §17 one-IR contract) on multi-file
# gather. trident is the front door (GT1) — leviathan never sees the manifest.
#
# Also asserts the project build is byte-identical to the plain concatenation of
# its sources in manifest order (the §12 "file boundaries dissolve" invariant).
bin="$1"; trident="$2"; root="$3"; fail=0; n=0
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

for mf in "$root"/*/trident.toml; do
  [ -e "$mf" ] || continue
  dir=$(dirname "$mf"); name=$(basename "$dir")

  plan="$work/$name.lvplan"
  if ! "$trident" plan "$dir" --plan "$plan" --leviathan "$bin" \
       >/dev/null 2>"$work/$name.resolve.err"; then
    echo "FAIL $name (trident plan resolve failed)"; cat "$work/$name.resolve.err"
    fail=1; n=$((n+1)); continue
  fi

  # Negative test: a project with expected.error must FAIL to compile, with the
  # file's text appearing in stderr (a diagnostic substring).
  if [ -f "$dir/expected.error" ]; then
    n=$((n+1)); esub=$(cat "$dir/expected.error")
    err=$("$bin" --run --plan "$plan" 2>&1 >/dev/null)
    if "$bin" --run --plan "$plan" >/dev/null 2>&1; then
      echo "FAIL $name (expected an error, but it compiled)"; fail=1
    elif ! grep -qF "$esub" <<<"$err"; then
      echo "FAIL $name (error did not match '$esub')"; echo "$err" | head -3; fail=1
    fi
    continue
  fi

  exp="$dir/expected.txt"
  if [ ! -f "$exp" ]; then echo "FAIL $name (no expected.txt)"; fail=1; continue; fi
  n=$((n+1)); want=$(cat "$exp")

  # oracle + IR interpreter
  for flag in --run --ir; do
    got=$("$bin" $flag --plan "$plan" 2>/dev/null)
    if [ "$got" != "$want" ]; then
      echo "FAIL $name ($flag)"; diff <(echo "$want") <(echo "$got") | head -8; fail=1
    fi
  done

  # pure ELF backend: emit, run the binary, diff stdout (the [heap] meter is on
  # stderr, so it does not perturb the comparison)
  if "$bin" --emit-elf "$work/$name" --plan "$plan" 2>"$work/$name.err"; then
    chmod +x "$work/$name"; got=$("$work/$name" 2>/dev/null)
    if [ "$got" != "$want" ]; then
      echo "FAIL $name (--emit-elf)"; diff <(echo "$want") <(echo "$got") | head -8; fail=1
    fi
  else
    if grep -qE "native-elf backend|not yet lowerable" "$work/$name.err"; then
      echo "SKIP (beyond ELF coverage): $name"
    else
      echo "FAIL $name (emit-elf)"; cat "$work/$name.err"; fail=1
    fi
  fi

  # P-4 provenance: if pinned, compare the file -> imports map. Source paths are
  # normalized to basenames (they vary by checkout location).
  if [ -f "$dir/expected.imports" ]; then
    igot=$("$bin" --imports --plan "$plan" 2>/dev/null | sed 's#\[imports\] .*/\([^/]*\.ext\)$#[imports] \1#')
    iwant=$(cat "$dir/expected.imports")
    if [ "$igot" != "$iwant" ]; then
      echo "FAIL $name (--imports provenance)"; diff <(echo "$iwant") <(echo "$igot") | head -12; fail=1
    fi
  fi

  # P-3 include graph: if pinned, compare the `uses` graph + build order. Path
  # tokens (the *.ext basenames) are normalized to strip the checkout directory,
  # leaving prose (e.g. the "declared-in / imported-by" header) untouched.
  if [ -f "$dir/expected.graph" ]; then
    ggot=$("$bin" --graph --plan "$plan" 2>/dev/null \
           | sed -E 's#[^ ,]*/([^ ,/]+\.ext)#\1#g')
    gwant=$(cat "$dir/expected.graph")
    if [ "$ggot" != "$gwant" ]; then
      echo "FAIL $name (--graph include graph)"; diff <(echo "$gwant") <(echo "$ggot") | head -16; fail=1
    fi
  fi

  # Discoverability queries (proposal §4.4): --namespaces (the symbol index) and
  # --why (name provenance). Path tokens are normalized to basenames as above so
  # the pins are checkout-location independent. --why reads its query args (a
  # name, optionally `in <file>`) from a why.query sidecar.
  if [ -f "$dir/expected.namespaces" ]; then
    ngot=$("$bin" --namespaces --plan "$plan" 2>/dev/null \
           | sed -E 's#[^ ,]*/([^ ,/]+\.(ext|lev))#\1#g')
    nwant=$(cat "$dir/expected.namespaces")
    if [ "$ngot" != "$nwant" ]; then
      echo "FAIL $name (--namespaces)"; diff <(echo "$nwant") <(echo "$ngot") | head -16; fail=1
    fi
  fi
  if [ -f "$dir/expected.why" ] && [ -f "$dir/why.query" ]; then
    # shellcheck disable=SC2046 -- deliberate word-split: the query is CLI args.
    wgot=$("$bin" --why $(cat "$dir/why.query") --plan "$plan" 2>/dev/null \
           | sed -E 's#[^ ,]*/([^ ,/]+\.(ext|lev))#\1#g')
    wwant=$(cat "$dir/expected.why")
    if [ "$wgot" != "$wwant" ]; then
      echo "FAIL $name (--why)"; diff <(echo "$wwant") <(echo "$wgot") | head -16; fail=1
    fi
  fi

  # LA-20 §12 item 5: `--assets` golden (path, byte count, hash presence in
  # plan mode, owning module) — deterministic across two runs. The `module`
  # field names a dep's absolute store/checkout directory, checkout-location
  # dependent like every other path token here, so it is normalized to its
  # basename the same way source paths are; a root-project asset's module
  # ("") has no '/' to match and is left alone.
  if [ -f "$dir/expected.assets" ]; then
    asgot1=$("$bin" --assets --plan "$plan" 2>/dev/null \
             | sed -E 's#\(module "[^"]*/([^"/]+)"\)#(module ".../\1")#')
    asgot2=$("$bin" --assets --plan "$plan" 2>/dev/null \
             | sed -E 's#\(module "[^"]*/([^"/]+)"\)#(module ".../\1")#')
    aswant=$(cat "$dir/expected.assets")
    if [ "$asgot1" != "$aswant" ]; then
      echo "FAIL $name (--assets)"; diff <(echo "$aswant") <(echo "$asgot1") | head -16; fail=1
    elif [ "$asgot1" != "$asgot2" ]; then
      echo "FAIL $name (--assets not deterministic across two runs)"; fail=1
    fi
  fi

  # Optional folder~namespace lint (proposal §4.4, opt-in): if pinned, compare the
  # report. `--lint-namespaces` exits non-zero when it finds a mismatch, so the
  # $(...) captures stdout while the exit code is intentionally ignored here.
  if [ -f "$dir/expected.lint" ]; then
    lgot=$("$bin" --lint-namespaces --plan "$plan" 2>/dev/null \
           | sed -E 's#[^ ,]*/([^ ,/]+\.(ext|lev))#\1#g')
    lwant=$(cat "$dir/expected.lint")
    if [ "$lgot" != "$lwant" ]; then
      echo "FAIL $name (--lint-namespaces)"; diff <(echo "$lwant") <(echo "$lgot") | head -16; fail=1
    fi
  fi

  # concatenation equivalence: the manifest build must equal the sources cat'd
  # together in listed order (file boundaries dissolve at gather time, §12).
  # Skipped when the manifest sets an `entry`: entry mode changes what executes
  # (a synthesized call, or one file's top-level only), so a raw concatenation is
  # not the equivalent program. The oracle==IR==ELF checks still cover it.
  #
  # Also skipped when the test pins a `skip_concat` marker (first line = reason).
  # Per-file `uses` scoping (P-4/§16.5) makes file layout itself observable: a
  # file sees declaresInto ∪ uses, so once a comptime-selected `uses` picks
  # between same-named symbols declared by OTHER files, the concatenation — which
  # now declares both itself — sees both unconditionally. That is a different
  # program (ambiguous attributes / different rule firings), not an equivalent
  # one, so the invariant does not apply. The oracle==IR==ELF checks still cover it.
  if [ -f "$dir/skip_concat" ]; then
    echo "SKIP (concat n/a): $name — $(head -1 "$dir/skip_concat")"
  elif ! grep -qE 'entry[[:space:]]*=' "$mf"; then
  # Read entries into an array (mapfile) so a glob entry like *.ext is NOT
  # expanded here against the cwd; it is expanded per-entry against $dir below,
  # the same alphabetical way the loader's glob() does.
  mapfile -t entries < <(sed -n 's/.*sources[^[]*\[\([^]]*\)\].*/\1/p' "$mf" \
                         | tr ',' '\n' | sed 's/[" ]//g' | grep -v '^$')
  if [ ${#entries[@]} -gt 0 ]; then
    : > "$work/$name.concat.ext"
    for s in "${entries[@]}"; do
      for f in $dir/$s; do
        [ -f "$f" ] && { cat "$f" >> "$work/$name.concat.ext"; echo >> "$work/$name.concat.ext"; }
      done
    done
    cgot=$("$bin" --run "$work/$name.concat.ext" 2>/dev/null)
    if [ "$cgot" != "$want" ]; then
      echo "FAIL $name (project build != concatenated single-file)"
      diff <(echo "$want") <(echo "$cgot") | head -8; fail=1
    fi
  fi
  fi
done

echo "$n multi-file project(s) checked (oracle == IR == ELF == concatenation)"
exit $fail
