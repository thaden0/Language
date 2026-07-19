#!/usr/bin/env bash
# P2.2 GT4 acceptance (techdesign-package-manager.md §6): `trident audit`
# verifies fetched content against the checksum DB + lock (and catches
# tampering), and `trident vendor` + `--vendor` produce a genuinely
# network-free build.
#
# Runs entirely offline (H-7): the initial `add`/`lock` step uses the same
# git url.insteadOf redirect to the local bare-repo fixture as
# run_trident_vcs_app.sh. The `--vendor` build step then runs with a fake
# `git` shadowing the real one at the front of PATH (findOnPath(),
# tools/trident/vcs.cpp, scans PATH left-to-right) that always fails — if
# VendorProvider ever fell through to GitProvider, that build would fail
# loudly instead of silently reaching the network. The real compiler stays
# reachable (only `git` is shadowed), so --build-native still works.
#
# Operates on a scratch COPY of the vcs_app fixture (same convention as
# run_trident_vcs_app.sh) — trident add/lock/vendor all write into it.
leviathan="$1"; trident="$2"; fixture_dir="$3"; bare_repo="$4"
fail=0
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

cp -r "$fixture_dir" "$work/vendor_app"
cd "$work/vendor_app" || { echo "FAIL vendor_app (cannot cd into scratch copy)"; exit 1; }

cat > "$work/gitconfig" <<EOF
[url "file://$bare_repo"]
	insteadOf = https://github.com/x/json
EOF
export GIT_CONFIG_GLOBAL="$work/gitconfig"
export TRIDENT_HOME="$work/trident-home"

mkdir -p "$work/fakebin"
cat > "$work/fakebin/git" <<'EOF'
#!/bin/sh
echo "git: blocked for the --vendor network-free test" >&2
exit 127
EOF
chmod +x "$work/fakebin/git"

if ! "$trident" add github.com/x/json@1.1.0 --as Json >"$work/add.out" 2>"$work/add.err"; then
    echo "FAIL vendor_app (trident add)"; cat "$work/add.err"; fail=1
fi

# --- audit: a clean lock verifies OK ---
if ! "$trident" audit . >"$work/audit1.out" 2>"$work/audit1.err"; then
    echo "FAIL vendor_app (trident audit on a clean lock)"; cat "$work/audit1.err"; fail=1
elif ! grep -q "audit passed" "$work/audit1.out"; then
    echo "FAIL vendor_app (trident audit did not report success)"; cat "$work/audit1.out"; fail=1
fi

# --- audit: tampering with the checksum DB's recorded hash is caught ---
sed -i 's/[0-9a-f]\{64\}$/deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef/' \
    "$TRIDENT_HOME/checksum.db"
if "$trident" audit . >"$work/audit2.out" 2>"$work/audit2.err"; then
    echo "FAIL vendor_app (trident audit should have caught the tampered checksum DB)"; fail=1
elif ! grep -qi "tamper\|does not match\|moved" "$work/audit2.err" "$work/audit2.out"; then
    echo "FAIL vendor_app (audit failure did not explain the mismatch)"
    cat "$work/audit2.err" "$work/audit2.out"; fail=1
fi
rm -rf "$TRIDENT_HOME"; mkdir -p "$TRIDENT_HOME"
if ! "$trident" lock >"$work/relock.out" 2>"$work/relock.err"; then
    echo "FAIL vendor_app (re-lock after resetting the checksum DB)"; cat "$work/relock.err"; fail=1
fi

# --- vendor: copy the resolved module into ./vendor ---
if ! "$trident" vendor . >"$work/vendor.out" 2>"$work/vendor.err"; then
    echo "FAIL vendor_app (trident vendor)"; cat "$work/vendor.err"; fail=1
fi
if [ ! -d vendor/github.com/x/json ]; then
    echo "FAIL vendor_app (vendor did not lay down vendor/github.com/x/json)"; fail=1
fi

# --- --vendor build: no git, no network ---
unset GIT_CONFIG_GLOBAL
if ! PATH="$work/fakebin:$PATH" "$trident" build . --leviathan "$leviathan" --vendor \
        >"$work/build.out" 2>"$work/build.err"; then
    echo "FAIL vendor_app (trident build --vendor)"; cat "$work/build.err"; fail=1
fi

# The output binary is named after trident.toml's `name` field ("vcs_app",
# inherited unchanged from the fixture) — not this scratch dir's name.
if [ -x ./vcs_app ]; then
    got=$(./vcs_app 2>/dev/null)
    want=$(cat expected.txt)
    if [ "$got" != "$want" ]; then
        echo "FAIL vendor_app (program output)"; diff <(echo "$want") <(echo "$got"); fail=1
    fi
else
    echo "FAIL vendor_app (--vendor build did not produce an executable)"; fail=1
fi

# --- a COLD build without --vendor and without the git redirect fails
#     offline (a warm content-addressed store is intentionally network-free
#     too, so remove only that cache for this sanity check). ---
rm -rf build
rm -rf "$TRIDENT_HOME/store"
if PATH="$work/fakebin:$PATH" "$trident" build . --leviathan "$leviathan" \
        >"$work/nonvendor.out" 2>"$work/nonvendor.err"; then
    echo "FAIL vendor_app (a non-vendor build with git blocked should have failed)"; fail=1
fi

if [ $fail -eq 0 ]; then echo "trident vendor/audit (P2.2/GT4) integration checked"; fi
exit $fail
