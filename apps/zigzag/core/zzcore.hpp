/**
 * @file zzcore.hpp
 * @brief Pure Xanadu ZigZag logic: link derivation, referential integrity,
 *        Preflet chain resolution, and parsing helpers.
 */
#ifndef ZIGZAG_ZZCORE_HPP
#define ZIGZAG_ZZCORE_HPP

#include "zzstructure.hpp"

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

inline constexpr std::string_view prefletDimension  = "d.preflet";
inline constexpr std::string_view prefletTypePrefix = "preflet_";

[[nodiscard]] bool isPrefletChainNode(std::string_view type);
[[nodiscard]] bool looksLikeBitTorrentMagnet(std::string_view identifier);
[[nodiscard]] std::optional<RgbColor> parseHexColor(std::string_view text);
[[nodiscard]] std::pair<std::string, std::string>
splitMetadataEntry(std::string_view text);

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

void deriveBacklinks(std::unordered_map<CellID, zzCell> &cells,
                     const std::vector<ExplicitLink> &explicitLinks,
                     Diagnostics &diagnostics);

void neutralizeDanglingLinks(std::unordered_map<CellID, zzCell> &cells,
                             Diagnostics &diagnostics);

[[nodiscard]] std::optional<Preflet>
resolvePreflet(CellID startId, const std::unordered_map<CellID, zzCell> &cells,
               CellID hostId, Diagnostics &diagnostics);

void resolveAllPreflets(std::unordered_map<CellID, zzCell> &cells,
                        Diagnostics &diagnostics);

/// Returns the 6 axis neighbours: [x+, x-, y+, y-, z+, z-] (0 for absent).
[[nodiscard]] std::array<CellID, 6> axisNeighbours(const zzCell *cell,
                                                   const ViewAxisBinding &view);

[[nodiscard]] const zzCell *
findCell(const std::unordered_map<CellID, zzCell> &cells, CellID id);

[[nodiscard]] LinkPairs linksOn(const zzCell *cell, std::string_view dimension);

} // namespace zigzag::zzcore

#endif // ZIGZAG_ZZCORE_HPP
