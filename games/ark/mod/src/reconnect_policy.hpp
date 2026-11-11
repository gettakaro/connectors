#pragma once
// When to reconnect to Takaro (gettakaro/connectors#858):
//  - a connection that was identified and stayed up for `stable_after` drops:
//    the first retry comes after `fast` plus up to `fast_jitter` (about 0.5-1.5 s);
//  - every further failure in a row (connect failed, identify refused, or a
//    connection that dropped before it was stable) backs off exponentially from
//    `base`, with equal jitter, never above `max`;
//  - the failure streak resets only once a connection has been stable.
#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <random>

namespace takaro {

class ReconnectPolicy {
 public:
  using clock = std::chrono::steady_clock;
  using ms = std::chrono::milliseconds;

  struct Settings {
    ms fast{500};
    ms fast_jitter{1000};
    ms base{2000};
    ms max{60000};
    ms stable_after{10000};
  };

  // `unit` returns a value in [0, 1); tests pass a fixed one.
  explicit ReconnectPolicy(Settings settings, std::function<double()> unit = {})
      : settings_(settings), unit_(std::move(unit)) {
    if (!unit_) {
      auto engine = std::make_shared<std::mt19937_64>(std::random_device{}());
      unit_ = [engine] { return std::uniform_real_distribution<double>(0.0, 1.0)(*engine); };
    }
  }

  // Identify was accepted on the current connection.
  void identified(clock::time_point now) { identified_at_ = now; has_identified_ = true; }

  // The connection dropped (or never got past identify). Returns the delay before the next attempt.
  ms dropped(clock::time_point now) {
    const bool stable = has_identified_ && now - identified_at_ >= settings_.stable_after;
    has_identified_ = false;
    if (stable) {
      streak_ = 0;
      return settings_.fast + scaled(settings_.fast_jitter, unit());
    }
    return failed();
  }

  // A connect attempt failed before any connection existed.
  ms failed() {
    has_identified_ = false;
    ++streak_;
    return backoff(streak_);
  }

  unsigned streak() const { return streak_; }
  const Settings& settings() const { return settings_; }

  // The n-th failure in a row (n >= 1): half of min(max, base * 2^(n-1)) plus jitter up to the other half.
  ms backoff(unsigned n) {
    const unsigned shift = std::min(n == 0 ? 0u : n - 1, 20u);
    const auto ceiling = std::min<ms>(settings_.base * (1LL << shift), settings_.max);
    const auto half = ceiling / 2;
    return half + scaled(ceiling - half, unit());
  }

 private:
  Settings settings_;
  std::function<double()> unit_;
  unsigned streak_ = 0;
  bool has_identified_ = false;
  clock::time_point identified_at_{};

  double unit() { return std::clamp(unit_(), 0.0, 1.0); }
  static ms scaled(ms span, double u) { return ms(static_cast<long long>(static_cast<double>(span.count()) * u)); }
};

}  // namespace takaro
