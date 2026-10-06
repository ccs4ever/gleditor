#include <gtest/gtest.h>

#include <gleditor/a11y/publisher.hpp>
#include <gleditor/ui/focus_manager.hpp>

#include <algorithm>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace a11y = gleditor::a11y;
namespace ui   = gleditor::ui;

class QueuedPlatform : public a11y::Platform {
public:
  std::deque<a11y::ActionRequest> requests;
  void update(const a11y::Tree &) override {}
  void setWindowFocused(bool) override {}
  void setWindowBounds(const a11y::Rect &, const a11y::Rect &) override {}
  std::optional<a11y::ActionRequest> nextAction() override {
    if (requests.empty()) return {};
    auto request = std::move(requests.front());
    requests.pop_front();
    return request;
  }
};

class AccessibleScope : public ui::FocusScope, public a11y::Source {
public:
  std::string name{"Scope"};
  std::uint32_t requestedFocus{20};
  bool secondRoot{};
  bool advertisesFocus{true}, handlesActions{true};
  int described{}, acted{};
  std::function<void()> onDescribe, onAction;

  void describe(a11y::Builder &builder) override {
    ++described;
    if (auto callback = std::exchange(onDescribe, {})) callback();
    auto &root    = builder.add(0, a11y::Role::Group);
    root.label    = name;
    root.modal    = true;
    root.children = {builder.id(10), builder.id(20)};
    builder.contribute(builder.id(0));
    for (const auto id : {10U, 20U}) {
      auto &field     = builder.add(id, a11y::Role::TextInput);
      field.label     = name + " field " + std::to_string(id);
      field.focusable = true;
      field.actions   = a11y::bit(a11y::Action::SetValue);
      if (advertisesFocus) field.actions |= a11y::bit(a11y::Action::Focus);
      field.modal = true;
    }
    if (secondRoot) {
      auto &extra     = builder.add(100, a11y::Role::Button);
      extra.label     = "Second root";
      extra.focusable = true;
      extra.actions   = a11y::bit(a11y::Action::Click);
      builder.contribute(builder.id(100));
    }
    builder.takeFocus(builder.id(requestedFocus));
  }
  std::uint64_t accessibilityRevision() const override { return 0; }
  bool performAction(std::uint64_t, a11y::Action, std::string_view) override {
    ++acted;
    if (onAction) onAction();
    return handlesActions;
  }
};

class AccessibleLayoutScope : public AccessibleScope {
public:
  std::shared_ptr<ui::LayoutResult> geometry =
      std::make_shared<ui::LayoutResult>();
  AccessibleLayoutScope() {
    geometry->boxes      = {{.id = 10, .focusable = true, .textInput = true},
                            {.id = 20, .focusable = true, .textInput = true}};
    geometry->focusOrder = {10, 20};
  }
  std::shared_ptr<const ui::LayoutResult> focusLayout() const override {
    return geometry;
  }
};

std::size_t modalCount(const a11y::Tree &tree) {
  return std::ranges::count_if(tree.nodes,
                               [](const auto &node) { return node.modal; });
}

TEST(A11yFocusTest, latestOpenScopeOwnsTheOnlyModalAndItsLegacyRequestedFocus) {
  ui::FocusManager manager;
  AccessibleScope older, newer, background;
  a11y::Publisher publisher("Window", "Tests", "1");
  publisher.setFocusManager(&manager);
  publisher.addSource(&newer, 16);
  publisher.addSource(&older, 17);
  publisher.addSource(&background, 18);
  const auto newerRegistration = manager.registerScope(newer);
  const auto olderRegistration = manager.registerScope(older);
  older.activate();
  newer.activate();
  publisher.rebuild(640, 480);
  const auto tree = publisher.snapshot();
  EXPECT_EQ(modalCount(tree), 1U);
  EXPECT_EQ(tree.focus, a11y::Ids::of(16, 20));
  ASSERT_TRUE(tree.find(a11y::Ids::of(16, 0)).has_value());
  EXPECT_TRUE(tree.find(a11y::Ids::of(16, 0))->modal);
  EXPECT_EQ(tree.find(a11y::Ids::of(16, 0))->role, a11y::Role::Dialog);
  EXPECT_FALSE(tree.find(a11y::Ids::of(18, 20))->focusable);
  EXPECT_EQ(tree.find(a11y::Ids::of(18, 20))->actions, 0U);
  newer.deactivate();
  publisher.rebuild(640, 480);
  EXPECT_EQ(publisher.snapshot().focus, a11y::Ids::of(17, 20));
  EXPECT_EQ(modalCount(publisher.snapshot()), 1U);
}

