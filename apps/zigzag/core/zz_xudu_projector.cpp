/**
 * @file zz_xudu_projector.cpp
 * @brief Implementation of bidirectional projection and rasterization between
 *        Xudu (Xanadocs/Xanalinks) and Zigzag (Multidimensional cell space).
 */
#include "zz_xudu_projector.hpp"

#include <algorithm>
#include <ctime>
#include <format>
#include <map>
#include <sstream>
#include <unordered_set>

#include "xudu/core/format.hpp"
#include "zzcore.hpp"

namespace zigzag {

namespace {

bool spansOverlap(const xudu::PrimediaSpan &a, const xudu::PrimediaSpan &b) {
  if (a.scroll != b.scroll || a.empty() || b.empty()) {
    return false;
  }
  return !a.intersect(b).empty();
}

} // namespace

ZzStructureDocument
projectXuduToZigzag(const std::vector<XuduDocInput> &docs,
                    const std::vector<xudu::Link> &links,
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

  CellID nextCellId = 1;
  struct CellMapping {
    CellID id{0};
    std::size_t docIndex{0};
    xudu::PrimediaSpan span;
  };
  std::vector<CellMapping> cellMappings;
  std::vector<std::vector<CellID>> docCellChains(docs.size());

  for (std::size_t docIdx = 0; docIdx < docs.size(); ++docIdx) {
    const auto &doc = docs[docIdx];
    if (doc.text.empty()) {
      continue;
    }

    if (opts.split_by_paragraphs) {
      // Split text into paragraphs
      std::size_t start   = 0;
      std::size_t paraIdx = 0;
      while (start < doc.text.size()) {
        std::size_t end = doc.text.find("\n\n", start);
        if (end == std::string::npos) {
          end = doc.text.size();
        }

        std::string paraText = doc.text.substr(start, end - start);
        const auto firstNonWs = paraText.find_first_not_of(" \t\r\n");
        const auto lastNonWs  = paraText.find_last_not_of(" \t\r\n");
        if (firstNonWs != std::string::npos && lastNonWs != std::string::npos) {
          paraText =
              paraText.substr(firstNonWs, lastNonWs - firstNonWs + 1);
        }

        if (!paraText.empty()) {
          const CellID id = nextCellId++;
          zzCell cell;
          cell.id        = id;
          cell.text_data = std::move(paraText);
          cell.type      = "xudu_span";

          xudu::PrimediaSpan span;
          if (!doc.spans.empty() && paraIdx < doc.spans.size()) {
            span = doc.spans[paraIdx];
          }
          ++paraIdx;

          result.cells[id] = std::move(cell);
          docCellChains[docIdx].push_back(id);
          cellMappings.push_back(CellMapping{id, docIdx, span});
        }

        start = (end == doc.text.size()) ? end : end + 2;
      }
    } else {

      const CellID id = nextCellId++;
      zzCell cell;
      cell.id        = id;
      cell.text_data = doc.text;
      cell.type      = "xudu_document";

      xudu::PrimediaSpan span;
      if (!doc.spans.empty()) {
        span = doc.spans.front();
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
      const CellID c1 = chain[i];
      const CellID c2 = chain[i + 1];
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
projectStoreToZigzag(const xudu::Store &store,
                     const std::vector<xudu::MicroversionId> &versions,
                     const XuduProjectorOptions &opts) {
  std::vector<XuduDocInput> docInputs;
  for (const auto &verId : versions) {
    const auto ver            = store.rebuild(verId);
    std::string assembledText = store.textOf(verId);
    std::vector<xudu::PrimediaSpan> spans = ver.pieces();

    docInputs.push_back(XuduDocInput{
        .name    = verId.str(),
        .text    = std::move(assembledText),
        .version = verId,
        .spans   = std::move(spans),
    });
  }

  std::vector<xudu::Link> allLinks;
  for (const auto &[linkId, link] : store.links()) {
    allLinks.push_back(link);
  }

  return projectXuduToZigzag(docInputs, allLinks, opts);
}


ZzRasterResult
rasterizeZzStructure(const ZzStructureDocument &doc,
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
    CellID current = colHead;
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

      result.text += it->second.text_data;
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
  while (!result.text.empty() && (result.text.back() == '\n' || result.text.back() == ' ')) {
    result.text.pop_back();
  }

  return result;
}

xudu::LinkPackage
zzStructureToLinkPackage(const ZzStructureDocument &doc,
                         const xudu::MutableKeys &keys,
                         const std::string &salt,
                         const std::int64_t sequence) {
  std::vector<xudu::GlobalLink> links;
  std::map<std::string, xudu::Scroll> scrolls;

  const std::string scrollName =
      "slice:" + (doc.meta.name.empty() ? "anonymous" : doc.meta.name);
  std::uint64_t currentOffset = 0;
  std::unordered_map<CellID, xudu::GlobalSpan> cellSpans;

  for (const auto &[id, cell] : doc.cells) {
    const auto len = static_cast<std::uint64_t>(cell.text_data.size());
    const xudu::GlobalSpan span{scrollName, currentOffset, len};
    cellSpans[id] = span;
    currentOffset += len;
  }

  for (const auto &[id, cell] : doc.cells) {
    for (const auto &[dim, linkPairs] : cell.dimensions) {
      if (linkPairs.pos != 0 && cellSpans.contains(linkPairs.pos)) {
        xudu::GlobalLink gLink;
        gLink.type  = xudu::LinkType::Dimension;
        gLink.tier  = xudu::ProminenceTier::Author;
        gLink.owner = "dim:" + dim;
        gLink.left.push_back(cellSpans[id]);
        gLink.right.push_back(cellSpans[linkPairs.pos]);
        links.push_back(std::move(gLink));
      }
    }
  }

  return xudu::publishLinkPackage(
      keys, salt, doc.meta.name, sequence,
      static_cast<std::uint64_t>(std::time(nullptr)),
      std::move(links), std::move(scrolls));
}

ZzStructureDocument
linkPackageToZzStructure(const xudu::LinkPackage &pkg) {
  ZzStructureDocument doc;
  doc.meta.name = pkg.title.empty() ? "Imported Link Package" : pkg.title;
  doc.focus     = 1;

  std::map<xudu::GlobalSpan, CellID> spanToCell;
  CellID nextCellId = 1;

  for (const auto &link : pkg.links) {
    if (link.type == xudu::LinkType::Dimension) {
      for (const auto &span : link.left) {
        if (!spanToCell.contains(span)) {
          const CellID id = nextCellId++;
          spanToCell[span] = id;
          zzCell cell;
          cell.id = id;
          cell.type = "cell";
          cell.text_data = std::format("Cell #{} [{}]", id, span.scroll);
          doc.cells[id] = std::move(cell);
        }
      }
      for (const auto &span : link.right) {
        if (!spanToCell.contains(span)) {
          const CellID id = nextCellId++;
          spanToCell[span] = id;
          zzCell cell;
          cell.id = id;
          cell.type = "cell";
          cell.text_data = std::format("Cell #{} [{}]", id, span.scroll);
          doc.cells[id] = std::move(cell);
        }
      }

      std::string dimName = "d.1";
      if (link.owner.starts_with("dim:")) {
        dimName = link.owner.substr(4);
      }

      if (!link.left.empty() && !link.right.empty()) {
        const CellID c1 = spanToCell[link.left.front()];
        const CellID c2 = spanToCell[link.right.front()];
        doc.cells[c1].dimensions[dimName].pos = c2;
        doc.cells[c2].dimensions[dimName].neg = c1;
      }
    }
  }

  return doc;
}

bool validate2RankManifold(const ZzStructureDocument &doc, std::string *errorOut) {
  for (const auto &[id, cell] : doc.cells) {
    for (const auto &[dim, linkPairs] : cell.dimensions) {
      if (linkPairs.pos != 0) {
        const auto targetIt = doc.cells.find(linkPairs.pos);
        if (targetIt == doc.cells.end()) {
          if (errorOut) {
            *errorOut = std::format("Cell {} links to non-existent positive target {} on dimension {}",
                                    id, linkPairs.pos, dim);
          }
          return false;
        }
        const auto backIt = targetIt->second.dimensions.find(dim);
        if (backIt == targetIt->second.dimensions.end() || backIt->second.neg != id) {
          if (errorOut) {
            *errorOut = std::format("Asymmetric link between {} and {} on dimension {}",
                                    id, linkPairs.pos, dim);
          }
          return false;
        }
      }
      if (linkPairs.neg != 0) {
        const auto targetIt = doc.cells.find(linkPairs.neg);
        if (targetIt == doc.cells.end()) {
          if (errorOut) {
            *errorOut = std::format("Cell {} links to non-existent negative target {} on dimension {}",
                                    id, linkPairs.neg, dim);
          }
          return false;
        }
        const auto backIt = targetIt->second.dimensions.find(dim);
        if (backIt == targetIt->second.dimensions.end() || backIt->second.pos != id) {
          if (errorOut) {
            *errorOut = std::format("Asymmetric link between {} and {} on dimension {}",
                                    id, linkPairs.neg, dim);
          }
          return false;
        }
      }
    }
  }
  return true;
}

} // namespace zigzag
