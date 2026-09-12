/**
 * @file zz_xudu_projector.cpp
 * @brief Implementation of bidirectional projection and rasterization between
 *        Xudu (Xanadocs/Xanalinks) and Zigzag (Multidimensional cell space).
 */
#include "common/xanadu/zigzag/zz_xudu_projector.hpp"

#include <algorithm>
#include <ctime>
#include <format>
#include <map>
#include <set>
#include <sstream>
#include <unordered_set>
#include <variant>

#include "common/xanadu/format.hpp"
#include "common/xanadu/zigzag/zzcore.hpp"

namespace zigzag {

namespace {

bool spansOverlap(const xanadu::PrimediaSpan &a,
                  const xanadu::PrimediaSpan &b) {
  if (a.scroll != b.scroll || a.empty() || b.empty()) {
    return false;
  }
  return !a.intersect(b).empty();
}

/// The ranks a cell's non-content attributes live on now that CellSlot has no
/// field for them. R13's answer for metadata: cells on a dimension.
constexpr std::string_view roleDimension  = "d.role";
constexpr std::string_view mimeDimension  = "d.mime";
constexpr std::string_view mediaDimension = "d.media";

} // namespace

ZzStructureDocument projectXuduToZigzag(const std::vector<XuduDocInput> &docs,
                                        const std::vector<xanadu::Link> &links,
                                        const XuduProjectorOptions &opts) {
  ZzStructureDocument result;
  result.meta.name = "Xudu Xanadoc Space";
  result.focus     = 1;

  result.view.x_dimension = opts.doc_dimension;
  result.view.y_dimension = opts.transclusion_dimension;
  result.view.z_dimension = opts.link_dimension;

  result.dimension_meta[opts.doc_dimension] = DimensionMeta{
      .label       = "Reading Flow",
      .description = "Document reading sequence",
      .color       = RgbColor{0.3F, 0.7F, 0.9F},
      .spacing     = 2.2F,
  };
  result.dimension_meta[opts.transclusion_dimension] = DimensionMeta{
      .label       = "Transclusion",
      .description = "Shared primedia scroll spans",
      .color       = RgbColor{0.95F, 0.5F, 0.2F},
      .spacing     = 2.5F,
  };
  result.dimension_meta[opts.link_dimension] = DimensionMeta{
      .label       = "Xanalinks",
      .description = "Curated commentary and link relations",
      .color       = RgbColor{0.8F, 0.3F, 0.85F},
      .spacing     = 2.5F,
  };
  result.dimension_meta[opts.version_dimension] = DimensionMeta{
      .label       = "Microversions",
      .description = "Version history lineage",
      .color       = RgbColor{0.3F, 0.85F, 0.4F},
      .spacing     = 2.0F,
  };
  result.dimension_meta[opts.clone_dimension] = DimensionMeta{
      .label       = "Clone Family",
      .description = "Identical unchanged primedia spans across hypertime",
      .color       = RgbColor{0.0F, 0.74F, 0.83F},
      .spacing     = 2.0F,
  };

  CellID nextCellId = 1;
  struct CellMapping {
    CellID id{0};
    std::size_t docIndex{0};
    xanadu::PrimediaSpan span;
  };
  std::vector<CellMapping> cellMappings;
  std::vector<std::vector<CellID>> docCellChains(docs.size());

  struct SpanTracker {
    CellID masterCellId{0};
    CellID latestCellId{0};
  };
  struct SpanLess {
    bool operator()(const xanadu::PrimediaSpan &a,
                    const xanadu::PrimediaSpan &b) const noexcept {
      if (a.scroll != b.scroll) {
        return a.scroll < b.scroll;
      }
      if (a.start != b.start) {
        return a.start < b.start;
      }
      return a.length < b.length;
    }
  };
  std::map<xanadu::PrimediaSpan, SpanTracker, SpanLess> spanTrackers;

  for (std::size_t docIdx = 0; docIdx < docs.size(); ++docIdx) {
    const auto &doc = docs[docIdx];
    if (doc.text.empty()) {
      continue;
    }

    if (opts.split_by_paragraphs) {
      struct PieceRange {
        std::size_t byteStart{0};
        std::size_t byteEnd{0};
        xanadu::PrimediaSpan span;
      };
      std::vector<PieceRange> pieceRanges;
      pieceRanges.reserve(doc.spans.size());
      std::size_t curOffset = 0;
      for (const auto &s : doc.spans) {
        const auto len = static_cast<std::size_t>(s.length);
        pieceRanges.push_back(PieceRange{
            .byteStart = curOffset,
            .byteEnd   = curOffset + len,
            .span      = s,
        });
        curOffset += len;
      }

      // Split text into paragraphs
      std::size_t start   = 0;
      std::size_t paraIdx = 0;
      while (start < doc.text.size()) {
        std::size_t end = doc.text.find("\n\n", start);
        if (end == std::string::npos) {
          end = doc.text.size();
        }

        std::string paraText  = doc.text.substr(start, end - start);
        const auto firstNonWs = paraText.find_first_not_of(" \t\r\n");
        const auto lastNonWs  = paraText.find_last_not_of(" \t\r\n");
        if (firstNonWs != std::string::npos && lastNonWs != std::string::npos) {
          paraText = paraText.substr(firstNonWs, lastNonWs - firstNonWs + 1);
        }

        if (!paraText.empty()) {
          const std::size_t paraByteStart = start + firstNonWs;
          const std::size_t paraByteLen   = lastNonWs - firstNonWs + 1;

          xanadu::PrimediaSpan span;
          for (const auto &piece : pieceRanges) {
            if (paraByteStart >= piece.byteStart &&
                paraByteStart < piece.byteEnd) {
              const std::size_t inPieceOffset = paraByteStart - piece.byteStart;
              span.scroll                     = piece.span.scroll;
              span.start = piece.span.start + inPieceOffset;
              // Clamped to the piece the paragraph starts in, because a cell
              // holds one span. A paragraph straddling two pieces is addressed
              // by its first part rather than by all of it -- a truncation, not
              // a misdirection, and the honest limit of one span per cell.
              // Carrying all of it needs a cell that can hold several spans,
              // which CellSlot deliberately does not.
              const std::size_t availInPiece = piece.byteEnd - paraByteStart;
              span.length                    = static_cast<std::uint64_t>(
                  std::min(paraByteLen, availInPiece));
              break;
            }
          }
          // No fallback to doc.spans[paraIdx]. That paired the *nth paragraph*
          // with the *nth piece*, and a paragraph index and a piece index have
          // no relationship whatever: a piece is a run of one primedia address,
          // split and coalesced by editing, so one paragraph can span three
          // pieces and one piece can hold five paragraphs.
          //
          // The address is not inert either. It is what clone detection
          // compares, so two paragraphs handed the same borrowed span were
          // declared clones: the second lost its text and gained a d.clone link
          // to a paragraph it has nothing to do with. A wrong address is a
          // claim, and Version::occurrencesOf() would report the quotation it
          // asserts.
          //
          // So a paragraph the pieces do not cover gets no address and reads as
          // empty, which is the answer Resolver gives for content it cannot
          // verify: nothing, rather than something plausible.
          ++paraIdx;

          const bool isUnchangedClone =
              (span.length > 0 && spanTrackers.contains(span));
          const CellID id = nextCellId++;
          Cell cell;
          cell.id = id;

          if (isUnchangedClone) {
            // Clones do not store redundant text; content is derived from
            // master
            cell.data           = "";
            cell.role           = "xudu_clone";
            const CellID prevId = spanTrackers[span].latestCellId;
            result.cells[prevId].dimensions[opts.clone_dimension].pos = id;
            cell.dimensions[opts.clone_dimension].neg                 = prevId;
            spanTrackers[span].latestCellId                           = id;
          } else {
            cell.data = std::move(paraText);
            cell.role = "xudu_span";
            if (span.length > 0) {
              spanTrackers[span] =
                  SpanTracker{.masterCellId = id, .latestCellId = id};
            }
          }

          result.cells[id] = std::move(cell);
          docCellChains[docIdx].push_back(id);
          cellMappings.push_back(CellMapping{id, docIdx, span});
        }

        start = (end == doc.text.size()) ? end : end + 2;
      }
    } else {
      xanadu::PrimediaSpan span;
      if (!doc.spans.empty()) {
        span = doc.spans.front();
      }

      const bool isUnchangedClone =
          (span.length > 0 && spanTrackers.contains(span));
      const CellID id = nextCellId++;
      Cell cell;
      cell.id = id;

      if (isUnchangedClone) {
        cell.data           = "";
        cell.role           = "xudu_clone";
        const CellID prevId = spanTrackers[span].latestCellId;
        result.cells[prevId].dimensions[opts.clone_dimension].pos = id;
        cell.dimensions[opts.clone_dimension].neg                 = prevId;
        spanTrackers[span].latestCellId                           = id;
      } else {
        cell.data = doc.text;
        cell.role = "xudu_document";
        if (span.length > 0) {
          spanTrackers[span] =
              SpanTracker{.masterCellId = id, .latestCellId = id};
        }
      }

      result.cells[id] = std::move(cell);
      docCellChains[docIdx].push_back(id);
      cellMappings.push_back(CellMapping{id, docIdx, span});
    }
  }

  if (result.cells.empty()) {
    return result;
  }

  // --- 1. Link sequential reading order along opts.doc_dimension ---
  for (const auto &chain : docCellChains) {
    for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
      const CellID c1                                     = chain[i];
      const CellID c2                                     = chain[i + 1];
      result.cells[c1].dimensions[opts.doc_dimension].pos = c2;
      result.cells[c2].dimensions[opts.doc_dimension].neg = c1;
    }
  }

