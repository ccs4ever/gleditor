/**
 * @file pouch_drawer.cpp
 * @brief Screen-edge Pouch Drawer overlay with partitioned drop zones and clasp
 * bench.
 */
#include "pouch_drawer.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <glm/ext/matrix_clip_space.hpp>

#include <gleditor/render/types.hpp>

namespace xudu {

PouchDrawer::PouchDrawer(Session &session, RendererRef renderer,
                         std::string fontName, const DockSide side)
    : session_(session), renderer_(std::move(renderer)),
      fontName_(std::move(fontName)), side_(side) {}

PouchDrawer::~PouchDrawer() = default;

void PouchDrawer::deviceReady(render::RenderDevice &device,
                              const render::PipelineDesc &pipeline) {
  canvas_ = std::make_unique<gleditor::Canvas>(&device, fontName_);
  canvas_->createPipeline(pipeline, false);
}

void PouchDrawer::setOpen(const bool open, const bool animated) noexcept {
  isOpen_           = open;
  targetSlideWidth_ = open ? kDrawerWidth : 0.0F;
  if (!animated) {
    currentSlideWidth_ = targetSlideWidth_;
  }
}

bool PouchDrawer::busy() const {
  return std::abs(currentSlideWidth_ - targetSlideWidth_) > 0.5F;
}

void PouchDrawer::layout(const float screenWidth, const float screenHeight) {
  if (std::abs(currentSlideWidth_ - targetSlideWidth_) > 0.5F) {
    currentSlideWidth_ += (targetSlideWidth_ - currentSlideWidth_) * 0.28F;
  } else {
    currentSlideWidth_ = targetSlideWidth_;
  }

  if (currentSlideWidth_ < 1.0F) {
    drawerW_ = 0.0F;
    drawerH_ = 0.0F;
    return;
  }

  drawerW_ = currentSlideWidth_;
  drawerH_ = screenHeight;
  drawerY_ = 0.0F;
  if (side_ == DockSide::Left) {
    drawerX_ = 0.0F;
  } else {
    drawerX_ = screenWidth - drawerW_;
  }

  // 1. Clasp Assembly Bench geometry below header
  constexpr float benchH = 105.0F;
  const float benchY     = screenHeight - 42.0F - benchH;
  forgeWidget_.setGeometry(drawerX_ + 6.0F, benchY, drawerW_ - 12.0F, benchH);

  // 2. Partitioned Drop Zones filling remaining vertical space
  const float zonesTop    = benchY - 10.0F;
  const float zonesBottom = 10.0F;
  const float availH      = std::max(40.0F, zonesTop - zonesBottom);

  float totalWeight = 0.0F;
  for (const auto &z : pouchManager_.zones()) {
    totalWeight += z->heightWeight();
  }
  if (totalWeight <= 0.0F) {
    totalWeight = 1.0F;
  }

  float curY = zonesTop;
  for (auto &zone : pouchManager_.zones()) {
    const float zoneH =
        std::max(36.0F, availH * (zone->heightWeight() / totalWeight) - 6.0F);
    curY -= (zoneH + 6.0F);
    zone->setRect(drawerX_ + 6.0F, curY, drawerW_ - 12.0F, zoneH);
  }
}

bool PouchDrawer::handleGhostDrop(const PrimediaSpan &span,
                                  const std::string &preview,
                                  const MicroversionId &sourceVer,
                                  const float screenX, const float screenY,
                                  const std::uint32_t docIndex,
                                  const std::uint32_t charStart,
                                  const std::uint32_t charEnd) {
  PouchItem item{
      .itemId          = 0,
      .span            = span,
      .previewText     = preview,
      .originVersion   = sourceVer,
      .originDocIndex  = docIndex,
      .originCharStart = charStart,
      .originCharEnd   = charEnd,
      .timestampUtc    = 0,
  };

  // 1. Check Clasp Forge Homestead (Left) Slot
  if (forgeWidget_.containsLeft(screenX, screenY)) {
    forgeWidget_.dropLeft(std::move(item));
    return true;
  }

  // 2. Check Clasp Forge Toward (Right) Slot
  if (forgeWidget_.containsRight(screenX, screenY)) {
    forgeWidget_.dropRight(std::move(item));
    return true;
  }

  // 3. Check Partitioned Drop Zones
  DropZone *zone = zoneAt(screenX, screenY);
  if (zone) {
    pouchManager_.dropSpan(zone->id(), span, preview, sourceVer, docIndex,
                           charStart, charEnd);
    return true;
  }

  return false;
}

void PouchDrawer::drawFrame(gleditor::FrameContext &ctx) {
  if (!canvas_) {
    return;
  }

  layout(static_cast<float>(ctx.screenWidth),
         static_cast<float>(ctx.screenHeight));
  if (currentSlideWidth_ < 1.0F) {
    return;
  }

  const auto width  = static_cast<float>(ctx.screenWidth);
  const auto height = static_cast<float>(ctx.screenHeight);
  const auto ortho  = glm::ortho(0.0F, width, 0.0F, height, -1.0F, 1.0F);

  canvas_->clear();

  // 1. Drawer Backdrop & Border
  canvas_->setTag(render::tagKindOverlay, 0);
  canvas_->addRect(drawerX_, drawerY_, drawerW_, drawerH_, 0x0A0E17FA);

  // Border separator
  if (side_ == DockSide::Left) {
    canvas_->addLine(drawerX_ + drawerW_, drawerY_, drawerX_ + drawerW_,
                     drawerY_ + drawerH_, 1.5F, 0x38BDF8AA);
  } else {
    canvas_->addLine(drawerX_, drawerY_, drawerX_, drawerY_ + drawerH_, 1.5F,
                     0x38BDF8AA);
  }

  // 2. Header Bar
  const float topY = drawerY_ + drawerH_;
  canvas_->addText(ctx.state, drawerX_ + 10.0F, topY - 16.0F, "POUCH DOCK",
                   0x38BDF8FF, 0);

  // Close [x] Button
  canvas_->setTag(render::tagKindOverlay, kTagDrawerClose);
  canvas_->addRect(drawerX_ + drawerW_ - 26.0F, topY - 32.0F, 20.0F, 20.0F,
                   0xDC2626CC);
  canvas_->addText(ctx.state, drawerX_ + drawerW_ - 21.0F, topY - 18.0F, "x",
                   0xFFFFFFFF, 0);

  // Add Zone [+ Zone] Button
  canvas_->setTag(render::tagKindOverlay, kTagDrawerAddZone);
  canvas_->addRect(drawerX_ + drawerW_ - 92.0F, topY - 32.0F, 62.0F, 20.0F,
                   0x0284C7CC);
  canvas_->addText(ctx.state, drawerX_ + drawerW_ - 88.0F, topY - 18.0F,
                   "+ Zone", 0xFFFFFFFF, 0);

  // Flip Dock [⇄] Button
  canvas_->setTag(render::tagKindOverlay, kTagDrawerFlipDock);
  canvas_->addRect(drawerX_ + drawerW_ - 124.0F, topY - 32.0F, 28.0F, 20.0F,
                   0x475569CC);
  canvas_->addText(ctx.state, drawerX_ + drawerW_ - 120.0F, topY - 18.0F, "<>",
                   0xFFFFFFFF, 0);

  // 3. Clasp Assembly Bench
  forgeWidget_.draw(*canvas_, ctx.state);

  // 4. Partitioned Drop Zones
  const auto &zones = pouchManager_.zones();
  for (std::size_t zIdx = 0; zIdx < zones.size(); ++zIdx) {
    const auto &zone = *zones[zIdx];
    if (zone.width() <= 0.0F || zone.height() <= 0.0F) {
      continue;
    }

    // Zone Background Tint
    canvas_->setTag(render::tagKindOverlay, 0);
    const auto bg    = zone.backgroundColor();
    const auto bgCol = (static_cast<std::uint32_t>(bg.r * 255.0F) << 24U) |
                       (static_cast<std::uint32_t>(bg.g * 255.0F) << 16U) |
                       (static_cast<std::uint32_t>(bg.b * 255.0F) << 8U) |
                       static_cast<std::uint32_t>(bg.a * 255.0F);
    canvas_->addRect(zone.x(), zone.y(), zone.width(), zone.height(), bgCol);

    // Glowing Aura Border
    const float auraThickness = zone.isHovered() ? 2.5F : 1.0F;
    const auto auraCol        = zone.auraColor();
    canvas_->addLine(zone.x(), zone.y(), zone.x() + zone.width(), zone.y(),
                     auraThickness, auraCol);
    canvas_->addLine(zone.x() + zone.width(), zone.y(), zone.x() + zone.width(),
                     zone.y() + zone.height(), auraThickness, auraCol);
    canvas_->addLine(zone.x() + zone.width(), zone.y() + zone.height(),
                     zone.x(), zone.y() + zone.height(), auraThickness,
                     auraCol);
    canvas_->addLine(zone.x(), zone.y() + zone.height(), zone.x(), zone.y(),
                     auraThickness, auraCol);

    // Zone Header Label
    const std::string zLabel =
        zone.label() + " (" + std::to_string(zone.items().size()) + ")";
    canvas_->addText(ctx.state, zone.x() + 8.0F,
                     zone.y() + zone.height() - 4.0F, zLabel, auraCol, 0);

    // Zone Clear [x] button
    canvas_->setTag(render::tagKindOverlay,
                    kTagZoneClearBase + static_cast<std::uint32_t>(zIdx));
    canvas_->addRect(zone.x() + zone.width() - 20.0F,
                     zone.y() + zone.height() - 18.0F, 14.0F, 14.0F,
                     0xDC262688);
    canvas_->addText(ctx.state, zone.x() + zone.width() - 17.0F,
                     zone.y() + zone.height() - 6.0F, "x", 0xFFFFFFFF, 0);

    // Sliced Cards inside Zone
    float cardY = zone.y() + zone.height() - 56.0F;
    for (const auto &item : zone.items()) {
      if (cardY < zone.y() + 4.0F) {
        break; // Viewport virtualization: don't shape overflow cards
      }

      constexpr float cardH = 34.0F;
      const float cardW     = zone.width() - 12.0F;
      const float cardX     = zone.x() + 6.0F;

      // Card body (Click -> Swing-Back)
      canvas_->setTag(render::tagKindOverlay,
                      kTagItemBase + static_cast<std::uint32_t>(item.itemId));
      canvas_->addRect(cardX, cardY, cardW, cardH, 0x0F172ACC);
      canvas_->addLine(cardX, cardY, cardX + cardW, cardY, 1.0F, 0x334155FF);
      canvas_->addLine(cardX + cardW, cardY, cardX + cardW, cardY + cardH, 1.0F,
                       0x334155FF);
      canvas_->addLine(cardX + cardW, cardY + cardH, cardX, cardY + cardH, 1.0F,
                       0x334155FF);
      canvas_->addLine(cardX, cardY + cardH, cardX, cardY, 1.0F, 0x334155FF);

      // Version badge (top line)
      const std::string vBadge = "v" + item.originVersion.str();
      canvas_->addText(ctx.state, cardX + 6.0F, cardY + cardH - 4.0F, vBadge,
                       0x38BDF8FF, 0);

      // Dismiss [x] button on card
      canvas_->setTag(render::tagKindOverlay,
                      kTagItemDismissBase +
                          static_cast<std::uint32_t>(item.itemId));
      canvas_->addRect(cardX + cardW - 16.0F, cardY + cardH - 16.0F, 12.0F,
                       12.0F, 0xEF444488);
      canvas_->addText(ctx.state, cardX + cardW - 13.0F, cardY + cardH - 6.0F,
                       "x", 0xFFFFFFFF, 0);

      // Preview snippet text (bottom line)
      std::string snippet = item.previewText;
      if (snippet.size() > 26) {
        snippet = snippet.substr(0, 23) + "...";
      }
      canvas_->addText(ctx.state, cardX + 6.0F, cardY + cardH - 18.0F, snippet,
                       0xF1F5F9FF, 0);

      cardY -= (cardH + 6.0F);
    }
  }

  canvas_->commit();
  canvas_->draw(ctx.state, ortho);
}

bool PouchDrawer::picked(const render::PickingResult &pick, RenderState &) {
  if (!isOpen_ || currentSlideWidth_ < 1.0F ||
      pick.tag.kind != render::tagKindOverlay) {
    return false;
  }

  const auto tag = pick.tag.clusterIndex;

  // 1. Drawer Controls
  if (tag == kTagDrawerClose) {
    setOpen(false);
    return true;
  }
  if (tag == kTagDrawerAddZone) {
    const auto count = pouchManager_.zones().size() + 1;
    pouchManager_.addZone(DropZoneConfig{
        .id              = "custom_" + std::to_string(count),
        .label           = "Zone " + std::to_string(count),
        .backgroundColor = glm::vec4(0.12F, 0.15F, 0.20F, 0.85F),
        .auraColor       = 0xF59E0BFFU,
        .heightWeight    = 1.0F,
    });
    return true;
  }
  if (tag == kTagDrawerFlipDock) {
    side_ = (side_ == DockSide::Left) ? DockSide::Right : DockSide::Left;
    return true;
  }

  // 2. Clasp Forge Actions
  if (forgeWidget_.picked(tag, session_, 0)) {
    return true;
  }

  // 3. Clear Zone
  if (tag >= kTagZoneClearBase && tag < kTagZoneClearBase + 50U) {
    const auto zIdx = tag - kTagZoneClearBase;
    if (zIdx < pouchManager_.zones().size()) {
      pouchManager_.zones()[zIdx]->clear();
      return true;
    }
  }

  // 4. Dismiss Item
  if (tag >= kTagItemDismissBase && tag < kTagItemDismissBase + 3000U) {
    const auto itemId = static_cast<std::uint64_t>(tag - kTagItemDismissBase);
    pouchManager_.dismissItem(itemId);
    return true;
  }

  // 5. Card Click -> Swing-Back Context Navigation
  if (tag >= kTagItemBase && tag < kTagItemBase + 3000U) {
    const auto itemId = static_cast<std::uint64_t>(tag - kTagItemBase);
    for (const auto &zone : pouchManager_.zones()) {
      for (const auto &item : zone->items()) {
        if (item.itemId == itemId) {
          if (swingBackHandler_) {
            swingBackHandler_(item);
          }
          return true;
        }
      }
    }
  }

  return false;
}

void PouchDrawer::describe(gleditor::a11y::Builder &into) {
  if (!isOpen_ || currentSlideWidth_ < 1.0F) {
    return;
  }
  constexpr std::uint64_t kDrawerNodeId = 0x70000000ULL;
  auto &drawerNode = into.add(kDrawerNodeId, gleditor::a11y::Role::Group);
  drawerNode.label = "Pouch Drawer";

  for (std::size_t i = 0; i < pouchManager_.zones().size(); ++i) {
    const auto &z               = pouchManager_.zones()[i];
    const std::uint64_t zNodeId = 0x70000100ULL + i;
    auto &zNode = into.add(zNodeId, gleditor::a11y::Role::Group);
    zNode.label =
        z->label() + " (" + std::to_string(z->items().size()) + " items)";
    drawerNode.children.push_back(zNodeId);
  }
}

std::uint64_t PouchDrawer::accessibilityRevision() const {
  return a11yRevision_;
}

} // namespace xudu
