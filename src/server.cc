#include "server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstring>
#include <vector>

#include "file_util.h"

namespace lsm {

namespace {

// Splits off the first space-separated token from *s.
std::string_view NextToken(std::string_view* s) {
  size_t start = s->find_first_not_of(' ');
  if (start == std::string_view::npos) {
    *s = std::string_view();
    return std::string_view();
  }
  s->remove_prefix(start);
  size_t end = s->find(' ');
  std::string_view tok = s->substr(0, end);
  s->remove_prefix(end == std::string_view::npos ? s->size() : end);
  return tok;
}

bool ParseCount(std::string_view s, size_t* out) {
  if (s.empty() || s.size() > 9) return false;
  size_t n = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    n = n * 10 + static_cast<size_t>(c - '0');
  }
  *out = n;
  return true;
}

void AppendError(std::string* reply, const std::string& msg) {
  reply->append("ERROR ").append(msg).append("\n");
}

}  // namespace

bool HandleCommand(DB* db, std::string_view line, std::string* reply) {
  std::string_view rest = line;
  std::string cmd(NextToken(&rest));
  for (char& c : cmd) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

  if (cmd == "PUT") {
    std::string_view key = NextToken(&rest);
    if (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);  // one separator
    if (key.empty() || rest.empty()) {
      AppendError(reply, "usage: PUT <key> <value>");
      return true;
    }
    Status s = db->Put(key, rest);
    if (s.ok()) reply->append("OK\n");
    else AppendError(reply, s.ToString());
  } else if (cmd == "GET" || cmd == "DEL") {
    std::string_view key = NextToken(&rest);
    if (key.empty() || !NextToken(&rest).empty()) {
      AppendError(reply, "usage: " + cmd + " <key>");
      return true;
    }
    if (cmd == "DEL") {
      Status s = db->Delete(key);
      if (s.ok()) reply->append("OK\n");
      else AppendError(reply, s.ToString());
      return true;
    }
    std::string value;
    Status s = db->Get(key, &value);
    if (s.ok()) reply->append("VALUE ").append(value).append("\n");
    else if (s.IsNotFound()) reply->append("NOT_FOUND\n");
    else AppendError(reply, s.ToString());
  } else if (cmd == "SCAN") {
    std::string_view start = NextToken(&rest);
    std::string_view count_str = NextToken(&rest);
    size_t count = 0;
    if (start.empty() || !ParseCount(count_str, &count) || !NextToken(&rest).empty()) {
      AppendError(reply, "usage: SCAN <start> <count>");
      return true;
    }
    if (count > kMaxScanCount) {
      AppendError(reply, "count must be <= " + std::to_string(kMaxScanCount));
      return true;
    }
    if (start == "\"\"") start = std::string_view();
    std::vector<std::pair<std::string, std::string>> rows;
    Status s = db->Scan(start, count, &rows);
    if (!s.ok()) {
      AppendError(reply, s.ToString());
      return true;
    }
    for (const auto& [k, v] : rows) reply->append(k).append(" ").append(v).append("\n");
    reply->append("END\n");
  } else if (cmd == "PING") {
    reply->append("PONG\n");
  } else if (cmd == "STATS") {
    DBStats st = db->GetStats();
    reply->append("STATS tables=" + std::to_string(st.num_tables) +
                  " table_bytes=" + std::to_string(st.table_bytes) +
                  " flushes=" + std::to_string(st.flushes) +
                  " compactions=" + std::to_string(st.compactions) +
                  " user_bytes=" + std::to_string(st.user_bytes_written) +
                  " disk_bytes=" + std::to_string(st.DiskBytesWritten()) +
                  " write_stalls=" + std::to_string(st.write_stalls) + "\n");
  } else if (cmd == "QUIT") {
    reply->append("BYE\n");
    return false;
  } else if (cmd.empty()) {
    AppendError(reply, "empty command");
  } else {
    AppendError(reply, "unknown command " + cmd);
  }
  return true;
}

