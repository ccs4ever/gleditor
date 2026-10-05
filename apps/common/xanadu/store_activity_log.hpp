#ifndef COMMON_XANADU_STORE_ACTIVITY_LOG_HPP
#define COMMON_XANADU_STORE_ACTIVITY_LOG_HPP

#include <filesystem>
#include <map>
#include <set>
#include <span>

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

  [[nodiscard]] std::span<const Visit> allVisits() const { return visits; }
  [[nodiscard]] std::string annotation(VisitId id) const;
  [[nodiscard]] bool referenced(VisitId id) const;
  void annotate(VisitId id, std::string text);
  void reference(VisitId id);

private:
  void appendRecord(std::string_view dimension, std::string_view text);

  Store *store;
  std::filesystem::path directory;
  std::vector<Visit> visits;
  std::optional<VisitId> selected;
  std::map<std::uint64_t, std::string> notes;
  std::set<std::uint64_t> references;
};

} // namespace xanadu

#endif
