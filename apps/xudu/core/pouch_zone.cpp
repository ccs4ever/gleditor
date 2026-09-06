/**
 * @file pouch_zone.cpp
 * @brief Core data model and backing system xanadoc manager for drop zone pouches.
 */
#include "pouch_zone.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>

namespace xudu {

DropZone::DropZone(DropZoneConfig config) : config_(std::move(config)) {}

void DropZone::setRect(const float x, const float y, const float width,
                       const float height) noexcept {
  x_      = x;
  y_      = y;
  width_  = width;
  height_ = height;
}

bool DropZone::contains(const float screenX, const float screenY) const noexcept {
  if (width_ <= 0.0F || height_ <= 0.0F) {
    return false;
  }
  return screenX >= x_ && screenX <= (x_ + width_) && screenY >= y_ &&
         screenY <= (y_ + height_);
}

void DropZone::addItem(PouchItem item) {
  items_.push_back(std::move(item));
}

bool DropZone::removeItem(const std::uint64_t itemId) {
  const auto it = std::find_if(items_.begin(), items_.end(),
                               [itemId](const PouchItem &item) {
                                 return item.itemId == itemId;
                               });
  if (it != items_.end()) {
    items_.erase(it);
    return true;
  }
  return false;
}

void DropZone::clear() {
  items_.clear();
}

std::vector<PrimediaSpan> DropZone::allSpans() const {
  std::vector<PrimediaSpan> result;
  result.reserve(items_.size());
  for (const auto &item : items_) {
    result.push_back(item.span);
  }
  return result;
}

// ---------------------------------------------------------------------------
// PouchManager
// ---------------------------------------------------------------------------

PouchManager::PouchManager(std::shared_ptr<UserPermascroll> permascroll) {
  if (permascroll) {
    store_ = std::make_unique<Store>(std::move(permascroll));
  } else {
    store_ = std::make_unique<Store>();
  }
  currentVersion_ = MicroversionId{};
  initDefaultZones();
}

void PouchManager::initDefaultZones() {
  zones_.clear();

  // 1. To Link (Left) - Cyan
  addZone(DropZoneConfig{
      .id              = "to_link_left",
      .label           = "To Link (Left)",
      .backgroundColor = glm::vec4(0.08F, 0.15F, 0.20F, 0.85F),
      .auraColor       = 0x06B6D4FFU,
      .heightWeight    = 1.0F,
  });

  // 2. To Link (Right) - Magenta
  addZone(DropZoneConfig{
      .id              = "to_link_right",
      .label           = "To Link (Right)",
      .backgroundColor = glm::vec4(0.18F, 0.08F, 0.16F, 0.85F),
      .auraColor       = 0xEC4899FFU,
      .heightWeight    = 1.0F,
  });

  // 3. Notes - Identity Gold
  addZone(DropZoneConfig{
      .id              = "notes",
      .label           = "Notes",
      .backgroundColor = glm::vec4(0.18F, 0.15F, 0.08F, 0.85F),
      .auraColor       = 0xEAB308FFU,
      .heightWeight    = 1.2F,
  });

  // 4. Scratch - Emerald
  addZone(DropZoneConfig{
      .id              = "scratch",
      .label           = "Scratch",
      .backgroundColor = glm::vec4(0.08F, 0.18F, 0.12F, 0.85F),
      .auraColor       = 0x10B981FFU,
      .heightWeight    = 1.0F,
  });
}

DropZone &PouchManager::addZone(DropZoneConfig config) {
  auto zone = std::make_unique<DropZone>(std::move(config));
  auto &ref = *zone;
  zones_.push_back(std::move(zone));
  return ref;
}

bool PouchManager::removeZone(const std::string_view id) {
  const auto it =
      std::find_if(zones_.begin(), zones_.end(),
                   [id](const auto &z) { return z->id() == id; });
  if (it != zones_.end()) {
    zones_.erase(it);
    return true;
  }
  return false;
}

DropZone *PouchManager::zoneById(const std::string_view id) noexcept {
  const auto it =
      std::find_if(zones_.begin(), zones_.end(),
                   [id](const auto &z) { return z->id() == id; });
  return (it != zones_.end()) ? it->get() : nullptr;
}

const DropZone *
PouchManager::zoneById(const std::string_view id) const noexcept {
  const auto it =
      std::find_if(zones_.begin(), zones_.end(),
                   [id](const auto &z) { return z->id() == id; });
  return (it != zones_.end()) ? it->get() : nullptr;
}

DropZone *PouchManager::zoneAt(const float screenX,
                               const float screenY) noexcept {
  for (const auto &zone : zones_) {
    if (zone->contains(screenX, screenY)) {
      return zone.get();
    }
  }
  return nullptr;
}

PouchItem PouchManager::dropSpan(const std::string_view zoneId,
                                 const PrimediaSpan &span,
                                 std::string previewText,
                                 const MicroversionId &sourceVer,
                                 const std::uint32_t docIndex,
                                 const std::uint32_t charStart,
                                 const std::uint32_t charEnd) {
  DropZone *zone = zoneById(zoneId);
  if (!zone) {
    if (!zones_.empty()) {
      zone = zones_.front().get();
    } else {
      initDefaultZones();
      zone = zones_.front().get();
    }
  }

  // Record transclusion into the system store: zero raw byte copying!
  currentVersion_ = store_->insertSpan(currentVersion_, 0, span);

  // Annotate microversion with zone and preview
  store_->setVersionAnnotation(
      currentVersion_,
      VersionAnnotation{
          .alias       = std::string(zone->id()),
          .description = previewText,
          .tag         = "pouch-drop",
          .timestamp   = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count()),
      });

