/**
 * @file pouch_zone.hpp
 * @brief Core data model and backing system xanadoc manager for drop zone
 * pouches.
 */
#ifndef XUDU_CORE_POUCH_ZONE_HPP
#define XUDU_CORE_POUCH_ZONE_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec4.hpp>

#include "microversion.hpp"
#include "ops.hpp"
#include "spool.hpp"
#include "store.hpp"
#include "user_permascroll.hpp"

namespace xanadu {

/**
 * @struct PouchItem
 * @brief An individual transcluded card stored within a drop zone.
 */
struct PouchItem {
  std::uint64_t itemId{0};
  PrimediaSpan span; // 24 bytes: {scrollId, start, length}
  std::string previewText;
  MicroversionId originVersion;
  std::uint32_t originDocIndex{0};
  std::uint32_t originCharStart{0};
  std::uint32_t originCharEnd{0};
  std::uint64_t timestampUtc{0};
};

/**
 * @struct DropZoneConfig
 * @brief User-configurable partition definition stored within the backing
 * xanadoc.
 */
struct DropZoneConfig {
  std::string id;
  std::string label{"Notes"};
  glm::vec4 backgroundColor{0.12F, 0.15F, 0.20F, 0.85F};
  std::uint32_t auraColor{0xFFEAB308U};
  float heightWeight{1.0F};
};

/**
 * @class DropZone
 * @brief A drop partition with customizable geometry, colors, and picking tag.
 */
class DropZone {
public:
  explicit DropZone(DropZoneConfig config);

  [[nodiscard]] const std::string &id() const noexcept { return config_.id; }
  [[nodiscard]] const std::string &label() const noexcept {
    return config_.label;
  }
  void setLabel(std::string label) { config_.label = std::move(label); }

  [[nodiscard]] const glm::vec4 &backgroundColor() const noexcept {
    return config_.backgroundColor;
  }
  void setBackgroundColor(const glm::vec4 &color) noexcept {
    config_.backgroundColor = color;
  }

  [[nodiscard]] std::uint32_t auraColor() const noexcept {
    return config_.auraColor;
  }
  void setAuraColor(const std::uint32_t color) noexcept {
    config_.auraColor = color;
  }

  [[nodiscard]] float heightWeight() const noexcept {
    return config_.heightWeight;
  }
  void setHeightWeight(const float weight) noexcept {
    config_.heightWeight = weight;
  }

  [[nodiscard]] const DropZoneConfig &config() const noexcept {
    return config_;
  }

  void setRect(float x, float y, float width, float height) noexcept;
  [[nodiscard]] bool contains(float screenX, float screenY) const noexcept;

  void addItem(PouchItem item);
  bool removeItem(std::uint64_t itemId);
  void clear();

  [[nodiscard]] const std::vector<PouchItem> &items() const noexcept {
    return items_;
  }
  [[nodiscard]] std::vector<PrimediaSpan> allSpans() const;

  void setHovered(const bool hovered) noexcept { isHovered_ = hovered; }
  [[nodiscard]] bool isHovered() const noexcept { return isHovered_; }

  void setTagOffset(const std::uint32_t offset) noexcept {
    tagOffset_ = offset;
  }
  [[nodiscard]] std::uint32_t tagOffset() const noexcept { return tagOffset_; }

  [[nodiscard]] float x() const noexcept { return x_; }
  [[nodiscard]] float y() const noexcept { return y_; }
  [[nodiscard]] float width() const noexcept { return width_; }
  [[nodiscard]] float height() const noexcept { return height_; }

private:
  DropZoneConfig config_;
  float x_{0.0F};
  float y_{0.0F};
  float width_{0.0F};
  float height_{0.0F};
  bool isHovered_{false};
  std::uint32_t tagOffset_{0};
  std::vector<PouchItem> items_;
};

/**
 * @class PouchManager
 * @brief Manages drop zones backed by an underlying system Store.
 */
class PouchManager {
public:
  explicit PouchManager(std::shared_ptr<UserPermascroll> permascroll = nullptr);

  DropZone &addZone(DropZoneConfig config);
  bool removeZone(std::string_view id);
  [[nodiscard]] DropZone *zoneById(std::string_view id) noexcept;
  [[nodiscard]] const DropZone *zoneById(std::string_view id) const noexcept;
  [[nodiscard]] DropZone *zoneAt(float screenX, float screenY) noexcept;
  [[nodiscard]] const std::vector<std::unique_ptr<DropZone>> &
  zones() const noexcept {
    return zones_;
  }

  /// Populate with default Nelsonian partitions.
  void initDefaultZones();

  /// Drop a span into a zone, recording OpKind::Transclude in the backing
  /// store.
  PouchItem dropSpan(std::string_view zoneId, const PrimediaSpan &span,
                     std::string previewText, const MicroversionId &sourceVer,
                     std::uint32_t docIndex = 0, std::uint32_t charStart = 0,
                     std::uint32_t charEnd = 0);

  /// Dismiss an item, moving it to non-destructive limbo in the backing store.
  bool dismissItem(std::uint64_t itemId);

  [[nodiscard]] Store &store() noexcept { return *store_; }
  [[nodiscard]] const Store &store() const noexcept { return *store_; }
  [[nodiscard]] const MicroversionId &currentVersion() const noexcept {
    return currentVersion_;
  }

  /// Serialization of drop zone manifest into root microversion metadata.
  void saveManifest();
  void loadManifest();

private:
  std::unique_ptr<Store> store_;
  MicroversionId currentVersion_;
  std::vector<std::unique_ptr<DropZone>> zones_;
  std::uint64_t nextItemId_{1};
};

} // namespace xanadu

#endif // XUDU_CORE_POUCH_ZONE_HPP
