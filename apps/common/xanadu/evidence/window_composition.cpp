#include "common/xanadu/evidence/window_composition.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace xanadu::evidence {
namespace {

using RecordMap  = std::unordered_map<RecordId, Record>;
using ChildIndex = std::unordered_map<RecordId, std::vector<RecordId>>;

WindowCompositionError
failure(const WindowCompositionErrorCode code, std::string message,
        const std::optional<std::size_t> occurrence = {}) {
  return {
      .code = code, .message = std::move(message), .occurrence = occurrence};
}

bool addWithin(std::size_t &total, const std::size_t amount,
               const std::size_t limit) {
  if (amount > limit - std::min(total, limit)) return false;
  total += amount;
  return true;
}

std::optional<std::string> attribute(const Record &record,
                                     const std::string_view name) {
  const auto found = record.attributes.find(std::string(name));
  if (found == record.attributes.end()) return {};
  return found->second;
}

std::expected<RecordMap, WindowCompositionError>
boundedRecords(const EvidenceCase &evidence, const std::size_t maximum,
               const std::size_t maximumBytes) {
  if (maximum == 0 || maximumBytes == 0)
    return std::unexpected(failure(WindowCompositionErrorCode::InvalidRequest,
                                   "window scan budgets must be positive"));
  const auto page = evidence.recordIds(maximum);
  if (!page.complete)
    return std::unexpected(
        failure(WindowCompositionErrorCode::ScanLimitExceeded,
                "evidence case exceeds the bounded window scan"));
  RecordMap records;
  records.reserve(page.ids.size());
  std::size_t bytes{};
  for (const auto id : page.ids) {
    auto record = evidence.inspect(id);
    bool fits   = addWithin(bytes, record.kind.size(), maximumBytes) &&
                addWithin(bytes, record.label.size(), maximumBytes) &&
                addWithin(bytes, record.text.size(), maximumBytes);
    for (const auto &[key, value] : record.attributes)
      fits = fits && addWithin(bytes, key.size(), maximumBytes) &&
             addWithin(bytes, value.size(), maximumBytes);
    if (!fits)
      return std::unexpected(
          failure(WindowCompositionErrorCode::ScanByteLimitExceeded,
                  "window scan byte limit exceeded"));
    records.emplace(id, std::move(record));
  }
  return records;
}

ChildIndex childIndex(const RecordMap &records) {
  ChildIndex children;
  for (const auto &[id, record] : records)
    if (record.parent) children[*record.parent].push_back(id);
  for (auto &[unused, ids] : children) {
    static_cast<void>(unused);
    std::ranges::sort(ids);
  }
  return children;
}

std::span<const RecordId> childrenOf(const ChildIndex &children,
                                     const RecordId parent) {
  const auto found = children.find(parent);
  return found == children.end() ? std::span<const RecordId>{}
                                 : std::span<const RecordId>{found->second};
}

std::optional<std::size_t> position(const Record &record) {
  const auto text = attribute(record, "result-position");
  if (!text) return {};
  std::size_t result{};
  const auto [end, error] =
      std::from_chars(text->data(), text->data() + text->size(), result);
  if (error != std::errc{} || end != text->data() + text->size()) return {};
  return result;
}

std::optional<std::int64_t> integer(std::string_view text) {
  std::int64_t result{};
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), result);
  if (error != std::errc{} || end != text.data() + text.size()) return {};
  return result;
}

std::optional<std::int64_t> isoUtcMilliseconds(const std::string_view text) {
  if (text.size() != 20 || text[4] != '-' || text[7] != '-' ||
      text[10] != 'T' || text[13] != ':' || text[16] != ':' || text[19] != 'Z')
    return {};
  const auto part = [&](const std::size_t start,
                        const std::size_t length) -> std::optional<int> {
    int value{};
    const auto [end, error] = std::from_chars(
        text.data() + start, text.data() + start + length, value);
    if (error != std::errc{} || end != text.data() + start + length) return {};
    return value;
  };
  const auto year   = part(0, 4);
  const auto month  = part(5, 2);
  const auto day    = part(8, 2);
  const auto hour   = part(11, 2);
  const auto minute = part(14, 2);
  const auto second = part(17, 2);
  if (!year || !month || !day || !hour || !minute || !second || *hour > 23 ||
      *minute > 59 || *second > 59)
    return {};
  const std::chrono::year_month_day date{
      std::chrono::year{*year},
      std::chrono::month{static_cast<unsigned>(*month)},
      std::chrono::day{static_cast<unsigned>(*day)}};
  if (!date.ok()) return {};
  const auto days = std::chrono::sys_days{date}.time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(days).count() +
         static_cast<std::int64_t>(*hour) * 3600000 +
         static_cast<std::int64_t>(*minute) * 60000 +
         static_cast<std::int64_t>(*second) * 1000;
}

struct SelectedValue {
  RecordId id{};
  vql::WindowTimestamp timestamp;
  vql::RelationKey key;
};

std::expected<SelectedValue, WindowCompositionError>
readSelectedValue(const EvidenceCase &evidence, const RecordMap &records,
                  const RecordId valueId, const std::size_t maximumBytes,
                  std::size_t &usedBytes, const bool asTimestamp,
                  const std::optional<std::int64_t> arrivalWatermark = {}) {
  const auto found = records.find(valueId);
  if (found == records.end() || found->second.kind != "value")
    return std::unexpected(
        failure(WindowCompositionErrorCode::InvalidRetainedRow,
                "window value witness is not a retained value"));
  const auto state   = attribute(found->second, "value-state");
  const auto storage = attribute(found->second, "storage-class");
  if (!state || !storage)
    return std::unexpected(
        failure(WindowCompositionErrorCode::InvalidRetainedRow,
                "window value lacks retained storage metadata"));
  SelectedValue result{.id = valueId};
  if (*state == "missing" || *state == "null") {
    result.timestamp.state = vql::WindowTimestampState::Missing;
    result.key.state       = *state == "null" ? vql::RelationKeyState::Null
                                              : vql::RelationKeyState::Missing;
    return result;
  }
  if (*state != "present" || (*storage != "integer" && *storage != "real" &&
                              *storage != "text" && *storage != "blob"))
    return std::unexpected(
        failure(WindowCompositionErrorCode::InvalidRetainedRow,
                "window value has invalid retained storage metadata"));
  const auto remaining = maximumBytes - std::min(usedBytes, maximumBytes);
  std::string bytes;
  try {
    bytes = evidence.rawText(valueId, remaining);
  } catch (const std::length_error &) {
    return std::unexpected(
        failure(WindowCompositionErrorCode::KeyByteLimitExceeded,
                "window retained scalar byte limit exceeded"));
  }
  if (!addWithin(usedBytes, bytes.size(), maximumBytes))
    return std::unexpected(
        failure(WindowCompositionErrorCode::KeyByteLimitExceeded,
                "window retained scalar byte limit exceeded"));
  if (asTimestamp) {
    const auto parsed = *storage == "integer" || *storage == "text"
                            ? integer(bytes)
                            : std::optional<std::int64_t>{};
    result.timestamp =
        parsed
            ? vql::WindowTimestamp{.state = vql::WindowTimestampState::Valid,
                                   .milliseconds       = *parsed,
                                   .watermarkAtArrival = arrivalWatermark}
            : vql::WindowTimestamp{.state = vql::WindowTimestampState::Invalid,
                                   .watermarkAtArrival = arrivalWatermark};
  } else {
    std::string canonical;
    canonical.reserve(bytes.size() + 9);
    canonical.push_back(storage->front());
    const auto length = static_cast<std::uint64_t>(bytes.size());
    for (int shift = 56; shift >= 0; shift -= 8)
      canonical.push_back(static_cast<char>((length >> shift) & 0xff));
    canonical += bytes;
    result.key = {.state          = vql::RelationKeyState::Present,
                  .canonicalBytes = std::move(canonical)};
  }
  return result;
}

