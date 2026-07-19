#!/usr/bin/env bash
# P2.3/GT5 optional caching-proxy acceptance. Builds the static proxy layout
# from the local git fixture, then shadows git with a failing executable.
# `trident add` (MVS manifest read + source materialization) and the compiled
# application must still succeed, proving the proxy is a real provider and
# not a GitProvider fallback.
leviathan="$1"; trident="$2"; fixture_dir="$3"; bare_repo="$4"
fail=0
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

cp -r "$fixture_dir" "$work/proxy_app"
mkdir -p "$work/proxy-src"
git --git-dir="$bare_repo" show v1.1.0:trident.toml > "$work/proxy-src/trident.toml"
git --git-dir="$bare_repo" show v1.1.0:json.lev > "$work/proxy-src/json.lev"

module_key='github.com%2Fx%2Fjson'
version_dir="$work/proxy/modules/$module_key/@v"
mkdir -p "$version_dir"
printf 'v1.0.0\nv1.1.0\n' > "$version_dir/list"
cp "$work/proxy-src/trident.toml" "$version_dir/v1.1.0.toml"
tar --create --file "$version_dir/v1.1.0.tar" --directory "$work/proxy-src" json.lev

mkdir -p "$work/fakebin"
cat > "$work/fakebin/git" <<EOF
#!/usr/bin/env bash
echo called >> "$work/git-called"
exit 91
EOF
chmod +x "$work/fakebin/git"
export PATH="$work/fakebin:$PATH"
export TRIDENT_PROXY="$work/proxy"
export TRIDENT_HOME="$work/trident-home"

cd "$work/proxy_app" || exit 1
if ! "$trident" add github.com/x/json@1.1.0 --as Json >"$work/add.out" 2>"$work/add.err"; then
    echo "FAIL proxy_app (proxy-backed add)"; cat "$work/add.err"; fail=1
fi
if [ -e "$work/git-called" ]; then
    echo "FAIL proxy_app (Trident invoked git despite TRIDENT_PROXY)"; fail=1
fi
if [ ! -f trident.lock ]; then
    echo "FAIL proxy_app (proxy-backed add did not write trident.lock)"; fail=1
fi

# Once the lock and verified content-addressed entry exist, neither the
# proxy nor upstream Git is needed for ordinary builds.
unset TRIDENT_PROXY
if ! "$trident" build . --leviathan "$leviathan" >"$work/build.out" 2>"$work/build.err"; then
    echo "FAIL proxy_app (warm-store build with proxy and git unavailable)"; cat "$work/build.err"; fail=1
elif [ ! -x ./vcs_app ]; then
    echo "FAIL proxy_app (build did not produce an executable)"; fail=1
else
    got=$(./vcs_app 2>/dev/null)
    want=$(cat expected.txt)
    if [ "$got" != "$want" ]; then
        echo "FAIL proxy_app (program output)"; diff <(echo "$want") <(echo "$got"); fail=1
    fi
fi
if [ -e "$work/git-called" ]; then
    echo "FAIL proxy_app (build invoked git despite warm proxy/store)"; fail=1
fi

if [ $fail -eq 0 ]; then echo "trident optional proxy (P2.3/GT5) integration checked"; fi
exit $fail