  // --- 2. Link transclusions along opts.transclusion_dimension ---
  for (std::size_t i = 0; i < cellMappings.size(); ++i) {
    for (std::size_t j = i + 1; j < cellMappings.size(); ++j) {
      const auto &m1 = cellMappings[i];
      const auto &m2 = cellMappings[j];
      if (m1.docIndex != m2.docIndex && spansOverlap(m1.span, m2.span)) {
        auto &c1 = result.cells[m1.id];
        auto &c2 = result.cells[m2.id];
        if (c1.dimensions[opts.transclusion_dimension].pos == 0 &&
            c2.dimensions[opts.transclusion_dimension].neg == 0) {
          c1.dimensions[opts.transclusion_dimension].pos = m2.id;
          c2.dimensions[opts.transclusion_dimension].neg = m1.id;
        }
      }
    }
  }

  // --- 3. Link sequential microversion steps along opts.version_dimension ---
  for (std::size_t docIdx = 0; docIdx + 1 < docCellChains.size(); ++docIdx) {
    const auto &c1Chain       = docCellChains[docIdx];
    const auto &c2Chain       = docCellChains[docIdx + 1];
    const std::size_t minSize = std::min(c1Chain.size(), c2Chain.size());
    for (std::size_t p = 0; p < minSize; ++p) {
      const CellID c1 = c1Chain[p];
      const CellID c2 = c2Chain[p];
      if (result.cells[c1].dimensions[opts.version_dimension].pos == 0 &&
          result.cells[c2].dimensions[opts.version_dimension].neg == 0) {
        result.cells[c1].dimensions[opts.version_dimension].pos = c2;
        result.cells[c2].dimensions[opts.version_dimension].neg = c1;
      }
    }
  }

