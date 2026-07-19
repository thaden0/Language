#pragma once
#include "store.hpp"
#include <string>
#include <vector>

// Expand a package manifest's declared source patterns into the exact file
// set whose canonical hash is published, proxied, and fetched. Keeping this
// in one helper prevents publish and providers from ever disagreeing about
// what `sha256:` means. Paths are relative to `baseDir`; absolute paths,
// parent traversal, directories, and symlinks are rejected.
bool collectDeclaredSources(const std::string& baseDir,
                            const std::vector<std::string>& patterns,
                            std::vector<StoreFile>& files, std::string& err);
