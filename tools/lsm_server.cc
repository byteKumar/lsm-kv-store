// lsm_server: serves a database directory over TCP.
//
//   lsm_server --dir DIR [--port 7070] [--host 127.0.0.1] [--sync none|fsync|full]
//              [--memtable-mb 64] [--no-compaction]
//
// Prints "listening on HOST:PORT" once ready. SIGINT/SIGTERM shut down
// cleanly; kill -9 is what the crash test uses.

#include <signal.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "db.h"
#include "server.h"

namespace {
void Usage() {
  std::fprintf(stderr,
               "usage: lsm_server --dir DIR [--port N] [--host ADDR] [--sync none|fsync|full]\n"
               "                  [--memtable-mb N] [--no-compaction]\n");
  std::exit(2);
}
}  // namespace

int main(int argc, char** argv) {
  std::string dir;
  lsm::Options options;
  lsm::ServerOptions server_options;
  server_options.port = 7070;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto need = [&]() -> std::string {
      if (i + 1 >= argc) Usage();
      return argv[++i];
    };
    if (a == "--dir") dir = need();
    else if (a == "--port") server_options.port = std::atoi(need().c_str());
    else if (a == "--host") server_options.host = need();
    else if (a == "--sync") {
      if (!lsm::ParseSyncMode(need(), &options.sync_mode)) Usage();
    } else if (a == "--memtable-mb") options.memtable_size = std::stoul(need()) << 20;
    else if (a == "--no-compaction") options.compaction = false;
    else Usage();
  }
  if (dir.empty()) Usage();

  // Block SIGINT/SIGTERM in every thread (threads inherit this mask) and
  // wait for them explicitly below. Ignore SIGPIPE from closed sockets.
  sigset_t sigs;
  sigemptyset(&sigs);
  sigaddset(&sigs, SIGINT);
  sigaddset(&sigs, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &sigs, nullptr);
  signal(SIGPIPE, SIG_IGN);

  std::unique_ptr<lsm::DB> db;
  lsm::Status s = lsm::DB::Open(options, dir, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "open %s: %s\n", dir.c_str(), s.ToString().c_str());
    return 1;
  }
  lsm::Server server(db.get(), server_options);
  s = server.Start();
  if (!s.ok()) {
    std::fprintf(stderr, "start: %s\n", s.ToString().c_str());
    return 1;
  }
  std::printf("listening on %s:%d (dir=%s sync=%s)\n", server_options.host.c_str(), server.port(),
              dir.c_str(), lsm::SyncModeName(options.sync_mode));
  std::fflush(stdout);

  int sig = 0;
  sigwait(&sigs, &sig);
  std::printf("signal %d: shutting down\n", sig);
  std::fflush(stdout);
  server.Stop();
  db.reset();
  return 0;
}
