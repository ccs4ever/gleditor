/**
 * @file zzstructure.hpp
 * @brief Shared Xanadu ZigZag structural data model.
 *
 * A "Slice" is one YAML file following this schema -- a self-contained set of
 * zzcells. A user's default Slice (the "Home Slice") lives at a standard
 * per-user config location. Slices can reference cells in other Slices via
 * Preflets.
 */
#ifndef ZIGZAG_ZZSTRUCTURE_HPP
#define ZIGZAG_ZZSTRUCTURE_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gleditor/color.hpp>

namespace zigzag {

using CellID = std::uint64_t;
using DimID  = std::string;

struct LinkPairs {
  CellID pos = 0; // Direction +1 link
  CellID neg = 0; // Direction -1 link

  bool operator==(const LinkPairs &) const = default;
};

/**
 * @brief A resolved Preflet: a lazy, cross-Slice connection from a cell in this
 *        Slice to a cell in some other (target) Slice.
 */
struct Preflet {
  std::string resource_identifier; // Required: BitTorrent magnet URI
  std::string hash;                // Optional: content hash
  std::string version;             // Optional: version string
  CellID target_cell_id = 0;       // Optional: target cell id; 0 = unspecified
  std::vector<std::pair<std::string, std::string>> metadata; // Free-form pairs

  bool operator==(const Preflet &) const = default;
};

struct zzCell {
  CellID id = 0;
  std::string text_data;
  std::string type; // Category e.g. "chapter", "detail", "note"
  std::unordered_map<DimID, LinkPairs> dimensions;
  std::optional<Preflet> preflet;
};

struct ViewAxisBinding {
  DimID x_dimension = "d.1";
  DimID y_dimension = "d.2";
  DimID z_dimension = "d.3";

  bool operator==(const ViewAxisBinding &) const = default;
};

/// Plain RGB colour in [0, 1].
using RgbColor = gleditor::color::Color3;

/// Per-dimension display metadata: rank label and cell styling.
struct DimensionMeta {
  std::string label;
  std::string description;
  RgbColor color;
  float spacing = 2.0F; // World-space distance to neighboring cell
};

/// Global scene appearance.
struct SceneMeta {
  RgbColor background{0.05F, 0.05F, 0.07F};
  RgbColor focus_color{0.956F, 0.773F, 0.259F};
  float focus_scale = 1.4F;
  float cell_radius = 0.35F;
};

struct StructureMeta {
  std::string name;
  std::string description;
  std::string version;
  std::string author;
  std::string created;
  std::vector<std::string> tags;
};

/// A fully-parsed Slice: cell space, focus/view state, and display metadata.
struct ZzStructureDocument {
  StructureMeta meta;
  CellID focus = 0;
  ViewAxisBinding view;
  std::unordered_map<DimID, DimensionMeta> dimension_meta;
  SceneMeta scene;
  std::unordered_map<CellID, zzCell> cells;
};

/// Why a Slice load failed.
struct LoadError {
  enum class Kind {
    FileUnreadable,  // Missing, permissions, not a file
    MalformedYaml,   // YAML parser syntax error
    SchemaViolation, // Parsed, but not a valid zzstructure
    DanglingFocus    // Focus names an undefined cell
  };

  Kind kind = Kind::SchemaViolation;
  std::string message;
  std::string path;
};

[[nodiscard]] std::string_view describe(LoadError::Kind kind);

} // namespace zigzag

#endif // ZIGZAG_ZZSTRUCTURE_HPP
