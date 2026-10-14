#ifndef COMMON_XANADU_CONTENT_REFERENCE_HPP
#define COMMON_XANADU_CONTENT_REFERENCE_HPP

#include <expected>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include "common/xanadu/link_occurrences.hpp"

namespace xanadu {

/// nullopt names the legacy top-level concatext, not an unspecified document.
/// A birth names a Xanadoc structure, never a cell or a microversion.
struct DocumentFrame {
  std::optional<std::uint32_t> birth;
  bool operator==(const DocumentFrame &) const = default;
};

struct CellFrame {
  zigzag::CellRef cell;
  bool operator==(const CellFrame &) const = default;
};

using ContentFrame = std::variant<DocumentFrame, CellFrame>;

/// Source addresses and their positions in one exact owning frame.
struct ContentRange {
  PrimediaSpan source;
  Extent range;
  bool operator==(const ContentRange &) const = default;
};

/// Link provenance is separate from content identity and reader location.
struct LinkOccurrenceIdentity {
  LinkKey link;
  LinkSide side;
  std::uint32_t member;
  std::uint32_t occurrence;
  bool operator==(const LinkOccurrenceIdentity &) const = default;
};

/**
 * An ephemeral exact byte-content descriptor shared at the native identity/VM
 * host boundary. C++ preserves Store-local scroll slots, native operation
 * identities and checked byte extents before Vortex actions consume them.
 *
 * authority owns the source scroll registry; store owns version and frame.
 * They may differ when a Link points to another store. ranges retain caller
 * order and gaps; no covering extent is substituted. Equal text never enters
 * equality. No persistence, resolution or latest-state fallback occurs here.
 * Media clocks/geometry require a supported locator service and are not
 * inferred from byte ranges. This descriptor does not claim to represent a
 * media cue.
 */
struct ContentReference {
  DocumentId authority;
  DocumentId store;
  MicroversionId version;
  ContentFrame frame;
  std::vector<ContentRange> ranges;
  std::optional<LinkOccurrenceIdentity> occurrence;
  bool operator==(const ContentReference &) const = default;
};

enum class ContentReferenceError : std::uint8_t {
  EmptySelection,
  InvalidSource,
  InvalidRange,
  OverlappingRanges,
  WrongFrame,
  MissingState,
  InvalidOccurrenceIdentity,
  OccurrenceNotFound,
  PartialOccurrence,
  SourceRegistryMismatch,
  AuthoredMemberMismatch,
};

/// Structural validation only. Availability, birth kind and correspondence
/// against replayed content remain the resolving service's responsibility.
[[nodiscard]] std::expected<void, ContentReferenceError>
validateContentReference(const ContentReference &reference);

[[nodiscard]] std::string_view name(ContentReferenceError error) noexcept;

/**
 * Convert one explicitly chosen, full native LinkMember occurrence without
 * widening. sourceRegistry must own the Link and its authored member addresses.
 * PackageOccurrenceIndex's retained-package members contain synthetic slot-zero
 * presentation spans; these are refused. GlobalSpan support is a later service.
 * DocumentSite has no birth field: the caller must pass the frame whose pieces
 * were queried, using OpenView.focusedBirth (or its resolved top-level birth),
 * not a birth inferred from the latest head. Cell sites must match CellFrame.
 * Partial occurrences lack the matched source offset and are refused.
 */
[[nodiscard]] std::expected<ContentReference, ContentReferenceError>
contentReferenceFor(const Store &sourceRegistry, const LinkKey &link,
                    const LinkMember &member, std::uint32_t occurrence,
                    ContentFrame frame);

} // namespace xanadu

#endif // COMMON_XANADU_CONTENT_REFERENCE_HPP
