#ifndef XANADU_EVIDENCE_CASE_HPP
#define XANADU_EVIDENCE_CASE_HPP

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/xanadu/evidence/capture.hpp"
#include "common/xanadu/store.hpp"

namespace xanadu::evidence {

using RecordId = zigzag::CellRef;

struct Record {
  RecordId id;
  std::string kind;
  std::string label;
  std::string text;
  std::optional<RecordId> parent;
  std::map<std::string, std::string> attributes;
};

struct RecordIdPage {
  std::vector<RecordId> ids;
  bool complete{};
};

struct SpanSelection {
  std::uint64_t offset;
  std::uint64_t length;
};

namespace detail {
[[nodiscard]] bool extendsTablesForSave(const StoreTables &oldTables,
                                        const StoreTables &newTables);
} // namespace detail

enum class SavePhaseForTesting {
  snapshotStaged,
  tablesCommitted,
  operationsCommitted
};

class EvidenceCase {
public:
  using SavePhaseHookForTesting = std::function<void(SavePhaseForTesting)>;

  explicit EvidenceCase(std::shared_ptr<UserPermascroll> permascroll);
  EvidenceCase(std::shared_ptr<UserPermascroll> permascroll,
               SavePhaseHookForTesting savePhaseHookForTesting);
  ~EvidenceCase();
  EvidenceCase(const EvidenceCase &)            = delete;
  EvidenceCase &operator=(const EvidenceCase &) = delete;

  void create(const std::filesystem::path &path, std::string_view title);
  void open(const std::filesystem::path &path);
  void close();
  void save();
  void durableSave();
  [[nodiscard]] bool isOpen() const;
  [[nodiscard]] std::string title() const;
  [[nodiscard]] std::filesystem::path path() const;
  [[nodiscard]] std::vector<Record> records() const;
  [[nodiscard]] RecordIdPage recordIds(std::size_t maximum) const;
  [[nodiscard]] Record inspect(RecordId id) const;
  [[nodiscard]] std::string rawText(RecordId id) const;
  [[nodiscard]] std::string rawText(RecordId id,
                                    std::size_t maximumBytes) const;
  [[nodiscard]] Source source(RecordId id) const;
  [[nodiscard]] RecordId addSource(const Source &source);
  [[nodiscard]] RecordId
  appendRecord(std::string_view kind, std::string_view label,
               std::string_view text, std::optional<RecordId> parent = {},
               std::map<std::string, std::string> attributes            = {},
               std::vector<std::pair<std::string, RecordId>> references = {});
  [[nodiscard]] RecordId retain(RecordId source, const CaptureResult &result);
  [[nodiscard]] RecordId annotate(RecordId target, std::string_view text,
                                  std::optional<SpanSelection> span = {});
  [[nodiscard]] std::vector<std::string> compare(RecordId before,
                                                 RecordId after) const;
  void exportSelection(std::span<const RecordId> selected,
                       const std::filesystem::path &destination) const;
  [[nodiscard]] const Store &store() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace xanadu::evidence
#endif
