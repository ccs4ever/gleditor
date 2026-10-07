#ifndef GLEDITOR_UI_FOCUS_MANAGER_HPP
#define GLEDITOR_UI_FOCUS_MANAGER_HPP

#include <atomic>
#include <gleditor/ui/input_event.hpp>
#include <gleditor/ui/layout.hpp>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace render {
struct PickingResult;
}
struct RenderState;

namespace gleditor::ui {
enum class OutsidePointer : std::uint8_t {
  Block,
  Dismiss,
  DismissAndPassThrough
};
enum class FocusTarget : std::uint8_t {
  FirstFocusable,
  DefaultAction,
  ExplicitNode
};
struct ScopePolicy {
  bool modal{true};
  OutsidePointer outside{OutsidePointer::Block};
  std::vector<std::string> allowedCommands;
  FocusTarget initial{FocusTarget::FirstFocusable};
  std::uint32_t initialNode{};
};
/// Registered scopes remain owned by the caller. Stop input dispatch before
/// destroying them, and reset registrations before derived state is torn down.
class FocusScope {
public:
  virtual ~FocusScope() = default;
  void activate();
  void deactivate();
  [[nodiscard]] virtual bool active() const;
  [[nodiscard]] std::uint64_t openedSequence() const;
  virtual bool keyPressed(const KeyEvent &) { return false; }
  virtual void textTyped(std::string_view) {}
  /// Retained screen controls handle press/release through pointerEvent.
  /// World-space scopes can opt into asynchronous GPU picking instead.
  [[nodiscard]] virtual bool usesGpuPointerPicking() const { return false; }
  /// GPU picks arrive on the render thread, separately from pointer capture.
  virtual bool pointerPick(const render::PickingResult &, RenderState &) {
    return false;
  }
  virtual bool pointerEvent(const PointerEvent &) { return false; }
  virtual void cancel() { deactivate(); }
  virtual void focusChanged(bool) {}
  [[nodiscard]] virtual std::shared_ptr<const LayoutResult>
  focusLayout() const {
    return {};
  }
  virtual void focusedNodeChanged(std::uint32_t) {}
  virtual void beforeFocusTraversal() {}
  virtual bool activateNode(std::uint32_t) { return false; }
  /// Picking and source-specific accessibility actions can request focus
  /// without retaining a pointer to the manager.
  void requestFocus(std::uint32_t node) {
    requestedNode_.store(static_cast<std::uint64_t>(node) + 1);
  }
  [[nodiscard]] virtual bool acceptsCommand(std::string_view) const {
    return false;
  }
  [[nodiscard]] virtual std::optional<InputArea> textArea() const {
    return std::nullopt;
  }
  [[nodiscard]] virtual std::optional<InputArea> pointerArea() const {
    return std::nullopt;
  }

private:
  friend class FocusManager;
  std::atomic<std::uint64_t> requestedNode_{0};
  std::atomic<bool> active_{false};
  std::atomic<std::uint64_t> opened_{0};
};

/// No raw scope pointer survives an asynchronous GPU readback.
struct PointerPickTarget {
  std::uint64_t registration{}, sequence{}, revision{};
  bool modal{};
  bool operator==(const PointerPickTarget &) const = default;
};
struct PointerDispatch {
  bool consumed{};
  std::optional<PointerPickTarget> gpuPick;
};

struct FocusSnapshot {
  FocusScope *scope{};
  bool modal{};
  std::optional<std::uint32_t> node;
  std::uint64_t revision{};
};
/// Shared traversal for managed scopes and the legacy Form entry point.
[[nodiscard]] std::optional<std::uint32_t>
nextFocusNode(std::span<const std::uint32_t> order,
              std::optional<std::uint32_t> current, bool reverse);

class FocusManager {
  struct State;

public:
  class ScopeHandle {
  public:
    ScopeHandle() = default;
    ~ScopeHandle();
    ScopeHandle(ScopeHandle &&) noexcept;
    ScopeHandle &operator=(ScopeHandle &&) noexcept;
    ScopeHandle(const ScopeHandle &)            = delete;
    ScopeHandle &operator=(const ScopeHandle &) = delete;
    void reset();

  private:
    friend class FocusManager;
    ScopeHandle(std::weak_ptr<State>, std::uint64_t);
    std::weak_ptr<State> state_;
    std::uint64_t id_{};
  };
  using Registration = ScopeHandle;
  FocusManager();
  ~FocusManager();
  FocusManager(const FocusManager &)            = delete;
  FocusManager &operator=(const FocusManager &) = delete;
  /// Polls active() without holding the manager lock. A registration may
  /// outlive this manager; destroying its handle then is harmless.
  [[nodiscard]] ScopeHandle registerScope(FocusScope &, ScopePolicy = {});
  [[nodiscard]] ScopeHandle push(FocusScope &, ScopePolicy = {});
  [[nodiscard]] ScopeHandle addPane(FocusScope &);
  [[nodiscard]] FocusScope *focusedScope();
  [[nodiscard]] bool modalActive();
  [[nodiscard]] FocusSnapshot focusSnapshot();
  [[nodiscard]] std::optional<std::uint32_t> focusedNode();
  bool focusNode(std::uint32_t);
  [[nodiscard]] std::uint64_t focusRevision();
  [[nodiscard]] std::optional<InputArea> textArea();
  bool dispatch(const InputEvent &);
  bool dispatchKey(const KeyEvent &);
  bool dispatchText(std::string_view);
  bool dispatchPointer(const PointerEvent &);
  [[nodiscard]] PointerDispatch dispatchPointerWithPick(const PointerEvent &);
  [[nodiscard]] PointerPickTarget pointerPickTarget();
  [[nodiscard]] bool acceptsPointerPick(const PointerPickTarget &);
  /// Invalidate delayed clicks when the rendered document membership changes.
  void invalidatePointerPicks();
  bool dispatchPointerPick(const PointerPickTarget &,
                           const render::PickingResult &, RenderState &);
  [[nodiscard]] bool permitsCommand(std::string_view);
  void setGlobalCommandAllowList(std::vector<std::string>);
  void focusLost();
  void windowFocusLost() { focusLost(); }
  bool cyclePane(bool reverse = false);
  [[nodiscard]] KeyMods modifiers() const;

private:
  std::shared_ptr<State> state_;
  ScopeHandle insert(FocusScope &, ScopePolicy, bool);
};
} // namespace gleditor::ui
#endif
