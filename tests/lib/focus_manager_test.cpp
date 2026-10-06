#include <gtest/gtest.h>

#include <gleditor/app.hpp>
#include <gleditor/form.hpp>
#include <gleditor/ui/focus_manager.hpp>

#include <atomic>
#include <functional>
#include <latch>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {
using gleditor::InputArea;
using gleditor::Key;
using gleditor::KeyMods;
using namespace gleditor::ui;

struct Scope : FocusScope {
  std::vector<KeyEvent> keys;
  std::vector<PointerEvent> pointers;
  std::vector<bool> focus;
  std::string text;
  std::optional<InputArea> area;
  bool handlesPointer{};
  bool handlesKey{};
  bool acceptSave{};
  std::function<void(bool)> onFocus;

  bool keyPressed(const KeyEvent &event) override {
    keys.push_back(event);
    return handlesKey;
  }
  void textTyped(std::string_view value) override { text.append(value); }
  bool pointerEvent(const PointerEvent &event) override {
    pointers.push_back(event);
    return handlesPointer;
  }
  void focusChanged(bool value) override {
    focus.push_back(value);
    if (onFocus) onFocus(value);
  }
  std::optional<InputArea> textArea() const override { return area; }
  std::optional<InputArea> pointerArea() const override { return area; }
  bool acceptsCommand(std::string_view value) const override {
    return acceptSave && value == "save";
  }
};

TEST(FocusManagerTest, openingOrderOverridesRegistrationOrderAndReopeningWins) {
  FocusManager manager;
  Scope first, second;
  const auto firstRegistration  = manager.registerScope(first);
  const auto secondRegistration = manager.registerScope(second);
  second.activate();
  first.activate();
  EXPECT_EQ(manager.focusedScope(), &first);
  first.deactivate();
  EXPECT_EQ(manager.focusedScope(), &second);
  first.activate();
  EXPECT_EQ(manager.focusedScope(), &first);
  second.deactivate();
  second.activate();
  EXPECT_EQ(manager.focusedScope(), &second);
}

TEST(FocusManagerTest, unhandledModalInputNeverFallsThroughToThePane) {
  FocusManager manager;
  Scope pane, modal;
  pane.activate();
  const auto paneRegistration  = manager.addPane(pane);
  const auto modalRegistration = manager.push(modal);
  const auto chords =
      static_cast<KeyMods>(static_cast<unsigned>(KeyMods::Alt) |
                           static_cast<unsigned>(KeyMods::Shift));
  for (const auto &event :
       {KeyEvent{Key::Unknown, KeyMods::None, U'a'},
        KeyEvent{Key::Unknown, chords, U'n'}, KeyEvent{Key::Tab},
        KeyEvent{Key::F6}, KeyEvent{Key::Left}}) {
    EXPECT_TRUE(manager.dispatchKey(event));
  }
  EXPECT_TRUE(manager.dispatchText("letters\xE6\xBC\xA2"));
  EXPECT_TRUE(
      manager.dispatchPointer({.phase = PointerPhase::Wheel, .deltaY = 2.0F}));
  EXPECT_TRUE(manager.dispatchPointer(
      {.phase = PointerPhase::Press, .button = 1, .x = 900.0F}));
  EXPECT_TRUE(pane.keys.empty());
  EXPECT_TRUE(pane.text.empty());
  EXPECT_TRUE(pane.pointers.empty());
  EXPECT_EQ(modal.text, "letters\xE6\xBC\xA2");
}

TEST(FocusManagerTest, escapeCancelsAnUnhandledModalAndRestoresFocus) {
  FocusManager manager;
  Scope pane, modal;
  const auto paneHandle  = manager.addPane(pane);
  const auto modalHandle = manager.push(modal);
  EXPECT_TRUE(manager.dispatchKey({Key::Escape}));
  EXPECT_FALSE(manager.modalActive());
  EXPECT_EQ(manager.focusedScope(), &pane);
  EXPECT_TRUE(pane.keys.empty());
}

