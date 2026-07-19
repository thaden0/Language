#pragma once
#include <string>

// Optional P2.3 thin index. It maps a friendly name to one immutable VCS
// path and never stores source. Explicit paths (anything containing '/')
// bypass it, so an index can never shadow a dependency identity.

std::string tridentIndexEndpoint();
bool isFriendlyPackageName(const std::string& text);

bool indexResolveName(const std::string& endpoint, const std::string& name,
                      std::string& modulePath, bool& found, std::string& err);
bool indexRegisterName(const std::string& endpoint, const std::string& name,
                       const std::string& modulePath, std::string& err);
