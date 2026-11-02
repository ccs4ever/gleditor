#include "common/xanadu/evidence/subscription.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <map>
#include <memory>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace xanadu::evidence {
namespace {

std::string idText(RecordId id) { return std::to_string(id); }

RecordId parseId(std::string_view text) {
  RecordId id{};
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), id);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    throw std::runtime_error("invalid subscription record reference");
  return id;
}

std::uint64_t parseGeneration(std::string_view text) {
  std::uint64_t generation{};
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), generation);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    throw std::runtime_error("invalid subscription document generation");
  return generation;
}

std::size_t parseSize(std::string_view text) {
  std::size_t value{};
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    throw std::runtime_error("invalid subscription count");
  return value;
}

bool validText(std::string_view value, std::size_t maximum,
               bool allowEmpty = false) {
  return value.size() <= maximum && (allowEmpty || !value.empty()) &&
         value.find('\0') == std::string_view::npos;
}

std::string randomToken() {
  std::array<unsigned char, 24> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    throw std::runtime_error("subscription token entropy unavailable");
  constexpr char hex[] = "0123456789abcdef";
  std::string token(bytes.size() * 2, '0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    token[2 * i]     = hex[bytes[i] >> 4];
    token[2 * i + 1] = hex[bytes[i] & 0x0f];
  }
  OPENSSL_cleanse(bytes.data(), bytes.size());
  return token;
}

std::string_view coverageName(SubscriptionCoverage coverage) {
  switch (coverage) {
  case SubscriptionCoverage::Complete:
    return "complete";
  case SubscriptionCoverage::Partial:
    return "partial";
  case SubscriptionCoverage::Unavailable:
    return "unavailable";
  }
  throw std::invalid_argument("invalid subscription coverage");
}

SubscriptionCoverage parseCoverage(std::string_view value) {
  if (value == "complete") return SubscriptionCoverage::Complete;
  if (value == "partial") return SubscriptionCoverage::Partial;
  if (value == "unavailable") return SubscriptionCoverage::Unavailable;
  throw std::runtime_error("unknown subscription coverage");
}

std::string_view gapName(SubscriptionGapKind kind) {
  switch (kind) {
  case SubscriptionGapKind::CursorExpired:
    return "cursor-expired";
  case SubscriptionGapKind::HistoryUnavailable:
    return "history-unavailable";
  case SubscriptionGapKind::IncarnationChanged:
    return "incarnation-changed";
  case SubscriptionGapKind::Truncated:
    return "truncated";
  case SubscriptionGapKind::Disconnected:
    return "disconnected";
  case SubscriptionGapKind::Other:
    return "other";
  }
  throw std::invalid_argument("invalid subscription gap kind");
}

SubscriptionGapKind parseGap(std::string_view value) {
  for (const auto kind :
       {SubscriptionGapKind::CursorExpired,
        SubscriptionGapKind::HistoryUnavailable,
        SubscriptionGapKind::IncarnationChanged, SubscriptionGapKind::Truncated,
        SubscriptionGapKind::Disconnected, SubscriptionGapKind::Other})
    if (gapName(kind) == value) return kind;
  throw std::runtime_error("unknown subscription gap kind");
}

std::size_t addBytes(std::size_t total, std::size_t amount) {
  if (amount > std::numeric_limits<std::size_t>::max() - total)
    throw std::length_error("subscription metadata size overflow");
  return total + amount;
}

} // namespace

struct SubscriptionJournal::Impl {
  struct PlannedRecord {
    std::string kind;
    std::string label;
    std::string text;
    std::optional<RecordId> parent;
    std::map<std::string, std::string> attributes;
    std::vector<std::pair<std::string, RecordId>> references;
  };

  struct PendingCommit {
    SubscriptionPendingDurability publicToken;
    SubscriptionReadyToAcknowledge readyToken;
    std::vector<PlannedRecord> plan;
  };

  EvidenceCase *evidence{};
  SubscriptionDocumentHandle document;
  SubscriptionLimits limits;
  std::map<std::string, PendingCommit> pending;
  std::map<RecordId, std::string> pendingByDefinition;
  std::map<std::string, SubscriptionReadyToAcknowledge> readyTokens;

