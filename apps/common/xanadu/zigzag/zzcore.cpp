#include "common/xanadu/zigzag/zzcore.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <type_traits>
#include <unordered_set>
#include <variant>

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

common::cpp26::optional<const Cell &>
findCell(const std::unordered_map<CellID, Cell> &cells, const CellID id) {
  const auto it = cells.find(id);
  if (it == cells.end()) {
    return common::cpp26::nullopt;
  }
  return it->second;
}

LinkPairs linksOn(const common::cpp26::optional<const Cell &> cell,
                  const std::string_view dimension) {
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
    const auto cell = findCell(cells, current);
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
  const auto master = findCell(cells, findCloneMaster(cells, id));
  if (master) {
    return cellDataAsText(master->data);
  }
  // Only reachable when the d.clone rank names a master this space does not
  // hold. The cell's own content is the best answer available.
  const auto cell = findCell(cells, id);
  return cell ? cellDataAsText(cell->data) : std::string{};
}

} // namespace zigzag::zzcore