  // --- 3. Link Xanalinks along opts.link_dimension ---
  for (const auto &link : links) {
    CellID leftId  = 0;
    CellID rightId = 0;

    for (const auto &mapping : cellMappings) {
      if (leftId == 0 && !link.left.empty() &&
          spansOverlap(mapping.span, link.left.front())) {
        leftId = mapping.id;
      }
      if (rightId == 0 && !link.right.empty() &&
          spansOverlap(mapping.span, link.right.front())) {
        rightId = mapping.id;
      }
    }

    if (leftId != 0 && rightId != 0 && leftId != rightId) {
      auto &cL = result.cells[leftId];
      auto &cR = result.cells[rightId];
      if (cL.dimensions[opts.link_dimension].pos == 0 &&
          cR.dimensions[opts.link_dimension].neg == 0) {
        cL.dimensions[opts.link_dimension].pos = rightId;
        cR.dimensions[opts.link_dimension].neg = leftId;
      }
    }
  }

  return result;
}

ZzStructureDocument
projectStoreToZigzag(const xanadu::Store &store,
                     const std::vector<xanadu::MicroversionId> &versions,
                     const XuduProjectorOptions &opts) {
  std::vector<XuduDocInput> docInputs;
  for (const auto &verId : versions) {
    const auto ver = store.rebuild(verId);
    std::string assembledText;
    std::vector<xanadu::PrimediaSpan> spans = ver.pieces();
    for (const auto &piece : spans) {
      if (piece.isLocal()) {
        assembledText += store.read(piece);
      } else {
        const auto res = store.resolve(piece);
        if (res.status == xanadu::ResolutionStatus::VerifiedBytes) {
          assembledText += res.text;
        } else if (res.status == xanadu::ResolutionStatus::WithheldRedacted) {
          assembledText += "[Redacted - Withheld]";
        } else if (res.status ==
                   xanadu::ResolutionStatus::TranscopyrightLocked) {
          if (res.lockInfo) {
            assembledText += "[🔒 " +
                             std::to_string(res.lockInfo->priceAtomicUnits) +
                             " " + res.lockInfo->currencySymbol + "]";
          } else {
            assembledText += "[🔒 Locked]";
          }
        }
      }
    }

    docInputs.push_back(XuduDocInput{
        .name    = verId.str(),
        .text    = std::move(assembledText),
        .version = verId,
        .spans   = std::move(spans),
    });
  }

  std::vector<xanadu::Link> allLinks;
  for (const auto &[linkId, link] : store.links()) {
    allLinks.push_back(link);
  }

  return projectXuduToZigzag(docInputs, allLinks, opts);
}

