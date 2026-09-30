/**
 * @file pouch_zone.cpp
 * @brief Core data model and backing system xanadoc manager for drop zone
 * pouches.
 */
#include "pouch_zone.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>

#include <gleditor/ranges.hpp>

#include "common/xanadu/system_docs.hpp"
#include "zigzag/cell_views.hpp"

namespace xanadu {

DropZone::DropZone(DropZoneConfig config) : config_(std::move(config)) {}

void DropZone::setRect(const float x, const float y, const float width,
                       const float height) noexcept {
  x_      = x;
  y_      = y;
  width_  = width;
  height_ = height;
}

bool DropZone::contains(const float screenX,
                        const float screenY) const noexcept {
  if (width_ <= 0.0F || height_ <= 0.0F) {
    return false;
  }
  return screenX >= x_ && screenX <= (x_ + width_) && screenY >= y_ &&
         screenY <= (y_ + height_);
}

void DropZone::addItem(PouchItem item) { items_.push_back(std::move(item)); }

bool DropZone::removeItem(const std::uint64_t itemId) {
  const auto it = std::ranges::find_if(items_, [itemId](const PouchItem &item) {
    return item.itemId == itemId;
  });
  if (it != items_.end()) {
    items_.erase(it);
    return true;
  }
  return false;
}

void DropZone::clear() { items_.clear(); }

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

PouchManager::PouchManager(Store &systemStore) : systemStore_(&systemStore) {
  if (systemStore_->opCount() == 0) {
    initializeSystemStore(*systemStore_, SystemDocKind::Pouches);
  }
  currentVersion_ = systemStore_->latest();
  initDefaultZones();
}

void PouchManager::initDefaultZones() {
  zones_.clear();

  if (store().opCount() > 0 && store().homeCell() != zigzag::noCell) {
    const auto pouchCfg = PouchConfig::fromStore(store());
    if (!pouchCfg.zones.empty()) {
      for (const auto &spec : pouchCfg.zones) {
        DropZoneConfig cfg{
            .id              = spec.id,
            .cell            = spec.cell,
            .label           = spec.label,
            .backgroundColor = glm::vec4(0.12F, 0.15F, 0.20F, 0.85F),
            .auraColor       = spec.auraColor,
            .heightWeight    = spec.heightWeight,
        };
        if (cfg.id == "to_link_left") {
          cfg.backgroundColor = glm::vec4(0.08F, 0.15F, 0.20F, 0.85F);
        } else if (cfg.id == "to_link_right") {
          cfg.backgroundColor = glm::vec4(0.18F, 0.08F, 0.16F, 0.85F);
        } else if (cfg.id == "notes") {
          cfg.backgroundColor = glm::vec4(0.18F, 0.15F, 0.08F, 0.85F);
          cfg.heightWeight    = 1.2F;
        } else if (cfg.id == "scratch") {
          cfg.backgroundColor = glm::vec4(0.08F, 0.18F, 0.12F, 0.85F);
        }
        addZone(std::move(cfg));
      }
      return;
    }
  }

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
  const auto it = std::ranges::find_if(
      zones_, [id](const auto &z) { return z->id() == id; });
  if (it != zones_.end()) {
    zones_.erase(it);
    return true;
  }
  return false;
}

namespace {
/// The zone @p pick chooses, as a reference to the zone rather than to the
/// unique_ptr that owns it.
template <typename Zones, typename Pick>
auto zoneWhere(Zones &zones, const Pick &pick) {
  return gleditor::findRef(zones, [&](const auto &zone) { return pick(*zone); })
      .transform([](auto &owner) -> decltype(*owner) { return *owner; });
}
} // namespace

gleditor::cpp26::optional<DropZone &>
PouchManager::zoneById(const std::string_view id) noexcept {
  return zoneWhere(zones_, [id](const DropZone &z) { return z.id() == id; });
}

gleditor::cpp26::optional<const DropZone &>
PouchManager::zoneById(const std::string_view id) const noexcept {
  return zoneWhere(zones_, [id](const DropZone &z) { return z.id() == id; });
}

gleditor::cpp26::optional<DropZone &>
PouchManager::zoneAt(const float screenX, const float screenY) noexcept {
  return zoneWhere(
      zones_, [=](const DropZone &z) { return z.contains(screenX, screenY); });
}

DropZone &PouchManager::zoneOrDefault(const std::string_view id) {
  const auto it = std::ranges::find_if(
      zones_, [id](const auto &z) { return z->id() == id; });
  if (it != zones_.end()) {
    return **it;
  }
  // An unknown zone drops into the first one, and a manager with none yet
  // gets its defaults first.
  if (zones_.empty()) {
    initDefaultZones();
  }
  return *zones_.front();
}

void PouchManager::ensureZoneCell(DropZone &zone) {
  if (zigzag::noCell != zone.cell()) {
    return;
  }
  if (store().opCount() == 0 || store().homeCell() == zigzag::noCell) {
    initializeSystemStore(store(), SystemDocKind::Pouches);
    currentVersion_ = store().latest();
  }
  const auto pouchCfg = PouchConfig::fromStore(store());
  for (const auto &spec : pouchCfg.zones) {
    if (spec.id == zone.id() && zigzag::noCell != spec.cell) {
      zone.setCell(spec.cell);
      return;
    }
  }

  // Not yet in store: add zone setting
  zigzag::CellRef cell = zigzag::noCell;
  currentVersion_      = addPouchZone(store(), currentVersion_,
                                      DropZoneSpec{
                                          .cell         = zigzag::noCell,
                                          .id           = zone.id(),
                                          .label        = zone.label(),
                                          .auraColor    = zone.auraColor(),
                                          .heightWeight = zone.heightWeight(),
                                      },
                                      &cell);
  if (zigzag::noCell != cell) {
    zone.setCell(cell);
  }
}

PouchItem PouchManager::dropSpan(const std::string_view zoneId,
                                 const PrimediaSpan &span,
                                 std::string previewText,
                                 const PouchOrigin &origin) {
  DropZone *const zone = &zoneOrDefault(zoneId);
  ensureZoneCell(*zone);

  const auto res = store().appendPouchItemWithRef(currentVersion_, zone->cell(),
                                                  span, origin);
  currentVersion_ = res.version;

  PouchItem item{
      .itemId      = res.itemCell,
      .span        = span,
      .previewText = std::move(previewText),
      .originVersion =
          origin.document ? origin.document->version : currentVersion_,
      .originDocIndex  = 0,
      .originCharStart = 0,
      .originCharEnd   = static_cast<std::uint32_t>(span.length),
      .timestampUtc    = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count()),
      .originKind       = PouchOriginKind::Document,
      .originCell       = res.itemCell,
      .originSliceIndex = 0,
      .originRankCoord  = "d.items: #" + std::to_string(res.itemCell),
      .originOpRef      = origin.cell,
      .originDocState   = origin.document,
  };

