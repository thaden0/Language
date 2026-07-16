#pragma once
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// tools/trident/store.{hpp,cpp} — the content-addressed store (techdesign-
// package-manager.md §3.6/§5.2 P2.1b): `$TRIDENT_HOME/store/<sha256>/`
// (default `~/.trident/store/<sha256>/`). Canonicalization is fixed HERE and
// must never change (H-3): sort files by relative path, then for each (in
// that order) feed [relPath, a single 0x00 separator, file bytes] into one
// SHA-256 stream (hash.hpp). Every later fetch must reproduce this exact
// hash, or it is a loud integrity error (checksum DB, P2.2).
//
// `contentHash`/`storeDir`'s `<sha256>` here is the BARE lowercase hex digest
// (no "sha256:" prefix) — that prefix is purely a display/lockfile-text
// convention (lock.cpp, P2.1d), not part of the hash or the store's
// directory naming.
// -----------------------------------------------------------------------------

// One file to materialize: `relPath` is the name recorded in the hash and
// used as the on-disk path inside the store entry (e.g. "json.lev",
// "sub/util.lev"); `absPath` is where to read its current bytes from right
// now (e.g. a temp git checkout, P2.1c).
struct StoreFile {
    std::string relPath;
    std::string absPath;
};

// `$TRIDENT_HOME/store`, or `~/.trident/store` if `$TRIDENT_HOME` is unset.
std::string storeRoot();

// Compute the canonical content hash of `files` (order-independent — sorted
// internally by `relPath`) without writing anything. Returns false and sets
// `err` if any file cannot be read.
bool canonicalContentHash(std::vector<StoreFile> files, std::string& hash, std::string& err);

// Materialize `files` under the store, keyed by their canonical content hash.
// Idempotent: if `storeRoot()/<hash>/` already exists, this is a no-op (the
// fetch is skipped, `storeDir`/`contentHash` are still filled in). Writes to
// a temp sibling directory and renames into place, so a crash mid-copy never
// leaves a partial hash directory that a later run would mistake for a valid
// cache hit.
bool materializeToStore(std::vector<StoreFile> files, std::string& storeDir,
                        std::string& contentHash, std::string& err);
