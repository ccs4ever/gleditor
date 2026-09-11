/**
 * @file zzstructure.hpp
 * @brief Shared Xanadu ZigZag structural data model.
 *
 * A "Slice" is one YAML file following this schema -- a self-contained set of
 * zzcells. A user's default Slice (the "Home Slice") lives at a standard
 * per-user config location.
 *
 * A slice used to reference cells in other slices through a Preflet: a magnet
 * URI, a hash, a version string and a target cell id, carried by the cell
 * doing the referring. That is deleted -- see design R13. Every field of it is
 * subsumed by something that already exists and does the job better, and the
 * locator was a layering violation besides: a transport address duplicated
 * into the document graph once per referring cell, correctable in no one
 * place when a swarm moves.
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
#include <variant>
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

/// A cell's content: text, a number, a flag, or an inline binary payload.
/// Which alternative is live is exactly the information `type`/`mime_type`
/// used to carry redundantly for the text-vs-blob distinction -- media
/// *kind* now lives entirely in `mime_type`, resolved via libmagic.
using CellData =
    std::variant<std::string, double, bool, std::vector<std::uint8_t>>;

/// A cell's representation in an authored YAML Slice document (DTO).
/// In-memory runtime cell topology is managed by Manifold and backed by
/// xanadu::Store operations; Cell serves as the serialization interchange
/// format for YAML loading, dumping, and testing.
struct Cell {
  CellID id = 0;
  CellData data;

  // Structural role within an authored schema (e.g. "schema_field",
  // "config_group") -- distinct from mime_type, which is
  // strictly the resolved media kind of `data`/`media_path`. Empty for
  // ordinary content cells.
  std::string role;

  std::string
      mime_type; // MIME type e.g. "image/png", "image/jpeg", "text/plain"
  std::string media_path; // Relative or absolute file path or URI
  std::unordered_map<DimID, LinkPairs> dimensions;

  /// The cell's content as text, or an empty view if `data` holds something
  /// else. Never throws: use `std::get<std::string>(data)` directly at call
  /// sites that already know the alternative is live.
  [[nodiscard]] std::string_view text() const noexcept {
    if (const auto *s = std::get_if<std::string>(&data)) {
      return *s;
    }
    return {};
  }

  /// The cell's content as a binary blob, or nullptr if `data` holds
  /// something else.
  [[nodiscard]] const std::vector<std::uint8_t> *blob() const noexcept {
    return std::get_if<std::vector<std::uint8_t>>(&data);
  }

  [[nodiscard]] bool isMedia() const {
    return (!mime_type.empty() && !mime_type.starts_with("text/")) ||
           !media_path.empty() || (blob() != nullptr && !blob()->empty());
  }

  [[nodiscard]] bool isImage() const {
    return mime_type.starts_with("image/") ||
           (!media_path.empty() &&
            (media_path.ends_with(".png") || media_path.ends_with(".jpg") ||
             media_path.ends_with(".jpeg") || media_path.ends_with(".webp") ||
             media_path.ends_with(".gif") || media_path.ends_with(".svg") ||
             media_path.ends_with(".bmp")));
  }
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
  float focus_scale       = 1.4F;
  float cell_radius       = 0.35F;
  float layout_speed      = 12.0F;
  float alpha_speed       = 8.0F;
  float border_thickness  = 2.0F;
  int neighborhood_radius = 3;
};

struct StructureMeta {
  std::string name;
  std::string description;
  std::string version;
  std::string author;
  std::string created;
  std::vector<std::string> tags;
};

/// An authored YAML Slice document (DTO). In-memory runtime state is
/// managed by xanadu::Store and zigzag::Manifold (via
/// UnifiedTransclusionEngine), while ZzStructureDocument serves as the
/// serialization/deserialization interchange schema for YAML slices.
struct ZzStructureDocument {
  StructureMeta meta;
  CellID focus = 0;
  ViewAxisBinding view;
  std::unordered_map<DimID, DimensionMeta> dimension_meta;
  SceneMeta scene;
  std::unordered_map<CellID, Cell> cells;
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
