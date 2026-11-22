#include "coffee/artifacts.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <vector>

namespace coffee {

namespace fs = std::filesystem;

namespace {

std::string errno_msg(const std::string& what) { return what + ": " + std::strerror(errno); }

bool valid_scope(const std::string& s) {
  if (s.empty() || s.size() > 128 || s == "." || s == "..") return false;
  for (char c : s)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
  return true;
}

// Durability helpers: every required open/fsync/chmod failure is surfaced as Error("io_error") so that a
// blob whose durability could not be established is never referenced by accepted metadata.
void fsync_dir(const fs::path& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) throw Error("io_error", errno_msg("cannot open directory " + dir.string() + " for sync"));
  if (::fsync(fd) != 0) {
    std::string msg = errno_msg("cannot fsync directory " + dir.string());
    ::close(fd);
    throw Error("io_error", msg);
  }
  ::close(fd);
}

void write_all(int fd, const char* data, size_t len, const fs::path& what) {
  size_t off = 0;
  while (off < len) {
    ssize_t w = ::write(fd, data + off, len - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      throw Error("io_error", errno_msg("cannot write " + what.string()));
    }
    off += static_cast<size_t>(w);
  }
}

std::string hash_fd(int fd) {
  Sha256Stream h;
  std::array<char, 65536> buf;
  for (;;) {
    ssize_t r = ::read(fd, buf.data(), buf.size());
    if (r < 0) {
      if (errno == EINTR) continue;
      throw Error("io_error", errno_msg("read error"));
    }
    if (r == 0) break;
    h.update(buf.data(), static_cast<size_t>(r));
  }
  return h.finish();
}

// Size and SHA-256 of an existing regular file (no symlink following); false when it is not that content.
bool file_matches(const fs::path& p, const std::string& sha256, uint64_t size) {
  int fd = ::open(p.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return false;
  struct Closer {
    int fd;
    ~Closer() { ::close(fd); }
  } closer{fd};
  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || static_cast<uint64_t>(st.st_size) != size) return false;
  try {
    return hash_fd(fd) == sha256;
  } catch (const Error&) {
    return false;
  }
}

// Make `src` (already verified to hold sha256/size) the immutable blob `dst`. Never leaves `dst` absent or partial:
//  * dst absent: hard-link src to dst;
//  * dst exists and verifies: keep it (idempotent publication), but still make it durable;
//  * dst exists but differs (corrupt/truncated/foreign): atomically replace it with a link to the verified bytes.
// The data, the link and the parent directory entry are all synchronized; any failure throws and nothing is accepted.
PublishOutcome install_blob(const fs::path& src, const fs::path& dst, const std::string& sha256, uint64_t size) {
  PublishOutcome outcome = PublishOutcome::Created;
  if (::link(src.c_str(), dst.c_str()) != 0) {
    if (errno != EEXIST) throw Error("io_error", errno_msg("cannot publish " + dst.string()));
    outcome = PublishOutcome::AlreadyPresent;
    if (!file_matches(dst, sha256, size)) {
      outcome = PublishOutcome::Repaired;
      fs::path tmp = dst;
      tmp += ".repair-" + random_hex(6);
      if (::link(src.c_str(), tmp.c_str()) != 0) throw Error("io_error", errno_msg("cannot stage repair of " + dst.string()));
      if (::rename(tmp.c_str(), dst.c_str()) != 0) {
        std::string msg = errno_msg("cannot replace damaged " + dst.string());
        ::unlink(tmp.c_str());
        throw Error("io_error", msg);
      }
    } else {
      int fd = ::open(dst.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);  // an earlier crash may have left it unsynced
      if (fd < 0 || ::fsync(fd) != 0) {
        std::string msg = errno_msg("cannot fsync existing " + dst.string());
        if (fd >= 0) ::close(fd);
        throw Error("io_error", msg);
      }
      ::close(fd);
    }
  }
  fsync_dir(dst.parent_path());
  return outcome;
}

// Walk `rel` below root, opening each directory component with O_NOFOLLOW. When `create_dirs` is set, missing
// directories are made. Returns the fd of the parent directory and the leaf name.
struct Walk {
  int dirfd = -1;
  std::string leaf;
  ~Walk() {
    if (dirfd >= 0) ::close(dirfd);
  }
};

Walk walk_parent(const fs::path& root, std::string_view rel, bool create_dirs) {
  if (auto why = check_safe_relative_path(rel)) throw Error("unsafe_path", "unsafe path '" + std::string(rel) + "': " + *why);
  Walk w;
  w.dirfd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (w.dirfd < 0) throw Error("io_error", errno_msg("cannot open " + root.string()));
  size_t pos = 0;
  for (;;) {
    size_t next = rel.find('/', pos);
    std::string comp(rel.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos));
    if (next == std::string_view::npos) {
      w.leaf = comp;
      return w;
    }
    if (create_dirs && ::mkdirat(w.dirfd, comp.c_str(), 0700) != 0 && errno != EEXIST) throw Error("io_error", errno_msg("cannot create directory '" + comp + "'"));
    int nfd = ::openat(w.dirfd, comp.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (nfd < 0) {
      if (errno == ENOENT) throw Error("not_found", "path '" + std::string(rel) + "' does not exist");
      throw Error("unsafe_path", "unsafe path '" + std::string(rel) + "': component '" + comp + "' is a symlink or not a directory");
    }
    ::close(w.dirfd);
    w.dirfd = nfd;
    pos = next + 1;
  }
}

}  // namespace

