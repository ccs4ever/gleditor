#include "common/xanadu/evidence/query_composition.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace xanadu::evidence {
namespace {

using RecordMap  = std::unordered_map<RecordId, Record>;
using ChildIndex = std::unordered_map<RecordId, std::vector<RecordId>>;

std::span<const RecordId> childrenOf(const ChildIndex &children,
                                     const RecordId parent) {
  const auto found = children.find(parent);
  return found == children.end() ? std::span<const RecordId>{}
                                 : std::span<const RecordId>{found->second};
}

QueryCompositionError failure(const QueryCompositionErrorCode code,
                              std::string message) {
  return {.code = code, .message = std::move(message)};
}

bool addWithin(std::size_t &total, const std::size_t amount,
               const std::size_t limit) {
  if (amount > limit - std::min(total, limit)) return false;
  total += amount;
  return true;
}

std::string number(const std::size_t value) { return std::to_string(value); }

std::optional<std::string> attribute(const Record &record,
                                     const std::string_view name) {
  const auto found = record.attributes.find(std::string(name));
  if (found == record.attributes.end()) return {};
  return found->second;
}

std::optional<vql::RelationInputCoverage>
coverageOf(const Record &observation) {
  const auto state = attribute(observation, "capture-state");
  if (!state) return {};
  if (*state == "failed" || *state == "cancelled")
    return vql::RelationInputCoverage::Gapped;
  if (*state == "partial") return vql::RelationInputCoverage::Partial;
  if (*state == "complete") return vql::RelationInputCoverage::Complete;
  return {};
}

std::string_view coverageName(const vql::RelationInputCoverage coverage) {
  return vql::relationCoverageName(coverage);
}

std::string_view joinName(const vql::RelationJoinKind kind) {
  return kind == vql::RelationJoinKind::Inner ? "inner" : "left";
}

std::string_view cardinalityName(const vql::RelationCardinality cardinality) {
  return cardinality == vql::RelationCardinality::Many ? "many" : "atMostOne";
}

std::string_view nullPolicyName(const vql::RelationNullMatchPolicy policy) {
  return policy == vql::RelationNullMatchPolicy::Never ? "never" : "equal";
}

bool sameKey(const vql::RelationKey &left, const vql::RelationKey &right) {
  return left.state == right.state &&
         left.canonicalBytes == right.canonicalBytes;
}

std::optional<std::size_t> resultPosition(const Record &record) {
  const auto text = attribute(record, "result-position");
  if (!text) return {};
  std::size_t value{};
  const auto [end, error] =
      std::from_chars(text->data(), text->data() + text->size(), value);
  if (error != std::errc{} || end != text->data() + text->size()) return {};
  return value;
}

struct SelectedColumn {
  std::optional<RecordId> field;
  std::vector<RecordId> rows;
  std::vector<RecordId> values;
  std::vector<vql::RelationKey> keys;
  vql::RelationInputCoverage coverage{vql::RelationInputCoverage::Complete};
};

std::expected<SelectedColumn, QueryCompositionError>
selectColumn(const EvidenceCase &evidence, const RecordMap &records,
             const ChildIndex &children, const Record &observation,
             const RecordId observationId, const std::string &columnName,
             const std::size_t maximumKeyBytes,
             std::size_t &aggregateKeyBytes) {
  const auto sourceId = observation.parent;
  if (!sourceId)
    return std::unexpected(
        failure(QueryCompositionErrorCode::InvalidObservation,
                "SQLite observation has no retained source"));
  const auto source = records.find(*sourceId);
  if (source == records.end() || source->second.kind != "connection" ||
      attribute(source->second, "source-kind") !=
          std::optional<std::string>("sqlite"))
    return std::unexpected(failure(QueryCompositionErrorCode::WrongSourceKind,
                                   "observation source is not SQLite"));

  SelectedColumn selected;
  const auto coverage = coverageOf(observation);
  if (!coverage)
    return std::unexpected(
        failure(QueryCompositionErrorCode::InvalidObservation,
                "SQLite observation has an unknown capture state"));
  selected.coverage = *coverage;
  for (const auto id : childrenOf(children, observationId)) {
    const auto &record = records.at(id);
    if (record.kind == "field" && record.label == columnName) {
      if (selected.field)
        return std::unexpected(failure(
            QueryCompositionErrorCode::AmbiguousKeyColumn,
            "selected SQLite key column has duplicate field witnesses"));
      selected.field = id;
    }
    if (record.kind == "row") selected.rows.push_back(id);
  }
  if (!selected.field)
    return std::unexpected(
        failure(QueryCompositionErrorCode::MissingKeyColumn,
                "selected SQLite key column was not retained"));
  for (const auto rowId : selected.rows) {
    if (!resultPosition(records.at(rowId)))
      return std::unexpected(
          failure(QueryCompositionErrorCode::InvalidRetainedRow,
                  "SQLite row has an invalid retained result position"));
  }
  std::ranges::sort(selected.rows, [&](const RecordId lhs, const RecordId rhs) {
    return *resultPosition(records.at(lhs)) < *resultPosition(records.at(rhs));
  });

  for (const auto rowId : selected.rows) {
    const auto rowIt = records.find(rowId);
    if (rowIt == records.end())
      return std::unexpected(
          failure(QueryCompositionErrorCode::InvalidRetainedRow,
                  "SQLite row is absent from bounded record scan"));
    std::optional<RecordId> keyValue;
    std::size_t matches{};
    for (const auto valueId : childrenOf(children, rowId)) {
      const auto &value = records.at(valueId);
      if (value.kind == "value" && value.label == columnName) {
        keyValue = valueId;
        ++matches;
      }
    }
    if (matches != 1)
      return std::unexpected(failure(
          QueryCompositionErrorCode::InvalidRetainedRow,
          "SQLite row does not have exactly one selected value witness"));

    const auto valueId = *keyValue;
    const auto &value  = records.at(valueId);
    std::size_t fieldReferences{};
    bool selectedFieldReferenced{};
    for (const auto referenceId : childrenOf(children, valueId)) {
      const auto &reference = records.at(referenceId);
      if (reference.kind == "reference" &&
          reference.label == "field occurrence") {
        ++fieldReferences;
        selectedFieldReferenced =
            reference.text == std::to_string(*selected.field);
      }
    }
    if (fieldReferences != 1 || !selectedFieldReferenced)
      return std::unexpected(
          failure(QueryCompositionErrorCode::InvalidRetainedRow,
                  "SQLite key value does not uniquely reference its selected "
                  "field witness"));
    const auto state   = attribute(value, "value-state");
    const auto storage = attribute(value, "storage-class");
    if (!state || !storage)
      return std::unexpected(
          failure(QueryCompositionErrorCode::InvalidRetainedRow,
                  "SQLite value lacks retained storage metadata"));
    if (*state == "missing") {
      selected.keys.push_back({.state = vql::RelationKeyState::Missing});
    } else if (*state == "null") {
      if (*storage != "NULL")
        return std::unexpected(
            failure(QueryCompositionErrorCode::InvalidRetainedRow,
                    "SQLite null value has inconsistent storage metadata"));
      selected.keys.push_back({.state = vql::RelationKeyState::Null});
    } else if (*state == "present") {
      if (*storage != "integer" && *storage != "real" && *storage != "text" &&
          *storage != "blob")
        return std::unexpected(
            failure(QueryCompositionErrorCode::InvalidRetainedRow,
                    "SQLite present value has invalid storage metadata"));
      const auto current = aggregateKeyBytes;
      if (maximumKeyBytes < 9 || current > maximumKeyBytes - 9)
        return std::unexpected(
            failure(QueryCompositionErrorCode::KeyByteLimitExceeded,
                    "SQLite join key byte limit exceeded"));
      std::string bytes;
      try {
        bytes = evidence.rawText(valueId, maximumKeyBytes - current - 9);
      } catch (const std::length_error &) {
        return std::unexpected(
            failure(QueryCompositionErrorCode::KeyByteLimitExceeded,
                    "SQLite join key byte limit exceeded"));
      }
      if (!addWithin(aggregateKeyBytes, bytes.size() + 9, maximumKeyBytes))
        return std::unexpected(
            failure(QueryCompositionErrorCode::KeyByteLimitExceeded,
                    "SQLite join key byte limit exceeded"));
      // Type byte plus an unsigned 64-bit length prevents cross-storage-class
      // collisions and ambiguous concatenations.
      std::string canonical;
      canonical.reserve(bytes.size() + 9);
      canonical.push_back(storage->front());
      const auto length = static_cast<std::uint64_t>(bytes.size());
      for (int shift = 56; shift >= 0; shift -= 8)
        canonical.push_back(static_cast<char>((length >> shift) & 0xff));
      canonical += bytes;
      selected.keys.push_back({.state          = vql::RelationKeyState::Present,
                               .canonicalBytes = std::move(canonical)});
    } else {
      return std::unexpected(
          failure(QueryCompositionErrorCode::InvalidRetainedRow,
                  "SQLite value has an unknown retained state"));
    }
    selected.values.push_back(valueId);
  }
  return selected;
}

std::expected<RecordMap, QueryCompositionError>
boundedRecords(const EvidenceCase &evidence, const std::size_t maximum,
               const std::size_t maximumBytes) {
  if (maximum == 0)
    return std::unexpected(failure(QueryCompositionErrorCode::InvalidRequest,
                                   "SQLite join scan limit must be positive"));
  const auto page = evidence.recordIds(maximum);
  if (!page.complete)
    return std::unexpected(
        failure(QueryCompositionErrorCode::ScanLimitExceeded,
                "evidence case exceeds the bounded SQLite join scan"));
  RecordMap records;
  records.reserve(page.ids.size());
  std::size_t scannedBytes{};
  for (const auto id : page.ids) {
    auto record       = evidence.inspect(id);
    const auto charge = [&](std::string_view value) {
      return addWithin(scannedBytes, value.size(), maximumBytes);
    };
    bool fits =
        charge(record.kind) && charge(record.label) && charge(record.text);
    for (const auto &[key, value] : record.attributes)
      fits = fits && charge(key) && charge(value);
    if (!fits)
      return std::unexpected(
          failure(QueryCompositionErrorCode::ScanByteLimitExceeded,
                  "evidence case exceeds the scanned record byte budget"));
    records.emplace(id, std::move(record));
  }
  return records;
}

std::expected<void, QueryCompositionError>
validateRequest(const SqliteObservationJoinRequest &request) {
  if (request.leftKeyColumn.empty() || request.rightKeyColumn.empty() ||
      request.maxScanRecords == 0 || request.maxRetainedRecords == 0 ||
      request.maxMetadataBytes == 0 || request.maxScanBytes == 0)
    return std::unexpected(
        failure(QueryCompositionErrorCode::InvalidRequest,
                "SQLite join request limits and columns must be set"));
  return {};
}

} // namespace

