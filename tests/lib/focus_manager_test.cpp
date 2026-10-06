#include <gtest/gtest.h>

#include <gleditor/app.hpp>
#include <gleditor/ui/focus_manager.hpp>

#include <functional>
#include <optional>
#include <string>
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
} // namespace