TEST(A11yFocusTest, multipleSourceRootsShareOneManagerOwnedModalWrapper) {
  ui::FocusManager manager;
  AccessibleScope scope;
  scope.secondRoot = true;
  a11y::Publisher publisher("Window", "Tests", "1");
  publisher.setFocusManager(&manager);
  publisher.addSource(&scope, 16);
  const auto registration = manager.push(scope);
  publisher.rebuild(640, 480);
  const auto tree = publisher.snapshot();
  EXPECT_EQ(modalCount(tree), 1U);
  const auto dialog = std::ranges::find_if(
      tree.nodes, [](const auto &node) { return node.modal; });
  ASSERT_NE(dialog, tree.nodes.end());
  EXPECT_EQ(dialog->children,
            (std::vector<std::uint64_t>{a11y::Ids::of(16, 0),
                                        a11y::Ids::of(16, 100)}));
  EXPECT_EQ(tree.focus, a11y::Ids::of(16, 20));
}

TEST(A11yFocusTest,
     layoutFocusChangesRebuildEvenWhenSourceRevisionsStayConstant) {
  ui::FocusManager manager;
  AccessibleLayoutScope scope;
  a11y::Publisher publisher("Window", "Tests", "1");
  publisher.setFocusManager(&manager);
  publisher.addSource(&scope, 23);
  const auto registration = manager.push(scope);
  publisher.rebuild(640, 480);
  EXPECT_EQ(publisher.snapshot().focus, a11y::Ids::of(23, 10));
  EXPECT_TRUE(manager.focusNode(20));
  publisher.rebuild(640, 480);
  EXPECT_EQ(publisher.snapshot().focus, a11y::Ids::of(23, 20));
  EXPECT_EQ(scope.described, 2);
}

TEST(A11yFocusTest, accessibilityFocusRequestsGoThroughTheFocusManager) {
  ui::FocusManager manager;
  AccessibleLayoutScope scope;
  scope.advertisesFocus = false;
  scope.handlesActions  = false;
  auto platform         = std::make_unique<QueuedPlatform>();
  auto *queue           = platform.get();
  a11y::Publisher publisher("Window", "Tests", "1", std::move(platform));
  publisher.setFocusManager(&manager);
  publisher.addSource(&scope, 23);
  const auto registration = manager.push(scope);
  publisher.rebuild(640, 480);
  queue->requests.push_back({a11y::Ids::of(23, 20), a11y::Action::Focus, {}});
  EXPECT_EQ(publisher.pumpActions(), 1U);
  EXPECT_EQ(manager.focusSnapshot().node, 20U);
  publisher.rebuild(640, 480);
  EXPECT_EQ(publisher.snapshot().focus, a11y::Ids::of(23, 20));
}

TEST(A11yFocusTest,
     aScopeWithoutAnAccessibilitySourceStillIsolatesTheBackground) {
  ui::FocusManager manager;
  ui::FocusScope modal;
  AccessibleScope background;
  a11y::Publisher publisher("Window", "Tests", "1");
  publisher.setFocusManager(&manager);
  publisher.addSource(&background, 16);
  const auto registration = manager.push(modal);
  publisher.rebuild(640, 480);
  const auto tree = publisher.snapshot();
  EXPECT_EQ(modalCount(tree), 1U);
  const auto focused = tree.find(tree.focus);
  ASSERT_TRUE(focused.has_value());
  EXPECT_TRUE(focused->modal);
  EXPECT_EQ(focused->role, a11y::Role::Dialog);
  EXPECT_FALSE(tree.find(a11y::Ids::of(16, 20))->focusable);
  EXPECT_EQ(tree.find(a11y::Ids::of(16, 20))->actions, 0U);
}

