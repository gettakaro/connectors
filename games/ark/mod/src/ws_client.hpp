#pragma once
// RFC 6455 WebSocket client over a nonblocking socket, optionally TLS (OpenSSL,
// linked statically from the pinned toolchain). One owner thread drives it; no
// method is thread-safe. Never used on the game thread.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ws {

struct Url {
  bool tls = true;
  std::string host;
  uint16_t port = 443;
  std::string path = "/";
};

inline std::optional<Url> parse_url(const std::string& text) {
  Url url;
  std::string rest;
  if (text.rfind("wss://", 0) == 0) { url.tls = true; url.port = 443; rest = text.substr(6); }
  else if (text.rfind("ws://", 0) == 0) { url.tls = false; url.port = 80; rest = text.substr(5); }
  else return std::nullopt;
  const size_t slash = rest.find('/');
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  url.path = slash == std::string::npos ? "/" : rest.substr(slash);
  if (authority.empty() || authority.find('@') != std::string::npos) return std::nullopt;
  if (authority.front() == '[') {
    const size_t close = authority.find(']');
    if (close == std::string::npos) return std::nullopt;
    url.host = authority.substr(1, close - 1);
    authority = authority.substr(close + 1);
    if (!authority.empty()) {
      if (authority.front() != ':') return std::nullopt;
      authority = authority.substr(1);
    } else authority.clear();
  } else {
    const size_t colon = authority.rfind(':');
    url.host = authority.substr(0, colon);
    authority = colon == std::string::npos ? std::string() : authority.substr(colon + 1);
  }
  if (!authority.empty()) {
    if (authority.size() > 5 || authority.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
    const unsigned long port = std::stoul(authority);
    if (port == 0 || port > 65535) return std::nullopt;
    url.port = static_cast<uint16_t>(port);
  }
  if (url.host.empty() || url.host.size() > 253) return std::nullopt;
  for (unsigned char c : url.path) if (c <= 0x20 || c >= 0x7f) return std::nullopt;
  return url;
}

inline std::string base64(const unsigned char* data, size_t length) {
  std::string out(4 * ((length + 2) / 3) + 1, '\0');
  const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(length));
  out.resize(n > 0 ? static_cast<size_t>(n) : 0);
  return out;
}

inline std::string accept_key(const std::string& key) {
  const std::string input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  unsigned char digest[SHA_DIGEST_LENGTH];
  SHA1(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);
  return base64(digest, sizeof(digest));
}

// The candidate CA bundles, in order; TAKARO_CA_FILE is tried first by the caller.
inline const char* const kCaCandidates[] = {
    "/etc/ssl/certs/ca-certificates.crt",  // Debian/Ubuntu/Alpine
    "/etc/pki/tls/certs/ca-bundle.crt",    // RHEL/Fedora
    "/etc/ssl/ca-bundle.pem",              // openSUSE
    "/etc/ssl/cert.pem",                   // Alpine/macOS style
};

inline bool readable(const std::string& path) { return !path.empty() && access(path.c_str(), R_OK) == 0; }

struct Frame {
  enum class Kind { text, binary, ping, pong, close } kind;
  std::string payload;
};

// Pure frame codec, separated so tests can drive it without sockets.
class Codec {
 public:
  static constexpr size_t kMaxMessage = 8 * 1024 * 1024;

