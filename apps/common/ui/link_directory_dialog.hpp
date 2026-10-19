/**
 * @file link_directory_dialog.hpp
 * @brief Expanded Link Directory dialog and 30% docked sidebar drawer (§4.2).
 *
 * Provides a document-wide overview of all authored and publication links with:
 * - Full-text passage search across both endsets.
 * - Faceted filter pills: Type (All, Quotation, Comment, Disagreement,
 * Citation, Transclusion), Author/Owner, and Endset balance (1:1, 1:N, N:M).
 * - Matching links list with status indicators (In View, Companion Cell,
 * Distant, Unresolved).
 * - Detailed Link Inspector with dual endset previews, occurrence sites, and
 * collapsible primedia disclosure (spans and cell addresses).
 * - Dual display modes: 780x540 modal dialog and 30% dockable sidebar drawer.
 */
#ifndef COMMON_UI_LINK_DIRECTORY_DIALOG_HPP
#define COMMON_UI_LINK_DIRECTORY_DIALOG_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/ui/focus_manager.hpp>
#include <gleditor/ui/overlay.hpp>

#include "common/ui/xanadoc/link_context.hpp"
#include "common/xanadu/link_navigation.hpp"
#include "common/xanadu/link_occurrences.hpp"
#include "common/xanadu/link_panel.hpp"
#include "common/xanadu/system_docs.hpp"

namespace xanadu::ui {

enum class DirectoryDisplayMode : unsigned char {
  Modal,
  DockedDrawer,
};

enum class BalanceFilter : unsigned char {
  All,
  OneToOne,
  OneToMany,
  ManyToMany,
};

struct DirectoryLinkItem {
  LinkKey key;
  LinkType type{LinkType::Quotation};
  std::string author;
  std::string leftSummary;
  std::string rightSummary;
  std::size_t leftMembers{1};
  std::size_t rightMembers{1};
  std::size_t leftOccurrences{0};
  std::size_t rightOccurrences{0};
  bool inView{false};
  bool companionCell{false};
};

class LinkDirectoryDialog : public gleditor::FrameContributor,
                            public gleditor::PickObserver,
                            public gleditor::ui::FocusScope,
                            public gleditor::a11y::Source {
public:
  LinkDirectoryDialog(LinkContext &context, std::string fontName);
  ~LinkDirectoryDialog() override;

  LinkDirectoryDialog(const LinkDirectoryDialog &)            = delete;
  LinkDirectoryDialog &operator=(const LinkDirectoryDialog &) = delete;
  LinkDirectoryDialog(LinkDirectoryDialog &&)                 = delete;
  LinkDirectoryDialog &operator=(LinkDirectoryDialog &&)      = delete;

  // -- Configuration & Mode ---------------------------------------------------
  LinkDirectoryDialog *setVisible(bool visible);
  LinkDirectoryDialog *toggle() { return setVisible(!visible_.load()); }
  [[nodiscard]] bool isVisible() const noexcept { return visible_.load(); }

  LinkDirectoryDialog *setDisplayMode(DirectoryDisplayMode mode);
  [[nodiscard]] DirectoryDisplayMode displayMode() const noexcept {
    return mode_;
  }

  LinkDirectoryDialog *setSearchQuery(std::string query);
  LinkDirectoryDialog *setTypeFilter(std::optional<LinkType> filter);
  LinkDirectoryDialog *setAuthorFilter(std::string author);
  LinkDirectoryDialog *setBalanceFilter(BalanceFilter filter);
  LinkDirectoryDialog *setAdvancedPrimediaOpen(bool open);

  // Refresh candidate links from the active session.
  void refresh();

  // -- FrameContributor -------------------------------------------------------
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &documentPipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override;

  // -- PickObserver -----------------------------------------------------------
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

  // -- FocusScope -------------------------------------------------------------
  void cancel() override { setVisible(false); }
  bool keyPressed(const gleditor::ui::KeyEvent &event) override {
    return keyPressed(event.key, event.mods);
  }
  bool keyPressed(gleditor::Key key, gleditor::KeyMods mods);
  void textTyped(std::string_view text) override;
  bool pointerPick(const render::PickingResult &pick,
                   RenderState &state) override {
    return picked(pick, state);
  }
  std::shared_ptr<const gleditor::ui::LayoutResult>
  focusLayout() const override;
  void focusedNodeChanged(std::uint32_t) override {}
  bool activateNode(std::uint32_t) override { return false; }
  void focusChanged(bool) override {}
  bool pointerEvent(const gleditor::ui::PointerEvent &) override;
  std::optional<gleditor::InputArea> pointerArea() const override;

  // -- a11y::Source -----------------------------------------------------------
  void describe(gleditor::a11y::Builder &into) override;
  [[nodiscard]] std::uint64_t accessibilityRevision() const override {
    return revision_.load() + overlay_.accessibilityRevision();
  }

  // Tags for picking and controls
  static constexpr std::uint32_t kTagCloseButton    = 28001U;
  static constexpr std::uint32_t kTagModeToggle     = 28002U;
  static constexpr std::uint32_t kTagSearchField    = 28003U;
  static constexpr std::uint32_t kTagClearSearch    = 28004U;
  static constexpr std::uint32_t kTagFilterTypeAll  = 28010U;
  static constexpr std::uint32_t kTagFilterTypeBase = 28011U; // + LinkType
  static constexpr std::uint32_t kTagFilterBalance  = 28020U;
  static constexpr std::uint32_t kTagAdvancedToggle = 28030U;
  static constexpr std::uint32_t kTagSelectInBar    = 28040U;
  static constexpr std::uint32_t kTagFollowLink     = 28041U;
  static constexpr std::uint32_t kTagJumpEndpoint   = 28042U;
  static constexpr std::uint32_t kTagItemListBase   = 28100U;

private:
  void rebuildModel();
  void filterLinks();
  void refreshLocked();
  void queueAction(std::uint32_t tag);
  void dispatchActions();

  LinkContext &context_;
  std::string fontName_;
  std::atomic<bool> visible_{false};
  std::atomic<std::uint64_t> revision_{1};
  DirectoryDisplayMode mode_{DirectoryDisplayMode::Modal};

  mutable std::mutex guard_;
  std::string searchQuery_;
  std::optional<LinkType> typeFilter_;
  std::string authorFilter_;
  BalanceFilter balanceFilter_{BalanceFilter::All};
  bool advancedPrimediaOpen_{false};

  std::vector<DirectoryLinkItem> allLinks_;
  std::vector<DirectoryLinkItem> filteredLinks_;
  std::size_t selectedIndex_{0};

  gleditor::ui::ScreenOverlay overlay_{gleditor::ui::Widget{}};
  gleditor::ui::UiMetrics metrics_;
  gleditor::ui::Theme theme_;
  gleditor::text::ShapingCache cache_;
  std::unique_ptr<gleditor::Canvas> canvas_;

  float panelX_{0.0F};
  float panelY_{0.0F};
  float panelW_{0.0F};
  float panelH_{0.0F};

  std::vector<std::uint32_t> pendingTags_;
};

} // namespace xanadu::ui

#endif // COMMON_UI_LINK_DIRECTORY_DIALOG_HPP