  Impl(EvidenceCase &active, SubscriptionDocumentHandle bound,
       SubscriptionLimits configured)
      : evidence(&active), document(std::move(bound)),
        limits(std::move(configured)) {
    if (document.documentId.empty() || !document.generation ||
        !limits.maxSubscriptions || !limits.maxBatchOccurrences ||
        !limits.maxBatchGaps || !limits.maxEvidenceReferencesPerOccurrence ||
        !limits.maxStringBytes || !limits.maxMetadataBytes ||
        !limits.maxEvidenceRecords)
      throw std::invalid_argument(
          "invalid subscription journal bounds or fence");
    if (limits.maxSubscriptions > limits.maxEvidenceRecords ||
        limits.maxBatchOccurrences > limits.maxEvidenceRecords ||
        limits.maxBatchGaps > limits.maxEvidenceRecords ||
        limits.maxSubscriptions > 1024 || limits.maxBatchOccurrences > 4096 ||
        limits.maxBatchGaps > 256 ||
        limits.maxEvidenceReferencesPerOccurrence > 64 ||
        limits.maxStringBytes < 48 || limits.maxStringBytes > 1024 * 1024 ||
        limits.maxMetadataBytes > 1024 * 1024 ||
        limits.maxEvidenceRecords > 100000)
      throw std::invalid_argument(
          "subscription journal bounds are inconsistent");
    verifyDocument(document);
    const auto records      = scan();
    std::size_t definitions = 0;
    for (const auto &record : records)
      if (record.kind == "subscription-definition" &&
          record.attributes.contains("document-id") &&
          record.attributes.at("document-id") == document.documentId &&
          record.attributes.contains("case-root-id") &&
          record.attributes.at("case-root-id") == idText(document.caseRoot))
        ++definitions;
    if (definitions > limits.maxSubscriptions)
      throw std::length_error("subscription definition scan limit exceeded");
  }

  void verifyHandle(const SubscriptionDocumentHandle &active) const {
    if (active.documentId != document.documentId ||
        active.caseRoot != document.caseRoot ||
        active.generation != document.generation)
      throw std::runtime_error(
          "subscription document generation fence mismatch");
    verifyDocument(active);
  }

  void verifyDocument(const SubscriptionDocumentHandle &active) const {
    if (!evidence->isOpen() ||
        evidence->store().documentId().str() != active.documentId)
      throw std::runtime_error("subscription evidence document changed");
    const auto page = evidence->recordIds(limits.maxEvidenceRecords);
    if (!page.complete)
      throw std::length_error("subscription document root scan limit exceeded");
    std::optional<RecordId> root;
    for (const auto id : page.ids) {
      if (evidence->inspect(id).kind != "case") continue;
      if (root)
        throw std::runtime_error("subscription case has multiple roots");
      root = id;
    }
    if (!root || *root != active.caseRoot)
      throw std::runtime_error("subscription case root changed");
  }

  std::vector<Record> scan() const {
    const auto page = evidence->recordIds(limits.maxEvidenceRecords);
    if (!page.complete)
      throw std::length_error("subscription evidence scan limit exceeded");
    std::vector<Record> result;
    result.reserve(page.ids.size());
    for (const auto id : page.ids) result.push_back(evidence->inspect(id));
    return result;
  }

  const Record &requireDefinition(const std::vector<Record> &records,
                                  RecordId id) const {
    const auto found = std::ranges::find(records, id, &Record::id);
    if (found == records.end() || found->kind != "subscription-definition" ||
        found->attributes.at("document-id") != document.documentId ||
        found->attributes.at("case-root-id") != idText(document.caseRoot))
      throw std::invalid_argument(
          "subscription definition is absent or foreign");
    return *found;
  }

  std::optional<Record> latestCheckpoint(const std::vector<Record> &records,
                                         RecordId definition) const {
    std::optional<Record> latest;
    for (const auto &record : records)
      if (record.kind == "subscription-checkpoint" &&
          record.parent == definition)
        latest = record;
    return latest;
  }

  void validateOptionalCursor(const std::optional<std::string> &cursor) const {
    if (cursor && !validText(*cursor, limits.maxStringBytes, true))
      throw std::invalid_argument(
          "subscription cursor exceeds configured bound");
  }

