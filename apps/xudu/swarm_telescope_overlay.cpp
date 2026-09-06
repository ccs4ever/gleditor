/**
 * @file swarm_telescope_overlay.cpp
 * @brief Implementation of Swarm Telescope discovery overlay with FTS5 search.
 */
#include "swarm_telescope_overlay.hpp"

#include <glm/ext/matrix_clip_space.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace xudu {

namespace {

std::string formatBytes(const std::uint64_t bytes) {
  if (bytes < 1024ULL) {
    return std::to_string(bytes) + " B";
  }
  if (bytes < 1024ULL * 1024ULL) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1)
       << (static_cast<double>(bytes) / 1024.0) << " KB";
    return ss.str();
  }
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(1)
     << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MB";
  return ss.str();
}

} // namespace

SwarmTelescopeOverlay::SwarmTelescopeOverlay(SwarmCatalog &catalog,
                                             RendererRef renderer,
                                             std::string fontName)
    : catalog_(catalog), renderer_(std::move(renderer)),
      fontName_(std::move(fontName)) {
  refreshSearch();
}

SwarmTelescopeOverlay::~SwarmTelescopeOverlay() = default;

void SwarmTelescopeOverlay::deviceReady(render::RenderDevice &device,
                                        const render::PipelineDesc &pipeline) {
  canvas_ = std::make_unique<gleditor::Canvas>(&device, fontName_);
  canvas_->createPipeline(pipeline, false);
}

bool SwarmTelescopeOverlay::busy() const { return false; }

void SwarmTelescopeOverlay::setVisible(const bool visible) {
  visible_ = visible;
  if (visible_) {
    refreshSearch();
  }
}

void SwarmTelescopeOverlay::toggle() { setVisible(!visible_); }

bool SwarmTelescopeOverlay::isVisible() const noexcept { return visible_; }

void SwarmTelescopeOverlay::setSearchQuery(std::string_view query) {
  searchQuery_ = query;
  refreshSearch();
}

std::string SwarmTelescopeOverlay::searchQuery() const { return searchQuery_; }

void SwarmTelescopeOverlay::selectCategory(const CatalogCategory cat) {
  activeCategory_ = cat;
  refreshSearch();
}

void SwarmTelescopeOverlay::selectItem(const std::size_t index) {
  if (index < currentResults_.size()) {
    selectedResultIndex_ = index;
  }
}

void SwarmTelescopeOverlay::refreshSearch() {
  currentResults_ = catalog_.search(searchQuery_, std::nullopt, 20);
  if (selectedResultIndex_ >= currentResults_.size()) {
    selectedResultIndex_ =
        currentResults_.empty() ? 0 : currentResults_.size() - 1;
  }
}