ZzRasterResult rasterizeZzStructure(const ZzStructureDocument &doc,
                                    const DimID &primaryDim,
                                    const DimID &secondaryDim,
                                    CellID startCell) {
  ZzRasterResult result;
  if (doc.cells.empty()) {
    return result;
  }

  if (startCell == 0 || !doc.cells.contains(startCell)) {
    startCell = doc.focus;
    if (startCell == 0 || !doc.cells.contains(startCell)) {
      startCell = doc.cells.begin()->first;
    }
  }

  // Find head of secondary dimension if present
  CellID rowHead = startCell;
  std::unordered_set<CellID> visitedRows;
  while (true) {
    const auto it = doc.cells.find(rowHead);
    if (it == doc.cells.end()) {
      break;
    }
    const auto lIt = it->second.dimensions.find(secondaryDim);
    if (lIt == it->second.dimensions.end() || lIt->second.neg == 0) {
      break;
    }
    if (visitedRows.contains(lIt->second.neg)) {
      break; // Cycle break
    }
    visitedRows.insert(rowHead);
    rowHead = lIt->second.neg;
  }

  visitedRows.clear();
  std::unordered_set<CellID> visitedCells;

  while (rowHead != 0 && !visitedRows.contains(rowHead)) {
    visitedRows.insert(rowHead);

    // Find head of primary dimension for this row
    CellID colHead = rowHead;
    std::unordered_set<CellID> visitedCols;
    while (true) {
      const auto it = doc.cells.find(colHead);
      if (it == doc.cells.end()) {
        break;
      }
      const auto lIt = it->second.dimensions.find(primaryDim);
      if (lIt == it->second.dimensions.end() || lIt->second.neg == 0) {
        break;
      }
      if (visitedCols.contains(lIt->second.neg)) {
        break;
      }
      visitedCols.insert(colHead);
      colHead = lIt->second.neg;
    }

    visitedCols.clear();
    CellID current  = colHead;
    bool firstInRow = true;

    while (current != 0 && !visitedCols.contains(current)) {
      visitedCols.insert(current);
      visitedCells.insert(current);

      const auto it = doc.cells.find(current);
      if (it == doc.cells.end()) {
        break;
      }

      if (!firstInRow) {
        result.text += " ";
      }
      firstInRow = false;

      result.text += zzcore::getEffectiveCellText(doc.cells, current);
      result.cell_sequence.push_back(current);

      const auto lIt = it->second.dimensions.find(primaryDim);
      current = (lIt != it->second.dimensions.end()) ? lIt->second.pos : 0;
    }

    result.text += "\n\n";
    result.line_breaks.push_back(result.text.size());

    const auto it = doc.cells.find(rowHead);
    if (it != doc.cells.end()) {
      const auto lIt = it->second.dimensions.find(secondaryDim);
      rowHead = (lIt != it->second.dimensions.end()) ? lIt->second.pos : 0;
    } else {
      break;
    }
  }

  // Remove trailing newlines
  while (!result.text.empty() &&
         (result.text.back() == '\n' || result.text.back() == ' ')) {
    result.text.pop_back();
  }

  return result;
}