  std::vector<PlannedRecord> planCommit(const std::vector<Record> &records,
                                        RecordId definition,
                                        const SubscriptionCommitInput &input,
                                        std::string_view commitToken) const {
    const auto &definitionRecord = requireDefinition(records, definition);
    const auto &attributes       = definitionRecord.attributes;
    if (!validText(input.sourceIncarnation, limits.maxStringBytes) ||
        input.sourceIncarnation != attributes.at("source-incarnation") ||
        input.deliveries.size() > limits.maxBatchOccurrences ||
        input.gaps.size() > limits.maxBatchGaps)
      throw std::invalid_argument(
          "subscription batch identity or bounds invalid");
    validateOptionalCursor(input.checkpointCursor);
    if (input.coverage == SubscriptionCoverage::Complete &&
        (!input.checkpointCursor || !input.gaps.empty()))
      throw std::invalid_argument(
          "complete subscription coverage requires a cursor and no gaps");
    if (input.coverage == SubscriptionCoverage::Unavailable &&
        input.gaps.empty())
      throw std::invalid_argument(
          "unavailable subscription coverage requires an explicit gap");

    std::optional<Record> predecessorCheckpoint;
    const auto predecessorText = attributes.find("supersedes-id");
    if (predecessorText != attributes.end()) {
      const auto predecessorId = parseId(predecessorText->second);
      const auto &predecessor  = requireDefinition(records, predecessorId);
      if (predecessor.attributes.at("source-incarnation") !=
          input.sourceIncarnation) {
        predecessorCheckpoint = latestCheckpoint(records, predecessorId);
        const auto gap        = std::ranges::find(
            input.gaps, SubscriptionGapKind::IncarnationChanged,
            &SubscriptionGapInput::kind);
        if (gap == input.gaps.end())
          throw std::invalid_argument(
              "new source incarnation requires an explicit gap");
        std::optional<std::string> oldCursor;
        if (predecessorCheckpoint &&
            predecessorCheckpoint->attributes.contains("checkpoint-cursor"))
          oldCursor = predecessorCheckpoint->attributes.at("checkpoint-cursor");
        if (gap->fromCursor != oldCursor ||
            gap->toCursor != input.checkpointCursor)
          throw std::invalid_argument(
              "incarnation gap cursors do not match adjacent checkpoints");
      }
    }

    std::size_t metadataBytes = input.sourceIncarnation.size();
    if (input.deliveries.size() > limits.maxEvidenceRecords - 1 ||
        input.gaps.size() >
            limits.maxEvidenceRecords - 1 - input.deliveries.size())
      throw std::length_error("subscription commit record count overflow");
    std::vector<PlannedRecord> plan;
    plan.reserve(input.deliveries.size() + input.gaps.size() + 1);
    for (std::size_t i = 0; i < input.deliveries.size(); ++i) {
      const auto &delivery = input.deliveries[i];
      validateOptionalCursor(delivery.cursor);
      if (!validText(delivery.sourceEventIdentity, limits.maxStringBytes) ||
          delivery.evidence.empty() ||
          delivery.evidence.size() > limits.maxEvidenceReferencesPerOccurrence)
        throw std::invalid_argument(
            "subscription delivery is invalid or unbounded");
      metadataBytes =
          addBytes(metadataBytes, delivery.sourceEventIdentity.size());
      if (delivery.cursor)
        metadataBytes = addBytes(metadataBytes, delivery.cursor->size());
      std::vector<std::pair<std::string, RecordId>> references;
      references.reserve(delivery.evidence.size());
      for (std::size_t ref = 0; ref < delivery.evidence.size(); ++ref) {
        static_cast<void>(evidence->inspect(delivery.evidence[ref]));
        references.emplace_back("retained-evidence-" + std::to_string(ref),
                                delivery.evidence[ref]);
        metadataBytes =
            addBytes(metadataBytes, idText(delivery.evidence[ref]).size());
      }
      std::map<std::string, std::string> recordAttributes{
          {"document-id", document.documentId},
          {"case-root-id", idText(document.caseRoot)},
          {"document-generation", std::to_string(document.generation)},
          {"commit-token", std::string(commitToken)},
          {"record-index", std::to_string(i)},
          {"delivery-occurrence", std::to_string(i + 1)},
          {"evidence-count", std::to_string(delivery.evidence.size())},
          {"source-incarnation", input.sourceIncarnation},
          {"source-event-identity", delivery.sourceEventIdentity}};
      if (delivery.cursor)
        recordAttributes.emplace("source-cursor", *delivery.cursor);
      for (std::size_t ref = 0; ref < delivery.evidence.size(); ++ref)
        recordAttributes.emplace("evidence-reference-" + std::to_string(ref),
                                 idText(delivery.evidence[ref]));
      plan.push_back(
          {"subscription-delivery", "Delivery " + std::to_string(i + 1),
           "One delivered occurrence pinned to retained evidence.", definition,
           std::move(recordAttributes), std::move(references)});
    }
    const std::size_t gapOffset = input.deliveries.size();
    for (std::size_t i = 0; i < input.gaps.size(); ++i) {
      const auto &gap = input.gaps[i];
      validateOptionalCursor(gap.fromCursor);
      validateOptionalCursor(gap.toCursor);
      if (!validText(gap.detail, limits.maxStringBytes))
        throw std::invalid_argument("subscription gap detail is invalid");
      metadataBytes = addBytes(metadataBytes, gap.detail.size());
      if (gap.fromCursor)
        metadataBytes = addBytes(metadataBytes, gap.fromCursor->size());
      if (gap.toCursor)
        metadataBytes = addBytes(metadataBytes, gap.toCursor->size());
      std::map<std::string, std::string> recordAttributes{
          {"document-id", document.documentId},
          {"case-root-id", idText(document.caseRoot)},
          {"document-generation", std::to_string(document.generation)},
          {"commit-token", std::string(commitToken)},
          {"record-index", std::to_string(gapOffset + i)},
          {"gap-occurrence", std::to_string(i + 1)},
          {"gap-kind", std::string(gapName(gap.kind))}};
      if (gap.fromCursor)
        recordAttributes.emplace("from-cursor", *gap.fromCursor);
      if (gap.toCursor) recordAttributes.emplace("to-cursor", *gap.toCursor);
      plan.push_back({"subscription-gap",
                      "Coverage gap",
                      gap.detail,
                      definition,
                      std::move(recordAttributes),
                      {}});
    }
    const auto checkpointIndex = input.deliveries.size() + input.gaps.size();
    std::map<std::string, std::string> checkpointAttributes{
        {"document-id", document.documentId},
        {"case-root-id", idText(document.caseRoot)},
        {"document-generation", std::to_string(document.generation)},
        {"commit-token", std::string(commitToken)},
        {"record-index", std::to_string(checkpointIndex)},
        {"source-incarnation", input.sourceIncarnation},
        {"coverage", std::string(coverageName(input.coverage))},
        {"delivery-occurrences", std::to_string(input.deliveries.size())},
        {"gap-count", std::to_string(input.gaps.size())}};
    if (input.checkpointCursor)
      checkpointAttributes.emplace("checkpoint-cursor",
                                   *input.checkpointCursor);
    // The checkpoint is appended after the child records. Its ordered child
    // rank and references form the durable delivery/checkpoint boundary.
    plan.push_back({"subscription-checkpoint",
                    "Subscription checkpoint",
                    "Durable local delivery and source coverage boundary.",
                    definition,
                    std::move(checkpointAttributes),
                    {}});
    for (const auto &record : plan) {
      metadataBytes = addBytes(metadataBytes, record.kind.size());
      metadataBytes = addBytes(metadataBytes, record.label.size());
      metadataBytes = addBytes(metadataBytes, record.text.size());
      for (const auto &[key, value] : record.attributes) {
        metadataBytes = addBytes(metadataBytes, key.size());
        metadataBytes = addBytes(metadataBytes, value.size());
      }
      for (const auto &[dimension, target] : record.references) {
        metadataBytes = addBytes(metadataBytes, dimension.size());
        metadataBytes = addBytes(metadataBytes, idText(target).size());
      }
    }
    const auto linkCount = input.deliveries.size() + input.gaps.size();
    if (linkCount > std::numeric_limits<std::size_t>::max() / 40)
      throw std::length_error("subscription reference metadata overflow");
    metadataBytes = addBytes(metadataBytes, linkCount * 40);
    if (metadataBytes > limits.maxMetadataBytes)
      throw std::length_error("subscription batch metadata budget exceeded");
    return plan;
  }

