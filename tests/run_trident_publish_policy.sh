#!/usr/bin/env bash
# P2.3/GT5 + P2.4/GT6 acceptance: publish an immutable git tag, register a
# friendly name in the optional first-wins index, consume it, record a human
# audit, verify a public-key-signed source attestation, then yank the version.
# A fresh resolution must reject the yank while the committed lock remains
# buildable/auditable.
trident="$1"; fixture_dir="$2"
fail=0
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
export TRIDENT_HOME="$work/trident-home"
export TRIDENT_INDEX="$work/index"

mkdir -p "$work/package" "$work/consumer/attestations" "$work/consumer/keys"
cat > "$work/package/trident.toml" <<'EOF'
name = "json"
version = "1.2.0"
sources = ["json.lev"]
EOF
cat > "$work/package/json.lev" <<'EOF'
namespace Json {
    public string encode(int n) => "{\"value\":" + n + "}";
    public string tag(string s) => "<" + s + ">";
}
EOF
git -C "$work/package" init --quiet
git -C "$work/package" config user.email test@example.invalid
git -C "$work/package" config user.name "Trident Test"
git -C "$work/package" add trident.toml json.lev
git -C "$work/package" commit --quiet -m fixture

openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 \
    -out "$work/private.pem" >/dev/null 2>&1
openssl pkey -in "$work/private.pem" -pubout \
    -out "$work/consumer/keys/acme.pem" >/dev/null 2>&1

attestation="$work/consumer/attestations/json.attestation"
if ! (cd "$work/package" && "$trident" publish . --path github.com/acme/json \
        --sign-key "$work/private.pem" --identity acme-ci \
        --attestation-out "$attestation") >"$work/publish.out" 2>"$work/publish.err"; then
    echo "FAIL publish_policy (publish)"; cat "$work/publish.err"; fail=1
fi
if ! git -C "$work/package" rev-parse --verify refs/tags/v1.2.0 >/dev/null 2>&1; then
    echo "FAIL publish_policy (publish did not create v1.2.0)"; fail=1
fi
if [ "$(cat "$work/index/names/json" 2>/dev/null)" != "github.com/acme/json" ]; then
    echo "FAIL publish_policy (optional index mapping)"; fail=1
fi
if [ ! -s "$attestation" ]; then
    echo "FAIL publish_policy (signed attestation missing)"; fail=1
fi
# Publish is retry-safe when the immutable tag already names the same commit.
if ! (cd "$work/package" && "$trident" publish . --path github.com/acme/json \
        --sign-key "$work/private.pem" --identity acme-ci \
        --attestation-out "$attestation") >/dev/null 2>"$work/republish.err"; then
    echo "FAIL publish_policy (idempotent republish)"; cat "$work/republish.err"; fail=1
fi

cp -r "$fixture_dir"/. "$work/consumer/"
cat > "$work/gitconfig" <<EOF
[url "file://$work/package"]
	insteadOf = https://github.com/acme/json
EOF
export GIT_CONFIG_GLOBAL="$work/gitconfig"

cd "$work/consumer" || exit 1
if ! "$trident" add json@1.2.0 --as Json >"$work/add.out" 2>"$work/add.err"; then
    echo "FAIL publish_policy (add through thin index)"; cat "$work/add.err"; fail=1
fi
if ! grep -q 'path = "github.com/acme/json"' trident.toml; then
    echo "FAIL publish_policy (friendly name was not resolved to explicit VCS path)"; fail=1
fi

# Editing a requirement without re-locking is a loud stale-lock error; build
# commands never silently resolve around a committed lock.
cp trident.toml "$work/manifest.good"
sed -i 's/version = "1.2.0"/version = "1.1.0"/' trident.toml
if "$trident" plan . --plan "$work/stale.lvplan" >"$work/stale.out" 2>"$work/stale.err"; then
    echo "FAIL publish_policy (stale lock was silently accepted)"; fail=1
elif ! grep -qi 'stale.*trident lock' "$work/stale.err"; then
    echo "FAIL publish_policy (stale-lock diagnostic omitted the fix)"; cat "$work/stale.err"; fail=1
fi
cp "$work/manifest.good" trident.toml

if ! "$trident" audit-record github.com/acme/json@1.2.0 --auditor security-team \
        --file trident.audits.toml . >"$work/record.out" 2>"$work/record.err"; then
    echo "FAIL publish_policy (audit-record)"; cat "$work/record.err"; fail=1
fi
cat > trident.audit.toml <<'EOF'
version = 1
trusted_auditors = ["security-team"]
audit_files = ["trident.audits.toml"]
require_attestations = true
attestation_dirs = ["attestations"]

[[key]]
identity = "acme-ci"
public_key = "keys/acme.pem"
EOF

if ! "$trident" audit . >"$work/audit.out" 2>"$work/audit.err"; then
    echo "FAIL publish_policy (trusted audit + attestation policy)"; cat "$work/audit.err"; fail=1
elif ! grep -q 'AUDIT github.com/acme/json@1.2.0 by security-team' "$work/audit.out" || \
     ! grep -q 'ATTEST github.com/acme/json@1.2.0 by acme-ci' "$work/audit.out"; then
    echo "FAIL publish_policy (policy report omitted accepted evidence)"; cat "$work/audit.out"; fail=1
fi

cp trident.audits.toml "$work/audits.good"
sed -i 's/security-team/untrusted/' trident.audits.toml
if "$trident" audit . --policy trident.audit.toml >"$work/audit_bad.out" 2>"$work/audit_bad.err"; then
    echo "FAIL publish_policy (untrusted audit should fail policy)"; fail=1
fi
cp "$work/audits.good" trident.audits.toml

cp "$attestation" "$work/attestation.good"
sed -i 's/^signature = .*/signature = "00"/' "$attestation"
if "$trident" audit . --policy trident.audit.toml >"$work/sig_bad.out" 2>"$work/sig_bad.err"; then
    echo "FAIL publish_policy (tampered attestation should fail policy)"; fail=1
fi
cp "$work/attestation.good" "$attestation"

if ! "$trident" yank github.com/acme/json@1.2.0 >"$work/yank.out" 2>"$work/yank.err"; then
    echo "FAIL publish_policy (yank)"; cat "$work/yank.err"; fail=1
fi
# Existing lock remains valid after yank.
if ! "$trident" audit . >"$work/audit_yanked.out" \
        2>"$work/audit_yanked.err"; then
    echo "FAIL publish_policy (existing lock did not survive yank)"; cat "$work/audit_yanked.err"; fail=1
fi
# Commands that explicitly re-resolve may not newly select the yanked tag.
if "$trident" fetch . >"$work/fetch_yanked.out" 2>"$work/fetch_yanked.err"; then
    echo "FAIL publish_policy (fresh resolution selected a yanked version)"; fail=1
elif ! grep -qi 'yanked' "$work/fetch_yanked.err"; then
    echo "FAIL publish_policy (yank rejection was not explained)"; cat "$work/fetch_yanked.err"; fail=1
fi

if [ $fail -eq 0 ]; then echo "trident publish/index/yank/provenance (GT5-GT6) checked"; fi
exit $fail
