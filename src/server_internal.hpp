// Internal helpers shared by the coordinator route modules (not a public API).
#pragma once

#include <httplib.h>

#include <functional>

#include "coffee/artifacts.hpp"
#include "coffee/server.hpp"

namespace coffee::detail {

using json = nlohmann::json;

struct ServerCtx {
  CoordinatorOptions opts;
  Store* store = nullptr;
  ArtifactStore* artifacts = nullptr;
};

void send_json(httplib::Response& res, int status, const json& body);
void send_error(httplib::Response& res, const std::string& code, const std::string& message, const std::string& field = {});
void send_lease_error(httplib::Response& res, const LeaseResult& r);

// Wrap a handler so every Coffee/JSON/std exception becomes a controlled error response.
using Handler = std::function<void(const httplib::Request&, httplib::Response&)>;
using ReaderHandler = std::function<void(const httplib::Request&, httplib::Response&, const httplib::ContentReader&)>;
httplib::Server::Handler guard(Handler h);
httplib::Server::HandlerWithContentReader guard_reader(ReaderHandler h);

// Reads and parses a JSON object body of at most `limit` bytes (invalid_json / too_large otherwise).
json read_json_body(const httplib::Request& req, const httplib::ContentReader& reader, size_t limit = protocol::kMaxJsonBytes);

std::string bean_header(const httplib::Request& req);

// Typed JSON field access with controlled invalid_argument errors.
std::string get_string(const json& j, const char* key, const std::string& def, bool required = false);
int64_t get_int(const json& j, const char* key, int64_t def, bool required = false);
bool get_bool(const json& j, const char* key, bool def);

void register_bean_routes(httplib::Server& s, ServerCtx& ctx);
void register_job_routes(httplib::Server& s, ServerCtx& ctx);
void register_lease_routes(httplib::Server& s, ServerCtx& ctx);

}  // namespace coffee::detail
