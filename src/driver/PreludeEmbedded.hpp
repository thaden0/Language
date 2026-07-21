#pragma once
#include <cstddef>

// Generated table of prelude segments (build/generated/PreludeEmbedded.cpp,
// produced by cmake/GenPreludeEmbedded.cmake from prelude/*.lev). Order is
// the canonical concatenation order. `data` is the embedded fallback copy of
// `prelude/<name>.lev`, byte-identical by construction (generated from it).
struct PreludeSegment {
    const char* name;          // segment stem: file is prelude/<name>.lev
    bool wasmOnly;             // include only for wasm32* target triples
    const unsigned char* data; // embedded fallback bytes (not NUL-terminated)
    unsigned long size;
};
extern const PreludeSegment kPreludeSegments[];
extern const unsigned long kPreludeSegmentCount;
