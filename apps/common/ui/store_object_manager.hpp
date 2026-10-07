/**
 * @file store_object_manager.hpp
 * @brief Interactive Store Object Manager drawer for multi-open and creation.
 */
#ifndef COMMON_UI_STORE_OBJECT_MANAGER_HPP
#define COMMON_UI_STORE_OBJECT_MANAGER_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/ui/focus_manager.hpp>
#include <gleditor/ui/overlay.hpp>

#include "common/xanadu/ops.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"

namespace xanadu {

/**
 * @class StoreObjectManager
 * @brief Interactive drawer overlay allowing users to discover, open
 *        simultaneously, unload, and author documents and slices.
 */
class StoreObjectManager : public gleditor::FrameContributor,
                           public gleditor::PickObserver,
                           public gleditor::ui::FocusScope,
                           public gleditor::a11y::Source {
public:
  void cancel() override {
    keyPressed(gleditor::Key::Escape, gleditor::KeyMods::None);
  }

  bool keyPressed(const gleditor::ui::KeyEvent &event) override {
    return keyPressed(event.key, event.mods);
  }

  [[nodiscard]] bool active() const override { return visible_.load(); }
  bool keyPressed(gleditor::Key, gleditor::KeyMods);
  void textTyped(std::string_view) override {}
  bool pointerPick(const render::PickingResult &pick,
                   RenderState &state) override {
    return picked(pick, state);
  }
  std::shared_ptr<const gleditor::ui::LayoutResult>
  focusLayout() const override;
  void focusedNodeChanged(std::uint32_t) override;
  bool activateNode(std::uint32_t) override;
  bool pointerEvent(const gleditor::ui::PointerEvent &) override;
  void focusChanged(bool) override;
  std::optional<gleditor::InputArea> pointerArea() const override;

  static constexpr std::uint32_t kTagCloseDrawer      = 26001U;
  static constexpr std::uint32_t kTagNewSlice         = 26002U;
  static constexpr std::uint32_t kTagNewXanadoc       = 26003U;
  static constexpr std::uint32_t kTagItemCheckboxBase = 26100U;
  static constexpr std::uint32_t kTagItemCloseBase    = 27100U;

  struct ObjectItem {
    std::uint32_t birthOp{0};
    StructureKind kind{StructureKind::Cell};
    std::string name;
    bool isOpen{false};
    float yTop{0.0F};
    float yBottom{0.0F};
  };

  using ToggleCallback  = std::function<void(
      std::uint32_t birthOp, StructureKind kind, bool shouldBeOpen)>;
  using CreateCallback  = std::function<void(StructureKind kind)>;
  using CloseCallback   = std::function<void(std::uint32_t birthOp)>;
  using IsOpenPredicate = std::function<bool(std::uint32_t birthOp)>;

  StoreObjectManager(Store &store, std::string fontName = {},
                     ToggleCallback onToggle = nullptr,
                     CreateCallback onCreate = nullptr,
                     CloseCallback onClose   = nullptr,
                     IsOpenPredicate isOpen  = nullptr);
  ~StoreObjectManager() override;

  StoreObjectManager(const StoreObjectManager &)            = delete;
  StoreObjectManager &operator=(const StoreObjectManager &) = delete;
  StoreObjectManager(StoreObjectManager &&)                 = delete;
  StoreObjectManager &operator=(StoreObjectManager &&)      = delete;

  // FrameContributor
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &documentPipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override;

  // PickObserver
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

  // a11y::Source
  void describe(gleditor::a11y::Builder &into) override;
  [[nodiscard]] std::uint64_t accessibilityRevision() const override {
    return a11yRevision_.load() + overlay_.accessibilityRevision();
  }

  bool performAction(std::uint64_t, gleditor::a11y::Action,
                     std::string_view) override;
  StoreObjectManager *setConfig(const StorePanelConfig &);
  [[nodiscard]] std::shared_ptr<const gleditor::ui::WidgetScene>
  prepare(const gleditor::ui::UiMetrics &, const gleditor::ui::Theme &);
  [[nodiscard]] gleditor::text::ShapingCache::Stats shapingStats() const {
    return overlay_.shapingStats();
  }
  StoreObjectManager *setVisible(bool visible);
  StoreObjectManager *toggle();
  [[nodiscard]] bool isVisible() const noexcept { return visible_.load(); }

  StoreObjectManager *refresh();
  [[nodiscard]] const std::vector<ObjectItem> &items() const noexcept {
    return items_;
  }

  StoreObjectManager *setOnToggle(ToggleCallback cb) {
    const std::scoped_lock lock(guard_);
    onToggle_ = std::move(cb);
    return this;
  }
  StoreObjectManager *setOnCreate(CreateCallback cb) {
    const std::scoped_lock lock(guard_);
    onCreate_ = std::move(cb);
    return this;
  }
  StoreObjectManager *setOnClose(CloseCallback cb) {
    const std::scoped_lock lock(guard_);
    onClose_ = std::move(cb);
    return this;
  }
  StoreObjectManager *setIsOpenPredicate(IsOpenPredicate pred) {
    const std::scoped_lock lock(guard_);
    isOpen_ = std::move(pred);
    return this;
  }

  StoreObjectManager *createSlice();
  StoreObjectManager *createXanadoc();
  StoreObjectManager *toggleItem(std::size_t index);
  StoreObjectManager *closeItem(std::size_t index);

private:
  Store &store_;
  std::string fontName_;
  gleditor::ui::ScreenOverlay overlay_;
  mutable std::recursive_mutex guard_;
  std::atomic<bool> visible_{false};
  std::atomic<std::uint64_t> a11yRevision_{1};
  StorePanelConfig config_;
  gleditor::ui::UiMetrics metrics_;
  gleditor::ui::Theme theme_, sourceTheme_;
  gleditor::text::ShapingCache measurements_;
  std::size_t observedOps_{};
  bool dirty_{true};
  float scrollPx_{}, rowHeight_{}, viewportHeight_{};
  std::uint64_t generation_{1};
  enum class ActionKind {
    CloseDrawer,
    CreateSlice,
    CreateXanadoc,
    Toggle,
    Close
  };
  struct Action {
    ActionKind kind;
    std::uint32_t birth{};
    bool open{};
    std::uint64_t generation{};
  };
  struct Identity {
    gleditor::ui::WidgetId toggle{}, close{};
    bool open{};
  };
  gleditor::ui::WidgetId nextId_{1024}, closeId_{}, sliceId_{}, docId_{};
  std::unordered_map<std::uint32_t, Identity> identities_;
  std::unordered_map<gleditor::ui::WidgetId, Action> actions_;
  std::vector<Action> pending_, toggleActions_, closeActions_;
  void queue(const gleditor::ui::WidgetAction &);
  void drain();
  void scroll(float);
  void changed();

  ToggleCallback onToggle_;
  CreateCallback onCreate_;
  CloseCallback onClose_;
  IsOpenPredicate isOpen_;

  std::vector<ObjectItem> items_;
};

} // namespace xanadu

#endif // COMMON_UI_STORE_OBJECT_MANAGER_HPP
