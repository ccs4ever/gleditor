/**
 * @file scroll.hpp
 * @brief What an address points into, separately from who is carrying it.
 *
 * Xanadu asks for content to have "canonical network addresses" -- names that
 * identify content itself, everywhere, permanently. An offset into one
 * machine's spool is not one: it means nothing elsewhere, and nothing here
 * either once the machine is gone.
 *
 * A torrent's info hash is a name of the kind asked for, and the previous
 * design used one directly: a span said which torrent, which file, and how far
 * into it. That works for content that is finished, and it is wrong for
 * content that is still being written, for a reason that only shows up later.
 *
 * An info hash fixes the file list, the piece length and every piece hash, so
 * appending produces a *different* torrent. A growing body of content is
 * therefore carried by a succession of torrents, each sealed when it stopped
 * growing. If a span names the torrent, then sealing changes what an address
 * means -- and an address that changes is the rot the whole exercise exists to
 * avoid. Worse, it is silent: the old reference still resolves, to content
 * that is no longer the same passage.
 *
 * So a span names a *scroll* and an offset within it. A scroll is one
 * publisher's append-only sequence, which only ever grows, so an offset into
 * it is fixed the moment the bytes are written and no later event can disturb
 * it. Which torrent carries any given stretch is a separate, replaceable fact,
 * recorded here as a list of segments.
 *
 * Two consequences worth naming, because they are the point:
 *
 *   - Sealing a segment republishes an index and touches no reference.
 *   - A quotation that spans a seal is one span over contiguous offsets, so
 *     nothing above has to know a seal happened. The alternative -- teaching
 *     the extent merge that one segment ends where the next begins -- treats
 *     the symptom, and leaves every other piece of address arithmetic to be
 *     taught the same lesson separately.
 */
#ifndef XUDU_SCROLL_H
#define XUDU_SCROLL_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "identity/identity_layout.hpp"
#include "mutable_link.hpp"
#include "spool.hpp"
#include "torrent.hpp"

