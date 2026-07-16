#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// -----------------------------------------------------------------------------
// tools/trident/hash.{hpp,cpp} — a hand-rolled SHA-256 (techdesign-package-
// manager.md §5.2 P2.1b). No crypto library dependency — trident stays
// dependency-free like the rest of the toolchain (there is no hash code
// anywhere else in the tree). Streaming (init/update/final) so store.cpp can
// feed a canonicalized sequence of (path, bytes) pairs without concatenating
// every dependency's source into one giant in-memory buffer first.
// -----------------------------------------------------------------------------

class Sha256 {
public:
    Sha256();

    void update(const void* data, size_t len);
    void update(const std::string& s) { update(s.data(), s.size()); }

    // Finalize and write the 32-byte digest to `out`. Only call once.
    void final(unsigned char out[32]);

    // Convenience: final() + lowercase hex encoding (64 chars).
    std::string finalHex();

private:
    uint32_t state_[8];
    uint64_t bitLen_ = 0;
    unsigned char buffer_[64];
    size_t bufferLen_ = 0;

    void processBlock(const unsigned char block[64]);
};

// One-shot convenience: hash `data`/`s` and return the lowercase hex digest.
std::string sha256Hex(const void* data, size_t len);
std::string sha256Hex(const std::string& s);
