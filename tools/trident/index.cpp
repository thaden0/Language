#include "index.hpp"
#include "endpoint.hpp"
#include <cctype>
#include <cstdlib>

namespace {

std::string trim(std::string text) {
    size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
    size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) --last;
    return text.substr(first, last - first);
}

std::string indexPath(const std::string& name) {
    return "names/" + percentEncodeSegment(name);
}

}  // namespace

std::string tridentIndexEndpoint() {
    const char* value = std::getenv("TRIDENT_INDEX");
    return value ? std::string(value) : std::string();
}

bool isFriendlyPackageName(const std::string& text) {
    if (text.empty() || text == "." || text == ".." || text.find('/') != std::string::npos ||
        text.find(':') != std::string::npos)
        return false;
    for (unsigned char c : text)
        if (!std::isalnum(c) && c != '-' && c != '_' && c != '.') return false;
    return true;
}

bool indexResolveName(const std::string& endpoint, const std::string& name,
                      std::string& modulePath, bool& found, std::string& err) {
    found = false;
    if (endpoint.empty()) return true;
    if (!isFriendlyPackageName(name)) {
        err = "'" + name + "' is not a valid package-index name";
        return false;
    }
    bool missing = false;
    std::string text;
    if (!endpointReadText(endpoint, indexPath(name), text, missing, err)) {
        if (missing) return true;
        return false;
    }
    modulePath = trim(text);
    if (modulePath.empty() || modulePath.find('/') == std::string::npos) {
        err = "optional index entry for '" + name + "' does not contain a VCS path";
        return false;
    }
    found = true;
    return true;
}

bool indexRegisterName(const std::string& endpoint, const std::string& name,
                       const std::string& modulePath, std::string& err) {
    if (endpoint.empty()) return true;
    if (!isFriendlyPackageName(name)) {
        err = "manifest name '" + name + "' is not valid for the optional index";
        return false;
    }
    if (modulePath.empty() || modulePath.find('/') == std::string::npos) {
        err = "cannot register '" + name + "': '" + modulePath +
              "' is not an explicit VCS path";
        return false;
    }
    std::string existing;
    bool found = false;
    if (!indexResolveName(endpoint, name, existing, found, err)) return false;
    if (found) {
        if (existing == modulePath) return true;
        err = "index name '" + name + "' is already registered to '" + existing +
              "' and cannot be remapped to '" + modulePath + "'";
        return false;
    }
    return endpointPutTextIfAbsent(endpoint, indexPath(name), modulePath + "\n", err);
}
