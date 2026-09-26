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
#include "zigzag/manifold.hpp"

namespace xanadu {

namespace {
// The rank items hang on from home, and what each carries.
constexpr std::string_view kPouchRank = "d.pouch";
constexpr std::string_view kZoneDim   = "d.zone";
constexpr std::string_view kOriginDim = "d.origin";
} // namespace

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
  if (const auto named = zoneById(id)) {
    return *named;
  }
  // An unknown zone drops into the first one, and a manager with none yet
  // gets its defaults first.
  if (zones_.empty()) {
    initDefaultZones();
  }
  return *zones_.front();
}

PouchItem PouchManager::dropSpan(const std::string_view zoneId,
                                 const PrimediaSpan &span,
                                 std::string previewText,
                                 const MicroversionId &sourceVer,
                                 const std::uint32_t docIndex,
                                 const std::uint32_t charStart,
                                 const std::uint32_t charEnd) {
  DropZone *const zone = &zoneOrDefault(zoneId);

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

  persistItem(item, zone->id());
  // The drop, labelled on the state it produced.
  store().setVersionAnnotation(
      currentVersion_,
      VersionAnnotation{
          .alias       = std::string(zone->id()),
          .description = item.previewText,
          .tag         = "pouch-drop",
          .timestamp   = std::to_string(
              std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count()),
      });
  zone->addItem(item);
  return item;
}

PouchItem PouchManager::dropCell(const std::string_view zoneId,
                                 const PrimediaSpan &span,
                                 std::string previewText,
                                 const std::uint32_t cellRef,
                                 const std::string_view rankCoord,
                                 const std::uint32_t sliceIndex) {
  DropZone *const zone = &zoneOrDefault(zoneId);

  PouchItem item{
      .itemId          = nextItemId_++,
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
  };

  persistItem(item, zone->id());
  // The drop, labelled on the state it produced.
  store().setVersionAnnotation(
      currentVersion_,
      VersionAnnotation{
          .alias       = std::string(zone->id()),
          .description = item.previewText,
          .tag         = "pouch-cell-drop",
          .timestamp   = std::to_string(
              std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count()),
      });
  zone->addItem(item);
  return item;
}

