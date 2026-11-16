#include "server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "test_util.h"

namespace lsm {
namespace {

std::string Exec(DB* db, std::string_view line, bool* keep_open = nullptr) {
  std::string reply;
  bool open = HandleCommand(db, line, &reply);
  if (keep_open) *keep_open = open;
  return reply;
}

class ProtocolTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(DB::Open(Options{}, dir_.path(), &db_).ok()); }
  test::TempDir dir_;
  std::unique_ptr<DB> db_;
};

TEST_F(ProtocolTest, PutGetDel) {
  EXPECT_EQ(Exec(db_.get(), "PUT user:1 alice"), "OK\n");
  EXPECT_EQ(Exec(db_.get(), "GET user:1"), "VALUE alice\n");
  EXPECT_EQ(Exec(db_.get(), "DEL user:1"), "OK\n");
  EXPECT_EQ(Exec(db_.get(), "GET user:1"), "NOT_FOUND\n");
  EXPECT_EQ(Exec(db_.get(), "get missing"), "NOT_FOUND\n");  // commands are case-insensitive
}

TEST_F(ProtocolTest, ValueKeepsInnerSpaces) {
  EXPECT_EQ(Exec(db_.get(), "PUT k hello  big world"), "OK\n");
  EXPECT_EQ(Exec(db_.get(), "GET k"), "VALUE hello  big world\n");
}

TEST_F(ProtocolTest, Scan) {
  for (const char* k : {"b", "a", "d", "c"}) Exec(db_.get(), std::string("PUT ") + k + " v" + k);
  Exec(db_.get(), "DEL c");
  EXPECT_EQ(Exec(db_.get(), "SCAN b 10"), "b vb\nd vd\nEND\n");
  EXPECT_EQ(Exec(db_.get(), "SCAN \"\" 2"), "a va\nb vb\nEND\n");
  EXPECT_EQ(Exec(db_.get(), "SCAN z 5"), "END\n");
  EXPECT_EQ(Exec(db_.get(), "SCAN a 0"), "END\n");
}

TEST_F(ProtocolTest, Errors) {
  EXPECT_EQ(Exec(db_.get(), "PUT onlykey"), "ERROR usage: PUT <key> <value>\n");
  EXPECT_EQ(Exec(db_.get(), "GET"), "ERROR usage: GET <key>\n");
  EXPECT_EQ(Exec(db_.get(), "GET a b"), "ERROR usage: GET <key>\n");
  EXPECT_EQ(Exec(db_.get(), "SCAN a"), "ERROR usage: SCAN <start> <count>\n");
  EXPECT_EQ(Exec(db_.get(), "SCAN a -1"), "ERROR usage: SCAN <start> <count>\n");
  EXPECT_EQ(Exec(db_.get(), "SCAN a 10001"), "ERROR count must be <= 10000\n");
  EXPECT_EQ(Exec(db_.get(), "FROB x"), "ERROR unknown command FROB\n");
  EXPECT_EQ(Exec(db_.get(), ""), "ERROR empty command\n");
}

TEST_F(ProtocolTest, PingStatsQuit) {
  EXPECT_EQ(Exec(db_.get(), "PING"), "PONG\n");
  EXPECT_EQ(Exec(db_.get(), "STATS").rfind("STATS tables=0 ", 0), 0u);
  bool open = true;
  EXPECT_EQ(Exec(db_.get(), "QUIT", &open), "BYE\n");
  EXPECT_FALSE(open);
}

// --- real sockets ---------------------------------------------------------

class Client {
 public:
  explicit Client(int port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    ok_ = ::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));  // macOS: no SIGPIPE
#endif
  }
  ~Client() {
    if (fd_ >= 0) ::close(fd_);
  }
  bool ok() const { return ok_; }
  void Send(const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
#ifdef MSG_NOSIGNAL
      ssize_t n = ::send(fd_, s.data() + off, s.size() - off, MSG_NOSIGNAL);  // Linux: no SIGPIPE
#else
      ssize_t n = ::send(fd_, s.data() + off, s.size() - off, 0);
#endif
      if (n <= 0) return;
      off += static_cast<size_t>(n);
    }
  }
  // Reads one reply line (without the newline). Empty string on EOF.
  std::string ReadLine() {
    while (true) {
      size_t nl = buf_.find('\n');
      if (nl != std::string::npos) {
        std::string line = buf_.substr(0, nl);
        buf_.erase(0, nl + 1);
        return line;
      }
      char chunk[4096];
      ssize_t n = ::read(fd_, chunk, sizeof(chunk));
      if (n <= 0) return "";
      buf_.append(chunk, static_cast<size_t>(n));
    }
  }
  std::string Call(const std::string& cmd) {
    Send(cmd + "\n");
    return ReadLine();
  }

 private:
  int fd_ = -1;
  bool ok_ = false;
  std::string buf_;
};

class ServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Options o;
    o.memtable_size = 64 << 10;  // flushes and compactions happen during the tests
    ASSERT_TRUE(DB::Open(o, dir_.path(), &db_).ok());
    server_ = std::make_unique<Server>(db_.get(), ServerOptions{});
    ASSERT_TRUE(server_->Start().ok());
    ASSERT_GT(server_->port(), 0);
  }
  void TearDown() override {
    server_.reset();
    db_.reset();
  }
  test::TempDir dir_;
  std::unique_ptr<DB> db_;
  std::unique_ptr<Server> server_;
};

TEST_F(ServerTest, BasicCommandsOverTcp) {
  Client c(server_->port());
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c.Call("PING"), "PONG");
  EXPECT_EQ(c.Call("PUT a 1"), "OK");
  EXPECT_EQ(c.Call("GET a"), "VALUE 1");
  EXPECT_EQ(c.Call("PUT b 2\r"), "OK");  // \r\n line endings work too
  c.Send("SCAN \"\" 10\n");
  EXPECT_EQ(c.ReadLine(), "a 1");
  EXPECT_EQ(c.ReadLine(), "b 2");
  EXPECT_EQ(c.ReadLine(), "END");
  EXPECT_EQ(c.Call("QUIT"), "BYE");
  EXPECT_EQ(c.ReadLine(), "");  // server closed the connection
}

TEST_F(ServerTest, PipelinedCommandsInOneWrite) {
  Client c(server_->port());
  std::string batch;
  for (int i = 0; i < 500; i++) batch += "PUT k" + std::to_string(i) + " v" + std::to_string(i) + "\n";
  for (int i = 0; i < 500; i++) batch += "GET k" + std::to_string(i) + "\n";
  c.Send(batch);
  for (int i = 0; i < 500; i++) ASSERT_EQ(c.ReadLine(), "OK");
  for (int i = 0; i < 500; i++) ASSERT_EQ(c.ReadLine(), "VALUE v" + std::to_string(i));
}

TEST_F(ServerTest, CommandSplitAcrossWrites) {
  Client c(server_->port());
  c.Send("PU");
  c.Send("T split ");
  c.Send("value\nGE");
  c.Send("T split\n");
  EXPECT_EQ(c.ReadLine(), "OK");
  EXPECT_EQ(c.ReadLine(), "VALUE value");
}

TEST_F(ServerTest, ManyConcurrentClients) {
  constexpr int kClients = 8, kOps = 400;
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kClients; t++) {
    threads.emplace_back([&, t] {
      Client c(server_->port());
      if (!c.ok()) {
        errors++;
        return;
      }
      for (int i = 0; i < kOps; i++) {
        std::string k = "c" + std::to_string(t) + "_" + std::to_string(i);
        if (c.Call("PUT " + k + " " + std::string(100, 'x')) != "OK") errors++;
        if (c.Call("GET " + k) != "VALUE " + std::string(100, 'x')) errors++;
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(errors.load(), 0);
  Client c(server_->port());
  for (int t = 0; t < kClients; t++) {
    EXPECT_EQ(c.Call("GET c" + std::to_string(t) + "_" + std::to_string(kOps - 1)),
              "VALUE " + std::string(100, 'x'));
  }
}

TEST_F(ServerTest, OverlongLineClosesConnection) {
  Client c(server_->port());
  c.Send(std::string((1 << 20) + 100, 'a'));  // no newline, over the 1 MB limit
  EXPECT_EQ(c.ReadLine(), "ERROR line too long");
  EXPECT_EQ(c.ReadLine(), "");
  Client c2(server_->port());  // the server itself is fine
  EXPECT_EQ(c2.Call("PING"), "PONG");
}

TEST_F(ServerTest, StopClosesIdleConnections) {
  Client a(server_->port()), b(server_->port());
  EXPECT_EQ(a.Call("PING"), "PONG");
  EXPECT_EQ(b.Call("PING"), "PONG");
  EXPECT_EQ(server_->ActiveConnections(), 2u);
  server_->Stop();  // must not hang on the two idle clients
  EXPECT_EQ(a.ReadLine(), "");
  EXPECT_EQ(server_->ActiveConnections(), 0u);
}

TEST_F(ServerTest, FinishedConnectionsAreReaped) {
  for (int i = 0; i < 20; i++) {
    Client c(server_->port());
    EXPECT_EQ(c.Call("PING"), "PONG");
  }
  // Closed clients' threads are joined by the accept loop within ~100 ms.
  for (int i = 0; i < 50 && server_->ActiveConnections() > 0; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_EQ(server_->ActiveConnections(), 0u);
}

}  // namespace
}  // namespace lsm