  zone->addItem(item);
  return item;
}

PouchItem PouchManager::dropSpan(const std::string_view zoneId,
                                 const PrimediaSpan &span,
                                 std::string previewText,
                                 const MicroversionId &sourceVer,
                                 const std::uint32_t docIndex,
                                 const std::uint32_t charStart,
                                 const std::uint32_t charEnd) {
  PouchOrigin origin;
  if (!sourceVer.isZero()) {
    origin.document = GlobalDocumentState{
        .scroll  = "",
        .version = sourceVer,
    };
  }
  auto item            = dropSpan(zoneId, span, std::move(previewText), origin);
  item.originDocIndex  = docIndex;
  item.originCharStart = charStart;
  item.originCharEnd   = charEnd;
  return item;
}

PouchItem PouchManager::dropCell(const std::string_view zoneId,
                                 const PrimediaSpan &span,
                                 std::string previewText,
                                 const GlobalOpRef &cellOrigin,
                                 const std::string_view rankCoord) {
  DropZone *const zone = &zoneOrDefault(zoneId);
  ensureZoneCell(*zone);

  PouchOrigin origin;
  origin.cell = cellOrigin;

  const auto res = store().appendPouchItemWithRef(currentVersion_, zone->cell(),
                                                  span, origin);
  currentVersion_ = res.version;

  PouchItem item{
      .itemId          = res.itemCell,
      .span            = span,
      .previewText     = std::move(previewText),
      .originVersion   = currentVersion_,
      .originDocIndex  = 0,
      .originCharStart = 0,
      .originCharEnd   = static_cast<std::uint32_t>(span.length),
      .timestampUtc    = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count()),
      .originKind       = PouchOriginKind::ZigzagCell,
      .originCell       = res.itemCell,
      .originSliceIndex = 0,
      .originRankCoord  = std::string(rankCoord),
      .originOpRef      = cellOrigin,
      .originDocState   = std::nullopt,
  };

  zone->addItem(item);
  return item;
}

