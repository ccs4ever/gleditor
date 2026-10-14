#include "common/xanadu/content_reference.hpp"

#include <limits>
#include <type_traits>
#include <utility>

#include "common/xanadu/store.hpp"

namespace xanadu {

std::expected<void, ContentReferenceError>
validateContentReference(const ContentReference &reference) {
  if (reference.ranges.empty()) {
    return std::unexpected(ContentReferenceError::EmptySelection);
  }
  if (reference.version.isZero()) {
    return std::unexpected(ContentReferenceError::MissingState);
  }
  if (const auto *document = std::get_if<DocumentFrame>(&reference.frame)) {
    if (document->birth && *document->birth == 0) {
      return std::unexpected(ContentReferenceError::WrongFrame);
    }
  } else if (std::get<CellFrame>(reference.frame).cell == zigzag::noCell ||
             zigzag::isEphemeral(std::get<CellFrame>(reference.frame).cell)) {
    return std::unexpected(ContentReferenceError::WrongFrame);
  }
  if (reference.occurrence &&
      (reference.occurrence->link.authority != reference.authority ||
       reference.occurrence->link.id == zigzag::noCell ||
       zigzag::isEphemeral(reference.occurrence->link.id))) {
    return std::unexpected(ContentReferenceError::InvalidOccurrenceIdentity);
  }
  std::optional<Extent> previous;
  for (const auto &entry : reference.ranges) {
    if (entry.source.empty() || isReservedScroll(entry.source.scroll) ||
        entry.source.length >
            std::numeric_limits<std::uint64_t>::max() - entry.source.start) {
      return std::unexpected(ContentReferenceError::InvalidSource);
    }
    if (entry.range.end <= entry.range.start ||
        entry.source.length != entry.range.end - entry.range.start) {
      return std::unexpected(ContentReferenceError::InvalidRange);
    }
    if (previous && entry.range.start < previous->end) {
      return std::unexpected(ContentReferenceError::OverlappingRanges);
    }
    previous = entry.range;
  }
  return {};
}

std::expected<ContentReference, ContentReferenceError>
contentReferenceFor(const Store &sourceRegistry, const LinkKey &link,
                    const LinkMember &member, const std::uint32_t occurrence,
                    ContentFrame frame) {
  if (sourceRegistry.documentId() != link.authority) {
    return std::unexpected(ContentReferenceError::SourceRegistryMismatch);
  }
  const auto authored = sourceRegistry.links().find(link.id);
  if (authored == sourceRegistry.links().end()) {
    return std::unexpected(ContentReferenceError::AuthoredMemberMismatch);
  }
  const auto &members = member.side == LinkSide::Left ? authored->second.left
                                                      : authored->second.right;
  if (member.index >= members.size() || members[member.index] != member.span) {
    return std::unexpected(ContentReferenceError::AuthoredMemberMismatch);
  }
  if (occurrence >= member.occurrences.size()) {
    return std::unexpected(ContentReferenceError::OccurrenceNotFound);
  }
  const auto &chosen = member.occurrences[occurrence];
  if (chosen.coverage != Coverage::Full) {
    return std::unexpected(ContentReferenceError::PartialOccurrence);
  }
  return std::visit(
      [&](const auto &site)
          -> std::expected<ContentReference, ContentReferenceError> {
        using Site = std::decay_t<decltype(site)>;
        if constexpr (std::is_same_v<Site, DocumentSite>) {
          if (!std::holds_alternative<DocumentFrame>(frame)) {
            return std::unexpected(ContentReferenceError::WrongFrame);
          }
        } else {
          const auto *cell = std::get_if<CellFrame>(&frame);
          if (!cell || cell->cell != site.cell) {
            return std::unexpected(ContentReferenceError::WrongFrame);
          }
        }
        ContentReference result{
            .authority  = link.authority,
            .store      = site.store,
            .version    = site.version,
            .frame      = std::move(frame),
            .ranges     = {{.source = member.span, .range = site.range}},
            .occurrence = LinkOccurrenceIdentity{.link       = link,
                                                 .side       = member.side,
                                                 .member     = member.index,
                                                 .occurrence = occurrence}};
        if (auto valid = validateContentReference(result); !valid) {
          return std::unexpected(valid.error());
        }
        return result;
      },
      chosen.site);
}

std::string_view name(const ContentReferenceError error) noexcept {
  switch (error) {
  case ContentReferenceError::EmptySelection:
    return "No selected content";
  case ContentReferenceError::InvalidSource:
    return "Invalid permanent source address";
  case ContentReferenceError::InvalidRange:
    return "Source and occurrence byte lengths differ";
  case ContentReferenceError::OverlappingRanges:
    return "Occurrence ranges overlap or are out of order";
  case ContentReferenceError::WrongFrame:
    return "Occurrence does not match the chosen document or cell frame";
  case ContentReferenceError::MissingState:
    return "Nonempty content has no produced state";
  case ContentReferenceError::InvalidOccurrenceIdentity:
    return "Invalid Link occurrence identity";
  case ContentReferenceError::OccurrenceNotFound:
    return "Chosen occurrence is unavailable";
  case ContentReferenceError::PartialOccurrence:
    return "Partial occurrence has no exact source-offset mapping";
  case ContentReferenceError::SourceRegistryMismatch:
    return "Source address registry does not own this native Link";
  case ContentReferenceError::AuthoredMemberMismatch:
    return "Member does not match a stored native Link endpoint";
  }
  return "Invalid content reference";
}

} // namespace xanadu
