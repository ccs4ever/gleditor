#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <gleditor/a11y/publisher.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/renderer.hpp>

#include "mocks/device.hpp"

class RendererScriptTest : public testing::Test {
protected:
  testing::NiceMock<MockRenderDevice> device;
  AppStateRef app                    = std::make_shared<AppState>();
  std::shared_ptr<Renderer> renderer = std::static_pointer_cast<Renderer>(
      Renderer::create(app, render::Backend::OpenGL));
  std::unique_ptr<RenderState> state;

  void SetUp() override {
    ON_CALL(device, textureLimits())
        .WillByDefault(testing::Return(render::TextureLimits{2048, 10}));
    ON_CALL(device,
            createTextureArray(testing::_, testing::_, testing::_, testing::_))
        .WillByDefault(testing::Return(render::TextureHandle{1}));
    state = std::make_unique<RenderState>(&device);
  }

  void advance() { renderer->advanceScript(*state); }
  bool capturePending() const {
    return renderer->pendingScriptCapture.has_value();
  }
  bool pending() const { return renderer->hasPendingWork(); }
  void drain() {
    while (auto item = renderer->renderQueue.pop())
      renderer->dispatch(*state, *item);
  }
  void startAnimation(ch::Output<float> &value) {
    renderer->timeline.apply(&value).then<ch::RampTo>(1.0F, 1.0,
                                                      ch::EaseNone());
  }
  void finishAnimation() { renderer->timeline.step(1.0); }
};

TEST_F(RendererScriptTest, QueuedFocusRunsBeforeTheNextSyntheticText) {
  using Step   = AppState::AutomationStep;
  app->script  = {{.kind  = Step::Kind::Input,
                   .input = {.kind = AppState::SyntheticInput::Kind::Text,
                             .text = "new document text"}}};
  bool focused = false;
  // A picking callback queues this after the loop computed its settled flag.
  renderer->runWithState([&](RenderState &) { focused = true; });
  advance();
  EXPECT_FALSE(focused);
  EXPECT_TRUE(app->syntheticQueue.empty());
  drain();
  ASSERT_TRUE(focused);
  advance();
  ASSERT_EQ(app->syntheticQueue.size(), 1U);
  EXPECT_EQ(app->syntheticQueue.front().text, "new document text");
}

TEST_F(RendererScriptTest,
       LiveCaptureObservesAnimationButOrdinaryCaptureWaits) {
  using Step = AppState::AutomationStep;
  ch::Output<float> value{0.0F};
  startAnimation(value);
  ASSERT_TRUE(pending());
  app->script = {{.kind = Step::Kind::Capture, .text = "settled.ppm"}};
  advance();
  EXPECT_FALSE(capturePending());
  app->script.front().kind = Step::Kind::CaptureLive;
  advance();
  EXPECT_TRUE(capturePending());
  EXPECT_TRUE(pending()) << "the live frame is captured before motion settles";
}

TEST_F(RendererScriptTest, LiveCaptureWaitsForQueuedFocusEvenDuringAnimation) {
  using Step = AppState::AutomationStep;
  ch::Output<float> value{0.0F};
  startAnimation(value);
  app->script  = {{.kind = Step::Kind::CaptureLive, .text = "live.ppm"}};
  bool focused = false;
  renderer->runWithState([&](RenderState &) { focused = true; });
  advance();
  EXPECT_FALSE(capturePending());
  drain();
  ASSERT_TRUE(focused);
  advance();
  EXPECT_TRUE(capturePending());
  EXPECT_TRUE(pending());
}

TEST_F(RendererScriptTest, OrdinaryCaptureRunsAfterAnimationFinishes) {
  using Step = AppState::AutomationStep;
  ch::Output<float> value{0.0F};
  startAnimation(value);
  app->script = {{.kind = Step::Kind::Capture, .text = "settled.ppm"}};
  advance();
  EXPECT_FALSE(capturePending());
  finishAnimation();
  ASSERT_FALSE(pending());
  advance();
  EXPECT_TRUE(capturePending());
}

TEST_F(RendererScriptTest, ReadinessTimeoutFailsEvenDuringAnimation) {
  using Step = AppState::AutomationStep;
  ch::Output<float> value{0.0F};
  startAnimation(value);
  app->alive  = true;
  app->script = {{.kind  = Step::Kind::AwaitAccessibility,
                  .text  = "Missing",
                  .delay = std::chrono::milliseconds(0)}};
  advance();
  EXPECT_TRUE(app->renderFailed);
  EXPECT_FALSE(app->alive);
}

namespace {
class ReadinessSource : public gleditor::a11y::Source {
public:
  std::string value = "Opening";
  bool duplicate    = false;
  void describe(gleditor::a11y::Builder &into) override {
    auto &node = into.add(1, gleditor::a11y::Role::Group);
    node.label = "Clip";
    node.value = value;
    into.contribute(into.id(1));
    if (duplicate) {
      auto &other = into.add(2, gleditor::a11y::Role::Group);
      other.label = "Clip";
      other.value = value;
      into.contribute(into.id(2));
    }
  }
  std::uint64_t accessibilityRevision() const override { return ++revision; }

private:
  mutable std::uint64_t revision = 0;
};
} // namespace

TEST_F(RendererScriptTest, ReadinessRequiresUniqueValueAndObservesAnimation) {
  using Step  = AppState::AutomationStep;
  auto source = std::make_shared<ReadinessSource>();
  app->accessibility =
      std::make_shared<gleditor::a11y::Publisher>("Test", "Test", "1");
  app->accessibility->addSource(source);
  app->accessibility->rebuild(800, 600);
  ch::Output<float> value{0.0F};
  startAnimation(value);
  app->script = {{.kind          = Step::Kind::AwaitAccessibility,
                  .text          = "Clip",
                  .expectedValue = "Playing",
                  .delay         = std::chrono::milliseconds(30000)},
                 {.kind = Step::Kind::CaptureLive, .text = "playing.ppm"}};
  advance();
  EXPECT_FALSE(app->renderFailed);
  source->value     = "Playing";
  source->duplicate = true;
  app->accessibility->rebuild(800, 600);
  advance();
  advance();
  EXPECT_FALSE(capturePending());
  source->duplicate = false;
  app->accessibility->rebuild(800, 600);
  advance();
  advance();
  EXPECT_TRUE(capturePending());
  EXPECT_TRUE(pending());
}

TEST_F(RendererScriptTest, ReadinessDeadlineIncludesQueuedFocus) {
  using Step  = AppState::AutomationStep;
  app->alive  = true;
  app->script = {{.kind  = Step::Kind::AwaitAccessibility,
                  .text  = "Missing",
                  .delay = std::chrono::milliseconds(0)}};
  renderer->runWithState([](RenderState &) {});
  advance();
  EXPECT_TRUE(app->renderFailed);
  EXPECT_FALSE(app->alive);
}