Status Server::Start() {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) return ErrnoStatus("socket");
  int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(options_.port));
  if (::inet_pton(AF_INET, options_.host.c_str(), &addr.sin_addr) != 1) {
    return Status::InvalidArgument("bad host " + options_.host);
  }
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    return ErrnoStatus("bind " + options_.host + ":" + std::to_string(options_.port));
  }
  if (::listen(listen_fd_, 128) != 0) return ErrnoStatus("listen");
  socklen_t len = sizeof(addr);
  ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
  port_ = ntohs(addr.sin_port);
  accept_thread_ = std::thread([this] { AcceptLoop(); });
  return Status::OK();
}

void Server::AcceptLoop() {
  while (!stopping_.load()) {
    // poll with a timeout so Stop() is noticed: on macOS, closing a listening
    // socket from another thread does not reliably wake a blocked accept().
    pollfd p{listen_fd_, POLLIN, 0};
    int r = ::poll(&p, 1, 100);
    ReapFinished();
    if (r <= 0) continue;
    int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) continue;
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));  // small replies, no Nagle delay
#ifdef SO_NOSIGPIPE
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));  // EPIPE instead of SIGPIPE
#endif
    std::lock_guard<std::mutex> l(mu_);
    if (stopping_.load()) {
      ::close(fd);
      break;
    }
    auto c = std::make_unique<Conn>();
    c->fd = fd;
    Conn* raw = c.get();
    conns_.push_back(std::move(c));
    raw->thread = std::thread([this, raw] { Serve(raw); });
  }
}

void Server::Serve(Conn* c) {
  int fd;
  {
    std::lock_guard<std::mutex> l(mu_);  // pairs with the push in AcceptLoop
    fd = c->fd;
  }
  std::string buf, reply;
  char chunk[64 << 10];
  bool open = true;
  while (open) {
    ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;  // client closed, error, or Stop() shut the socket down
    buf.append(chunk, static_cast<size_t>(n));
    // Handle every complete line; several may arrive in one read (pipelining).
    size_t pos = 0;
    reply.clear();
    while (open) {
      size_t nl = buf.find('\n', pos);
      if (nl == std::string::npos) break;
      std::string_view line(buf.data() + pos, nl - pos);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      open = HandleCommand(db_, line, &reply);
      pos = nl + 1;
    }
    buf.erase(0, pos);
    if (buf.size() > options_.max_line) {
      AppendError(&reply, "line too long");
      open = false;
    }
    if (!reply.empty() && !WriteAll(fd, reply.data(), reply.size()).ok()) break;
  }
  {
    std::lock_guard<std::mutex> l(mu_);
    c->fd = -1;
  }
  ::close(fd);
  c->done.store(true);
}

void Server::ReapFinished() {
  std::list<std::unique_ptr<Conn>> finished;
  {
    std::lock_guard<std::mutex> l(mu_);
    for (auto it = conns_.begin(); it != conns_.end();) {
      if ((*it)->done.load()) {
        finished.push_back(std::move(*it));
        it = conns_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& c : finished) c->thread.join();
}

size_t Server::ActiveConnections() const {
  std::lock_guard<std::mutex> l(mu_);
  size_t n = 0;
  for (const auto& c : conns_) n += !c->done.load();
  return n;
}

void Server::Stop() {
  if (stopping_.exchange(true)) return;
  if (accept_thread_.joinable()) accept_thread_.join();
  if (listen_fd_ >= 0) ::close(listen_fd_);
  listen_fd_ = -1;
  {
    // Wake every blocked read(); each thread then closes its own socket.
    std::lock_guard<std::mutex> l(mu_);
    for (auto& c : conns_) {
      if (c->fd >= 0) ::shutdown(c->fd, SHUT_RDWR);
    }
  }
  std::list<std::unique_ptr<Conn>> all;
  {
    std::lock_guard<std::mutex> l(mu_);
    all.swap(conns_);
  }
  for (auto& c : all) c->thread.join();
}

}  // namespace lsm