template <typename T> std::string number(const T value) {
  static_assert(std::is_integral_v<T>);
  return std::to_string(value);
}

std::string_view
timestampStateName(const vql::WindowTimestampState state) noexcept {
  switch (state) {
  case vql::WindowTimestampState::Valid:
    return "valid";
  case vql::WindowTimestampState::Missing:
    return "missing";
  case vql::WindowTimestampState::Invalid:
    return "invalid";
  }
  return "invalid";
}

} // namespace

std::expected<PreparedObservationWindow, WindowCompositionError>
prepareSqliteObservationWindow(const EvidenceCase &evidence,
                               const SqliteObservationWindowRequest &request) {
  if (!evidence.isOpen() || request.observation == 0 ||
      request.startInclusive >= request.endExclusive ||
      request.maxInputOccurrences == 0 || request.maxSourceGaps == 0 ||
      request.maxSourceGapBytes == 0 || request.maxKeyBytes == 0 ||
      request.maxOutputOccurrences == 0 || request.maxWork == 0 ||
      request.maxRetainedRecords == 0 || request.maxMetadataBytes == 0 ||
      (request.timeBasis == vql::WindowTimeBasis::EventTime &&
       request.timeColumn.empty()))
    return std::unexpected(failure(
        WindowCompositionErrorCode::InvalidRequest,
        "SQLite observation window request is incomplete or unbounded"));
  auto scanned =
      boundedRecords(evidence, request.maxScanRecords, request.maxScanBytes);
  if (!scanned) return std::unexpected(scanned.error());
  auto records             = std::move(*scanned);
  auto children            = childIndex(records);
  const auto observationIt = records.find(request.observation);
  if (observationIt == records.end() ||
      observationIt->second.kind != "observation")
    return std::unexpected(
        failure(WindowCompositionErrorCode::InvalidObservation,
                "selected observation is not retained in this case"));
  const auto sourceIt = observationIt->second.parent
                            ? records.find(*observationIt->second.parent)
                            : records.end();
  if (sourceIt == records.end() || sourceIt->second.kind != "connection" ||
      attribute(sourceIt->second, "source-kind") !=
          std::optional<std::string>("sqlite"))
    return std::unexpected(
        failure(WindowCompositionErrorCode::WrongSourceKind,
                "selected observation is not from a retained SQLite source"));

  std::optional<RecordId> timeField;
  std::optional<RecordId> arrivalField;
  std::optional<RecordId> keyField;
  for (const auto id : childrenOf(children, request.observation)) {
    const auto &record = records.at(id);
    if (record.kind != "field") continue;
    if (request.timeBasis == vql::WindowTimeBasis::EventTime &&
        record.label == request.timeColumn) {
      if (timeField)
        return std::unexpected(
            failure(WindowCompositionErrorCode::AmbiguousTimeColumn,
                    "selected timestamp column has duplicate field witnesses"));
      timeField = id;
    }
    if (request.arrivalWatermarkColumn &&
        record.label == *request.arrivalWatermarkColumn) {
      if (arrivalField)
        return std::unexpected(
            failure(WindowCompositionErrorCode::AmbiguousTimeColumn,
                    "arrival watermark column has duplicate field witnesses"));
      arrivalField = id;
    }
    if (request.keyColumn && record.label == *request.keyColumn) {
      if (keyField)
        return std::unexpected(
            failure(WindowCompositionErrorCode::AmbiguousTimeColumn,
                    "selected key column has duplicate field witnesses"));
      keyField = id;
    }
  }
  if (request.timeBasis == vql::WindowTimeBasis::EventTime && !timeField)
    return std::unexpected(
        failure(WindowCompositionErrorCode::MissingTimeColumn,
                "selected event-time column was not retained"));
  if (request.arrivalWatermarkColumn && !arrivalField)
    return std::unexpected(
        failure(WindowCompositionErrorCode::MissingTimeColumn,
                "selected arrival watermark column was not retained"));
  if (request.keyColumn && !keyField)
    return std::unexpected(
        failure(WindowCompositionErrorCode::MissingTimeColumn,
                "selected window key column was not retained"));

  auto rows = std::vector<RecordId>{};
  for (const auto id : childrenOf(children, request.observation))
    if (records.at(id).kind == "row") rows.push_back(id);
  for (const auto id : rows)
    if (!position(records.at(id)))
      return std::unexpected(
          failure(WindowCompositionErrorCode::InvalidRetainedRow,
                  "SQLite row has an invalid retained result position"));
  std::ranges::sort(rows, [&](const RecordId left, const RecordId right) {
    return *position(records.at(left)) < *position(records.at(right));
  });
  if (rows.size() > request.maxInputOccurrences)
    return std::unexpected(failure(WindowCompositionErrorCode::InvalidRequest,
                                   "window input occurrence limit exceeded"));

  const auto observationStartedAt =
      attribute(observationIt->second, "started-at");
  const auto observationTime = observationStartedAt
                                   ? isoUtcMilliseconds(*observationStartedAt)
                                   : std::optional<std::int64_t>{};
  const auto captureState = attribute(observationIt->second, "capture-state");
  auto coverage           = vql::RelationInputCoverage::Gapped;
  if (captureState == std::optional<std::string>("complete"))
    coverage = vql::RelationInputCoverage::Complete;
  else if (captureState == std::optional<std::string>("partial"))
    coverage = vql::RelationInputCoverage::Partial;
  else if (captureState != std::optional<std::string>("failed") &&
           captureState != std::optional<std::string>("cancelled"))
    return std::unexpected(
        failure(WindowCompositionErrorCode::InvalidObservation,
                "SQLite observation has an unknown capture state"));
  const auto coverageDescription =
      attribute(observationIt->second, "coverage").value_or("");

  ObservationWindowEvaluation evaluation;
  evaluation.request = {.timeBasis            = request.timeBasis,
                        .startInclusive       = request.startInclusive,
                        .endExclusive         = request.endExclusive,
                        .watermark            = request.watermark,
                        .latePolicy           = request.latePolicy,
                        .inputCoverage        = coverage,
                        .maxSourceGaps        = request.maxSourceGaps,
                        .maxSourceGapBytes    = request.maxSourceGapBytes,
                        .maxInputOccurrences  = request.maxInputOccurrences,
                        .maxKeyBytes          = request.maxKeyBytes,
                        .maxOutputOccurrences = request.maxOutputOccurrences,
                        .maxWork              = request.maxWork};
  ObservationWindowLineage lineage{
      .caseDocumentId         = evidence.store().documentId().str(),
      .observation            = request.observation,
      .timeColumn             = request.timeColumn,
      .timeField              = timeField,
      .arrivalWatermarkColumn = request.arrivalWatermarkColumn,
      .arrivalWatermarkField  = arrivalField,
      .keyColumn              = request.keyColumn,
      .keyField               = keyField,
      .observationTimeWitness = observationStartedAt.value_or("")};
  std::size_t sourceGapBytes{};
  const auto addSourceGap = [&](const RecordId witness, std::string gap) {
    if (lineage.sourceGaps.size() >= request.maxSourceGaps) return false;
    if (gap.size() > request.maxSourceGapBytes - sourceGapBytes) return false;
    sourceGapBytes += gap.size();
    lineage.sourceGapRecords.push_back(witness);
    lineage.sourceGaps.push_back(std::move(gap));
    return true;
  };
  if (coverage != vql::RelationInputCoverage::Complete &&
      !coverageDescription.empty()) {
    const auto coverageGap = "observation coverage: " + coverageDescription;
    if (!addSourceGap(request.observation, coverageGap))
      return std::unexpected(
          failure(lineage.sourceGaps.size() >= request.maxSourceGaps
                      ? WindowCompositionErrorCode::SourceGapLimitExceeded
                      : WindowCompositionErrorCode::SourceGapByteLimitExceeded,
                  "window source-gap budget exceeded"));
  }
  std::size_t aggregateBytes{};
  evaluation.occurrences.reserve(rows.size());
  for (const auto rowId : rows) {
    std::optional<RecordId> timeValue;
    std::optional<RecordId> arrivalValue;
    std::optional<RecordId> keyValue;
    std::size_t timeMatches{};
    std::size_t arrivalMatches{};
    std::size_t keyMatches{};
    for (const auto id : childrenOf(children, rowId)) {
      const auto &value = records.at(id);
      if (value.kind != "value") continue;
      const auto fieldReference = [&] {
        for (const auto refId : childrenOf(children, id)) {
          const auto &ref = records.at(refId);
          if (ref.kind == "reference" && ref.label == "field occurrence")
            return ref.text;
        }
        return std::string{};
      }();
      if (timeField && fieldReference == number(*timeField)) {
        timeValue = id;
        ++timeMatches;
      }
      if (arrivalField && fieldReference == number(*arrivalField)) {
        arrivalValue = id;
        ++arrivalMatches;
      }
      if (keyField && fieldReference == number(*keyField)) {
        keyValue = id;
        ++keyMatches;
      }
    }
    if ((timeField && timeMatches != 1) ||
        (arrivalField && arrivalMatches > 1) || (keyField && keyMatches != 1))
      return std::unexpected(failure(
          WindowCompositionErrorCode::InvalidRetainedRow,
          "SQLite row has ambiguous or missing selected value witnesses"));

    vql::WindowTimestamp timestamp;
    if (request.timeBasis == vql::WindowTimeBasis::ObservationTime) {
      if (!observationStartedAt) {
        timestamp.state = vql::WindowTimestampState::Missing;
      } else if (!observationTime) {
        timestamp.state = vql::WindowTimestampState::Invalid;
      } else {
        timestamp.state        = vql::WindowTimestampState::Valid;
        timestamp.milliseconds = *observationTime;
      }
      timestamp.watermarkAtArrival =
          attribute(observationIt->second, "watermark-at-arrival")
              ? integer(
                    *attribute(observationIt->second, "watermark-at-arrival"))
              : std::optional<std::int64_t>{};
      if (observationStartedAt) timeValue = request.observation;
    } else {
      const auto selected =
          readSelectedValue(evidence, records, *timeValue, request.maxKeyBytes,
                            aggregateBytes, true);
      if (!selected) return std::unexpected(selected.error());
      timestamp = selected->timestamp;
      if (arrivalValue) {
        const auto arrival =
            readSelectedValue(evidence, records, *arrivalValue,
                              request.maxKeyBytes, aggregateBytes, true);
        if (!arrival) return std::unexpected(arrival.error());
        if (arrival->timestamp.state == vql::WindowTimestampState::Valid)
          timestamp.watermarkAtArrival = arrival->timestamp.milliseconds;
      }
    }
    vql::RelationKey key;
    if (keyValue) {
      const auto selected =
          readSelectedValue(evidence, records, *keyValue, request.maxKeyBytes,
                            aggregateBytes, false);
      if (!selected) return std::unexpected(selected.error());
      key = selected->key;
    }
    evaluation.occurrences.push_back({.timestamp = timestamp, .key = key});
    lineage.rows.push_back(rowId);
    lineage.timeValues.push_back(timeValue);
    lineage.arrivalWatermarkValues.push_back(arrivalValue);
    lineage.keyValues.push_back(keyValue);
  }

  for (const auto id : childrenOf(children, request.observation)) {
    const auto &record   = records.at(id);
    const auto pageState = attribute(record, "state");
    if (record.kind != "page" ||
        pageState == std::optional<std::string>("complete") ||
        pageState == std::optional<std::string>("succeeded"))
      continue;
    const auto gap = "page " + record.label + " " +
                     attribute(record, "state").value_or("unknown") + ": " +
                     attribute(record, "error").value_or("unspecified");
    if (!addSourceGap(id, gap))
      return std::unexpected(
          failure(lineage.sourceGaps.size() >= request.maxSourceGaps
                      ? WindowCompositionErrorCode::SourceGapLimitExceeded
                      : WindowCompositionErrorCode::SourceGapByteLimitExceeded,
                  "window source-gap budget exceeded"));
  }
  evaluation.request.sourceGaps = lineage.sourceGaps;
  PreparedObservationWindow prepared{
      .evaluation         = std::move(evaluation),
      .lineage            = std::move(lineage),
      .maxScanRecords     = request.maxScanRecords,
      .maxScanBytes       = request.maxScanBytes,
      .maxRetainedRecords = request.maxRetainedRecords,
      .maxMetadataBytes   = request.maxMetadataBytes};
  return prepared;
}

