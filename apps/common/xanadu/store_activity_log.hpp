#ifndef COMMON_XANADU_STORE_ACTIVITY_LOG_HPP
#define COMMON_XANADU_STORE_ACTIVITY_LOG_HPP

#include <filesystem>
#include <stdexcept>

#include "common/xanadu/link_navigation.hpp"

namespace xanadu {

class Store;

std::string encodeLinkContext(const LinkVisitContext &context);
LinkVisitContext decodeLinkContext(std::string_view text);

enum class ActivityError { UnknownVisit, EmptyAnnotation };

class ActivityRecordUnreadable : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct WalkEntry {
  Visit visit;
  bool referenced{};
  std::vector<std::string> annotations;
};

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

  [[nodiscard]] std::vector<WalkEntry> walkEntries() const;
  std::expected<StoreActivityLog *, ActivityError> reference(VisitId id);
  std::expected<StoreActivityLog *, ActivityError>
  annotate(VisitId id, std::string_view text);

private:
  void appendRecord(std::string_view dimension, std::string_view text);

  Store *store;
  std::filesystem::path directory;
  std::vector<Visit> visits;
  std::optional<VisitId> selected;
  std::vector<VisitId> references;
  struct Annotation {
    VisitId visit;
    std::string text;
  };
  std::vector<Annotation> annotations;
};

} // namespace xanadu

#endif
