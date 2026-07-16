#pragma once
#include "Source.hpp"
#include <string>
#include <vector>

enum class Severity { Error, Warning, Note };

struct Diagnostic {
    Severity severity = Severity::Error;
    std::string message;
    SourceSpan span;
};

// Collects diagnostics as the front-end runs, rather than throwing, so one
// error never aborts the whole pass (per the design's "diagnostics, not
// exceptions, for user errors" decision).
class DiagnosticSink {
public:
    void error(SourceSpan span, std::string message) {
        diags_.push_back({Severity::Error, std::move(message), span});
        ++errorCount_;
    }
    void warning(SourceSpan span, std::string message) {
        diags_.push_back({Severity::Warning, std::move(message), span});
    }
    void note(SourceSpan span, std::string message) {
        diags_.push_back({Severity::Note, std::move(message), span});
    }

    bool hasErrors() const { return errorCount_ > 0; }
    size_t errorCount() const { return errorCount_; }
    const std::vector<Diagnostic>& all() const { return diags_; }

    // Print each diagnostic with location, source line, and a caret.
    void render(const SourceFile& file) const;

private:
    std::vector<Diagnostic> diags_;
    size_t errorCount_ = 0;
};
