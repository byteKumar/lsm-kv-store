#pragma once

// A small TCP server for the database, one thread per connection.
//
// Text protocol, one command per line (\n or \r\n):
//
//   PUT <key> <value>      -> OK
//   GET <key>              -> VALUE <value>   |  NOT_FOUND
//   DEL <key>              -> OK
//   SCAN <start> <count>   -> <key> <value> lines (up to count), then END
//                             (use "" for <start> to scan from the beginning)
//   PING                   -> PONG
//   STATS                  -> STATS name=value ...
//   QUIT                   -> BYE, then the server closes the connection
//   anything else          -> ERROR <message>
//
// Keys are non-empty and contain no spaces. A value is the rest of the line
// after the key, so it may contain spaces but not newlines. (Binary-safe
// keys and values would need length-prefixed framing, like Redis's RESP.)
//
// "OK" for PUT/DEL is sent only after DB::Put/Delete returned, i.e. after the
// write is in the WAL and synced per the database's sync mode. That reply is
// what the crash test treats as an acknowledgement.

#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "db.h"
#include "status.h"

namespace lsm {

constexpr size_t kMaxScanCount = 10000;

// Runs one command line against db and appends the reply (with trailing
// newlines) to *reply. Returns false if the connection should be closed.
bool HandleCommand(DB* db, std::string_view line, std::string* reply);

struct ServerOptions {
  std::string host = "127.0.0.1";  // listen on loopback only unless told otherwise
  int port = 0;                    // 0 = pick a free port
  size_t max_line = 1 << 20;       // longer request lines get an error and a close
};

class Server {
 public:
  Server(DB* db, ServerOptions options) : db_(db), options_(std::move(options)) {}
  ~Server() { Stop(); }

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  Status Start();
  // Stops accepting, closes every connection, joins all threads. Idempotent.
  void Stop();

  int port() const { return port_; }
  size_t ActiveConnections() const;

 private:
  struct Conn {
    int fd = -1;  // set to -1 (under mu_) just before the thread closes it
    std::thread thread;
    std::atomic<bool> done{false};
  };

  void AcceptLoop();
  void Serve(Conn* c);
  void ReapFinished();  // joins threads of closed connections

  DB* db_;
  ServerOptions options_;
  int listen_fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stopping_{false};
  std::thread accept_thread_;
  mutable std::mutex mu_;
  std::list<std::unique_ptr<Conn>> conns_;
};

}  // namespace lsm
