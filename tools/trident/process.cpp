#include "process.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

bool isExecutableFile(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
           ::access(path.c_str(), X_OK) == 0;
}

}  // namespace

std::string findExecutable(const std::string& name) {
    if (name.find('/') != std::string::npos)
        return isExecutableFile(name) ? name : std::string();

    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return "";
    std::string paths = pathEnv;
    size_t start = 0;
    while (start <= paths.size()) {
        size_t colon = paths.find(':', start);
        std::string dir = paths.substr(start, colon == std::string::npos
                                                   ? std::string::npos
                                                   : colon - start);
        // POSIX gives an empty PATH entry the meaning "current directory".
        if (dir.empty()) dir = ".";
        std::string candidate = dir + "/" + name;
        if (isExecutableFile(candidate)) return candidate;
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return "";
}

int runProcess(const std::vector<std::string>& argv, std::string& stdoutOut,
               std::string& err, const std::string& cwd) {
    if (argv.empty()) {
        err = "cannot run an empty command";
        return -1;
    }
    std::string executable = findExecutable(argv[0]);
    if (executable.empty()) {
        err = "cannot find the '" + argv[0] + "' executable on $PATH";
        return -1;
    }

    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        err = "pipe() failed: " + std::string(std::strerror(errno));
        return -1;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        err = "fork() failed: " + std::string(std::strerror(errno));
        return -1;
    }
    if (pid == 0) {
        ::close(pipefd[0]);
        if (::dup2(pipefd[1], STDOUT_FILENO) < 0) ::_exit(126);
        ::close(pipefd[1]);
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) ::_exit(126);

        std::vector<char*> childArgv;
        childArgv.reserve(argv.size() + 1);
        childArgv.push_back(const_cast<char*>(executable.c_str()));
        for (size_t i = 1; i < argv.size(); ++i)
            childArgv.push_back(const_cast<char*>(argv[i].c_str()));
        childArgv.push_back(nullptr);
        ::execv(executable.c_str(), childArgv.data());
        std::perror("trident: execv");
        ::_exit(127);
    }

    ::close(pipefd[1]);
    stdoutOut.clear();
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
        if (n > 0) stdoutOut.append(buf, static_cast<size_t>(n));
        else if (n == 0) break;
        else if (errno != EINTR) break;
    }
    ::close(pipefd[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        err = "waitpid() failed: " + std::string(std::strerror(errno));
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}
