#include "mocks/device.hpp"
#include <algorithm>
#include <cstring>
#include <gleditor/doc.hpp>
#include <gleditor/form.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/text/diagnostics.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <map>
#include <vector>

namespace {
class FormDevice : public testing::NiceMock<MockRenderDevice> {
public:
  std::map<std::uint32_t, std::vector<std::byte>> buffers;
  std::vector<Doc::VBORow> drawn;
  std::uint32_t next{1};
  FormDevice() {
    ON_CALL(*this, textureLimits())
        .WillByDefault(testing::Return(render::TextureLimits{2048, 10}));
    ON_CALL(*this, createTextureArray)
        .WillByDefault(testing::Return(render::TextureHandle{1}));
    ON_CALL(*this, createPipeline)
        .WillByDefault(testing::Return(render::PipelineHandle{1}));
    ON_CALL(*this, createBuffer)
        .WillByDefault([this](render::BufferKind, std::size_t size) {
          const auto id = next++;
          buffers[id].resize(size);
          return render::BufferHandle{id};
        });
    ON_CALL(*this, resizeBuffer)
        .WillByDefault([this](render::BufferHandle buffer, std::size_t size) {
          buffers[buffer.id].resize(size);
          return buffer;
        });
    ON_CALL(*this, updateBuffer)
        .WillByDefault([this](render::BufferHandle buffer, std::size_t offset,
                              std::span<const std::byte> bytes) {
          auto &storage = buffers[buffer.id];
          storage.resize(std::max(storage.size(), offset + bytes.size()));
          std::memcpy(storage.data() + offset, bytes.data(), bytes.size());
        });
    ON_CALL(*this, drawGlyphs)
        .WillByDefault([this](const render::DrawUniforms &,
                              render::BufferHandle buffer, std::size_t offset,
                              std::uint32_t count) {
          const auto &storage = buffers.at(buffer.id);
          for (std::uint32_t i = 0; i < count; ++i) {
            Doc::VBORow row{};
            std::memcpy(&row, storage.data() + offset + i * sizeof(row),
                        sizeof(row));
            drawn.push_back(row);
          }
        });
  }
};
struct FormLayoutTest : testing::Test {
  FormDevice device;
  RenderState state{&device};
  gleditor::Form form;
  ch::Timeline timeline;
  glm::mat4 projection{1};
  void draw(gleditor::ui::UiMetrics metrics,
            const gleditor::ui::Theme &theme = gleditor::ui::defaultTheme()) {
    device.drawn.clear();
    gleditor::FrameContext ctx{.state          = state,
                               .viewProjection = projection,
                               .screenWidth    = metrics.screenWidth,
                               .screenHeight   = metrics.screenHeight,
                               .timeline       = timeline,
                               .chrome         = metrics.chrome,
                               .metrics        = metrics,
                               .theme          = theme};
    form.drawFrame(ctx);
  }
  void ready() { form.deviceReady(device, {}); }
};
void contained(gleditor::ui::Rect child, gleditor::ui::Rect parent) {
  EXPECT_GE(child.left, parent.left - .01F);
  EXPECT_GE(child.bottom, parent.bottom - .01F);
  EXPECT_LE(child.left + child.width, parent.left + parent.width + .01F);
  EXPECT_LE(child.bottom + child.height, parent.bottom + parent.height + .01F);
}

TEST_F(FormLayoutTest, responsiveFieldsImeAndInkShareContainedGeometry) {
  form.open("A long title with an identifier and a explanation",
            "A long note that wraps inside the available safe area rather than "
            "crossing its panel edge.",
            {{.label = "An unusually long field label",
              .value = std::string(150, 'W')},
             {.label = "Passphrase",
              .value = "never publish me",
              .kind  = gleditor::Form::Kind::Secret},
             {.label          = "Visibility",
              .kind           = gleditor::Form::Kind::Toggle,
              .revealsSecrets = true}},
            [](const auto &) {});
  ready();
  for (const auto size :
       {std::pair{320, 240}, std::pair{640, 360}, std::pair{1280, 720}})
    for (float scale : {1.0F, 1.25F, 2.0F})
      for (float fontScale : {.8F, 1.0F, 2.0F}) {
        const gleditor::ui::UiMetrics metrics{.contentScale = scale,
                                              .fontScale    = fontScale,
                                              .screenWidth  = size.first,
                                              .screenHeight = size.second,
                                              .chrome       = {.top = 24}};
        draw(metrics);
        const auto layout = form.focusLayout();
        ASSERT_TRUE(layout);
        contained(layout->bounds, metrics.pixelSafeArea());
        EXPECT_EQ(layout->focusOrder,
                  (std::vector<std::uint32_t>{16, 80, 144}));
        for (const auto &box : layout->boxes)
          contained(box.rect, layout->bounds);
        const auto *focused = layout->find(16);
        ASSERT_NE(focused, nullptr);
        EXPECT_GT(focused->contentRect.width, 0);
        EXPECT_GT(focused->contentRect.height, 0);
        EXPECT_EQ(form.textArea(),
                  gleditor::ui::toInputArea(focused->contentRect, size.second));
        for (const auto &row : device.drawn) {
          if ((row.foreground & Doc::VBORow::solidFlag) != 0) continue;
          const float w = (row.quad >> 20U) & 4095U,
                      h = (row.quad >> 8U) & 4095U;
          contained({row.pos[0] - w * .5F, row.pos[1] - h * .5F, w, h},
                    layout->bounds);
        }
        gleditor::a11y::Tree tree;
        gleditor::a11y::Builder builder(tree, 16);
        form.describe(builder);
        const auto password = tree.find(builder.id(80));
        ASSERT_TRUE(password);
        EXPECT_EQ(password->value, std::string(16, '*'));
        EXPECT_NE(password->value, "never publish me");
        EXPECT_TRUE(password->actions &
                    gleditor::a11y::bit(gleditor::a11y::Action::SetValue));
      }
}
TEST_F(FormLayoutTest, keyboardHelpDrawsVisibleInk) {
  form.open("Title", "", {{.label = "Name", .value = "Ada"}},
            [](const auto &) {});
  ready();
  for (const float scale : {1.0F, 1.25F, 2.0F}) {
    draw({.contentScale = scale, .screenWidth = 1280, .screenHeight = 800});
    const auto footer = form.focusLayout()->find(4)->rect;
    EXPECT_TRUE(std::ranges::any_of(device.drawn, [&](const auto &row) {
      return (row.foreground & Doc::VBORow::solidFlag) == 0 &&
             row.pos[0] >= footer.left &&
             row.pos[0] <= footer.left + footer.width &&
             row.pos[1] >= footer.bottom &&
             row.pos[1] <= footer.bottom + footer.height;
    }));
  }
}
TEST_F(FormLayoutTest, steadyFramesDoNotReshapeOrReuploadChrome) {
  form.open("Retained form", "", {{.label = "Name", .value = "Ada"}},
            [](const auto &) {});
  ready();
  const gleditor::ui::UiMetrics metrics{.screenWidth  = 640,
                                        .screenHeight = 480};
  draw(metrics);
  EXPECT_CALL(device, updateBuffer).Times(0);
  EXPECT_CALL(device, createPipeline).Times(0);
  gleditor::text::ShapingStatsScope measured;
  for (int i = 0; i < 50; ++i) draw(metrics);
  EXPECT_EQ(measured.stats().layoutCalls, 0U);
  EXPECT_EQ(measured.stats().harfbuzzCalls, 0U);
}
TEST_F(FormLayoutTest, traversalRevealsHiddenFieldsAndKeepsTheirValues) {
  std::vector<gleditor::Form::Field> fields;
  for (int i = 0; i < 12; ++i)
    fields.push_back(
        {.label = "Field " + std::to_string(i), .value = std::to_string(i)});
  form.open("Many fields", "", fields, [](const auto &) {});
  ready();
  const gleditor::ui::UiMetrics metrics{
      .fontScale = 2, .screenWidth = 640, .screenHeight = 360};
  draw(metrics);
  EXPECT_EQ(form.focusLayout()->find(16 + 11 * 64)->rect.height, 0);
  for (int i = 0; i < 11; ++i)
    form.keyPressed(gleditor::Key::Tab, gleditor::KeyMods::None);
  draw(metrics);
  EXPECT_GT(form.focusLayout()->find(16 + 11 * 64)->rect.height, 0);
  EXPECT_EQ(form.focusLayout()->find(16)->rect.height, 0);
  const auto current = form.current();
  ASSERT_EQ(current.size(), fields.size());
  for (std::size_t i = 0; i < fields.size(); ++i)
    EXPECT_EQ(current[i].value, fields[i].value);
}
TEST_F(FormLayoutTest, openChoicesVirtualizeAndFollowTheKeyboardHighlight) {
  gleditor::Form::Field choice{.label = "Choice",
                               .kind  = gleditor::Form::Kind::Choice};
  for (int i = 0; i < 40; ++i)
    choice.options.push_back("Choice " + std::to_string(i));
  form.open("Options", "", {choice}, [](const auto &) {});
  ready();
  form.keyPressed(gleditor::Key::Space, gleditor::KeyMods::None);
  for (int i = 0; i < 39; ++i)
    form.keyPressed(gleditor::Key::Down, gleditor::KeyMods::None);
  draw({.screenWidth = 640, .screenHeight = 480});
  const auto layout = form.focusLayout();
  ASSERT_NE(layout->find(16 + 40), nullptr);
  EXPECT_EQ(layout->find(17), nullptr);
  contained(layout->find(16 + 40)->rect, layout->bounds);
  form.keyPressed(gleditor::Key::Return, gleditor::KeyMods::None);
  EXPECT_EQ(form.current()[0].chosen, 39U);
  EXPECT_FALSE(form.listOpen());
  EXPECT_TRUE(form.isOpen());
}
TEST_F(FormLayoutTest, tinyExpandedChoiceKeepsItsHighlightedOptionVisible) {
  form.open("Options", "",
            {{.label   = "Choice",
              .kind    = gleditor::Form::Kind::Choice,
              .options = {"First", "Second"}}},
            [](const auto &) {});
  ready();
  form.keyPressed(gleditor::Key::Space, gleditor::KeyMods::None);
  form.keyPressed(gleditor::Key::Down, gleditor::KeyMods::None);
  draw({.contentScale = 2,
        .fontScale    = 2,
        .screenWidth  = 320,
        .screenHeight = 240});
  const auto layout     = form.focusLayout();
  const auto *highlight = layout->find(18);
  ASSERT_NE(highlight, nullptr);
  EXPECT_GT(highlight->rect.height, 0);
  EXPECT_GT(layout->find(16)->contentRect.height, 0);
}
TEST_F(FormLayoutTest, themeAndFontScaleInvalidateRetainedGeometry) {
  form.open("Theme", "", {{.label = "Name", .value = "Ada"}},
            [](const auto &) {});
  ready();
  const gleditor::ui::UiMetrics metrics{.screenWidth  = 1280,
                                        .screenHeight = 720};
  draw(metrics);
  const auto original = form.focusLayout()->find(16)->rect.height;
  auto theme          = gleditor::ui::defaultTheme();
  theme.fonts[static_cast<std::size_t>(gleditor::ui::FontRole::Label)].points =
      24;
  draw(metrics, theme);
  EXPECT_GT(form.focusLayout()->find(16)->rect.height, original);
}
} // namespace

TEST(OverlayPickScopeTest, PersistentScopesStayDistinctWhenFrameScopesWrap) {
  testing::NiceMock<MockRenderDevice> device;
  RenderState state(&device);
  const auto first     = state.allocatePersistentOverlayPickScope();
  const auto second    = state.allocatePersistentOverlayPickScope();
  const auto available = ((1U << render::tagDocBits) - 1U) - 2;
  EXPECT_NE(first, second);
  for (std::uint32_t i = 0; i < available * 2; ++i) {
    const auto frameScope = state.allocateOverlayPickScope();
    EXPECT_NE(frameScope, 0U);
    EXPECT_LT(frameScope, second);
  }
}

TEST(OverlayPickScopeTest, FrameAllocationBeforePersistentCannotAlias) {
  testing::NiceMock<MockRenderDevice> device;
  RenderState state(&device);
  state.nextOverlayPickScope = (1U << render::tagDocBits) - 1U;
  const auto frame           = state.allocateOverlayPickScope();
  const auto retained        = state.allocatePersistentOverlayPickScope();
  EXPECT_NE(frame, retained);
  EXPECT_LT(frame, 1U << (render::tagDocBits - 1));
  EXPECT_GE(retained, 1U << (render::tagDocBits - 1));
}
