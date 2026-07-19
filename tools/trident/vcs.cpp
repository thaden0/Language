#include "vcs.hpp"
#include "process.hpp"
#include <algorithm>
#include <cstdio>
#include <sstream>
#include <unistd.h>

namespace {

// Run `git <args...>`, capturing stdout into `stdoutOut` (ls-remote's
// output). stderr is inherited — git's own diagnostics surface directly,
// same as `runLeviathan`, main.cpp. Returns the exit code, or -1 on a fork/
// exec-machinery failure (`err` set; distinct from git itself failing,
// which is a normal nonzero exit).
int runGitCaptured(const std::vector<std::string>& args, std::string& stdoutOut,
                   std::string& err) {
    if (findGit().empty()) {
        err = "cannot find the 'git' executable on $PATH (required for VCS "
             "dependencies — local-path deps need no git)";
        return -1;
    }
    std::vector<std::string> command{"git"};
    command.insert(command.end(), args.begin(), args.end());
    return runProcess(command, stdoutOut, err);
}

std::string trimNewlines(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

bool runGitChecked(const std::vector<std::string>& args, std::string& out,
                   const std::string& what, std::string& err) {
    int rc = runGitCaptured(args, out, err);
    if (rc < 0) return false;
    if (rc != 0) {
        err = what + " failed";
        return false;
    }
    out = trimNewlines(out);
    return true;
}

}  // namespace

std::string findGit() {
    return findExecutable("git");
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

bool gitRepoRoot(const std::string& path, std::string& root, std::string& err) {
    return runGitChecked({"-C", path, "rev-parse", "--show-toplevel"}, root,
                         "git repository discovery in '" + path + "'", err);
}

bool gitWorkingTreeClean(const std::string& repo, bool& clean, std::string& details,
                         std::string& err) {
    if (!runGitChecked({"-C", repo, "status", "--porcelain", "--untracked-files=all"},
                       details, "git status in '" + repo + "'", err))
        return false;
    clean = details.empty();
    return true;
}

bool gitOriginUrl(const std::string& repo, std::string& remote, std::string& err) {
    return runGitChecked({"-C", repo, "remote", "get-url", "origin"}, remote,
                         "reading git remote 'origin' in '" + repo + "'", err);
}

bool gitHeadCommit(const std::string& repo, std::string& commit, std::string& err) {
    return runGitChecked({"-C", repo, "rev-parse", "HEAD^{commit}"}, commit,
                         "reading HEAD in '" + repo + "'", err);
}

bool gitPathTracked(const std::string& repo, const std::string& relativePath,
                    bool& tracked, std::string& err) {
    std::string out;
    int rc = runGitCaptured({"-C", repo, "ls-files", "--error-unmatch", "--", relativePath},
                            out, err);
    if (rc < 0) return false;
    tracked = rc == 0;
    return true;
}

bool gitTagCommit(const std::string& repo, const std::string& tag, bool& exists,
                  std::string& commit, std::string& err) {
    std::string out;
    int rc = runGitCaptured({"-C", repo, "rev-parse", "--quiet", "--verify",
                             "refs/tags/" + tag + "^{commit}"}, out, err);
    if (rc < 0) return false;
    if (rc != 0) {
        exists = false;
        commit.clear();
        return true;
    }
    exists = true;
    commit = trimNewlines(out);
    return true;
}

bool gitCreateTag(const std::string& repo, const std::string& tag,
                  const std::string& commit, std::string& err) {
    std::string out;
    return runGitChecked({"-C", repo, "tag", tag, commit}, out,
                         "creating git tag '" + tag + "'", err);
}

bool gitDeleteTag(const std::string& repo, const std::string& tag, std::string& err) {
    std::string out;
    return runGitChecked({"-C", repo, "tag", "-d", tag}, out,
                         "deleting git tag '" + tag + "'", err);
}

std::string modulePathFromRemote(const std::string& remoteText) {
    std::string remote = trimNewlines(remoteText);
    bool localPath = remote.compare(0, 7, "file://") == 0 ||
                     (!remote.empty() && (remote[0] == '/' || remote[0] == '.'));
    if (remote.compare(0, 7, "file://") == 0) remote.erase(0, 7);
    else {
        size_t scheme = remote.find("://");
        if (scheme != std::string::npos) {
            std::string rest = remote.substr(scheme + 3);
            size_t slash = rest.find('/');
            std::string host = slash == std::string::npos ? rest : rest.substr(0, slash);
            size_t at = host.rfind('@');
            if (at != std::string::npos) host = host.substr(at + 1);
            remote = host + (slash == std::string::npos ? "" : rest.substr(slash));
        } else if (!remote.empty() && remote[0] != '/') {
            // SCP-style SSH remote: git@host:owner/repo.git.
            size_t colon = remote.find(':');
            size_t at = remote.find('@');
            if (colon != std::string::npos && at != std::string::npos && at < colon)
                remote = remote.substr(at + 1, colon - at - 1) + "/" + remote.substr(colon + 1);
        }
    }
    while (remote.size() > 1 && remote.back() == '/') remote.pop_back();
    if (!localPath && remote.size() > 4 &&
        remote.compare(remote.size() - 4, 4, ".git") == 0)
        remote.resize(remote.size() - 4);
    return remote;
}