  void ensureCapacity(const std::vector<Record> &records,
                      std::size_t adding) const {
    if (records.size() > limits.maxEvidenceRecords ||
        adding > limits.maxEvidenceRecords - records.size())
      throw std::length_error("subscription evidence record budget exceeded");
  }

  std::vector<RecordId> appendPlan(const std::vector<PlannedRecord> &plan,
                                   std::string_view commitToken) {
    auto records = scan();
    std::vector<RecordId> result;
    result.reserve(plan.size());
    std::unordered_map<std::size_t, RecordId> existing;
    for (const auto &record : records) {
      if (!record.attributes.contains("commit-token") ||
          record.attributes.at("commit-token") != commitToken ||
          !record.attributes.contains("record-index"))
        continue;
      const auto index = static_cast<std::size_t>(
          std::stoull(record.attributes.at("record-index")));
      if (index >= plan.size() || !existing.emplace(index, record.id).second)
        throw std::runtime_error("subscription commit index is ambiguous");
      if (record.kind != plan[index].kind ||
          record.parent != plan[index].parent)
        throw std::runtime_error("subscription commit record changed");
    }
    ensureCapacity(records, plan.size() - existing.size());
    for (std::size_t index = 0; index < plan.size(); ++index) {
      if (existing.contains(index)) {
        result.push_back(existing.at(index));
        continue;
      }
      const auto &item = plan[index];
      auto references  = item.references;
      auto attributes  = item.attributes;
      if (item.kind == "subscription-checkpoint") {
        for (std::size_t previous = 0; previous < index; ++previous) {
          const auto &prior    = plan[previous];
          const auto dimension = prior.kind == "subscription-delivery"
                                     ? "delivery-occurrence-"
                                     : "coverage-gap-";
          const auto occurrence =
              prior.kind == "subscription-delivery"
                  ? prior.attributes.at("delivery-occurrence")
                  : prior.attributes.at("gap-occurrence");
          const auto childKey =
              (prior.kind == "subscription-delivery" ? "delivery-record-"
                                                     : "gap-record-") +
              occurrence;
          attributes.emplace(childKey, idText(result[previous]));
          references.emplace_back(std::string(dimension) + occurrence,
                                  result[previous]);
        }
      }
      result.push_back(
          evidence->appendRecord(item.kind, item.label, item.text, item.parent,
                                 std::move(attributes), std::move(references)));
    }
    return result;
  }

  SubscriptionCommitResult drive(PendingCommit &commit) {
    static_cast<void>(appendPlan(commit.plan, commit.publicToken.pendingId));
    const auto records = scan();
    std::optional<RecordId> checkpointId;
    for (const auto &record : records) {
      if (record.kind == "subscription-checkpoint" &&
          record.attributes.contains("commit-token") &&
          record.attributes.at("commit-token") == commit.publicToken.pendingId)
        checkpointId = record.id;
    }
    if (!checkpointId)
      throw std::runtime_error("subscription checkpoint was not appended");
    evidence->durableSave();
    commit.readyToken.checkpoint = *checkpointId;
    readyTokens.insert_or_assign(commit.readyToken.token, commit.readyToken);
    return commit.readyToken;
  }
};

