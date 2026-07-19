#pragma once
#include <string>

// Optional proxy/index transport. An endpoint is either a filesystem
// directory (plain path or file:// URL) or an HTTP(S) base URL. HTTP uses
// the system curl executable, keeping Trident free of linked networking
// dependencies and leaving the service entirely optional.

std::string percentEncodeSegment(const std::string& text);

bool endpointReadText(const std::string& endpoint, const std::string& relative,
                      std::string& text, bool& missing, std::string& err);
bool endpointDownload(const std::string& endpoint, const std::string& relative,
                      const std::string& destination, bool& missing,
                      std::string& err);
bool endpointPutTextIfAbsent(const std::string& endpoint, const std::string& relative,
                             const std::string& text, std::string& err);