ZzStructureDocument linkPackageToZzStructure(const xanadu::LinkPackage &pkg) {
  ZzStructureDocument doc;
  doc.meta.name = pkg.title.empty() ? "Imported Link Package" : pkg.title;
  doc.focus     = 1;

  std::map<xanadu::GlobalSpan, CellID> spanToCell;
  CellID nextCellId = 1;

  for (const auto &link : pkg.links) {
    if (link.type == xanadu::LinkType::Dimension) {
      for (const auto &span : link.left) {
        if (!spanToCell.contains(span)) {
          const CellID id  = nextCellId++;
          spanToCell[span] = id;
          Cell cell;
          cell.id       = id;
          cell.role     = "cell";
          cell.data     = std::format("Cell #{} [{}]", id, span.scroll);
          doc.cells[id] = std::move(cell);
        }
      }
      for (const auto &span : link.right) {
        if (!spanToCell.contains(span)) {
          const CellID id  = nextCellId++;
          spanToCell[span] = id;
          Cell cell;
          cell.id       = id;
          cell.role     = "cell";
          cell.data     = std::format("Cell #{} [{}]", id, span.scroll);
          doc.cells[id] = std::move(cell);
        }
      }

      std::string dimName = "d.1";
      if (link.owner.starts_with("dim:")) {
        dimName = link.owner.substr(4);
      }

      if (!link.left.empty() && !link.right.empty()) {
        const CellID c1                       = spanToCell[link.left.front()];
        const CellID c2                       = spanToCell[link.right.front()];
        doc.cells[c1].dimensions[dimName].pos = c2;
        doc.cells[c2].dimensions[dimName].neg = c1;
      }
    }
  }

  return doc;
}

// -- a slice is a store -------------------------------------------------------

SlicedStore sliceToStore(const ZzStructureDocument &doc, xanadu::Store &store,
                         const xanadu::MicroversionId &parent) {
  SlicedStore out;
  out.version = parent;

  if (zigzag::noCell == store.homeCell()) {
    out.version = store.sliceGenesis(out.version);
  }

  // Sorted, so that one document mints one operation sequence however the
  // hash tables happened to iterate. A fixture that cannot be regenerated
  // byte-for-byte is a fixture nobody can diff.
  std::vector<CellID> ids;
  ids.reserve(doc.cells.size());
  for (const auto &[id, cell] : doc.cells) {
    ids.push_back(id);
  }
  std::ranges::sort(ids);

  std::set<DimID> dimensionNames;
  for (const auto &[id, cell] : doc.cells) {
    for (const auto &[dim, links] : cell.dimensions) {
      dimensionNames.insert(dim);
    }
    if (!cell.role.empty()) {
      dimensionNames.insert(DimID{roleDimension});
    }
    if (!cell.mime_type.empty()) {
      dimensionNames.insert(DimID{mimeDimension});
    }
    if (!cell.media_path.empty()) {
      dimensionNames.insert(DimID{mediaDimension});
    }
  }

  auto manifold = store.rebuildManifold(out.version);
  for (const auto &name : dimensionNames) {
    // Reused by name when the store already has it, which is what makes minting
    // a second slice into one store coherent rather than a way to end up with
    // two cells both called "d.1" and a dimensionNamed() that has to pick.
    if (const auto existing = manifold.dimensionNamed(name, store);
        zigzag::noCell != existing) {
      out.dimensions.emplace(name, existing);
      continue;
    }
    const auto minted = store.makeDimension(out.version, name, &manifold);
    out.version       = minted.version;
    out.dimensions.emplace(name, minted.dim);
    manifold = store.rebuildManifold(out.version);
  }

  // One cell per YAML cell, in id order. A number or a flag becomes a scalar
  // cell, so it carries canonical bits as well as a rendering (R6) rather than
  // being flattened to the text it renders as.
  for (const auto id : ids) {
    const auto &cell = doc.cells.at(id);
    if (const auto *const number = std::get_if<double>(&cell.data)) {
      out.version = store.makeScalarCell(out.version, *number);
    } else if (const auto *const flag = std::get_if<bool>(&cell.data)) {
      out.version = store.makeScalarCell(out.version, *flag);
    } else {
      out.version =
          store.makeCell(out.version, zzcore::cellDataAsText(cell.data));
    }
    out.cells.emplace(id, store.cellRefOf(out.version));
  }
  manifold = store.rebuildManifold(out.version);

  const auto linkTo = [&](const CellRef from, const DimRef dim,
                          const DimVector dir, const CellRef to) {
    if (zigzag::noCell == from || zigzag::noCell == dim ||
        zigzag::noCell == to) {
      return;
    }
    out.version = store.setLink(out.version, from, dim, dir, to, &manifold);
    static_cast<void>(manifold.advance(store, out.version));
  };

  // An attribute is a cell of its own on the matching rank, posward of the cell
  // it describes. Minting it needs the manifold to have caught up, so each is
  // minted and then folded before its link is recorded.
  const auto attach = [&](const CellRef owner, const std::string_view dimName,
                          const std::string &value) {
    if (value.empty()) {
      return;
    }
    out.version          = store.makeCell(out.version, value);
    const auto attribute = store.cellRefOf(out.version);
    static_cast<void>(manifold.advance(store, out.version));
    linkTo(owner, out.dimensions.at(DimID{dimName}), DimVector::POS, attribute);
  };

  for (const auto id : ids) {
    const auto &cell  = doc.cells.at(id);
    const auto ownRef = out.cells.at(id);
    attach(ownRef, roleDimension, cell.role);
    attach(ownRef, mimeDimension, cell.mime_type);
    attach(ownRef, mediaDimension, cell.media_path);
  }

  // Posward links only, plus a negward one the document does not state
  // reciprocally. The fold maintains both ends of an edge, so restating it
  // would write the same edge twice.
  for (const auto id : ids) {
    const auto &cell = doc.cells.at(id);
    std::vector<DimID> dims;
    dims.reserve(cell.dimensions.size());
    for (const auto &[dim, links] : cell.dimensions) {
      dims.push_back(dim);
    }
    std::ranges::sort(dims);

    for (const auto &dim : dims) {
      const auto links   = cell.dimensions.at(dim);
      const auto dimCell = out.dimensions.at(dim);
      if (0 != links.pos && out.cells.contains(links.pos)) {
        linkTo(out.cells.at(id), dimCell, DimVector::POS,
               out.cells.at(links.pos));
      }
      if (0 != links.neg && out.cells.contains(links.neg)) {
        const auto &other    = doc.cells.at(links.neg);
        const auto reachesUs = other.dimensions.contains(dim) &&
                               other.dimensions.at(dim).pos == id;
        if (!reachesUs) {
          linkTo(out.cells.at(id), dimCell, DimVector::NEG,
                 out.cells.at(links.neg));
        }
      }
    }
  }

  if (0 != doc.focus && out.cells.contains(doc.focus)) {
    out.focus = out.cells.at(doc.focus);
  }
  return out;
}

