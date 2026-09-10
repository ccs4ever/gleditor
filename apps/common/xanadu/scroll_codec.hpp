/**
 * @file scroll_codec.hpp
 * @brief The one encoding of a scroll segment, and of the withheld-range
 *        record inside it.
 *
 * A ScrollSegment means the same thing wherever it appears -- in a
 * publication's manifest, in a link package's scroll table, and in a store's
 * own registry -- so it is encoded here once rather than wherever it is first
 * needed. It had been written twice, and the two copies had already drifted:
 * publication.cpp's carried a segment's `kind` and its `holeRecord` and
 * link_package.cpp's silently dropped both, so a link package's scroll table
 * came back with every withheld range looking like ordinary content. That is
 * the failure mode duplication produces, found the usual way, by needing a
 * third copy and looking at the other two first.
 *
 * A *Scroll* is deliberately not here. Publication's scroll table means
 * something narrower than a store's registry does -- it requires a publisher
 * key and has no use for a default MIME type, where a registry holds unnamed
 * torrent-file scrolls and needs one -- so those two encodings differ on
 * purpose, and only the part that is genuinely one object is shared.
 */
#ifndef XUDU_SCROLL_CODEC_HPP
#define XUDU_SCROLL_CODEC_HPP

#include <optional>

#include "bencode.hpp"
#include "scroll.hpp"

namespace xanadu {

/// A withheld or transcopyright-locked range, with its descriptor when it has
/// one. Omits what is at its default, so an ordinary hole stays three keys.
[[nodiscard]] bencode::Value encodeHole(const PublishedHoleRecord &hole);
[[nodiscard]] std::optional<PublishedHoleRecord>
decodeHole(const bencode::Value &value);

/// One stretch of a scroll and the torrent carrying it, including whether it
/// is withheld and what the record of that says.
[[nodiscard]] bencode::Value encodeSegment(const ScrollSegment &segment);
[[nodiscard]] std::optional<ScrollSegment>
decodeSegment(const bencode::Value &value);

} // namespace xanadu

#endif // XUDU_SCROLL_CODEC_HPP
