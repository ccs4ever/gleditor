#ifndef COMMON_XANADU_EVIDENCE_WINDOW_COMPOSITION_HPP
#define COMMON_XANADU_EVIDENCE_WINDOW_COMPOSITION_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "common/xanadu/evidence/evidence_case.hpp"
#include "common/xanadu/vql/windows.hpp"

namespace xanadu::evidence {

struct SqliteObservationWindowRequest {
  RecordId observation{};
  // Event-time uses timeColumn. Observation-time pins the observation's
  // retained started-at value as the time witness for each row.
  vql::WindowTimeBasis timeBasis{vql::WindowTimeBasis::EventTime};
  std::string timeColumn;
  std::optional<std::string> arrivalWatermarkColumn;
  std::optional<std::string> keyColumn;
  std::int64_t startInclusive{};
  std::int64_t endExclusive{};
  std::int64_t watermark{};
  vql::WindowLatePolicy latePolicy{vql::WindowLatePolicy::Exclude};
  std::size_t maxInputOccurrences{10000};
  std::size_t maxSourceGaps{1000};
  std::size_t maxSourceGapBytes{1024 * 1024};
  std::size_t maxKeyBytes{1024 * 1024};
  std::size_t maxOutputOccurrences{1000};
  std::size_t maxWork{1000000};
  std::size_t maxScanRecords{100000};
  std::size_t maxScanBytes{8 * 1024 * 1024};
  std::size_t maxRetainedRecords{10000};
  std::size_t maxMetadataBytes{1024 * 1024};
};

enum class WindowCompositionErrorCode : std::uint8_t {
  InvalidRequest,
  ScanLimitExceeded,
  ScanByteLimitExceeded,
  SourceGapLimitExceeded,
  SourceGapByteLimitExceeded,
  WrongSourceKind,
  InvalidObservation,
  MissingTimeColumn,
  AmbiguousTimeColumn,
  InvalidRetainedRow,
  KeyByteLimitExceeded,
  RetainedRecordLimitExceeded,
  MetadataLimitExceeded,
  InvalidEvaluation,
  LateDataRejected,
  DifferentCase,
  KernelFailure
};

struct WindowCompositionError {
  WindowCompositionErrorCode code{WindowCompositionErrorCode::InvalidRequest};
  std::string message;
  std::optional<std::size_t> occurrence;
};

// Worker-safe input has only copied scalar/time values, never evidence IDs or
// engine objects. The owner keeps corresponding retained witnesses below.
struct ObservationWindowEvaluation {
  vql::WindowRequest request;
  std::vector<vql::WindowOccurrence> occurrences;
};

struct ObservationWindowLineage {
  std::string caseDocumentId;
  RecordId observation{};
  std::string timeColumn;
  std::optional<RecordId> timeField;
  std::optional<std::string> arrivalWatermarkColumn;
  std::optional<RecordId> arrivalWatermarkField;
  std::optional<std::string> keyColumn;
  std::optional<RecordId> keyField;
  std::vector<RecordId> rows;
  std::vector<std::optional<RecordId>> timeValues;
  std::vector<std::optional<RecordId>> arrivalWatermarkValues;
  std::vector<std::optional<RecordId>> keyValues;
  std::vector<std::string> sourceGaps;
  std::vector<RecordId> sourceGapRecords;
  std::string observationTimeWitness;
};

struct PreparedObservationWindow {
  ObservationWindowEvaluation evaluation;
  ObservationWindowLineage lineage;
  std::size_t maxScanRecords{};
  std::size_t maxScanBytes{};
  std::size_t maxRetainedRecords{};
  std::size_t maxMetadataBytes{};
};

struct ObservationWindowRetention {
  RecordId queryRun{};
  RecordId window{};
  std::vector<RecordId> members;
  std::vector<RecordId> exclusions;
  std::vector<RecordId> gaps;
};

struct RejectedObservationWindowRetention {
  RecordId queryRun{};
  RecordId window{};
  RecordId exclusion{};
  std::vector<RecordId> gaps;
};

[[nodiscard]] std::expected<PreparedObservationWindow, WindowCompositionError>
prepareSqliteObservationWindow(const EvidenceCase &evidence,
                               const SqliteObservationWindowRequest &request);

[[nodiscard]] std::expected<vql::WindowResult, WindowCompositionError>
evaluateObservationWindow(const ObservationWindowEvaluation &evaluation);

[[nodiscard]] std::expected<ObservationWindowRetention, WindowCompositionError>
retainObservationWindow(EvidenceCase &evidence,
                        const PreparedObservationWindow &prepared,
                        const vql::WindowResult &result);

[[nodiscard]] std::expected<RejectedObservationWindowRetention,
                            WindowCompositionError>
retainRejectedObservationWindow(EvidenceCase &evidence,
                                const PreparedObservationWindow &prepared,
                                const WindowCompositionError &rejection);

} // namespace xanadu::evidence
#endif
