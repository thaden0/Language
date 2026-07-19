#pragma once
#include <string>
#include <vector>

// Small fork/exec helper shared by Trident's optional external-tool paths.
// The package manager remains free of linked dependencies: git, curl, tar,
// and openssl are located on PATH only when the feature that needs them is
// used. `runProcess` captures stdout, inherits stderr, and returns the
// child's ordinary exit code (or -1 for local fork/exec machinery errors).

std::string findExecutable(const std::string& name);

int runProcess(const std::vector<std::string>& argv, std::string& stdoutOut,
               std::string& err, const std::string& cwd = "");
