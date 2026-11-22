#include "backend.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <vector>

namespace fvmap {

namespace {

struct Pipe {
  int fd[2]{-1, -1};
  Pipe() {
    if (::pipe(fd) != 0) throw Error(503, "backend_unavailable", "cannot create backend pipe");
    for (int f : fd) ::fcntl(f, F_SETFD, FD_CLOEXEC);
  }
  ~Pipe() {
    for (int f : fd)
      if (f >= 0) ::close(f);
  }
  void close_end(int i) {
    if (fd[i] >= 0) ::close(fd[i]);
    fd[i] = -1;
  }
};

struct Spawned {
  pid_t pid = -1;
  ~Spawned() {
    if (pid > 0) {
      ::kill(pid, SIGKILL);
      int st;
      ::waitpid(pid, &st, 0);
    }
  }
};

// Runs argv with env, returns stdout. Kills the child on deadline, overflow or cancel.
std::string run_bounded(const std::vector<std::string> &argv, const std::vector<std::string> &env,
                        int timeout_ms, std::size_t max_bytes, const std::atomic<bool> *cancel,
                        int &exit_code) {
  Pipe out, err;
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, out.fd[1], 1);
  posix_spawn_file_actions_adddup2(&fa, err.fd[1], 2);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  short flags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
  flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
  posix_spawnattr_setflags(&attr, flags);
  sigset_t all;
  sigfillset(&all);
  posix_spawnattr_setsigdefault(&attr, &all);
  std::vector<char *> a, e;
  for (auto &s : argv) a.push_back(const_cast<char *>(s.c_str()));
  a.push_back(nullptr);
  for (auto &s : env) e.push_back(const_cast<char *>(s.c_str()));
  e.push_back(nullptr);
  Spawned child;
  int rc = ::posix_spawn(&child.pid, a[0], &fa, &attr, a.data(), e.data());
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  if (rc != 0) {
    child.pid = -1;
    throw Error(503, "backend_unavailable", "map engine could not be started");
  }
  out.close_end(1);
  err.close_end(1);
  std::string body, errors;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  bool out_open = true, err_open = true;
  char buf[1 << 16];
  while (out_open || err_open) {
    if (cancel && cancel->load()) throw Error(499, "cancelled", "request cancelled");
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) throw Error(504, "backend_timeout", "map engine exceeded its time bound");
    pollfd p[2] = {{out_open ? out.fd[0] : -1, POLLIN, 0}, {err_open ? err.fd[0] : -1, POLLIN, 0}};
    int n = ::poll(p, 2, static_cast<int>(std::min<long long>(left, 100)));
    if (n < 0 && errno != EINTR) throw Error(502, "backend_failure", "backend poll failed");
    for (int i = 0; i < 2; ++i) {
      if (!(p[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      ssize_t r = ::read(p[i].fd, buf, sizeof buf);
      if (r <= 0) {
        (i == 0 ? out_open : err_open) = false;
        continue;
      }
      if (i == 0) {
        body.append(buf, static_cast<std::size_t>(r));
        if (body.size() > max_bytes) throw Error(502, "backend_output_too_large", "map engine output exceeded its bound");
      } else if (errors.size() < 4096) {
        errors.append(buf, static_cast<std::size_t>(r));
      }
    }
  }
  int status = 0;
  for (;;) {
    pid_t w = ::waitpid(child.pid, &status, WNOHANG);
    if (w == child.pid) break;
    if (std::chrono::steady_clock::now() > deadline) throw Error(504, "backend_timeout", "map engine did not exit");
    ::usleep(1000);
  }
  child.pid = -1;
  exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
  return body;
}

}  // namespace

Backend::Backend(const BackendConfig &config) : config_(config), slots_(config.max_concurrency) {
  struct stat st{};
  if (::stat(config.mapserv.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR))
    throw std::runtime_error("backend mapserv is not an executable file");
  sha256_ = sha256_file_hex(config.mapserv);
  if (config.sha256 && *config.sha256 != sha256_)
    throw std::runtime_error("backend mapserv does not match its pinned sha256");
  int code = 0;
  std::vector<std::string> env;
  for (auto &[k, v] : config.env) env.push_back(k + "=" + v);
  version_ = run_bounded({config.mapserv.string(), "-v"}, env, config.timeout_ms, 4096, nullptr, code);
  while (!version_.empty() && (version_.back() == '\n' || version_.back() == '\r')) version_.pop_back();
  if (code != 0 || !version_.starts_with("MapServer version"))
    throw std::runtime_error("backend did not identify as MapServer");
  if (version_.find("SUPPORTS=WMS_SERVER") == std::string::npos)
    throw std::runtime_error("backend was built without WMS server support");
}

bool Backend::healthy() const {
  struct stat st{};
  return ::stat(config_.mapserv.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

BackendResult Backend::run(const std::string &mapserver_config, const std::string &alias,
                           const std::string &query, const std::string &server_port, int queue_timeout_ms,
                           const std::atomic<bool> *cancel) {
  if (!slots_.try_acquire_for(std::chrono::milliseconds(queue_timeout_ms)))
    throw Error(503, "backend_saturated", "all map engine slots are busy");
  struct Release {
    Backend *b;
    ~Release() {
      b->inflight_--;
      b->slots_.release();
    }
  } release{this};
  inflight_++;
  std::vector<std::string> env = {
      "MAPSERVER_CONFIG_FILE=" + mapserver_config,
      "REQUEST_METHOD=GET",
      "QUERY_STRING=map=" + alias + "&" + query,
      "SERVER_NAME=127.0.0.1",
      "SERVER_PORT=" + server_port,
      "SCRIPT_NAME=/wms",
      "GATEWAY_INTERFACE=CGI/1.1",
      "PROJ_NETWORK=OFF",
      "GDAL_HTTP_UNSAFESSL=NO",
      "CPL_VSIL_CURL_ALLOWED_EXTENSIONS=.none",
  };
  for (auto &[k, v] : config_.env) env.push_back(k + "=" + v);
  uint64_t t0 = monotonic_us();
  int code = 0;
  std::string raw = run_bounded({config_.mapserv.string()}, env, config_.timeout_ms, config_.max_output_bytes,
                                cancel, code);
  BackendResult r;
  r.duration_us = monotonic_us() - t0;
  if (code != 0) throw Error(502, "backend_failure", "map engine exited with status " + std::to_string(code));
  std::size_t split = raw.find("\r\n\r\n");
  std::size_t skip = 4;
  if (split == std::string::npos || split > 8192) {
    split = raw.find("\n\n");
    skip = 2;
  }
  if (split == std::string::npos || split > 8192) throw Error(502, "backend_failure", "map engine produced no CGI headers");
  std::string headers = raw.substr(0, split);
  r.body = raw.substr(split + skip);
  std::size_t pos = 0;
  while (pos < headers.size()) {
    std::size_t end = headers.find('\n', pos);
    std::string line = headers.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string lower = line;
    for (char &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.starts_with("content-type:")) {
      r.content_type = line.substr(13);
      while (!r.content_type.empty() && r.content_type.front() == ' ') r.content_type.erase(0, 1);
    } else if (lower.starts_with("status:")) {
      r.cgi_status = std::atoi(line.c_str() + 7);
    }
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  if (r.content_type.empty()) throw Error(502, "backend_failure", "map engine response has no content type");
  return r;
}

}  // namespace fvmap
