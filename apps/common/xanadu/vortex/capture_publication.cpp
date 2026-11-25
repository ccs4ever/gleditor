#include "common/xanadu/vortex/capture_publication.hpp"

#include <cstdint>
#include <exception>
#include <utility>

#include <gleditor/logging.hpp>

#include "common/xanadu/zigzag/dimension_registry.hpp"

namespace xanadu::vortex {

namespace {

constexpr const char *kLog = "vortex.io.capture";

// stableDetail values for PersistenceRefusal, so a log line names the step.
constexpr std::uint32_t kDetailDimension = 10;
constexpr std::uint32_t kDetailMint      = 11;
constexpr std::uint32_t kDetailSave      = 12;

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

/// The receipts, in rank order, as (cell, id) pairs.
auto receiptRank(const zigzag::Manifold &manifold, const Dims &dims)
    -> std::vector<std::pair<zigzag::CellRef, CaptureId>> {
  std::vector<std::pair<zigzag::CellRef, CaptureId>> out;
  auto cell =
      manifold.linked(dims.capture, dims.capture, zigzag::DimVector::POS);
  while (cell) {
    if (const auto id = manifold.asInt64(*cell)) {
      out.emplace_back(*cell, static_cast<CaptureId>(*id));
    }
    cell = manifold.linked(*cell, dims.capture, zigzag::DimVector::POS);
  }
  return out;
}

auto readPublication(const zigzag::Manifold &manifold, const Dims &dims,
                     const zigzag::CellRef receipt) -> PublishedCapture {
  PublishedCapture out{.receipt = receipt, .pieces = {}};
  auto piece = manifold.linked(receipt, dims.range, zigzag::DimVector::POS);
  while (piece) {
    PublishedPiece read;
    if (const auto content = manifold.contentOf(*piece); !content.empty()) {
      read.span = content.front();
    }
    std::array<std::int64_t, kProvenanceFields> fields{};
    auto field =
        manifold.linked(*piece, dims.provenance, zigzag::DimVector::POS);
    for (std::size_t i = 0; i < kProvenanceFields && field; ++i) {
      fields[i] = manifold.asInt64(*field).value_or(0);
      field = manifold.linked(*field, dims.provenance, zigzag::DimVector::POS);
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

auto CapturePublisher::published(const CaptureId captureId) const
    -> std::optional<PublishedCapture> {
  auto manifold = store_.rebuildManifold(store_.latest());
  manifold.setStore(&store_);
  const auto dims = lookupDims(manifold, store_);
  if (!dims) return std::nullopt;
  for (const auto &[cell, id] : receiptRank(manifold, *dims)) {
    if (id == captureId) return readPublication(manifold, *dims, cell);
  }
  return std::nullopt;
}

auto CapturePublisher::publishedIds() const -> std::unordered_set<CaptureId> {
  auto manifold = store_.rebuildManifold(store_.latest());
  manifold.setStore(&store_);
  std::unordered_set<CaptureId> ids;
  if (const auto dims = lookupDims(manifold, store_)) {
    for (const auto &entry : receiptRank(manifold, *dims)) {
      ids.insert(entry.second);
    }
  }
  return ids;
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

    // Idempotence is keyed by CaptureId: a sealed receipt means the whole
    // publication is already in the Store.
    zigzag::CellRef tail = dims.capture;
    for (const auto &[cell, id] : receiptRank(manifold, dims)) {
      if (id == publication.captureId) {
        GLEDITOR_LOG_DEBUG(kLog, "capture {} already published", id);
        return readPublication(manifold, dims, cell);
      }
      tail = cell;
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
    // The seal: the receipt joins the rank last.
    head = store_.setLink(head, tail, dims.capture, zigzag::DimVector::POS,
                          receipt);
    GLEDITOR_LOG_DEBUG(kLog, "capture {} published: {} pieces",
                       publication.captureId, publication.pieces.size());
    return published(publication.captureId).value();
  } catch (const std::exception &error) {
    GLEDITOR_LOG_WARN(kLog, "capture {} publication refused: {}",
                      publication.captureId, error.what());
    return std::unexpected(
        IoErrorInfo{IoErrorCode::PersistenceRefusal, kDetailMint});
  }
}

auto CapturePublisher::save() const -> std::expected<void, IoErrorInfo> {
  try {
    store_.save(directory_.string());
    return {};
  } catch (const std::exception &error) {
    GLEDITOR_LOG_WARN(kLog, "store save refused: {}", error.what());
    return std::unexpected(
        IoErrorInfo{IoErrorCode::PersistenceRefusal, kDetailSave});
  }
}

} // namespace xanadu::vortex