TEST(FocusManagerTest, outsideClickDismissesWithoutPassingIntoThePane) {
  FocusManager manager;
  Scope pane, modal;
  modal.area            = InputArea{10, 20, 30, 40};
  const auto paneHandle = manager.addPane(pane);
  const auto modalHandle =
      manager.push(modal, {.outside = OutsidePointer::Dismiss});
  EXPECT_TRUE(manager.dispatchPointer(
      {.phase = PointerPhase::Press, .button = 1, .x = 200.0F}));
  EXPECT_FALSE(manager.modalActive());
  EXPECT_EQ(manager.focusedScope(), &pane);
  EXPECT_TRUE(pane.pointers.empty());
}

TEST(FocusManagerTest, outsideClickIsBlockedByDefault) {
  FocusManager manager;
  Scope pane, modal;
  modal.area             = InputArea{10, 20, 30, 40};
  const auto paneHandle  = manager.addPane(pane);
  const auto modalHandle = manager.push(modal);
  EXPECT_TRUE(manager.dispatchPointer(
      {.phase = PointerPhase::Press, .button = 1, .x = 200.0F}));
  EXPECT_TRUE(manager.modalActive());
  EXPECT_EQ(manager.focusedScope(), &modal);
  EXPECT_TRUE(pane.pointers.empty());
  EXPECT_TRUE(modal.pointers.empty());
}

TEST(FocusManagerTest, closingNestedScopesRestoresThePaneAndItsImeRectangle) {
  FocusManager manager;
  Scope firstPane, secondPane, modal, nested;
  firstPane.area  = InputArea{1, 2, 30, 40};
  secondPane.area = InputArea{100, 200, 300, 400};
  modal.area      = InputArea{10, 20, 30, 40};
  nested.area     = InputArea{50, 60, 70, 80};
  firstPane.activate();
  secondPane.activate();
  const auto first  = manager.addPane(firstPane);
  const auto second = manager.addPane(secondPane);
  ASSERT_TRUE(manager.cyclePane());
  auto *const previousPane = manager.focusedScope();
  const auto previousArea  = manager.textArea();
  auto modalHandle         = manager.push(modal);
  EXPECT_EQ(manager.textArea(), modal.area);
  auto nestedHandle = manager.push(nested);
  EXPECT_EQ(manager.textArea(), nested.area);
  nestedHandle.reset();
  EXPECT_EQ(manager.focusedScope(), &modal);
  EXPECT_EQ(manager.textArea(), modal.area);
  modalHandle.reset();
  EXPECT_EQ(manager.focusedScope(), previousPane);
  EXPECT_EQ(manager.textArea(), previousArea);
}

TEST(FocusManagerTest, commandsRequireGlobalAndFocusedScopeConsent) {
  FocusManager manager;
  Scope modal;
  const auto handle = manager.push(modal);
  EXPECT_FALSE(manager.permitsCommand("save"));
  manager.setGlobalCommandAllowList({"save"});
  EXPECT_FALSE(manager.permitsCommand("save"));
  modal.acceptSave = true;
  EXPECT_TRUE(manager.permitsCommand("save"));
  EXPECT_FALSE(manager.permitsCommand("new-document"));
  manager.setGlobalCommandAllowList({});
  EXPECT_FALSE(manager.permitsCommand("save"));
}

TEST(FocusManagerTest, focusCallbacksCanQueryTheManagerWithoutDeadlocking) {
  FocusManager manager;
  Scope pane, modal;
  const auto paneHandle = manager.addPane(pane);
  modal.onFocus         = [&](bool gained) {
    if (gained) {
      EXPECT_EQ(manager.focusedScope(), &modal);
      EXPECT_TRUE(manager.modalActive());
      EXPECT_FALSE(manager.permitsCommand("new-document"));
    }
  };
  auto modalHandle = manager.push(modal);
  EXPECT_EQ(modal.focus, std::vector<bool>{true});
  modalHandle.reset();
  EXPECT_EQ(modal.focus, (std::vector<bool>{true, false}));
  EXPECT_EQ(manager.focusedScope(), &pane);
}

TEST(FocusManagerTest, aDragKeepsItsOwnerOutsideItsBoundsUntilRelease) {
  FocusManager manager;
  Scope original, newer;
  original.handlesPointer = true;
  auto originalHandle     = manager.push(original);
  EXPECT_TRUE(manager.dispatchPointer(
      {.phase = PointerPhase::Press, .button = 1, .pointerId = 7}));
  EXPECT_TRUE(manager.dispatchPointer(
      {.phase = PointerPhase::Move, .x = 500.0F, .pointerId = 7}));
  EXPECT_TRUE(manager.dispatchPointer(
      {.phase = PointerPhase::Release, .button = 1, .pointerId = 7}));
  ASSERT_EQ(original.pointers.size(), 3U);
  auto newerHandle = manager.push(newer);
  EXPECT_TRUE(newer.pointers.empty());
  EXPECT_TRUE(manager.dispatchPointer({.phase = PointerPhase::Wheel}));
  EXPECT_EQ(newer.pointers.size(), 1U);
}