ZzStructureDocument storeToSlice(const xanadu::Store &store,
                                 const Manifold &manifold,
                                 const CellRef focus) {
  ZzStructureDocument doc;
  doc.focus = focus;

  // A dimension's name is its cell's content, and the attribute ranks are read
  // back out rather than emitted as cells of their own -- which is what makes
  // this the inverse of sliceToStore() rather than a dump of the manifold.
  std::unordered_map<DimRef, DimID> names;
  DimRef roleDim  = zigzag::noCell;
  DimRef mimeDim  = zigzag::noCell;
  DimRef mediaDim = zigzag::noCell;
  for (const auto dim : manifold.dimensions()) {
    const auto name = manifold.textOf(dim, store);
    names.emplace(dim, name);
    if (name == roleDimension) {
      roleDim = dim;
    } else if (name == mimeDimension) {
      mimeDim = dim;
    } else if (name == mediaDimension) {
      mediaDim = dim;
    }
  }

  // Attribute cells and the dimension cells themselves are structure rather
  // than content: they were minted by the conversion, so emitting them as
  // cells would grow the document on every round trip.
  std::set<CellRef> structural;
  structural.insert(manifold.home());
  for (const auto &[dim, name] : names) {
    structural.insert(dim);
  }
  for (const auto &slot : manifold.cells()) {
    for (const auto attribute : {roleDim, mimeDim, mediaDim}) {
      if (zigzag::noCell == attribute) {
        continue;
      }
      if (const auto held =
              manifold.linked(slot.birthOp, attribute, DimVector::POS);
          zigzag::noCell != held) {
        structural.insert(held);
      }
    }
  }

  for (const auto &slot : manifold.cells()) {
    if (structural.contains(slot.birthOp)) {
      continue;
    }
    Cell cell;
    cell.id = slot.birthOp;
    if (const auto number = manifold.asDouble(slot.birthOp)) {
      cell.data = *number;
    } else if (const auto flag = manifold.asBool(slot.birthOp)) {
      cell.data = *flag;
    } else {
      cell.data = manifold.textOf(slot.birthOp, store);
    }

    const auto attributeOf = [&](const DimRef dim) {
      if (zigzag::noCell == dim) {
        return std::string{};
      }
      const auto held = manifold.linked(slot.birthOp, dim, DimVector::POS);
      return zigzag::noCell == held ? std::string{}
                                    : manifold.textOf(held, store);
    };
    cell.role       = attributeOf(roleDim);
    cell.mime_type  = attributeOf(mimeDim);
    cell.media_path = attributeOf(mediaDim);

    for (const auto &link : manifold.dimensionsOf(slot.birthOp)) {
      const auto named = names.find(link.dim);
      if (named == names.end() || link.dim == roleDim || link.dim == mimeDim ||
          link.dim == mediaDim) {
        continue;
      }
      LinkPairs pairs;
      pairs.pos = structural.contains(link.pos) ? 0 : link.pos;
      pairs.neg = structural.contains(link.neg) ? 0 : link.neg;
      if (0 != pairs.pos || 0 != pairs.neg) {
        cell.dimensions[named->second] = pairs;
      }
    }
    doc.cells[cell.id] = std::move(cell);
  }
  return doc;
}