PouchItem PouchManager::dropCell(const std::string_view zoneId,
                                 const PrimediaSpan &span,
                                 std::string previewText,
                                 const std::uint32_t cellRef,
                                 const std::string_view rankCoord,
                                 const std::uint32_t sliceIndex,
                                 const std::optional<GlobalOpRef> &cellOrigin) {
  if (cellOrigin) {
    auto item =
        dropCell(zoneId, span, std::move(previewText), *cellOrigin, rankCoord);
    item.originCell       = cellRef;
    item.originSliceIndex = sliceIndex;
    return item;
  }

  DropZone *const zone = &zoneOrDefault(zoneId);
  ensureZoneCell(*zone);

  PouchOrigin origin;
  const auto res = store().appendPouchItemWithRef(currentVersion_, zone->cell(),
                                                  span, origin);
  currentVersion_ = res.version;

  PouchItem item{
      .itemId          = res.itemCell,
      .span            = span,
      .previewText     = std::move(previewText),
      .originVersion   = currentVersion_,
      .originDocIndex  = 0,
      .originCharStart = 0,
      .originCharEnd   = static_cast<std::uint32_t>(span.length),
      .timestampUtc    = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count()),
      .originKind       = PouchOriginKind::ZigzagCell,
      .originCell       = cellRef,
      .originSliceIndex = sliceIndex,
      .originRankCoord  = std::string(rankCoord),
      .originOpRef      = std::nullopt,
      .originDocState   = std::nullopt,
  };

  zone->addItem(item);
  return item;
}

bool PouchManager::dismissItem(const std::uint64_t itemId) {
  const auto itemCell = static_cast<zigzag::CellRef>(itemId);
  for (const auto &zone : zones_) {
    const auto &items = zone->items();
    const auto it =
        std::ranges::find_if(items, [itemId](const PouchItem &item) {
          return item.itemId == itemId;
        });
    if (it != items.end()) {
      zone->removeItem(itemId);
      currentVersion_ =
          store().dismissPouchItem(currentVersion_, zone->cell(), itemCell);
      return true;
    }
  }
  return false;
}

void PouchManager::saveManifest() {
  // Pouch item and zone states are persisted as first-class structure cells
  // in the backing store (§5.8).
}

