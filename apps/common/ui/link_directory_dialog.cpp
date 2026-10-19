/**
 * @file link_directory_dialog.cpp
 * @brief Expanded Link Directory dialog and 30% docked sidebar drawer
 * implementation.
 */
#include "link_directory_dialog.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <glm/gtc/matrix_transform.hpp>

#include <gleditor/logging.hpp>
#include <gleditor/text/font.hpp>
#include <gleditor/ui/button_metrics.hpp>
#include <gleditor/ui/recessed_well.hpp>

namespace xanadu::ui {

LinkDirectoryDialog::LinkDirectoryDialog(LinkContext &context,
                                         std::string fontName)
    : context_(context), fontName_(std::move(fontName)) {}

LinkDirectoryDialog::~LinkDirectoryDialog() = default;

LinkDirectoryDialog *LinkDirectoryDialog::setVisible(const bool visible) {
  if (visible_.exchange(visible) != visible) {
    if (visible) {
      refresh();
    }
    ++revision_;
  }
  return this;
}

LinkDirectoryDialog *
LinkDirectoryDialog::setDisplayMode(const DirectoryDisplayMode mode) {
  const std::scoped_lock lock(guard_);
  if (mode_ != mode) {
    mode_ = mode;
    ++revision_;
  }
  return this;
}

LinkDirectoryDialog *LinkDirectoryDialog::setSearchQuery(std::string query) {
  const std::scoped_lock lock(guard_);
  if (searchQuery_ != query) {
    searchQuery_ = std::move(query);
    filterLinks();
    ++revision_;
  }
  return this;
}

LinkDirectoryDialog *
LinkDirectoryDialog::setTypeFilter(std::optional<LinkType> filter) {
  const std::scoped_lock lock(guard_);
  if (typeFilter_ != filter) {
    typeFilter_ = filter;
    filterLinks();
    ++revision_;
  }
  return this;
}

LinkDirectoryDialog *LinkDirectoryDialog::setAuthorFilter(std::string author) {
  const std::scoped_lock lock(guard_);
  if (authorFilter_ != author) {
    authorFilter_ = std::move(author);
    filterLinks();
    ++revision_;
  }
  return this;
}

LinkDirectoryDialog *
LinkDirectoryDialog::setBalanceFilter(const BalanceFilter filter) {
  const std::scoped_lock lock(guard_);
  if (balanceFilter_ != filter) {
    balanceFilter_ = filter;
    filterLinks();
    ++revision_;
  }
  return this;
}

LinkDirectoryDialog *
LinkDirectoryDialog::setAdvancedPrimediaOpen(const bool open) {
  const std::scoped_lock lock(guard_);
  if (advancedPrimediaOpen_ != open) {
    advancedPrimediaOpen_ = open;
    ++revision_;
  }
  return this;
}

void LinkDirectoryDialog::refresh() {
  const std::scoped_lock lock(guard_);
  refreshLocked();
}

void LinkDirectoryDialog::refreshLocked() {
  allLinks_.clear();

  // Populate from active link context candidates or active selection
  if (const auto selected = context_.selection()) {
    DirectoryLinkItem item;
    item.key = selected->key;
    if (selected->occurrences) {
      item.type         = selected->occurrences->link.type;
      item.author       = selected->occurrences->link.owner;
      item.leftMembers  = selected->occurrences->link.left.size();
      item.rightMembers = selected->occurrences->link.right.size();
      item.inView       = true;
      for (const auto &m : selected->occurrences->left) {
        item.leftOccurrences += m.occurrences.size();
      }
      for (const auto &m : selected->occurrences->right) {
        item.rightOccurrences += m.occurrences.size();
      }
      std::string leftText;
      for (const auto &span : selected->occurrences->link.left) {
        leftText += context_.sessionRef().store().userPermascroll().read(span);
        if (leftText.size() > 60) break;
      }
      std::string rightText;
      for (const auto &span : selected->occurrences->link.right) {
        rightText += context_.sessionRef().store().userPermascroll().read(span);
        if (rightText.size() > 60) break;
      }
      if (!leftText.empty()) {
        std::ranges::replace(leftText, '\n', ' ');
        item.leftSummary = leftText.substr(0, 50);
      } else {
        item.leftSummary = std::format("{} member{}", item.leftMembers,
                                       item.leftMembers == 1 ? "" : "s");
      }
      if (!rightText.empty()) {
        std::ranges::replace(rightText, '\n', ' ');
        item.rightSummary = rightText.substr(0, 50);
      } else {
        item.rightSummary = std::format("{} member{}", item.rightMembers,
                                        item.rightMembers == 1 ? "" : "s");
      }
    } else {
      item.leftSummary  = "Left Endset Passage";
      item.rightSummary = "Right Endset Passage";
    }
    allLinks_.push_back(std::move(item));
  }

  // Populate from all open stores in the session
  for (std::size_t i = 0; i < context_.sessionRef().storeCount(); ++i) {
    const auto &store = context_.sessionRef().store(i);
    for (const auto &[id, link] : store.links()) {
      if (link.type == LinkType::Format) continue;
      LinkKey key{.authority = store.documentId(), .id = id};
      if (std::ranges::any_of(allLinks_, [&](const auto &existing) {
            return existing.key == key;
          })) {
        continue;
      }
      DirectoryLinkItem item;
      item.key    = key;
      item.type   = link.type;
      item.author = link.owner.empty() ? store.documentId().str() : link.owner;
      item.leftMembers  = link.left.size();
      item.rightMembers = link.right.size();

      std::string leftText;
      for (const auto &span : link.left) {
        leftText += store.userPermascroll().read(span);
        if (leftText.size() > 60) break;
      }
      std::string rightText;
      for (const auto &span : link.right) {
        rightText += store.userPermascroll().read(span);
        if (rightText.size() > 60) break;
      }
      if (!leftText.empty()) {
        std::ranges::replace(leftText, '\n', ' ');
        item.leftSummary = leftText.substr(0, 50);
      } else {
        item.leftSummary = std::format("{} member{}", item.leftMembers,
                                       item.leftMembers == 1 ? "" : "s");
      }
      if (!rightText.empty()) {
        std::ranges::replace(rightText, '\n', ' ');
        item.rightSummary = rightText.substr(0, 50);
      } else {
        item.rightSummary = std::format("{} member{}", item.rightMembers,
                                        item.rightMembers == 1 ? "" : "s");
      }

      if (auto resolved = context_.resolveForPresentation(key);
          resolved.has_value()) {
        for (const auto &m : resolved->left) {
          item.leftOccurrences += m.occurrences.size();
        }
        for (const auto &m : resolved->right) {
          item.rightOccurrences += m.occurrences.size();
        }
        item.inView = (item.leftOccurrences > 0 || item.rightOccurrences > 0);
      } else {
        item.leftOccurrences  = link.left.size();
        item.rightOccurrences = link.right.size();
        item.inView           = false;
      }
      allLinks_.push_back(std::move(item));
    }
  }

  // Also query reader link packages
  const auto packages = context_.packagePresentation(-1);
  for (const auto &pkgOcc : packages) {
    if (std::ranges::any_of(allLinks_, [&](const auto &existing) {
          return existing.key == pkgOcc.key;
        })) {
      continue;
    }
    DirectoryLinkItem item;
    item.key              = pkgOcc.key;
    item.type             = pkgOcc.link.type;
    item.author           = pkgOcc.link.owner;
    item.leftMembers      = pkgOcc.left.size();
    item.rightMembers     = pkgOcc.right.size();
    item.leftOccurrences  = 0;
    item.rightOccurrences = 0;
    for (const auto &m : pkgOcc.left) {
      item.leftOccurrences += m.occurrences.size();
    }
    for (const auto &m : pkgOcc.right) {
      item.rightOccurrences += m.occurrences.size();
    }
    item.inView = (item.leftOccurrences > 0 || item.rightOccurrences > 0);
    item.companionCell = false;
    item.leftSummary   = std::format("{} members", item.leftMembers);
    item.rightSummary  = std::format("{} members", item.rightMembers);
    allLinks_.push_back(std::move(item));
  }

  filterLinks();
}

void LinkDirectoryDialog::filterLinks() {
  filteredLinks_.clear();
  std::string lowerQuery = searchQuery_;
  std::ranges::transform(lowerQuery, lowerQuery.begin(),
                         [](unsigned char c) { return std::tolower(c); });

  for (const auto &link : allLinks_) {
    // Type filter
    if (typeFilter_.has_value() && link.type != *typeFilter_) {
      continue;
    }
    // Author filter
    if (!authorFilter_.empty() &&
        link.author.find(authorFilter_) == std::string::npos) {
      continue;
    }
    // Balance filter
    if (balanceFilter_ == BalanceFilter::OneToOne) {
      if (link.leftMembers != 1 || link.rightMembers != 1) continue;
    } else if (balanceFilter_ == BalanceFilter::OneToMany) {
      if (!(link.leftMembers == 1 && link.rightMembers > 1) &&
          !(link.rightMembers == 1 && link.leftMembers > 1)) {
        continue;
      }
    } else if (balanceFilter_ == BalanceFilter::ManyToMany) {
      if (link.leftMembers <= 1 || link.rightMembers <= 1) continue;
    }
    // Search query match
    if (!lowerQuery.empty()) {
      std::string lSummary = link.leftSummary;
      std::string rSummary = link.rightSummary;
      std::string author   = link.author;
      std::ranges::transform(lSummary, lSummary.begin(),
                             [](unsigned char c) { return std::tolower(c); });
      std::ranges::transform(rSummary, rSummary.begin(),
                             [](unsigned char c) { return std::tolower(c); });
      std::ranges::transform(author, author.begin(),
                             [](unsigned char c) { return std::tolower(c); });

      const bool matched =
          lSummary.find(lowerQuery) != std::string::npos ||
          rSummary.find(lowerQuery) != std::string::npos ||
          author.find(lowerQuery) != std::string::npos ||
          std::to_string(link.key.id).find(lowerQuery) != std::string::npos;
      if (!matched) continue;
    }

    filteredLinks_.push_back(link);
  }

  if (selectedIndex_ >= filteredLinks_.size() && !filteredLinks_.empty()) {
    selectedIndex_ = filteredLinks_.size() - 1;
  }
}

void LinkDirectoryDialog::deviceReady(
    render::RenderDevice &device,
    const render::PipelineDesc &documentPipeline) {
  const std::scoped_lock lock(guard_);
  overlay_.deviceReady(device, documentPipeline);
  canvas_ = std::make_unique<gleditor::Canvas>(
      &device, gleditor::ui::scaledFontDescription(
                   fontName_, gleditor::ui::FontRole::Body, {},
                   gleditor::ui::defaultTheme()));
  canvas_->createPipeline(documentPipeline, false);
}

void LinkDirectoryDialog::drawFrame(gleditor::FrameContext &ctx) {
  const std::scoped_lock lock(guard_);
  dispatchActions();
  if (!visible_.load() || !canvas_) {
    return;
  }

  metrics_        = ctx.metrics;
  theme_          = ctx.theme;
  const auto safe = metrics_.pixelSafeArea();

  if (mode_ == DirectoryDisplayMode::Modal) {
    panelW_ = std::floor(std::min(780.0F, safe.width * 0.90F));
    panelH_ = std::floor(std::min(540.0F, safe.height * 0.85F));
    panelX_ = std::floor(safe.left + (safe.width - panelW_) * 0.5F);
    panelY_ = std::floor(safe.bottom + (safe.height - panelH_) * 0.5F);
  } else {
    // 30% Docked Drawer
    panelW_ = std::floor(std::max(300.0F, safe.width * 0.30F));
    panelH_ = safe.height;
    panelX_ = safe.left + safe.width - panelW_;
    panelY_ = safe.bottom;
  }

  if (allLinks_.empty()) {
    refreshLocked();
  }

  rebuildModel();

  canvas_->clear();
  canvas_->setTag(render::tagKindOverlay, 0);

  // Surface background with P1 micro-radius
  const auto surfaceCol = gleditor::ui::rgba(theme_.colours.surface);
  const float radius    = theme_.buttonMetrics.radiusMicroPx;
  canvas_->addRoundedRect(panelX_, panelY_, panelW_, panelH_, radius,
                          surfaceCol);

  // 1px architectural hairline border
  const std::uint32_t borderCol = 0x38BDF844; // Subtle Cyan border
  canvas_->addLine(panelX_, panelY_, panelX_ + panelW_, panelY_, 1.0F,
                   borderCol);
  canvas_->addLine(panelX_, panelY_ + panelH_, panelX_ + panelW_,
                   panelY_ + panelH_, 1.0F, borderCol);
  canvas_->addLine(panelX_, panelY_, panelX_, panelY_ + panelH_, 1.0F,
                   borderCol);
  canvas_->addLine(panelX_ + panelW_, panelY_, panelX_ + panelW_,
                   panelY_ + panelH_, 1.0F, borderCol);

  canvas_->commit();
  const auto ortho =
      glm::ortho(0.F, static_cast<float>(ctx.screenWidth), 0.F,
                 static_cast<float>(ctx.screenHeight), -1.F, 1.F);
  canvas_->draw(ctx.state, ortho);

  gleditor::FrameContext draw{
      ctx.state,         ctx.viewProjection, ctx.screenWidth,
      ctx.screenHeight,  ctx.timeline,       ctx.chrome,
      ctx.settledChrome, metrics_,           theme_};
  overlay_.drawFrame(draw);
}

void LinkDirectoryDialog::rebuildModel() {
  gleditor::ui::Widget root{
      .id        = 1,
      .model     = gleditor::ui::PositionedPanel{},
      .preferred = {metrics_.logical(panelW_), metrics_.logical(panelH_)}};
  auto &positioned = std::get<gleditor::ui::PositionedPanel>(root.model);

  auto addWidget = [&](gleditor::ui::Widget w, gleditor::ui::Rect bounds) {
    positioned.childBounds.push_back({metrics_.logical(bounds.left - panelX_),
                                      metrics_.logical(bounds.bottom - panelY_),
                                      metrics_.logical(bounds.width),
                                      metrics_.logical(bounds.height)});
    w.preferred = {metrics_.logical(bounds.width),
                   metrics_.logical(bounds.height)};
    root.children.push_back(std::move(w));
  };

  const float pad  = 10.0F;
  const float topY = panelY_ + panelH_ - 36.0F;

  // Header Title
  addWidget({.id    = 10,
             .model = gleditor::ui::Label{"LINK DIRECTORY",
                                          gleditor::ui::TextPurpose::Title}},
            {panelX_ + pad, topY, 200.0F, 28.0F});

  // Mode Toggle Button
  const std::string modeLabel =
      (mode_ == DirectoryDisplayMode::Modal) ? "Dock 30% [|]" : "Modal [^]";
  addWidget(
      {.id = kTagModeToggle, .model = gleditor::ui::Button{modeLabel, "mode"}},
      {panelX_ + panelW_ - 180.0F, topY, 110.0F, 26.0F});

  // Close Button
  addWidget(
      {.id = kTagCloseButton, .model = gleditor::ui::Button{"X", "close"}},
      {panelX_ + panelW_ - 45.0F, topY, 35.0F, 26.0F});

  // Search Field
  const float searchY = topY - 34.0F;
  const std::string searchDisplay =
      searchQuery_.empty() ? "Search passage text..." : searchQuery_;
  addWidget({.id    = kTagSearchField,
             .model = gleditor::ui::Label{"[?] " + searchDisplay,
                                          gleditor::ui::TextPurpose::Label}},
            {panelX_ + pad, searchY, panelW_ - pad * 2 - 60.0F, 26.0F});

  if (!searchQuery_.empty()) {
    addWidget({.id    = kTagClearSearch,
               .model = gleditor::ui::Button{"Clear", "clear"}},
              {panelX_ + panelW_ - 60.0F, searchY, 50.0F, 26.0F});
  }

  // Filter Pills Row
  const float filterY = searchY - 32.0F;
  float pillX         = panelX_ + pad;
  const auto addPill  = [&](std::uint32_t tag, std::string label,
                            bool selected) {
    const float pillW = 75.0F;
    addWidget({.id    = tag,
               .model = gleditor::ui::Button{std::move(label), "filter", true},
               .tone  = selected ? gleditor::ui::Tone::Accent
                                 : gleditor::ui::Tone::Normal},
              {pillX, filterY, pillW, 24.0F});
    pillX += pillW + 6.0F;
  };

  addPill(kTagFilterTypeAll, "All", !typeFilter_.has_value());
  addPill(kTagFilterTypeBase + static_cast<std::uint32_t>(LinkType::Quotation),
          "Quote", typeFilter_ == LinkType::Quotation);
  addPill(kTagFilterTypeBase + static_cast<std::uint32_t>(LinkType::Comment),
          "Comment", typeFilter_ == LinkType::Comment);
  addPill(kTagFilterTypeBase +
              static_cast<std::uint32_t>(LinkType::Disagreement),
          "Dispute", typeFilter_ == LinkType::Disagreement);

  // Content Split Area: Left (Matching Links List), Right (Link Detail
  // Inspector)
  [[maybe_unused]] const float bodyY = panelY_ + 44.0F;
  [[maybe_unused]] const float bodyH = std::max(60.0F, filterY - bodyY - 8.0F);

  if (mode_ == DirectoryDisplayMode::Modal) {
    const float listW   = std::floor(panelW_ * 0.45F);
    const float detailW = panelW_ - listW - pad * 3;

    // Matching Links List
    float rowY = filterY - 32.0F;
    for (std::size_t i = 0; i < filteredLinks_.size() && i < 8; ++i) {
      const auto &item      = filteredLinks_[i];
      const bool isSelected = (i == selectedIndex_);
      const std::string rowLabel =
          std::format("> Link {} * {}", item.key.id, linkTypeName(item.type));
      addWidget({.id    = static_cast<std::uint32_t>(kTagItemListBase + i),
                 .model = gleditor::ui::Button{rowLabel, "select_row", true},
                 .tone  = isSelected ? gleditor::ui::Tone::Accent
                                     : gleditor::ui::Tone::Normal},
                {panelX_ + pad, rowY, listW, 28.0F});
      rowY -= 32.0F;
    }

    // Detail Inspector Pane (Right Side)
    const float detailX = panelX_ + listW + pad * 2;
    if (selectedIndex_ < filteredLinks_.size()) {
      const auto &sel = filteredLinks_[selectedIndex_];
      addWidget(
          {.id    = 50,
           .model = gleditor::ui::Label{std::format("LINK DETAIL (Link {})",
                                                    sel.key.id),
                                        gleditor::ui::TextPurpose::Title}},
          {detailX, filterY - 28.0F, detailW, 24.0F});

      addWidget({.id = 51,
                 .model =
                     gleditor::ui::Label{
                         std::format("LEFT ENDSET ({} Members):\n* {}",
                                     sel.leftMembers, sel.leftSummary),
                         gleditor::ui::TextPurpose::Description}},
                {detailX, filterY - 74.0F, detailW, 40.0F});

      addWidget({.id = 52,
                 .model =
                     gleditor::ui::Label{
                         std::format("RIGHT ENDSET ({} Members):\n* {}",
                                     sel.rightMembers, sel.rightSummary),
                         gleditor::ui::TextPurpose::Description}},
                {detailX, filterY - 124.0F, detailW, 40.0F});

      // Advanced Primedia Disclosure Toggle
      const std::string advLabel =
          advancedPrimediaOpen_ ? "[-] Advanced Primedia Spans"
                                : "[+] Advanced Primedia Spans (Collapsed)";
      addWidget({.id    = kTagAdvancedToggle,
                 .model = gleditor::ui::Button{advLabel, "toggle_primedia"}},
                {detailX, filterY - 164.0F, detailW, 26.0F});

      if (advancedPrimediaOpen_) {
        addWidget({.id = 53,
                   .model =
                       gleditor::ui::Label{
                           std::format("Authority: {}\nOp Index: {}",
                                       sel.key.authority.str(), sel.key.id),
                           gleditor::ui::TextPurpose::Description},
                   .fontRole = gleditor::ui::FontRole::Mono},
                  {detailX, filterY - 210.0F, detailW, 40.0F});
      }
    }
  } else {
    // Docked Drawer: Stacked compact list
    float rowY = filterY - 30.0F;
    for (std::size_t i = 0; i < filteredLinks_.size() && i < 12; ++i) {
      const auto &item      = filteredLinks_[i];
      const bool isSelected = (i == selectedIndex_);
      const std::string rowLabel =
          std::format("Link {} [{}]", item.key.id, valenceBadge(item.type));
      addWidget({.id    = static_cast<std::uint32_t>(kTagItemListBase + i),
                 .model = gleditor::ui::Button{rowLabel, "select_row", true},
                 .tone  = isSelected ? gleditor::ui::Tone::Accent
                                     : gleditor::ui::Tone::Normal},
                {panelX_ + pad, rowY, panelW_ - pad * 2, 26.0F});
      rowY -= 28.0F;
    }
  }

  // Bottom Action Bar
  const float bottomY = panelY_ + 8.0F;
  addWidget({.id    = kTagSelectInBar,
             .model = gleditor::ui::Button{"Select in Bar", "select_bar"}},
            {panelX_ + pad, bottomY, 110.0F, 30.0F});
  addWidget(
      {.id    = kTagFollowLink,
       .model = gleditor::ui::Button{"Follow Link \u23CE", "follow_link"}},
      {panelX_ + pad + 120.0F, bottomY, 120.0F, 30.0F});
  addWidget(
      {.id    = kTagJumpEndpoint,
       .model = gleditor::ui::Button{"Jump to Endpoint", "jump_endpoint"}},
      {panelX_ + pad + 250.0F, bottomY, 130.0F, 30.0F});

  overlay_.setModel(root);
  overlay_.setBounds(gleditor::ui::Rect{panelX_, panelY_, panelW_, panelH_});
  overlay_.setVisible(true);
  std::ignore = overlay_.prepare(metrics_, theme_);
}

void LinkDirectoryDialog::queueAction(const std::uint32_t tag) {
  pendingTags_.push_back(tag);
}

void LinkDirectoryDialog::dispatchActions() {
  auto tags = std::move(pendingTags_);
  pendingTags_.clear();

  for (const auto tag : tags) {
    if (tag == kTagCloseButton) {
      setVisible(false);
    } else if (tag == kTagModeToggle) {
      setDisplayMode(mode_ == DirectoryDisplayMode::Modal
                         ? DirectoryDisplayMode::DockedDrawer
                         : DirectoryDisplayMode::Modal);
    } else if (tag == kTagClearSearch) {
      setSearchQuery({});
    } else if (tag == kTagFilterTypeAll) {
      setTypeFilter(std::nullopt);
    } else if (tag >= kTagFilterTypeBase && tag < kTagFilterTypeBase + 10) {
      setTypeFilter(static_cast<LinkType>(tag - kTagFilterTypeBase));
    } else if (tag == kTagAdvancedToggle) {
      setAdvancedPrimediaOpen(!advancedPrimediaOpen_);
    } else if (tag >= kTagItemListBase &&
               tag < kTagItemListBase + filteredLinks_.size()) {
      selectedIndex_ = tag - kTagItemListBase;
      ++revision_;
    } else if (tag == kTagSelectInBar &&
               selectedIndex_ < filteredLinks_.size()) {
      std::ignore =
          context_.execute(nav::SelectLink{filteredLinks_[selectedIndex_].key});
    } else if (tag == kTagFollowLink &&
               selectedIndex_ < filteredLinks_.size()) {
      std::ignore =
          context_.execute(nav::SelectLink{filteredLinks_[selectedIndex_].key});
      std::ignore = context_.execute(nav::FollowLink{});
    } else if (tag == kTagJumpEndpoint &&
               selectedIndex_ < filteredLinks_.size()) {
      std::ignore =
          context_.execute(nav::SelectLink{filteredLinks_[selectedIndex_].key});
      std::ignore = context_.execute(nav::Enter{});
    }
  }
}

bool LinkDirectoryDialog::busy() const {
  const std::scoped_lock lock(guard_);
  return !pendingTags_.empty();
}

bool LinkDirectoryDialog::picked(const render::PickingResult &pick,
                                 RenderState &state) {
  const std::scoped_lock lock(guard_);
  if (!visible_.load()) {
    return false;
  }
  if (!overlay_.picked(pick, state)) {
    return false;
  }
  const auto scene = overlay_.snapshot();
  const auto id    = pick.overlayWidgetId ? pick.overlayWidgetId
                     : scene ? scene->resolvePickingId(pick.tag.clusterIndex)
                             : std::nullopt;
  if (!id) {
    return false;
  }
  queueAction(*id);
  return true;
}

bool LinkDirectoryDialog::keyPressed(
    const gleditor::Key key, [[maybe_unused]] const gleditor::KeyMods mods) {
  if (!visible_.load()) {
    return false;
  }
  if (key == gleditor::Key::Escape) {
    setVisible(false);
    return true;
  }
  if (key == gleditor::Key::Up) {
    const std::scoped_lock lock(guard_);
    if (selectedIndex_ > 0) {
      --selectedIndex_;
      ++revision_;
    }
    return true;
  }
  if (key == gleditor::Key::Down) {
    const std::scoped_lock lock(guard_);
    if (selectedIndex_ + 1 < filteredLinks_.size()) {
      ++selectedIndex_;
      ++revision_;
    }
    return true;
  }
  if (key == gleditor::Key::Return) {
    const std::scoped_lock lock(guard_);
    if (selectedIndex_ < filteredLinks_.size()) {
      queueAction(kTagSelectInBar);
      return true;
    }
  }
  if (key == gleditor::Key::Backspace) {
    const std::scoped_lock lock(guard_);
    if (!searchQuery_.empty()) {
      searchQuery_.pop_back();
      filterLinks();
      ++revision_;
      return true;
    }
  }
  return false;
}

void LinkDirectoryDialog::textTyped(const std::string_view text) {
  if (!visible_.load() || text.empty()) {
    return;
  }
  const std::scoped_lock lock(guard_);
  searchQuery_.append(text);
  filterLinks();
  ++revision_;
}

std::shared_ptr<const gleditor::ui::LayoutResult>
LinkDirectoryDialog::focusLayout() const {
  const auto scene = overlay_.snapshot();
  return scene ? std::make_shared<gleditor::ui::LayoutResult>(scene->layout)
               : nullptr;
}

bool LinkDirectoryDialog::pointerEvent(const gleditor::ui::PointerEvent &) {
  return visible_.load();
}

std::optional<gleditor::InputArea> LinkDirectoryDialog::pointerArea() const {
  if (!visible_.load()) {
    return std::nullopt;
  }
  return gleditor::InputArea{
      static_cast<int>(panelX_), static_cast<int>(panelY_),
      static_cast<int>(panelW_), static_cast<int>(panelH_)};
}

void LinkDirectoryDialog::describe(gleditor::a11y::Builder &into) {
  overlay_.describe(into);
}

} // namespace xanadu::ui