std::expected<vql::WindowResult, WindowCompositionError>
evaluateObservationWindow(const ObservationWindowEvaluation &evaluation) {
  auto result =
      vql::windowOccurrences(evaluation.request, evaluation.occurrences);
  if (!result) {
    const auto code =
        result.error().code == vql::WindowErrorCode::LateDataRejected
            ? WindowCompositionErrorCode::LateDataRejected
            : WindowCompositionErrorCode::KernelFailure;
    return std::unexpected(
        failure(code, result.error().message, result.error().occurrence));
  }
  return *result;
}

std::expected<ObservationWindowRetention, WindowCompositionError>
retainObservationWindow(EvidenceCase &evidence,
                        const PreparedObservationWindow &prepared,
                        const vql::WindowResult &result) {
  const auto &lineage = prepared.lineage;
  if (!evidence.isOpen() ||
      evidence.store().documentId().str() != lineage.caseDocumentId)
    return std::unexpected(
        failure(WindowCompositionErrorCode::DifferentCase,
                "prepared observation window belongs to another case"));
  SqliteObservationWindowRequest request{
      .observation            = lineage.observation,
      .timeBasis              = prepared.evaluation.request.timeBasis,
      .timeColumn             = lineage.timeColumn,
      .arrivalWatermarkColumn = lineage.arrivalWatermarkColumn,
      .keyColumn              = lineage.keyColumn,
      .startInclusive         = prepared.evaluation.request.startInclusive,
      .endExclusive           = prepared.evaluation.request.endExclusive,
      .watermark              = prepared.evaluation.request.watermark,
      .latePolicy             = prepared.evaluation.request.latePolicy,
      .maxInputOccurrences    = prepared.evaluation.request.maxInputOccurrences,
      .maxSourceGaps          = prepared.evaluation.request.maxSourceGaps,
      .maxSourceGapBytes      = prepared.evaluation.request.maxSourceGapBytes,
      .maxKeyBytes            = prepared.evaluation.request.maxKeyBytes,
      .maxOutputOccurrences = prepared.evaluation.request.maxOutputOccurrences,
      .maxWork              = prepared.evaluation.request.maxWork,
      .maxScanRecords       = prepared.maxScanRecords,
      .maxScanBytes         = prepared.maxScanBytes,
      .maxRetainedRecords   = prepared.maxRetainedRecords,
      .maxMetadataBytes     = prepared.maxMetadataBytes};
  const auto current = prepareSqliteObservationWindow(evidence, request);
  if (!current || current->lineage.rows != lineage.rows ||
      current->lineage.timeField != lineage.timeField ||
      current->lineage.arrivalWatermarkField != lineage.arrivalWatermarkField ||
      current->lineage.keyField != lineage.keyField ||
      current->lineage.timeValues != lineage.timeValues ||
      current->lineage.arrivalWatermarkValues !=
          lineage.arrivalWatermarkValues ||
      current->lineage.keyValues != lineage.keyValues ||
      current->lineage.sourceGapRecords != lineage.sourceGapRecords ||
      current->lineage.sourceGaps != lineage.sourceGaps ||
      current->lineage.observationTimeWitness !=
          lineage.observationTimeWitness ||
      current->evaluation.occurrences != prepared.evaluation.occurrences)
    return std::unexpected(
        failure(WindowCompositionErrorCode::InvalidEvaluation,
                "prepared window lineage no longer matches retained records"));
  const auto checked = evaluateObservationWindow(prepared.evaluation);
  if (!checked || checked->outcomes != result.outcomes ||
      checked->inputCoverage != result.inputCoverage ||
      checked->sourceGaps != result.sourceGaps ||
      checked->finalized != result.finalized ||
      checked->policyGap != result.policyGap ||
      checked->truncated != result.truncated ||
      checked->computationGap != result.computationGap ||
      checked->work != result.work)
    return std::unexpected(failure(
        WindowCompositionErrorCode::InvalidEvaluation,
        "window result does not match its prepared inputs and policies"));
  if (lineage.rows.size() != prepared.evaluation.occurrences.size() ||
      lineage.timeValues.size() != lineage.rows.size() ||
      lineage.arrivalWatermarkValues.size() != lineage.rows.size() ||
      lineage.keyValues.size() != lineage.rows.size() ||
      lineage.sourceGapRecords.size() != lineage.sourceGaps.size())
    return std::unexpected(
        failure(WindowCompositionErrorCode::InvalidEvaluation,
                "prepared observation window lineage is malformed"));

  std::size_t retainedRecords = 5; // run, input/ref, window/observation ref
  for (const auto field :
       {lineage.timeField, lineage.arrivalWatermarkField, lineage.keyField})
    if (field && !addWithin(retainedRecords, 2, prepared.maxRetainedRecords))
      return std::unexpected(
          failure(WindowCompositionErrorCode::RetainedRecordLimitExceeded,
                  "window field witness record limit exceeded"));
  std::size_t metadataBytes{};
  const auto charge = [&](std::string_view text) {
    return addWithin(metadataBytes, text.size(), prepared.maxMetadataBytes);
  };
  if (!charge("finite-bounded-window-membership-v1") ||
      !charge(
          vql::windowTimeBasisName(prepared.evaluation.request.timeBasis)) ||
      !charge(
          vql::windowLatePolicyName(prepared.evaluation.request.latePolicy)) ||
      !charge(vql::relationCoverageName(result.inputCoverage)) ||
      !charge(lineage.timeColumn) ||
      !charge(lineage.arrivalWatermarkColumn.value_or("")) ||
      !charge(lineage.keyColumn.value_or("")) ||
      !charge(result.policyGap.value_or("")) ||
      !charge(result.computationGap.value_or("")))
    return std::unexpected(
        failure(WindowCompositionErrorCode::MetadataLimitExceeded,
                "window metadata byte limit exceeded"));
  for (const auto value : {prepared.evaluation.request.startInclusive,
                           prepared.evaluation.request.endExclusive,
                           prepared.evaluation.request.watermark})
    if (!charge(number(value)))
      return std::unexpected(
          failure(WindowCompositionErrorCode::MetadataLimitExceeded,
                  "window metadata byte limit exceeded"));
  for (const auto value :
       {prepared.evaluation.request.maxInputOccurrences,
        prepared.evaluation.request.maxSourceGaps,
        prepared.evaluation.request.maxSourceGapBytes,
        prepared.evaluation.request.maxKeyBytes,
        prepared.evaluation.request.maxOutputOccurrences,
        prepared.evaluation.request.maxWork, prepared.maxScanRecords,
        prepared.maxScanBytes, prepared.maxRetainedRecords,
        prepared.maxMetadataBytes})
    if (!charge(number(value)))
      return std::unexpected(
          failure(WindowCompositionErrorCode::MetadataLimitExceeded,
                  "window resource policy metadata byte limit exceeded"));
  for (const auto &gap : result.sourceGaps)
    if (!charge(gap))
      return std::unexpected(
          failure(WindowCompositionErrorCode::MetadataLimitExceeded,
                  "window source gap metadata byte limit exceeded"));
  for (const auto &outcome : result.outcomes) {
    if (outcome.inputOccurrence >= lineage.rows.size())
      return std::unexpected(
          failure(WindowCompositionErrorCode::InvalidEvaluation,
                  "window result occurrence is out of range"));
    const auto index = outcome.inputOccurrence;
    if (!charge(number(index)) || !charge(number(lineage.rows[index])) ||
        !charge("occurrence " + number(index)) ||
        !charge("Occurrence excluded from bounded window") ||
        !charge("Occurrence belongs to bounded window") ||
        !charge("Retained input occurrence row") ||
        !charge("Retained timestamp value witness") ||
        !charge("Retained per-occurrence arrival watermark witness") ||
        !charge("Retained partition key witness") || !charge("input row") ||
        !charge("time value") || !charge("arrival watermark value") ||
        !charge("partition key") || !charge("valid") || !charge("missing") ||
        !charge("invalid") || !charge("late") || !charge("not-late") ||
        !charge("unknown"))
      return std::unexpected(
          failure(WindowCompositionErrorCode::MetadataLimitExceeded,
                  "window occurrence metadata byte limit exceeded"));
    if (outcome.exclusion &&
        !charge(vql::windowExclusionName(*outcome.exclusion)))
      return std::unexpected(
          failure(WindowCompositionErrorCode::MetadataLimitExceeded,
                  "window exclusion metadata byte limit exceeded"));
    if (!addWithin(retainedRecords, 3, prepared.maxRetainedRecords))
      return std::unexpected(
          failure(WindowCompositionErrorCode::RetainedRecordLimitExceeded,
                  "window retained-record limit exceeded"));
    if (lineage.timeValues[index]) {
      if (!charge(number(*lineage.timeValues[index])) ||
          !addWithin(retainedRecords, 2, prepared.maxRetainedRecords))
        return std::unexpected(
            failure(WindowCompositionErrorCode::RetainedRecordLimitExceeded,
                    "window timestamp witness record limit exceeded"));
    }
    if (lineage.arrivalWatermarkValues[index]) {
      if (!charge(number(*lineage.arrivalWatermarkValues[index])))
        return std::unexpected(
            failure(WindowCompositionErrorCode::MetadataLimitExceeded,
                    "window occurrence metadata byte limit exceeded"));
      if (!addWithin(retainedRecords, 2, prepared.maxRetainedRecords))
        return std::unexpected(
            failure(WindowCompositionErrorCode::RetainedRecordLimitExceeded,
                    "window arrival watermark witness record limit exceeded"));
    }
    if (lineage.keyValues[index]) {
      if (!charge(number(*lineage.keyValues[index])) ||
          !charge(lineage.keyColumn.value_or("")))
        return std::unexpected(
            failure(WindowCompositionErrorCode::MetadataLimitExceeded,
                    "window occurrence metadata byte limit exceeded"));
      if (!addWithin(retainedRecords, 2, prepared.maxRetainedRecords))
        return std::unexpected(
            failure(WindowCompositionErrorCode::RetainedRecordLimitExceeded,
                    "window key witness record limit exceeded"));
    }
  }
  for (std::size_t i = 0; i < result.sourceGaps.size(); ++i)
    if (!addWithin(retainedRecords, 2, prepared.maxRetainedRecords))
      return std::unexpected(
          failure(WindowCompositionErrorCode::RetainedRecordLimitExceeded,
                  "window gap record limit exceeded"));
  if (result.policyGap &&
      !addWithin(retainedRecords, 1, prepared.maxRetainedRecords))
    return std::unexpected(
        failure(WindowCompositionErrorCode::RetainedRecordLimitExceeded,
                "window lateness gap record limit exceeded"));
  if (result.computationGap &&
      !addWithin(retainedRecords, 1, prepared.maxRetainedRecords))
    return std::unexpected(
        failure(WindowCompositionErrorCode::RetainedRecordLimitExceeded,
                "window computation gap record limit exceeded"));
  if (retainedRecords > prepared.maxRetainedRecords)
    return std::unexpected(
        failure(WindowCompositionErrorCode::RetainedRecordLimitExceeded,
                "window retained-record limit exceeded"));

  const auto run = evidence.appendRecord(
      "query-run", "Bounded observation window",
      "Finite membership over one retained SQLite observation", {},
      {{"algorithm", "finite-bounded-window-membership-v1"},
       {"time-basis", std::string(vql::windowTimeBasisName(
                          prepared.evaluation.request.timeBasis))},
       {"late-policy", std::string(vql::windowLatePolicyName(
                           prepared.evaluation.request.latePolicy))},
       {"input-coverage",
        std::string(vql::relationCoverageName(result.inputCoverage))},
       {"window-start-inclusive",
        number(prepared.evaluation.request.startInclusive)},
       {"window-end-exclusive",
        number(prepared.evaluation.request.endExclusive)},
       {"watermark", number(prepared.evaluation.request.watermark)},
       {"window-finalized", result.finalized ? "true" : "false"},
       {"lateness-policy-gap", result.policyGap.value_or("")},
       {"truncated", result.truncated ? "true" : "false"},
       {"computation-gap", result.computationGap.value_or("")},
       {"work", number(result.work)},
       {"time-column", lineage.timeColumn},
       {"arrival-watermark-column",
        lineage.arrivalWatermarkColumn.value_or("")},
       {"key-column", lineage.keyColumn.value_or("")},
       {"max-input-occurrences",
        number(prepared.evaluation.request.maxInputOccurrences)},
       {"max-source-gaps", number(prepared.evaluation.request.maxSourceGaps)},
       {"max-source-gap-bytes",
        number(prepared.evaluation.request.maxSourceGapBytes)},
       {"max-key-bytes", number(prepared.evaluation.request.maxKeyBytes)},
       {"max-output-occurrences",
        number(prepared.evaluation.request.maxOutputOccurrences)},
       {"max-work", number(prepared.evaluation.request.maxWork)},
       {"max-scan-records", number(prepared.maxScanRecords)},
       {"max-scan-bytes", number(prepared.maxScanBytes)},
       {"max-retained-records", number(prepared.maxRetainedRecords)},
       {"max-metadata-bytes", number(prepared.maxMetadataBytes)}});
  const auto input = evidence.appendRecord(
      "query-input", "observation", "Pinned retained input observation", run,
      {{"coverage",
        std::string(vql::relationCoverageName(result.inputCoverage))}},
      {{"d.evidence.target", lineage.observation}});
  static_cast<void>(input);
  const auto window = evidence.appendRecord(
      "query-window", "window membership", "Pinned finite window bounds", run,
      {{"time-basis", std::string(vql::windowTimeBasisName(
                          prepared.evaluation.request.timeBasis))},
       {"start-inclusive", number(prepared.evaluation.request.startInclusive)},
       {"end-exclusive", number(prepared.evaluation.request.endExclusive)},
       {"watermark", number(prepared.evaluation.request.watermark)},
       {"finalized", result.finalized ? "true" : "false"}},
      {{"d.evidence.target", lineage.observation}});
  if (lineage.timeField)
    static_cast<void>(evidence.appendRecord(
        "query-field-witness", "time field", "Pinned timestamp field", window,
        {}, {{"d.evidence.target", *lineage.timeField}}));
  if (lineage.arrivalWatermarkField)
    static_cast<void>(evidence.appendRecord(
        "query-field-witness", "arrival watermark field",
        "Pinned arrival watermark field", window, {},
        {{"d.evidence.target", *lineage.arrivalWatermarkField}}));
  if (lineage.keyField)
    static_cast<void>(evidence.appendRecord(
        "query-field-witness", "key field", "Pinned partition key field",
        window, {}, {{"d.evidence.target", *lineage.keyField}}));

  ObservationWindowRetention retention{.queryRun = run, .window = window};
  for (const auto &outcome : result.outcomes) {
    const auto index = outcome.inputOccurrence;
    RecordId outcomeId{};
    if (outcome.exclusion) {
      outcomeId = evidence.appendRecord(
          "query-exclusion", "occurrence " + number(index),
          "Occurrence excluded from bounded window", window,
          {{"reason",
            std::string(vql::windowExclusionName(*outcome.exclusion))},
           {"timestamp-state",
            std::string(timestampStateName(
                prepared.evaluation.occurrences[index].timestamp.state))},
           {"late-state",
            outcome.lateState == vql::WindowLateState::Late      ? "late"
            : outcome.lateState == vql::WindowLateState::NotLate ? "not-late"
                                                                 : "unknown"}},
          {});
      retention.exclusions.push_back(outcomeId);
    } else {
      outcomeId = evidence.appendRecord(
          "query-member", "occurrence " + number(index),
          "Occurrence belongs to bounded window", window,
          {{"input-position", number(index)},
           {"timestamp-state",
            std::string(timestampStateName(
                prepared.evaluation.occurrences[index].timestamp.state))},
           {"late-state",
            outcome.lateState == vql::WindowLateState::Late      ? "late"
            : outcome.lateState == vql::WindowLateState::NotLate ? "not-late"
                                                                 : "unknown"}},
          {});
      retention.members.push_back(outcomeId);
    }
    static_cast<void>(evidence.appendRecord(
        "query-lineage", "input row", "Retained input occurrence row",
        outcomeId, {}, {{"d.evidence.target", lineage.rows[index]}}));
    if (lineage.timeValues[index])
      static_cast<void>(evidence.appendRecord(
          "query-lineage", "time value", "Retained timestamp value witness",
          outcomeId, {}, {{"d.evidence.target", *lineage.timeValues[index]}}));
    if (lineage.arrivalWatermarkValues[index])
      static_cast<void>(evidence.appendRecord(
          "query-lineage", "arrival watermark value",
          "Retained per-occurrence arrival watermark witness", outcomeId, {},
          {{"d.evidence.target", *lineage.arrivalWatermarkValues[index]}}));
    if (lineage.keyValues[index])
      static_cast<void>(evidence.appendRecord(
          "query-key-witness", "partition key", lineage.keyColumn.value_or(""),
          outcomeId, {}, {{"d.evidence.target", *lineage.keyValues[index]}}));
  }
  for (std::size_t i = 0; i < result.sourceGaps.size(); ++i) {
    const auto id = evidence.appendRecord(
        "query-gap", "source gap " + number(i), result.sourceGaps[i], window,
        {{"scope", "source-coverage"}},
        {{"d.evidence.target", lineage.sourceGapRecords[i]}});
    retention.gaps.push_back(id);
  }
  if (result.policyGap) {
    retention.gaps.push_back(evidence.appendRecord(
        "query-gap", "lateness uncertainty", *result.policyGap, window,
        {{"scope", "late-data-policy"}}));
  }
  if (result.computationGap) {
    retention.gaps.push_back(evidence.appendRecord(
        "query-gap", "computation gap", *result.computationGap, window,
        {{"scope", "computation-truncation"}}));
  }
  return retention;
}

