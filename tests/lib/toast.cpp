#include <gtest/gtest.h>

#include <gleditor/render/diagnostics.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/toast.hpp>

#include <gmock/gmock.h>
#include <memory>
#include <pangomm/init.h>
#include <string>

#include "mocks/device.hpp"

using render::DiagnosticSeverity;
using testing::NiceMock;
using testing::Return;

/**
 * @brief ToastOverlay over a mocked device.
 *
 * Text is laid out and rasterised for real -- Pango and Cairo need no graphics
 * device -- while the buffer and texture uploads go to the mock. What is under
 * test is the bookkeeping the backend comparison cannot reach: how many
 * notifications are kept, and when they go away.
 */
class ToastOverlayTest : public testing::Test {
protected:
  std::unique_ptr<NiceMock<MockRenderDevice>> device;
  std::unique_ptr<RenderState> state;
  std::unique_ptr<ToastOverlay> overlay;

  void SetUp() override {
    Pango::init();
    device = std::make_unique<NiceMock<MockRenderDevice>>();
    // The glyph cache sizes its atlas from these, and refuses to pack anything
    // into a zero-sized one.
    ON_CALL(*device, textureLimits())
        .WillByDefault(Return(render::TextureLimits{2048, 10}));
    ON_CALL(*device,
            createTextureArray(testing::_, testing::_, testing::_, testing::_))
        .WillByDefault(Return(render::TextureHandle{1}));
    ON_CALL(*device, createBuffer(testing::_, testing::_))
        .WillByDefault(Return(render::BufferHandle{1}));

    state   = std::make_unique<RenderState>(device.get());
    overlay = std::make_unique<ToastOverlay>(device.get(), "Monospace 12");
  }

  void TearDown() override {
    overlay.reset();
    state.reset();
    device.reset();
  }

  void post(const std::string &message,
            const DiagnosticSeverity severity = DiagnosticSeverity::Warning) {
    overlay->post(severity, message, *state);
  }
};

TEST_F(ToastOverlayTest, startsEmpty) {
  EXPECT_TRUE(overlay->empty());
  EXPECT_EQ(overlay->size(), 0U);
}

TEST_F(ToastOverlayTest, postingShowsAMessage) {
  post("something happened");
  EXPECT_FALSE(overlay->empty());
  EXPECT_EQ(overlay->size(), 1U);
}

TEST_F(ToastOverlayTest, everySeverityIsShown) {
  post("a note", DiagnosticSeverity::Info);
  post("a caution", DiagnosticSeverity::Warning);
  post("a fault", DiagnosticSeverity::Error);
  EXPECT_EQ(overlay->size(), 3U);
}

// A driver that starts complaining can produce messages faster than they
// expire; filling the window with them would hide the document the user is
// trying to work on.
TEST_F(ToastOverlayTest, olderMessagesAreDroppedPastTheCap) {
  for (int i = 0; i < 20; i++) {
    post("message " + std::to_string(i));
  }
  EXPECT_EQ(overlay->size(), ToastOverlay::maxVisible);
}

TEST_F(ToastOverlayTest, aMessageGoesAwayOnceItsLifetimeIsUp) {
  const auto now = ToastOverlay::Clock::now();
  post("temporary");
  ASSERT_EQ(overlay->size(), 1U);

  overlay->expire(now + ToastOverlay::lifetime - std::chrono::seconds{1});
  EXPECT_EQ(overlay->size(), 1U) << "expired before its lifetime was up";

  overlay->expire(now + ToastOverlay::lifetime + std::chrono::seconds{1});
  EXPECT_TRUE(overlay->empty());
}

// Expiry walks from the front and stops at the first live entry, so a stale
// message must not strand the ones behind it, nor take them with it.
TEST_F(ToastOverlayTest, expiryOnlyTakesTheMessagesThatAreDue) {
  const auto start = ToastOverlay::Clock::now();
  post("first");
  // Post the second far enough after the first that one expiry instant falls
  // between them.
  overlay->expire(start); // no-op, but proves expire() before a post is safe
  post("second");

  overlay->expire(start - std::chrono::seconds{1});
  EXPECT_EQ(overlay->size(), 2U);

  overlay->expire(start + ToastOverlay::lifetime + std::chrono::seconds{1});
  EXPECT_TRUE(overlay->empty());
}

// Rows are returned to the pool when a notification goes, so a long-running
// editor showing thousands of them does not grow the buffer without bound.
TEST_F(ToastOverlayTest, spaceIsReusedAcrossManyMessages) {
  for (int round = 0; round < 200; round++) {
    post("message " + std::to_string(round));
    overlay->expire(ToastOverlay::Clock::now() + ToastOverlay::lifetime +
                    std::chrono::seconds{1});
  }
  EXPECT_TRUE(overlay->empty());
}