TEST(FocusManagerTest, aNewModalCancelsThePreviousPointerOwner) {
  FocusManager manager;
  Scope original, newer;
  original.handlesPointer   = true;
  const auto originalHandle = manager.push(original);
  EXPECT_TRUE(manager.dispatchPointer(
      {.phase = PointerPhase::Press, .button = 1, .pointerId = 7}));
  const auto newerHandle = manager.push(newer);
  ASSERT_EQ(original.pointers.size(), 2U);
  EXPECT_EQ(original.pointers.back().phase, PointerPhase::Cancel);
  EXPECT_EQ(original.pointers.back().pointerId, 7U);
  EXPECT_TRUE(
      manager.dispatchPointer({.phase = PointerPhase::Move, .pointerId = 7}));
  EXPECT_EQ(newer.pointers.size(), 1U);
}

TEST(FocusManagerTest, removingAnOlderScopeDoesNotStealFocusFromANewerOne) {
  FocusManager manager;
  Scope pane, older, newer;
  const auto paneHandle = manager.addPane(pane);
  auto olderHandle      = manager.push(older);
  auto newerHandle      = manager.push(newer);
  olderHandle.reset();
  EXPECT_EQ(manager.focusedScope(), &newer);
  newerHandle.reset();
  EXPECT_EQ(manager.focusedScope(), &pane);
}

TEST(FocusManagerTest, f6CyclesPanesAndShiftF6GoesBackWithoutTyping) {
  FocusManager manager;
  Scope first, second;
  const auto firstHandle  = manager.addPane(first);
  const auto secondHandle = manager.addPane(second);
  EXPECT_EQ(manager.focusedScope(), &first);
  EXPECT_TRUE(manager.dispatchKey({Key::F6}));
  EXPECT_EQ(manager.focusedScope(), &second);
  EXPECT_TRUE(manager.dispatchKey({Key::F6, KeyMods::Shift}));
  EXPECT_EQ(manager.focusedScope(), &first);
  EXPECT_TRUE(first.keys.empty());
  EXPECT_TRUE(second.keys.empty());
}

TEST(FocusManagerTest, windowFocusLossCancelsCaptureAndResetsModifiers) {
  FocusManager manager;
  Scope modal;
  modal.handlesPointer = true;
  const auto handle    = manager.push(modal);
  EXPECT_TRUE(manager.dispatchKey({Key::Unknown, KeyMods::Alt, U'a'}));
  EXPECT_EQ(manager.modifiers(), KeyMods::Alt);
  EXPECT_TRUE(manager.dispatchPointer(
      {.phase = PointerPhase::Press, .button = 1, .pointerId = 3}));
  manager.windowFocusLost();
  EXPECT_EQ(manager.modifiers(), KeyMods::None);
  ASSERT_EQ(modal.pointers.size(), 2U);
  EXPECT_EQ(modal.pointers.back().phase, PointerPhase::Cancel);
}

TEST(FocusManagerTest, registrationLifetimeAndMovesDoNotLeaveDanglingFocus) {
  Scope scope;
  FocusManager::ScopeHandle surviving;
  {
    FocusManager manager;
    auto handle = manager.push(scope);
    surviving   = std::move(handle);
    EXPECT_EQ(manager.focusedScope(), &scope);
    handle.reset();
    EXPECT_EQ(manager.focusedScope(), &scope);
  }
  surviving.reset();
}

TEST(FocusManagerTest, commandGateCoversNamedAndBoundCommands) {
  gleditor::CommandTable table;
  int ran      = 0;
  bool allowed = false;
  table.bind(4, "save", "", [&] { ++ran; });
  table.setCommandGate([&](std::string_view) { return allowed; });
  EXPECT_FALSE(table.run("save"));
  EXPECT_FALSE(table.dispatch(4, gleditor::Mod::None));
  EXPECT_EQ(ran, 0);
  allowed = true;
  EXPECT_TRUE(table.run("save"));
  EXPECT_TRUE(table.dispatch(4, gleditor::Mod::None));
  EXPECT_EQ(ran, 2);
}

