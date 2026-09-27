#ifndef COMMON_XANADU_STORE_ACTIVITY_LOG_HPP
#define COMMON_XANADU_STORE_ACTIVITY_LOG_HPP

#include <filesystem>

#include "common/xanadu/link_navigation.hpp"

namespace xanadu {

class Store;

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
  void appendRecord(std::string_view dimension, std::string_view text);

  Store *store;
  std::filesystem::path directory;
  std::vector<Visit> visits;
  std::optional<VisitId> selected;
};

} // namespace xanadu

#endif