bool is_sha256_hex(std::string_view s) {
  if (s.size() != 64) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

// ------------------------------------------------------------------ Upload

Upload::Upload(fs::path scope_dir, uint64_t limit) : scope_dir_(std::move(scope_dir)), limit_(limit), hash_(std::make_unique<Sha256Stream>()) {
  path_ = scope_dir_ / ("up-" + random_hex(8) + ".part");
  fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd_ < 0) throw Error("io_error", errno_msg("cannot create " + path_.string()));
}

Upload::~Upload() {
  if (fd_ >= 0) ::close(fd_);
  if (!path_.empty()) ::unlink(path_.c_str());  // no-op after a successful rename
  ::rmdir(scope_dir_.c_str());  // remove only an empty scope; staged results/other uploads survive
}

void Upload::write(const char* data, size_t len) {
  if (size_ + len > limit_) throw Error("too_large", "artifact exceeds the limit of " + std::to_string(limit_) + " bytes");
  write_all(fd_, data, len, path_);
  hash_->update(data, len);
  size_ += len;
}

// ------------------------------------------------------------------ ArtifactStore

ArtifactStore::ArtifactStore(fs::path artifacts_dir, uint64_t max_artifact_bytes) : root_(std::move(artifacts_dir)), max_bytes_(max_artifact_bytes) {
  for (const char* sub : {"", "inputs", "outputs", "logs", "tmp"}) {
    fs::path p = sub[0] ? root_ / sub : root_;
    std::error_code ec;
    fs::create_directories(p, ec);
    if (ec) throw Error("io_error", "cannot create " + p.string() + ": " + ec.message());
    ::chmod(p.c_str(), 0700);
  }
}

fs::path ArtifactStore::blob_path(BlobKind kind, const std::string& sha256) const {
  if (!is_sha256_hex(sha256)) throw Error("invalid_argument", "malformed sha256 digest");
  return root_ / (kind == BlobKind::Input ? "inputs" : "outputs") / sha256;
}

fs::path ArtifactStore::scope_dir(const std::string& scope) const {
  if (!valid_scope(scope)) throw Error("invalid_argument", "invalid transfer scope");
  return root_ / "tmp" / scope;
}

std::unique_ptr<Upload> ArtifactStore::begin_upload(const std::string& scope) {
  fs::path dir = scope_dir(scope);
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) throw Error("io_error", "cannot create " + dir.string() + ": " + ec.message());
  try {
    return std::unique_ptr<Upload>(new Upload(dir, max_bytes_));
  } catch (...) {
    ::rmdir(dir.c_str());  // constructor failure has no Upload destructor
    throw;
  }
}

ArtifactStore::ScopePin::ScopePin(std::shared_ptr<std::mutex> mutex, bool try_only)
    : mutex_(std::move(mutex)), lock_(*mutex_, std::defer_lock) {
  if (try_only) (void)lock_.try_lock();
  else lock_.lock();
}