TEST(FocusManagerTest, aDeniedScopedCommandCannotFallThroughToAnotherBinding) {
  gleditor::CommandTable table;
  int ran = 0;
  table.bind(4, "background", "", [&] { ++ran; });
  table.bind(4, "modal", "", [&] { ++ran; });
  ASSERT_TRUE(table.setScope("modal", "dialog"));
  table.setScopeResolver([] { return "dialog"; });
  table.setCommandGate([](std::string_view id) { return id == "background"; });
  EXPECT_FALSE(table.dispatch(4, gleditor::Mod::None));
  EXPECT_EQ(ran, 0);
}
struct NodeScope : Scope {
  std::shared_ptr<LayoutResult> layout = std::make_shared<LayoutResult>();
  std::vector<std::uint32_t> nodeFocus, activations;
  bool handlesEditingArrow{};
  std::function<void(std::uint32_t)> onNode;
  NodeScope() {
    layout->bounds     = {0.0F, 0.0F, 300.0F, 100.0F};
    layout->boxes      = {{.id         = 10,
                           .rect       = {0.0F, 0.0F, 100.0F, 50.0F},
                           .focusable  = true,
                           .focusGroup = 1,
                           .textInput  = true},
                          {.id         = 20,
                           .rect       = {100.0F, 0.0F, 100.0F, 50.0F},
                           .focusable  = true,
                           .focusGroup = 1},
                          {.id            = 30,
                           .rect          = {200.0F, 0.0F, 100.0F, 50.0F},
                           .focusable     = true,
                           .focusGroup    = 2,
                           .defaultAction = true}};
    layout->focusOrder = {10, 20, 30};
  }
  std::shared_ptr<const LayoutResult> focusLayout() const override {
    return layout;
  }
  void focusedNodeChanged(std::uint32_t id) override {
    nodeFocus.push_back(id);
    if (onNode) onNode(id);
  }
  bool activateNode(std::uint32_t id) override {
    activations.push_back(id);
    return true;
  }
  bool keyPressed(const KeyEvent &event) override {
    keys.push_back(event);
    return handlesEditingArrow &&
           (event.key == Key::Left || event.key == Key::Right ||
            event.key == Key::Up || event.key == Key::Down);
  }
};
TEST(FocusTraversalTest, InitialTargetsUseValidatedLayoutNodes) {
  FocusManager manager;
  NodeScope scope;
  auto handle = manager.push(scope);
  EXPECT_EQ(manager.focusedNode(), 10U);
  EXPECT_EQ(scope.nodeFocus, (std::vector<std::uint32_t>{10}));
  handle.reset();
  handle = manager.push(scope, {.initial = FocusTarget::DefaultAction});
  EXPECT_EQ(manager.focusedNode(), 30U);
  handle.reset();
  handle = manager.push(
      scope, {.initial = FocusTarget::ExplicitNode, .initialNode = 20});
  EXPECT_EQ(manager.focusedNode(), 20U);
  handle.reset();
  handle = manager.push(
      scope, {.initial = FocusTarget::ExplicitNode, .initialNode = 999});
  EXPECT_EQ(manager.focusedNode(), 10U);
}
TEST(FocusTraversalTest, TabWrapsLayoutOrderAndSkipsDisabledNodes) {
  FocusManager manager;
  NodeScope scope;
  scope.layout->boxes[1].enabled = false;
  const auto handle              = manager.push(scope);
  EXPECT_TRUE(manager.dispatchKey({Key::Tab}));
  EXPECT_EQ(manager.focusedNode(), 30U);
  EXPECT_TRUE(manager.dispatchKey({Key::Tab}));
  EXPECT_EQ(manager.focusedNode(), 10U);
  EXPECT_TRUE(manager.dispatchKey({Key::Tab, KeyMods::Shift}));
  EXPECT_EQ(manager.focusedNode(), 30U);
  EXPECT_FALSE(manager.focusNode(20));
  EXPECT_FALSE(manager.focusNode(999));
}
TEST(FocusTraversalTest,
     EditingArrowsHavePriorityAndNavigationStaysWithinGroup) {
  FocusManager manager;
  NodeScope scope;
  const auto handle         = manager.push(scope);
  scope.handlesEditingArrow = true;
  EXPECT_TRUE(manager.dispatchKey({Key::Right}));
  EXPECT_EQ(manager.focusedNode(), 10U);
  scope.handlesEditingArrow = false;
  EXPECT_TRUE(manager.dispatchKey({Key::Right}));
  EXPECT_EQ(manager.focusedNode(), 20U);
  EXPECT_TRUE(manager.dispatchKey({Key::Right}));
  EXPECT_EQ(manager.focusedNode(), 10U);
  EXPECT_TRUE(manager.focusNode(30));
  EXPECT_TRUE(manager.dispatchKey({Key::Down}));
  EXPECT_EQ(manager.focusedNode(), 30U);
}
TEST(FocusTraversalTest, ReturnUsesDefaultActionAndEscapeRestoresNode) {
  FocusManager manager;
  NodeScope pane, modal;
  const auto paneHandle = manager.addPane(pane);
  EXPECT_TRUE(manager.focusNode(20));
  const auto modalHandle = manager.push(modal);
  EXPECT_TRUE(manager.dispatchKey({Key::Return}));
  EXPECT_EQ(modal.activations, (std::vector<std::uint32_t>{30}));
  EXPECT_TRUE(manager.dispatchKey({Key::Escape}));
  EXPECT_EQ(manager.focusedScope(), &pane);
  EXPECT_EQ(manager.focusedNode(), 20U);
}
TEST(FocusTraversalTest, NodeRequestsAreValidatedAndSnapshotRevisionIsStable) {
  FocusManager manager;
  NodeScope scope;
  const auto handle = manager.push(scope);
  const auto before = manager.focusSnapshot();
  EXPECT_EQ(before.scope, &scope);
  EXPECT_TRUE(before.modal);
  EXPECT_EQ(before.node, 10U);
  EXPECT_EQ(manager.focusRevision(), before.revision);
  scope.requestFocus(20);
  const auto after = manager.focusSnapshot();
  EXPECT_EQ(after.node, 20U);
  EXPECT_GT(after.revision, before.revision);
  EXPECT_EQ(manager.focusRevision(), after.revision);
  scope.requestFocus(999);
  EXPECT_EQ(manager.focusRevision(), after.revision);
  EXPECT_EQ(manager.focusedNode(), 20U);
}
TEST(FocusTraversalTest, NonTextNodesDisableImeAndSwallowComposedText) {
  FocusManager manager;
  NodeScope scope;
  scope.area        = InputArea{10, 20, 100, 50};
  const auto handle = manager.push(scope);
  EXPECT_EQ(manager.textArea(), scope.area);
  EXPECT_TRUE(manager.dispatchText("typed"));
  EXPECT_EQ(scope.text, "typed");
  EXPECT_TRUE(manager.focusNode(20));
  EXPECT_FALSE(manager.textArea());
  EXPECT_TRUE(manager.dispatchText("ignored"));
  EXPECT_EQ(scope.text, "typed");
}
TEST(FocusTraversalTest, FocusCallbacksCanRequestAnotherNodeReentrantly) {
  FocusManager manager;
  NodeScope scope;
  scope.onNode = [&](std::uint32_t id) {
    if (id == 10) scope.requestFocus(20);
  };
  const auto handle = manager.push(scope);
  EXPECT_EQ(manager.focusedNode(), 20U);
  EXPECT_EQ(scope.nodeFocus, (std::vector<std::uint32_t>{10, 20}));
}
TEST(FocusTraversalTest, LayoutChangesReplaceAnUnavailableFocusedNode) {
  FocusManager manager;
  NodeScope scope;
  const auto handle = manager.push(scope);
  EXPECT_TRUE(manager.focusNode(20));
  const auto before              = manager.focusRevision();
  scope.layout->boxes[1].enabled = false;
  EXPECT_EQ(manager.focusedNode(), 10U);
  EXPECT_GT(manager.focusRevision(), before);
  scope.layout->focusOrder.clear();
  EXPECT_FALSE(manager.focusedNode());
  EXPECT_FALSE(manager.textArea());
}
TEST(FocusTraversalTest, FormUsesSharedTraversalAndPreservesExpandedChoices) {
  FocusManager manager;
  gleditor::Form form("Sans 12");
  form.open("Fields", "",
            {{.label = "Name"},
             {.label   = "Choice",
              .kind    = gleditor::Form::Kind::Choice,
              .options = {"One", "Two"}},
             {.label = "Tail"}},
            [](const auto &) {});
  const auto handle = manager.registerScope(form);
  EXPECT_EQ(manager.focusedNode(), 16U);
  EXPECT_TRUE(manager.dispatchKey({Key::Tab}));
  EXPECT_EQ(form.focused(), 1U);
  EXPECT_EQ(manager.focusedNode(), 80U);
  EXPECT_TRUE(manager.dispatchKey({Key::Return}));
  EXPECT_TRUE(form.listOpen());
  EXPECT_TRUE(manager.dispatchKey({Key::Down}));
  EXPECT_EQ(manager.focusedNode(), 80U);
  EXPECT_TRUE(manager.dispatchKey({Key::Escape}));
  EXPECT_TRUE(form.isOpen());
  EXPECT_FALSE(form.listOpen());
  EXPECT_TRUE(manager.dispatchKey({Key::Return}));
  EXPECT_TRUE(manager.dispatchKey({Key::Down}));
  EXPECT_TRUE(manager.dispatchKey({Key::Tab}));
  EXPECT_EQ(manager.focusedNode(), 144U);
  EXPECT_EQ(form.current()[1].chosen, 1U);
  EXPECT_FALSE(form.listOpen());
  EXPECT_TRUE(manager.dispatchKey({Key::Up}));
  EXPECT_EQ(form.focused(), 1U);
  EXPECT_EQ(manager.focusedNode(), 80U);
}
TEST(FocusTraversalTest, OneFieldChoiceTabCommitsWithoutMovingToAnotherNode) {
  FocusManager manager;
  gleditor::Form form("Sans 12");
  form.open("Choice", "",
            {{.label   = "Pick",
              .kind    = gleditor::Form::Kind::Choice,
              .options = {"One", "Two"}}},
            [](const auto &) {});
  const auto handle = manager.registerScope(form);
  EXPECT_TRUE(manager.dispatchKey({Key::Return}));
  EXPECT_TRUE(manager.dispatchKey({Key::Down}));
  EXPECT_TRUE(manager.dispatchKey({Key::Tab}));
  EXPECT_FALSE(form.listOpen());
  EXPECT_EQ(form.current()[0].chosen, 1U);
  EXPECT_EQ(manager.focusedNode(), 16U);
}
TEST(FocusTraversalTest, SpaceOperatesNonTextControlsWithoutComposedText) {
  FocusManager manager;
  gleditor::Form form("Sans 12");
  form.open("Toggle", "",
            {{.label = "On", .kind = gleditor::Form::Kind::Toggle}},
            [](const auto &) {});
  const auto handle = manager.registerScope(form);
  EXPECT_TRUE(manager.dispatchKey({Key::Space}));
  EXPECT_TRUE(form.current()[0].on);
  EXPECT_TRUE(manager.dispatchText(" "));
  EXPECT_TRUE(form.current()[0].on);
}

