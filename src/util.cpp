#include "coffee/util.hpp"

#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>

namespace coffee {

namespace fs = std::filesystem;

int64_t SystemClock::now_ms() const {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

namespace {
constexpr char kHex[] = "0123456789abcdef";

std::string to_hex(const unsigned char* p, size_t n) {
  std::string s(n * 2, '0');
  for (size_t i = 0; i < n; i++) {
    s[2 * i] = kHex[p[i] >> 4];
    s[2 * i + 1] = kHex[p[i] & 15];
  }
  return s;
}

std::string errno_text(const std::string& what, const fs::path& p) {
  return what + " " + p.string() + ": " + std::strerror(errno);
}

// ---- SHA-256 (FIPS 180-4)
class Sha256 {
 public:
  void update(const unsigned char* data, size_t len) {
    total_ += len;
    while (len > 0) {
      size_t take = std::min(len, buf_.size() - used_);
      std::memcpy(buf_.data() + used_, data, take);
      used_ += take;
      data += take;
      len -= take;
      if (used_ == buf_.size()) {
        block(buf_.data());
        used_ = 0;
      }
    }
  }
  std::string finish() {
    uint64_t bits = total_ * 8;
    unsigned char pad = 0x80;
    update(&pad, 1);
    unsigned char zero = 0;
    while (used_ != 56) update(&zero, 1);
    unsigned char len[8];
    for (int i = 0; i < 8; i++) len[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
    update(len, 8);
    unsigned char out[32];
    for (int i = 0; i < 8; i++)
      for (int j = 0; j < 4; j++) out[4 * i + j] = static_cast<unsigned char>(h_[i] >> (24 - 8 * j));
    return to_hex(out, 32);
  }

 private:
  static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void block(const unsigned char* p) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
      w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) | uint32_t(p[4 * i + 3]);
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; i++) {
      uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
  }

  uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::array<unsigned char, 64> buf_{};
  size_t used_ = 0;
  uint64_t total_ = 0;
};
}  // namespace

struct Sha256Stream::Impl {
  Sha256 h;
};
Sha256Stream::Sha256Stream() : impl_(new Impl) {}
Sha256Stream::~Sha256Stream() { delete impl_; }
void Sha256Stream::update(const void* data, size_t len) { impl_->h.update(static_cast<const unsigned char*>(data), len); }
std::string Sha256Stream::finish() { return impl_->h.finish(); }

std::string random_hex(size_t nbytes) {
  std::string raw(nbytes, '\0');
  size_t got = 0;
  while (got < nbytes) {
    ssize_t r = ::getrandom(raw.data() + got, nbytes - got, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      throw Error("io_error", std::string("getrandom failed: ") + std::strerror(errno));
    }
    got += static_cast<size_t>(r);
  }
  return to_hex(reinterpret_cast<const unsigned char*>(raw.data()), nbytes);
}

std::string sha256_hex(std::string_view data) {
  Sha256 s;
  s.update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
  return s.finish();
}

std::string sha256_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw Error("io_error", "cannot open " + path.string());
  Sha256 s;
  std::array<char, 65536> buf;
  while (in.read(buf.data(), buf.size()) || in.gcount() > 0)
    s.update(reinterpret_cast<const unsigned char*>(buf.data()), static_cast<size_t>(in.gcount()));
  if (in.bad()) throw Error("io_error", "read error on " + path.string());
  return s.finish();
}

std::optional<std::string> check_safe_relative_path(std::string_view path) {
  if (path.empty()) return "path must not be empty";
  if (path.size() > 1024) return "path longer than 1024 bytes";
  if (path.front() == '/') return "path must be relative";
  if (path.back() == '/') return "path must not end with '/'";
  for (unsigned char ch : path) {
    if (ch < 0x20 || ch == 0x7f) return "path contains a control character";
    if (ch == '\\') return "path must not contain backslashes";
  }
  size_t pos = 0;
  while (pos <= path.size()) {
    size_t next = path.find('/', pos);
    std::string_view comp = path.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos);
    if (comp.empty()) return "path contains an empty component";
    if (comp == ".") return "path contains a '.' component";
    if (comp == "..") return "path must not contain '..'";
    if (comp.size() > 255) return "path component longer than 255 bytes";
    if (next == std::string_view::npos) break;
    pos = next + 1;
  }
  return std::nullopt;
}

fs::path confine_path(const fs::path& root, std::string_view relative) {
  if (auto why = check_safe_relative_path(relative)) throw Error("invalid_argument", "unsafe path '" + std::string(relative) + "': " + *why);
  std::error_code ec;
  fs::path canon_root = fs::weakly_canonical(root, ec);
  if (ec) throw Error("io_error", "cannot resolve " + root.string() + ": " + ec.message());
  fs::path resolved = fs::weakly_canonical(canon_root / std::string(relative), ec);
  if (ec) throw Error("io_error", "cannot resolve " + std::string(relative) + ": " + ec.message());
  auto [rend, pend] = std::mismatch(canon_root.begin(), canon_root.end(), resolved.begin(), resolved.end());
  if (rend != canon_root.end()) throw Error("invalid_argument", "path '" + std::string(relative) + "' escapes " + root.string());
  return resolved;
}

std::string read_file_limited(const fs::path& path, size_t max_bytes) {
  std::error_code ec;
  auto size = fs::file_size(path, ec);
  if (ec) throw Error("io_error", "cannot stat " + path.string() + ": " + ec.message());
  if (size > max_bytes) throw Error("too_large", path.string() + " exceeds " + std::to_string(max_bytes) + " bytes");
  std::ifstream in(path, std::ios::binary);
  if (!in) throw Error("io_error", "cannot open " + path.string());
  std::string data(static_cast<size_t>(size), '\0');
  in.read(data.data(), static_cast<std::streamsize>(data.size()));
  data.resize(static_cast<size_t>(in.gcount()));
  return data;
}

void write_file_atomic(const fs::path& path, std::string_view data, unsigned mode) {
  fs::path tmp = path;
  tmp += ".tmp-" + random_hex(6);
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
  if (fd < 0) throw Error("io_error", errno_text("cannot create", tmp));
  auto fail = [&](const std::string& msg) {
    ::close(fd);
    ::unlink(tmp.c_str());
    throw Error("io_error", msg);
  };
  size_t off = 0;
  while (off < data.size()) {
    ssize_t w = ::write(fd, data.data() + off, data.size() - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      fail(errno_text("cannot write", tmp));
    }
    off += static_cast<size_t>(w);
  }
  if (::fchmod(fd, mode) != 0) fail(errno_text("cannot chmod", tmp));
  if (::fsync(fd) != 0) fail(errno_text("cannot fsync", tmp));
  if (::close(fd) != 0) {
    std::string msg = errno_text("cannot close", tmp);
    ::unlink(tmp.c_str());
    throw Error("io_error", msg);
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    std::string msg = errno_text("cannot rename to", path);
    ::unlink(tmp.c_str());
    throw Error("io_error", msg);
  }
  // The rename is only durable once the parent directory entry is synchronized; a failure here must surface.
  fs::path parent = path.parent_path().empty() ? fs::path(".") : path.parent_path();
  int dfd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd < 0) throw Error("io_error", errno_text("cannot open directory for sync", parent));
  if (::fsync(dfd) != 0) {
    std::string msg = errno_text("cannot fsync directory", parent);
    ::close(dfd);
    throw Error("io_error", msg);
  }
  ::close(dfd);
}

}  // namespace coffee
