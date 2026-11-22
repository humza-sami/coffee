#include "coffee/client.hpp"

#include <unistd.h>

#include <httplib.h>

namespace coffee {

using json = nlohmann::json;

namespace {

void fill_error(ApiResponse& r, const std::string& body_text) {
  json j = json::parse(body_text, nullptr, false);
  if (!j.is_discarded()) {
    r.body = j;
    if (j.is_object() && j.contains("error") && j["error"].is_object()) {
      r.error_code = j["error"].value("code", "");
      r.error_message = j["error"].value("message", "");
    }
  }
  if (r.error_code.empty()) {
    r.error_code = "http_" + std::to_string(r.status);
    r.error_message = "HTTP status " + std::to_string(r.status);
  }
}

ApiResponse to_response(const httplib::Result& res) {
  if (!res) throw NetworkError(httplib::to_string(res.error()));
  ApiResponse r;
  r.status = res->status;
  if (r.status >= 400) fill_error(r, res->body);
  else if (!res->body.empty()) {
    json j = json::parse(res->body, nullptr, false);
    if (!j.is_discarded()) r.body = std::move(j);
  }
  return r;
}

}  // namespace

Client::Client(ClientOptions options) : opts_(std::move(options)) {
  protocol::ServerUrl u = protocol::parse_server_url(opts_.url);
  host_ = u.host;
  port_ = u.port;
  if (!protocol::is_loopback_host(host_) && !opts_.allow_plaintext_remote)
    throw Error("insecure_remote", "refusing to send the cluster token to non-loopback server '" + host_ +
                                       "' over plaintext HTTP (this build has no TLS). Connect through an SSH/WireGuard/Tailscale tunnel to a loopback or overlay address, "
                                       "or pass --allow-plaintext-remote if the path is already encrypted and trusted (the flag adds no encryption).");
}

namespace {
struct Conn {
  httplib::Client cli;
  httplib::Headers headers;
  Conn(const std::string& host, int port, const ClientOptions& o, const std::map<std::string, std::string>& extra) : cli(host, port) {
    cli.set_connection_timeout(o.connect_timeout_s);
    cli.set_read_timeout(o.read_timeout_s);
    cli.set_write_timeout(o.write_timeout_s);
    cli.set_keep_alive(false);
    cli.set_follow_location(false);
    if (!o.token.empty()) headers.emplace("Authorization", "Bearer " + o.token);
    for (const auto& [k, v] : extra) headers.emplace(k, v);
  }
};
}  // namespace

ApiResponse Client::get(const std::string& path) {
  Conn c(host_, port_, opts_, {});
  return to_response(c.cli.Get(path, c.headers));
}

ApiResponse Client::post(const std::string& path, const json& body) {
  Conn c(host_, port_, opts_, {});
  return to_response(c.cli.Post(path, c.headers, body.dump(), "application/json"));
}

ApiResponse Client::put_text(const std::string& path, const std::string& body, const std::map<std::string, std::string>& headers) {
  Conn c(host_, port_, opts_, headers);
  return to_response(c.cli.Put(path, c.headers, body, "text/plain"));
}

ApiResponse Client::put_fd(const std::string& path, int fd, uint64_t size, const std::map<std::string, std::string>& headers) {
  Conn c(host_, port_, opts_, headers);
  auto provider = [fd](size_t offset, size_t length, httplib::DataSink& sink) {
    char buf[65536];
    ssize_t r = ::pread(fd, buf, std::min(length, sizeof buf), static_cast<off_t>(offset));
    if (r <= 0) return false;
    return sink.write(buf, static_cast<size_t>(r));
  };
  return to_response(c.cli.Put(path, c.headers, static_cast<size_t>(size), provider, "application/octet-stream"));
}

ApiResponse Client::get_stream(const std::string& path, const std::function<bool(const char*, size_t)>& sink, const std::map<std::string, std::string>& headers) {
  Conn c(host_, port_, opts_, headers);
  ApiResponse out;
  std::string err_body;
  bool good = false;
  auto res = c.cli.Get(
      path, c.headers,
      [&](const httplib::Response& r) {
        out.status = r.status;
        good = r.status == 200;
        return true;
      },
      [&](const char* data, size_t n) {
        if (good) return sink(data, n);
        if (err_body.size() < (64u << 10)) err_body.append(data, n);
        return true;
      });
  if (!res) throw NetworkError(httplib::to_string(res.error()));
  if (!good) fill_error(out, err_body);
  return out;
}

ApiResponse Client::get_text(const std::string& path, std::string* text_out) {
  Conn c(host_, port_, opts_, {});
  auto res = c.cli.Get(path, c.headers);
  if (!res) throw NetworkError(httplib::to_string(res.error()));
  ApiResponse r;
  r.status = res->status;
  if (r.status >= 400) fill_error(r, res->body);
  else if (text_out) *text_out = res->body;
  return r;
}

}  // namespace coffee