  static std::string encode(uint8_t opcode, std::string_view payload, bool mask = true) {
    std::string out;
    out.push_back(static_cast<char>(0x80 | (opcode & 0x0f)));
    const uint8_t mask_bit = mask ? 0x80 : 0;
    if (payload.size() < 126) {
      out.push_back(static_cast<char>(mask_bit | payload.size()));
    } else if (payload.size() <= 0xffff) {
      out.push_back(static_cast<char>(mask_bit | 126));
      out.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
      out.push_back(static_cast<char>(payload.size() & 0xff));
    } else {
      out.push_back(static_cast<char>(mask_bit | 127));
      for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<char>((static_cast<uint64_t>(payload.size()) >> shift) & 0xff));
    }
    if (!mask) { out.append(payload); return out; }
    unsigned char key[4];
    if (RAND_bytes(key, sizeof(key)) != 1) { key[0] = 0x5a; key[1] = 0xa5; key[2] = 0x3c; key[3] = 0xc3; }
    out.append(reinterpret_cast<const char*>(key), 4);
    const size_t base = out.size();
    out.append(payload);
    for (size_t i = 0; i < payload.size(); ++i) out[base + i] = static_cast<char>(out[base + i] ^ key[i & 3]);
    return out;
  }

  enum class Status { ok, need_more, error };

  // Consumes complete frames from `in`. Server frames must be unmasked
  // (`expect_masked` lets a test server reuse the codec).
  Status decode(std::string& in, std::vector<Frame>& out, bool expect_masked = false) {
    for (;;) {
      if (in.size() < 2) return Status::need_more;
      const auto b0 = static_cast<uint8_t>(in[0]);
      const auto b1 = static_cast<uint8_t>(in[1]);
      const bool fin = b0 & 0x80;
      const uint8_t opcode = b0 & 0x0f;
      if (b0 & 0x70) return Status::error;  // no extensions negotiated
      const bool masked = b1 & 0x80;
      if (masked != expect_masked) return Status::error;
      uint64_t length = b1 & 0x7f;
      size_t header = 2;
      if (length == 126) {
        if (in.size() < 4) return Status::need_more;
        length = (static_cast<uint64_t>(static_cast<uint8_t>(in[2])) << 8) | static_cast<uint8_t>(in[3]);
        header = 4;
      } else if (length == 127) {
        if (in.size() < 10) return Status::need_more;
        length = 0;
        for (int k = 0; k < 8; ++k) length = (length << 8) | static_cast<uint8_t>(in[2 + k]);
        header = 10;
      }
      if (length > kMaxMessage) return Status::error;
      const size_t key_at = header;
      if (masked) header += 4;
      if (in.size() < header + length) return Status::need_more;
      std::string payload = in.substr(header, static_cast<size_t>(length));
      if (masked)
        for (size_t i = 0; i < payload.size(); ++i)
          payload[i] = static_cast<char>(payload[i] ^ in[key_at + (i & 3)]);
      in.erase(0, header + static_cast<size_t>(length));
      if (opcode >= 0x8) {
        if (!fin || length > 125) return Status::error;
        if (opcode == 0x8) out.push_back({Frame::Kind::close, std::move(payload)});
        else if (opcode == 0x9) out.push_back({Frame::Kind::ping, std::move(payload)});
        else if (opcode == 0xA) out.push_back({Frame::Kind::pong, std::move(payload)});
        else return Status::error;
        continue;
      }
      if (opcode == 0x0) {
        if (!fragmenting_) return Status::error;
      } else if (opcode == 0x1 || opcode == 0x2) {
        if (fragmenting_) return Status::error;
        fragment_kind_ = opcode == 0x1 ? Frame::Kind::text : Frame::Kind::binary;
        fragment_.clear();
        fragmenting_ = true;
      } else {
        return Status::error;
      }
      if (fragment_.size() + payload.size() > kMaxMessage) return Status::error;
      fragment_ += payload;
      if (fin) {
        fragmenting_ = false;
        out.push_back({fragment_kind_, std::move(fragment_)});
        fragment_.clear();
      }
    }
  }

 private:
  bool fragmenting_ = false;
  Frame::Kind fragment_kind_ = Frame::Kind::text;
  std::string fragment_;
};

class Connection {
 public:
  struct Options {
    std::string ca_file;  // empty: probe kCaCandidates
    std::chrono::milliseconds timeout{15000};
    size_t max_buffered = 2 * 1024 * 1024;
  };

  Connection() = default;
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  ~Connection() {
    close_now();
    if (ctx_) SSL_CTX_free(ctx_);
  }

  // Blocking (bounded by options.timeout) connect + TLS + upgrade. On failure the
  // connection is closed and `error` names the stage without any secret.
  bool open(const Url& url, const Options& options, std::string& error) {
    close_now();
    options_ = options;
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    if (!connect_tcp(url, deadline, error)) { close_now(); return false; }
    if (url.tls && !start_tls(url, deadline, error)) { close_now(); return false; }
    if (!upgrade(url, deadline, error)) { close_now(); return false; }
    open_ = true;
    return true;
  }

  bool is_open() const { return open_; }
  int fd() const { return fd_; }
  size_t buffered() const { return out_.size() - out_offset_; }
  uint64_t bytes_written() const { return written_total_; }

  // Queues a frame. `on_written` runs on the owner thread once every byte of
  // the frame has been handed to the kernel (not proof that the peer got it).
  bool send_text(std::string_view text, std::function<void()> on_written = {}) {
    return queue(Codec::encode(0x1, text), std::move(on_written));
  }
  bool send_ping(std::string_view payload) { return queue(Codec::encode(0x9, payload), {}); }
  bool send_close(uint16_t code) {
    std::string body;
    body.push_back(static_cast<char>(code >> 8));
    body.push_back(static_cast<char>(code & 0xff));
    return queue(Codec::encode(0x8, body), {});
  }

