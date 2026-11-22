// fvmapd — Finalverse Map Server gateway.
#include <signal.h>

#include <cstdio>
#include <iostream>
#include <thread>

#include <gdal_priv.h>

#include "server.hpp"

using namespace fvmap;

namespace {

int usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  fvmapd serve --config FILE        run the gateway (SIGHUP reloads, SIGTERM stops)\n"
               "  fvmapd check-config --config FILE validate configuration and token file\n"
               "  fvmapd new-token                  print a random token and its sha256 digest\n"
               "  fvmapd hash-token < TOKEN         print the sha256 digest of a token on stdin\n"
               "  fvmapd version\n");
  return 2;
}

std::string arg(int argc, char **argv, const char *name) {
  for (int i = 2; i + 1 < argc; ++i)
    if (std::string_view(argv[i]) == name) return argv[i + 1];
  return {};
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) return usage();
  std::string cmd = argv[1];
  try {
    if (cmd == "version") {
      std::printf("Finalverse Map Server 0.1.0 (fvmapd)\n");
      return 0;
    }
    if (cmd == "new-token") {
      std::string token = random_hex(32);
      std::printf("{\"token\":\"%s\",\"token_sha256\":\"%s\"}\n", token.c_str(), sha256_hex(token).c_str());
      return 0;
    }
    if (cmd == "hash-token") {
      std::string token;
      std::getline(std::cin, token);
      std::printf("%s\n", sha256_hex(token).c_str());
      return 0;
    }
    std::string path = arg(argc, argv, "--config");
    if (path.empty()) return usage();
    GDALAllRegister();
    CPLSetConfigOption("GDAL_HTTP_TIMEOUT", "1");
    CPLSetConfigOption("CPL_VSIL_CURL_ALLOWED_EXTENSIONS", ".none");
    Config config = load_config(path);
    if (cmd == "check-config") {
      TokenStore tokens(config.tokens);
      std::printf("{\"valid\":true,\"datasets\":%zu,\"worlds\":%zu,\"principals\":%zu}\n", config.datasets.size(),
                  config.worlds.size(), tokens.size());
      return 0;
    }
    if (cmd != "serve") return usage();
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    Service service(std::move(config));
    int port = service.bind();
    std::printf("READY %d\n", port);
    std::fflush(stdout);
    std::thread signals([&] {
      for (;;) {
        int sig = 0;
        if (sigwait(&set, &sig) != 0) continue;
        if (sig == SIGHUP) {
          try {
            service.reload();
          } catch (const std::exception &e) {
            log_event({{"event", "reload_failed"}, {"detail", e.what()}});
          }
        } else if (sig == SIGINT || sig == SIGTERM) {
          service.stop();
          return;
        }
      }
    });
    service.serve();
    signals.join();
    return 0;
  } catch (const std::exception &e) {
    log_event({{"event", "fatal"}, {"detail", e.what()}});
    return 1;
  }
}