std::expected<PreparedSqliteObservationJoin, QueryCompositionError>
prepareSqliteObservationJoin(const EvidenceCase &evidence,
                             const SqliteObservationJoinRequest &request) {
  if (const auto valid = validateRequest(request); !valid)
    return std::unexpected(valid.error());
  if (request.maxScanRecords == std::numeric_limits<std::size_t>::max())
    return std::unexpected(failure(QueryCompositionErrorCode::InvalidRequest,
                                   "SQLite join scan limit is too large"));
  auto scanned =
      boundedRecords(evidence, request.maxScanRecords, request.maxScanBytes);
  if (!scanned) return std::unexpected(scanned.error());
  const auto &records = *scanned;
  ChildIndex children;
  for (const auto &[id, record] : records)
    if (record.parent) children[*record.parent].push_back(id);
  const auto leftObservation  = records.find(request.leftObservation);
  const auto rightObservation = records.find(request.rightObservation);
  if (leftObservation == records.end() || rightObservation == records.end() ||
      leftObservation->second.kind != "observation" ||
      rightObservation->second.kind != "observation")
    return std::unexpected(
        failure(QueryCompositionErrorCode::InvalidObservation,
                "selected SQLite observations are not retained in this case"));

  std::size_t metadataBytes{};
  for (const auto *value : {&request.leftKeyColumn, &request.rightKeyColumn}) {
    if (!addWithin(metadataBytes, value->size(), request.maxMetadataBytes))
      return std::unexpected(
          failure(QueryCompositionErrorCode::MetadataLimitExceeded,
                  "SQLite join metadata byte limit exceeded"));
  }
  for (const auto *observation :
       {&leftObservation->second, &rightObservation->second}) {
    const auto sourceCoverage = attribute(*observation, "coverage");
    if (sourceCoverage && !addWithin(metadataBytes, sourceCoverage->size(),
                                     request.maxMetadataBytes))
      return std::unexpected(
          failure(QueryCompositionErrorCode::MetadataLimitExceeded,
                  "SQLite join metadata byte limit exceeded"));
  }
  std::size_t leftRowCount{};
  std::size_t rightRowCount{};
  for (const auto &[unusedId, record] : records) {
    static_cast<void>(unusedId);
    if (record.parent == request.leftObservation && record.kind == "row")
      ++leftRowCount;
    if (record.parent == request.rightObservation && record.kind == "row")
      ++rightRowCount;
  }
  if (leftRowCount > request.joinLimits.maxInputRows ||
      rightRowCount > request.joinLimits.maxInputRows ||
      rightRowCount >
          request.joinLimits.maxInputRows -
              std::min(leftRowCount, request.joinLimits.maxInputRows))
    return std::unexpected(failure(QueryCompositionErrorCode::KernelFailure,
                                   "SQLite join input row limit exceeded"));
  std::size_t aggregateKeyBytes{};
  auto left = selectColumn(evidence, records, children, leftObservation->second,
                           request.leftObservation, request.leftKeyColumn,
                           request.joinLimits.maxKeyBytes, aggregateKeyBytes);
  if (!left) return std::unexpected(left.error());
  auto right =
      selectColumn(evidence, records, children, rightObservation->second,
                   request.rightObservation, request.rightKeyColumn,
                   request.joinLimits.maxKeyBytes, aggregateKeyBytes);
  if (!right) return std::unexpected(right.error());
  if (metadataBytes > request.maxMetadataBytes)
    return std::unexpected(
        failure(QueryCompositionErrorCode::MetadataLimitExceeded,
                "SQLite join metadata byte limit exceeded"));

  auto kernelRequest = vql::RelationJoinRequest{
      .kind          = request.kind,
      .cardinality   = request.cardinality,
      .nullMatch     = request.nullMatch,
      .keyPolicy     = "storage-class-and-retained-bytes-v1",
      .leftCoverage  = left->coverage,
      .rightCoverage = right->coverage,
      .limits        = request.joinLimits};
  if (left->rows.size() > request.joinLimits.maxInputRows ||
      right->rows.size() > request.joinLimits.maxInputRows ||
      right->rows.size() >
          request.joinLimits.maxInputRows -
              std::min(left->rows.size(), request.joinLimits.maxInputRows))
    return std::unexpected(failure(QueryCompositionErrorCode::KernelFailure,
                                   "SQLite join input row limit exceeded"));

  PreparedSqliteObservationJoin prepared;
  prepared.evaluation = {.request   = std::move(kernelRequest),
                         .leftKeys  = std::move(left->keys),
                         .rightKeys = std::move(right->keys)};
  prepared.lineage    = {
         .caseDocumentId     = evidence.store().documentId().str(),
         .leftObservation    = request.leftObservation,
         .rightObservation   = request.rightObservation,
         .leftKeyColumn      = request.leftKeyColumn,
         .rightKeyColumn     = request.rightKeyColumn,
         .leftSourceCoverage = attribute(leftObservation->second, "coverage")
                                .value_or("unspecified"),
         .rightSourceCoverage = attribute(rightObservation->second, "coverage")
                                 .value_or("unspecified"),
         .leftCoverage   = left->coverage,
         .rightCoverage  = right->coverage,
         .leftRows       = std::move(left->rows),
         .rightRows      = std::move(right->rows),
         .leftKeyValues  = std::move(left->values),
         .rightKeyValues = std::move(right->values)};
  prepared.maxRetainedRecords = request.maxRetainedRecords;
  prepared.maxScanRecords     = request.maxScanRecords;
  prepared.maxScanBytes       = request.maxScanBytes;
  prepared.maxMetadataBytes   = request.maxMetadataBytes;
  return prepared;
}

