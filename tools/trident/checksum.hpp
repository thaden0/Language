#pragma once
#include "provider.hpp"
#include <string>

// -----------------------------------------------------------------------------
// tools/trident/checksum.{hpp,cpp} — the checksum DB (techdesign-package-
// manager.md §6 P2.2, GT4): an append-only, tamper-evident log of
// `module@version -> sha256(content)`, checked on every fetch alongside
// trident.lock (proposal §4.5: "on first fetch, record the hash; on every
// later fetch, verify against the log and the lockfile... tamper-evident
// without trusting the proxy or the git host"). Written behind this small
// interface (§9) so the baseline local-file backend can grow into a full
// Merkle transparency log (P2.4) without touching callers.
//
// Deliberate simplification, not a gap: the design's prose calls the
// baseline a "signed" file, but real signing (asymmetric keys, a trust
// root) is explicitly Sigstore/P2.4 territory (proposal §4.4, techdesign
// §6 P2.4). What a purely LOCAL file needs today is tamper-evidence, not
// authentication of a remote signer — so the baseline here hash-chains
// each record to the previous one (each entry's own hash covers the prior
// entry's hash, Merkle-log style) using the same hand-rolled SHA-256
// already in hash.hpp. Reordering, editing, or truncating any past entry
// breaks the chain and is caught on load. Growing this into a real
// Merkle transparency log (P2.4) only needs a new backend behind this same
// interface; callers never see the difference.
// -----------------------------------------------------------------------------

// `$TRIDENT_HOME/checksum.db`, or `~/.trident/checksum.db` if unset (mirrors
// store.hpp's storeRoot()).
std::string checksumDbPath();

// Verify `contentHash` for `mod`@`version` against the checksum DB at
// `dbPath`, recording it if this is the first time this exact module@version
// has ever been seen:
//   - No existing entry: append a new hash-chained record. ok = true (this
//     fetch becomes the trusted baseline for every future fetch).
//   - An existing entry with a MATCHING hash: ok = true, no write.
//   - An existing entry with a DIFFERENT hash: ok = false, `err` names the
//     module/version and both hashes — a moved tag or swapped content.
// Also verifies the on-disk chain is internally consistent before trusting
// any existing entry; a broken chain (the log itself was tampered with) is
// its own loud `err`, ok = false.
bool checksumDbVerifyOrRecord(const std::string& dbPath, const ModuleId& mod,
                              const Version& version, const std::string& contentHash,
                              std::string& err);

// GT5 yank metadata is another append-only event in the SAME hash-chained
// transparency log. A yank never deletes or changes the recorded content
// hash: it only blocks a fresh MVS selection. Builds using an existing lock
// continue to verify and materialize the version normally.
bool checksumDbYank(const std::string& dbPath, const ModuleId& mod,
                    const Version& version, std::string& err);
bool checksumDbIsYanked(const std::string& dbPath, const ModuleId& mod,
                        const Version& version, bool& yanked, std::string& err);
