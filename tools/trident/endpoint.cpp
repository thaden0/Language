#include "endpoint.hpp"
#include "process.hpp"
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool isHttp(const std::string& endpoint) {
    return endpoint.compare(0, 7, "http://") == 0 ||
           endpoint.compare(0, 8, "https://") == 0;
}

std::string localRoot(const std::string& endpoint) {
    return endpoint.compare(0, 7, "file://") == 0 ? endpoint.substr(7) : endpoint;
}

bool safeRelative(const std::string& relative) {
    if (relative.empty() || relative.front() == '/') return false;
    size_t start = 0;
    while (start <= relative.size()) {
        size_t slash = relative.find('/', start);
        std::string part = relative.substr(start, slash == std::string::npos
                                                       ? std::string::npos
                                                       : slash - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

std::string joined(const std::string& endpoint, const std::string& relative) {
    std::string out = endpoint;
    while (!out.empty() && out.back() == '/') out.pop_back();
    return out + "/" + relative;
}

bool ensureDirRec(const std::string& dir) {
    if (dir.empty() || dir == "/") return true;
    struct stat st;
    if (::stat(dir.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    size_t slash = dir.find_last_of('/');
    if (slash != std::string::npos && !ensureDirRec(dir.substr(0, slash))) return false;
    return ::mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST;
}

bool readFile(const std::string& path, std::string& text) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    text = ss.str();
    return true;
}

bool copyFile(const std::string& source, const std::string& destination,
              std::string& err) {
    std::ifstream in(source, std::ios::binary);
    if (!in) { err = "cannot read '" + source + "'"; return false; }
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!out) { err = "cannot write '" + destination + "'"; return false; }
    out << in.rdbuf();
    return out.good();
}

}  // namespace

std::string percentEncodeSegment(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                     c == '.' || c == '~';
        if (plain) out += static_cast<char>(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

bool endpointReadText(const std::string& endpoint, const std::string& relative,
                      std::string& text, bool& missing, std::string& err) {
    missing = false;
    if (!safeRelative(relative)) {
        err = "unsafe endpoint path '" + relative + "'";
        return false;
    }
    if (!isHttp(endpoint)) {
        std::string path = joined(localRoot(endpoint), relative);
        if (!readFile(path, text)) {
            struct stat st;
            missing = ::stat(path.c_str(), &st) != 0 && errno == ENOENT;
            if (!missing) err = "cannot read '" + path + "'";
            return false;
        }
        return true;
    }

    std::string curl = findExecutable("curl");
    if (curl.empty()) {
        err = "cannot find the 'curl' executable on $PATH (required for HTTP(S) "
              "TRIDENT_PROXY/TRIDENT_INDEX endpoints)";
        return false;
    }
    std::string output;
    int rc = runProcess({"curl", "--silent", "--show-error", "--location",
                         "--write-out", "\n%{http_code}", joined(endpoint, relative)},
                        output, err);
    if (rc != 0) {
        err = "cannot fetch '" + joined(endpoint, relative) + "' from the optional service";
        return false;
    }
    size_t marker = output.find_last_of('\n');
    if (marker == std::string::npos) {
        err = "HTTP client returned no status for '" + joined(endpoint, relative) + "'";
        return false;
    }
    std::string status = output.substr(marker + 1);
    if (status == "404") { missing = true; return false; }
    if (status.size() != 3 || status[0] != '2') {
        err = "optional service returned HTTP " + status + " for '" +
              joined(endpoint, relative) + "'";
        return false;
    }
    text = output.substr(0, marker);
    return true;
}

bool endpointDownload(const std::string& endpoint, const std::string& relative,
                      const std::string& destination, bool& missing,
                      std::string& err) {
    missing = false;
    if (!safeRelative(relative)) {
        err = "unsafe endpoint path '" + relative + "'";
        return false;
    }
    if (!isHttp(endpoint)) {
        std::string path = joined(localRoot(endpoint), relative);
        struct stat st;
        if (::stat(path.c_str(), &st) != 0) {
            missing = errno == ENOENT;
            if (!missing) err = "cannot stat '" + path + "'";
            return false;
        }
        return copyFile(path, destination, err);
    }

    if (findExecutable("curl").empty()) {
        err = "cannot find the 'curl' executable on $PATH (required for HTTP(S) "
              "TRIDENT_PROXY endpoints)";
        return false;
    }
    std::string output;
    int rc = runProcess({"curl", "--silent", "--show-error", "--location",
                         "--write-out", "%{http_code}", "--output", destination,
                         joined(endpoint, relative)}, output, err);
    if (rc != 0) {
        err = "cannot fetch '" + joined(endpoint, relative) + "' from the optional service";
        return false;
    }
    if (output == "404") {
        ::unlink(destination.c_str());
        missing = true;
        return false;
    }
    if (output.size() != 3 || output[0] != '2') {
        ::unlink(destination.c_str());
        err = "optional service returned HTTP " + output + " for '" +
              joined(endpoint, relative) + "'";
        return false;
    }
    return true;
}

bool endpointPutTextIfAbsent(const std::string& endpoint, const std::string& relative,
                             const std::string& text, std::string& err) {
    if (!safeRelative(relative)) {
        err = "unsafe endpoint path '" + relative + "'";
        return false;
    }
    if (!isHttp(endpoint)) {
        std::string path = joined(localRoot(endpoint), relative);
        size_t slash = path.find_last_of('/');
        if (slash != std::string::npos && !ensureDirRec(path.substr(0, slash))) {
            err = "cannot create directory for '" + path + "'";
            return false;
        }
        int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            if (errno == EEXIST) {
                std::string existing;
                if (readFile(path, existing) && existing == text) return true;
                err = "index name is already registered to a different VCS path: '" + path + "'";
                return false;
            }
            err = "cannot create '" + path + "'";
            return false;
        }
        size_t written = 0;
        while (written < text.size()) {
            ssize_t n = ::write(fd, text.data() + written, text.size() - written);
            if (n > 0) written += static_cast<size_t>(n);
            else if (errno != EINTR) break;
        }
        int closeRc = ::close(fd);
        bool ok = written == text.size() && closeRc == 0;
        if (!ok) {
            ::unlink(path.c_str());
            err = "cannot write '" + path + "'";
        }
        return ok;
    }

    if (findExecutable("curl").empty()) {
        err = "cannot find the 'curl' executable on $PATH (required for an HTTP(S) index)";
        return false;
    }
    std::string tmpl = "/tmp/trident-index-put-XXXXXX";
    std::vector<char> bytes(tmpl.begin(), tmpl.end());
    bytes.push_back('\0');
    int fd = ::mkstemp(bytes.data());
    if (fd < 0) { err = "cannot create temporary index request"; return false; }
    size_t written = 0;
    while (written < text.size()) {
        ssize_t n = ::write(fd, text.data() + written, text.size() - written);
        if (n > 0) written += static_cast<size_t>(n);
        else if (errno != EINTR) break;
    }
    ::close(fd);
    std::string tmp = bytes.data();
    if (written != text.size()) {
        ::unlink(tmp.c_str());
        err = "cannot write temporary index request";
        return false;
    }
    std::string output;
    int rc = runProcess({"curl", "--silent", "--show-error", "--location",
                         "--write-out", "%{http_code}",
                         "--request", "PUT", "--header", "If-None-Match: *",
                         "--data-binary", "@" + tmp, joined(endpoint, relative)}, output, err);
    ::unlink(tmp.c_str());
    if (rc != 0 || output.size() != 3 || output[0] != '2') {
        err = "optional index rejected registration for '" + relative +
              "' (HTTP " + (output.empty() ? std::string("transport error") : output) +
              "; names are first-registration-wins and immutable)";
        return false;
    }
    return true;
}
