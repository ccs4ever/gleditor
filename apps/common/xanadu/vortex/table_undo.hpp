/**
 * @file table_undo.hpp
 * @brief Per-key undo logs for the VM's tables, so a failing opcode restores
 *        what it changed instead of swapping back whole copied tables.
 */
#ifndef COMMON_XANADU_VORTEX_TABLE_UNDO_HPP
#define COMMON_XANADU_VORTEX_TABLE_UNDO_HPP

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag::vortex {

/// What a test arms the frame failpoint to throw.
class FrameFailpointError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// Counts the table entries the frame opcodes copy or journal, and carries the
/// frame failpoint. The failpoint is a plain counter: every statement of an
/// InvokeFrame or ReturnFrame body that the undo log must be able to reverse
/// calls hit() after it ran, and when a test has armed statement N, the Nth hit
/// throws FrameFailpointError. Unarmed, a hit is one increment and one test of
/// an empty optional, so the production path pays no measurable cost, and the
/// rollback paths, which no ordinary failure reaches, can be run after every
/// statement.
class FrameProbe {
public:
  std::size_t entries{};

  void hit() {
    ++statements_;
    if (armedAt_ && statements_ == *armedAt_) [[unlikely]]
      fire();
  }
  /// Throws at the nth statement from now (counting from 1); nullopt disarms.
  FrameProbe *arm(const std::optional<std::size_t> nth) noexcept {
    statements_ = 0;
    armedAt_    = nth;
    return this;
  }
  [[nodiscard]] std::size_t statements() const noexcept { return statements_; }

private:
  [[noreturn]] void fire() {
    armedAt_.reset();
    throw FrameFailpointError("frame failpoint at statement " +
                              std::to_string(statements_));
  }
  std::size_t statements_{};
  std::optional<std::size_t> armedAt_;
};

/// Undo log over one VM table. InvokeFrame and ReturnFrame must leave every
/// table as it was if they fail part-way, but they change only the entries of
/// the frame's own cells, its argument and result slots and its cursor. They
/// used to copy each whole table to be able to swap it back, which made every
/// call cost the size of everything earlier runs had left behind. Each change
/// now goes through here, which records the prior state of that one key first;
/// rollback replays the records newest first, so a key touched twice ends at
/// its oldest recorded state. Rollback restores contents, not bucket order.
///
/// rollback() is noexcept yet re-inserts entries, so an allocation failure
/// during it ends in std::terminate. That is an accepted trade: the whole-table
/// swap it replaced could not fail, but it paid for that with a copy of every
/// table on every call, and an allocation that fails while undoing a failure
/// leaves the VM unrecoverable either way.
template <class Map> class MapUndo {
public:
  MapUndo(Map &table, FrameProbe &probe) : table_(table), probe_(probe) {}
  MapUndo(const MapUndo &)            = delete;
  MapUndo &operator=(const MapUndo &) = delete;

  void set(const CellRef key, typename Map::mapped_type value) {
    remember(key);
    table_.insert_or_assign(key, std::move(value));
    probe_.hit();
  }
  void erase(const CellRef key) {
    if (!table_.contains(key)) return;
    remember(key);
    table_.erase(key);
    probe_.hit();
  }
  /// Records the key's current state for a change another function makes.
  void touch(const CellRef key) {
    remember(key);
    probe_.hit();
  }
  void rollback() noexcept {
    for (auto entry = records_.rbegin(); entry != records_.rend(); ++entry) {
      if (entry->prior)
        table_.insert_or_assign(entry->key, std::move(*entry->prior));
      else
        table_.erase(entry->key);
    }
    records_.clear();
  }

private:
  struct Record {
    CellRef key;
    std::optional<typename Map::mapped_type> prior;
  };
  void remember(const CellRef key) {
    const auto found = table_.find(key);
    records_.push_back({.key   = key,
                        .prior = found == table_.end()
                                     ? std::nullopt
                                     : std::optional<typename Map::mapped_type>{
                                           found->second}});
    ++probe_.entries;
  }
  Map &table_;
  FrameProbe &probe_;
  std::vector<Record> records_;
};

/// MapUndo's counterpart for the VM's slot sets.
class SetUndo {
public:
  SetUndo(std::unordered_set<CellRef> &table, FrameProbe &probe)
      : table_(table), probe_(probe) {}
  SetUndo(const SetUndo &)            = delete;
  SetUndo &operator=(const SetUndo &) = delete;

  void insert(const CellRef key) {
    if (table_.contains(key)) return;
    records_.push_back({key, false});
    ++probe_.entries;
    table_.insert(key);
    probe_.hit();
  }
  void erase(const CellRef key) {
    if (!table_.contains(key)) return;
    records_.push_back({key, true});
    ++probe_.entries;
    table_.erase(key);
    probe_.hit();
  }
  void rollback() noexcept {
    for (auto entry = records_.rbegin(); entry != records_.rend(); ++entry) {
      if (entry->wasMember)
        table_.insert(entry->key);
      else
        table_.erase(entry->key);
    }
    records_.clear();
  }

private:
  struct Record {
    CellRef key;
    bool wasMember;
  };
  std::unordered_set<CellRef> &table_;
  FrameProbe &probe_;
  std::vector<Record> records_;
};

} // namespace zigzag::vortex

#endif // COMMON_XANADU_VORTEX_TABLE_UNDO_HPP