  bool wants_write() const { return buffered() > 0 || (ssl_ && ssl_want_write_); }

  // Nonblocking: writes what the kernel accepts. False on a fatal error.
  bool flush(std::string& error) {
    if (!open_) return false;
    while (out_offset_ < out_.size()) {
      const size_t remaining = out_.size() - out_offset_;
      const ssize_t n = raw_write(out_.data() + out_offset_, remaining, error);
      if (n < 0) return false;
      if (n == 0) break;
      out_offset_ += static_cast<size_t>(n);
      written_total_ += static_cast<uint64_t>(n);
      while (!callbacks_.empty() && callbacks_.front().first <= written_total_) {
        auto cb = std::move(callbacks_.front().second);
        callbacks_.pop_front();
        if (cb) cb();
      }
    }
    if (out_offset_ == out_.size()) { out_.clear(); out_offset_ = 0; }
    else if (out_offset_ > 65536) { out_.erase(0, out_offset_); out_offset_ = 0; }
    return true;
  }

  enum class ReadStatus { ok, closed, error };

  // Nonblocking: reads what is available and decodes frames. Pings are answered here.
  ReadStatus read(std::vector<Frame>& frames, std::string& error) {
    if (!open_) return ReadStatus::closed;
    char buf[16384];
    for (int rounds = 0; rounds < 64; ++rounds) {
      const ssize_t n = raw_read(buf, sizeof(buf), error);
      if (n < 0) return ReadStatus::error;
      if (n == 0) break;
      in_.append(buf, static_cast<size_t>(n));
      if (in_.size() > Codec::kMaxMessage + 16) { error = "inbound frame too large"; return ReadStatus::error; }
    }
    if (eof_ && in_.empty()) return ReadStatus::closed;
    std::vector<Frame> decoded;
    if (codec_.decode(in_, decoded) == Codec::Status::error) { error = "websocket protocol error"; return ReadStatus::error; }
    for (auto& frame : decoded) {
      if (frame.kind == Frame::Kind::ping) { queue(Codec::encode(0xA, frame.payload), {}); continue; }
      if (frame.kind == Frame::Kind::close) {
        if (!close_sent_) { send_close(1000); close_sent_ = true; std::string ignored; flush(ignored); }
        frames.push_back(std::move(frame));
        return ReadStatus::closed;
      }
      frames.push_back(std::move(frame));
    }
    if (eof_) return ReadStatus::closed;
    return ReadStatus::ok;
  }

  short poll_events() const {
    short events = POLLIN;
    if (wants_write()) events |= POLLOUT;
    if (ssl_ && ssl_want_write_) events |= POLLOUT;
    return events;
  }

  void close_now() {
    if (ssl_) { SSL_free(ssl_); ssl_ = nullptr; }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    open_ = false;
    eof_ = false;
    close_sent_ = false;
    ssl_want_write_ = false;
    in_.clear();
    out_.clear();
    out_offset_ = 0;
    callbacks_.clear();
    codec_ = Codec();
  }

 private:
  Options options_;
  int fd_ = -1;
  // Kept across reconnects: parsing the CA bundle costs hundreds of ms per connect.
  SSL_CTX* ctx_ = nullptr;
  std::string ctx_ca_;
  SSL* ssl_ = nullptr;
  bool open_ = false;
  bool eof_ = false;
  bool close_sent_ = false;
  bool ssl_want_write_ = false;
  std::string in_;
  std::string out_;
  size_t out_offset_ = 0;
  uint64_t written_total_ = 0;
  uint64_t queued_total_ = 0;
  std::deque<std::pair<uint64_t, std::function<void()>>> callbacks_;
  Codec codec_;

  bool queue(std::string frame, std::function<void()> on_written) {
    if (buffered() + frame.size() > options_.max_buffered) return false;
    out_ += frame;
    queued_total_ = written_total_ + buffered();
    if (on_written) callbacks_.emplace_back(queued_total_, std::move(on_written));
    return true;
  }

  static int wait(int fd, short events, std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return 0;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    pollfd p{fd, events, 0};
    const int r = ::poll(&p, 1, static_cast<int>(std::min<long long>(ms, 1000)));
    if (r < 0 && errno == EINTR) return -2;
    return r;
  }

