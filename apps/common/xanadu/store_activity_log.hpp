#ifndef COMMON_XANADU_STORE_ACTIVITY_LOG_HPP
#define COMMON_XANADU_STORE_ACTIVITY_LOG_HPP

#include <filesystem>
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

} // namespace xanadu

#endif
