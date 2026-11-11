#ifndef XANADU_EVIDENCE_QUERY_COMPOSITION_HPP
#define XANADU_EVIDENCE_QUERY_COMPOSITION_HPP

#include <cstddef>
#include <expected>
#include <string>
#include <vector>

#include "common/xanadu/evidence/evidence_case.hpp"
#include "common/xanadu/vql/relations.hpp"

namespace xanadu::evidence {

struct SqliteObservationJoinRequest {
  RecordId leftObservation{};
  std::string leftKeyColumn;
  RecordId rightObservation{};
  std::string rightKeyColumn;
  vql::RelationJoinKind kind{vql::RelationJoinKind::Inner};
  vql::RelationCardinality cardinality{vql::RelationCardinality::Many};
  vql::RelationNullMatchPolicy nullMatch{vql::RelationNullMatchPolicy::Never};
  std::size_t maxScanRecords{100000};
  std::size_t maxRetainedRecords{10000};
  std::size_t maxMetadataBytes{1024 * 1024};
  vql::RelationJoinLimits joinLimits{.maxInputRows   = 10000,
                                     .maxKeyBytes    = 1024 * 1024,
                                     .maxOutputPairs = 1000,
                                     .maxWork        = 1000000};
  std::size_t maxScanBytes{8 * 1024 * 1024};
};

enum class QueryCompositionErrorCode {
  InvalidRequest,
  ScanLimitExceeded,
  ScanByteLimitExceeded,
  WrongSourceKind,
  InvalidObservation,
  AmbiguousKeyColumn,
  MissingKeyColumn,
  InvalidRetainedRow,
  KeyByteLimitExceeded,
  RetainedRecordLimitExceeded,
  MetadataLimitExceeded,
  InvalidEvaluation,
  DifferentCase,
  KernelFailure
};

struct QueryCompositionError {
  QueryCompositionErrorCode code{QueryCompositionErrorCode::InvalidRequest};
  std::string message;
};

// Worker-safe value data contains no Store, CellRef, RecordId or EvidenceCase.
struct SqliteJoinEvaluation {
  vql::RelationJoinRequest request;
  std::vector<vql::RelationKey> leftKeys;
  std::vector<vql::RelationKey> rightKeys;
};

// Owner-thread lineage is kept separate from the worker-safe evaluation data.
struct SqliteJoinLineage {
  std::string caseDocumentId;
  RecordId leftObservation{};
  RecordId rightObservation{};
  std::string leftKeyColumn;
  std::string rightKeyColumn;
  std::string leftSourceCoverage;
  std::string rightSourceCoverage;
  vql::RelationInputCoverage leftCoverage{vql::RelationInputCoverage::Complete};
  vql::RelationInputCoverage rightCoverage{
      vql::RelationInputCoverage::Complete};
  std::vector<RecordId> leftRows;
  std::vector<RecordId> rightRows;
  std::vector<RecordId> leftKeyValues;
  std::vector<RecordId> rightKeyValues;
};

struct PreparedSqliteObservationJoin {
  SqliteJoinEvaluation evaluation;
  SqliteJoinLineage lineage;
  std::size_t maxScanRecords{};
  std::size_t maxRetainedRecords{};
  std::size_t maxMetadataBytes{};
  std::size_t maxScanBytes{};
};

struct SqliteObservationJoinRetention {
  RecordId queryRun{};
  std::vector<RecordId> occurrences;
};

[[nodiscard]] std::expected<PreparedSqliteObservationJoin,
                            QueryCompositionError>
prepareSqliteObservationJoin(const EvidenceCase &evidence,
                             const SqliteObservationJoinRequest &request);

[[nodiscard]] std::expected<vql::RelationJoinResult, QueryCompositionError>
evaluateSqliteObservationJoin(const SqliteJoinEvaluation &evaluation);

[[nodiscard]] std::expected<SqliteObservationJoinRetention,
                            QueryCompositionError>
retainSqliteObservationJoin(EvidenceCase &evidence,
                            const PreparedSqliteObservationJoin &prepared,
                            const vql::RelationJoinResult &result);

} // namespace xanadu::evidence
#endif
