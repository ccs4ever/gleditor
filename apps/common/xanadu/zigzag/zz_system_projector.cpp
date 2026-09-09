/**
 * @file zz_system_projector.cpp
 * @brief Implementation of bidirectional projection between Zigzag slices and
 *        sovereign system xanadocs.
 */
#include "common/xanadu/zigzag/zz_system_projector.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/xanadu/format.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/version.hpp"

namespace zigzag {

std::string extractSliceConfigText(const ZzStructureDocument &slice) {
  // Find cells with incoming negative links on d.config (i.e. targets of pos
  // links)
  std::unordered_set<CellID> hasIncomingNeg;
  for (const auto &[id, c] : slice.cells) {
    const auto it = c.dimensions.find(std::string(kDimConfig));
    if (it != c.dimensions.end() && it->second.pos != 0) {
      hasIncomingNeg.insert(it->second.pos);
    }
  }

  // Find root on d.config
  CellID rootId = 0;
  for (const auto &[id, c] : slice.cells) {
    const auto it = c.dimensions.find(std::string(kDimConfig));
    if (it != c.dimensions.end() && it->second.pos != 0 &&
        !hasIncomingNeg.contains(id)) {
      rootId = id;
      break;
    }
  }

  if (rootId == 0 && slice.focus != 0 && slice.cells.contains(slice.focus)) {
    rootId = slice.focus;
  }

  std::string out;
  std::unordered_set<CellID> visited;
  CellID cur = rootId;

  while (cur != 0 && !visited.contains(cur)) {
    visited.insert(cur);
    const auto itCell = slice.cells.find(cur);
    if (itCell == slice.cells.end()) {
      break;
    }
    const auto &c = itCell->second;
    if (c.role == "setting") {
      out += c.text();
      if (!out.ends_with('\n')) {
        out += '\n';
      }
    } else if (c.text().find(':') != std::string::npos &&
               c.role != "config_group" && !c.text().starts_with("Schema:") &&
               !c.text().starts_with("Notes:")) {
      out += c.text();
      if (!out.ends_with('\n')) {
        out += '\n';
      }
    }

    const auto itDim = c.dimensions.find(std::string(kDimConfig));
    cur              = (itDim != c.dimensions.end()) ? itDim->second.pos : 0;
  }

  // Fallback: iterate slice cells directly if traversal yielded nothing
  if (out.empty()) {
    for (const auto &[id, c] : slice.cells) {
      if (c.role == "setting" ||
          (c.text().find(':') != std::string::npos &&
           c.role != "config_group" && !c.text().starts_with("Schema:") &&
           !c.text().starts_with("Notes:") && c.role != "schema_doc" &&
           c.role != "schema_field" && c.role != "user_notes")) {
        out += c.text();
        if (!out.ends_with('\n')) {
          out += '\n';
        }
      }
    }
  }

  return out;
}

std::string extractSliceSchemaText(const ZzStructureDocument &slice) {
  const Cell *schemaDoc = nullptr;
  for (const auto &[id, c] : slice.cells) {
    if (c.role == "schema_doc" || c.text().starts_with("Schema and Purpose")) {
      schemaDoc = &c;
      break;
    }
  }

  std::string out;
  if (schemaDoc != nullptr) {
    out += schemaDoc->text();
    if (!out.ends_with('\n')) {
      out += "\n\n";
    } else if (!out.ends_with("\n\n")) {
      out += '\n';
    }
  }

  for (const auto &[id, c] : slice.cells) {
    if (&c == schemaDoc) {
      continue;
    }
    if (c.role == "schema_field" || c.text().starts_with("Schema:")) {
      if (out.find(c.text()) == std::string::npos) {
        out += c.text();
        if (!out.ends_with('\n')) {
          out += '\n';
        }
      }
    }
  }

  return out;
}

std::string extractSliceNotesText(const ZzStructureDocument &slice) {
  const Cell *rootNote = nullptr;
  for (const auto &[id, c] : slice.cells) {
    if (c.role == "user_notes" &&
        (c.text().starts_with("Notes\n") || c.text() == "Notes" ||
         rootNote == nullptr)) {
      rootNote = &c;
      if (c.text().starts_with("Notes\n") || c.text() == "Notes") {
        break;
      }
    }
  }

  std::string out;
  if (rootNote != nullptr) {
    out += rootNote->text();
    if (!out.ends_with('\n')) {
      out += "\n\n";
    } else if (!out.ends_with("\n\n")) {
      out += '\n';
    }
  }

  for (const auto &[id, c] : slice.cells) {
    if (&c == rootNote) {
      continue;
    }
    if (c.role == "user_notes" || c.text().starts_with("Notes:")) {
      if (out.find(c.text()) == std::string::npos) {
        out += c.text();
        if (!out.ends_with('\n')) {
          out += '\n';
        }
      }
    }
  }

  return out;
}

xanadu::MicroversionId
projectSystemSliceToStore(const ZzStructureDocument &slice,
                          xanadu::Store &store,
                          const xanadu::SystemDocKind kind) {
  std::string p1 = extractSliceConfigText(slice);
  if (p1.empty()) {
    p1 = xanadu::defaultSystemDocContent(kind);
  }

  std::string p2 = extractSliceSchemaText(slice);
  if (p2.empty()) {
    p2 = xanadu::defaultSystemDocSchema(kind);
  }

  std::string p3 = extractSliceNotesText(slice);
  if (p3.empty()) {
    p3 = xanadu::defaultSystemDocNotes(kind);
  }

  xanadu::MicroversionId cur{};
  cur = store.insert(cur, 0, p1);

  const auto p1Size = static_cast<std::uint32_t>(p1.size());
  cur               = store.insertBreak(cur, p1Size);
  cur               = store.insert(cur, p1Size, p2);

  const auto p12Size = static_cast<std::uint32_t>(p1Size + p2.size());
  cur                = store.insertBreak(cur, p12Size);
  cur                = store.insert(cur, p12Size, p3);

  const auto doc = store.rebuild(cur);

  // Format links for Page 2 header: "Schema and Purpose" (centered and bold)
  constexpr std::string_view schemaHeader = "Schema and Purpose";
  const auto schemaHeaderSpans =
      doc.spansFor(p1Size, static_cast<std::uint32_t>(schemaHeader.size()));
  if (!schemaHeaderSpans.empty()) {
    xanadu::Link boldLink;
    boldLink.type  = xanadu::LinkType::Format;
    boldLink.tier  = xanadu::ProminenceTier::Author;
    boldLink.owner = "system";
    boldLink.left  = schemaHeaderSpans;
    boldLink.right = {xanadu::vocabularySpanFor(xanadu::FormatAttribute::Bold)};
    cur            = store.addLink(cur, std::move(boldLink));

    xanadu::Link centreLink;
    centreLink.type  = xanadu::LinkType::Format;
    centreLink.tier  = xanadu::ProminenceTier::Author;
    centreLink.owner = "system";
    centreLink.left  = schemaHeaderSpans;
    centreLink.right = {
        xanadu::vocabularySpanFor(xanadu::FormatAttribute::AlignCentre)};
    cur = store.addLink(cur, std::move(centreLink));
  }

  // Format links for Page 3 header: "Notes" (centered and bold)
  constexpr std::string_view notesHeader = "Notes";
  const auto notesHeaderSpans =
      doc.spansFor(p12Size, static_cast<std::uint32_t>(notesHeader.size()));
  if (!notesHeaderSpans.empty()) {
    xanadu::Link boldLink;
    boldLink.type  = xanadu::LinkType::Format;
    boldLink.tier  = xanadu::ProminenceTier::Author;
    boldLink.owner = "system";
    boldLink.left  = notesHeaderSpans;
    boldLink.right = {xanadu::vocabularySpanFor(xanadu::FormatAttribute::Bold)};
    cur            = store.addLink(cur, std::move(boldLink));

    xanadu::Link centreLink;
    centreLink.type  = xanadu::LinkType::Format;
    centreLink.tier  = xanadu::ProminenceTier::Author;
    centreLink.owner = "system";
    centreLink.left  = notesHeaderSpans;
    centreLink.right = {
        xanadu::vocabularySpanFor(xanadu::FormatAttribute::AlignCentre)};
    cur = store.addLink(cur, std::move(centreLink));
  }

  // Butterfly links connecting config (Page 1) to Schema (Page 2) and Notes
  // (Page 3)
  const auto p1Spans = doc.spansFor(0, p1Size);
  const auto p2Spans =
      doc.spansFor(p1Size, static_cast<std::uint32_t>(p2.size()));
  const auto p3Spans =
      doc.spansFor(p12Size, static_cast<std::uint32_t>(p3.size()));

  if (!p1Spans.empty() && !p2Spans.empty()) {
    xanadu::Link schemaLink;
    schemaLink.type  = xanadu::LinkType::Comment;
    schemaLink.tier  = xanadu::ProminenceTier::Author;
    schemaLink.owner = "system";
    schemaLink.left  = p1Spans;
    schemaLink.right = p2Spans;
    cur              = store.addLink(cur, std::move(schemaLink));
  }

  if (!p1Spans.empty() && !p3Spans.empty()) {
    xanadu::Link notesLink;
    notesLink.type  = xanadu::LinkType::Comment;
    notesLink.tier  = xanadu::ProminenceTier::Author;
    notesLink.owner = "system";
    notesLink.left  = p1Spans;
    notesLink.right = p3Spans;
    cur             = store.addLink(cur, std::move(notesLink));
  }

  store.repointCurrentVersion(cur);
  store.setVersionAnnotation(
      cur, {.alias = "default",
            .description =
                "System default " + std::string(xanadu::systemDocName(kind)),
            .tag       = "system",
            .timestamp = ""});

  return cur;
}

ZzStructureDocument
projectSystemStoreToSlice(const xanadu::Store &store,
                          const xanadu::SystemDocKind kind) {
  const auto &curVers = store.currentVersions();
  const auto curVer =
      curVers.empty() ? xanadu::MicroversionId{} : curVers.front();
  const auto ver  = store.rebuild(curVer);
  const auto full = ver.materialize(store);

  std::string_view fullView = full;
  const auto p1View         = xanadu::extractConfigSection(fullView);

  std::string p2;
  std::string p3;
  const auto schemaPos = fullView.find("Schema and Purpose");
  const auto notesPos  = fullView.find("Notes\n");

  if (schemaPos != std::string_view::npos) {
    if (notesPos != std::string_view::npos && notesPos > schemaPos) {
      p2 = std::string(fullView.substr(schemaPos, notesPos - schemaPos));
      p3 = std::string(fullView.substr(notesPos));
    } else {
      p2 = std::string(fullView.substr(schemaPos));
    }
  }

  ZzStructureDocument result;
  result.meta.name =
      "ZigZag System " + std::string(xanadu::systemDocName(kind)) + " Slice";
  result.meta.description = "Sovereign runtime parameters for system://" +
                            std::string(xanadu::systemDocName(kind));
  result.meta.version     = "1.0";
  result.meta.author      = "Project Xanadu ZigZag System";
  result.meta.tags        = {"system", std::string(xanadu::systemDocName(kind)),
                             "configuration", "zigzag"};

  result.focus = 1;

  result.view.x_dimension = std::string(kDimConfig);
  result.view.y_dimension = std::string(kDimSchema);
  result.view.z_dimension = std::string(kDimNotes);

  result.dimension_meta[std::string(kDimConfig)] =
      DimensionMeta{.label       = "Configuration",
                    .description = "Runtime settings and parameters",
                    .color       = RgbColor{0.31F, 0.62F, 0.88F},
                    .spacing     = 2.5F};
  result.dimension_meta[std::string(kDimSchema)] =
      DimensionMeta{.label       = "Schema",
                    .description = "Specification of cell purpose and units",
                    .color       = RgbColor{0.96F, 0.65F, 0.14F},
                    .spacing     = 2.2F};
  result.dimension_meta[std::string(kDimNotes)] = DimensionMeta{
      .label       = "Notes",
      .description = "User annotations and display calibration notes",
      .color       = RgbColor{0.61F, 0.32F, 0.88F},
      .spacing     = 2.2F};

  // Group cell (id 1)
  Cell groupCell;
  groupCell.id   = 1;
  groupCell.data = std::string(xanadu::systemDocName(kind)) + " Settings";
  groupCell.role = "config_group";
  groupCell.dimensions[std::string(kDimSchema)].pos = 100;
  groupCell.dimensions[std::string(kDimNotes)].pos  = 200;

  CellID lastSettingId = 1;
  CellID nextId        = 2;

  std::istringstream iss{std::string(p1View)};
  std::string line;
  while (std::getline(iss, line)) {
    // Trim whitespace
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
      continue;
    }
    const auto last    = line.find_last_not_of(" \t\r\n");
    const auto trimmed = line.substr(first, last - first + 1);
    if (trimmed.empty() || trimmed.starts_with('#')) {
      continue;
    }

    const CellID cid = nextId++;
    Cell settingCell;
    settingCell.id   = cid;
    settingCell.data = trimmed;
    settingCell.role = "setting";

    // Link previous setting on d.config
    if (lastSettingId == 1) {
      groupCell.dimensions[std::string(kDimConfig)].pos = cid;
    } else {
      result.cells[lastSettingId].dimensions[std::string(kDimConfig)].pos = cid;
    }

    lastSettingId     = cid;
    result.cells[cid] = std::move(settingCell);
  }

  result.cells[1] = std::move(groupCell);

  // Schema doc cell (id 100)
  Cell schemaCell;
  schemaCell.id     = 100;
  schemaCell.data   = p2.empty() ? xanadu::defaultSystemDocSchema(kind) : p2;
  schemaCell.role   = "schema_doc";
  result.cells[100] = std::move(schemaCell);

  // Notes doc cell (id 200)
  Cell notesCell;
  notesCell.id      = 200;
  notesCell.data    = p3.empty() ? xanadu::defaultSystemDocNotes(kind) : p3;
  notesCell.role    = "user_notes";
  result.cells[200] = std::move(notesCell);

  return result;
}

} // namespace zigzag