TEST(A11yFocusTest, queuedActionsCannotReachTheBackgroundWhileAModalIsOpen) {
  ui::FocusManager manager;
  AccessibleScope modal, background;
  auto platform = std::make_unique<QueuedPlatform>();
  auto *queue   = platform.get();
  a11y::Publisher publisher("Window", "Tests", "1", std::move(platform));
  publisher.setFocusManager(&manager);
  publisher.addSource(&modal, 16);
  publisher.addSource(&background, 17);
  const auto registration = manager.push(modal);
  publisher.rebuild(640, 480);
  queue->requests.push_back(
      {a11y::Ids::of(17, 10), a11y::Action::SetValue, "background"});
  queue->requests.push_back(
      {a11y::Ids::of(16, 10), a11y::Action::SetValue, "modal"});
  EXPECT_EQ(publisher.pumpActions(), 1U);
  EXPECT_EQ(background.acted, 0);
  EXPECT_EQ(modal.acted, 1);
}

TEST(A11yFocusTest,
     newlyOpenedScopesBlockOldQueuedActionsBeforeTheTreeRebuilds) {
  ui::FocusManager manager;
  ui::FocusScope modal;
  AccessibleScope background;
  auto platform = std::make_unique<QueuedPlatform>();
  auto *queue   = platform.get();
  a11y::Publisher publisher("Window", "Tests", "1", std::move(platform));
  publisher.setFocusManager(&manager);
  publisher.addSource(&background, 16);
  publisher.rebuild(640, 480);
  const auto registration = manager.push(modal);
  queue->requests.push_back(
      {a11y::Ids::of(16, 10), a11y::Action::SetValue, "background"});
  EXPECT_EQ(publisher.pumpActions(), 0U);
  EXPECT_EQ(background.acted, 0);
}

TEST(A11yFocusTest, actionCallbacksMayUnregisterTheirOwnSource) {
  AccessibleScope source;
  auto platform = std::make_unique<QueuedPlatform>();
  auto *queue   = platform.get();
  a11y::Publisher publisher("Window", "Tests", "1", std::move(platform));
  publisher.addSource(&source, 16);
  publisher.rebuild(640, 480);
  source.onAction = [&] { publisher.removeSource(&source); };
  queue->requests.push_back(
      {a11y::Ids::of(16, 10), a11y::Action::SetValue, "first"});
  queue->requests.push_back(
      {a11y::Ids::of(16, 10), a11y::Action::SetValue, "stale"});
  EXPECT_EQ(publisher.pumpActions(), 1U);
  EXPECT_EQ(source.acted, 1);
}

TEST(A11yFocusTest, dynamicSourceSurvivesRemovalUntilItsActionReturns) {
  struct DynamicSource : AccessibleScope {
    std::function<void()> onDestruction;
    ~DynamicSource() override {
      if (onDestruction) onDestruction();
    }
  };
  auto platform = std::make_unique<QueuedPlatform>();
  auto *queue   = platform.get();
  a11y::Publisher publisher("Window", "Tests", "1", std::move(platform));
  auto source    = std::make_shared<DynamicSource>();
  auto *pointer  = source.get();
  bool destroyed = false, returned = false;
  source->onDestruction = [&] {
    destroyed = true;
    EXPECT_TRUE(returned);
    publisher.removeSource(pointer);
  };
  source->onAction = [&] {
    publisher.removeSource(pointer);
    source.reset();
    EXPECT_FALSE(destroyed);
    returned = true;
  };
  publisher.addSource(source, 16);
  publisher.rebuild(640, 480);
  queue->requests.push_back(
      {a11y::Ids::of(16, 10), a11y::Action::SetValue, "first"});
  queue->requests.push_back(
      {a11y::Ids::of(16, 10), a11y::Action::SetValue, "stale"});
  EXPECT_EQ(publisher.pumpActions(), 1U);
  EXPECT_TRUE(destroyed);
}

TEST(A11yFocusTest,
     sourceCallbacksMayRegisterSourcesAndResizeInvalidatesTheTree) {
  AccessibleScope first, second;
  a11y::Publisher publisher("Window", "Tests", "1");
  publisher.addSource(&first, 16);
  first.onDescribe = [&] { publisher.addSource(&second, 17); };
  publisher.rebuild(640, 480);
  publisher.rebuild(640, 480);
  EXPECT_EQ(second.described, 1);
  publisher.rebuild(1280, 800);
  EXPECT_EQ(
      publisher.snapshot().find(publisher.snapshot().root())->bounds->right,
      1280);
  EXPECT_EQ(
      publisher.snapshot().find(publisher.snapshot().root())->bounds->bottom,
      800);
}
} // namespace