SubscriptionJournal::SubscriptionJournal(EvidenceCase &evidence,
                                         SubscriptionDocumentHandle document,
                                         SubscriptionLimits limits)
    : impl_(std::make_unique<Impl>(evidence, std::move(document),
                                   std::move(limits))) {}
SubscriptionJournal::~SubscriptionJournal() = default;
SubscriptionJournal::SubscriptionJournal(SubscriptionJournal &&) noexcept =
    default;
SubscriptionJournal &
SubscriptionJournal::operator=(SubscriptionJournal &&) noexcept = default;

RecordId
SubscriptionJournal::define(const SubscriptionDocumentHandle &activeDocument,
                            const SubscriptionDefinitionInput &definition,
                            const std::optional<RecordId> supersedes) {
  impl_->verifyHandle(activeDocument);
  std::size_t metadata{};
  for (const auto *value :
       {&definition.sourceAuthority, &definition.sourceKey,
        &definition.adapterId, &definition.adapterEdition,
        &definition.sourceIncarnation, &definition.selector}) {
    if (!validText(*value, impl_->limits.maxStringBytes))
      throw std::invalid_argument("subscription definition field is invalid");
    metadata = addBytes(metadata, value->size());
  }
  metadata = addBytes(metadata, 512);
  if (metadata > impl_->limits.maxMetadataBytes)
    throw std::length_error("subscription definition metadata limit exceeded");
  const auto records = impl_->scan();
  for (const auto &record : records) {
    if (record.kind != "subscription-definition" ||
        record.attributes.at("document-id") != impl_->document.documentId ||
        record.attributes.at("case-root-id") !=
            idText(impl_->document.caseRoot) ||
        record.attributes.at("source-authority") !=
            definition.sourceAuthority ||
        record.attributes.at("source-key") != definition.sourceKey ||
        record.attributes.at("adapter-id") != definition.adapterId ||
        record.attributes.at("adapter-edition") != definition.adapterEdition ||
        record.attributes.at("source-incarnation") !=
            definition.sourceIncarnation ||
        record.attributes.at("selector") != definition.selector)
      continue;
    const auto storedSupersedes =
        record.attributes.contains("supersedes-id")
            ? std::optional<RecordId>(
                  parseId(record.attributes.at("supersedes-id")))
            : std::nullopt;
    if (storedSupersedes == supersedes) {
      impl_->evidence->durableSave();
      return record.id;
    }
  }
  std::size_t count{};
  for (const auto &record : records)
    if (record.kind == "subscription-definition" &&
        record.attributes.contains("document-id") &&
        record.attributes.at("document-id") == impl_->document.documentId)
      ++count;
  if (count >= impl_->limits.maxSubscriptions)
    throw std::length_error("subscription definition budget exceeded");
  if (records.size() >= impl_->limits.maxEvidenceRecords)
    throw std::length_error("subscription evidence record budget exceeded");
  std::vector<std::pair<std::string, RecordId>> references;
  if (supersedes) {
    const auto &previous = impl_->requireDefinition(records, *supersedes);
    if (previous.attributes.at("source-authority") !=
            definition.sourceAuthority ||
        previous.attributes.at("source-key") != definition.sourceKey)
      throw std::invalid_argument(
          "superseding subscription must retain its source scope");
    references.emplace_back("supersedes-subscription", *supersedes);
  }
  std::map<std::string, std::string> attributes{
      {"document-id", impl_->document.documentId},
      {"case-root-id", idText(impl_->document.caseRoot)},
      {"document-generation", std::to_string(impl_->document.generation)},
      {"source-authority", definition.sourceAuthority},
      {"source-key", definition.sourceKey},
      {"adapter-id", definition.adapterId},
      {"adapter-edition", definition.adapterEdition},
      {"source-incarnation", definition.sourceIncarnation},
      {"selector", definition.selector}};
  if (supersedes) attributes.emplace("supersedes-id", idText(*supersedes));
  const auto id = impl_->evidence->appendRecord(
      "subscription-definition", "Subscription definition",
      "Immutable source, adapter edition and incarnation binding.",
      impl_->document.caseRoot, std::move(attributes), std::move(references));
  impl_->evidence->durableSave();
  return id;
}

