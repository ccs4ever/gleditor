/**
 * @file clasp_link_forge.cpp
 * @brief Tripartite Clasp Assembly Bench widget for N x M asymmetric hyperlinking.
 */
#include "clasp_link_forge.hpp"

#include <algorithm>
#include <string>

#include <gleditor/render/types.hpp>

namespace xudu {

LinkForgeWidget::LinkForgeWidget() = default;

void LinkForgeWidget::setGeometry(const float x, const float y,
                                  const float width,
                                  const float height) noexcept {
  x_      = x;
  y_      = y;
  width_  = width;
  height_ = height;

  const float padding = 6.0F;
  const float availW  = width - padding * 4.0F;
  const float slotW   = std::max(60.0F, availW * 0.32F);
  const float slotH   = std::max(20.0F, height - padding * 2.0F - 16.0F);

  // Left Homestead Slot
  leftX_ = x + padding;
  leftY_ = y + padding;
  leftW_ = slotW;
  leftH_ = slotH;

  // Right Toward Slot
  rightX_ = x + width - padding - slotW;
  rightY_ = y + padding;
  rightW_ = slotW;
  rightH_ = slotH;
}

void LinkForgeWidget::dropLeft(PouchItem item) {
  leftSpans_.push_back(std::move(item));
}

void LinkForgeWidget::dropRight(PouchItem item) {
  rightSpans_.push_back(std::move(item));
}

void LinkForgeWidget::clearLeft() noexcept {
  leftSpans_.clear();
}

void LinkForgeWidget::clearRight() noexcept {
  rightSpans_.clear();
}

void LinkForgeWidget::cycleType() noexcept {
  switch (selectedType_) {
  case LinkType::Comment:
    selectedType_ = LinkType::Illustration;
    break;
  case LinkType::Illustration:
    selectedType_ = LinkType::Disagreement;
    break;
  case LinkType::Disagreement:
    selectedType_ = LinkType::Authorship;
    break;
  case LinkType::Authorship:
    selectedType_ = LinkType::Quotation;
    break;
  case LinkType::Quotation:
    selectedType_ = LinkType::Dimension;
    break;
  case LinkType::Dimension:
    selectedType_ = LinkType::Format;
    break;
  default:
    selectedType_ = LinkType::Comment;
    break;
  }
}

void LinkForgeWidget::cycleTier() noexcept {
  switch (selectedTier_) {
  case ProminenceTier::Author:
    selectedTier_ = ProminenceTier::Curated;
    break;
  case ProminenceTier::Curated:
    selectedTier_ = ProminenceTier::Public;
    break;
  default:
    selectedTier_ = ProminenceTier::Author;
    break;
  }
}

bool LinkForgeWidget::containsLeft(const float screenX,
                                   const float screenY) const noexcept {
  return screenX >= leftX_ && screenX <= (leftX_ + leftW_) &&
         screenY >= leftY_ && screenY <= (leftY_ + leftH_);
}

bool LinkForgeWidget::containsRight(const float screenX,
                                    const float screenY) const noexcept {
  return screenX >= rightX_ && screenX <= (rightX_ + rightW_) &&
         screenY >= rightY_ && screenY <= (rightY_ + rightH_);
}

bool LinkForgeWidget::forge(Session &session,
                            const std::uint32_t activeDocIndex) {
  if (!canForge()) {
    return false;
  }

  Link link;
  link.type  = selectedType_;
  link.tier  = selectedTier_;
  link.owner = "local_author";

  link.left.reserve(leftSpans_.size());
  for (const auto &item : leftSpans_) {
    link.left.push_back(item.span);
  }

  link.right.reserve(rightSpans_.size());
  for (const auto &item : rightSpans_) {
    link.right.push_back(item.span);
  }

  session.addLink(activeDocIndex, std::move(link));
  clearLeft();
  clearRight();
  return true;
}

bool LinkForgeWidget::picked(const std::uint32_t tag, Session &session,
                             const std::uint32_t activeDocIndex) {
  switch (tag) {
  case kTagClaspTypeSelector:
    cycleType();
    return true;
  case kTagClaspTierSelector:
    cycleTier();
    return true;
  case kTagClaspClearLeft:
    clearLeft();
    return true;
  case kTagClaspClearRight:
    clearRight();
    return true;
  case kTagClaspForgeButton:
    return forge(session, activeDocIndex);
  default:
    return false;
  }
}

void LinkForgeWidget::draw(gleditor::Canvas &canvas, RenderState &state) {
  if (width_ <= 0.0F || height_ <= 0.0F) {
    return;
  }

  // 1. Outer Bench Frame
  canvas.setTag(render::tagKindOverlay, 0);
  canvas.addRect(x_, y_, width_, height_, 0x1E1E2EDD);
  canvas.addLine(x_, y_, x_ + width_, y_, 1.0F, 0x334155FF);
  canvas.addLine(x_ + width_, y_, x_ + width_, y_ + height_, 1.0F, 0x334155FF);
  canvas.addLine(x_ + width_, y_ + height_, x_, y_ + height_, 1.0F, 0x334155FF);
  canvas.addLine(x_, y_ + height_, x_, y_, 1.0F, 0x334155FF);

  const float topY = y_ + height_;
  canvas.addText(state, x_ + 8.0F, topY - 14.0F, "CLASP ASSEMBLY BENCH",
                 0x94A3B8FF, 0);

  // 2. Left Homestead Drop Slot (Cyan aura)
  canvas.setTag(render::tagKindOverlay, kTagClaspLeftDrop);
  canvas.addRect(leftX_, leftY_, leftW_, leftH_, 0x081E2CDD);
  canvas.addLine(leftX_, leftY_, leftX_ + leftW_, leftY_, 1.5F, 0x06B6D4FF);
  canvas.addLine(leftX_ + leftW_, leftY_, leftX_ + leftW_, leftY_ + leftH_,
                 1.5F, 0x06B6D4FF);
  canvas.addLine(leftX_ + leftW_, leftY_ + leftH_, leftX_, leftY_ + leftH_,
                 1.5F, 0x06B6D4FF);
  canvas.addLine(leftX_, leftY_ + leftH_, leftX_, leftY_, 1.5F, 0x06B6D4FF);

  canvas.addText(state, leftX_ + 4.0F, leftY_ + leftH_ - 14.0F, "HOMESTEAD",
                 0x06B6D4FF, 0);
  const std::string leftCountStr =
      std::to_string(leftSpans_.size()) + (leftSpans_.size() == 1 ? " Span" : " Spans");
  canvas.addText(state, leftX_ + 4.0F, leftY_ + 14.0F, leftCountStr, 0xE2E8F0FF,
                 0);

  if (!leftSpans_.empty()) {
    canvas.setTag(render::tagKindOverlay, kTagClaspClearLeft);
    canvas.addRect(leftX_ + leftW_ - 16.0F, leftY_ + leftH_ - 16.0F, 14.0F,
                   14.0F, 0xDC2626CC);
    canvas.addText(state, leftX_ + leftW_ - 13.0F, leftY_ + leftH_ - 4.0F, "x",
                   0xFFFFFFFF, 0);
  }

  // 3. Right Toward Drop Slot (Magenta aura)
  canvas.setTag(render::tagKindOverlay, kTagClaspRightDrop);
  canvas.addRect(rightX_, rightY_, rightW_, rightH_, 0x2A0824DD);
  canvas.addLine(rightX_, rightY_, rightX_ + rightW_, rightY_, 1.5F,
                 0xEC4899FF);
  canvas.addLine(rightX_ + rightW_, rightY_, rightX_ + rightW_,
                 rightY_ + rightH_, 1.5F, 0xEC4899FF);
  canvas.addLine(rightX_ + rightW_, rightY_ + rightH_, rightX_,
                 rightY_ + rightH_, 1.5F, 0xEC4899FF);
  canvas.addLine(rightX_, rightY_ + rightH_, rightX_, rightY_, 1.5F,
                 0xEC4899FF);

  canvas.addText(state, rightX_ + 4.0F, rightY_ + rightH_ - 14.0F,
                 "TOWARD", 0xEC4899FF, 0);
  const std::string rightCountStr =
      std::to_string(rightSpans_.size()) +
      (rightSpans_.size() == 1 ? " Span" : " Spans");
  canvas.addText(state, rightX_ + 4.0F, rightY_ + 14.0F, rightCountStr,
                 0xE2E8F0FF, 0);

  if (!rightSpans_.empty()) {
    canvas.setTag(render::tagKindOverlay, kTagClaspClearRight);
    canvas.addRect(rightX_ + rightW_ - 16.0F, rightY_ + rightH_ - 16.0F, 14.0F,
                   14.0F, 0xDC2626CC);
    canvas.addText(state, rightX_ + rightW_ - 13.0F, rightY_ + rightH_ - 4.0F,
                   "x", 0xFFFFFFFF, 0);
  }

  // 4. Center Relation Nexus
  const float nexusX = leftX_ + leftW_ + 4.0F;
  const float nexusW = rightX_ - nexusX - 4.0F;

  // Type Button
  const float typeBtnY = leftY_ + leftH_ - 18.0F;
  canvas.setTag(render::tagKindOverlay, kTagClaspTypeSelector);
  canvas.addRect(nexusX, typeBtnY, nexusW, 18.0F, 0x334155EE);
  const std::string typeLabel =
      "[" + std::string(linkTypeName(selectedType_)) + " v]";
  canvas.addText(state, nexusX + 4.0F, typeBtnY + 14.0F, typeLabel,
                 0xF8FAFCFF, 0);

  // Tier Button
  const float tierBtnY = typeBtnY - 20.0F;
  canvas.setTag(render::tagKindOverlay, kTagClaspTierSelector);
  canvas.addRect(nexusX, tierBtnY, nexusW, 18.0F, 0x1E293BEE);
  const std::string tierLabel =
      "Tier: " + std::string(prominenceTierName(selectedTier_));
  canvas.addText(state, nexusX + 4.0F, tierBtnY + 14.0F, tierLabel,
                 0xCBD5E1FF, 0);

  // Forge Button
  const float forgeBtnY = leftY_;
  canvas.setTag(render::tagKindOverlay, kTagClaspForgeButton);
  const std::uint32_t forgeCol = canForge() ? 0x059669FF : 0x33415588;
  canvas.addRect(nexusX, forgeBtnY, nexusW, 20.0F, forgeCol);
  const std::string forgeText = canForge() ? "FORGE CLASP !" : "EMPTY SLOTS";
  canvas.addText(state, nexusX + 6.0F, forgeBtnY + 15.0F, forgeText,
                 canForge() ? 0xFFFFFFFF : 0x94A3B8FF, 0);
}

} // namespace xudu
