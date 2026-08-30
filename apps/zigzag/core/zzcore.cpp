#include "zzcore.hpp"

#include <xudu/core/torrent.hpp>

#include <gleditor/color.hpp>
#include <gleditor/paths.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <unordered_set>

namespace zigzag {

std::string_view describe(const LoadError::Kind kind) {
  switch (kind) {
  case LoadError::Kind::FileUnreadable:
    return "file unreadable";
  case LoadError::Kind::MalformedYaml:
    return "malformed YAML";
  case LoadError::Kind::SchemaViolation:
    return "schema violation";
  case LoadError::Kind::DanglingFocus:
    return "focus cell not defined";
  }
  return "unknown";
}

} // namespace zigzag

namespace zigzag::zzcore {

std::size_t Diagnostics::count(const Severity severity) const {
  return static_cast<std::size_t>(std::count_if(
      entries_.begin(), entries_.end(),
      [severity](const auto &e) { return e.severity == severity; }));
}

bool Diagnostics::mentions(const std::string_view needle) const {
  return std::ranges::any_of(entries_, [needle](const auto &entry) {
    return entry.message.find(needle) != std::string::npos;
  });
}

bool isPrefletChainNode(const std::string_view type) {
  return type.starts_with(prefletTypePrefix);
}

bool looksLikeBitTorrentMagnet(const std::string_view identifier) {
  try {
    static_cast<void>(xudu::MagnetLink::parse(identifier));
    return true;
  } catch (...) {
    return false;
  }
}

std::optional<RgbColor> parseHexColor(const std::string_view text) {
  return gleditor::color::parseHexColor(text);
}

std::pair<std::string, std::string>
splitMetadataEntry(const std::string_view text) {
  const auto colon = text.find(':');
  if (colon == std::string_view::npos) {
    return {std::string{text}, std::string{}};
  }

  std::string_view value = text.substr(colon + 1);
  while (!value.empty() && value.front() == ' ') {
    value.remove_prefix(1);
  }
  return {std::string{text.substr(0, colon)}, std::string{value}};
}

std::string resolveXdgPath(const char *const xdgValue,
                           const char *const homeValue,
                           const std::string_view homeRelativeDir,
                           const std::string_view leaf) {
  if (nullptr != xdgValue && xdgValue[0] != '\0') {
    return std::format("{}/{}", xdgValue, leaf);
  }
  if (nullptr != homeValue && homeValue[0] != '\0') {
    return std::format("{}/{}/{}", homeValue, homeRelativeDir, leaf);
  }
  return {};
}

namespace {

bool hasSuffixIgnoringCase(const std::string_view text,
                           const std::string_view suffix) {
  if (text.size() < suffix.size()) {
    return false;
  }
  return std::equal(suffix.rbegin(), suffix.rend(), text.rbegin(),
                    [](const char a, const char b) {
                      return std::tolower(static_cast<unsigned char>(a)) ==
                             std::tolower(static_cast<unsigned char>(b));
                    });
}

} // namespace

std::string selectSliceFile(const std::vector<std::string> &paths,
                            const std::string_view preferred) {
  if (!preferred.empty()) {
    for (const std::string &path : paths) {
      if (hasSuffixIgnoringCase(path, preferred)) {
        return path;
      }
    }
  }
  for (const std::string &path : paths) {
    if (hasSuffixIgnoringCase(path, ".yaml") ||
        hasSuffixIgnoringCase(path, ".yml")) {
      return path;
    }
  }
  return {};
}

void deriveBacklinks(std::unordered_map<CellID, zzCell> &cells,
                     const std::vector<ExplicitLink> &explicitLinks,
                     Diagnostics &diagnostics) {
  for (const ExplicitLink &link : explicitLinks) {
    const auto neighbor = cells.find(link.target);
    if (neighbor == cells.end()) {
      continue; // Dangling; neutralized in neutralizeDanglingLinks
    }

    LinkPairs &neighborLinks = neighbor->second.dimensions[link.dimension];
    CellID &backReference = link.isPos ? neighborLinks.neg : neighborLinks.pos;

    if (backReference == 0) {
      backReference = link.from;
    } else if (backReference != link.from) {
      diagnostics.warn(std::format(
          "cell {} dimension {} {} -> {}, but {}'s {} already points at {} -- "
          "leaving both as declared",
          link.from, link.dimension, link.isPos ? "pos" : "neg", link.target,
          link.target, link.isPos ? "neg" : "pos", backReference));
    }
  }
}

void neutralizeDanglingLinks(std::unordered_map<CellID, zzCell> &cells,
                             Diagnostics &diagnostics) {
  for (auto &[id, cell] : cells) {
    for (auto &[dimensionName, links] : cell.dimensions) {
      const auto clamp = [&](CellID &target, const char *direction) {
        if (target != 0 && !cells.contains(target)) {
          diagnostics.warn(
              std::format("cell {} dimension {} {} link {} does not exist -- "
                          "treating as unlinked",
                          id, dimensionName, direction, target));
          target = 0;
        }
      };
      clamp(links.pos, "pos");
      clamp(links.neg, "neg");
    }
  }
}

std::optional<Preflet>
resolvePreflet(const CellID startId,
               const std::unordered_map<CellID, zzCell> &cells,
               const CellID hostId, Diagnostics &diagnostics) {
  Preflet result;
  bool haveResource = false;

  std::unordered_set<CellID> visited;
  constexpr int maxChainLength = 32;

  CellID current = startId;
  for (int hop = 0; hop < maxChainLength && current != 0; ++hop) {
    if (!visited.insert(current).second) {
      diagnostics.warn(std::format("cell {}'s d.preflet chain loops back on "
                                   "cell {} -- stopping there",
                                   hostId, current));
      break;
    }

    const zzCell *cell = findCell(cells, current);
    if (!cell) {
      break; // Dangling; neutralized in neutralizeDanglingLinks
    }

    if (cell->type == "preflet_resource") {
      result.resource_identifier = cell->text_data;
      haveResource               = true;
      if (!looksLikeBitTorrentMagnet(cell->text_data)) {
        diagnostics.warn(std::format("cell {}'s preflet resource identifier "
                                     "doesn't look like a magnet:?xt=urn:btih: "
                                     "link -- {}",
                                     hostId, cell->text_data));
      }
    } else if (cell->type == "preflet_hash") {
      result.hash = cell->text_data;
    } else if (cell->type == "preflet_version") {
      result.version = cell->text_data;
    } else if (cell->type == "preflet_cell_id") {
      try {
        result.target_cell_id =
            static_cast<CellID>(std::stoull(cell->text_data));
      } catch (...) {
        diagnostics.warn(std::format("cell {}'s preflet cell id {} is not a "
                                     "number -- ignoring",
                                     hostId, cell->text_data));
      }
    } else if (cell->type == "preflet_meta") {
      result.metadata.push_back(splitMetadataEntry(cell->text_data));
    } else {
      diagnostics.warn(std::format("cell {} is in cell {}'s d.preflet chain "
                                   "but has an unrecognized type {} -- "
                                   "ignoring it",
                                   current, hostId,
                                   cell->type.empty() ? "(none)" : cell->type));
    }

    current = linksOn(cell, prefletDimension).pos;
  }

  if (!haveResource) {
    diagnostics.warn(std::format("cell {} has a d.preflet link but its chain "
                                 "has no preflet_resource cell -- ignoring",
                                 hostId));
    return std::nullopt;
  }

  return result;
}

void resolveAllPreflets(std::unordered_map<CellID, zzCell> &cells,
                        Diagnostics &diagnostics) {
  std::vector<std::pair<CellID, CellID>> hosts; // {hostId, chainStart}
  for (const auto &[id, cell] : cells) {
    if (isPrefletChainNode(cell.type)) {
      continue;
    }
    const CellID start = linksOn(&cell, prefletDimension).pos;
    if (start != 0) {
      hosts.emplace_back(id, start);
    }
  }

  std::ranges::sort(hosts);

  for (const auto &[hostId, startId] : hosts) {
    if (auto resolved = resolvePreflet(startId, cells, hostId, diagnostics)) {
      cells[hostId].preflet = std::move(*resolved);
    }
  }
}

std::array<CellID, 6> axisNeighbours(const zzCell *cell,
                                     const ViewAxisBinding &view) {
  std::array<CellID, 6> neighbours{};
  const std::array<const DimID *, 3> dims = {
      &view.x_dimension, &view.y_dimension, &view.z_dimension};
  std::size_t idx = 0;
  for (const DimID *dimension : dims) {
    const LinkPairs links = linksOn(cell, *dimension);
    neighbours[idx++]     = links.pos;
    neighbours[idx++]     = links.neg;
  }
  return neighbours;
}

const zzCell *findCell(const std::unordered_map<CellID, zzCell> &cells,
                       const CellID id) {
  const auto it = cells.find(id);
  return it != cells.end() ? &it->second : nullptr;
}

LinkPairs linksOn(const zzCell *cell, const std::string_view dimension) {
  if (!cell) {
    return LinkPairs{};
  }
  const auto it = cell->dimensions.find(std::string{dimension});
  return it != cell->dimensions.end() ? it->second : LinkPairs{};
}

CellID findCloneMaster(const std::unordered_map<CellID, zzCell> &cells,
                       const CellID id) {
  if (0 == id) {
    return 0;
  }
  std::unordered_set<CellID> visited;
  CellID current = id;
  while (current != 0 && visited.insert(current).second) {
    const auto *cell = findCell(cells, current);
    if (!cell) {
      break;
    }
    const CellID neg = linksOn(cell, cloneDimension).neg;
    if (0 == neg || !cells.contains(neg)) {
      return current;
    }
    current = neg;
  }
  return current != 0 ? current : id;
}

bool isCloneCell(const std::unordered_map<CellID, zzCell> &cells,
                 const CellID id) {
  const auto *cell = findCell(cells, id);
  if (!cell) {
    return false;
  }
  const CellID neg = linksOn(cell, cloneDimension).neg;
  return neg != 0 && cells.contains(neg);
}

std::string_view
getEffectiveCellText(const std::unordered_map<CellID, zzCell> &cells,
                     const CellID id) {
  const CellID masterId = findCloneMaster(cells, id);
  const auto *master    = findCell(cells, masterId);
  if (master && !master->text_data.empty()) {
    return master->text_data;
  }
  const auto *cell = findCell(cells, id);
  return cell ? std::string_view{cell->text_data} : std::string_view{};
}

std::vector<CellID>
getCloneRank(const std::unordered_map<CellID, zzCell> &cells, const CellID id) {
  std::vector<CellID> rank;
  const CellID masterId = findCloneMaster(cells, id);
  if (0 == masterId) {
    return rank;
  }
  std::unordered_set<CellID> visited;
  CellID current = masterId;
  while (current != 0 && visited.insert(current).second) {
    rank.push_back(current);
    const auto *cell = findCell(cells, current);
    if (!cell) {
      break;
    }
    const CellID pos = linksOn(cell, cloneDimension).pos;
    if (0 == pos || !cells.contains(pos)) {
      break;
    }
    current = pos;
  }
  return rank;
}

void updateMasterText(std::unordered_map<CellID, zzCell> &cells,
                      const CellID id, std::string newText) {
  const CellID masterId = findCloneMaster(cells, id);
  if (auto it = cells.find(masterId); it != cells.end()) {
    it->second.text_data = std::move(newText);
  }
}

} // namespace zigzag::zzcore