namespace xudu {

/**
 * @enum HoleReason
 * @brief Semantic reason why a contiguous scroll span cannot be resolved in the
 * clear.
 */
enum class HoleReason : std::uint8_t {
  Withheld = 0x00, ///< Author redacted/withheld (editorial/embargo)
  Revoked  = 0x01, ///< Cryptographic revocation / tombstoned
  Takedown = 0x02, ///< Legal takedown / dispute
  TranscopyrightLock =
      0x03,       ///< Ted Nelson Transcopyright: pay tokens to unlock
  Unsealed = 0x04 ///< Local span not yet sealed into BitTorrent
};

[[nodiscard]] inline std::string_view
holeReasonName(const HoleReason reason) noexcept {
  switch (reason) {
  case HoleReason::Withheld:
    return "withheld";
  case HoleReason::Revoked:
    return "revoked";
  case HoleReason::Takedown:
    return "takedown";
  case HoleReason::TranscopyrightLock:
    return "transcopyright";
  case HoleReason::Unsealed:
    return "unsealed";
  }
  return "unknown";
}

[[nodiscard]] inline HoleReason
holeReasonFromName(const std::string_view name) noexcept {
  if (name == "transcopyright") return HoleReason::TranscopyrightLock;
  if (name == "revoked") return HoleReason::Revoked;
  if (name == "takedown") return HoleReason::Takedown;
  if (name == "unsealed") return HoleReason::Unsealed;
  return HoleReason::Withheld;
}

/// RGBA the renderer paints a hole in, one per reason.
///
/// Beside HoleReason rather than in the session, because which reason a hole
/// has is a fact about the document, and the mapping needs testing without a
/// graphics device -- xudu_test links the engine and no renderer, on purpose.
constexpr std::uint32_t kWithheldColour = 0x111827FFU; ///< Obsidian: embargoed
constexpr std::uint32_t kRevokedColour  = 0x7F1D1DFFU; ///< Deep red: withdrawn
constexpr std::uint32_t kTakedownColour = 0x334155FFU; ///< Slate: compelled
constexpr std::uint32_t kUnsealedColour = 0x6B7280AAU; ///< Grey: not yet sealed
constexpr std::uint32_t kTranscopyrightColour = 0xF59E0BCCU; ///< Amber: on sale

/**
 * @brief The colour a hole is drawn in, by why it is a hole.
 *
 * All of these used to paint as kWithheldColour, so a reader could not tell
 * an author's embargo from a court order, or from a span that simply has not
 * been sealed yet -- even though the reason was carried the whole way through
 * in PublishedHoleRecord and then dropped at the last step.
 */
[[nodiscard]] inline std::uint32_t
colourForHole(const HoleReason reason) noexcept {
  switch (reason) {
  case HoleReason::Revoked:
    return kRevokedColour;
  case HoleReason::Takedown:
    return kTakedownColour;
  case HoleReason::Unsealed:
    return kUnsealedColour;
  case HoleReason::TranscopyrightLock:
    // Reached when a locked span resolved as merely withheld, which means its
    // descriptor never arrived. Painted as on sale anyway: the reader can buy
    // it, and saying otherwise sends them away from something they could have
    // read.
    return kTranscopyrightColour;
  case HoleReason::Withheld:
    break;
  }
  return kWithheldColour;
}

/**
 * @enum SegmentKind
 * @brief Classification of carrier data for a scroll segment.
 */
enum class SegmentKind : std::uint8_t {
  Plain    = 0x00, ///< Cleartext verified torrent payload
  Withheld = 0x01  ///< Withheld or encrypted under Transcopyright CEK
};

/**
 * @struct TranscopyrightDescriptor
 * @brief Cryptographic and economic metadata required to invoice and unlock a
 * paywalled span.
 */
struct TranscopyrightDescriptor {
  /// Micropayment pricing in atomic units (nano-xu: 1 XU = 10^9 nano-xu)
  std::uint64_t priceAtomicUnits{0};

  /// Fee calculation model: true = flat rate for the span; false = per-byte
  bool flatFee{true};

  /// Author settlement destination (40-char OpenPGP v4 fingerprint)
  identity::Fingerprint authorWallet{};
  /// 32-byte Ed25519 PubKey
  identity::PubKey32 authorPubKey{};

  /// 32-byte Blake3/SHA-256 identifier of the Content Encryption Key (CEK)
  std::array<std::uint8_t, 32> keyId{};

  /// 24-byte nonce for XChaCha20-Poly1305 payload decryption
  std::array<std::uint8_t, 24> nonce{};

  /// Currency denomination symbol: "XU", "SAT", "USDN"
  std::string currencySymbol{"XU"};

  /// Human-readable terms of use / licensing terms
  std::string licenseMemo{"Nelson-Transcopyright-v1"};

  [[nodiscard]] std::uint64_t
  computeCost(const std::uint64_t byteCount) const noexcept {
    return flatFee ? priceAtomicUnits : (priceAtomicUnits * byteCount);
  }

  bool operator==(const TranscopyrightDescriptor &) const = default;
};

/**
 * @struct PublishedHoleRecord
 * @brief Authoritative signed record of a withheld/locked span within a
 * published manifest.
 */
struct PublishedHoleRecord {
  std::uint64_t at{};
  std::uint64_t length{};
  HoleReason reason{HoleReason::Withheld};
  std::optional<TranscopyrightDescriptor> transcopyright{};

  /// Cryptographic commitment (Blake3 / SHA-256 Merkle root) over the withheld
  /// cleartext
  std::array<std::uint8_t, 32> contentCommitment{};

  [[nodiscard]] std::uint64_t end() const noexcept { return at + length; }
  [[nodiscard]] bool covers(const std::uint64_t offset) const noexcept {
    return offset >= at && offset < end();
  }