ArtifactStore::ScopePin ArtifactStore::pin_scope(const std::string& scope, bool try_only) {
  (void)scope_dir(scope);  // validate before recording the name
  std::shared_ptr<std::mutex> mutex;
  {
    std::lock_guard lock(scopes_mutex_);
    auto& weak = scopes_[scope];
    mutex = weak.lock();
    if (!mutex) weak = mutex = std::make_shared<std::mutex>();
  }
  return ScopePin(std::move(mutex), try_only);
}

void ArtifactStore::reap_scopes(const std::function<bool(const std::string&)>& keep) {
  // Registry housekeeping does not involve filesystem IO or Store calls.
  {
    std::lock_guard lock(scopes_mutex_);
    for (auto it = scopes_.begin(); it != scopes_.end(); ) {
      if (it->second.expired()) it = scopes_.erase(it);
      else ++it;
    }
  }
  for (const auto& entry : fs::directory_iterator(root_ / "tmp")) {
    const auto scope = entry.path().filename().string();
    if (!valid_scope(scope)) continue;
    auto pin = pin_scope(scope, true);
    if (pin.owns_lock() && !keep(scope)) discard_scope(scope);
  }
}

namespace {
void check_upload(Upload& up, const std::string& sha256, uint64_t size, const std::string& actual_sha, int fd) {
  if (up.size() != size) throw Error("digest_mismatch", "uploaded " + std::to_string(up.size()) + " bytes, declared " + std::to_string(size));
  if (actual_sha != sha256) throw Error("digest_mismatch", "uploaded content does not match the declared sha256");
  if (::fsync(fd) != 0) throw Error("io_error", errno_msg("fsync failed"));
}
}  // namespace

PublishOutcome ArtifactStore::publish(BlobKind kind, Upload& up, const std::string& sha256, uint64_t size) {
  fs::path final_path = blob_path(kind, sha256);
  check_upload(up, sha256, size, up.hash_->finish(), up.fd_);
  if (::fchmod(up.fd_, 0444) != 0) throw Error("io_error", errno_msg("cannot make upload immutable"));
  if (::fsync(up.fd_) != 0) throw Error("io_error", errno_msg("cannot fsync upload"));
  return install_blob(up.path_, final_path, sha256, size);
}

void ArtifactStore::stage(Upload& up, const std::string& sha256, uint64_t size) {
  if (!is_sha256_hex(sha256)) throw Error("invalid_argument", "malformed sha256 digest");
  check_upload(up, sha256, size, up.hash_->finish(), up.fd_);
  fs::path staged_path = up.scope_dir_ / (sha256 + ".staged");
  if (::rename(up.path_.c_str(), staged_path.c_str()) != 0) throw Error("io_error", errno_msg("cannot stage upload"));
  up.path_.clear();
}

bool ArtifactStore::staged(const std::string& scope, const std::string& sha256, uint64_t size) {
  if (!is_sha256_hex(sha256)) return false;
  struct stat st{};
  fs::path p = scope_dir(scope) / (sha256 + ".staged");
  return ::lstat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && static_cast<uint64_t>(st.st_size) == size;
}

PublishOutcome ArtifactStore::publish_staged(BlobKind kind, const std::string& scope, const std::string& sha256, uint64_t size) {
  fs::path src = scope_dir(scope) / (sha256 + ".staged");
  fs::path dst = blob_path(kind, sha256);
  int fd = ::open(src.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) throw Error("missing_upload", "no staged upload with digest " + sha256);
  struct Closer {
    int fd;
    ~Closer() { ::close(fd); }
  } closer{fd};
  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || static_cast<uint64_t>(st.st_size) != size) throw Error("digest_mismatch", "staged file size changed");
  if (hash_fd(fd) != sha256) throw Error("digest_mismatch", "staged file content changed");
  if (::fchmod(fd, 0444) != 0) throw Error("io_error", errno_msg("cannot make staged file immutable"));
  if (::fsync(fd) != 0) throw Error("io_error", errno_msg("cannot fsync staged file"));
  return install_blob(src, dst, sha256, size);
}

void ArtifactStore::discard_scope(const std::string& scope) {
  std::error_code ec;
  fs::remove_all(scope_dir(scope), ec);
}