std::expected<RejectedObservationWindowRetention, WindowCompositionError>
retainRejectedObservationWindow(EvidenceCase &evidence,
                                const PreparedObservationWindow &prepared,
                                const WindowCompositionError &rejection) {
  const auto &lineage = prepared.lineage;
  if (!evidence.isOpen() ||
      evidence.store().documentId().str() != lineage.caseDocumentId)
    return std::unexpected(
        failure(WindowCompositionErrorCode::DifferentCase,
                "prepared observation window belongs to another case"));
  if (rejection.code != WindowCompositionErrorCode::LateDataRejected ||
      !rejection.occurrence)
    return std::unexpected(
        failure(WindowCompositionErrorCode::InvalidEvaluation,
                "window rejection is not a typed late-data result"));

  SqliteObservationWindowRequest request{
      .observation            = lineage.observation,
      .timeBasis              = prepared.evaluation.request.timeBasis,
      .timeColumn             = lineage.timeColumn,
      .arrivalWatermarkColumn = lineage.arrivalWatermarkColumn,
      .keyColumn              = lineage.keyColumn,
      .startInclusive         = prepared.evaluation.request.startInclusive,
      .endExclusive           = prepared.evaluation.request.endExclusive,
      .watermark              = prepared.evaluation.request.watermark,
      .latePolicy             = prepared.evaluation.request.latePolicy,
      .maxInputOccurrences    = prepared.evaluation.request.maxInputOccurrences,
      .maxSourceGaps          = prepared.evaluation.request.maxSourceGaps,
      .maxSourceGapBytes      = prepared.evaluation.request.maxSourceGapBytes,
      .maxKeyBytes            = prepared.evaluation.request.maxKeyBytes,
      .maxOutputOccurrences = prepared.evaluation.request.maxOutputOccurrences,
      .maxWork              = prepared.evaluation.request.maxWork,
      .maxScanRecords       = prepared.maxScanRecords,
      .maxScanBytes         = prepared.maxScanBytes,
      .maxRetainedRecords   = prepared.maxRetainedRecords,
      .maxMetadataBytes     = prepared.maxMetadataBytes};
  const auto current = prepareSqliteObservationWindow(evidence, request);
  if (!current || current->lineage.rows != lineage.rows ||
      current->lineage.timeField != lineage.timeField ||
      current->lineage.arrivalWatermarkField != lineage.arrivalWatermarkField ||
      current->lineage.keyField != lineage.keyField ||
      current->lineage.timeValues != lineage.timeValues ||
      current->lineage.arrivalWatermarkValues !=
          lineage.arrivalWatermarkValues ||
      current->lineage.keyValues != lineage.keyValues ||
      current->lineage.sourceGapRecords != lineage.sourceGapRecords ||
      current->lineage.observationTimeWitness !=
          lineage.observationTimeWitness ||
      current->evaluation.occurrences != prepared.evaluation.occurrences)
    return std::unexpected(
        failure(WindowCompositionErrorCode::InvalidEvaluation,
                "prepared window lineage no longer matches retained records"));

  const auto evaluated = vql::windowOccurrences(
      prepared.evaluation.request, prepared.evaluation.occurrences);
  if (evaluated ||
      evaluated.error().code != vql::WindowErrorCode::LateDataRejected ||
      evaluated.error().occurrence != rejection.occurrence ||
      *rejection.occurrence >= lineage.rows.size())
    return std::unexpected(
        failure(WindowCompositionErrorCode::InvalidEvaluation,
                "late-data rejection does not match the prepared inputs"));
  const auto index       = *rejection.occurrence;
  const auto &occurrence = prepared.evaluation.occurrences[index];
  if (occurrence.timestamp.state != vql::WindowTimestampState::Valid ||
      !occurrence.timestamp.watermarkAtArrival ||
      occurrence.timestamp.milliseconds <
          prepared.evaluation.request.startInclusive ||
      occurrence.timestamp.milliseconds >=
          prepared.evaluation.request.endExclusive ||
      occurrence.timestamp.milliseconds >=
          *occurrence.timestamp.watermarkAtArrival ||
      !lineage.timeValues[index] || !lineage.arrivalWatermarkValues[index])
    return std::unexpected(
        failure(WindowCompositionErrorCode::InvalidEvaluation,
                "rejected occurrence has no valid timestamp witnesses"));

  if (lineage.sourceGapRecords.size() != lineage.sourceGaps.size())
    return std::unexpected(
        failure(WindowCompositionErrorCode::InvalidEvaluation,
                "prepared source gap lineage is malformed"));
  std::size_t retainedRecords = 6; // run, input/ref, window/ref, exclusion
  std::size_t metadataBytes{};
  const auto charge = [&](std::string_view text) {
    return addWithin(metadataBytes, text.size(), prepared.maxMetadataBytes);
  };
  for (const auto field :
       {lineage.timeField, lineage.arrivalWatermarkField, lineage.keyField})
    if (field && !addWithin(retainedRecords, 2, prepared.maxRetainedRecords))
      return std::unexpected(
          failure(WindowCompositionErrorCode::RetainedRecordLimitExceeded,
                  "window field witness record limit exceeded"));
  if (!addWithin(retainedRecords, 6, prepared.maxRetainedRecords))
    return std::unexpected(
        failure(WindowCompositionErrorCode::RetainedRecordLimitExceeded,
                "rejected occurrence witness record limit exceeded"));
  if (!charge("finite-bounded-window-membership-v1") ||
      !charge("late-data-rejected") || !charge("query-run") ||
      !charge("Bounded observation window") ||
      !charge("Rejected bounded observation window") ||
      !charge("Window evaluation rejected a late occurrence") ||
      !charge("run-state") || !charge("rejected") || !charge("failure-code") ||
      !charge("rejected-occurrence") || !charge("time-basis") ||
      !charge(
          vql::windowTimeBasisName(prepared.evaluation.request.timeBasis)) ||
      !charge("late-policy") || !charge("reject") ||
      !charge("input-coverage") ||
      !charge(vql::relationCoverageName(
          prepared.evaluation.request.inputCoverage)) ||
      !charge("Pinned retained input observation") || !charge("coverage") ||
      !charge("query-input") || !charge("query-window") ||
      !charge("window membership") || !charge("Pinned finite window bounds") ||
      !charge("start-inclusive") || !charge("end-exclusive") ||
      !charge("watermark") || !charge("finalized") || !charge("true") ||
      !charge("false") || !charge("query-exclusion") ||
      !charge("occurrence " + number(index)) ||
      !charge("Occurrence excluded by the reject late-data policy") ||
      !charge("reason") || !charge("timestamp-state") ||
      !charge("late-state") || !charge("late-data") || !charge("valid") ||
      !charge("late") || !charge("failure-code") ||
      !charge("d.evidence.target") || !charge(lineage.timeColumn) ||
      !charge(lineage.arrivalWatermarkColumn.value_or("")) ||
      !charge(lineage.keyColumn.value_or("")) ||
      !charge(number(lineage.observation)) ||
      !charge(number(lineage.rows[index])) ||
      !charge(number(*lineage.timeValues[index])) ||
      !charge(number(*lineage.arrivalWatermarkValues[index])))
    return std::unexpected(
        failure(WindowCompositionErrorCode::MetadataLimitExceeded,
                "rejected window metadata byte limit exceeded"));
  for (const auto field :
       {lineage.timeField, lineage.arrivalWatermarkField, lineage.keyField})
    if (field && !charge(number(*field)))
      return std::unexpected(
          failure(WindowCompositionErrorCode::MetadataLimitExceeded,
                  "rejected window field metadata byte limit exceeded"));
  if (lineage.keyValues[index] && (!charge(number(*lineage.keyValues[index])) ||
                                   !charge(lineage.keyColumn.value_or(""))))
    return std::unexpected(
        failure(WindowCompositionErrorCode::MetadataLimitExceeded,
                "rejected window key metadata byte limit exceeded"));
  for (const auto value : {prepared.evaluation.request.startInclusive,
                           prepared.evaluation.request.endExclusive,
                           prepared.evaluation.request.watermark})
    if (!charge(number(value)))
      return std::unexpected(
          failure(WindowCompositionErrorCode::MetadataLimitExceeded,
                  "rejected window bounds exceed metadata budget"));
  for (const auto &gap : prepared.evaluation.request.sourceGaps)
    if (!charge(gap))
      return std::unexpected(
          failure(WindowCompositionErrorCode::MetadataLimitExceeded,
                  "rejected window source gaps exceed metadata budget"));
  for (std::size_t gap = 0; gap < prepared.evaluation.request.sourceGaps.size();
       ++gap)
    if (!charge("source gap " + number(gap)) || !charge("source-coverage") ||
        !charge(number(lineage.sourceGapRecords[gap])))
      return std::unexpected(
          failure(WindowCompositionErrorCode::MetadataLimitExceeded,
                  "rejected window gap metadata exceeds metadata budget"));
  for (const auto value :
       {prepared.evaluation.request.maxInputOccurrences,
        prepared.evaluation.request.maxSourceGaps,
        prepared.evaluation.request.maxSourceGapBytes,
        prepared.evaluation.request.maxKeyBytes,
        prepared.evaluation.request.maxOutputOccurrences,
        prepared.evaluation.request.maxWork, prepared.maxScanRecords,
        prepared.maxScanBytes, prepared.maxRetainedRecords,
        prepared.maxMetadataBytes})
    if (!charge(number(value)))
      return std::unexpected(
          failure(WindowCompositionErrorCode::MetadataLimitExceeded,
                  "rejected window resource policy exceeds metadata budget"));
  if (lineage.keyValues[index] &&
      !addWithin(retainedRecords, 2, prepared.maxRetainedRecords))
    return std::unexpected(
        failure(WindowCompositionErrorCode::RetainedRecordLimitExceeded,
                "rejected window key witness record limit exceeded"));
  for (std::size_t gap = 0; gap < prepared.evaluation.request.sourceGaps.size();
       ++gap)
    if (!addWithin(retainedRecords, 2, prepared.maxRetainedRecords))
      return std::unexpected(
          failure(WindowCompositionErrorCode::RetainedRecordLimitExceeded,
                  "rejected window gap record limit exceeded"));

  const auto run = evidence.appendRecord(
      "query-run", "Rejected bounded observation window",
      "Window evaluation rejected a late occurrence", {},
      {{"algorithm", "finite-bounded-window-membership-v1"},
       {"run-state", "rejected"},
       {"failure-code", "late-data-rejected"},
       {"rejected-occurrence", number(index)},
       {"time-basis", std::string(vql::windowTimeBasisName(
                          prepared.evaluation.request.timeBasis))},
       {"late-policy", "reject"},
       {"input-coverage", std::string(vql::relationCoverageName(
                              prepared.evaluation.request.inputCoverage))},
       {"time-column", lineage.timeColumn},
       {"arrival-watermark-column",
        lineage.arrivalWatermarkColumn.value_or("")},
       {"key-column", lineage.keyColumn.value_or("")},
       {"max-input-occurrences",
        number(prepared.evaluation.request.maxInputOccurrences)},
       {"max-source-gaps", number(prepared.evaluation.request.maxSourceGaps)},
       {"max-source-gap-bytes",
        number(prepared.evaluation.request.maxSourceGapBytes)},
       {"max-key-bytes", number(prepared.evaluation.request.maxKeyBytes)},
       {"max-output-occurrences",
        number(prepared.evaluation.request.maxOutputOccurrences)},
       {"max-work", number(prepared.evaluation.request.maxWork)},
       {"max-scan-records", number(prepared.maxScanRecords)},
       {"max-scan-bytes", number(prepared.maxScanBytes)},
       {"max-retained-records", number(prepared.maxRetainedRecords)},
       {"max-metadata-bytes", number(prepared.maxMetadataBytes)}});
  static_cast<void>(evidence.appendRecord(
      "query-input", "observation", "Pinned retained input observation", run,
      {{"coverage", std::string(vql::relationCoverageName(
                        prepared.evaluation.request.inputCoverage))}},
      {{"d.evidence.target", lineage.observation}}));
  const auto window = evidence.appendRecord(
      "query-window", "window membership", "Pinned finite window bounds", run,
      {{"start-inclusive", number(prepared.evaluation.request.startInclusive)},
       {"end-exclusive", number(prepared.evaluation.request.endExclusive)},
       {"watermark", number(prepared.evaluation.request.watermark)},
       {"finalized", prepared.evaluation.request.watermark >=
                             prepared.evaluation.request.endExclusive
                         ? "true"
                         : "false"}},
      {{"d.evidence.target", lineage.observation}});
  if (lineage.timeField)
    static_cast<void>(evidence.appendRecord(
        "query-field-witness", "time field", "Pinned timestamp field", window,
        {}, {{"d.evidence.target", *lineage.timeField}}));
  if (lineage.arrivalWatermarkField)
    static_cast<void>(evidence.appendRecord(
        "query-field-witness", "arrival watermark field",
        "Pinned arrival watermark field", window, {},
        {{"d.evidence.target", *lineage.arrivalWatermarkField}}));
  if (lineage.keyField)
    static_cast<void>(evidence.appendRecord(
        "query-field-witness", "key field", "Pinned partition key field",
        window, {}, {{"d.evidence.target", *lineage.keyField}}));
  const auto exclusion = evidence.appendRecord(
      "query-exclusion", "occurrence " + number(index),
      "Occurrence excluded by the reject late-data policy", window,
      {{"reason", "late-data"},
       {"timestamp-state", "valid"},
       {"late-state", "late"},
       {"failure-code", "late-data-rejected"}},
      {});
  static_cast<void>(evidence.appendRecord(
      "query-lineage", "input row", "Retained input occurrence row", exclusion,
      {}, {{"d.evidence.target", lineage.rows[index]}}));
  static_cast<void>(evidence.appendRecord(
      "query-lineage", "time value", "Retained timestamp value witness",
      exclusion, {}, {{"d.evidence.target", *lineage.timeValues[index]}}));
  static_cast<void>(evidence.appendRecord(
      "query-lineage", "arrival watermark value",
      "Retained per-occurrence arrival watermark witness", exclusion, {},
      {{"d.evidence.target", *lineage.arrivalWatermarkValues[index]}}));
  if (lineage.keyValues[index])
    static_cast<void>(evidence.appendRecord(
        "query-key-witness", "partition key", lineage.keyColumn.value_or(""),
        exclusion, {}, {{"d.evidence.target", *lineage.keyValues[index]}}));
  RejectedObservationWindowRetention retention{
      .queryRun = run, .window = window, .exclusion = exclusion};
  for (std::size_t gap = 0; gap < prepared.evaluation.request.sourceGaps.size();
       ++gap) {
    const auto id = evidence.appendRecord(
        "query-gap", "source gap " + number(gap),
        prepared.evaluation.request.sourceGaps[gap], window,
        {{"scope", "source-coverage"}},
        {{"d.evidence.target", lineage.sourceGapRecords[gap]}});
    retention.gaps.push_back(id);
  }
  return retention;
}

} // namespace xanadu::evidence
