#include "common/xanadu/zigzag/zzcore.hpp"

#include <common/xanadu/torrent.hpp>

#include <gleditor/color.hpp>
#include <gleditor/paths.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <type_traits>
#include <unordered_set>
#include <variant>

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

std::optional<RgbColor> parseHexColor(const std::string_view text) {
  return gleditor::color::parseHexColor(text);
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

void deriveBacklinks(std::unordered_map<CellID, Cell> &cells,
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

void neutralizeDanglingLinks(std::unordered_map<CellID, Cell> &cells,
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

std::array<CellID, 6> axisNeighbours(const Cell *cell,
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

const Cell *findCell(const std::unordered_map<CellID, Cell> &cells,
                     const CellID id) {
  const auto it = cells.find(id);
  return it != cells.end() ? &it->second : nullptr;
}

LinkPairs linksOn(const Cell *cell, const std::string_view dimension) {
  if (!cell) {
    return LinkPairs{};
  }
  const auto it = cell->dimensions.find(std::string{dimension});
  return it != cell->dimensions.end() ? it->second : LinkPairs{};
}

CellID findCloneMaster(const std::unordered_map<CellID, Cell> &cells,
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

bool isCloneCell(const std::unordered_map<CellID, Cell> &cells,
                 const CellID id) {
  const auto *cell = findCell(cells, id);
  if (!cell) {
    return false;
  }
  const CellID neg = linksOn(cell, cloneDimension).neg;
  return neg != 0 && cells.contains(neg);
}

std::string cellDataAsText(const CellData &data) {
  return std::visit(
      [](const auto &held) -> std::string {
        using Held = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<Held, std::string>) {
          return held;
        } else if constexpr (std::is_same_v<Held, double>) {
          // Shortest form that reads back as the same double, so that a cell
          // written as a number and read as text and parsed again is the
          // number it started as.
          std::array<char, 32> buffer{};
          const auto [end, ec] =
              std::to_chars(buffer.data(), buffer.data() + buffer.size(), held);
          return std::errc{} == ec ? std::string(buffer.data(), end)
                                   : std::string{};
        } else if constexpr (std::is_same_v<Held, bool>) {
          return held ? "true" : "false";
        } else {
          return {}; // a blob is not text; ask Cell::blob() for it
        }
      },
      data);
}

std::string getEffectiveCellText(const std::unordered_map<CellID, Cell> &cells,
                                 const CellID id) {
  // A clone shows its master's content, and it shows it whatever alternative
  // that content is. Asking through text() made a master holding a double
  // indistinguishable from a master holding an empty string, so a clone of a
  // number fell back to whatever text the clone itself happened to carry --
  // which is the one thing a clone must never do.
  const auto *const master = findCell(cells, findCloneMaster(cells, id));
  if (nullptr != master) {
    return cellDataAsText(master->data);
  }
  // Only reachable when the d.clone rank names a master this space does not
  // hold. The cell's own content is the best answer available.
  const auto *const cell = findCell(cells, id);
  return nullptr != cell ? cellDataAsText(cell->data) : std::string{};
}

std::vector<CellID> getCloneRank(const std::unordered_map<CellID, Cell> &cells,
                                 const CellID id) {
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

void updateMasterText(std::unordered_map<CellID, Cell> &cells, const CellID id,
                      std::string newText) {
  const CellID masterId = findCloneMaster(cells, id);
  if (auto it = cells.find(masterId); it != cells.end()) {
    it->second.data = std::move(newText);
  }
}

} // namespace zigzag::zzcore
