#pragma once
#include "provider.hpp"
#include <string>

// Optional P2.3 caching-proxy provider. `$TRIDENT_PROXY` points at either a
// filesystem directory/file:// URL (offline CI and self-hosting) or an
// HTTP(S) base URL. Its static, registry-less layout is:
//
//   modules/<percent-encoded ModuleId>/@v/list
//   modules/<percent-encoded ModuleId>/@v/v1.2.3.toml
//   modules/<percent-encoded ModuleId>/@v/v1.2.3.tar
//
// The tar contains only the manifest-declared source files at their package-
// relative paths. Hash verification remains in resolve.cpp, exactly as for
// GitProvider, so the proxy is never a trust root.

std::string tridentProxyEndpoint();
std::string proxyModulePrefix(const ModuleId& mod);

class ProxyProvider : public ModuleProvider {
public:
    explicit ProxyProvider(std::string endpoint) : endpoint_(std::move(endpoint)) {}

    bool manifestOf(const ModuleId& mod, const Version& version, ProjectManifest& out,
                    std::string& err) override;
    bool materialize(const ModuleId& mod, const Version& version, std::string& storeDir,
                     std::string& contentHash, std::string& err) override;
    bool versions(const ModuleId& mod, std::vector<Version>& out, std::string& err) override;

private:
    std::string endpoint_;
};
