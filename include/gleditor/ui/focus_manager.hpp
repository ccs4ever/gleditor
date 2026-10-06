#ifndef GLEDITOR_UI_FOCUS_MANAGER_HPP
#define GLEDITOR_UI_FOCUS_MANAGER_HPP

#include <atomic>
#include <gleditor/ui/input_event.hpp>
#include <memory>
#include <string_view>
#include <vector>

namespace gleditor::ui {
enum class OutsidePointer : std::uint8_t {
  Block,
  Dismiss,
  DismissAndPassThrough
};
struct ScopePolicy {
  bool modal{true};
  OutsidePointer outside{OutsidePointer::Block};
  std::vector<std::string> allowedCommands;
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
  virtual bool pointerEvent(const PointerEvent &) { return false; }
  virtual void cancel() { deactivate(); }
  virtual void focusChanged(bool) {}
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
  std::atomic<bool> active_{false};
  std::atomic<std::uint64_t> opened_{0};
};

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
  [[nodiscard]] std::optional<InputArea> textArea();
  bool dispatch(const InputEvent &);
  bool dispatchKey(const KeyEvent &);
  bool dispatchText(std::string_view);
  bool dispatchPointer(const PointerEvent &);
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
