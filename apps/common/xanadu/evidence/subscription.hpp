#ifndef XANADU_EVIDENCE_SUBSCRIPTION_HPP
#define XANADU_EVIDENCE_SUBSCRIPTION_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "common/xanadu/evidence/evidence_case.hpp"

namespace xanadu::evidence {

struct SubscriptionDocumentHandle {
  std::string documentId;
  RecordId caseRoot{};
  std::uint64_t generation{};
};

struct SubscriptionLimits {
  std::size_t maxSubscriptions{256};
  std::size_t maxBatchOccurrences{4096};
  std::size_t maxBatchGaps{256};
  std::size_t maxEvidenceReferencesPerOccurrence{64};
  std::size_t maxStringBytes{4096};
  std::size_t maxMetadataBytes{1024 * 1024};
  std::size_t maxEvidenceRecords{100000};
};

struct SubscriptionDefinitionInput {
  std::string sourceAuthority;
  std::string sourceKey;
  std::string adapterId;
  std::string adapterEdition;
  std::string sourceIncarnation;
  std::string selector;
};

struct SubscriptionDefinition {
  RecordId record{};
  SubscriptionDefinitionInput identity;
  std::optional<RecordId> supersedes;
};

enum class SubscriptionCoverage : unsigned char {
  Complete,
  Partial,
  Unavailable
};

enum class SubscriptionGapKind : unsigned char {
  CursorExpired,
  HistoryUnavailable,
  IncarnationChanged,
  Truncated,
  Disconnected,
  Other
};

struct SubscriptionGapInput {
  SubscriptionGapKind kind{SubscriptionGapKind::Other};
  std::optional<std::string> fromCursor;
  std::optional<std::string> toCursor;
  std::string detail;
};

struct SubscriptionDeliveryInput {
  std::string sourceEventIdentity;
  std::optional<std::string> cursor;
  std::vector<RecordId> evidence;
};

struct SubscriptionCommitInput {
  std::string sourceIncarnation;
  std::optional<std::string> checkpointCursor;
  SubscriptionCoverage coverage{SubscriptionCoverage::Partial};
  std::vector<SubscriptionDeliveryInput> deliveries;
  std::vector<SubscriptionGapInput> gaps;
};

struct SubscriptionReadyToAcknowledge {
  std::string token;
  std::string persistedCommitId;
  SubscriptionDocumentHandle document;
  RecordId definition{};
  RecordId checkpoint{};
  // Generation stored on the persisted checkpoint, distinct from the active
  // document generation that issued this in-memory capability.
  std::uint64_t checkpointGeneration{};
  std::string sourceIncarnation;
  std::optional<std::string> cursor;
};

struct SubscriptionPendingDurability {
  std::string pendingId;
  SubscriptionDocumentHandle document;
  RecordId definition{};
};

using SubscriptionCommitResult =
    std::variant<SubscriptionReadyToAcknowledge, SubscriptionPendingDurability>;

struct SubscriptionCheckpointView {
  RecordId record{};
  RecordId definition{};
  std::string sourceIncarnation;
  std::optional<std::string> cursor;
  SubscriptionCoverage coverage{SubscriptionCoverage::Partial};
  std::size_t deliveryOccurrences{};
  std::size_t gaps{};
};

/// Persists subscription delivery occurrences and checkpoints locally.
/// Construction and reopen never acquire a source or invoke a transport.
class SubscriptionJournal {
public:
  explicit SubscriptionJournal(EvidenceCase &evidence,
                               SubscriptionDocumentHandle document,
                               SubscriptionLimits limits = {});
  ~SubscriptionJournal();
  SubscriptionJournal(const SubscriptionJournal &)            = delete;
  SubscriptionJournal &operator=(const SubscriptionJournal &) = delete;
  SubscriptionJournal(SubscriptionJournal &&) noexcept;
  SubscriptionJournal &operator=(SubscriptionJournal &&) noexcept;

  [[nodiscard]] RecordId
  define(const SubscriptionDocumentHandle &activeDocument,
         const SubscriptionDefinitionInput &definition,
         std::optional<RecordId> supersedes = {});
  [[nodiscard]] SubscriptionCommitResult
  commit(const SubscriptionDocumentHandle &activeDocument, RecordId definition,
         const SubscriptionCommitInput &input);
  [[nodiscard]] SubscriptionCommitResult
  retryDurability(const SubscriptionDocumentHandle &activeDocument,
                  const SubscriptionPendingDurability &pending);
  [[nodiscard]] std::vector<SubscriptionDefinition> definitions() const;
  [[nodiscard]] std::vector<SubscriptionCheckpointView>
  checkpoints(RecordId definition) const;
  [[nodiscard]] SubscriptionReadyToAcknowledge
  revalidateCheckpoint(const SubscriptionDocumentHandle &activeDocument,
                       RecordId definition, RecordId checkpoint);
  [[nodiscard]] bool
  acknowledgementMatches(const SubscriptionDocumentHandle &activeDocument,
                         const SubscriptionReadyToAcknowledge &token) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace xanadu::evidence

#endif
