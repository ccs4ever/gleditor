/**
 * @file zzstructure_loader.cpp
 * @brief Implementation of ZigZag Slice YAML loader using rapidyaml.
 */
#include "zzstructure_loader.hpp"
#include "zzcore.hpp"

#include <ryml.hpp>
#include <ryml_std.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zigzag {

namespace {

using zzcore::Diagnostics;
using zzcore::ExplicitLink;

void rymlErrorHandler(const c4::csubstr msg, const c4::yml::ErrorDataBasic &,
                      void *) {
  throw std::runtime_error(std::string{msg.str, msg.len});
}

struct RymlCallbackSetup {
  RymlCallbackSetup() {
    c4::yml::Callbacks cb;
    cb.m_error_basic = rymlErrorHandler;
    c4::yml::set_callbacks(cb);
  }
};

const RymlCallbackSetup setupRymlCallbacks;

std::unexpected<LoadError> fail(const LoadError::Kind kind, std::string message,
                                std::string path) {
  return std::unexpected(LoadError{kind, std::move(message), std::move(path)});
}

std::string str(const c4::csubstr s) { return std::string{s.str, s.len}; }

CellID readCellIdOrZero(const ryml::ConstNodeRef &node) {
  if (!node.readable() || !node.has_val()) {
    return 0;
  }
  CellID id = 0;
  try {
    node >> id;
  } catch (...) {
    return 0;
  }
  return id;
}

RgbColor readColorOr(const ryml::ConstNodeRef &node, const RgbColor &fallback,
                     const std::string_view field, Diagnostics &diagnostics) {
  if (!node.readable() || !node.has_val()) {
    return fallback;
  }
  const std::string text = str(node.val());
  if (const auto parsed = zzcore::parseHexColor(text)) {
    return *parsed;
  }
  diagnostics.warn(std::format("{} colour {} is not a #RRGGBB hex value -- "
                               "using default",
                               field, text));
  return fallback;
}

void readStructureMeta(const ryml::ConstNodeRef &structure,
                       ZzStructureDocument &doc) {
  if (!structure.has_child("meta")) {
    return;
  }
  const ryml::ConstNodeRef metaNode = structure["meta"];
  if (!metaNode.is_map()) {
    return;
  }

  if (metaNode.has_child("name") && metaNode["name"].has_val()) {
    metaNode["name"] >> doc.meta.name;
  }
  if (metaNode.has_child("description") && metaNode["description"].has_val()) {
    metaNode["description"] >> doc.meta.description;
  }
  if (metaNode.has_child("version") && metaNode["version"].has_val()) {
    metaNode["version"] >> doc.meta.version;
  }
  if (metaNode.has_child("author") && metaNode["author"].has_val()) {
    metaNode["author"] >> doc.meta.author;
  }
  if (metaNode.has_child("created") && metaNode["created"].has_val()) {
    metaNode["created"] >> doc.meta.created;
  }
  if (metaNode.has_child("tags") && metaNode["tags"].is_seq()) {
    for (const auto tagNode : metaNode["tags"].children()) {
      if (tagNode.has_val()) {
        doc.meta.tags.push_back(str(tagNode.val()));
      }
    }
  }
}

zzCell readCell(const ryml::ConstNodeRef &cellNode,
                std::vector<ExplicitLink> &explicitLinks,
                Diagnostics &diagnostics) {
  zzCell cell;
  cellNode["id"] >> cell.id;
  if (cellNode.has_child("text") && cellNode["text"].has_val()) {
    cellNode["text"] >> cell.text_data;
  }
  if (cellNode.has_child("type") && cellNode["type"].has_val()) {
    cellNode["type"] >> cell.type;
  }

  if (!cellNode.has_child("dimensions")) {
    return cell;
  }

  const ryml::ConstNodeRef dimsNode = cellNode["dimensions"];
  if (!dimsNode.is_map()) {
    return cell;
  }

  for (const auto entry : dimsNode.children()) {
    if (!entry.has_key()) {
      continue;
    }
    const std::string dimName = str(entry.key());

    LinkPairs links;
    if (entry.has_val() && !entry.is_map()) {
      // Shorthand: scalar id means pos target
      try {
        entry >> links.pos;
      } catch (...) {
        diagnostics.warn(std::format("cell {} dimension {} has an invalid "
                                     "scalar value",
                                     cell.id, dimName));
        continue;
      }
    } else if (entry.is_map()) {
      if (entry.has_child("pos")) {
        links.pos = readCellIdOrZero(entry["pos"]);
      }
      if (entry.has_child("neg")) {
        links.neg = readCellIdOrZero(entry["neg"]);
      }
    } else {
      diagnostics.warn(std::format("cell {} dimension {} has an unrecognized "
                                   "value -- ignoring",
                                   cell.id, dimName));
      continue;
    }

    cell.dimensions[dimName] = links;
    if (links.pos != 0) {
      explicitLinks.push_back({cell.id, dimName, true, links.pos});
    }
    if (links.neg != 0) {
      explicitLinks.push_back({cell.id, dimName, false, links.neg});
    }
  }

  return cell;
}

void readDimensionMeta(const ryml::ConstNodeRef &structure,
                       ZzStructureDocument &doc, Diagnostics &diagnostics) {
  if (!structure.has_child("dimensions")) {
    return;
  }
  const ryml::ConstNodeRef dimsNode = structure["dimensions"];
  if (!dimsNode.is_map()) {
    return;
  }

  for (const auto entry : dimsNode.children()) {
    if (!entry.has_key() || !entry.is_map()) {
      continue;
    }
    const std::string dimName = str(entry.key());
    DimensionMeta meta;

    if (entry.has_child("label") && entry["label"].has_val()) {
      entry["label"] >> meta.label;
    }
    if (entry.has_child("description") && entry["description"].has_val()) {
      entry["description"] >> meta.description;
    }
    if (entry.has_child("color")) {
      meta.color = readColorOr(entry["color"], meta.color,
                               "dimension " + dimName, diagnostics);
    }
    if (entry.has_child("spacing") && entry["spacing"].has_val()) {
      try {
        entry["spacing"] >> meta.spacing;
      } catch (...) {
      }
    }

    doc.dimension_meta[dimName] = meta;
  }
}

void readSceneMeta(const ryml::ConstNodeRef &structure,
                   ZzStructureDocument &doc, Diagnostics &diagnostics) {
  if (!structure.has_child("scene")) {
    return;
  }
  const ryml::ConstNodeRef sceneNode = structure["scene"];
  if (!sceneNode.is_map()) {
    return;
  }

  if (sceneNode.has_child("background_color")) {
    doc.scene.background =
        readColorOr(sceneNode["background_color"], doc.scene.background,
                    "scene background", diagnostics);
  }
  if (sceneNode.has_child("focus_color")) {
    doc.scene.focus_color =
        readColorOr(sceneNode["focus_color"], doc.scene.focus_color,
                    "scene focus", diagnostics);
  }
  if (sceneNode.has_child("focus_scale") &&
      sceneNode["focus_scale"].has_val()) {
    try {
      sceneNode["focus_scale"] >> doc.scene.focus_scale;
    } catch (...) {
    }
  }
  if (sceneNode.has_child("cell_radius") &&
      sceneNode["cell_radius"].has_val()) {
    try {
      sceneNode["cell_radius"] >> doc.scene.cell_radius;
    } catch (...) {
    }
  }
}

void reportDiagnostics(const Diagnostics &diagnostics,
                       const std::string_view path) {
  for (const auto &entry : diagnostics.entries()) {
    const auto prefix =
        entry.severity == zzcore::Severity::Error ? "error" : "warning";
    std::cerr << std::format("zzstructure [{}]: {} ({})\n", prefix,
                             entry.message, path);
  }
}

std::expected<ZzStructureDocument, LoadError>
buildDocument(const ryml::ConstNodeRef &root, const std::string &origin) {
  if (!root.readable() || !root.has_child("zzstructure")) {
    return fail(LoadError::Kind::SchemaViolation,
                "has no top-level 'zzstructure' map", origin);
  }

  const ryml::ConstNodeRef structure = root["zzstructure"];
  if (!structure.is_map()) {
    return fail(LoadError::Kind::SchemaViolation, "'zzstructure' is not a map",
                origin);
  }

  if (!structure.has_child("cells") || !structure["cells"].is_seq() ||
      structure["cells"].num_children() == 0) {
    return fail(LoadError::Kind::SchemaViolation, "defines no cells", origin);
  }

  Diagnostics diagnostics;
  ZzStructureDocument doc;

  readStructureMeta(structure, doc);
  if (doc.meta.name.empty()) {
    doc.meta.name = origin;
  }

  std::vector<ExplicitLink> explicitLinks;
  for (const auto cellNode : structure["cells"].children()) {
    if (!cellNode.has_child("id") || !cellNode["id"].has_val()) {
      diagnostics.warn("skipping a cell with no 'id'");
      continue;
    }

    const std::size_t linksBefore = explicitLinks.size();
    zzCell cell = readCell(cellNode, explicitLinks, diagnostics);

    if (doc.cells.contains(cell.id)) {
      diagnostics.warn(
          std::format("duplicate cell id {} -- keeping later", cell.id));
      const auto purgeEnd =
          explicitLinks.begin() + static_cast<std::ptrdiff_t>(linksBefore);
      const auto kept = std::remove_if(
          explicitLinks.begin(), purgeEnd,
          [&](const ExplicitLink &link) { return link.from == cell.id; });
      explicitLinks.erase(kept, purgeEnd);
    }

    doc.cells[cell.id] = std::move(cell);
  }

  if (doc.cells.empty()) {
    reportDiagnostics(diagnostics, origin);
    return fail(LoadError::Kind::SchemaViolation, "produced no valid cells",
                origin);
  }

  zzcore::deriveBacklinks(doc.cells, explicitLinks, diagnostics);

  if (!structure.has_child("focus") || !structure["focus"].has_val()) {
    reportDiagnostics(diagnostics, origin);
    return fail(LoadError::Kind::SchemaViolation, "does not specify 'focus'",
                origin);
  }
  structure["focus"] >> doc.focus;
  if (!doc.cells.contains(doc.focus)) {
    reportDiagnostics(diagnostics, origin);
    return fail(
        LoadError::Kind::DanglingFocus,
        std::format("focus cell {} is not among defined cells", doc.focus),
        origin);
  }

  if (structure.has_child("view") && structure["view"].is_map()) {
    const ryml::ConstNodeRef viewNode = structure["view"];
    if (viewNode.has_child("x_dimension") &&
        viewNode["x_dimension"].has_val()) {
      viewNode["x_dimension"] >> doc.view.x_dimension;
    }
    if (viewNode.has_child("y_dimension") &&
        viewNode["y_dimension"].has_val()) {
      viewNode["y_dimension"] >> doc.view.y_dimension;
    }
    if (viewNode.has_child("z_dimension") &&
        viewNode["z_dimension"].has_val()) {
      viewNode["z_dimension"] >> doc.view.z_dimension;
    }
  }

  readDimensionMeta(structure, doc, diagnostics);
  readSceneMeta(structure, doc, diagnostics);

  zzcore::neutralizeDanglingLinks(doc.cells, diagnostics);
  zzcore::resolveAllPreflets(doc.cells, diagnostics);

  reportDiagnostics(diagnostics, origin);
  return doc;
}

} // namespace