  bool connect_tcp(const Url& url, std::chrono::steady_clock::time_point deadline, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    const std::string port = std::to_string(url.port);
    const int rc = getaddrinfo(url.host.c_str(), port.c_str(), &hints, &list);
    if (rc != 0 || !list) { error = std::string("dns: ") + gai_strerror(rc); return false; }
    std::string last = "connect: no address";
    for (addrinfo* ai = list; ai; ai = ai->ai_next) {
      const int fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, ai->ai_protocol);
      if (fd < 0) continue;
      int one = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
      if (::connect(fd, ai->ai_addr, ai->ai_addrlen) != 0 && errno != EINPROGRESS) {
        last = std::string("connect: ") + std::strerror(errno);
        ::close(fd);
        continue;
      }
      bool connected = false;
      for (;;) {
        const int r = wait(fd, POLLOUT, deadline);
        if (r == -2) continue;
        if (r == 0 && std::chrono::steady_clock::now() < deadline) continue;
        if (r <= 0) { last = "connect: timeout"; break; }
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (so_error) { last = std::string("connect: ") + std::strerror(so_error); break; }
        connected = true;
        break;
      }
      if (connected) { fd_ = fd; freeaddrinfo(list); return true; }
      ::close(fd);
    }
    freeaddrinfo(list);
    error = last;
    return false;
  }

  static std::string ssl_error_text() {
    const unsigned long code = ERR_get_error();
    ERR_clear_error();
    if (!code) return "unknown";
    char buf[256];
    ERR_error_string_n(code, buf, sizeof(buf));
    return buf;
  }

  bool start_tls(const Url& url, std::chrono::steady_clock::time_point deadline, std::string& error) {
    OPENSSL_init_ssl(OPENSSL_INIT_NO_LOAD_CONFIG | OPENSSL_INIT_LOAD_SSL_STRINGS |
                     OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);
    std::string ca = options_.ca_file;
    if (ca.empty()) {
      for (const char* candidate : kCaCandidates)
        if (readable(candidate)) { ca = candidate; break; }
    }
    if (ca.empty() || !readable(ca)) { error = "tls: no readable CA bundle (set TAKARO_CA_FILE)"; return false; }
    if (!ctx_ || ctx_ca_ != ca) {
      if (ctx_) { SSL_CTX_free(ctx_); ctx_ = nullptr; }
      SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
      if (!ctx) { error = "tls: context " + ssl_error_text(); return false; }
      SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
      SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
      if (SSL_CTX_load_verify_locations(ctx, ca.c_str(), nullptr) != 1) {
        SSL_CTX_free(ctx);
        error = "tls: CA bundle unusable " + ssl_error_text();
        return false;
      }
      ctx_ = ctx;
      ctx_ca_ = ca;
    }
    ssl_ = SSL_new(ctx_);
    if (!ssl_) { error = "tls: session " + ssl_error_text(); return false; }
    SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_set_options(ssl_, SSL_OP_IGNORE_UNEXPECTED_EOF);
    in6_addr v6{};
    in_addr v4{};
    const bool literal = inet_pton(AF_INET, url.host.c_str(), &v4) == 1 ||
                         inet_pton(AF_INET6, url.host.c_str(), &v6) == 1;
    if (!literal) SSL_set_tlsext_host_name(ssl_, url.host.c_str());
    if (literal) {
      X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl_), url.host.c_str());
    } else if (SSL_set1_host(ssl_, url.host.c_str()) != 1) {
      error = "tls: hostname check setup failed";
      return false;
    }
    SSL_set_fd(ssl_, fd_);
    for (;;) {
      const int r = SSL_connect(ssl_);
      if (r == 1) break;
      const int e = SSL_get_error(ssl_, r);
      short events = 0;
      if (e == SSL_ERROR_WANT_READ) events = POLLIN;
      else if (e == SSL_ERROR_WANT_WRITE) events = POLLOUT;
      else {
        const long verify = SSL_get_verify_result(ssl_);
        error = verify != X509_V_OK ? std::string("tls: certificate ") + X509_verify_cert_error_string(verify)
                                    : "tls: handshake " + ssl_error_text();
        return false;
      }
      const int w = wait(fd_, events, deadline);
      if (w == 0 && std::chrono::steady_clock::now() >= deadline) { error = "tls: handshake timeout"; return false; }
      if (w == -1) { error = "tls: poll failed"; return false; }
    }
    return true;
  }

  // Returns bytes written, 0 when it would block, -1 on error.
  ssize_t raw_write(const char* data, size_t length, std::string& error) {
    if (ssl_) {
      ssl_want_write_ = false;
      const int n = SSL_write(ssl_, data, static_cast<int>(std::min<size_t>(length, 1 << 20)));
      if (n > 0) return n;
      const int e = SSL_get_error(ssl_, n);
      if (e == SSL_ERROR_WANT_WRITE) { ssl_want_write_ = true; return 0; }
      if (e == SSL_ERROR_WANT_READ) return 0;
      error = "tls write: " + ssl_error_text();
      return -1;
    }
    const ssize_t n = ::send(fd_, data, length, MSG_NOSIGNAL);
    if (n >= 0) return n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    error = std::string("write: ") + std::strerror(errno);
    return -1;
  }

  ssize_t raw_read(char* data, size_t length, std::string& error) {
    if (ssl_) {
      const int n = SSL_read(ssl_, data, static_cast<int>(length));
      if (n > 0) return n;
      const int e = SSL_get_error(ssl_, n);
      if (e == SSL_ERROR_WANT_READ) return 0;
      if (e == SSL_ERROR_WANT_WRITE) { ssl_want_write_ = true; return 0; }
      if (e == SSL_ERROR_ZERO_RETURN) { eof_ = true; return 0; }
      if (e == SSL_ERROR_SYSCALL && ERR_peek_error() == 0) { eof_ = true; return 0; }
      error = "tls read: " + ssl_error_text();
      return -1;
    }
    const ssize_t n = ::recv(fd_, data, length, 0);
    if (n > 0) return n;
    if (n == 0) { eof_ = true; return 0; }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    error = std::string("read: ") + std::strerror(errno);
    return -1;
  }

  bool upgrade(const Url& url, std::chrono::steady_clock::time_point deadline, std::string& error) {
    unsigned char nonce[16];
    if (RAND_bytes(nonce, sizeof(nonce)) != 1) { error = "handshake: no randomness"; return false; }
    const std::string key = base64(nonce, sizeof(nonce));
    const bool default_port = (url.tls && url.port == 443) || (!url.tls && url.port == 80);
    const bool v6 = url.host.find(':') != std::string::npos;
    std::string host = v6 ? "[" + url.host + "]" : url.host;
    if (!default_port) host += ":" + std::to_string(url.port);
    const std::string request = "GET " + url.path + " HTTP/1.1\r\nHost: " + host +
        "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
        "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: takaro-ark-native\r\n\r\n";
    size_t sent = 0;
    while (sent < request.size()) {
      const ssize_t n = raw_write(request.data() + sent, request.size() - sent, error);
      if (n < 0) return false;
      if (n == 0) {
        const int w = wait(fd_, ssl_want_write_ || !ssl_ ? POLLOUT : POLLIN, deadline);
        if (w == 0 && std::chrono::steady_clock::now() >= deadline) { error = "handshake: write timeout"; return false; }
        continue;
      }
      sent += static_cast<size_t>(n);
    }
    std::string response;
    char buf[2048];
    size_t end = std::string::npos;
    while ((end = response.find("\r\n\r\n")) == std::string::npos) {
      if (response.size() > 16384) { error = "handshake: response too large"; return false; }
      const ssize_t n = raw_read(buf, sizeof(buf), error);
      if (n < 0) return false;
      if (n == 0) {
        if (eof_) { error = "handshake: connection closed"; return false; }
        const int w = wait(fd_, ssl_want_write_ ? POLLOUT : POLLIN, deadline);
        if (w == 0 && std::chrono::steady_clock::now() >= deadline) { error = "handshake: read timeout"; return false; }
        continue;
      }
      response.append(buf, static_cast<size_t>(n));
    }
    const std::string head = response.substr(0, end + 2);
    in_ = response.substr(end + 4);  // frames sent right after the upgrade
    const size_t line_end = head.find("\r\n");
    const std::string status = head.substr(0, line_end);
    if (status.rfind("HTTP/1.1 101", 0) != 0) {
      error = "handshake: unexpected status '" + status.substr(0, 64) + "'";
      return false;
    }
    std::string lower = head;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string field = "\r\nsec-websocket-accept:";
    const size_t at = lower.find(field);
    if (at == std::string::npos) { error = "handshake: missing accept"; return false; }
    size_t value_start = at + field.size();
    while (value_start < head.size() && head[value_start] == ' ') ++value_start;
    const size_t value_end = head.find("\r\n", value_start);
    std::string value = head.substr(value_start, value_end - value_start);
    while (!value.empty() && value.back() == ' ') value.pop_back();
    if (value != accept_key(key)) { error = "handshake: accept mismatch"; return false; }
    if (lower.find("\r\nsec-websocket-extensions:") != std::string::npos) {
      error = "handshake: unrequested extension";
      return false;
    }
    return true;
  }
};

}  // namespace ws
