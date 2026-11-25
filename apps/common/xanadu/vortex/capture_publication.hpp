#ifndef XANADU_VORTEX_CAPTURE_PUBLICATION_HPP
#define XANADU_VORTEX_CAPTURE_PUBLICATION_HPP

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "common/xanadu/spool.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/capture_journal.hpp"
#include "common/xanadu/vortex/io_error.hpp"
#include "common/xanadu/vortex/payload_resolver.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

// design/vortex-io-implementation-plan.md §8.2: the Structure half of a
// capture. A committed capture's translated spans become ordinary Structure
// operations in a Store, through the existing API only (makeDimension,
// makeCell, makeScalarCell, setLink) -- no new OpKind, verb or format.
//
// Shape, per capture (every cell is minted by an operation):
//   d.io.capture   one rank threading the receipts in publication order,
//                  headed by the dimension's own cell. A receipt joins it
//                  LAST: being on the rank is the seal, so a receipt that
//                  is reachable is a publication that is complete.
//   receipt        a scalar cell whose canonical int64 bits are the
//                  CaptureId, with the journal's identity as a scalar cell on
//                  its d.io.provenance rank. A CaptureId is unique only within
//                  one capture.journal while a Store may be shared, so
//                  idempotence is keyed by the pair (journalId, CaptureId),
//                  never by a content hash.
//   d.io.range     receipt -> one cell per appended piece, in append order;
//                  each cell's content IS the permanent permascroll span.
//   d.io.provenance  piece -> source kind, source, generation, occurrence
//                  and source offset as scalar cells: where the bytes came
//                  from, as structure rather than text to parse. Local
//                  capture invents no remote signature.
//
// A capture whose selection was wholly covered by earlier translations
// appended nothing and publishes a receipt with an empty d.io.range rank;
// the spans it reuses were published by the capture that appended them.

namespace xanadu::vortex {

/// System dimension names a publication mints through the standard dimension
/// registry, once per store.
inline constexpr std::string_view kCaptureDimension    = "d.io.capture";
inline constexpr std::string_view kRangeDimension      = "d.io.range";
inline constexpr std::string_view kProvenanceDimension = "d.io.provenance";

/// One appended piece: the logical source interval and the permanent span
/// its bytes now occupy.
struct PublishedPiece {
  PayloadRef origin;
  std::uint64_t srcOffset{};
  xanadu::PrimediaSpan span;
  friend bool operator==(const PublishedPiece &,
                         const PublishedPiece &) = default;
};

/// What names one publication: the journal that issued the id, and the id.
struct PublicationKey {
  std::uint64_t journalId{};
  CaptureId captureId{};
  friend bool operator==(const PublicationKey &,
                         const PublicationKey &) = default;
};
struct PublicationKeyHash {
  [[nodiscard]] auto operator()(const PublicationKey &key) const noexcept
      -> std::size_t {
    return std::hash<std::uint64_t>{}(key.journalId * 0x9e3779b97f4a7c15ULL ^
                                      key.captureId);
  }
};

/// What publish() is asked to make durable-by-reference.
struct CapturePublication {
  std::uint64_t journalId{};
  CaptureId captureId{};
  std::span<const PublishedPiece> pieces;
};

/// What a Store holds for one published capture.
struct PublishedCapture {
  zigzag::CellRef receipt;
  std::vector<PublishedPiece> pieces;
};

/// The Store a capture coordinator publishes into, plus the directory it is
/// saved to. publish() mutates the Store in memory; save() is the durability
/// point the coordinator orders against its journal. Operations land on
/// store.latest(), so the Store must be single-writer for the duration (a
/// dedicated capture store, or the author's tip). Owner thread only.
class CapturePublisher {
public:
  CapturePublisher(xanadu::Store &store, std::filesystem::path directory);

  /// Publish @p publication unless the Store already holds a sealed receipt
  /// for its CaptureId, in which case that publication is returned and
  /// nothing is minted. Never double-publishes.
  [[nodiscard]] auto publish(const CapturePublication &publication)
      -> std::expected<PublishedCapture, IoErrorInfo>;
  /// Write the Store to its directory (Store::save, which also flushes the
  /// permascroll the captured spans live in).
  [[nodiscard]] auto save() const -> std::expected<void, IoErrorInfo>;

  /// The sealed publication for @p key, when the Store holds one. A receipt
  /// the reader cannot parse (short provenance chain, a non-scalar, an
  /// unknown source kind) is a PersistenceRefusal, never a default.
  [[nodiscard]] auto published(const PublicationKey &key) const
      -> std::expected<std::optional<PublishedCapture>, IoErrorInfo>;
  /// The sealed publication for @p captureId under any journal (the first
  /// on the rank): for a Store a single journal writes, and for diagnostics.
  [[nodiscard]] auto published(CaptureId captureId) const
      -> std::optional<PublishedCapture>;
  /// Every key with a sealed receipt: one walk of the receipt rank.
  [[nodiscard]] auto publishedKeys() const
      -> std::expected<std::unordered_set<PublicationKey, PublicationKeyHash>,
                       IoErrorInfo>;
  /// Every CaptureId with a sealed receipt, across journals.
  [[nodiscard]] auto publishedIds() const -> std::unordered_set<CaptureId>;

  [[nodiscard]] auto store() noexcept -> xanadu::Store & { return store_; }
  [[nodiscard]] auto directory() const noexcept
      -> const std::filesystem::path & {
    return directory_;
  }

private:
  xanadu::Store &store_;
  std::filesystem::path directory_;
};

} // namespace xanadu::vortex

#endif