void PouchManager::loadManifest() {
  if (store().opCount() > 0 && store().homeCell() != zigzag::noCell) {
    const auto pouchCfg = PouchConfig::fromStore(store());
    if (!pouchCfg.zones.empty()) {
      zones_.clear();
      for (const auto &spec : pouchCfg.zones) {
        DropZoneConfig cfg{
            .id              = spec.id,
            .cell            = spec.cell,
            .label           = spec.label,
            .backgroundColor = glm::vec4(0.12F, 0.15F, 0.20F, 0.85F),
            .auraColor       = spec.auraColor,
            .heightWeight    = spec.heightWeight,
        };
        if (cfg.id == "to_link_left") {
          cfg.backgroundColor = glm::vec4(0.08F, 0.15F, 0.20F, 0.85F);
        } else if (cfg.id == "to_link_right") {
          cfg.backgroundColor = glm::vec4(0.18F, 0.08F, 0.16F, 0.85F);
        } else if (cfg.id == "notes") {
          cfg.backgroundColor = glm::vec4(0.18F, 0.15F, 0.08F, 0.85F);
          cfg.heightWeight    = 1.2F;
        } else if (cfg.id == "scratch") {
          cfg.backgroundColor = glm::vec4(0.08F, 0.18F, 0.12F, 0.85F);
        }
        addZone(std::move(cfg));
      }
    }

    const auto curVer =
        currentVersion_.isZero() ? store().latest() : currentVersion_;
    const auto manifold    = store().rebuildManifold(curVer);
    const auto dimItemsOpt = manifold.dimensionNamed("d.items", store());
    if (dimItemsOpt) {
      const auto dimItems = *dimItemsOpt;
      const auto dimOriginCellOpt =
          manifold.dimensionNamed("d.origin-cell", store());
      const auto dimOriginStateOpt =
          manifold.dimensionNamed("d.origin-state", store());

      const auto registry = manifold.scrollRegistry(store());

      for (auto &zone : zones_) {
        zone->clear();
        if (zigzag::noCell == zone->cell()) {
          continue;
        }

        for (const auto itemCell :
             zigzag::rankAfter(manifold, zone->cell(), dimItems)) {
          const auto slot = manifold.slot(itemCell);
          if (!slot) {
            continue;
          }
          const auto contentSpans = manifold.contentOf(itemCell);
          const auto span =
              contentSpans.empty() ? PrimediaSpan{} : contentSpans.front();

          // Lazy preview text from SpanReader (store)
          std::string previewText = manifold.textOf(itemCell, store());

          PouchOriginKind originKind = PouchOriginKind::Document;
          std::optional<GlobalOpRef> originOpRef;
          std::optional<GlobalDocumentState> originDocState;

          if (dimOriginCellOpt) {
            const auto phCell = manifold.linked(itemCell, *dimOriginCellOpt,
                                                zigzag::DimVector::POS);
            if (phCell != zigzag::noCell) {
              originKind = PouchOriginKind::ZigzagCell;
              if (const auto extRef = store().externTarget(phCell)) {
                if (const auto rec = registry.findRecord(extRef->scroll)) {
                  originOpRef = GlobalOpRef{
                      .scroll   = rec->globalKey,
                      .produces = extRef->produces,
                  };
                } else if (const auto srec =
                               store().scrollRegistry().findRecord(
                                   extRef->scroll)) {
                  originOpRef = GlobalOpRef{
                      .scroll   = srec->globalKey,
                      .produces = extRef->produces,
                  };
                }
              }
            }
          }

          if (dimOriginStateOpt) {
            const auto descCell = manifold.linked(itemCell, *dimOriginStateOpt,
                                                  zigzag::DimVector::POS);
            if (descCell != zigzag::noCell) {
              const auto descText = manifold.textOf(descCell, store());
              originDocState      = readGlobalDocumentState(descText);
            }
          }

          PouchItem item{
              .itemId           = itemCell,
              .span             = span,
              .previewText      = std::move(previewText),
              .originVersion    = store().segmentedOps().idOf(slot->birthOp),
              .originDocIndex   = 0,
              .originCharStart  = 0,
              .originCharEnd    = static_cast<std::uint32_t>(span.length),
              .timestampUtc     = 0,
              .originKind       = originKind,
              .originCell       = itemCell,
              .originSliceIndex = 0,
              .originRankCoord  = "d.items: #" + std::to_string(itemCell),
              .originOpRef      = originOpRef,
              .originDocState   = originDocState,
          };
          zone->addItem(std::move(item));
        }
      }
    }
  }
}

} // namespace xanadu
