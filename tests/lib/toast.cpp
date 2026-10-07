#include <gtest/gtest.h>

#include <gleditor/doc.hpp>
#include <gleditor/render/diagnostics.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/text/diagnostics.hpp>
#include <gleditor/text/font.hpp>
#include <gleditor/toast.hpp>

#include <cstring>
#include <fstream>
#include <gmock/gmock.h>
#include <memory>
#include <string>

#include "mocks/device.hpp"

using render::DiagnosticSeverity;
using testing::NiceMock;
using testing::Return;

/**
 * @brief ToastOverlay over a mocked device.
 *
 * Text is laid out and rasterised for real -- FreeType and HarfBuzz need no
 * graphics device -- while the buffer and texture uploads go to the mock. What
 * is under test is the bookkeeping the backend comparison cannot reach: how
 * many notifications are kept, and when they go away.
 */
class ToastOverlayTest : public testing::Test {
protected:
  std::unique_ptr<NiceMock<MockRenderDevice>> device;
  std::unique_ptr<RenderState> state;
  std::unique_ptr<ToastOverlay> overlay;
  std::vector<Doc::VBORow> uploaded;

  void SetUp() override {
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
    ON_CALL(*device, createPipeline(testing::_))
        .WillByDefault(Return(render::PipelineHandle{1}));
    ON_CALL(*device, updateBuffer(testing::_, testing::_, testing::_))
        .WillByDefault([this](render::BufferHandle, std::size_t,
                              std::span<const std::byte> data) {
          for (std::size_t offset = 0;
               offset + sizeof(Doc::VBORow) <= data.size();
               offset += sizeof(Doc::VBORow)) {
            Doc::VBORow row{};
            std::memcpy(&row, data.data() + offset, sizeof(row));
            uploaded.push_back(row);
          }
        });

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

// Expired notifications release their retained geometry before another is
// posted.
TEST_F(ToastOverlayTest, spaceIsReusedAcrossManyMessages) {
  for (int round = 0; round < 200; round++) {
    post("message " + std::to_string(round));
    overlay->expire(ToastOverlay::Clock::now() + ToastOverlay::lifetime +
                    std::chrono::seconds{1});
  }
  EXPECT_TRUE(overlay->empty());
}

TEST_F(ToastOverlayTest, longMessagesStayInsideTheSafeAreaAtEveryFontScale) {
  using namespace gleditor;
  std::ifstream fixtures("tests/samples/ui/long-labels.tsv");
  std::vector<std::string> labels;
  for (std::string line; std::getline(fixtures, line);) {
    if (line.empty() || line.front() == '#') continue;
    labels.push_back(line.substr(line.find('\t') + 1));
  }
  ASSERT_EQ(labels.size(), 9U);
  overlay = std::make_unique<ToastOverlay>(device.get());
  overlay->createPipeline({});
  for (const auto &label : labels) post(label);
  for (const auto size :
       {ui::Size{640, 480}, ui::Size{1280, 800}, ui::Size{2560, 1440}}) {
    for (const auto scale : {0.8F, 1.0F, 1.5F, 2.0F}) {
      for (const auto *family :
           {"Sans", "Serif", "Monospace", "Noto Sans CJK JP"}) {
        SCOPED_TRACE(testing::Message() << size.width << 'x' << size.height
                                        << ' ' << scale << ' ' << family);
        ui::UiMetrics metrics{.fontScale    = scale,
                              .screenWidth  = static_cast<int>(size.width),
                              .screenHeight = static_cast<int>(size.height),
                              .chrome       = {.top = 44, .bottom = 20}};
        ui::Theme theme;
        theme.fonts[static_cast<std::size_t>(ui::FontRole::Caption)].family =
            family;
        overlay->setPresentation(metrics, theme);
        uploaded.clear();
        overlay->draw(*state, metrics.screenWidth, metrics.screenHeight);
        const auto safe      = metrics.pixelSafeArea();
        const auto contained = [&](ui::Rect box) {
          EXPECT_GE(box.left, safe.left - 0.01F);
          EXPECT_GE(box.bottom, safe.bottom - 0.01F);
          EXPECT_LE(box.left + box.width, safe.left + safe.width + 0.01F);
          EXPECT_LE(box.bottom + box.height, safe.bottom + safe.height + 0.01F);
        };
        ASSERT_EQ(overlay->layout().boxes.size(), ToastOverlay::maxVisible);
        for (const auto &box : overlay->layout().boxes) contained(box.rect);
        ASSERT_FALSE(uploaded.empty());
        for (const auto &row : uploaded) {
          const auto width  = static_cast<float>((row.quad >> 20U) & 4095U);
          const auto height = static_cast<float>((row.quad >> 8U) & 4095U);
          contained({row.pos[0] - width * 0.5F, row.pos[1] - height * 0.5F,
                     width, height});
        }
        a11y::Tree tree;
        a11y::Builder builder(tree, 12);
        overlay->describe(builder);
        const auto node = tree.find(builder.id(labels.size()));
        ASSERT_TRUE(node);
        EXPECT_EQ(node->label, labels.back());
        EXPECT_EQ(node->value, labels.back());
        ASSERT_TRUE(node->bounds);
        const auto &box = overlay->layout().boxes.back().rect;
        EXPECT_FLOAT_EQ(static_cast<float>(node->bounds->left), box.left);
        EXPECT_FLOAT_EQ(static_cast<float>(node->bounds->top),
                        static_cast<float>(metrics.screenHeight) - box.bottom -
                            box.height);
        const text::ShapingStatsScope capture;
        overlay->draw(*state, metrics.screenWidth, metrics.screenHeight);
        EXPECT_EQ(capture.stats().layoutCalls, 0U);
        EXPECT_EQ(capture.stats().harfbuzzCalls, 0U);
      }
    }
  }
}

TEST_F(ToastOverlayTest, changingTypographyKeepsTheOriginalExpiryAndSerial) {
  overlay->createPipeline({});
  const auto before = ToastOverlay::Clock::now();
  post("A message whose lifetime must survive resizing and typography changes");
  overlay->draw(*state, 640, 480);
  const auto revision = overlay->accessibilityRevision();
  gleditor::ui::UiMetrics metrics{
      .fontScale = 2, .screenWidth = 1280, .screenHeight = 800};
  overlay->setPresentation(metrics, gleditor::ui::defaultTheme(), 1);
  overlay->draw(*state, 1280, 800);
  EXPECT_GT(overlay->accessibilityRevision(), revision);
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 12);
  overlay->describe(builder);
  EXPECT_TRUE(tree.find(builder.id(1)));
  overlay->expire(before + ToastOverlay::lifetime + std::chrono::seconds{1});
  EXPECT_TRUE(overlay->empty());
  EXPECT_TRUE(overlay->layout().boxes.empty());
}

TEST_F(ToastOverlayTest, everyUnicodeFixtureHasBoundedGeometryAndItsFullLabel) {
  using namespace gleditor;
  overlay = std::make_unique<ToastOverlay>(device.get());
  overlay->createPipeline({});
  const ui::UiMetrics metrics{
      .fontScale = 2, .screenWidth = 640, .screenHeight = 480};
  overlay->setPresentation(metrics, ui::defaultTheme());
  std::ifstream fixtures("tests/samples/ui/long-labels.tsv");
  std::uint64_t serial{};
  for (std::string line; std::getline(fixtures, line);) {
    if (line.empty() || line.front() == '#') continue;
    const auto label = line.substr(line.find('\t') + 1);
    SCOPED_TRACE(label);
    overlay->expire(ToastOverlay::Clock::now() + ToastOverlay::lifetime +
                    std::chrono::seconds{1});
    post(label);
    uploaded.clear();
    overlay->draw(*state, metrics.screenWidth, metrics.screenHeight);
    ASSERT_EQ(overlay->layout().boxes.size(), 1U);
    const auto &box = overlay->layout().boxes.front();
    for (const auto &row : uploaded) {
      const auto width   = static_cast<float>((row.quad >> 20U) & 4095U);
      const auto height  = static_cast<float>((row.quad >> 8U) & 4095U);
      const auto &parent = (row.foreground & Doc::VBORow::solidFlag) == 0
                               ? box.contentRect
                               : box.rect;
      EXPECT_GE(row.pos[0] - width * 0.5F, parent.left - 0.01F);
      EXPECT_GE(row.pos[1] - height * 0.5F, parent.bottom - 0.01F);
      EXPECT_LE(row.pos[0] + width * 0.5F, parent.left + parent.width + 0.01F);
      EXPECT_LE(row.pos[1] + height * 0.5F,
                parent.bottom + parent.height + 0.01F);
    }
    a11y::Tree tree;
    a11y::Builder builder(tree, 12);
    overlay->describe(builder);
    const auto node = tree.find(builder.id(++serial));
    ASSERT_TRUE(node);
    EXPECT_EQ(node->label, label);
  }
  EXPECT_EQ(serial, 9U);
}