std::expected<ZzStructureDocument, LoadError>
parseZzStructure(const std::string &yamlText, const std::string &originLabel) {
  try {
    ryml::Tree tree = ryml::parse_in_arena(ryml::to_csubstr(yamlText));
    return buildDocument(tree.rootref(), originLabel);
  } catch (const std::exception &err) {
    return fail(LoadError::Kind::MalformedYaml, err.what(), originLabel);
  } catch (...) {
    return fail(LoadError::Kind::MalformedYaml, "unknown parse error",
                originLabel);
  }
}

std::expected<ZzStructureDocument, LoadError>
loadZzStructure(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return fail(LoadError::Kind::FileUnreadable, "could not open " + path,
                path);
  }
  std::ostringstream ss;
  ss << file.rdbuf();
  return parseZzStructure(ss.str(), path);
}

std::string serializeZzStructure(const ZzStructureDocument &doc) {
  std::ostringstream ss;
  ss << "zzstructure:\n";
  ss << "  meta:\n";
  ss << "    name: \""
     << (doc.meta.name.empty() ? "ZigZag Document" : doc.meta.name) << "\"\n";
  if (!doc.meta.description.empty()) {
    ss << "    description: \"" << doc.meta.description << "\"\n";
  }
  if (!doc.meta.author.empty()) {
    ss << "    author: \"" << doc.meta.author << "\"\n";
  }
  ss << "    version: \""
     << (doc.meta.version.empty() ? "1.0" : doc.meta.version) << "\"\n";

  ss << "  focus: " << (doc.focus == 0 ? 1 : doc.focus) << "\n\n";

  ss << "  view:\n";
  ss << "    x_dimension: "
     << (doc.view.x_dimension.empty() ? "d.1" : doc.view.x_dimension) << "\n";
  ss << "    y_dimension: "
     << (doc.view.y_dimension.empty() ? "d.2" : doc.view.y_dimension) << "\n";
  ss << "    z_dimension: "
     << (doc.view.z_dimension.empty() ? "d.3" : doc.view.z_dimension) << "\n\n";

  if (!doc.dimension_meta.empty()) {
    ss << "  dimensions:\n";
    for (const auto &[dim, meta] : doc.dimension_meta) {
      ss << "    " << dim << ":\n";
      if (!meta.label.empty()) {
        ss << "      label: \"" << meta.label << "\"\n";
      }
      if (!meta.description.empty()) {
        ss << "      description: \"" << meta.description << "\"\n";
      }
      ss << "      color: \"#"
         << std::format("{:02x}{:02x}{:02x}",
                        static_cast<int>(meta.color.r * 255.0F),
                        static_cast<int>(meta.color.g * 255.0F),
                        static_cast<int>(meta.color.b * 255.0F))
         << "\"\n";
      if (meta.spacing > 0.0F) {
        ss << "      spacing: " << meta.spacing << "\n";
      }
    }
    ss << "\n";
  }

  ss << "  cells:\n";
  for (const auto &[id, cell] : doc.cells) {
    ss << "    - id: " << id << "\n";
    ss << "      text: \"" << cell.text_data << "\"\n";
    if (!cell.type.empty() && cell.type != "text") {
      ss << "      type: " << cell.type << "\n";
    }
    if (!cell.dimensions.empty()) {
      ss << "      dimensions: {";
      bool first = true;
      for (const auto &[dim, links] : cell.dimensions) {
        if (!first) ss << ", ";
        first = false;
        if (links.pos != 0 && links.neg != 0) {
          ss << dim << ": { pos: " << links.pos << ", neg: " << links.neg
             << " }";
        } else if (links.pos != 0) {
          ss << dim << ": { pos: " << links.pos << " }";
        } else if (links.neg != 0) {
          ss << dim << ": { neg: " << links.neg << " }";
        }
      }
      ss << "}\n";
    }
    if (cell.preflet) {
      ss << "      preflet:\n";
      ss << "        resource_identifier: \""
         << cell.preflet->resource_identifier << "\"\n";
      if (cell.preflet->target_cell_id != 0) {
        ss << "        target_cell_id: " << cell.preflet->target_cell_id
           << "\n";
      }
      if (!cell.preflet->version.empty()) {
        ss << "        version: \"" << cell.preflet->version << "\"\n";
      }
    }
    ss << "\n";
  }

  return ss.str();
}

bool saveZzStructure(const ZzStructureDocument &doc, const std::string &path) {
  std::ofstream out(path, std::ios::trunc);
  if (!out.is_open()) {
    return false;
  }
  out << serializeZzStructure(doc);
  return out.good();
}

} // namespace zigzag