  PouchItem item{
      .itemId          = nextItemId_++,
      .span            = span,
      .previewText     = std::move(previewText),
      .originVersion   = sourceVer,
      .originDocIndex  = docIndex,
      .originCharStart = charStart,
      .originCharEnd   = charEnd,
      .timestampUtc    = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count()),
  };

  zone->addItem(item);
  return item;
}

bool PouchManager::dismissItem(const std::uint64_t itemId) {
  for (const auto &zone : zones_) {
    const auto &items = zone->items();
    const auto it     = std::find_if(
        items.begin(), items.end(),
        [itemId](const PouchItem &item) { return item.itemId == itemId; });
    if (it != items.end()) {
      const auto span = it->span;
      zone->removeItem(itemId);
      // Non-destructive limbo: record erase in backing store
      if (span.length > 0) {
        currentVersion_ = store_->erase(currentVersion_, 0, span.length);
      }
      return true;
    }
  }
  return false;
}

void PouchManager::saveManifest() {
  // Store zone definitions as a formatted manifest annotation on root
  std::ostringstream ss;
  for (std::size_t i = 0; i < zones_.size(); ++i) {
    const auto &cfg = zones_[i]->config();
    ss << cfg.id << "|" << cfg.label << "|" << cfg.auraColor << "|"
       << cfg.heightWeight;
    if (i + 1 < zones_.size()) {
      ss << ";";
    }
  }
  store_->setVersionAnnotation(
      MicroversionId{},
      VersionAnnotation{
          .alias       = "pouch-manifest",
          .description = ss.str(),
          .tag         = "manifest",
          .timestamp   = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count()),
      });
}

void PouchManager::loadManifest() {
  const auto ann = store_->versionAnnotation(MicroversionId{});
  if (!ann || ann->alias != "pouch-manifest" || ann->description.empty()) {
    return;
  }
  // Parse serialized manifest if present
  std::istringstream ss(ann->description);
  std::string zoneEntry;
  std::vector<DropZoneConfig> loadedConfigs;
  while (std::getline(ss, zoneEntry, ';')) {
    if (zoneEntry.empty()) {
      continue;
    }
    std::istringstream entryStream(zoneEntry);
    std::string id, label, auraStr, weightStr;
    if (std::getline(entryStream, id, '|') &&
        std::getline(entryStream, label, '|') &&
        std::getline(entryStream, auraStr, '|') &&
        std::getline(entryStream, weightStr, '|')) {
      try {
        const auto aura   = static_cast<std::uint32_t>(std::stoul(auraStr));
        const auto weight = std::stof(weightStr);
        loadedConfigs.push_back(DropZoneConfig{
            .id              = id,
            .label           = label,
            .backgroundColor = glm::vec4(0.12F, 0.15F, 0.20F, 0.85F),
            .auraColor       = aura,
            .heightWeight    = weight,
        });
      } catch (...) {
      }
    }
  }
  if (!loadedConfigs.empty()) {
    zones_.clear();
    for (auto &cfg : loadedConfigs) {
      addZone(std::move(cfg));
    }
  }
}

} // namespace xudu
