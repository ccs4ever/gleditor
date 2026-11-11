#ifndef XANADU_EVIDENCE_CAPTURE_HPP
#define XANADU_EVIDENCE_CAPTURE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace xanadu::evidence {

enum class SourceKind { SQLite, Rest };
enum class CaptureState { Complete, Partial, Failed, Cancelled };
enum class StorageClass { Null, Integer, Real, Text, Blob };

struct Binary {
  std::string bytes;
  bool operator==(const Binary &) const = default;
};
using Value =
    std::variant<std::monostate, std::int64_t, double, std::string, Binary>;

struct CaptureLimits {
  // Retained capture budgets, rather than promises of unbounded source
  // coverage.
  std::size_t maxRows{1000};
  std::size_t maxColumns{64};
  std::size_t maxPages{16};
  std::size_t maxBytes{1024 * 1024};
  long timeoutMs{5000};
};

struct Source {
  std::string name;
  SourceKind kind{SourceKind::SQLite};
  std::string locator;
  std::string query;
  // An explicit continuity assertion, never an implicit SQLite rowid policy.
  std::string keyColumn;
  std::vector<Value> parameters;
  CaptureLimits limits;
};

struct Column {
  std::string name;
  std::string declaredType;
};

struct CapturedValue {
  StorageClass storage{StorageClass::Null};
  Value value;
};

struct CapturedRow {
  std::vector<CapturedValue> values;
};

struct Page {
  std::string locator;
  long status{};
  std::string body;
  std::string contentType;
  std::string etag;
  std::string lastModified;
  std::string next;
  CaptureState state{CaptureState::Complete};
  std::string error;
  bool redacted{};
};

struct CaptureResult {
  Source source;
  CaptureState state{CaptureState::Failed};
  std::string startedAt;
  std::string finishedAt;
  std::string coverage;
  std::string error;
  std::vector<Column> columns;
  std::vector<CapturedRow> rows;
  std::vector<Page> pages;
  // A witnessed source instance, not a universal guarantee against key reuse.
  std::optional<std::string> incarnation;
};

// Pure validation before a connection is retained in append-only storage.
// Errors name the contract, never echo possible credential bytes.
[[nodiscard]] std::optional<std::string> sourceProblem(const Source &source);

// Only owned source data crosses this boundary; workers never see a Store,
// arena, local CellRef, UI object or borrowed primedia span.
class CaptureJob {
public:
  CaptureJob();
  ~CaptureJob();
  CaptureJob(const CaptureJob &)            = delete;
  CaptureJob &operator=(const CaptureJob &) = delete;
  bool start(Source source);
  void cancel();
  [[nodiscard]] bool busy() const;
  [[nodiscard]] std::optional<CaptureResult> poll();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string stateName(CaptureState state);
[[nodiscard]] std::string storageName(StorageClass storage);
[[nodiscard]] std::string displayValue(const CapturedValue &value);

} // namespace xanadu::evidence
#endif
