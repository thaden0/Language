#pragma once
#include <cstdint>
#include <string>

// A half-open byte range [offset, offset+length) into a SourceFile's text.
struct SourceSpan {
    uint32_t offset = 0;
    uint32_t length = 0;

    uint32_t end() const { return offset + length; }
};

// Owns the full text of one source file. Tokens and diagnostics refer back
// into `text` by SourceSpan / std::string_view, so a SourceFile must outlive
// everything the front-end produces from it.
struct SourceFile {
    std::string name;
    std::string text;
};

// 1-based line and column, for human-facing diagnostics.
struct LineCol {
    uint32_t line = 1;
    uint32_t col = 1;
};

// Compute 1-based line/column for a byte offset. Linear scan — fine for
// diagnostics (cold path); we can add a cached line table if it ever matters.
LineCol lineColAt(const std::string& text, uint32_t offset);
