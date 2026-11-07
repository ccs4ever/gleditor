/**
 * @file agent_executor_liveness.hpp
 * @brief The host's liveness memory (P19 I3-3c1, `i3-3-design.md` 5.2 and
 *        5.3): the indexed deadline heap and the per-source mirror.
 *
 * Host memory only. The arena holds what a checkpoint must carry (`silent`,
 * `stale` and the stamps); this holds when the next deadline falls, which a
 * resume re-derives from the rebased stamps (5.4).
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_AGENT_EXECUTOR_LIVENESS_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_AGENT_EXECUTOR_LIVENESS_HPP

#include <algorithm>
#include <chrono>
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace xanadu::vagent::runtime {

using LivenessTime = std::chrono::steady_clock::time_point;

/// One thing the liveness turn is to do for the arena. A `Silent` or `Stale`
/// entry carries the stamp its deadline was armed from, so the turn can see a
/// commit that raced the timer and leave the flag alone.
struct LivenessDue {
  enum class Kind : std::uint8_t { Silent, Stale, Clear };
  Kind kind{Kind::Silent};
  std::string principal;
  /// The stream, for `Stale`.
  std::optional<std::string> predicate;
  /// The arena stamp (`last_receive` or the stream's `received`) the deadline
  /// was armed from; absent when it was armed from none.
  std::optional<double> stamp;
  /// `heartbeat` or `clock`, for `Silent`.
  std::string cause{"heartbeat"};
};

/// One entry per (agent, source) for its heartbeat deadline and one per
/// (agent, source, predicate) for its data deadline.
struct LivenessKey {
  std::string pid;
  std::string principal;
  std::optional<std::string> predicate;
  auto operator<=>(const LivenessKey &) const = default;
};

struct LivenessEntry {
  LivenessTime deadline{};
  /// Raised by every arm, so a re-arm of the same key is visible as one.
  std::uint64_t generation{0};
  std::optional<double> stamp;
};

/// An indexed priority queue: the entry map is the index, and the ordered set
/// is the queue. A re-arm moves the key's one entry in place and never adds a
/// second (S-13a), so the size is at most sources x (1 + streams) however many
/// messages arrive.
class LivenessHeap {
public:
  /// Arm @p key at @p deadline. With @p keepLater an existing, later deadline
  /// stays: a message that arrives out of order must not shorten the wait.
  const LivenessEntry &arm(const LivenessKey &key, LivenessTime deadline,
                           std::optional<double> stamp, bool keepLater);
  [[nodiscard]] const LivenessEntry *find(const LivenessKey &key) const;
  void remove(const LivenessKey &key);
  void removeSource(const std::string &pid, const std::string &principal);
  void removePid(const std::string &pid);
  [[nodiscard]] std::optional<LivenessTime> top() const;
  /// Remove and return every entry whose deadline is at or before @p now.
  [[nodiscard]] std::vector<std::pair<LivenessKey, LivenessEntry>>
  popDue(LivenessTime now);
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
  std::map<LivenessKey, LivenessEntry> entries_;
  std::set<std::pair<LivenessTime, LivenessKey>> order_;
  std::uint64_t generation_{0};
};

/// What the host remembers of one source between messages.
struct LivenessSource {
  /// The newest fresh message, of any kind.
  std::optional<LivenessTime> lastFresh;
  /// The arena's `last_receive`, as this host last knew it: what a deadline is
  /// armed from, and what the liveness turn compares the arena against.
  std::optional<double> arenaStamp;
  /// Whether the arena holds `silent` for the profile, or a liveness item
  /// that sets it is queued. A fresh heartbeat writes the arena only while
  /// this is true.
  bool silent{false};
  /// When the adapter dropped the source's paired mapping (design 4.5).
  std::optional<LivenessTime> clockPending;
  /// The arena's latch as `livenessBoundLocked` read it. Kept apart from the
  /// executor's latch cache, which is learned from a refusal and nothing else.
  bool latched{false};
};

inline const LivenessEntry &LivenessHeap::arm(const LivenessKey &key,
                                              LivenessTime deadline,
                                              std::optional<double> stamp,
                                              bool keepLater) {
  auto [it, fresh] = entries_.try_emplace(key);
  auto &entry      = it->second;
  if (!fresh) {
    order_.erase({entry.deadline, key});
    if (keepLater) deadline = std::max(deadline, entry.deadline);
  }
  entry.deadline   = deadline;
  entry.generation = ++generation_;
  entry.stamp      = stamp;
  order_.emplace(deadline, key);
  return entry;
}

inline const LivenessEntry *LivenessHeap::find(const LivenessKey &key) const {
  const auto it = entries_.find(key);
  return it == entries_.end() ? nullptr : &it->second;
}

inline void LivenessHeap::remove(const LivenessKey &key) {
  const auto it = entries_.find(key);
  if (it == entries_.end()) return;
  order_.erase({it->second.deadline, key});
  entries_.erase(it);
}

inline void LivenessHeap::removeSource(const std::string &pid,
                                       const std::string &principal) {
  // `nullopt` orders before any predicate, so the source's entries are one
  // contiguous run starting at its heartbeat entry.
  for (auto it = entries_.lower_bound({pid, principal, std::nullopt});
       it != entries_.end() && it->first.pid == pid &&
       it->first.principal == principal;) {
    order_.erase({it->second.deadline, it->first});
    it = entries_.erase(it);
  }
}

inline void LivenessHeap::removePid(const std::string &pid) {
  for (auto it = entries_.lower_bound({pid, std::string{}, std::nullopt});
       it != entries_.end() && it->first.pid == pid;) {
    order_.erase({it->second.deadline, it->first});
    it = entries_.erase(it);
  }
}

inline std::optional<LivenessTime> LivenessHeap::top() const {
  if (order_.empty()) return std::nullopt;
  return order_.begin()->first;
}

inline std::vector<std::pair<LivenessKey, LivenessEntry>>
LivenessHeap::popDue(LivenessTime now) {
  std::vector<std::pair<LivenessKey, LivenessEntry>> due;
  while (!order_.empty() && order_.begin()->first <= now) {
    const auto key = order_.begin()->second;
    const auto it  = entries_.find(key);
    due.emplace_back(key, it->second);
    order_.erase(order_.begin());
    entries_.erase(it);
  }
  return due;
}

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_AGENT_EXECUTOR_LIVENESS_HPP