xanadu::LinkPackage
storeToLinkPackage(const xanadu::Store &store, const Manifold &manifold,
                   const xanadu::MutableKeys &keys, const std::string &salt,
                   const std::int64_t sequence, const std::string &title) {
  std::vector<xanadu::GlobalLink> links;
  std::map<std::string, xanadu::Scroll> scrolls;

  std::unordered_map<DimRef, DimID> names;
  DimRef roleDim  = zigzag::noCell;
  DimRef mimeDim  = zigzag::noCell;
  DimRef mediaDim = zigzag::noCell;
  for (const auto dim : manifold.dimensions()) {
    const auto name = manifold.textOf(dim, store);
    names.emplace(dim, name);
    if (name == roleDimension) {
      roleDim = dim;
    } else if (name == mimeDimension) {
      mimeDim = dim;
    } else if (name == mediaDimension) {
      mediaDim = dim;
    }
  }

  std::set<CellRef> structural;
  structural.insert(manifold.home());
  for (const auto &[dim, name] : names) {
    structural.insert(dim);
  }
  for (const auto &slot : manifold.cells()) {
    for (const auto attribute : {roleDim, mimeDim, mediaDim}) {
      if (zigzag::noCell == attribute) {
        continue;
      }
      if (const auto held =
              manifold.linked(slot.birthOp, attribute, DimVector::POS);
          zigzag::noCell != held) {
        structural.insert(held);
      }
    }
  }

  const auto localScroll = store.userPermascroll().currentScroll();
  const auto scrollFor   = [&store,
                            &localScroll](const xanadu::PrimediaSpan &span) {
    return span.isLocal() ? &localScroll : store.scroll(span.scroll);
  };

  std::unordered_map<CellRef, xanadu::GlobalSpan> cellSpans;
  for (const auto &slot : manifold.cells()) {
    if (structural.contains(slot.birthOp)) {
      continue;
    }
    // The first span of the cell's content. A cell whose content is several
    // spans -- because it has been edited, see U3 -- is published under the
    // address of its first, which is what a package holding one GlobalSpan per
    // cell can say. Carrying all of them is a change to LinkPackage's shape.
    const auto contentRun = manifold.contentOf(slot.birthOp);
    if (contentRun.empty()) {
      continue;
    }
    if (const auto global =
            xanadu::globalise(store, contentRun.front(), &localScroll)) {
      cellSpans.emplace(slot.birthOp, *global);
      if (const auto *const s = scrollFor(contentRun.front())) {
        scrolls.insert_or_assign(global->scroll, *s);
      }
    }
  }

  for (const auto &slot : manifold.cells()) {
    if (structural.contains(slot.birthOp)) {
      continue;
    }
    const auto itFrom = cellSpans.find(slot.birthOp);
    if (itFrom == cellSpans.end()) {
      continue;
    }
    for (const auto &link : manifold.dimensionsOf(slot.birthOp)) {
      if (link.dim == roleDim || link.dim == mimeDim || link.dim == mediaDim) {
        continue;
      }
      if (link.pos != 0 && !structural.contains(link.pos)) {
        const auto itTo = cellSpans.find(link.pos);
        if (itTo != cellSpans.end()) {
          const auto dimIt = names.find(link.dim);
          const std::string dimName =
              (dimIt != names.end()) ? dimIt->second : "d.1";
          xanadu::GlobalLink gLink;
          gLink.type  = xanadu::LinkType::Dimension;
          gLink.tier  = xanadu::ProminenceTier::Author;
          gLink.owner = "dim:" + dimName;
          gLink.left.push_back(itFrom->second);
          gLink.right.push_back(itTo->second);
          links.push_back(std::move(gLink));
        }
      }
    }
  }

  const std::string pkgTitle =
      title.empty() ? "Zigzag Slice Link Package" : title;
  return xanadu::publishLinkPackage(
      keys, salt, pkgTitle, sequence,
      static_cast<std::uint64_t>(std::time(nullptr)), std::move(links),
      std::move(scrolls));
}