  bool operator==(const PublishedHoleRecord &) const = default;
};

/**
 * @brief One stretch of a scroll, and the torrent currently carrying it.
 *
 * The mapping from scroll coordinates to a torrent. Everything in here except
 * @p at and @p length may change when content is re-sealed into a new torrent;
 * nothing above this file may depend on any of it.
 */
struct ScrollSegment {
  /// The first scroll offset this segment carries, and how many bytes.
  std::uint64_t at{};
  std::uint64_t length{};

  /// The torrent those bytes are in, and where they begin in its concatenated
  /// stream. The stream rather than the file, because piece hashes are over
  /// the stream and verification is the reason any of this is here.
  InfoHash torrent;
  std::uint64_t streamOffset{};

  /// Which file, and its path. Display and diagnostics; not the identity.
  std::uint32_t fileIndex{};
  std::string path;

  SegmentKind kind{SegmentKind::Plain};
  std::optional<PublishedHoleRecord> holeRecord{};

  [[nodiscard]] std::uint64_t end() const noexcept { return at + length; }
  [[nodiscard]] bool covers(const std::uint64_t offset) const noexcept {
    return offset >= at && offset < end();
  }
  [[nodiscard]] bool isWithheld() const noexcept {
    return kind != SegmentKind::Plain;
  }
  [[nodiscard]] bool isLocked() const noexcept {
    return kind == SegmentKind::Withheld && holeRecord.has_value() &&
           holeRecord->reason == HoleReason::TranscopyrightLock;
  }

  bool operator==(const ScrollSegment &) const = default;
};

/**
 * @brief An append-only sequence of bytes, addressed in its own coordinates.
 *
 * Named by a publisher's BEP 46 key when it has one, which is what lets the
 * carrier change underneath it: the key survives re-sealing and an info hash
 * cannot. A scroll with no key is content that exists only as a fixed torrent
 * file, and it is identified by that file -- honest, because without a
 * publisher there is nothing to bind two packagings of the same bytes
 * together, and they are not the same scroll.
 */
struct Scroll {
  /// The publisher's name. Zero when this scroll has none.
  PublicKey publisher;
  /// The salt distinguishing this scroll from others under the same key.
  std::string salt;
  /// Where the bytes are, ordered by @p at and not overlapping. May have gaps:
  /// a stretch nobody has sealed yet is simply not here, and reads as missing
  /// rather than as something else.
  std::vector<ScrollSegment> segments;

  /// One past the last offset any segment carries.
  [[nodiscard]] std::uint64_t length() const {
    return segments.empty() ? 0 : segments.back().end();
  }

  [[nodiscard]] bool isNamed() const { return !publisher.isZero(); }

  /// The segment carrying @p offset, or nullptr when nothing does.
  [[nodiscard]] const ScrollSegment *segmentAt(std::uint64_t offset) const;

  /// Record a segment, keeping the list ordered. A segment covering offsets
  /// an existing one already covers replaces it, which is how a re-seal is
  /// applied: the address stays, the carrier moves.
  void addSegment(ScrollSegment segment);

  /**
   * @brief Whether two scrolls are the same scroll.
   *
   * By name when there is one -- so a scroll re-sealed into an entirely
   * different set of torrents is still recognisably itself, which is the
   * property the rest of this depends on. By the first segment's file
   * otherwise.
   */
  [[nodiscard]] bool sameContentAs(const Scroll &other) const;

  /// How this is written down, for a person.
  [[nodiscard]] std::string describe() const;

  /// A scroll that is one file of one torrent, which is what content that was
  /// never published as a growing thing amounts to.
  [[nodiscard]] static Scroll ofTorrentFile(const InfoHash &torrent,
                                            std::uint32_t fileIndex,
                                            std::string path,
                                            std::uint64_t fileOffset,
                                            std::uint64_t fileLength);
};

} // namespace xudu

#endif // XUDU_SCROLL_H
