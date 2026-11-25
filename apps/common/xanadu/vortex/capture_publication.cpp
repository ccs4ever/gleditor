#include "common/xanadu/vortex/capture_publication.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <utility>

#include <gleditor/logging.hpp>

#include "common/xanadu/zigzag/dimension_registry.hpp"

namespace xanadu::vortex {

namespace {

constexpr const char *kLog = "vortex.io.capture";

// stableDetail values for PersistenceRefusal, so a log line names the step.
constexpr std::uint32_t kDetailDimension  = 10;
constexpr std::uint32_t kDetailMint       = 11;
constexpr std::uint32_t kDetailSave       = 12;
constexpr std::uint32_t kDetailUnreadable = 13;
constexpr std::uint32_t kDetailMismatch   = 14;

/// The five provenance scalars one piece carries, in rank order.
constexpr std::size_t kProvenanceFields = 5;

struct Dims {
  zigzag::DimRef capture;
  zigzag::DimRef range;
  zigzag::DimRef provenance;
};

auto lookupDims(const zigzag::Manifold &manifold, const xanadu::Store &store)
    -> std::optional<Dims> {
  const auto capture = manifold.dimensionNamed(kCaptureDimension, store);
  const auto range   = manifold.dimensionNamed(kRangeDimension, store);
  const auto prov    = manifold.dimensionNamed(kProvenanceDimension, store);
  if (!capture || !range || !prov) return std::nullopt;
  return Dims{*capture, *range, *prov};
}

struct Receipt {
  zigzag::CellRef cell;
  PublicationKey key;
};

auto unreadable() -> std::unexpected<IoErrorInfo> {
  return std::unexpected(
      IoErrorInfo{IoErrorCode::PersistenceRefusal, kDetailUnreadable});
}

/// The scalar a provenance chain's next cell holds, or a refusal: a short
/// chain or a cell that is not a scalar is a damaged store, never zero.
auto nextScalar(const zigzag::Manifold &manifold, const Dims &dims,
                const zigzag::CellRef from)
    -> std::expected<std::pair<zigzag::CellRef, std::int64_t>, IoErrorInfo> {
  const auto field =
      manifold.linked(from, dims.provenance, zigzag::DimVector::POS);
  if (!field) return unreadable();
  const auto value = manifold.asInt64(*field);
  if (!value) return unreadable();
  return std::pair{*field, *value};
}

/// The receipts, in rank order.
auto receiptRank(const zigzag::Manifold &manifold, const Dims &dims)
    -> std::expected<std::vector<Receipt>, IoErrorInfo> {
  std::vector<Receipt> out;
  auto cell =
      manifold.linked(dims.capture, dims.capture, zigzag::DimVector::POS);
  while (cell) {
    const auto id = manifold.asInt64(*cell);
    if (!id) return unreadable();
    const auto journal = nextScalar(manifold, dims, *cell);
    if (!journal) return std::unexpected(journal.error());
    out.push_back(
        Receipt{.cell = *cell,
                .key  = PublicationKey{
                     .journalId = static_cast<std::uint64_t>(journal->second),
                     .captureId = static_cast<CaptureId>(*id)}});
    cell = manifold.linked(*cell, dims.capture, zigzag::DimVector::POS);
  }
  return out;
}

auto readPublication(const zigzag::Manifold &manifold, const Dims &dims,
                     const zigzag::CellRef receipt)
    -> std::expected<PublishedCapture, IoErrorInfo> {
  PublishedCapture out{.receipt = receipt, .pieces = {}};
  auto piece = manifold.linked(receipt, dims.range, zigzag::DimVector::POS);
  while (piece) {
    PublishedPiece read;
    const auto content = manifold.contentOf(*piece);
    if (content.empty()) return unreadable();
    read.span = content.front();
    std::array<std::int64_t, kProvenanceFields> fields{};
    auto at = *piece;
    for (auto &field : fields) {
      const auto next = nextScalar(manifold, dims, at);
      if (!next) return std::unexpected(next.error());
      at    = next->first;
      field = next->second;
    }
    if (fields[0] < 0 ||
        fields[0] > static_cast<std::int64_t>(PayloadSourceKind::Proxy)) {
      return unreadable();
    }
    read.origin =
        PayloadRef{.kind       = static_cast<PayloadSourceKind>(fields[0]),
                   .source     = static_cast<std::uint64_t>(fields[1]),
                   .generation = static_cast<std::uint64_t>(fields[2]),
                   .occurrence = static_cast<std::uint64_t>(fields[3])};
    read.srcOffset = static_cast<std::uint64_t>(fields[4]);
    out.pieces.push_back(read);
    piece = manifold.linked(*piece, dims.range, zigzag::DimVector::POS);
  }
  return out;
}

} // namespace

CapturePublisher::CapturePublisher(xanadu::Store &store,
                                   std::filesystem::path directory)
    : store_(store), directory_(std::move(directory)) {}

auto CapturePublisher::published(const PublicationKey &key) const
    -> std::expected<std::optional<PublishedCapture>, IoErrorInfo> {
  auto manifold = store_.rebuildManifold(store_.latest());
  manifold.setStore(&store_);
  const auto dims = lookupDims(manifold, store_);
  if (!dims) return std::optional<PublishedCapture>{};
  const auto rank = receiptRank(manifold, *dims);
  if (!rank) return std::unexpected(rank.error());
  for (const auto &receipt : *rank) {
    if (receipt.key != key) continue;
    auto read = readPublication(manifold, *dims, receipt.cell);
    if (!read) return std::unexpected(read.error());
    return std::optional{std::move(*read)};
  }
  return std::optional<PublishedCapture>{};
}

auto CapturePublisher::publishedKeys() const
    -> std::expected<std::unordered_set<PublicationKey, PublicationKeyHash>,
                     IoErrorInfo> {
  auto manifold = store_.rebuildManifold(store_.latest());
  manifold.setStore(&store_);
  std::unordered_set<PublicationKey, PublicationKeyHash> keys;
  if (const auto dims = lookupDims(manifold, store_)) {
    const auto rank = receiptRank(manifold, *dims);
    if (!rank) return std::unexpected(rank.error());
    for (const auto &receipt : *rank) keys.insert(receipt.key);
  }
  return keys;
}

auto CapturePublisher::publish(const CapturePublication &publication)
    -> std::expected<PublishedCapture, IoErrorInfo> {
  try {
    auto head     = store_.latest();
    auto manifold = store_.rebuildManifold(head);
    manifold.setStore(&store_);
    auto &registry = zigzag::DimensionRegistry::instance();
    const auto minted =
        [&](const std::string_view name) -> std::optional<zigzag::DimRef> {
      const auto dim = registry.getOrCreate(store_, head, manifold, name);
      if (!dim) return std::nullopt;
      return *dim;
    };
    const auto captureDim = minted(kCaptureDimension);
    const auto rangeDim   = minted(kRangeDimension);
    const auto provDim    = minted(kProvenanceDimension);
    if (!captureDim || !rangeDim || !provDim) {
      return std::unexpected(
          IoErrorInfo{IoErrorCode::PersistenceRefusal, kDetailDimension});
    }
    const Dims dims{*captureDim, *rangeDim, *provDim};

    // Idempotence is keyed by (journalId, CaptureId): a sealed receipt
    // means the whole publication is already in the Store. A CaptureId alone
    // is journal-local, so another journal's receipt never stands in.
    const auto key  = PublicationKey{.journalId = publication.journalId,
                                     .captureId = publication.captureId};
    const auto rank = receiptRank(manifold, dims);
    if (!rank) return std::unexpected(rank.error());
    zigzag::CellRef tail = dims.capture;
    for (const auto &receipt : *rank) {
      if (receipt.key == key) {
        GLEDITOR_LOG_DEBUG(kLog, "capture {} already published", key.captureId);
        auto read = readPublication(manifold, dims, receipt.cell);
        if (!read) return read;
        // The same key naming different content is not "already done": it is
        // two captures sharing an identity, and answering with the first
        // would drop the second's Structure silently.
        if (!std::ranges::equal(read->pieces, publication.pieces)) {
          GLEDITOR_LOG_ERROR(kLog,
                             "capture {} already published with other pieces",
                             key.captureId);
          return std::unexpected(
              IoErrorInfo{IoErrorCode::PersistenceRefusal, kDetailMismatch});
        }
        return read;
      }
      tail = receipt.cell;
    }

    const auto cellOf = [&](const xanadu::MicroversionId &version) {
      return store_.cellRefOf(version).value();
    };
    // Pieces, with their provenance chains, before the receipt: a failure
    // part-way leaves unlinked cells, never a reachable partial receipt.
    std::vector<zigzag::CellRef> pieceCells;
    pieceCells.reserve(publication.pieces.size());
    for (const auto &piece : publication.pieces) {
      head                 = store_.makeCell(head, piece.span);
      const auto pieceCell = cellOf(head);
      pieceCells.push_back(pieceCell);
      const std::array<std::int64_t, kProvenanceFields> fields{
          static_cast<std::int64_t>(piece.origin.kind),
          static_cast<std::int64_t>(piece.origin.source),
          static_cast<std::int64_t>(piece.origin.generation),
          static_cast<std::int64_t>(piece.origin.occurrence),
          static_cast<std::int64_t>(piece.srcOffset)};
      zigzag::CellRef previous = pieceCell;
      for (const auto field : fields) {
        head                 = store_.makeScalarCell(head, field);
        const auto fieldCell = cellOf(head);
        head                 = store_.setLink(head, previous, dims.provenance,
                                              zigzag::DimVector::POS, fieldCell);
        previous             = fieldCell;
      }
    }
    for (std::size_t i = 1; i < pieceCells.size(); ++i) {
      head = store_.setLink(head, pieceCells[i - 1], dims.range,
                            zigzag::DimVector::POS, pieceCells[i]);
    }
    head = store_.makeScalarCell(
        head, static_cast<std::int64_t>(publication.captureId));
    const auto receipt = cellOf(head);
    if (!pieceCells.empty()) {
      head = store_.setLink(head, receipt, dims.range, zigzag::DimVector::POS,
                            pieceCells.front());
    }
    head = store_.makeScalarCell(
        head, static_cast<std::int64_t>(publication.journalId));
    head = store_.setLink(head, receipt, dims.provenance,
                          zigzag::DimVector::POS, cellOf(head));
    // The seal: the receipt joins the rank last.
    head = store_.setLink(head, tail, dims.capture, zigzag::DimVector::POS,
                          receipt);
    GLEDITOR_LOG_DEBUG(kLog, "capture {} published: {} pieces",
                       publication.captureId, publication.pieces.size());
    PublishedCapture out{.receipt = receipt, .pieces = {}};
    out.pieces.assign(publication.pieces.begin(), publication.pieces.end());
    return out;
  } catch (const std::exception &error) {
    GLEDITOR_LOG_WARN(kLog, "capture {} publication refused: {}",
                      publication.captureId, error.what());
    return std::unexpected(
        IoErrorInfo{IoErrorCode::PersistenceRefusal, kDetailMint});
  }
}

auto CapturePublisher::snapshot()
    -> std::expected<xanadu::StoreSnapshot, IoErrorInfo> {
  try {
    return store_.saveSnapshot(directory_.string());
  } catch (const std::exception &error) {
    GLEDITOR_LOG_WARN(kLog, "store snapshot refused: {}", error.what());
    return std::unexpected(
        IoErrorInfo{IoErrorCode::PersistenceRefusal, kDetailSave});
  }
}

auto CapturePublisher::write(const xanadu::StoreSnapshot &snapshot) const
    -> std::expected<void, IoErrorInfo> {
  try {
    const bool written =
        writer_ ? writer_(snapshot) : xanadu::SnapshotWriter::write(snapshot);
    if (!written) {
      GLEDITOR_LOG_WARN(kLog, "store snapshot did not extend the directory");
      return std::unexpected(
          IoErrorInfo{IoErrorCode::PersistenceRefusal, kDetailSave});
    }
    return {};
  } catch (const std::exception &error) {
    GLEDITOR_LOG_WARN(kLog, "store save refused: {}", error.what());
    return std::unexpected(
        IoErrorInfo{IoErrorCode::PersistenceRefusal, kDetailSave});
  }
}

auto CapturePublisher::save() -> std::expected<void, IoErrorInfo> {
  // A refused extension resets what the Store believes the directory holds,
  // so the second snapshot is the whole Store: the one retry that can help.
  std::expected<void, IoErrorInfo> last;
  for (int attempt = 0; attempt < 2; ++attempt) {
    const auto taken = snapshot();
    if (!taken) return std::unexpected(taken.error());
    last = write(*taken);
    if (last) return last;
  }
  return last;
}

auto CapturePublisher::setSnapshotWriter(
    std::function<bool(const xanadu::StoreSnapshot &)> writer)
    -> CapturePublisher * {
  writer_ = std::move(writer);
  return this;
}

} // namespace xanadu::vortex