bool PouchManager::dismissItem(const std::uint64_t itemId) {
  for (const auto &zone : zones_) {
    const auto &items = zone->items();
    const auto it =
        std::ranges::find_if(items, [itemId](const PouchItem &item) {
          return item.itemId == itemId;
        });
    if (it != items.end()) {
      const auto cell = static_cast<zigzag::CellRef>(it->cell);
      zone->removeItem(itemId);
      // Off the rank, into limbo: the cell and its history stay.
      if (zigzag::noCell != cell) {
        const auto rank     = dimension(kPouchRank);
        const auto manifold = store().rebuildManifold(currentVersion_);
        const auto before = manifold.linked(cell, rank, zigzag::DimVector::NEG);
        const auto after  = manifold.linked(cell, rank, zigzag::DimVector::POS);
        if (zigzag::noCell != before) {
          currentVersion_ = store().setLink(currentVersion_, before, rank,
                                            zigzag::DimVector::POS, after);
        }
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
  store().setVersionAnnotation(
      MicroversionId{},
      VersionAnnotation{
          .alias       = "pouch-manifest",
          .description = ss.str(),
          .tag         = "manifest",
          .timestamp   = std::to_string(
              std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count()),
      });
}

zigzag::DimRef PouchManager::dimension(const std::string_view name) {
  if (const auto found = store()
                             .rebuildManifold(currentVersion_)
                             .dimensionNamed(name, store())) {
    return *found;
  }
  const auto minted = store().makeDimension(currentVersion_, name);
  currentVersion_   = minted.version;
  return minted.dim;
}

void PouchManager::persistItem(PouchItem &item, const std::string_view zoneId) {
  auto &st = store();
  if (zigzag::noCell == st.homeCell()) {
    currentVersion_ = st.sliceGenesis(currentVersion_);
  }
  const auto rank   = dimension(kPouchRank);
  const auto zone   = dimension(kZoneDim);
  const auto origin = dimension(kOriginDim);
  const auto mint   = [&](const MicroversionId &next) {
    currentVersion_ = next;
    return st.cellRefOf(next);
  };
  const auto link = [&](const zigzag::CellRef from, const zigzag::DimRef dim,
                        const zigzag::CellRef to) {
    currentVersion_ =
        st.setLink(currentVersion_, from, dim, zigzag::DimVector::POS, to);
  };

  auto tail           = st.homeCell();
  const auto manifold = st.rebuildManifold(currentVersion_);
  for (auto next = manifold.linked(tail, rank); zigzag::noCell != next;
       next      = manifold.linked(tail, rank)) {
    tail = next;
  }
  // The item's content is the span it quotes: no bytes are copied.
  const auto cell = mint(st.makeCell(currentVersion_, item.span));
  item.cell       = cell;
  link(tail, rank, cell);
  link(cell, zone, mint(st.makeCell(currentVersion_, zoneId)));

  const auto number = [&](const std::uint64_t value) {
    return mint(
        st.makeScalarCell(currentVersion_, static_cast<std::int64_t>(value)));
  };
  const zigzag::CellRef fields[] = {
      mint(st.makeCell(currentVersion_, item.originVersion.str())),
      number(static_cast<std::uint64_t>(item.originKind)),
      number(item.originDocIndex),
      number(item.originCharStart),
      number(item.originCharEnd),
      number(item.originCell),
      number(item.originSliceIndex),
      mint(st.makeCell(currentVersion_, item.originRankCoord)),
      number(item.timestampUtc),
  };
  auto last = cell;
  for (const auto field : fields) {
    link(last, origin, field);
    last = field;
  }
}

void PouchManager::loadItems() {
  const auto &st = store();
  if (zigzag::noCell == st.homeCell()) {
    return;
  }
  const auto manifold = st.rebuildManifold(currentVersion_);
  const auto rank     = manifold.dimensionNamed(kPouchRank, st);
  const auto zoneDim  = manifold.dimensionNamed(kZoneDim, st);
  const auto origin   = manifold.dimensionNamed(kOriginDim, st);
  if (!rank) {
    return;
  }
  // A preview long enough to recognise, as a drop's own is.
  constexpr std::size_t kPreviewBytes = 40;
  for (auto cell                    = manifold.linked(st.homeCell(), *rank);
       zigzag::noCell != cell; cell = manifold.linked(cell, *rank)) {
    PouchItem item;
    item.itemId = nextItemId_++;
    item.cell   = cell;
    if (const auto content = manifold.contentOf(cell); !content.empty()) {
      item.span = content.front();
    }
    item.previewText = manifold.textOf(cell, st).substr(0, kPreviewBytes);
    std::vector<zigzag::CellRef> fields;
    if (origin) {
      for (auto at = manifold.linked(cell, *origin); zigzag::noCell != at;
           at      = manifold.linked(at, *origin)) {
        fields.push_back(at);
      }
    }
    const auto text = [&](const std::size_t i) {
      return i < fields.size() ? manifold.textOf(fields[i], st) : std::string{};
    };
    const auto number = [&](const std::size_t i) -> std::uint64_t {
      return i < fields.size() ? static_cast<std::uint64_t>(
                                     manifold.asInt64(fields[i]).value_or(0))
                               : 0U;
    };
    item.originVersion    = MicroversionId::parse(text(0));
    item.originKind       = static_cast<PouchOriginKind>(number(1));
    item.originDocIndex   = static_cast<std::uint32_t>(number(2));
    item.originCharStart  = static_cast<std::uint32_t>(number(3));
    item.originCharEnd    = static_cast<std::uint32_t>(number(4));
    item.originCell       = static_cast<std::uint32_t>(number(5));
    item.originSliceIndex = static_cast<std::uint32_t>(number(6));
    item.originRankCoord  = text(7);
    item.timestampUtc     = number(8);
    const auto zoneCell =
        zoneDim ? manifold.linked(cell, *zoneDim) : zigzag::noCell;
    const auto zoneId = zigzag::noCell == zoneCell
                            ? std::string{}
                            : manifold.textOf(zoneCell, st);
    zoneOrDefault(zoneId).addItem(std::move(item));
  }
}

void PouchManager::loadManifest() {
  loadZones();
  loadItems();
}

void PouchManager::loadZones() {
  if (store().opCount() > 0) {
    const auto pouchCfg = PouchConfig::fromStore(store());
    if (!pouchCfg.zones.empty()) {
      zones_.clear();
      for (const auto &spec : pouchCfg.zones) {
        addZone(DropZoneConfig{
            .id              = spec.id,
            .label           = spec.label,
            .backgroundColor = glm::vec4(0.12F, 0.15F, 0.20F, 0.85F),
            .auraColor       = spec.auraColor,
            .heightWeight    = spec.heightWeight,
        });
      }
      return;
    }
  }

  const auto ann = store().versionAnnotation(MicroversionId{});
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
    std::string id;
    std::string label;
    std::string auraStr;
    std::string weightStr;
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
      } catch (...) { // NOLINT(bugprone-empty-catch)
        // A malformed entry is skipped rather than failing the whole load.
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

} // namespace xanadu
