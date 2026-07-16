#include "Diagnostic.hpp"
#include <cstdio>

LineCol lineColAt(const std::string& text, uint32_t offset) {
    LineCol lc;
    uint32_t limit = offset < text.size() ? offset : (uint32_t)text.size();
    for (uint32_t i = 0; i < limit; ++i) {
        if (text[i] == '\n') {
            ++lc.line;
            lc.col = 1;
        } else {
            ++lc.col;
        }
    }
    return lc;
}

static const char* severityName(Severity s) {
    switch (s) {
        case Severity::Error:   return "error";
        case Severity::Warning: return "warning";
        case Severity::Note:    return "note";
    }
    return "?";
}

// Return the [start,end) byte range of the line containing `offset`.
static std::pair<uint32_t, uint32_t> lineExtent(const std::string& text, uint32_t offset) {
    uint32_t start = offset;
    while (start > 0 && text[start - 1] != '\n') --start;
    uint32_t end = offset;
    while (end < text.size() && text[end] != '\n') ++end;
    return {start, end};
}

void DiagnosticSink::render(const SourceFile& file) const {
    for (const Diagnostic& d : diags_) {
        if (d.span.offset > file.text.size()) {   // span from another unit (prelude)
            std::fprintf(stderr, "%s: %s: %s\n", file.name.c_str(),
                         severityName(d.severity), d.message.c_str());
            continue;
        }
        LineCol lc = lineColAt(file.text, d.span.offset);
        std::fprintf(stderr, "%s:%u:%u: %s: %s\n",
                     file.name.c_str(), lc.line, lc.col,
                     severityName(d.severity), d.message.c_str());

        auto [start, end] = lineExtent(file.text, d.span.offset);
        std::string line = file.text.substr(start, end - start);
        std::fprintf(stderr, "  %s\n", line.c_str());

        // Caret under the offending column; underline the span width.
        std::string caret(lc.col - 1 + 2, ' ');
        uint32_t width = d.span.length ? d.span.length : 1;
        caret.push_back('^');
        for (uint32_t i = 1; i < width; ++i) caret.push_back('~');
        std::fprintf(stderr, "%s\n", caret.c_str());
    }
}
