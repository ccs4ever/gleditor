/**
 * @file store_object_manager.cpp
 * @brief Implementation of StoreObjectManager drawer.
 */
#include "common/ui/store_object_manager.hpp"

#include <algorithm>
#include <utility>

#include <glm/ext/matrix_clip_space.hpp>

#include <gleditor/render/device.hpp>
#include <gleditor/render_state.hpp>

namespace a11y = gleditor::a11y;

namespace xanadu {

StoreObjectManager::StoreObjectManager(Store &store, std::string fontName,
                                       ToggleCallback onToggle,
                                       CreateCallback onCreate,
                                       CloseCallback onClose,
                                       IsOpenPredicate isOpen)
    : store_(store), fontName_(std::move(fontName)),
      onToggle_(std::move(onToggle)), onCreate_(std::move(onCreate)),
      onClose_(std::move(onClose)), isOpen_(std::move(isOpen)) {
  refresh();
}

StoreObjectManager::~StoreObjectManager() = default;

void StoreObjectManager::deviceReady(
    render::RenderDevice &device,
    const render::PipelineDesc &documentPipeline) {
  canvas_ = std::make_unique<gleditor::Canvas>(&device, fontName_);
  canvas_->createPipeline(documentPipeline, false);
}

StoreObjectManager *StoreObjectManager::setVisible(const bool visible) {
  if (visible_ != visible) {
    visible_ = visible;
    if (visible_)
      activate();
    else
      deactivate();
    if (visible_) {
      refresh();
    }
    a11yRevision_++;
  }
  return this;
}

StoreObjectManager *StoreObjectManager::toggle() {
  setVisible(!visible_);
  return this;
}

StoreObjectManager *StoreObjectManager::refresh() {
  items_.clear();
  const auto births = store_.discoverStructureBirths(store_.latest());
  for (const auto &b : births) {
    bool open = false;
    if (isOpen_) {
      open = isOpen_(b.opIndex);
    }
    items_.push_back(ObjectItem{
        .birthOp = b.opIndex,
        .kind    = b.kind,
        .name    = b.name,
        .isOpen  = open,
        .yTop    = 0.0F,
        .yBottom = 0.0F,
    });
  }
  a11yRevision_++;
  return this;
}

StoreObjectManager *StoreObjectManager::createSlice() {
  if (onCreate_) {
    onCreate_(StructureKind::Slice);
  } else {
    const auto parent = store_.latest();
    const auto name   = "Slice " + std::to_string(store_.opCount() + 1);
    if (store_.homeCell() == zigzag::noCell) {
      std::ignore = store_.sliceGenesis(parent, name);
    } else {
      std::ignore = store_.makeSlice(parent, name);
    }
  }
  refresh();
  if (!items_.empty() && onToggle_) {
    const auto &last = items_.back();
    if (last.kind == StructureKind::Slice && !last.isOpen) {
      onToggle_(last.birthOp, last.kind, true);
    }
  }
  return this;
}

StoreObjectManager *StoreObjectManager::createXanadoc() {
  if (onCreate_) {
    onCreate_(StructureKind::Xanadoc);
  } else {
    const auto parent = store_.latest();
    const auto name   = "Document " + std::to_string(store_.opCount() + 1);
    std::ignore       = store_.makeXanadoc(parent, name);
  }
  refresh();
  if (!items_.empty() && onToggle_) {
    const auto &last = items_.back();
    if (last.kind == StructureKind::Xanadoc && !last.isOpen) {
      onToggle_(last.birthOp, last.kind, true);
    }
  }
  return this;
}

StoreObjectManager *StoreObjectManager::toggleItem(const std::size_t index) {
  if (index >= items_.size()) {
    return this;
  }
  auto &item          = items_[index];
  const bool newState = !item.isOpen;
  item.isOpen         = newState;
  if (onToggle_) {
    onToggle_(item.birthOp, item.kind, newState);
  }
  a11yRevision_++;
  return this;
}

StoreObjectManager *StoreObjectManager::closeItem(const std::size_t index) {
  if (index >= items_.size()) {
    return this;
  }
  auto &item = items_[index];
  if (item.isOpen) {
    item.isOpen = false;
    if (onClose_) {
      onClose_(item.birthOp);
    } else if (onToggle_) {
      onToggle_(item.birthOp, item.kind, false);
    }
    a11yRevision_++;
  }
  return this;
}

void StoreObjectManager::drawFrame(gleditor::FrameContext &ctx) {
  if (!visible_ || nullptr == canvas_) {
    return;
  }

  const auto width  = static_cast<float>(ctx.screenWidth);
  const auto height = static_cast<float>(ctx.screenHeight);
  const auto ortho  = glm::ortho(0.0F, width, 0.0F, height, -1.0F, 1.0F);

  canvas_->clear();

  // Drawer geometry on right side
  constexpr float dw = 340.0F;
  const float dx     = std::max(0.0F, width - dw);
  constexpr float dy = 0.0F;
  const float dh     = height;

  // Background and border
  canvas_->addRect(dx, dy, dw, dh, 0x161A22F5U);
  canvas_->addLine(dx, dy, dx, dy + dh, 1.5F, 0x303644FFU);

  // Header
  constexpr float headerH = 44.0F;
  const float headerY     = dh - headerH;
  canvas_->addRect(dx, headerY, dw, headerH, 0x202636FFU);
  canvas_->addLine(dx, headerY, dx + dw, headerY, 1.0F, 0x303644FFU);
  canvas_->addText(ctx.state, dx + 14.0F, dh - 13.0F, "Store Objects",
                   0xFFFFFFFFU, 0x202636FFU);

  // Close drawer [×] button
  canvas_->setTag(render::tagKindOverlay, kTagCloseDrawer);
  canvas_->addText(ctx.state, dx + dw - 24.0F, dh - 13.0F, "×", 0x9AA3B2FFU,
                   0x202636FFU);

  // Creation buttons row
  const float curYStart = headerY - 36.0F;
  constexpr float btnH  = 26.0F;
  constexpr float btnW  = (dw - 36.0F) / 2.0F;

  // [+ Slice] button
  canvas_->setTag(render::tagKindOverlay, kTagNewSlice);
  canvas_->addRect(dx + 12.0F, curYStart, btnW, btnH, 0x253348FFU);
  canvas_->addText(ctx.state, dx + 22.0F, curYStart + 18.0F, "+ Slice",
                   0x7EBEFFFFU, 0x253348FFU);

  // [+ Xanadoc] button
  canvas_->setTag(render::tagKindOverlay, kTagNewXanadoc);
  canvas_->addRect(dx + 12.0F + btnW + 12.0F, curYStart, btnW, btnH,
                   0x253348FFU);
  canvas_->addText(ctx.state, dx + 12.0F + btnW + 18.0F, curYStart + 18.0F,
                   "+ Xanadoc", 0x7EBEFFFFU, 0x253348FFU);

  // Separator
  float curY = curYStart - 10.0F;
  canvas_->addLine(dx + 12.0F, curY, dx + dw - 12.0F, curY, 1.0F, 0x2A3040FFU);
  curY -= 12.0F;

  if (items_.empty()) {
    // Fresh / New Store Mode
    const float emptyY = curY - 30.0F;
    canvas_->addText(ctx.state, dx + 24.0F, emptyY,
                     "No documents or slices yet.", 0x8892A2FFU, 0x161A22F5U);
    canvas_->addText(ctx.state, dx + 24.0F, emptyY - 22.0F,
                     "Click [+ Xanadoc] or [+ Slice] above", 0x5C667AFFU,
                     0x161A22F5U);
    canvas_->addText(ctx.state, dx + 24.0F, emptyY - 40.0F,
                     "to initialize this store.", 0x5C667AFFU, 0x161A22F5U);
  } else {
    // Populated Store Mode
    constexpr float rowH = 30.0F;
    for (std::size_t i = 0; i < items_.size(); ++i) {
      auto &item = items_[i];
      if (curY < 20.0F) {
        break;
      }
      const float rowY = curY - rowH;
      item.yTop        = curY;
      item.yBottom     = rowY;

      const std::uint32_t rowBg = item.isOpen ? 0x202838FFU : 0x1C212BFFU;
      canvas_->addRect(dx + 8.0F, rowY, dw - 16.0F, rowH, rowBg);

      // Checkbox
      constexpr float cbSize = 18.0F;
      const float cbX        = dx + 16.0F;
      const float cbY        = rowY + 6.0F;
      canvas_->setTag(render::tagKindOverlay,
                      kTagItemCheckboxBase + static_cast<std::uint32_t>(i));
      canvas_->addRect(cbX, cbY, cbSize, cbSize,
                       item.isOpen ? 0x3B82F6FFU : 0x283040FFU);
      if (item.isOpen) {
        canvas_->addText(ctx.state, cbX + 4.0F, cbY + 14.0F, "v", 0xFFFFFFFFU,
                         0x3B82F6FFU);
      }

      // Type badge
      const float badgeX             = cbX + cbSize + 8.0F;
      const bool isSlice             = (item.kind == StructureKind::Slice);
      const std::string badge        = isSlice ? "[SLICE]" : "[DOC]";
      const std::uint32_t badgeColor = isSlice ? 0x34D399FFU : 0x60A5FAFFU;
      canvas_->addText(ctx.state, badgeX, rowY + 21.0F, badge, badgeColor,
                       rowBg);

      // Name
      const float nameX = badgeX + 54.0F;
      canvas_->setTextWidthLimit(
          static_cast<int>(dw - (nameX - dx) - (item.isOpen ? 30.0F : 10.0F)));
      canvas_->addText(ctx.state, nameX, rowY + 21.0F, item.name, 0xE2E8F0FFU,
                       rowBg);

      // Close button [×] if open
      if (item.isOpen) {
        const float closeX = dx + dw - 28.0F;
        canvas_->setTag(render::tagKindOverlay,
                        kTagItemCloseBase + static_cast<std::uint32_t>(i));
        canvas_->addText(ctx.state, closeX, rowY + 21.0F, "×", 0xEF4444FFU,
                         rowBg);
      }

      curY -= (rowH + 4.0F);
    }
  }

  canvas_->commit();
  canvas_->draw(ctx.state, ortho);
}

bool StoreObjectManager::picked(const render::PickingResult &pick,
                                RenderState & /*state*/) {
  if (!visible_ || pick.tag.kind != render::tagKindOverlay) {
    return false;
  }
  const auto rawTag = pick.tag.clusterIndex;
  if (rawTag == kTagCloseDrawer) {
    setVisible(false);
    return true;
  }
  if (rawTag == kTagNewSlice) {
    createSlice();
    return true;
  }
  if (rawTag == kTagNewXanadoc) {
    createXanadoc();
    return true;
  }
  if (rawTag >= kTagItemCheckboxBase && rawTag < kTagItemCloseBase) {
    const auto idx = rawTag - kTagItemCheckboxBase;
    toggleItem(idx);
    return true;
  }
  if (rawTag >= kTagItemCloseBase && rawTag < kTagItemCloseBase + 1000U) {
    const auto idx = rawTag - kTagItemCloseBase;
    closeItem(idx);
    return true;
  }
  return false;
}

void StoreObjectManager::describe(gleditor::a11y::Builder &into) {
  if (!visible_) {
    return;
  }
  constexpr std::uint64_t drawerId = 26000;
  auto &drawer                     = into.add(drawerId, a11y::Role::Group);
  drawer.label                     = "Store Object Manager";

  const auto closeNodeId = 26001U;
  auto &closeNode        = into.add(closeNodeId, a11y::Role::Button);
  closeNode.label        = "Close Drawer";
  closeNode.actions      = a11y::bit(a11y::Action::Click);
  drawer.children.push_back(into.id(closeNodeId));

  const auto sliceBtnNodeId = 26002U;
  auto &sliceBtnNode        = into.add(sliceBtnNodeId, a11y::Role::Button);
  sliceBtnNode.label        = "New Slice";
  sliceBtnNode.actions      = a11y::bit(a11y::Action::Click);
  drawer.children.push_back(into.id(sliceBtnNodeId));

  const auto docBtnNodeId = 26003U;
  auto &docBtnNode        = into.add(docBtnNodeId, a11y::Role::Button);
  docBtnNode.label        = "New Xanadoc";
  docBtnNode.actions      = a11y::bit(a11y::Action::Click);
  drawer.children.push_back(into.id(docBtnNodeId));

  for (std::size_t i = 0; i < items_.size(); ++i) {
    const auto &item  = items_[i];
    const auto itemId = 26100U + static_cast<std::uint32_t>(i);
    auto &node        = into.add(itemId, a11y::Role::Switch);
    node.label        = item.name;
    node.toggled      = item.isOpen;
    node.actions      = a11y::bit(a11y::Action::Click);
    drawer.children.push_back(into.id(itemId));
  }

  into.contribute(into.id(drawerId));
}

} // namespace xanadu
