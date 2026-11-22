// Coffee wire protocol (API version coffee/v1alpha1, routes under /v1). See docs/native-protocol.md for the
// complete route/status table. This header holds the pieces shared by coordinator, Beans and CLI client.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "coffee/util.hpp"

namespace coffee::protocol {

inline constexpr const char* kHeaderBean = "X-Coffee-Bean";       // Bean id on lease-scoped requests
inline constexpr const char* kHeaderSha256 = "X-Coffee-Sha256";   // digest of a downloaded artifact
inline constexpr size_t kMaxJsonBytes = 4u << 20;                 // JSON request body limit
inline constexpr size_t kMaxLogBytes = 1u << 20;                  // per-attempt per-stream log upload limit
inline constexpr uint64_t kDefaultMaxArtifactBytes = 256ull << 20;
inline constexpr int kDefaultPort = 7077;

// Error document: {"error":{"code":"...","message":"...","field":"..."?}}
nlohmann::json error_body(const std::string& code, const std::string& message, const std::string& field = {});
// HTTP status for a controlled error code (unknown codes map to 500).
int status_for_code(const std::string& code);

struct ListenAddress {
  std::string host;
  int port = 0;
};
// "HOST:PORT", "[v6]:PORT" or ":PORT" (loopback). Throws Error("invalid_argument").
ListenAddress parse_listen(const std::string& text);
// True for 127.0.0.0/8, ::1 and "localhost".
bool is_loopback_host(const std::string& host);

struct ServerUrl {
  std::string host;
  int port = 0;
  std::string scheme;  // "http" (the only supported scheme in this build)
};
// Parse "http://HOST:PORT" (https:// is recognised and refused with an explanatory Error("unsupported")).
ServerUrl parse_server_url(const std::string& url);

}  // namespace coffee::protocol
