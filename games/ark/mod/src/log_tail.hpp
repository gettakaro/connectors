#pragma once

// Bounded tail of ARK's own Saved/Logs/ShooterGame.log. Prime at EOF after
// connector startup so existing logs are never replayed as fresh events.
// Only fixed, safe game-engine messages are forwarded; arbitrary command
// output, command-line args, credentials and network addresses stay out.
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace ark_log_tail {

constexpr size_t max_scan_bytes = 65536;
constexpr size_t max_line_bytes = 512;
constexpr size_t max_events = 32;

struct Result {
  std::vector<std::string> messages;
  bool more = false;
  bool dropped = false;
};

inline bool decimal(std::string_view value, bool dot = false) {
  if (value.empty() || value.size() > 32) return false;
  unsigned dots = 0;
  for (char c : value) {
    if (c == '.') { if (!dot || ++dots > 3) return false; }
    else if (c < '0' || c > '9') return false;
  }
  return value.front() != '.' && value.back() != '.';
}

inline bool unsafe_token(std::string_view text) {
  std::string lower;
  lower.reserve(text.size());
  for (unsigned char c : text) lower.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
  for (std::string_view marker : {"commandline", "password", "token", "authorization",
                                  "auth=", "steam_", "ip address", "rcon"}) {
    if (lower.find(marker) != std::string::npos) return true;
  }
  return false;
}

inline std::string safe_message(std::string_view line) {
  if (line.size() > max_line_bytes || unsafe_token(line)) return {};
  if (line.starts_with("\xef\xbb\xbf")) line.remove_prefix(3);
  // Native ARK log prefix: [YYYY.MM.DD-HH.MM.SS:mmm][frame]message.
  if (line.size() < 31 || line[0] != '[' || line[24] != ']' || line[25] != '[') return {};
  const size_t close = line.find(']', 26);
  if (close == std::string_view::npos || close > 35) return {};
  const std::string_view text = line.substr(close + 1);
  if (text == "Saving world..." || text == "New Save Format enabled") return std::string(text);
  constexpr std::string_view save = "World Save Complete. Took ";
  if (text.starts_with(save) && decimal(text.substr(save.size()), true)) return std::string(text);
  constexpr std::string_view version = "ARK Version: ";
  if (text.starts_with(version) && decimal(text.substr(version.size()), true)) return std::string(text);
  constexpr std::string_view load = "Primal Game Data Took ";
  if (text.starts_with(load) && text.ends_with(" seconds") &&
      decimal(text.substr(load.size(), text.size() - load.size() - 8), true)) return std::string(text);
  return {};
}

class Tail {
 public:
  explicit Tail(std::string path) : path_(std::move(path)) {}

  // Return false if the file is absent or not a regular non-symlink file.
  // A later poll may still start at byte zero if the file is newly created.
  bool prime() {
    int fd = open_safe();
    if (fd < 0) return false;
    struct stat st{};
    const bool okay = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0;
    close(fd);
    if (!okay) return false;
    device_ = st.st_dev;
    inode_ = st.st_ino;
    offset_ = st.st_size;
    known_ = true;
    partial_.clear();
    dropping_ = false;
    return true;
  }

  Result poll() {
    Result result;
    int fd = open_safe();
    if (fd < 0) return result;
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
      close(fd); return result;
    }
    if (!known_ || st.st_dev != device_ || st.st_ino != inode_ || st.st_size < offset_) {
      // Rotation/truncation: only this new file's lines can be fresh.
      device_ = st.st_dev;
      inode_ = st.st_ino;
      offset_ = 0;
      known_ = true;
      partial_.clear();
      dropping_ = false;
    }
    if (st.st_size == offset_) { close(fd); return result; }
    std::array<char, max_scan_bytes> buffer{};
    const size_t wanted = static_cast<size_t>(std::min<off_t>(st.st_size - offset_, max_scan_bytes));
    const ssize_t got = pread(fd, buffer.data(), wanted, offset_);
    close(fd);
    if (got <= 0) return result;
    offset_ += got;
    result.more = offset_ < st.st_size;
    for (ssize_t i = 0; i < got; ++i) {
      const char c = buffer[static_cast<size_t>(i)];
      if (c == '\n') {
        if (!dropping_) {
          if (!partial_.empty() && partial_.back() == '\r') partial_.pop_back();
          const std::string message = safe_message(partial_);
          if (!message.empty()) {
            if (result.messages.size() < max_events) result.messages.push_back(message);
            else result.dropped = true;
          }
        }
        partial_.clear();
        dropping_ = false;
      } else if (!dropping_) {
        if (partial_.size() == max_line_bytes) {
          partial_.clear(); dropping_ = true; result.dropped = true;
        } else partial_.push_back(c);
      }
    }
    return result;
  }

 private:
  int open_safe() const { return open(path_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK); }
  std::string path_;
  dev_t device_ = 0;
  ino_t inode_ = 0;
  off_t offset_ = 0;
  bool known_ = false;
  bool dropping_ = false;
  std::string partial_;
};

} // namespace ark_log_tail
