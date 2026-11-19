/**
 * @file table_undo.hpp
 * @brief Per-key undo logs for the VM's tables, so a failing opcode restores
 *        what it changed instead of swapping back whole copied tables.
 */
#ifndef COMMON_XANADU_VORTEX_TABLE_UNDO_HPP
#define COMMON_XANADU_VORTEX_TABLE_UNDO_HPP

#include <cstddef>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag::vortex {

/// Undo log over one VM table. InvokeFrame and ReturnFrame must leave every
/// table as it was if they fail part-way, but they change only the entries of
/// the frame's own cells, its argument and result slots and its cursor. They
/// used to copy each whole table to be able to swap it back, which made every
/// call cost the size of everything earlier runs had left behind. Each change
/// now goes through here, which records the prior state of that one key first;
/// rollback replays the records newest first, so a key touched twice ends at
/// its oldest recorded state. Rollback restores contents, not bucket order.
template <class Map> class MapUndo {
public:
  MapUndo(Map &table, std::size_t &entriesCounted)
      : table_(table), counted_(entriesCounted) {}
  MapUndo(const MapUndo &)            = delete;
  MapUndo &operator=(const MapUndo &) = delete;

  void set(const CellRef key, typename Map::mapped_type value) {
    remember(key);
    table_.insert_or_assign(key, std::move(value));
  }
  void erase(const CellRef key) {
    if (!table_.contains(key)) return;
    remember(key);
    table_.erase(key);
  }
  /// Records the key's current state for a change another function makes.
  void touch(const CellRef key) { remember(key); }
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
    ++counted_;
  }
  Map &table_;
  std::size_t &counted_;
  std::vector<Record> records_;
};

/// MapUndo's counterpart for the VM's slot sets.
class SetUndo {
public:
  SetUndo(std::unordered_set<CellRef> &table, std::size_t &entriesCounted)
      : table_(table), counted_(entriesCounted) {}
  SetUndo(const SetUndo &)            = delete;
  SetUndo &operator=(const SetUndo &) = delete;

  void insert(const CellRef key) {
    if (table_.contains(key)) return;
    records_.push_back({key, false});
    ++counted_;
    table_.insert(key);
  }
  void erase(const CellRef key) {
    if (!table_.contains(key)) return;
    records_.push_back({key, true});
    ++counted_;
    table_.erase(key);
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
  std::size_t &counted_;
  std::vector<Record> records_;
};

} // namespace zigzag::vortex

#endif // COMMON_XANADU_VORTEX_TABLE_UNDO_HPP