std::optional<fs::path> ArtifactStore::find(BlobKind kind, const std::string& sha256, uint64_t size) {
  fs::path p = blob_path(kind, sha256);
  struct stat st{};
  if (::lstat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || static_cast<uint64_t>(st.st_size) != size) return std::nullopt;
  return p;
}

bool ArtifactStore::ensure_durable(BlobKind kind, const std::string& sha256, uint64_t size) {
  fs::path p = blob_path(kind, sha256);
  int fd = ::open(p.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  struct Closer {
    int fd;
    ~Closer() { ::close(fd); }
  } closer{fd};
  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || static_cast<uint64_t>(st.st_size) != size || hash_fd(fd) != sha256) return false;
  if (::fsync(fd) != 0) throw Error("io_error", errno_msg("cannot fsync " + p.string()));
  fsync_dir(p.parent_path());
  return true;
}

void ArtifactStore::verify(BlobKind kind, const std::string& sha256, uint64_t size) {
  fs::path p = blob_path(kind, sha256);
  int fd = ::open(p.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) throw Error("artifact_corrupt", "stored artifact " + sha256 + " is missing");
  struct Closer {
    int fd;
    ~Closer() { ::close(fd); }
  } closer{fd};
  struct stat st{};
  if (::fstat(fd, &st) != 0 || static_cast<uint64_t>(st.st_size) != size) throw Error("artifact_corrupt", "stored artifact " + sha256 + " has an unexpected size");
  if (hash_fd(fd) != sha256) throw Error("artifact_corrupt", "stored artifact " + sha256 + " does not match its digest");
}

fs::path ArtifactStore::log_path(const std::string& job_id, const std::string& task_id, int attempt, const std::string& stream) {
  if (!valid_scope(job_id) || !valid_scope(task_id)) throw Error("invalid_argument", "invalid job or task id");
  if (stream != "stdout" && stream != "stderr") throw Error("invalid_argument", "stream must be stdout or stderr");
  return root_ / "logs" / job_id / task_id / (std::to_string(attempt) + "." + stream);
}

void ArtifactStore::write_log(const std::string& job_id, const std::string& task_id, int attempt, const std::string& stream, std::string_view data) {
  fs::path p = log_path(job_id, task_id, attempt, stream);
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  if (ec) throw Error("io_error", "cannot create " + p.parent_path().string() + ": " + ec.message());
  write_file_atomic(p, data, 0600);
}

// ------------------------------------------------------------------ workspace operations

int create_confined_file(const fs::path& root, std::string_view rel, unsigned mode) {
  Walk w = walk_parent(root, rel, true);
  int fd = ::openat(w.dirfd, w.leaf.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
  if (fd < 0) {
    if (errno == EEXIST) throw Error("conflict", "'" + std::string(rel) + "' already exists (duplicate destination or symlink)");
    throw Error("io_error", errno_msg("cannot create '" + std::string(rel) + "'"));
  }
  return fd;
}

int open_confined_regular(const fs::path& root, std::string_view rel, uint64_t* size_out) {
  Walk w = walk_parent(root, rel, false);
  int fd = ::openat(w.dirfd, w.leaf.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) throw Error("not_found", "'" + std::string(rel) + "' does not exist");
    if (errno == ELOOP) throw Error("unsafe_path", "'" + std::string(rel) + "' is a symlink");
    throw Error("io_error", errno_msg("cannot open '" + std::string(rel) + "'"));
  }
  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    throw Error("unsafe_path", "'" + std::string(rel) + "' is not a regular file");
  }
  if (size_out) *size_out = static_cast<uint64_t>(st.st_size);
  return fd;
}

FileDigest digest_confined_regular(const fs::path& root, std::string_view rel, uint64_t max_bytes) {
  uint64_t size = 0;
  int fd = open_confined_regular(root, rel, &size);
  struct Closer {
    int fd;
    ~Closer() { ::close(fd); }
  } closer{fd};
  if (size > max_bytes) throw Error("too_large", "'" + std::string(rel) + "' is " + std::to_string(size) + " bytes, above the limit of " + std::to_string(max_bytes));
  return {size, hash_fd(fd)};
}

}  // namespace coffee
