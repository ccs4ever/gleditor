/**
 * @file zzcore.hpp
 * @brief Pure Xanadu ZigZag logic: clone-master resolution and cell content
 *        rendering over the ZzStructureDocument cell map.
 */
#ifndef ZIGZAG_ZZCORE_HPP
#define ZIGZAG_ZZCORE_HPP

#include "common/xanadu/zigzag/zzstructure.hpp"

#include <cstdint>
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
    entries_.push_back(
        {.severity = Severity::Warning, .message = std::move(message)});
  }
  void error(std::string message) {
    entries_.push_back(
        {.severity = Severity::Error, .message = std::move(message)});
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

} // namespace zigzag::zzcore

#endif // ZIGZAG_ZZCORE_HPP
