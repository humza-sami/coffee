#include "coffee/protocol.hpp"

#include <algorithm>
#include <map>

namespace coffee::protocol {

nlohmann::json error_body(const std::string& code, const std::string& message, const std::string& field) {
  nlohmann::json e = {{"code", code}, {"message", message}};
  if (!field.empty()) e["field"] = field;
  return {{"error", e}};
}

int status_for_code(const std::string& code) {
  static const std::map<std::string, int> table = {
      {"invalid_json", 400}, {"invalid_argument", 400}, {"invalid_workload", 400}, {"invalid_outputs", 400}, {"digest_mismatch", 400},
      {"unauthorized", 401}, {"wrong_bean", 403}, {"not_found", 404}, {"unknown_bean", 404}, {"unknown_job", 404}, {"unknown_task", 404},
      {"unknown_lease", 404}, {"no_such_input", 404}, {"no_such_output", 404}, {"no_logs", 404},
      {"conflict", 409}, {"missing_input", 409}, {"missing_upload", 409}, {"expired", 409}, {"deadline_exceeded", 409},
      {"cancelled", 409}, {"not_running", 409},
      {"too_large", 413}, {"unsafe_path", 400}};
  auto it = table.find(code);
  return it == table.end() ? 500 : it->second;
}

bool is_loopback_host(const std::string& host) {
  if (host == "localhost" || host == "::1" || host == "[::1]") return true;
  return host.rfind("127.", 0) == 0;
}

ListenAddress parse_listen(const std::string& text) {
  ListenAddress a;
  std::string host, port;
  if (!text.empty() && text[0] == '[') {
    size_t close = text.find(']');
    if (close == std::string::npos || close + 1 >= text.size() || text[close + 1] != ':') throw Error("invalid_argument", "invalid listen address '" + text + "'");
    host = text.substr(1, close - 1);
    port = text.substr(close + 2);
  } else {
    size_t colon = text.rfind(':');
    if (colon == std::string::npos) throw Error("invalid_argument", "listen address must be HOST:PORT (got '" + text + "')");
    host = text.substr(0, colon);
    port = text.substr(colon + 1);
  }
  if (host.empty()) host = "127.0.0.1";
  if (port.empty() || !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; }) || port.size() > 5)
    throw Error("invalid_argument", "invalid port in '" + text + "'");
  int p = std::stoi(port);
  if (p > 65535) throw Error("invalid_argument", "port out of range in '" + text + "'");
  a.host = host;
  a.port = p;
  return a;
}

ServerUrl parse_server_url(const std::string& url) {
  ServerUrl u;
  std::string rest;
  if (url.rfind("http://", 0) == 0) {
    u.scheme = "http";
    rest = url.substr(7);
  } else if (url.rfind("https://", 0) == 0) {
    throw Error("unsupported", "https:// is not supported in this build (no TLS library is bundled); use http:// over an encrypted tunnel such as SSH, WireGuard or Tailscale");
  } else {
    throw Error("invalid_argument", "server URL must look like http://HOST:PORT (got '" + url + "')");
  }
  while (!rest.empty() && rest.back() == '/') rest.pop_back();
  if (rest.find('/') != std::string::npos) throw Error("invalid_argument", "server URL must not contain a path");
  ListenAddress a = parse_listen(rest.find(':') == std::string::npos ? rest + ":" + std::to_string(kDefaultPort) : rest);
  u.host = a.host;
  u.port = a.port;
  return u;
}

}  // namespace coffee::protocol
