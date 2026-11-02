#include "common/xanadu/evidence/communications.hpp"

#include <algorithm>
#include <charconv>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_set>

namespace xanadu::evidence {
namespace {

class Budget {
public:
  explicit Budget(const CommunicationBounds &bounds) : bounds_(bounds) {}

  void record() {
    if (records_ == bounds_.maxRecords)
      throw std::invalid_argument("communication record limit exceeded");
    ++records_;
  }

  void bytes(const std::string_view value) {
    if (value.size() > bounds_.maxStringBytes ||
        value.size() > bounds_.maxBytes - std::min(bytes_, bounds_.maxBytes))
      throw std::invalid_argument("communication byte limit exceeded");
    bytes_ += value.size();
  }

private:
  const CommunicationBounds &bounds_;
  std::size_t records_{};
  std::size_t bytes_{};
};

void charge(Budget &budget, const std::string_view value) {
  budget.bytes(value);
}

[[nodiscard]] std::string stateText(const CaptureState state) {
  return stateName(state);
}

[[nodiscard]] RecordId
append(EvidenceCase &evidence, Budget &budget, const std::string_view kind,
       const std::string_view label, const std::string_view text,
       const std::optional<RecordId> parent,
       std::map<std::string, std::string> attributes            = {},
       std::vector<std::pair<std::string, RecordId>> references = {}) {
  budget.record();
  return evidence.appendRecord(kind, label, text, parent, std::move(attributes),
                               std::move(references));
}

void validateBounds(const CommunicationBounds &bounds) {
  if (bounds.maxRecords == 0 || bounds.maxBytes == 0 ||
      bounds.maxStringBytes == 0 || bounds.maxParts == 0 ||
      bounds.maxPartDepth == 0) {
    throw std::invalid_argument("communication bounds must be positive");
  }
}

class RecordPlan {
public:
  explicit RecordPlan(const std::size_t limit) : limit_(limit) {}
  void add(const std::size_t count = 1) {
    if (count > limit_ - std::min(count_, limit_)) {
      throw std::invalid_argument("communication record limit exceeded");
    }
    count_ += count;
  }

private:
  std::size_t limit_;
  std::size_t count_{};
};

void validateCommon(Budget &budget, const std::string_view authority,
                    const std::string_view account,
                    const std::string_view container,
                    const std::string_view coverage) {
  if (authority.empty() || account.empty() || container.empty()) {
    throw std::invalid_argument("communication observation identity is empty");
  }
  charge(budget, authority);
  charge(budget, account);
  charge(budget, container);
  charge(budget, coverage);
}

[[nodiscard]] std::string mailIdentity(const MailMessageEvidence &message) {
  return "mail:" + std::to_string(message.uidValidity) + ":" +
         std::to_string(message.uid);
}

std::string identityTuple(std::initializer_list<std::string_view> parts) {
  std::string result;
  for (const auto part : parts) {
    result += std::to_string(part.size());
    result += ':';
    result += part;
  }
  return result;
}

[[nodiscard]] std::string mailCaseIdentity(const MailObservation &observation,
                                           const MailMessageEvidence &message) {
  return identityTuple({observation.serverAuthority, observation.accountScope,
                        observation.mailbox,
                        std::to_string(message.uidValidity),
                        std::to_string(message.uid)});
}

[[nodiscard]] std::string ircEventIdentity(const IrcObservation &observation,
                                           const IrcMessageEvidence &message) {
  if (message.messageId)
    return identityTuple({observation.serverAuthority,
                          observation.networkIncarnation, message.target,
                          "msgid=" + *message.messageId});
  return identityTuple({observation.serverAuthority,
                        observation.networkIncarnation,
                        observation.connectionIncarnation,
                        "seq=" + std::to_string(message.localSequence)});
}

[[nodiscard]] std::string ircEventIdentity(const Record &record) {
  const auto get = [&](const std::string &name) -> std::string {
    const auto found = record.attributes.find(name);
    return found == record.attributes.end() ? std::string{} : found->second;
  };
  const auto msgid = get("msgid");
  if (!msgid.empty())
    return identityTuple({get("server-authority"), get("network-incarnation"),
                          get("target"), "msgid=" + msgid});
  return identityTuple({get("server-authority"), get("network-incarnation"),
                        get("connection-incarnation"),
                        "seq=" + get("local-sequence")});
}

[[nodiscard]] std::string fediIdentityKey(const std::string_view uri,
                                          const std::string_view revision) {
  return "object-observation:" + identityTuple({uri, revision});
}

} // namespace

CommunicationRetention
retainMailObservation(EvidenceCase &evidence,
                      const MailObservation &observation) {
  validateBounds(observation.bounds);
  Budget budget(observation.bounds);
  validateCommon(budget, observation.serverAuthority, observation.accountScope,
                 observation.mailbox, observation.coverage);
  charge(budget, observation.mailboxIncarnation);
  if (observation.messages.size() + observation.gaps.size() >
      observation.bounds.maxRecords) {
    throw std::invalid_argument("mail observation item limit exceeded");
  }

  std::set<std::pair<std::uint64_t, std::uint32_t>> messageIds;
  const auto priorRecords = evidence.records();
  std::unordered_map<std::string, Record> priorMessages;
  for (const auto &record : priorRecords) {
    if (record.kind != "mail-message") continue;
    const auto get = [&](const std::string &name) -> std::string {
      const auto found = record.attributes.find(name);
      return found == record.attributes.end() ? std::string{} : found->second;
    };
    const auto identity =
        identityTuple({get("server-authority"), get("account-scope"),
                       get("mailbox"), get("uidvalidity"), get("uid")});
    if (!identity.empty()) priorMessages.emplace(identity, record);
  }
  RecordPlan recordPlan(observation.bounds.maxRecords);
  recordPlan.add();
  for (const auto &message : observation.messages) {
    recordPlan.add(2 + static_cast<std::size_t>(!message.rawMessage.empty()) +
                   message.headers.size() + message.recipients.size() +
                   (message.flags ? message.flags->size() : 0));
    if (message.uidValidity == 0 || message.uid == 0 ||
        !messageIds.emplace(message.uidValidity, message.uid).second) {
      throw std::invalid_argument(
          "mail message identity is missing or repeated");
    }
    if (const auto previous =
            priorMessages.find(mailCaseIdentity(observation, message));
        previous != priorMessages.end()) {
      const auto messageId = previous->second.attributes.find("message-id");
      if (previous->second.text != message.subject ||
          (messageId != previous->second.attributes.end() &&
           messageId->second != message.messageId)) {
        throw std::invalid_argument(
            "mail UID identity has conflicting message metadata");
      }
      if (!message.rawMessage.empty()) {
        const auto raw =
            std::ranges::find_if(priorRecords, [&](const Record &record) {
              return record.kind == "mail-raw-message" &&
                     record.parent == previous->second.id;
            });
        if (raw != priorRecords.end() && raw->text != message.rawMessage) {
          throw std::invalid_argument(
              "mail UID identity has conflicting raw message bytes");
        }
      }
    }
    charge(budget, message.messageId);
    charge(budget, message.subject);
    charge(budget, message.rawMessage);
    if (message.flags) {
      for (const auto &flag : *message.flags) {
        if (flag.empty()) throw std::invalid_argument("mail flag is empty");
        charge(budget, flag);
      }
    }
    if (message.parts.size() > observation.bounds.maxParts) {
      throw std::invalid_argument("mail MIME part limit exceeded");
    }
    for (const auto &part : message.parts) {
      recordPlan.add(1 + static_cast<std::size_t>(!part.rawBytes.empty()) +
                     static_cast<std::size_t>(part.decodedBytes.has_value()) +
                     static_cast<std::size_t>(part.decodedText.has_value()));
      recordPlan.add(part.headers.size() + part.embeddedMessageHeaders.size());
    }
    std::unordered_map<std::string, const MailMimePart *> parts;
    for (const auto &part : message.parts) {
      if (part.key.empty() || !parts.emplace(part.key, &part).second) {
        throw std::invalid_argument(
            "mail MIME part key is missing or repeated");
      }
      charge(budget, part.key);
      charge(budget, part.contentType);
      charge(budget, part.disposition);
      charge(budget, part.charset);
      charge(budget, part.transferEncoding);
      charge(budget, part.filename);
      charge(budget, part.contentId);
      charge(budget, part.contentLocation);
      charge(budget, part.contentDescription);
      charge(budget, part.charsetConversionStatus);
      charge(budget, part.rawBytes);
      if (part.decodedBytes) charge(budget, *part.decodedBytes);
      if (part.decodedText) charge(budget, *part.decodedText);
      for (const auto &header : part.headers) {
        if (header.name.empty())
          throw std::invalid_argument("MIME part header name is empty");
        charge(budget, header.name);
        charge(budget, header.value);
        charge(budget, header.rawValue);
      }
      for (const auto &header : part.embeddedMessageHeaders) {
        if (header.name.empty())
          throw std::invalid_argument("embedded message header name is empty");
        charge(budget, header.name);
        charge(budget, header.value);
        charge(budget, header.rawValue);
      }
    }
    for (const auto &part : message.parts) {
      std::unordered_set<std::string> seen{part.key};
      auto parent       = part.parentKey;
      std::size_t depth = 1;
      while (parent) {
        if (!seen.insert(*parent).second || !parts.contains(*parent)) {
          throw std::invalid_argument(
              "mail MIME part tree has an invalid parent");
        }
        parent = parts.at(*parent)->parentKey;
        if (++depth > observation.bounds.maxPartDepth) {
          throw std::invalid_argument("mail MIME part depth limit exceeded");
        }
      }
    }
    for (const auto &header : message.headers) {
      if (header.name.empty())
        throw std::invalid_argument("mail header name is empty");
      charge(budget, header.name);
      charge(budget, header.value);
      charge(budget, header.rawValue);
    }
    for (const auto &recipient : message.recipients) {
      if (recipient.role.empty() || recipient.address.empty()) {
        throw std::invalid_argument("mail recipient occurrence is incomplete");
      }
      charge(budget, recipient.role);
      charge(budget, recipient.address);
    }
  }
  for (const auto &gap : observation.gaps) {
    recordPlan.add();
    charge(budget, gap.label);
    charge(budget, gap.detail);
    charge(budget, gap.cursor);
  }

  CommunicationRetention retained;
  retained.records.reserve(observation.messages.size() * 4 +
                           observation.gaps.size());
  const auto root =
      append(evidence, budget, "mail-observation", observation.mailbox,
             observation.coverage, {},
             {{"state", stateText(observation.state)},
              {"server-authority", observation.serverAuthority},
              {"account-scope", observation.accountScope},
              {"mailbox", observation.mailbox},
              {"mailbox-incarnation", observation.mailboxIncarnation},
              {"message-count", std::to_string(observation.messages.size())},
              {"gap-count", std::to_string(observation.gaps.size())}});
  retained.observation = root;
  for (const auto &message : observation.messages) {
    const auto key            = mailIdentity(message);
    const auto stableIdentity = mailCaseIdentity(observation, message);
    const auto previous       = priorMessages.find(stableIdentity);
    const bool isNew          = previous == priorMessages.end();
    const auto id =
        isNew
            ? append(
                  evidence, budget, "mail-message", key, message.subject, root,
                  {{"server-authority", observation.serverAuthority},
                   {"account-scope", observation.accountScope},
                   {"mailbox", observation.mailbox},
                   {"mailbox-incarnation", observation.mailboxIncarnation},
                   {"uidvalidity", std::to_string(message.uidValidity)},
                   {"uid", std::to_string(message.uid)},
                   {"sequence-number", std::to_string(message.sequenceNumber)},
                   {"message-id", message.messageId}})
            : previous->second.id;
    retained.records.emplace(key, id);
    const auto seenId =
        append(evidence, budget, "mail-message-seen", key + ":seen",
               "Observed mailbox message occurrence", root,
               {{"mailbox-incarnation", observation.mailboxIncarnation},
                {"sequence-number", std::to_string(message.sequenceNumber)},
                {"duplicate", isNew ? "no" : "yes"},
                {"flags-state", message.flags ? "observed" : "unavailable"},
                {"canonical-record-id", std::to_string(id)}},
               {{"d.evidence.target", id}});
    retained.records.emplace(key + ":seen", seenId);
    const auto payloadParent = isNew ? id : seenId;
    if (message.flags) {
      for (std::size_t index = 0; index < message.flags->size(); ++index) {
        const auto &flag = message.flags->at(index);
        const auto flagId =
            append(evidence, budget, "mail-flag", flag, flag, seenId,
                   {{"occurrence", std::to_string(index)}});
        retained.records.emplace(key + ":flag:" + std::to_string(index),
                                 flagId);
      }
    }
    if (!message.rawMessage.empty()) {
      const auto rawId = append(evidence, budget, "mail-raw-message", "raw",
                                message.rawMessage, payloadParent,
                                {{"representation", "rfc5322-bytes"}});
      retained.records.emplace(key + ":raw", rawId);
    }
    std::size_t occurrence = 0;
    for (const auto &header : message.headers) {
      const auto headerKey = key + ":header:" + std::to_string(occurrence++);
      retained.records.emplace(
          headerKey, append(evidence, budget, "mail-header", header.name,
                            header.value, payloadParent,
                            {{"occurrence", std::to_string(occurrence - 1)},
                             {"raw-value", header.rawValue}}));
    }
    occurrence = 0;
    for (const auto &recipient : message.recipients) {
      const auto recipientKey =
          key + ":recipient:" + std::to_string(occurrence++);
      retained.records.emplace(
          recipientKey,
          append(evidence, budget, "mail-recipient", recipient.address,
                 recipient.address, payloadParent,
                 {{"role", recipient.role},
                  {"occurrence", std::to_string(occurrence - 1)}}));
    }

    std::vector<const MailMimePart *> orderedParts;
    orderedParts.reserve(message.parts.size());
    for (const auto &part : message.parts) orderedParts.push_back(&part);
    const auto depthOf = [&](const MailMimePart *part) {
      std::size_t depth = 1;
      auto parent       = part->parentKey;
      while (parent) {
        ++depth;
        parent = std::ranges::find(message.parts, *parent, &MailMimePart::key)
                     ->parentKey;
      }
      return depth;
    };
    std::ranges::stable_sort(orderedParts, {}, depthOf);
    for (const auto *part : orderedParts) {
      const auto partParent =
          part->parentKey
              ? retained.records.at(key + ":part:" + *part->parentKey)
              : payloadParent;
      const auto partKey = key + ":part:" + part->key;
      const auto partId  = append(
          evidence, budget, "mail-mime-part", part->key, {}, partParent,
          {{"content-type", part->contentType},
            {"disposition", part->disposition},
            {"charset", part->charset},
            {"transfer-encoding", part->transferEncoding},
            {"filename", part->filename},
            {"content-id", part->contentId},
            {"content-location", part->contentLocation},
            {"content-description", part->contentDescription},
            {"charset-conversion-status", part->charsetConversionStatus}});
      retained.records.emplace(partKey, partId);
      std::size_t partHeaderIndex = 0;
      for (const auto &header : part->headers) {
        const auto headerKey =
            partKey + ":header:" + std::to_string(partHeaderIndex++);
        retained.records.emplace(
            headerKey,
            append(evidence, budget, "mail-mime-header", header.name,
                   header.value, partId,
                   {{"occurrence", std::to_string(partHeaderIndex - 1)},
                    {"raw-value", header.rawValue},
                    {"header-scope", "mime-part"}}));
      }
      std::size_t embeddedHeaderIndex = 0;
      for (const auto &header : part->embeddedMessageHeaders) {
        const auto headerKey = partKey + ":embedded-header:" +
                               std::to_string(embeddedHeaderIndex++);
        retained.records.emplace(
            headerKey,
            append(evidence, budget, "mail-embedded-header", header.name,
                   header.value, partId,
                   {{"occurrence", std::to_string(embeddedHeaderIndex - 1)},
                    {"raw-value", header.rawValue},
                    {"header-scope", "embedded-message"}}));
      }
      if (!part->rawBytes.empty()) {
        retained.records.emplace(
            partKey + ":raw",
            append(evidence, budget, "mail-mime-raw", "raw part bytes",
                   part->rawBytes, partId,
                   {{"representation", "source-part-bytes"}}));
      }
      if (part->decodedBytes) {
        retained.records.emplace(
            partKey + ":decoded-bytes",
            append(evidence, budget, "mail-mime-decoded-bytes",
                   "transfer-decoded bytes", *part->decodedBytes, partId,
                   {{"representation", "transfer-decoded-bytes"}}));
      }
      if (part->decodedText) {
        retained.records.emplace(partKey + ":decoded",
                                 append(evidence, budget, "mail-mime-text",
                                        "decoded text", *part->decodedText,
                                        partId,
                                        {{"representation", "decoded-text"},
                                         {"charset", part->charset}}));
      }
    }
  }
  std::size_t gapIndex = 0;
  for (const auto &gap : observation.gaps) {
    const auto key = "mail-gap:" + std::to_string(gapIndex++);
    retained.records.emplace(
        key, append(evidence, budget, "mail-gap", gap.label, gap.detail, root,
                    {{"cursor", gap.cursor}, {"state", "unavailable"}}));
  }
  return retained;
}

CommunicationRetention retainIrcObservation(EvidenceCase &evidence,
                                            const IrcObservation &observation) {
  validateBounds(observation.bounds);
  Budget budget(observation.bounds);
  if (observation.serverAuthority.empty() ||
      observation.networkIncarnation.empty() ||
      observation.connectionIncarnation.empty()) {
    throw std::invalid_argument("IRC observation identity is empty");
  }
  charge(budget, observation.serverAuthority);
  charge(budget, observation.networkIncarnation);
  charge(budget, observation.connectionIncarnation);
  if (observation.messages.size() + observation.gaps.size() >
      observation.bounds.maxRecords) {
    throw std::invalid_argument("IRC observation item limit exceeded");
  }
  for (const auto &capability : observation.capabilities)
    charge(budget, capability);
  RecordPlan recordPlan(observation.bounds.maxRecords);
  recordPlan.add(1 + observation.capabilities.size());
  const auto priorRecords = evidence.records();
  std::unordered_map<std::string, Record> priorEvents;
  for (const auto &record : priorRecords) {
    if (record.kind == "irc-message")
      priorEvents.emplace(ircEventIdentity(record), record);
  }
  std::unordered_map<std::string, const IrcMessageEvidence *> identities;
  for (const auto &message : observation.messages) {
    recordPlan.add(2 + message.tags.size());
    if (message.target.empty() || message.command.empty() ||
        (!message.messageId && message.localSequence == 0)) {
      throw std::invalid_argument("IRC event identity or target is missing");
    }
    const auto identity             = ircEventIdentity(observation, message);
    const auto [existing, inserted] = identities.emplace(identity, &message);
    if (!inserted && (existing->second->sender != message.sender ||
                      existing->second->command != message.command ||
                      existing->second->text != message.text ||
                      existing->second->serverTime != message.serverTime ||
                      existing->second->tags != message.tags)) {
      throw std::invalid_argument(
          "duplicate IRC identity has conflicting event content");
    }
    if (const auto prior = priorEvents.find(identity);
        prior != priorEvents.end()) {
      const auto attr = [&](const std::string &name) -> std::string {
        const auto found = prior->second.attributes.find(name);
        return found == prior->second.attributes.end() ? std::string{}
                                                       : found->second;
      };
      std::map<std::string, std::string> priorTags;
      for (const auto &record : priorRecords) {
        if (record.kind == "irc-message-tag" &&
            record.parent == prior->second.id)
          priorTags.emplace(record.label, record.text);
      }
      if (prior->second.text != message.text ||
          attr("sender") != message.sender ||
          attr("command") != message.command ||
          attr("server-time") != message.serverTime ||
          priorTags != message.tags) {
        throw std::invalid_argument(
            "IRC message identity has conflicting retained content");
      }
    }
    if (message.messageId) charge(budget, *message.messageId);
    charge(budget, message.target);
    charge(budget, message.sender);
    charge(budget, message.command);
    charge(budget, message.text);
    charge(budget, message.serverTime);
    for (const auto &[key, value] : message.tags) {
      charge(budget, key);
      charge(budget, value);
    }
  }
  for (const auto &gap : observation.gaps) {
    recordPlan.add();
    charge(budget, gap.fromCursor);
    charge(budget, gap.toCursor);
    charge(budget, gap.reason);
  }

  CommunicationRetention retained;
  const auto root =
      append(evidence, budget, "irc-observation", observation.serverAuthority,
             "IRC session observation", {},
             {{"state", stateText(observation.state)},
              {"network-incarnation", observation.networkIncarnation},
              {"connection-incarnation", observation.connectionIncarnation},
              {"event-count", std::to_string(observation.messages.size())},
              {"gap-count", std::to_string(observation.gaps.size())}});
  retained.observation = root;
  for (const auto &capability : observation.capabilities) {
    const auto id = append(evidence, budget, "irc-capability", capability,
                           capability, root);
    retained.records.emplace("capability:" + capability, id);
  }

  std::unordered_map<std::string, RecordId> canonicalEvents;
  for (const auto &[identity, record] : priorEvents)
    canonicalEvents.emplace(identity, record.id);
  std::size_t index = 0;
  for (const auto &message : observation.messages) {
    const auto identity  = ircEventIdentity(observation, message);
    auto found           = canonicalEvents.find(identity);
    const bool duplicate = found != canonicalEvents.end();
    RecordId eventId{};
    if (!duplicate) {
      eventId =
          append(evidence, budget, "irc-message", identity, message.text, root,
                 {{"server-authority", observation.serverAuthority},
                  {"network-incarnation", observation.networkIncarnation},
                  {"connection-incarnation", observation.connectionIncarnation},
                  {"target", message.target},
                  {"sender", message.sender},
                  {"command", message.command},
                  {"server-time", message.serverTime},
                  {"msgid", message.messageId.value_or("")},
                  {"local-sequence", std::to_string(message.localSequence)}});
      canonicalEvents.emplace(identity, eventId);
      retained.records.emplace("message:" + identity, eventId);
      for (const auto &[key, value] : message.tags) {
        const auto tagId =
            append(evidence, budget, "irc-message-tag", key, value, eventId);
        retained.records.emplace("message-tag:" + identity + ":" + key, tagId);
      }
    } else {
      eventId = found->second;
    }
    retained.records.emplace("message:" + identity, eventId);
    const auto deliveryKey = "delivery:" + std::to_string(index++);
    const auto delivery = append(evidence, budget, "irc-delivery", deliveryKey,
                                 "Observed delivery occurrence", root,
                                 {{"identity", identity},
                                  {"duplicate", duplicate ? "yes" : "no"},
                                  {"arrival-index", std::to_string(index - 1)}},
                                 {{"d.evidence.target", eventId}});
    retained.records.emplace(deliveryKey, delivery);
  }
  std::size_t gapIndex = 0;
  for (const auto &gap : observation.gaps) {
    const auto key = "gap:" + std::to_string(gapIndex++);
    retained.records.emplace(key, append(evidence, budget, "irc-gap", key,
                                         gap.reason, root,
                                         {{"from-cursor", gap.fromCursor},
                                          {"to-cursor", gap.toCursor},
                                          {"reason", gap.reason},
                                          {"coverage", "incomplete"}}));
  }
  return retained;
}

CommunicationRetention
retainFediverseObservation(EvidenceCase &evidence,
                           const FediverseObservation &observation) {
  validateBounds(observation.bounds);
  Budget budget(observation.bounds);
  validateCommon(budget, observation.instanceAuthority, observation.accountId,
                 observation.collectionUri, observation.coverage);
  charge(budget, observation.accountActorUri);
  const auto inputCount =
      observation.actors.size() + observation.objects.size() +
      observation.activities.size() + observation.deliveries.size();
  if (inputCount > observation.bounds.maxRecords) {
    throw std::invalid_argument("fediverse observation item limit exceeded");
  }
  const auto priorRecords = evidence.records();
  std::unordered_map<std::string, Record> priorActors;
  std::unordered_map<std::string, Record> priorObjects;
  std::unordered_map<std::string, Record> priorActivities;
  for (const auto &record : priorRecords) {
    if (record.kind == "fediverse-actor") {
      const auto uri = record.attributes.find("actor-uri");
      if (uri != record.attributes.end())
        priorActors.emplace(uri->second, record);
    } else if (record.kind == "fediverse-object") {
      const auto uri = record.attributes.find("object-uri");
      if (uri != record.attributes.end())
        priorObjects.emplace(uri->second, record);
    } else if (record.kind == "fediverse-activity") {
      const auto uri = record.attributes.find("activity-uri");
      if (uri != record.attributes.end())
        priorActivities.emplace(uri->second, record);
    }
  }
  std::set<std::string> actorUris;
  RecordPlan recordPlan(observation.bounds.maxRecords);
  recordPlan.add();
  for (const auto &actor : observation.actors) {
    recordPlan.add(2);
    if (actor.actorUri.empty() || !actorUris.insert(actor.actorUri).second) {
      throw std::invalid_argument(
          "fediverse actor identity is missing or repeated");
    }
    charge(budget, actor.actorUri);
    charge(budget, actor.accountId);
    charge(budget, actor.handle);
    charge(budget, actor.displayName);
    charge(budget, actor.identityBasis);
    charge(budget, actor.profileUrl);
  }
  std::map<std::pair<std::string, std::string>, const FediverseObjectEvidence *>
      objectRevisions;
  for (const auto &object : observation.objects) {
    recordPlan.add(3 + object.audience.size());
    if (object.objectUri.empty() || object.revision.empty()) {
      throw std::invalid_argument("fediverse object revision is missing");
    }
    const auto [prior, inserted] = objectRevisions.emplace(
        std::pair{object.objectUri, object.revision}, &object);
    if (!inserted && (prior->second->restId != object.restId ||
                      prior->second->actorUri != object.actorUri ||
                      prior->second->type != object.type ||
                      prior->second->content != object.content ||
                      prior->second->visibility != object.visibility ||
                      prior->second->contentWarning != object.contentWarning ||
                      prior->second->inReplyToUri != object.inReplyToUri ||
                      prior->second->audience != object.audience)) {
      throw std::invalid_argument(
          "duplicate fediverse object revision has conflicting content");
    }
    charge(budget, object.objectUri);
    charge(budget, object.restId);
    charge(budget, object.actorUri);
    charge(budget, object.type);
    charge(budget, object.content);
    charge(budget, object.visibility);
    charge(budget, object.contentWarning);
    charge(budget, object.revision);
    if (object.inReplyToUri) charge(budget, *object.inReplyToUri);
    for (const auto &recipient : object.audience) charge(budget, recipient);
    if (const auto prior = priorObjects.find(object.objectUri);
        prior != priorObjects.end()) {
      for (const auto &record : priorRecords) {
        if (record.kind != "fediverse-object-observation" ||
            record.parent != prior->second.id)
          continue;
        const auto revision = record.attributes.find("revision");
        if (revision == record.attributes.end() ||
            revision->second != object.revision)
          continue;
        const auto getVersion = [&](const std::string &name) {
          const auto found = record.attributes.find(name);
          return found == record.attributes.end() ? std::string{}
                                                  : found->second;
        };
        std::set<std::string> priorAudience;
        for (const auto &child : priorRecords) {
          if (child.kind == "fediverse-audience" && child.parent == record.id &&
              child.attributes.contains("visibility"))
            priorAudience.insert(child.label);
        }
        const std::set<std::string> nextAudience(object.audience.begin(),
                                                 object.audience.end());
        if (record.text != object.content ||
            getVersion("rest-id") != object.restId ||
            getVersion("type") != object.type ||
            getVersion("visibility") != object.visibility ||
            getVersion("content-warning") != object.contentWarning ||
            getVersion("actor-uri") != object.actorUri ||
            getVersion("in-reply-to-uri") != object.inReplyToUri.value_or("") ||
            priorAudience != nextAudience) {
          throw std::invalid_argument(
              "fediverse object revision conflicts with retained content");
        }
      }
    }
  }
  std::map<std::string, const FediverseActivityEvidence *> activityUris;
  for (const auto &activity : observation.activities) {
    if (activity.activityUri.empty() || activity.type.empty()) {
      throw std::invalid_argument("fediverse activity identity is missing");
    }
    const auto [existing, inserted] =
        activityUris.emplace(activity.activityUri, &activity);
    if (!inserted && (existing->second->type != activity.type ||
                      existing->second->actorUri != activity.actorUri ||
                      existing->second->objectUri != activity.objectUri ||
                      existing->second->published != activity.published ||
                      existing->second->audience != activity.audience)) {
      throw std::invalid_argument(
          "duplicate fediverse activity identity has conflicting content");
    }
    if (inserted) recordPlan.add(3 + activity.audience.size());
    charge(budget, activity.activityUri);
    charge(budget, activity.type);
    charge(budget, activity.actorUri);
    charge(budget, activity.objectUri);
    charge(budget, activity.published);
    for (const auto &recipient : activity.audience) charge(budget, recipient);
    if (const auto prior = priorActivities.find(activity.activityUri);
        prior != priorActivities.end()) {
      const auto get = [&](const std::string &name) {
        const auto found = prior->second.attributes.find(name);
        return found == prior->second.attributes.end() ? std::string{}
                                                       : found->second;
      };
      if (prior->second.text != activity.type ||
          get("actor-uri") != activity.actorUri ||
          get("object-uri") != activity.objectUri ||
          get("published") != activity.published) {
        throw std::invalid_argument(
            "fediverse activity URI has conflicting retained content");
      }
      std::set<std::string> priorAudience;
      for (const auto &record : priorRecords) {
        if (record.kind == "fediverse-audience" &&
            record.parent == prior->second.id &&
            record.attributes.contains("role") &&
            record.attributes.at("role") == "activity-audience")
          priorAudience.insert(record.label);
      }
      const std::set<std::string> nextAudience(activity.audience.begin(),
                                               activity.audience.end());
      if (priorAudience != nextAudience)
        throw std::invalid_argument(
            "fediverse activity URI has conflicting audience");
    }
  }
  for (const auto &delivery : observation.deliveries) {
    recordPlan.add();
    charge(budget, delivery.activityUri);
    charge(budget, delivery.collectionUri);
  }
  std::set<std::string> resolvingActorUris;
  std::set<std::string> resolvingObjectUris;
  for (const auto &entry : priorActors) resolvingActorUris.insert(entry.first);
  for (const auto &entry : priorObjects)
    resolvingObjectUris.insert(entry.first);
  for (const auto &actor : observation.actors)
    resolvingActorUris.insert(actor.actorUri);
  for (const auto &object : observation.objects)
    resolvingObjectUris.insert(object.objectUri);
  std::set<std::string> alreadyResolved;
  for (const auto &record : priorRecords) {
    if (record.kind != "fediverse-reference-resolution") continue;
    if (const auto found = record.attributes.find("unresolved-record-id");
        found != record.attributes.end())
      alreadyResolved.insert(found->second);
  }
  std::vector<Record> pendingResolutions;
  for (const auto &record : priorRecords) {
    if (record.attributes.find("resolution") == record.attributes.end() ||
        record.attributes.at("resolution") != "unresolved" ||
        (record.kind != "fediverse-object-reference" &&
         record.kind != "fediverse-actor-reference"))
      continue;
    const auto id = std::to_string(record.id);
    if (alreadyResolved.contains(id)) continue;
    const bool isObject     = record.kind == "fediverse-object-reference";
    const auto uriAttribute = isObject ? "object-uri" : "actor-uri";
    const auto uri          = record.attributes.find(uriAttribute);
    if (uri == record.attributes.end()) continue;
    const auto &incoming = isObject ? resolvingObjectUris : resolvingActorUris;
    if (!incoming.contains(uri->second)) continue;
    pendingResolutions.push_back(record);
    recordPlan.add();
    charge(budget, uri->second);
  }
  recordPlan.add(observation.activities.size());

  CommunicationRetention retained;
  const auto root =
      append(evidence, budget, "fediverse-observation",
             observation.collectionUri, observation.coverage, {},
             {{"state", stateText(observation.state)},
              {"instance-authority", observation.instanceAuthority},
              {"account-id", observation.accountId},
              {"account-actor-uri", observation.accountActorUri},
              {"collection-uri", observation.collectionUri},
              {"item-count", std::to_string(inputCount)}});
  retained.observation = root;

  std::unordered_map<std::string, RecordId> actors;
  for (const auto &[uri, record] : priorActors) actors.emplace(uri, record.id);
  for (const auto &actor : observation.actors) {
    if (actors.contains(actor.actorUri)) {
      retained.records.emplace("actor:" + actor.actorUri,
                               actors.at(actor.actorUri));
    } else {
      const auto key = "actor:" + actor.actorUri;
      const auto id =
          append(evidence, budget, "fediverse-actor", actor.actorUri,
                 actor.displayName, root,
                 {{"actor-uri", actor.actorUri},
                  {"instance-authority", observation.instanceAuthority}});
      actors.emplace(actor.actorUri, id);
      retained.records.emplace(key, id);
    }
    const auto profileId =
        append(evidence, budget, "fediverse-actor-observation", actor.actorUri,
               actor.displayName, root,
               {{"actor-uri", actor.actorUri},
                {"instance-authority", observation.instanceAuthority},
                {"account-id", actor.accountId},
                {"handle", actor.handle},
                {"display-name", actor.displayName},
                {"identity-basis", actor.identityBasis},
                {"profile-url", actor.profileUrl}},
               {{"d.evidence.target", actors.at(actor.actorUri)}});
    retained.records.emplace("actor-observation:" + actor.actorUri + ":" +
                                 std::to_string(profileId),
                             profileId);
  }

  std::unordered_map<std::string, RecordId> objects;
  std::unordered_map<std::string, RecordId> objectObservations;
  for (const auto &[uri, record] : priorObjects) {
    objects.emplace(uri, record.id);
    for (const auto &prior : priorRecords) {
      if (prior.kind != "fediverse-object-observation" ||
          prior.parent != record.id)
        continue;
      const auto revision = prior.attributes.find("revision");
      if (revision != prior.attributes.end())
        objectObservations.emplace(fediIdentityKey(uri, revision->second),
                                   prior.id);
    }
  }
  std::size_t objectOccurrence = 0;
  for (const auto &object : observation.objects) {
    auto found = objects.find(object.objectUri);
    RecordId objectId{};
    if (found == objects.end()) {
      objectId = append(evidence, budget, "fediverse-object", object.objectUri,
                        object.type, root,
                        {{"object-uri", object.objectUri},
                         {"rest-id", object.restId},
                         {"instance-authority", observation.instanceAuthority},
                         {"actor-uri", object.actorUri}});
      objects.emplace(object.objectUri, objectId);
      retained.records.emplace("object:" + object.objectUri, objectId);
    } else {
      objectId = found->second;
    }
    retained.records.emplace("object:" + object.objectUri, objectId);
    const auto observationKey =
        fediIdentityKey(object.objectUri, object.revision);
    auto version = objectObservations.find(observationKey);
    RecordId versionId{};
    if (version == objectObservations.end()) {
      versionId =
          append(evidence, budget, "fediverse-object-observation",
                 object.revision, object.content, objectId,
                 {{"revision", object.revision},
                  {"rest-id", object.restId},
                  {"type", object.type},
                  {"visibility", object.visibility},
                  {"content-warning", object.contentWarning},
                  {"actor-uri", object.actorUri},
                  {"in-reply-to-uri", object.inReplyToUri.value_or("")}});
      objectObservations.emplace(observationKey, versionId);
      retained.records.emplace(observationKey, versionId);
      for (const auto &recipient : object.audience) {
        const auto audienceId =
            append(evidence, budget, "fediverse-audience", recipient, recipient,
                   versionId, {{"visibility", object.visibility}});
        retained.records.emplace(observationKey + ":audience:" + recipient,
                                 audienceId);
      }
    } else {
      versionId = version->second;
    }
    retained.records.emplace(observationKey, versionId);
    const auto seenId =
        append(evidence, budget, "fediverse-object-seen", object.objectUri,
               "Object observation occurrence", root,
               {{"revision", object.revision},
                {"occurrence", std::to_string(objectOccurrence++)}},
               {{"d.evidence.target", versionId}});
    retained.records.emplace(
        observationKey + ":seen:" + std::to_string(objectOccurrence - 1),
        seenId);
  }

  for (const auto &unresolved : pendingResolutions) {
    const bool isObject     = unresolved.kind == "fediverse-object-reference";
    const auto uriAttribute = isObject ? "object-uri" : "actor-uri";
    const auto uri          = unresolved.attributes.at(uriAttribute);
    const auto &entities    = isObject ? objects : actors;
    const auto entity       = entities.find(uri);
    if (entity == entities.end()) continue;
    const auto resolutionId = std::to_string(unresolved.id);
    const auto id = append(evidence, budget, "fediverse-reference-resolution",
                           isObject ? "object reference resolution"
                                    : "actor reference resolution",
                           uri, unresolved.id,
                           {{"unresolved-record-id", resolutionId},
                            {"uri", uri},
                            {"entity-kind", isObject ? "object" : "actor"}},
                           {{"d.evidence.target", entity->second}});
    retained.records.emplace("resolution:" + resolutionId, id);
  }

  std::unordered_map<std::string, RecordId> activities;
  for (const auto &[uri, record] : priorActivities)
    activities.emplace(uri, record.id);
  for (const auto &activity : observation.activities) {
    if (activities.contains(activity.activityUri)) {
      retained.records.emplace("activity:" + activity.activityUri,
                               activities.at(activity.activityUri));
      continue;
    }
    const auto activityId =
        append(evidence, budget, "fediverse-activity", activity.activityUri,
               activity.type, root,
               {{"activity-uri", activity.activityUri},
                {"type", activity.type},
                {"actor-uri", activity.actorUri},
                {"object-uri", activity.objectUri},
                {"published", activity.published},
                {"instance-authority", observation.instanceAuthority}});
    activities.emplace(activity.activityUri, activityId);
    retained.records.emplace("activity:" + activity.activityUri, activityId);
    if (const auto actor = actors.find(activity.actorUri);
        actor != actors.end()) {
      const auto linkId =
          append(evidence, budget, "fediverse-actor-reference", "actor", {},
                 activityId, {{"actor-uri", activity.actorUri}},
                 {{"d.evidence.target", actor->second}});
      retained.records.emplace("activity-actor:" + activity.activityUri,
                               linkId);
    } else {
      const auto linkId = append(
          evidence, budget, "fediverse-actor-reference", "unresolved actor",
          activity.actorUri, activityId,
          {{"actor-uri", activity.actorUri}, {"resolution", "unresolved"}});
      retained.records.emplace("activity-actor:" + activity.activityUri,
                               linkId);
    }
    if (const auto object = objects.find(activity.objectUri);
        object != objects.end()) {
      const auto linkId =
          append(evidence, budget, "fediverse-object-reference", "object", {},
                 activityId, {{"object-uri", activity.objectUri}},
                 {{"d.evidence.target", object->second}});
      retained.records.emplace("activity-object:" + activity.activityUri,
                               linkId);
    } else {
      const auto linkId = append(
          evidence, budget, "fediverse-object-reference", "unresolved object",
          activity.objectUri, activityId,
          {{"object-uri", activity.objectUri}, {"resolution", "unresolved"}});
      retained.records.emplace("activity-object:" + activity.activityUri,
                               linkId);
    }
    std::size_t recipientIndex = 0;
    for (const auto &recipient : activity.audience) {
      const auto recipientId =
          append(evidence, budget, "fediverse-audience", recipient, recipient,
                 activityId,
                 {{"occurrence", std::to_string(recipientIndex++)},
                  {"role", "activity-audience"}});
      retained.records.emplace("activity-audience:" + activity.activityUri +
                                   ":" + std::to_string(recipientIndex - 1),
                               recipientId);
    }
  }

  // Each observation occurrence remains distinct even when ActivityPub
  // identity says that multiple fetches saw the same activity.
  std::unordered_map<std::string, std::size_t> occurrenceCounts;
  for (const auto &record : priorRecords) {
    if (record.kind != "fediverse-delivery") continue;
    const auto uri = record.attributes.find("activity-uri");
    if (uri != record.attributes.end()) ++occurrenceCounts[uri->second];
  }
  for (const auto &activity : observation.activities) {
    const auto occurrence = occurrenceCounts[activity.activityUri]++;
    const auto deliveryId =
        append(evidence, budget, "fediverse-delivery", activity.activityUri,
               "Observed activity occurrence", root,
               {{"activity-uri", activity.activityUri},
                {"collection-uri", observation.collectionUri},
                {"occurrence", std::to_string(occurrence)},
                {"duplicate", occurrence == 0 ? "no" : "yes"}},
               {{"d.evidence.target", activities.at(activity.activityUri)}});
    retained.records.emplace("delivery:" + activity.activityUri + ":" +
                                 std::to_string(occurrence),
                             deliveryId);
  }
  for (const auto &delivery : observation.deliveries) {
    const auto activity   = activities.find(delivery.activityUri);
    const auto occurrence = occurrenceCounts[delivery.activityUri]++;
    const auto refs       = activity == activities.end()
                                ? std::vector<std::pair<std::string, RecordId>>{}
                                : std::vector<std::pair<std::string, RecordId>>{
                                {"d.evidence.target", activity->second}};
    const auto deliveryId =
        append(evidence, budget, "fediverse-delivery", delivery.activityUri,
               "Collection delivery occurrence", root,
               {{"activity-uri", delivery.activityUri},
                {"collection-uri", delivery.collectionUri},
                {"occurrence", std::to_string(delivery.occurrence)},
                {"duplicate", occurrence == 0 ? "no" : "yes"}},
               refs);
    retained.records.emplace("source-delivery:" + delivery.activityUri + ":" +
                                 std::to_string(delivery.occurrence),
                             deliveryId);
  }
  return retained;
}

} // namespace xanadu::evidence