std::expected<vql::RelationJoinResult, QueryCompositionError>
evaluateSqliteObservationJoin(const SqliteJoinEvaluation &evaluation) {
  const auto leftAt = [&](const std::size_t index) {
    return evaluation.leftKeys.at(index);
  };
  const auto rightAt = [&](const std::size_t index) {
    return evaluation.rightKeys.at(index);
  };
  auto joined =
      vql::joinOccurrences(evaluation.request, evaluation.leftKeys.size(),
                           leftAt, evaluation.rightKeys.size(), rightAt);
  if (!joined)
    return std::unexpected(failure(QueryCompositionErrorCode::KernelFailure,
                                   joined.error().message));
  return *joined;
}

std::expected<SqliteObservationJoinRetention, QueryCompositionError>
retainSqliteObservationJoin(EvidenceCase &evidence,
                            const PreparedSqliteObservationJoin &prepared,
                            const vql::RelationJoinResult &result) {
  const auto &lineage = prepared.lineage;
  if (!evidence.isOpen() ||
      evidence.store().documentId().str() != lineage.caseDocumentId)
    return std::unexpected(
        failure(QueryCompositionErrorCode::DifferentCase,
                "prepared SQLite join belongs to another case"));
  SqliteObservationJoinRequest currentRequest{
      .leftObservation    = lineage.leftObservation,
      .leftKeyColumn      = lineage.leftKeyColumn,
      .rightObservation   = lineage.rightObservation,
      .rightKeyColumn     = lineage.rightKeyColumn,
      .kind               = prepared.evaluation.request.kind,
      .cardinality        = prepared.evaluation.request.cardinality,
      .nullMatch          = prepared.evaluation.request.nullMatch,
      .maxScanRecords     = prepared.maxScanRecords,
      .maxRetainedRecords = prepared.maxRetainedRecords,
      .maxMetadataBytes   = prepared.maxMetadataBytes,
      .joinLimits         = prepared.evaluation.request.limits,
      .maxScanBytes       = prepared.maxScanBytes};
  auto current = prepareSqliteObservationJoin(evidence, currentRequest);
  if (!current || current->lineage.leftRows != lineage.leftRows ||
      current->lineage.rightRows != lineage.rightRows ||
      current->lineage.leftKeyValues != lineage.leftKeyValues ||
      current->lineage.rightKeyValues != lineage.rightKeyValues ||
      current->evaluation.leftKeys.size() !=
          prepared.evaluation.leftKeys.size() ||
      current->evaluation.rightKeys.size() !=
          prepared.evaluation.rightKeys.size())
    return std::unexpected(failure(
        QueryCompositionErrorCode::InvalidEvaluation,
        "prepared SQLite join lineage no longer matches retained records"));
  for (std::size_t i = 0; i < current->evaluation.leftKeys.size(); ++i) {
    if (!sameKey(current->evaluation.leftKeys[i],
                 prepared.evaluation.leftKeys[i]))
      return std::unexpected(
          failure(QueryCompositionErrorCode::InvalidEvaluation,
                  "prepared SQLite left keys do not match retained witnesses"));
  }
  for (std::size_t i = 0; i < current->evaluation.rightKeys.size(); ++i) {
    if (!sameKey(current->evaluation.rightKeys[i],
                 prepared.evaluation.rightKeys[i]))
      return std::unexpected(failure(
          QueryCompositionErrorCode::InvalidEvaluation,
          "prepared SQLite right keys do not match retained witnesses"));
  }
  auto checked = evaluateSqliteObservationJoin(prepared.evaluation);
  if (!checked || checked->pairs != result.pairs ||
      checked->leftCoverage != result.leftCoverage ||
      checked->rightCoverage != result.rightCoverage ||
      checked->inputCoverage != result.inputCoverage ||
      checked->keyPolicy != result.keyPolicy || checked->kind != result.kind ||
      checked->cardinality != result.cardinality ||
      checked->nullMatch != result.nullMatch ||
      checked->truncated != result.truncated || checked->gap != result.gap ||
      checked->work != result.work)
    return std::unexpected(failure(
        QueryCompositionErrorCode::InvalidEvaluation,
        "SQLite join result does not match its prepared inputs and policies"));
  if (lineage.leftRows.size() != prepared.evaluation.leftKeys.size() ||
      lineage.rightRows.size() != prepared.evaluation.rightKeys.size() ||
      lineage.leftKeyValues.size() != lineage.leftRows.size() ||
      lineage.rightKeyValues.size() != lineage.rightRows.size())
    return std::unexpected(
        failure(QueryCompositionErrorCode::InvalidEvaluation,
                "prepared SQLite join lineage is malformed"));

  std::size_t recordCount = 5; // run, two inputs, and their target records
  if (recordCount > prepared.maxRetainedRecords)
    return std::unexpected(
        failure(QueryCompositionErrorCode::RetainedRecordLimitExceeded,
                "SQLite join retained-record limit exceeded"));
  std::size_t metadataBytes{};
  const auto charge = [&](std::string_view value) {
    return addWithin(metadataBytes, value.size(), prepared.maxMetadataBytes);
  };
  if (!charge(prepared.evaluation.request.keyPolicy) ||
      !charge(lineage.leftKeyColumn) || !charge(lineage.rightKeyColumn) ||
      !charge(number(lineage.leftObservation)) ||
      !charge(number(lineage.rightObservation)) ||
      !charge(result.gap.value_or("")))
    return std::unexpected(
        failure(QueryCompositionErrorCode::MetadataLimitExceeded,
                "SQLite join metadata byte limit exceeded"));
  for (const auto value :
       {result.work, prepared.maxScanRecords, prepared.maxScanBytes,
        prepared.maxRetainedRecords, prepared.maxMetadataBytes,
        prepared.evaluation.request.limits.maxInputRows,
        prepared.evaluation.request.limits.maxKeyBytes,
        prepared.evaluation.request.limits.maxOutputPairs,
        prepared.evaluation.request.limits.maxWork,
        prepared.evaluation.request.limits.maxPolicyBytes})
    if (!charge(number(value)))
      return std::unexpected(
          failure(QueryCompositionErrorCode::MetadataLimitExceeded,
                  "SQLite join metadata byte limit exceeded"));
  if (!addWithin(metadataBytes, lineage.leftSourceCoverage.size(),
                 prepared.maxMetadataBytes) ||
      !addWithin(metadataBytes, lineage.rightSourceCoverage.size(),
                 prepared.maxMetadataBytes))
    return std::unexpected(
        failure(QueryCompositionErrorCode::MetadataLimitExceeded,
                "SQLite join metadata byte limit exceeded"));
  std::size_t ordinal{};
  for (const auto &pair : result.pairs) {
    if (pair.leftOccurrence >= lineage.leftRows.size())
      return std::unexpected(
          failure(QueryCompositionErrorCode::InvalidEvaluation,
                  "SQLite join left occurrence is out of range"));
    if (!charge(number(ordinal++)) || !charge(number(pair.leftOccurrence)) ||
        !charge(lineage.leftKeyColumn) ||
        !charge(number(lineage.leftRows[pair.leftOccurrence])) ||
        !charge(number(lineage.leftKeyValues[pair.leftOccurrence])))
      return std::unexpected(
          failure(QueryCompositionErrorCode::MetadataLimitExceeded,
                  "SQLite join output metadata byte limit exceeded"));
    if (pair.state == vql::RelationPairState::Matched) {
      if (!pair.rightOccurrence ||
          *pair.rightOccurrence >= lineage.rightRows.size() ||
          !sameKey(prepared.evaluation.leftKeys[pair.leftOccurrence],
                   prepared.evaluation.rightKeys[*pair.rightOccurrence]))
        return std::unexpected(
            failure(QueryCompositionErrorCode::InvalidEvaluation,
                    "SQLite matched pair has invalid key lineage"));
      if (!charge(number(*pair.rightOccurrence)) ||
          !charge(lineage.rightKeyColumn) ||
          !charge(number(lineage.rightRows[*pair.rightOccurrence])) ||
          !charge(number(lineage.rightKeyValues[*pair.rightOccurrence])))
        return std::unexpected(
            failure(QueryCompositionErrorCode::MetadataLimitExceeded,
                    "SQLite join output metadata byte limit exceeded"));
      if (!addWithin(recordCount, 9, prepared.maxRetainedRecords))
        return std::unexpected(
            failure(QueryCompositionErrorCode::RetainedRecordLimitExceeded,
                    "SQLite join retained-record limit exceeded"));
    } else {
      if (prepared.evaluation.request.kind != vql::RelationJoinKind::Left ||
          pair.rightOccurrence ||
          (pair.state == vql::RelationPairState::Unmatched &&
           lineage.rightCoverage != vql::RelationInputCoverage::Complete) ||
          (pair.state == vql::RelationPairState::UnknownCoverage &&
           lineage.rightCoverage == vql::RelationInputCoverage::Complete))
        return std::unexpected(
            failure(QueryCompositionErrorCode::InvalidEvaluation,
                    "SQLite unmatched pair has invalid coverage"));
      if (!addWithin(recordCount, 5, prepared.maxRetainedRecords))
        return std::unexpected(
            failure(QueryCompositionErrorCode::RetainedRecordLimitExceeded,
                    "SQLite join retained-record limit exceeded"));
    }
  }

  // All expected budget and lineage errors are refused above, before writes.
  const auto run = evidence.appendRecord(
      "query-run", "SQLite observation join",
      "Finite join over retained SQLite observations", {},
      {{"algorithm", "finite-occurrence-hash-join-v1"},
       {"key-policy", result.keyPolicy},
       {"join-kind", std::string(joinName(result.kind))},
       {"cardinality", std::string(cardinalityName(result.cardinality))},
       {"null-match", std::string(nullPolicyName(result.nullMatch))},
       {"left-key-column", lineage.leftKeyColumn},
       {"right-key-column", lineage.rightKeyColumn},
       {"left-source-coverage", lineage.leftSourceCoverage},
       {"right-source-coverage", lineage.rightSourceCoverage},
       {"left-coverage", std::string(coverageName(result.leftCoverage))},
       {"right-coverage", std::string(coverageName(result.rightCoverage))},
       {"input-coverage", std::string(coverageName(result.inputCoverage))},
       {"empty-result-reason",
        result.pairs.empty() ? "join emitted no occurrence pairs" : ""},
       {"truncated", result.truncated ? "true" : "false"},
       {"gap", result.gap.value_or("")},
       {"work", number(result.work)},
       {"max-input-rows",
        number(prepared.evaluation.request.limits.maxInputRows)},
       {"max-key-bytes",
        number(prepared.evaluation.request.limits.maxKeyBytes)},
       {"max-output-pairs",
        number(prepared.evaluation.request.limits.maxOutputPairs)},
       {"max-work", number(prepared.evaluation.request.limits.maxWork)},
       {"max-policy-bytes",
        number(prepared.evaluation.request.limits.maxPolicyBytes)},
       {"max-scan-records", number(prepared.maxScanRecords)},
       {"max-scan-bytes", number(prepared.maxScanBytes)},
       {"max-retained-records", number(prepared.maxRetainedRecords)},
       {"max-metadata-bytes", number(prepared.maxMetadataBytes)}});
  const auto leftInput = evidence.appendRecord(
      "query-input", "left", "Pinned retained input observation", run,
      {{"coverage", std::string(coverageName(result.leftCoverage))}},
      {{"d.evidence.target", lineage.leftObservation}});
  const auto rightInput = evidence.appendRecord(
      "query-input", "right", "Pinned retained input observation", run,
      {{"coverage", std::string(coverageName(result.rightCoverage))}},
      {{"d.evidence.target", lineage.rightObservation}});
  static_cast<void>(leftInput);
  static_cast<void>(rightInput);

  SqliteObservationJoinRetention retention{.queryRun = run};
  retention.occurrences.reserve(result.pairs.size());
  for (std::size_t ordinal = 0; ordinal < result.pairs.size(); ++ordinal) {
    const auto &pair      = result.pairs[ordinal];
    const auto occurrence = evidence.appendRecord(
        "query-result", "occurrence " + number(ordinal),
        std::string(vql::relationPairStateName(pair.state)), run,
        {{"state", std::string(vql::relationPairStateName(pair.state))},
         {"left-occurrence", number(pair.leftOccurrence)},
         {"right-occurrence",
          pair.rightOccurrence ? number(*pair.rightOccurrence) : ""}});
    const auto leftRow = lineage.leftRows[pair.leftOccurrence];
    static_cast<void>(evidence.appendRecord(
        "query-lineage", "left row", "Retained input row", occurrence, {},
        {{"d.evidence.target", leftRow}}));
    const auto leftWitness = evidence.appendRecord(
        "query-key-witness", "left key", lineage.leftKeyColumn, occurrence, {},
        {{"d.evidence.target", lineage.leftKeyValues[pair.leftOccurrence]}});
    static_cast<void>(leftWitness);
    if (pair.rightOccurrence) {
      const auto rightRow = lineage.rightRows[*pair.rightOccurrence];
      static_cast<void>(evidence.appendRecord(
          "query-lineage", "right row", "Retained input row", occurrence, {},
          {{"d.evidence.target", rightRow}}));
      static_cast<void>(evidence.appendRecord(
          "query-key-witness", "right key", lineage.rightKeyColumn, occurrence,
          {},
          {{"d.evidence.target",
            lineage.rightKeyValues[*pair.rightOccurrence]}}));
    }
    retention.occurrences.push_back(occurrence);
  }
  return retention;
}

} // namespace xanadu::evidence
