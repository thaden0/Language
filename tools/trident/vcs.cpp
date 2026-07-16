#include "vcs.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

bool isExecutableFile(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && ::access(path.c_str(), X_OK) == 0;
}

std::string findOnPath(const std::string& name) {
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return "";
    std::string paths = pathEnv;
    size_t start = 0;
    while (start <= paths.size()) {
        size_t colon = paths.find(':', start);
        std::string dir = paths.substr(start, colon == std::string::npos
                                                   ? std::string::npos
                                                   : colon - start);
        if (!dir.empty() && isExecutableFile(dir + "/" + name)) return dir + "/" + name;
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return "";
}

// Run `git <args...>`, capturing stdout into `stdoutOut` (ls-remote's
// output). stderr is inherited — git's own diagnostics surface directly,
// same as `runLeviathan`, main.cpp. Returns the exit code, or -1 on a fork/
// exec-machinery failure (`err` set; distinct from git itself failing,
// which is a normal nonzero exit).
int runGitCaptured(const std::vector<std::string>& args, std::string& stdoutOut,
                   std::string& err) {
    std::string git = findGit();
    if (git.empty()) {
        err = "cannot find the 'git' executable on $PATH (required for VCS "
             "dependencies — local-path deps need no git)";
        return -1;
    }

    int pipefd[2];
    if (::pipe(pipefd) != 0) { err = "pipe() failed"; return -1; }

    pid_t pid = ::fork();
    if (pid < 0) { err = "fork() failed"; return -1; }
    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::close(pipefd[1]);

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(git.c_str()));
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        ::execv(git.c_str(), argv.data());
        std::perror("trident: execv git");
        ::_exit(127);
    }
    ::close(pipefd[1]);

    stdoutOut.clear();
    char buf[4096];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0) stdoutOut.append(buf, n);
    ::close(pipefd[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

}  // namespace

std::string findGit() {
    return findOnPath("git");
}

bool gitListTags(const std::string& remote, std::vector<std::string>& out, std::string& err) {
    std::string stdoutText;
    int rc = runGitCaptured({"ls-remote", "--tags", remote}, stdoutText, err);
    if (rc < 0) return false;
    if (rc != 0) {
        err = "git ls-remote --tags '" + remote + "' failed";
        return false;
    }

    out.clear();
    std::istringstream iss(stdoutText);
    std::string line;
    const std::string prefix = "refs/tags/";
    while (std::getline(iss, line)) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string ref = line.substr(tab + 1);
        if (ref.compare(0, prefix.size(), prefix) != 0) continue;
        std::string name = ref.substr(prefix.size());
        if (name.size() >= 3 && name.compare(name.size() - 3, 3, "^{}") == 0)
            name = name.substr(0, name.size() - 3);        // dereferenced annotated tag
        if (!out.empty() && out.back() == name) continue;   // its "^{}" line follows immediately
        out.push_back(name);
    }
    return true;
}

bool gitCloneTag(const std::string& remote, const std::string& tag, const std::string& parentDir,
                 std::string& checkoutDir, std::string& err) {
    std::string parent = parentDir.empty() ? "/tmp" : parentDir;
    std::string tmpl = parent + "/trident-fetch-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (::mkdtemp(buf.data()) == nullptr) {
        err = "cannot create a temp directory under '" + parent + "'";
        return false;
    }
    checkoutDir = buf.data();

    std::string stdoutText;
    int rc = runGitCaptured(
        {"-c", "advice.detachedHead=false", "clone", "--quiet", "--depth", "1", "--branch", tag,
         remote, checkoutDir},
        stdoutText, err);
    if (rc < 0) return false;
    if (rc != 0) {
        err = "git clone --branch " + tag + " '" + remote +
             "' failed (missing tag, or remote unreachable)";
        return false;
    }
    return true;
}