xanadu::LinkPackage zzStructureToLinkPackage(const ZzStructureDocument &doc,
                                             const xanadu::MutableKeys &keys,
                                             const std::string &salt,
                                             const std::int64_t sequence) {
  xanadu::Store store;
  const auto sliced   = sliceToStore(doc, store);
  const auto manifold = store.rebuildManifold(sliced.version);
  return storeToLinkPackage(store, manifold, keys, salt, sequence,
                            doc.meta.name);
}

bool validate2RankManifold(const ZzStructureDocument &doc,
                           std::string *errorOut) {
  for (const auto &[id, cell] : doc.cells) {
    for (const auto &[dim, linkPairs] : cell.dimensions) {
      if (linkPairs.pos != 0) {
        const auto targetIt = doc.cells.find(linkPairs.pos);
        if (targetIt == doc.cells.end()) {
          if (errorOut) {
            *errorOut = std::format("Cell {} links to non-existent positive "
                                    "target {} on dimension {}",
                                    id, linkPairs.pos, dim);
          }
          return false;
        }
        const auto backIt = targetIt->second.dimensions.find(dim);
        if (backIt == targetIt->second.dimensions.end() ||
            backIt->second.neg != id) {
          if (errorOut) {
            *errorOut =
                std::format("Asymmetric link between {} and {} on dimension {}",
                            id, linkPairs.pos, dim);
          }
          return false;
        }
      }
      if (linkPairs.neg != 0) {
        const auto targetIt = doc.cells.find(linkPairs.neg);
        if (targetIt == doc.cells.end()) {
          if (errorOut) {
            *errorOut = std::format("Cell {} links to non-existent negative "
                                    "target {} on dimension {}",
                                    id, linkPairs.neg, dim);
          }
          return false;
        }
        const auto backIt = targetIt->second.dimensions.find(dim);
        if (backIt == targetIt->second.dimensions.end() ||
            backIt->second.pos != id) {
          if (errorOut) {
            *errorOut =
                std::format("Asymmetric link between {} and {} on dimension {}",
                            id, linkPairs.neg, dim);
          }
          return false;
        }
      }
    }
  }
  return true;
}

bool verifySliceAuthor(const ZzStructureDocument &doc,
                       const xanadu::MerkleLedger &ledger,
                       const std::array<std::uint8_t, 32> &expectedRoot,
                       std::string *errorOut) {
  if (doc.meta.author.empty()) {
    if (errorOut) {
      *errorOut = "Slice metadata has no author declared";
    }
    return false;
  }
  std::string email;
  const auto start = doc.meta.author.find('<');
  const auto end   = doc.meta.author.find('>');
  if (start != std::string::npos && end != std::string::npos &&
      end > start + 1) {
    email = doc.meta.author.substr(start + 1, end - start - 1);
  } else {
    email = doc.meta.author;
  }

  const auto links = ledger.findByEmail(email);
  if (links.empty()) {
    if (errorOut) {
      *errorOut = "No verified ledger entry found for author email: " + email;
    }
    return false;
  }

  const auto *link = links.front();
  if (link->revoked) {
    if (errorOut) {
      *errorOut = "Ledger entry for author email is revoked";
    }
    return false;
  }

  const auto proof = ledger.generateProof(link->sequence);
  if (!xanadu::MerkleLedger::verifyInclusion(*link, proof, expectedRoot)) {
    if (errorOut) {
      *errorOut = "Merkle inclusion proof failed against expected root";
    }
    return false;
  }
  return true;
}

} // namespace zigzag