TEST(FocusTraversalTest, HiddenDefaultActionCannotOverrideAVisibleFocusedNode) {
  FocusManager manager;
  NodeScope scope;
  scope.layout->focusOrder = {10, 20};
  const auto handle =
      manager.push(scope, {.initial = FocusTarget::DefaultAction});
  EXPECT_EQ(manager.focusedNode(), 10U);
  EXPECT_TRUE(manager.dispatchKey({Key::Return}));
  EXPECT_EQ(scope.activations, (std::vector<std::uint32_t>{10}));
}

TEST(FocusTraversalTest, ConcurrentLayoutReadCannotOverwriteNewerNodeFocus) {
  struct ConcurrentScope : NodeScope {
    mutable std::latch entered{1}, release{1};
    std::atomic<bool> block{false};
    mutable std::atomic<bool> paused{false};
    std::atomic<std::uint32_t> lastNode{0};
    const std::thread::id eventThread = std::this_thread::get_id();
    std::shared_ptr<const LayoutResult> focusLayout() const override {
      if (block.load() && std::this_thread::get_id() != eventThread &&
          !paused.exchange(true)) {
        entered.count_down();
        release.wait();
      }
      return layout;
    }
    void focusChanged(bool) override {}
    void focusedNodeChanged(std::uint32_t id) override { lastNode.store(id); }
  };
  FocusManager manager;
  ConcurrentScope scope;
  const auto handle = manager.push(scope);
  scope.block.store(true);
  FocusSnapshot read;
  std::jthread reader([&] { read = manager.focusSnapshot(); });
  scope.entered.wait();
  EXPECT_TRUE(manager.focusNode(20));
  scope.release.count_down();
  reader.join();
  EXPECT_EQ(read.node, 20U);
  EXPECT_EQ(manager.focusedNode(), 20U);
  EXPECT_EQ(scope.lastNode.load(), 20U);
}