SubscriptionCommitResult
SubscriptionJournal::commit(const SubscriptionDocumentHandle &activeDocument,
                            const RecordId definition,
                            const SubscriptionCommitInput &input) {
  impl_->verifyHandle(activeDocument);
  if (impl_->pendingByDefinition.contains(definition))
    throw std::logic_error("subscription has an unresolved local checkpoint");
  const auto token   = randomToken();
  const auto records = impl_->scan();
  auto plan          = impl_->planCommit(records, definition, input, token);
  impl_->ensureCapacity(records, plan.size());
  const auto pendingToken =
      SubscriptionPendingDurability{token, impl_->document, definition};
  SubscriptionReadyToAcknowledge readyToken{token,
                                            token,
                                            impl_->document,
                                            definition,
                                            0,
                                            impl_->document.generation,
                                            input.sourceIncarnation,
                                            input.checkpointCursor};
  auto [at, inserted] = impl_->pending.emplace(
      token, Impl::PendingCommit{pendingToken, readyToken, std::move(plan)});
  if (!inserted) throw std::runtime_error("subscription token collision");
  impl_->pendingByDefinition.emplace(definition, token);
  try {
    auto result = impl_->drive(at->second);
    impl_->pendingByDefinition.erase(definition);
    impl_->pending.erase(at);
    return result;
  } catch (const std::exception &) {
    return pendingToken;
  }
}

SubscriptionCommitResult SubscriptionJournal::retryDurability(
    const SubscriptionDocumentHandle &activeDocument,
    const SubscriptionPendingDurability &pending) {
  impl_->verifyHandle(activeDocument);
  impl_->verifyHandle(pending.document);
  const auto found = impl_->pending.find(pending.pendingId);
  if (found == impl_->pending.end() ||
      found->second.publicToken.definition != pending.definition ||
      found->second.publicToken.document.documentId !=
          pending.document.documentId ||
      found->second.publicToken.document.caseRoot !=
          pending.document.caseRoot ||
      found->second.publicToken.document.generation !=
          pending.document.generation)
    throw std::invalid_argument(
        "subscription pending token is stale or foreign");
  try {
    auto result = impl_->drive(found->second);
    impl_->pendingByDefinition.erase(pending.definition);
    impl_->pending.erase(found);
    return result;
  } catch (const std::exception &) {
    return pending;
  }
}

std::vector<SubscriptionDefinition> SubscriptionJournal::definitions() const {
  impl_->verifyDocument(impl_->document);
  const auto records = impl_->scan();
  std::vector<SubscriptionDefinition> result;
  for (const auto &record : records) {
    if (record.kind != "subscription-definition" ||
        record.attributes.at("document-id") != impl_->document.documentId ||
        record.attributes.at("case-root-id") !=
            idText(impl_->document.caseRoot))
      continue;
    SubscriptionDefinitionInput input{
        record.attributes.at("source-authority"),
        record.attributes.at("source-key"),
        record.attributes.at("adapter-id"),
        record.attributes.at("adapter-edition"),
        record.attributes.at("source-incarnation"),
        record.attributes.at("selector")};
    std::optional<RecordId> supersedes;
    if (record.attributes.contains("supersedes-id"))
      supersedes = parseId(record.attributes.at("supersedes-id"));
    result.push_back({record.id, std::move(input), supersedes});
    if (result.size() > impl_->limits.maxSubscriptions)
      throw std::length_error("subscription definition budget exceeded");
  }
  return result;
}

std::vector<SubscriptionCheckpointView>
SubscriptionJournal::checkpoints(const RecordId definition) const {
  impl_->verifyDocument(impl_->document);
  const auto records = impl_->scan();
  static_cast<void>(impl_->requireDefinition(records, definition));
  std::vector<SubscriptionCheckpointView> result;
  for (const auto &record : records) {
    if (record.kind != "subscription-checkpoint" || record.parent != definition)
      continue;
    SubscriptionCheckpointView view;
    view.record            = record.id;
    view.definition        = definition;
    view.sourceIncarnation = record.attributes.at("source-incarnation");
    if (record.attributes.contains("checkpoint-cursor"))
      view.cursor = record.attributes.at("checkpoint-cursor");
    view.coverage            = parseCoverage(record.attributes.at("coverage"));
    view.deliveryOccurrences = static_cast<std::size_t>(
        std::stoull(record.attributes.at("delivery-occurrences")));
    view.gaps = static_cast<std::size_t>(
        std::stoull(record.attributes.at("gap-count")));
    result.push_back(std::move(view));
  }
  return result;
}

