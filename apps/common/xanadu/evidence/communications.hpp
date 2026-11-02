#ifndef XANADU_EVIDENCE_COMMUNICATIONS_HPP
#define XANADU_EVIDENCE_COMMUNICATIONS_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/xanadu/evidence/capture.hpp"
#include "common/xanadu/evidence/evidence_case.hpp"

namespace xanadu::evidence {

struct CommunicationBounds {
  // Limits on retained communication records and their source-derived text.
  // Store operation and indexing overhead is not part of maxBytes.
  std::size_t maxRecords{4096};
  std::size_t maxBytes{8 * 1024 * 1024};
  std::size_t maxStringBytes{1024 * 1024};
  std::size_t maxParts{256};
  std::size_t maxPartDepth{24};
};

struct MailHeaderOccurrence {
  std::string name;
  std::string value;
  std::string rawValue;
};

struct MailRecipientOccurrence {
  std::string role;
  std::string address;
};

// This is the structured output expected from a MIME library, not a MIME
// parser. Raw and decoded content remain separate evidence artifacts.
struct MailMimePart {
  std::string key;
  std::optional<std::string> parentKey;
  std::string contentType;
  std::string disposition;
  std::string charset;
  std::string transferEncoding;
  std::string filename;
  std::string contentId;
  std::string contentLocation;
  std::string contentDescription;
  std::string charsetConversionStatus{"not-text"};
  std::vector<MailHeaderOccurrence> headers;
  std::vector<MailHeaderOccurrence> embeddedMessageHeaders;
  std::string rawBytes;
  std::optional<std::string> decodedBytes;
  std::optional<std::string> decodedText;
};

struct MailMessageEvidence {
  std::uint64_t uidValidity{};
  std::uint32_t uid{};
  std::uint32_t sequenceNumber{};
  std::string messageId;
  std::string subject;
  std::string rawMessage;
  std::vector<MailHeaderOccurrence> headers;
  std::vector<MailRecipientOccurrence> recipients;
  std::vector<MailMimePart> parts;
  std::optional<std::vector<std::string>> flags;
};

struct MailGap {
  std::string label;
  std::string detail;
  std::string cursor;
};

struct MailObservation {
  std::string serverAuthority;
  std::string accountScope;
  std::string mailbox;
  std::string mailboxIncarnation;
  CaptureState state{CaptureState::Complete};
  std::string coverage;
  std::vector<MailMessageEvidence> messages;
  std::vector<MailGap> gaps;
  CommunicationBounds bounds;
};

struct IrcMessageEvidence {
  std::optional<std::string> messageId;
  std::uint64_t localSequence{};
  std::string target;
  std::string sender;
  std::string command{"PRIVMSG"};
  std::string text;
  std::string serverTime;
  std::map<std::string, std::string> tags;
};

struct IrcGap {
  std::string fromCursor;
  std::string toCursor;
  std::string reason;
};

struct IrcObservation {
  std::string serverAuthority;
  std::string networkIncarnation;
  std::string connectionIncarnation;
  std::vector<std::string> capabilities;
  CaptureState state{CaptureState::Complete};
  std::vector<IrcMessageEvidence> messages;
  std::vector<IrcGap> gaps;
  CommunicationBounds bounds;
};

struct FediverseActorEvidence {
  std::string actorUri;
  std::string accountId;
  std::string handle;
  std::string displayName;
  std::string identityBasis{"supplied-observation"};
  std::string profileUrl;
};

struct FediverseObjectEvidence {
  std::string objectUri;
  std::string restId;
  std::string actorUri;
  std::string type;
  std::string content;
  std::string visibility;
  std::string contentWarning;
  std::optional<std::string> inReplyToUri;
  std::string revision;
  std::vector<std::string> audience;
};

struct FediverseActivityEvidence {
  std::string activityUri;
  std::string type;
  std::string actorUri;
  std::string objectUri;
  std::string published;
  std::vector<std::string> audience;
};

struct FediverseDeliveryEvidence {
  std::string activityUri;
  std::string collectionUri;
  std::uint64_t occurrence{};
};

struct FediverseObservation {
  std::string instanceAuthority;
  std::string accountId;
  std::string accountActorUri;
  std::string collectionUri;
  CaptureState state{CaptureState::Complete};
  std::string coverage;
  std::vector<FediverseActorEvidence> actors;
  std::vector<FediverseObjectEvidence> objects;
  std::vector<FediverseActivityEvidence> activities;
  std::vector<FediverseDeliveryEvidence> deliveries;
  CommunicationBounds bounds;
};

struct CommunicationRetention {
  RecordId observation{};
  std::unordered_map<std::string, RecordId> records;
};

[[nodiscard]] CommunicationRetention
retainMailObservation(EvidenceCase &evidence,
                      const MailObservation &observation);
[[nodiscard]] CommunicationRetention
retainIrcObservation(EvidenceCase &evidence, const IrcObservation &observation);
[[nodiscard]] CommunicationRetention
retainFediverseObservation(EvidenceCase &evidence,
                           const FediverseObservation &observation);

} // namespace xanadu::evidence
#endif
