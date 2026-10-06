/**
 * @file store_object_manager.hpp
 * @brief Interactive Store Object Manager drawer for multi-open and creation.
 */
#ifndef COMMON_UI_STORE_OBJECT_MANAGER_HPP
#define COMMON_UI_STORE_OBJECT_MANAGER_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/modal_input.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/renderer.hpp>

#include "common/xanadu/ops.hpp"
#include "common/xanadu/store.hpp"

namespace xanadu {

/**
 * @class StoreObjectManager
 * @brief Interactive drawer overlay allowing users to discover, open
 *        simultaneously, unload, and author documents and slices.
 */
class StoreObjectManager : public gleditor::FrameContributor,
                           public gleditor::PickObserver,
                           public gleditor::ModalInput,
                           public gleditor::a11y::Source {
public:
  [[nodiscard]] bool grabbing() const override { return visible_; }
  bool keyPressed(gleditor::Key key, gleditor::KeyMods) override {
    if (key != gleditor::Key::Escape || !grabbing()) return false;
    setVisible(false);
    return true;
  }
  void textTyped(const std::string &) override {}
  bool pointerPick(const render::PickingResult &pick,
                   RenderState &state) override {
    return picked(pick, state);
  }

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

  StoreObjectManager(Store &store, std::string fontName = "Sans 10",
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
  [[nodiscard]] bool busy() const override { return false; }

  // PickObserver
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

  // a11y::Source
  void describe(gleditor::a11y::Builder &into) override;
  [[nodiscard]] std::uint64_t accessibilityRevision() const override {
    return a11yRevision_;
  }

  StoreObjectManager *setVisible(bool visible);
  StoreObjectManager *toggle();
  [[nodiscard]] bool isVisible() const noexcept { return visible_; }

  StoreObjectManager *refresh();
  [[nodiscard]] const std::vector<ObjectItem> &items() const noexcept {
    return items_;
  }

  StoreObjectManager *setOnToggle(ToggleCallback cb) {
    onToggle_ = std::move(cb);
    return this;
  }
  StoreObjectManager *setOnCreate(CreateCallback cb) {
    onCreate_ = std::move(cb);
    return this;
  }
  StoreObjectManager *setOnClose(CloseCallback cb) {
    onClose_ = std::move(cb);
    return this;
  }
  StoreObjectManager *setIsOpenPredicate(IsOpenPredicate pred) {
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
  std::unique_ptr<gleditor::Canvas> canvas_;
  bool visible_{false};
  std::uint64_t a11yRevision_{1};

  ToggleCallback onToggle_;
  CreateCallback onCreate_;
  CloseCallback onClose_;
  IsOpenPredicate isOpen_;

  std::vector<ObjectItem> items_;
};

} // namespace xanadu

#endif // COMMON_UI_STORE_OBJECT_MANAGER_HPP