SubscriptionReadyToAcknowledge SubscriptionJournal::revalidateCheckpoint(
    const SubscriptionDocumentHandle &activeDocument, RecordId definition,
    RecordId checkpointId) {
  impl_->verifyHandle(activeDocument);
  if (impl_->pendingByDefinition.contains(definition))
    throw std::logic_error("subscription has an unresolved local checkpoint");
  const auto records           = impl_->scan();
  const auto &definitionRecord = impl_->requireDefinition(records, definition);
  const auto checkpointAt =
      std::ranges::find(records, checkpointId, &Record::id);
  if (checkpointAt == records.end() ||
      checkpointAt->kind != "subscription-checkpoint" ||
      checkpointAt->parent != definition ||
      checkpointAt->attributes.at("document-id") != activeDocument.documentId ||
      checkpointAt->attributes.at("case-root-id") !=
          idText(activeDocument.caseRoot))
    throw std::invalid_argument("subscription checkpoint is absent or foreign");

  const auto &attributes     = checkpointAt->attributes;
  const auto commitId        = attributes.at("commit-token");
  const auto incarnation     = attributes.at("source-incarnation");
  const auto coverage        = parseCoverage(attributes.at("coverage"));
  const auto deliveryCount   = parseSize(attributes.at("delivery-occurrences"));
  const auto gapCount        = parseSize(attributes.at("gap-count"));
  const auto checkpointIndex = parseSize(attributes.at("record-index"));
  const auto checkpointGeneration =
      parseGeneration(attributes.at("document-generation"));
  if (incarnation != definitionRecord.attributes.at("source-incarnation") ||
      !validText(incarnation, impl_->limits.maxStringBytes) ||
      deliveryCount > impl_->limits.maxBatchOccurrences ||
      gapCount > impl_->limits.maxBatchGaps ||
      deliveryCount > impl_->limits.maxEvidenceRecords - 1 ||
      gapCount > impl_->limits.maxEvidenceRecords - 1 - deliveryCount ||
      checkpointIndex != deliveryCount + gapCount || !checkpointGeneration)
    throw std::runtime_error("subscription checkpoint identity is invalid");
  std::optional<std::string> cursor;
  if (attributes.contains("checkpoint-cursor"))
    cursor = attributes.at("checkpoint-cursor");
  impl_->validateOptionalCursor(cursor);
  if ((coverage == SubscriptionCoverage::Complete &&
       (!cursor || gapCount != 0)) ||
      (coverage == SubscriptionCoverage::Unavailable && gapCount == 0))
    throw std::runtime_error("subscription checkpoint coverage is incomplete");

  std::unordered_map<std::size_t, const Record *> recordsByIndex;
  for (const auto &record : records) {
    if (record.parent != definition ||
        !record.attributes.contains("commit-token") ||
        record.attributes.at("commit-token") != commitId)
      continue;
    if (!record.attributes.contains("record-index"))
      throw std::runtime_error(
          "subscription checkpoint child index is missing");
    const auto index = parseSize(record.attributes.at("record-index"));
    if (index > checkpointIndex ||
        !recordsByIndex.emplace(index, &record).second)
      throw std::runtime_error(
          "subscription checkpoint child index is ambiguous");
    if (record.attributes.at("document-id") != activeDocument.documentId ||
        record.attributes.at("case-root-id") !=
            idText(activeDocument.caseRoot) ||
        parseGeneration(record.attributes.at("document-generation")) !=
            checkpointGeneration)
      throw std::runtime_error("subscription checkpoint child fence mismatch");
  }
  if (recordsByIndex.size() != checkpointIndex + 1 ||
      !recordsByIndex.contains(checkpointIndex) ||
      recordsByIndex.at(checkpointIndex)->id != checkpointId)
    throw std::runtime_error("subscription checkpoint closure is incomplete");

  auto manifold = impl_->evidence->store().rebuildManifold(
      impl_->evidence->store().latest());
  auto verifyReference = [&](RecordId owner, std::string_view dimensionName,
                             RecordId expectedTarget) {
    const auto dimension =
        manifold.dimensionNamed(dimensionName, impl_->evidence->store());
    if (!dimension)
      throw std::runtime_error("subscription graph dimension is missing");
    const auto wrapper = manifold.linked(owner, *dimension);
    if (!wrapper)
      throw std::runtime_error("subscription graph reference edge is missing");
    const auto reference = impl_->evidence->inspect(*wrapper);
    if (reference.kind != "reference" || reference.label != dimensionName ||
        reference.text != idText(expectedTarget))
      throw std::runtime_error("subscription graph reference target changed");
  };

  for (std::size_t i = 0; i < checkpointIndex; ++i) {
    const auto *record = recordsByIndex.at(i);
    if (i < deliveryCount) {
      if (record->kind != "subscription-delivery" ||
          record->attributes.at("source-incarnation") != incarnation ||
          record->attributes.at("delivery-occurrence") !=
              std::to_string(i + 1) ||
          !validText(record->attributes.at("source-event-identity"),
                     impl_->limits.maxStringBytes))
        throw std::runtime_error("subscription delivery closure is invalid");
      const auto refCount = parseSize(record->attributes.at("evidence-count"));
      if (!refCount ||
          refCount > impl_->limits.maxEvidenceReferencesPerOccurrence)
        throw std::runtime_error("subscription evidence closure is invalid");
      if (record->attributes.contains("source-cursor"))
        impl_->validateOptionalCursor(record->attributes.at("source-cursor"));
      for (std::size_t ref = 0; ref < refCount; ++ref) {
        const auto key = "evidence-reference-" + std::to_string(ref);
        if (!record->attributes.contains(key))
          throw std::runtime_error(
              "subscription evidence reference is missing");
        const auto evidenceId = parseId(record->attributes.at(key));
        static_cast<void>(impl_->evidence->inspect(evidenceId));
        verifyReference(record->id, "retained-evidence-" + std::to_string(ref),
                        evidenceId);
      }
      const auto referenceKey = "delivery-record-" + std::to_string(i + 1);
      if (!attributes.contains(referenceKey) ||
          attributes.at(referenceKey) != idText(record->id))
        throw std::runtime_error("checkpoint omits a delivery reference");
      verifyReference(checkpointId, "delivery-record-" + std::to_string(i + 1),
                      record->id);
    } else {
      const auto gapOccurrence = i - deliveryCount + 1;
      if (record->kind != "subscription-gap" ||
          record->attributes.at("gap-occurrence") !=
              std::to_string(gapOccurrence) ||
          !validText(record->text, impl_->limits.maxStringBytes))
        throw std::runtime_error("subscription gap closure is invalid");
      static_cast<void>(parseGap(record->attributes.at("gap-kind")));
      if (record->attributes.contains("from-cursor"))
        impl_->validateOptionalCursor(record->attributes.at("from-cursor"));
      if (record->attributes.contains("to-cursor"))
        impl_->validateOptionalCursor(record->attributes.at("to-cursor"));
      const auto referenceKey = "gap-record-" + std::to_string(gapOccurrence);
      if (!attributes.contains(referenceKey) ||
          attributes.at(referenceKey) != idText(record->id))
        throw std::runtime_error("checkpoint omits a gap reference");
      verifyReference(checkpointId,
                      "gap-record-" + std::to_string(gapOccurrence),
                      record->id);
    }
  }

  if (definitionRecord.attributes.contains("supersedes-id")) {
    const auto predecessorId =
        parseId(definitionRecord.attributes.at("supersedes-id"));
    const auto &predecessor = impl_->requireDefinition(records, predecessorId);
    if (predecessor.attributes.at("source-incarnation") != incarnation) {
      const auto previous = impl_->latestCheckpoint(records, predecessorId);
      std::optional<std::string> priorCursor;
      if (previous && previous->attributes.contains("checkpoint-cursor"))
        priorCursor = previous->attributes.at("checkpoint-cursor");
      bool gapWitnessed = false;
      for (std::size_t i = deliveryCount; i < checkpointIndex; ++i) {
        const auto &gap = *recordsByIndex.at(i);
        if (gap.attributes.at("gap-kind") == "incarnation-changed" &&
            (gap.attributes.contains("from-cursor")
                 ? std::optional<std::string>(gap.attributes.at("from-cursor"))
                 : std::nullopt) == priorCursor &&
            (gap.attributes.contains("to-cursor")
                 ? std::optional<std::string>(gap.attributes.at("to-cursor"))
                 : std::nullopt) == cursor)
          gapWitnessed = true;
      }
      if (!gapWitnessed)
        throw std::runtime_error(
            "new source incarnation lacks its continuity gap");
    }
  }

  SubscriptionReadyToAcknowledge ready{randomToken(),  commitId,
                                       activeDocument, definition,
                                       checkpointId,   checkpointGeneration,
                                       incarnation,    cursor};
  impl_->evidence->durableSave();
  impl_->readyTokens.insert_or_assign(ready.token, ready);
  return ready;
}