void SwarmTelescopeOverlay::drawFrame(gleditor::FrameContext &ctx) {
  if (!visible_ || !canvas_) {
    return;
  }

  const auto screenW = static_cast<float>(ctx.screenWidth);
  const auto screenH = static_cast<float>(ctx.screenHeight);
  const auto ortho   = glm::ortho(0.0F, screenW, 0.0F, screenH, -1.0F, 1.0F);

  width_            = std::min(860.0F, screenW - 40.0F);
  height_           = std::min(560.0F, screenH - 50.0F);
  const float deckX = (screenW - width_) * 0.5F;
  const float deckY = (screenH - height_) * 0.5F;
  const float topY  = deckY + height_;

  canvas_->clear();

  // 1. Semi-transparent dark cosmic backdrop
  canvas_->setTag(render::tagKindOverlay, 0);
  canvas_->addRect(0.0F, 0.0F, screenW, screenH, 0x00000088); // Dim background

  // 2. Telescope Deck Box
  canvas_->addRect(deckX, deckY, width_, height_, 0x0B0F19F5);
  canvas_->addLine(deckX, topY, deckX + width_, topY, 2.0F,
                   0x38BDF8FF); // Cyan top edge
  canvas_->addLine(deckX, deckY, deckX + width_, deckY, 1.0F, 0x1E293BFF);
  canvas_->addLine(deckX, deckY, deckX, topY, 1.0F, 0x1E293BFF);
  canvas_->addLine(deckX + width_, deckY, deckX + width_, topY, 1.0F,
                   0x1E293BFF);

  // 3. Header Bar
  canvas_->addText(ctx.state, deckX + 16.0F, topY - 18.0F,
                   "DOCUVERSE SWARM TELESCOPE", 0x38BDF8FF, 0);
  canvas_->addText(ctx.state, deckX + 275.0F, topY - 18.0F,
                   "| BEP 46 Author Catalogs & DHT Topic Swarms", 0x94A3B8CC,
                   0);

  // Close [Esc X] Button
  canvas_->setTag(render::tagKindOverlay, kTagTelescopeClose);
  canvas_->addRect(deckX + width_ - 75.0F, topY - 34.0F, 65.0F, 24.0F,
                   0xDC2626CC);
  canvas_->addText(ctx.state, deckX + width_ - 66.0F, topY - 20.0F, "Esc  x",
                   0xFFFFFFFF, 0);

  // 4. Search Input Bar
  const float searchY = topY - 78.0F;
  const float searchW = width_ - 32.0F;
  canvas_->setTag(render::tagKindOverlay, kTagSearchBar);
  canvas_->addRect(deckX + 16.0F, searchY, searchW, 34.0F, 0x1E293BEE);
  canvas_->addLine(deckX + 16.0F, searchY, deckX + 16.0F + searchW, searchY,
                   1.0F, 0x38BDF888);

  std::string searchDisplay = "Search:  " + searchQuery_ + "_";
  canvas_->addText(ctx.state, deckX + 26.0F, searchY + 11.0F, searchDisplay,
                   0xF1F5F9FF, 0);

  // 5. Category Tabs
  const float tabY = searchY - 34.0F;
  float curTabX    = deckX + 16.0F;

  auto drawTab = [&](const char *label, const CatalogCategory cat,
                     const std::uint32_t tag) {
    const bool active = (activeCategory_ == cat);
    const float tabW  = 130.0F;
    canvas_->setTag(render::tagKindOverlay, tag);
    canvas_->addRect(curTabX, tabY, tabW, 26.0F,
                     active ? 0x0284C7DD : 0x1E293B88);
    canvas_->addText(ctx.state, curTabX + 10.0F, tabY + 7.0F, label,
                     active ? 0xFFFFFFFF : 0x94A3B8CC, 0);
    curTabX += tabW + 8.0F;
  };

  drawTab("Topic Swarms", CatalogCategory::TopicSwarms, kTagTabTopics);
  drawTab("Followed Authors", CatalogCategory::FollowedAuthors, kTagTabAuthors);
  drawTab("Recent Local", CatalogCategory::RecentLocal, kTagTabRecent);

  // 6. Content Columns Setup
  const float contentTopY = tabY - 12.0F;
  const float col1W       = 180.0F;
  const float col2W       = 330.0F;
  const float col3W       = width_ - col1W - col2W - 48.0F;

  const float col1X = deckX + 16.0F;
  const float col2X = col1X + col1W + 8.0F;
  const float col3X = col2X + col2W + 8.0F;

  // Vertical column dividers
  canvas_->setTag(render::tagKindOverlay, 0);
  canvas_->addLine(col2X - 4.0F, deckY + 16.0F, col2X - 4.0F, contentTopY, 1.0F,
                   0x1E293B88);
  canvas_->addLine(col3X - 4.0F, deckY + 16.0F, col3X - 4.0F, contentTopY, 1.0F,
                   0x1E293B88);

  // -------------------------------------------------------------
  // COLUMN 1: Categories / Topics Nav
  // -------------------------------------------------------------
  canvas_->addText(ctx.state, col1X, contentTopY - 14.0F, "SWARM CHANNELS",
                   0x38BDF8CC, 0);

  float navY = contentTopY - 42.0F;
  if (activeCategory_ == CatalogCategory::TopicSwarms) {
    const auto topics = catalog_.topicSwarms();
    for (std::size_t idx = 0; idx < topics.size() && navY > deckY + 20.0F;
         ++idx) {
      const auto &top = topics[idx];
      canvas_->setTag(render::tagKindOverlay,
                      kTagCategoryBase + static_cast<std::uint32_t>(idx));
      canvas_->addRect(col1X, navY, col1W, 26.0F, 0x1E293B66);
      canvas_->addText(ctx.state, col1X + 8.0F, navY + 7.0F, "#" + top.topic,
                       0x38BDF8FF, 0);

      const std::string countStr = std::to_string(top.activePeers);
      canvas_->addText(ctx.state, col1X + col1W - 30.0F, navY + 7.0F, countStr,
                       0x64748BCC, 0);
      navY -= 30.0F;
    }
  } else if (activeCategory_ == CatalogCategory::FollowedAuthors) {
    const auto authors = catalog_.followedAuthors();
    for (std::size_t idx = 0; idx < authors.size() && navY > deckY + 20.0F;
         ++idx) {
      const auto &auth = authors[idx];
      canvas_->setTag(render::tagKindOverlay,
                      kTagCategoryBase + static_cast<std::uint32_t>(idx));
      canvas_->addRect(col1X, navY, col1W, 26.0F, 0x1E293B66);
      canvas_->addText(ctx.state, col1X + 8.0F, navY + 7.0F, auth.name,
                       0xF1F5F9FF, 0);

      const std::string nodeStr = std::to_string(auth.seederNodes);
      canvas_->addText(ctx.state, col1X + col1W - 25.0F, navY + 7.0F, nodeStr,
                       0x10B981CC, 0);
      navY -= 30.0F;
    }
  } else {
    canvas_->addText(ctx.state, col1X + 8.0F, navY + 7.0F, "• Philosophy_Notes",
                     0x94A3B8CC, 0);
    navY -= 26.0F;
    canvas_->addText(ctx.state, col1X + 8.0F, navY + 7.0F, "• Xanadu_Spec_v2",
                     0x94A3B8CC, 0);
    navY -= 26.0F;
    canvas_->addText(ctx.state, col1X + 8.0F, navY + 7.0F, "• Quick_Brown_Fox",
                     0x94A3B8CC, 0);
  }

  // -------------------------------------------------------------
  // COLUMN 2: Matching Publication Cards (FTS5 Results)
  // -------------------------------------------------------------
  canvas_->addText(ctx.state, col2X, contentTopY - 14.0F,
                   "PUBLICATIONS (FTS5 BM25)", 0x38BDF8CC, 0);

  float cardY = contentTopY - 80.0F;
  for (std::size_t idx = 0;
       idx < currentResults_.size() && cardY > deckY + 20.0F; ++idx) {
    const auto &res     = currentResults_[idx];
    const bool isSelect = (idx == selectedResultIndex_);
    const float cardH   = 68.0F;

    canvas_->setTag(render::tagKindOverlay,
                    kTagPublicationBase + static_cast<std::uint32_t>(idx));
    canvas_->addRect(col2X, cardY, col2W, cardH,
                     isSelect ? 0x0369A1CC : 0x1E293B88);
    if (isSelect) {
      canvas_->addLine(col2X, cardY, col2X + col2W, cardY, 1.5F, 0x38BDF8FF);
    }

    // Title
    std::string titleTrunc = res.entry.title;
    if (titleTrunc.size() > 36) {
      titleTrunc = titleTrunc.substr(0, 33) + "...";
    }
    canvas_->addText(ctx.state, col2X + 8.0F, cardY + cardH - 18.0F, titleTrunc,
                     0xFFFFFFFF, 0);

    // Author & Seeders
    std::string metaStr = res.entry.authorName + " | " +
                          std::to_string(res.seederCount) + " seeds";
    canvas_->addText(ctx.state, col2X + 8.0F, cardY + cardH - 36.0F, metaStr,
                     0x94A3B8CC, 0);

    // Topic Tags
    std::string tagsStr;
    for (const auto &t : res.entry.topics) {
      tagsStr += "#" + t + " ";
    }
    canvas_->addText(ctx.state, col2X + 8.0F, cardY + cardH - 52.0F, tagsStr,
                     0x38BDF8AA, 0);

    // Merkle Verified Badge
    if (res.isVerified) {
      canvas_->addText(ctx.state, col2X + col2W - 85.0F, cardY + cardH - 36.0F,
                       "[✓ Merkle]", 0x10B981FF, 0);
    }

    cardY -= (cardH + 8.0F);
  }

  // -------------------------------------------------------------
  // COLUMN 3: Inspector Deck & Action Button
  // -------------------------------------------------------------
  canvas_->addText(ctx.state, col3X, contentTopY - 14.0F,
                   "INSPECTOR & PROVENANCE", 0x38BDF8CC, 0);

  if (selectedResultIndex_ < currentResults_.size()) {
    const auto &sel = currentResults_[selectedResultIndex_];
    float inspY     = contentTopY - 36.0F;

    // Title
    canvas_->addText(ctx.state, col3X, inspY, sel.entry.title, 0xFFD700FF, 0);
    inspY -= 22.0F;

    // Author
    canvas_->addText(ctx.state, col3X, inspY, "Author: " + sel.entry.authorName,
                     0xF1F5F9FF, 0);
    inspY -= 18.0F;

    // Fingerprint
    std::string fpShort = sel.entry.authorFingerprint;
    if (fpShort.size() > 20) {
      fpShort = fpShort.substr(0, 16) + "...";
    }
    canvas_->addText(ctx.state, col3X, inspY, "Key: " + fpShort, 0x94A3B8AA, 0);
    inspY -= 18.0F;

    // Metrics
    std::string sizeStr = "Size: " + formatBytes(sel.entry.totalBytes) + " (" +
                          std::to_string(sel.entry.microversions) +
                          " microversions)";
    canvas_->addText(ctx.state, col3X, inspY, sizeStr, 0x94A3B8AA, 0);
    inspY -= 18.0F;

    // Swarm Health
    std::string healthStr = "Health: " + std::to_string(sel.seederCount) +
                            " seeders | " + std::to_string(sel.peerCount) +
                            " peers";
    canvas_->addText(ctx.state, col3X, inspY, healthStr, 0x10B981CC, 0);
    inspY -= 22.0F;

    // Abstract / Description Box
    canvas_->addRect(col3X, inspY - 80.0F, col3W, 80.0F, 0x1E293B66);
    std::string abs1 = sel.entry.abstractText;
    if (abs1.size() > 48) {
      std::string abs2 = abs1.substr(48);
      abs1             = abs1.substr(0, 48);
      canvas_->addText(ctx.state, col3X + 6.0F, inspY - 16.0F, abs1, 0xCBD5E1FF,
                       0);
      if (abs2.size() > 48) {
        abs2 = abs2.substr(0, 45) + "...";
      }
      canvas_->addText(ctx.state, col3X + 6.0F, inspY - 34.0F, abs2, 0xCBD5E1FF,
                       0);
    } else {
      canvas_->addText(ctx.state, col3X + 6.0F, inspY - 16.0F, abs1, 0xCBD5E1FF,
                       0);
    }
    inspY -= 96.0F;

    // URI
    std::string uriShort = sel.entry.bep46Uri;
    if (uriShort.size() > 34) {
      uriShort = uriShort.substr(0, 31) + "...";
    }
    canvas_->addText(ctx.state, col3X, inspY, uriShort, 0x38BDF888, 0);
    inspY -= 36.0F;

    // Action Button: [ ✦ Summon into 3D Space (Enter) ]
    canvas_->setTag(render::tagKindOverlay, kTagSummonButton);
    canvas_->addRect(col3X, inspY - 10.0F, col3W, 36.0F, 0x059669EE);
    canvas_->addLine(col3X, inspY - 10.0F, col3X + col3W, inspY - 10.0F, 1.5F,
                     0x34D399FF);
    canvas_->addText(ctx.state, col3X + 22.0F, inspY + 5.0F,
                     "✦ Summon into 3D Space (Enter)", 0xFFFFFFFF, 0);
  }

  canvas_->commit();
  canvas_->draw(ctx.state, ortho);
}

bool SwarmTelescopeOverlay::picked(const render::PickingResult &pick,
                                   RenderState & /*state*/) {
  if (!visible_ || pick.tag.kind != render::tagKindOverlay) {
    return false;
  }

  const auto tag = pick.tag.clusterIndex;
  if (tag == kTagTelescopeClose) {
    setVisible(false);
    return true;
  }
  if (tag == kTagTabRecent) {
    selectCategory(CatalogCategory::RecentLocal);
    return true;
  }
  if (tag == kTagTabAuthors) {
    selectCategory(CatalogCategory::FollowedAuthors);
    return true;
  }
  if (tag == kTagTabTopics) {
    selectCategory(CatalogCategory::TopicSwarms);
    return true;
  }
  if (tag >= kTagPublicationBase && tag < kTagPublicationBase + 100) {
    const auto idx = static_cast<std::size_t>(tag - kTagPublicationBase);
    selectItem(idx);
    return true;
  }
  if (tag == kTagSummonButton) {
    if (selectedResultIndex_ < currentResults_.size() && onSummon_) {
      onSummon_(currentResults_[selectedResultIndex_].entry);
      setVisible(false);
    }
    return true;
  }

  return false;
}

} // namespace xudu
