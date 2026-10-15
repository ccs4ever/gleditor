#ifndef COMMON_XANADU_STORE_ACTIVITY_LOG_HPP
#define COMMON_XANADU_STORE_ACTIVITY_LOG_HPP

#include <array>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/link_navigation.hpp"

namespace xanadu {

class Store;

/// Append @p text as one record cell at the end of @p dimension's rank from
/// the activity store's home cell, and save the store to @p directory. Each
/// kind of activity record keeps a rank of its own, so a reader of one kind
/// never reads another's.
void appendActivityRecord(Store &store, const std::filesystem::path &directory,
                          std::string_view dimension, std::string_view text);

/// A document id as an activity record spells it: 32 lower-case hex digits.
[[nodiscard]] std::string activityIdText(const DocumentId &id);
/// Throws std::runtime_error on anything activityIdText() did not write.
[[nodiscard]] DocumentId parseActivityId(std::string_view hex);

/// Append-only branching walks in the reader's system://activity store.
class StoreActivityLog final : public ActivityLog {
public:
  StoreActivityLog(Store *store, std::filesystem::path directory);

  VisitId append(Visit visit) override;
  [[nodiscard]] gleditor::cpp26::optional<const Visit &>
  find(VisitId id) const override;
  [[nodiscard]] std::vector<VisitId> children(VisitId parent) const override;
  [[nodiscard]] std::optional<VisitId> current() const override {
    return selected;
  }
  void select(VisitId id) override;

private:
  Store *store;
  std::filesystem::path directory;
  std::vector<Visit> visits;
  std::optional<VisitId> selected;
};

/**
 * @brief Where the reader last rested in each slice, kept in the activity
 *        store (system://activity).
 *
 * A focus that changes with every step is reader activity, not a setting, so
 * it does not go in system://layout (store-slice-convergence.md R8;
 * view-system.md §7.5). One record, on `d.activity-focus`, per settled walk,
 * never per step: the caller keeps only when the reader has come to rest, and
 * keep() skips a cell that is already the slice's latest. A later record for a
 * slice supersedes the earlier ones, so the newest is where the slice reopens.
 *
 * With no store the log keeps in memory only, as the other activity logs do.
 * The constructor throws std::runtime_error on a record it cannot read.
 */
class SliceFocusLog {
public:
  SliceFocusLog(Store *activity, std::filesystem::path directory);

  /// Where @p slice was last rested on, or nothing for a slice never moved in.
  [[nodiscard]] std::optional<zigzag::CellRef>
  last(const DocumentId &slice) const;

  /// Record @p cell as @p slice's resting place; nothing is written when it
  /// already is. A refusal (the store could not be saved) leaves the log as
  /// it was.
  std::expected<SliceFocusLog *, std::string> keep(const DocumentId &slice,
                                                   zigzag::CellRef cell);

  /// Records filed so far, for a test or a diagnostic.
  [[nodiscard]] std::size_t records() const noexcept { return filed; }

private:
  Store *store;
  std::filesystem::path directory;
  std::map<std::array<std::uint8_t, 16>, zigzag::CellRef> latest;
  std::size_t filed{};
};

} // namespace xanadu

#endif