bool SubscriptionJournal::acknowledgementMatches(
    const SubscriptionDocumentHandle &activeDocument,
    const SubscriptionReadyToAcknowledge &token) const {
  try {
    impl_->verifyHandle(activeDocument);
    if (token.document.documentId != activeDocument.documentId ||
        token.document.caseRoot != activeDocument.caseRoot ||
        token.document.generation != activeDocument.generation)
      return false;
    const auto issued = impl_->readyTokens.find(token.token);
    if (issued == impl_->readyTokens.end() ||
        issued->second.document.documentId != token.document.documentId ||
        issued->second.document.caseRoot != token.document.caseRoot ||
        issued->second.document.generation != token.document.generation ||
        issued->second.persistedCommitId != token.persistedCommitId ||
        issued->second.definition != token.definition ||
        issued->second.checkpoint != token.checkpoint ||
        issued->second.checkpointGeneration != token.checkpointGeneration ||
        issued->second.sourceIncarnation != token.sourceIncarnation ||
        issued->second.cursor != token.cursor)
      return false;
    const auto records = impl_->scan();
    const auto found =
        std::ranges::find(records, token.checkpoint, &Record::id);
    return found != records.end() && found->kind == "subscription-checkpoint" &&
           found->parent == token.definition &&
           found->attributes.at("document-id") == activeDocument.documentId &&
           found->attributes.at("case-root-id") ==
               idText(activeDocument.caseRoot) &&
           found->attributes.contains("commit-token") &&
           found->attributes.at("commit-token") == token.persistedCommitId &&
           found->attributes.at("source-incarnation") ==
               token.sourceIncarnation &&
           (token.cursor
                ? found->attributes.contains("checkpoint-cursor") &&
                      found->attributes.at("checkpoint-cursor") == *token.cursor
                : !found->attributes.contains("checkpoint-cursor")) &&
           found->attributes.contains("document-generation") &&
           parseGeneration(found->attributes.at("document-generation")) ==
               token.checkpointGeneration;
  } catch (...) {
    return false;
  }
}

} // namespace xanadu::evidence
