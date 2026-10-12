#include <gmock/gmock.h>
#include <gtest/gtest.h>

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
