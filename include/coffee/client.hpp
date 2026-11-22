// Coffee HTTP client used by Beans and the CLI. One short-lived connection per request (thread-safe by
// construction). Plain HTTP only: a non-loopback server needs the explicit allow_plaintext_remote override,
// which adds no encryption (use an SSH/WireGuard/Tailscale tunnel); https:// URLs are refused.
#pragma once

#include <functional>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "coffee/protocol.hpp"

namespace coffee {

// Transport-level failure (connection refused, timeout, truncated response). Code "network_error".
class NetworkError : public Error {
 public:
  explicit NetworkError(const std::string& message) : Error("network_error", message) {}
};

struct ClientOptions {
  std::string url = "http://127.0.0.1:7077";
  std::string token;                       // never logged or echoed
  bool allow_plaintext_remote = false;
  int connect_timeout_s = 5;
  int read_timeout_s = 60;
  int write_timeout_s = 60;
};

struct ApiResponse {
  int status = 0;
  nlohmann::json body;                     // parsed JSON body (null when empty/not JSON)
  std::string error_code, error_message;   // from {"error":{...}} when status >= 400
  bool ok() const { return status >= 200 && status < 300; }
};

class Client {
 public:
  // Throws Error("insecure_remote") for a plaintext non-loopback URL without the override,
  // Error("unsupported") for https://, Error("invalid_argument") for a malformed URL.
  explicit Client(ClientOptions options);

  ApiResponse get(const std::string& path);
  ApiResponse post(const std::string& path, const nlohmann::json& body = nlohmann::json::object());

  // Stream `size` bytes of the open file `fd` (from offset 0) as the PUT body.
  ApiResponse put_fd(const std::string& path, int fd, uint64_t size, const std::map<std::string, std::string>& headers = {});
  ApiResponse put_text(const std::string& path, const std::string& body, const std::map<std::string, std::string>& headers = {});

  // GET streaming a 200 body into `sink` (chunks); non-200 responses are returned as ApiResponse errors.
  ApiResponse get_stream(const std::string& path, const std::function<bool(const char*, size_t)>& sink, const std::map<std::string, std::string>& headers = {});
  // GET returning the body as text (status/err in ApiResponse::body["text"]).
  ApiResponse get_text(const std::string& path, std::string* text_out);

  const std::string& host() const { return host_; }
  int port() const { return port_; }

 private:
  ClientOptions opts_;
  std::string host_;
  int port_ = 0;
};

}  // namespace coffee