TEST(FocusTraversalTest, RequestFromPreviousOpeningCannotChangeReopenedScope) {
  struct ReopeningScope : NodeScope {
    mutable std::latch entered{1}, release{1};
    std::atomic<bool> block{false};
    mutable std::atomic<unsigned> reads{0};
    std::atomic<std::uint32_t> lastNode{0};
    const std::thread::id eventThread = std::this_thread::get_id();
    std::shared_ptr<const LayoutResult> focusLayout() const override {
      if (block.load() && std::this_thread::get_id() != eventThread &&
          ++reads == 2) {
        entered.count_down();
        release.wait();
      }
      return layout;
    }
    void focusChanged(bool) override {}
    void focusedNodeChanged(std::uint32_t id) override { lastNode.store(id); }
  };
  FocusManager manager;
  ReopeningScope scope;
  scope.activate();
  const auto handle = manager.registerScope(scope);
  scope.block.store(true);
  bool accepted = true;
  std::jthread reader([&] { accepted = manager.focusNode(20); });
  scope.entered.wait();
  scope.deactivate();
  scope.activate();
  EXPECT_EQ(manager.focusedNode(), 10U);
  scope.release.count_down();
  reader.join();
  EXPECT_FALSE(accepted);
  EXPECT_EQ(manager.focusedNode(), 10U);
  EXPECT_EQ(scope.lastNode.load(), 10U);
}
TEST(FocusTraversalTest, DelayedNodeCallbackReappliesTheLatestNode) {
  struct DelayedScope : NodeScope {
    std::latch entered{1}, release{1};
    std::atomic<bool> paused{false};
    std::atomic<std::uint32_t> lastNode{0};
    void focusChanged(bool) override {}
    void focusedNodeChanged(std::uint32_t id) override {
      if (id == 20 && !paused.exchange(true)) {
        entered.count_down();
        release.wait();
      }
      lastNode.store(id);
    }
  };
  FocusManager manager;
  DelayedScope scope;
  const auto handle = manager.push(scope);
  bool accepted     = false;
  std::jthread earlier([&] { accepted = manager.focusNode(20); });
  scope.entered.wait();
  EXPECT_TRUE(manager.focusNode(30));
  EXPECT_EQ(scope.lastNode.load(), 30U);
  scope.release.count_down();
  earlier.join();
  EXPECT_TRUE(accepted);
  EXPECT_EQ(manager.focusedNode(), 30U);
  EXPECT_EQ(scope.lastNode.load(), 30U);
}
TEST(FocusTraversalTest, DelayedScopeGainDoesNotLeaveTheBackgroundFocused) {
  struct DelayedScope : FocusScope {
    std::latch entered{1}, release{1};
    std::atomic<bool> delay{false}, paused{false}, isFocused{false};
    void focusChanged(bool value) override {
      if (value && delay.load() && !paused.exchange(true)) {
        entered.count_down();
        release.wait();
      }
      isFocused.store(value);
    }
  };
  FocusManager manager;
  DelayedScope first, second;
  const auto firstHandle  = manager.registerScope(first);
  const auto secondHandle = manager.registerScope(second);
  first.delay.store(true);
  FocusScope *read = nullptr;
  std::jthread earlier([&] {
    first.activate();
    read = manager.focusedScope();
  });
  first.entered.wait();
  second.activate();
  EXPECT_EQ(manager.focusedScope(), &second);
  EXPECT_TRUE(second.isFocused.load());
  first.release.count_down();
  earlier.join();
  EXPECT_EQ(read, &second);
  EXPECT_EQ(manager.focusedScope(), &second);
  EXPECT_FALSE(first.isFocused.load());
  EXPECT_TRUE(second.isFocused.load());
}

} // namespace
