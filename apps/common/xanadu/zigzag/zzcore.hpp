/**
 * @file zzcore.hpp
 * @brief Pure Xanadu ZigZag logic: link derivation, referential integrity,
 *        and parsing helpers.
 */
#ifndef ZIGZAG_ZZCORE_HPP
#define ZIGZAG_ZZCORE_HPP

#include "common/xanadu/zigzag/zzstructure.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zigzag::zzcore {

enum class Severity { Warning, Error };

struct Diagnostic {
  Severity severity = Severity::Warning;
  std::string message;
};

/// Collects diagnostics without directly printing them.
class Diagnostics {
public:
  void warn(std::string message) {
    entries_.push_back({Severity::Warning, std::move(message)});
  }
  void error(std::string message) {
    entries_.push_back({Severity::Error, std::move(message)});
  }

  [[nodiscard]] const std::vector<Diagnostic> &entries() const {
    return entries_;
  }
  [[nodiscard]] std::size_t count(Severity severity) const;
  [[nodiscard]] bool empty() const { return entries_.empty(); }
  void clear() { entries_.clear(); }

  [[nodiscard]] bool mentions(std::string_view needle) const;

private:
  std::vector<Diagnostic> entries_;
};

inline constexpr std::string_view cloneDimension = "d.clone";

[[nodiscard]] std::optional<RgbColor> parseHexColor(std::string_view text);
[[nodiscard]] std::string resolveXdgPath(const char *xdgValue,
                                         const char *homeValue,
                                         std::string_view homeRelativeDir,
                                         std::string_view leaf);

[[nodiscard]] std::string selectSliceFile(const std::vector<std::string> &paths,
                                          std::string_view preferred);

struct ExplicitLink {
  CellID from = 0;
  DimID dimension;
  bool isPos = true; // true: from's pos is target; false: from's neg is target
  CellID target = 0;
};

void deriveBacklinks(std::unordered_map<CellID, Cell> &cells,
                     const std::vector<ExplicitLink> &explicitLinks,
                     Diagnostics &diagnostics);

void neutralizeDanglingLinks(std::unordered_map<CellID, Cell> &cells,
                             Diagnostics &diagnostics);

/// Returns the 6 axis neighbours: [x+, x-, y+, y-, z+, z-] (0 for absent).
[[nodiscard]] std::array<CellID, 6> axisNeighbours(const Cell *cell,
                                                   const ViewAxisBinding &view);

[[nodiscard]] const Cell *
findCell(const std::unordered_map<CellID, Cell> &cells, CellID id);

[[nodiscard]] LinkPairs linksOn(const Cell *cell, std::string_view dimension);

/**
 * @brief Find the master cell at the head of a cell's d.clone rank.
 *
 * In ZigZag, a d.clone rank links a master cell to all its clones. Following
 * negward links on d.clone leads to the head of the rank, which is the Master
 * Cell. If the cell has no negward d.clone link, it is its own master.
 */
[[nodiscard]] CellID
findCloneMaster(const std::unordered_map<CellID, Cell> &cells, CellID id);

/**
 * @brief Returns true iff this cell is a clone (i.e. has a negward link on
 * d.clone).
 */
[[nodiscard]] bool isCloneCell(const std::unordered_map<CellID, Cell> &cells,
                               CellID id);

/**
 * @brief A cell's content rendered as text, whichever alternative is live.
 *
 * A double comes out in its shortest form that reads back as the same double,
 * a bool as "true" or "false", and a binary blob as nothing at all -- a blob
 * is not text and callers that want one ask Cell::blob() for it. Cell::text()
 * answers only for the string alternative and hands back an empty view for
 * the other three, which reads as "this cell is empty" when what it means is
 * "this cell does not hold a string".
 */
[[nodiscard]] std::string cellDataAsText(const CellData &data);

/**
 * @brief Returns the effective text of a cell by looking up its master cell at
 * the head of its d.clone rank.
 *
 * By value rather than by view because the answer for a scalar cell does not
 * exist anywhere to point at. Every caller copied the view into a string
 * immediately anyway.
 */
[[nodiscard]] std::string
getEffectiveCellText(const std::unordered_map<CellID, Cell> &cells, CellID id);

/**
 * @brief Returns all cells in the d.clone rank containing @p id, starting from
 * the head master cell and walking posward.
 */
[[nodiscard]] std::vector<CellID>
getCloneRank(const std::unordered_map<CellID, Cell> &cells, CellID id);

/**
 * @brief Updates the text on the master cell at the head of @p id's d.clone
 * rank.
 */
void updateMasterText(std::unordered_map<CellID, Cell> &cells, CellID id,
                      std::string newText);

} // namespace zigzag::zzcore

#endif // ZIGZAG_ZZCORE_HPP
