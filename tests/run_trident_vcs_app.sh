#!/usr/bin/env bash
# GT3 acceptance (techdesign-package-manager.md §5.5): a project whose
# trident.toml gets a [[dep]] added via `trident add` for a real VCS path +
# version resolves via MVS, fetches into the content-addressed store, writes
# trident.lock, and builds — the fetched module's namespace compiles in.
# `trident why` explains the selection.
#
# Runs entirely offline (H-7 — no test may reach the real network): git's
# own url.insteadOf rewrite (via a throwaway $GIT_CONFIG_GLOBAL) redirects
# the fixture's "github.com/x/json"-looking path to the local bare-repo
# fixture (tests/trident/store/fixture_json.git) transparently — trident's
# own code never knows the difference.
#
# Operates on a scratch COPY of the vcs_app fixture (matching this repo's
# other tests/run_*.sh convention of a mktemp -d work dir), not the tracked
# source directory — `trident add` edits trident.toml and writes
# trident.lock, and a repeated ctest run must not accumulate diffs in the
# tracked fixture.
leviathan="$1"; trident="$2"; fixture_dir="$3"; bare_repo="$4"
fail=0
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

cp -r "$fixture_dir" "$work/vcs_app"
cd "$work/vcs_app" || { echo "FAIL vcs_app (cannot cd into scratch copy)"; exit 1; }

cat > "$work/gitconfig" <<EOF
[url "file://$bare_repo"]
	insteadOf = https://github.com/x/json
EOF
export GIT_CONFIG_GLOBAL="$work/gitconfig"
export TRIDENT_HOME="$work/trident-home"

if ! "$trident" add github.com/x/json@1.1.0 --as Json >"$work/add.out" 2>"$work/add.err"; then
    echo "FAIL vcs_app (trident add)"; cat "$work/add.err"; fail=1
fi

if [ ! -f trident.lock ]; then
    echo "FAIL vcs_app (trident.lock not written by add)"; fail=1
fi

if ! "$trident" build . --leviathan "$leviathan" >"$work/build.out" 2>"$work/build.err"; then
    echo "FAIL vcs_app (trident build)"; cat "$work/build.err"; fail=1
fi

if [ -x ./vcs_app ]; then
    got=$(./vcs_app 2>/dev/null)
    want=$(cat expected.txt)
    if [ "$got" != "$want" ]; then
        echo "FAIL vcs_app (program output)"; diff <(echo "$want") <(echo "$got"); fail=1
    fi
else
    echo "FAIL vcs_app (build did not produce an executable)"; fail=1
fi

why_out=$("$trident" why github.com/x/json 2>"$work/why.err")
why_rc=$?
if [ $why_rc -ne 0 ] || ! grep -qF "github.com/x/json@1.1.0" <<<"$why_out"; then
    echo "FAIL vcs_app (trident why)"; cat "$work/why.err"; echo "$why_out"; fail=1
fi

# G-CYC1: a lock-verbatim resolution rejects a hand-edited cycle before
# materialization, and the named repair (`trident lock`) really repairs it.
if [ -f trident.lock ]; then
    sed -i '/^hash = /a requires = ["github.com/x/json@1.1.0"]' trident.lock
    if "$trident" build . --leviathan "$leviathan" >"$work/cycle.out" 2>"$work/cycle.err"; then
        echo "FAIL vcs_app (cyclic lock was accepted)"; fail=1
    else
        if ! grep -qF "require cycle" "$work/cycle.err"; then
            echo "FAIL vcs_app (cyclic lock error omitted cycle)"; cat "$work/cycle.err"; fail=1
        fi
        if ! grep -qF 'run `trident lock`' "$work/cycle.err"; then
            echo "FAIL vcs_app (cyclic lock error omitted repair)"; cat "$work/cycle.err"; fail=1
        fi
    fi

    if ! "$trident" lock . >"$work/relock.out" 2>"$work/relock.err"; then
        echo "FAIL vcs_app (trident lock did not repair cycle)"; cat "$work/relock.err"; fail=1
    elif ! "$trident" build . --leviathan "$leviathan" \
            >"$work/rebuild.out" 2>"$work/rebuild.err"; then
        echo "FAIL vcs_app (build after cycle repair)"; cat "$work/rebuild.err"; fail=1
    fi
fi

if [ $fail -eq 0 ]; then echo "trident vcs_app (P2.1e/GT3 + G-CYC1) integration checked"; fi
exit $fail
