#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <gleditor/doc.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/text/diagnostics.hpp>
#include <gleditor/ui/layout.hpp>
#include <gleditor/ui/metrics.hpp>
#include <gleditor/ui/overlay.hpp>

#include "mocks/device.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

class OverlayRecordingDevice : public testing::NiceMock<MockRenderDevice> {
public:
  std::map<std::uint32_t, std::vector<std::byte>> buffers;
  std::vector<Doc::VBORow> drawn;
  std::vector<std::uint32_t> identities;
  std::uint32_t nextBuffer{1};

  OverlayRecordingDevice() {
    ON_CALL(*this, textureLimits())
        .WillByDefault(testing::Return(render::TextureLimits{2048, 10}));
    ON_CALL(*this, createTextureArray)
        .WillByDefault(testing::Return(render::TextureHandle{1}));
    ON_CALL(*this, createBuffer)
        .WillByDefault([this](render::BufferKind, std::size_t size) {
          const auto id = nextBuffer++;
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
    ON_CALL(*this, createPipeline)
        .WillByDefault(testing::Return(render::PipelineHandle{1}));
    ON_CALL(*this, drawGlyphs)
        .WillByDefault([this](const render::DrawUniforms &uniforms,
                              render::BufferHandle buffer, std::size_t offset,
                              std::uint32_t count) {
          const auto &storage = buffers.at(buffer.id);
          ASSERT_LE(offset + count * sizeof(Doc::VBORow), storage.size());
          for (std::uint32_t index = 0; index < count; ++index) {
            Doc::VBORow row{};
            std::memcpy(&row, storage.data() + offset + index * sizeof(row),
                        sizeof(row));
            drawn.push_back(row);
            identities.push_back(uniforms.identity);
          }
        });
  }
};

gleditor::ui::Rect rectangleOf(const Doc::VBORow &row) {
  const auto width  = static_cast<float>((row.quad >> 20U) & 4095U);
  const auto height = static_cast<float>((row.quad >> 8U) & 4095U);
  return {row.pos[0] - width * 0.5F, row.pos[1] - height * 0.5F, width, height};
}

void expectContained(gleditor::ui::Rect child, gleditor::ui::Rect parent) {
  constexpr float roundingTolerance = 0.01F;
  EXPECT_GE(child.left, parent.left - roundingTolerance);
  EXPECT_GE(child.bottom, parent.bottom - roundingTolerance);
  EXPECT_LE(child.left + child.width,
            parent.left + parent.width + roundingTolerance);
  EXPECT_LE(child.bottom + child.height,
            parent.bottom + parent.height + roundingTolerance);
}

std::vector<std::string> longLabels() {
  std::ifstream input("tests/samples/ui/long-labels.tsv");
  std::vector<std::string> result;
  for (std::string line; std::getline(input, line);) {
    if (line.empty() || line.front() == '#') continue;
    const auto separator = line.find('\t');
    if (separator != std::string::npos)
      result.push_back(line.substr(separator + 1));
  }
  return result;
}

gleditor::ui::Widget fixturePanel(const std::vector<std::string> &labels) {
  using namespace gleditor::ui;
  Widget root{.id = 1, .model = Panel{"Long label fixtures"}};
  WidgetId id = 10;
  for (const auto &label : labels) {
    root.children.push_back({.id       = id++,
                             .model    = Label{label, TextPurpose::Description},
                             .maxLines = 1});
  }
  root.children.push_back(
      {.id = 100, .model = Button{labels.front(), "accept"}, .maxLines = 1});
  return root;
}

class ScreenOverlayTest : public testing::Test {
protected:
  OverlayRecordingDevice device;
  RenderState state{&device};
  glm::mat4 projection{1.0F};
  ch::Timeline timeline;

  void draw(gleditor::ui::ScreenOverlay &overlay,
            const gleditor::ui::UiMetrics &metrics,
            const gleditor::ui::Theme &theme) {
    gleditor::FrameContext context{.state          = state,
                                   .viewProjection = projection,
                                   .screenWidth    = metrics.screenWidth,
                                   .screenHeight   = metrics.screenHeight,
                                   .timeline       = timeline,
                                   .metrics        = metrics,
                                   .theme          = theme};
    device.drawn.clear();
    device.identities.clear();
    state.beginPickScene();
    overlay.drawFrame(context);
  }
};

TEST_F(ScreenOverlayTest, layoutDrawPickAndAccessibilityShareTheSameBounds) {
  using namespace gleditor::ui;
  const auto labels = longLabels();
  ASSERT_EQ(labels.size(), 9U);
  ScreenOverlay overlay(fixturePanel(labels));
  overlay.deviceReady(device, {});
  for (const auto dimensions :
       {Size{640, 480}, Size{1280, 800}, Size{2560, 1440}}) {
    for (const auto fontScale : {0.8F, 1.0F, 1.5F, 2.0F}) {
      for (const auto *family :
           {"Sans", "Serif", "Monospace", "Noto Sans CJK JP"}) {
        SCOPED_TRACE(testing::Message()
                     << dimensions.width << 'x' << dimensions.height << ' '
                     << family << " scale " << fontScale);
        UiMetrics metrics{.fontScale    = fontScale,
                          .screenWidth  = static_cast<int>(dimensions.width),
                          .screenHeight = static_cast<int>(dimensions.height),
                          .chrome       = {.top = 24.0F, .bottom = 18.0F}};
        Theme theme;
        for (auto &font : theme.fonts) font.family = family;
        const auto scene = overlay.prepare(metrics, theme);
        ASSERT_NE(scene, nullptr);
        expectContained(scene->layout.bounds, metrics.pixelSafeArea());
        for (const auto &box : scene->layout.boxes) {
          SCOPED_TRACE(box.id);
          expectContained(box.rect, scene->layout.bounds);
          expectContained(box.contentRect, box.rect);
          if (box.parentId != 0) {
            const auto *parent = scene->layout.find(box.parentId);
            ASSERT_NE(parent, nullptr);
            expectContained(box.rect, parent->rect);
          }
          const auto *visual = scene->find(box.id);
          if (visual != nullptr) {
            EXPECT_LE(visual->fitted.widthPx, box.contentRect.width + 0.01F);
            EXPECT_LE(visual->fitted.heightPx, box.contentRect.height + 0.01F);
          }
        }
        draw(overlay, metrics, theme);
        ASSERT_FALSE(device.drawn.empty());
        for (const auto &quad : device.drawn) {
          const auto id = scene->resolvePickingId(quad.paper & 65535U);
          ASSERT_TRUE(id.has_value());
          const auto *box = scene->layout.find(*id);
          ASSERT_NE(box, nullptr)
              << "every drawn picking tag needs a layout box";
          expectContained(rectangleOf(quad), box->rect);
          if ((quad.foreground & Doc::VBORow::solidFlag) == 0) {
            expectContained(rectangleOf(quad), box->contentRect);
          }
        }
        gleditor::a11y::Tree tree;
        gleditor::a11y::Builder builder(tree, 16);
        overlay.describe(builder);
        for (std::size_t index = 0; index < labels.size(); ++index) {
          const auto id   = static_cast<WidgetId>(10 + index);
          const auto node = tree.find(builder.id(id));
          ASSERT_TRUE(node.has_value());
          EXPECT_EQ(node->label, labels[index]);
          const auto *box = scene->layout.find(id);
          ASSERT_NE(box, nullptr);
          ASSERT_TRUE(node->bounds.has_value());
          EXPECT_DOUBLE_EQ(node->bounds->left, box->rect.left);
          EXPECT_DOUBLE_EQ(node->bounds->right,
                           box->rect.left + box->rect.width);
          EXPECT_DOUBLE_EQ(node->bounds->top, metrics.screenHeight -
                                                  box->rect.bottom -
                                                  box->rect.height);
          EXPECT_DOUBLE_EQ(node->bounds->bottom,
                           metrics.screenHeight - box->rect.bottom);
        }
        const auto *button = scene->layout.find(100);
        ASSERT_NE(button, nullptr);
        const auto *hit = scene->layout.hitTest(
            button->rect.left + button->rect.width * 0.5F,
            button->rect.bottom + button->rect.height * 0.5F);
        ASSERT_NE(hit, nullptr);
        EXPECT_EQ(hit->id, 100U);
        const auto covered =
            std::ranges::any_of(device.drawn, [&](const auto &row) {
              if (scene->resolvePickingId(row.paper & 65535U) != 100U ||
                  (row.foreground & Doc::VBORow::solidFlag) == 0)
                return false;
              const auto rectangle = rectangleOf(row);
              return rectangle.left == button->rect.left &&
                     rectangle.bottom == button->rect.bottom &&
                     rectangle.width == button->rect.width &&
                     rectangle.height == button->rect.height;
            });
        EXPECT_TRUE(covered)
            << "the whole interactive box must carry its pick tag";
      }
    }
  }
}

TEST_F(ScreenOverlayTest, unchangedFramesReuseTheLayoutAndNeverShapeText) {
  using namespace gleditor::ui;
  const auto labels = longLabels();
  ASSERT_FALSE(labels.empty());
  ScreenOverlay overlay(fixturePanel(labels));
  UiMetrics metrics{.screenWidth = 1280, .screenHeight = 800};
  const Theme theme;
  overlay.deviceReady(device, {});
  draw(overlay, metrics, theme);
  const auto scene    = overlay.snapshot();
  const auto revision = overlay.layoutRevision();
  gleditor::text::ShapingStatsScope capture;
  for (int frame = 0; frame < 100; ++frame) draw(overlay, metrics, theme);
  EXPECT_EQ(overlay.snapshot(), scene);
  EXPECT_EQ(overlay.layoutRevision(), revision);
  EXPECT_EQ(capture.stats().layoutCalls, 0U);
  EXPECT_EQ(capture.stats().harfbuzzCalls, 0U);
  EXPECT_EQ(capture.stats().inputBytes, 0U);
}

TEST_F(ScreenOverlayTest, metricsThemeAndContentChangesInvalidateExactlyOnce) {
  using namespace gleditor::ui;
  Widget model{.id = 1, .model = Label{"Original content"}};
  ScreenOverlay overlay(model);
  UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  Theme theme;
  std::ignore                 = overlay.prepare(metrics, theme);
  const auto expectOneRebuild = [&] {
    const auto previous = overlay.layoutRevision();
    const auto oldScene = overlay.snapshot();
    const auto current  = overlay.prepare(metrics, theme);
    EXPECT_NE(current, oldScene);
    EXPECT_EQ(overlay.layoutRevision(), previous + 1);
    EXPECT_EQ(overlay.prepare(metrics, theme), current);
    EXPECT_EQ(overlay.layoutRevision(), previous + 1);
  };
  metrics.fontScale = 1.5F;
  expectOneRebuild();
  metrics.screenWidth  = 1280;
  metrics.screenHeight = 800;
  expectOneRebuild();
  metrics.contentScale = 1.25F;
  expectOneRebuild();
  theme.fonts[static_cast<std::size_t>(FontRole::Label)].family = "Serif";
  expectOneRebuild();
  theme.colours.text.r = 0.25F;
  expectOneRebuild();
  const auto retained = overlay.snapshot();
  model.model         = Label{"Different content"};
  overlay.setModel(model);
  expectOneRebuild();
  EXPECT_EQ(overlay.snapshot()->find(1)->accessibleLabel, "Different content");
  EXPECT_EQ(retained->find(1)->accessibleLabel, "Original content");
}

TEST_F(ScreenOverlayTest, focusDrawingRetainsTextBatchesAndStaysInsideBoxes) {
  using namespace gleditor::ui;
  Widget model{.id = 1, .model = Panel{"Focus geometry"}};
  model.children = {
      {.id = 10, .model = Button{"Accept", "accept"}},
      {.id    = 20,
       .model = TextField{.value = "A long editable field", .caret = 21}},
  };
  ScreenOverlay overlay(std::move(model));
  const UiMetrics metrics{
      .contentScale = 1.25F, .screenWidth = 641, .screenHeight = 481};
  const Theme theme;
  overlay.deviceReady(device, {});
  const auto scene = overlay.prepare(metrics, theme);
  FocusManager manager;
  const auto registration = manager.registerScope(overlay);
  draw(overlay, metrics, theme);
  const auto revision = overlay.layoutRevision();
  gleditor::text::ShapingStatsScope shaping;
  EXPECT_CALL(device, createPipeline).Times(0);
  for (int frame = 0; frame < 50; ++frame) {
    ASSERT_TRUE(manager.focusNode(frame % 2 == 0 ? 20 : 10));
    draw(overlay, metrics, theme);
    ASSERT_FALSE(device.drawn.empty());
    for (const auto &quad : device.drawn) {
      const auto id = scene->resolvePickingId(quad.paper & 65535U);
      ASSERT_TRUE(id.has_value());
      const auto *box = scene->layout.find(*id);
      ASSERT_NE(box, nullptr);
      expectContained(rectangleOf(quad), box->rect);
    }
  }
  EXPECT_EQ(overlay.layoutRevision(), revision);
  const auto counters = shaping.stats();
  EXPECT_EQ(counters.layoutCalls, 0U);
  EXPECT_EQ(counters.harfbuzzCalls, 0U);
}

TEST_F(ScreenOverlayTest, hiddenOverlaysDrawAndExposeNothing) {
  using namespace gleditor::ui;
  ScreenOverlay overlay({.id = 1, .model = Button{"Visible action", "accept"}});
  UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  const Theme theme;
  overlay.deviceReady(device, {});
  draw(overlay, metrics, theme);
  ASSERT_FALSE(device.drawn.empty());
  overlay.setVisible(false);
  draw(overlay, metrics, theme);
  EXPECT_TRUE(device.drawn.empty());
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 16);
  overlay.describe(builder);
  EXPECT_TRUE(tree.nodes.empty());
}

TEST_F(ScreenOverlayTest, fractionalSafeAreaEdgesStillContainEveryDrawnQuad) {
  using namespace gleditor::ui;
  ScreenOverlay overlay(
      {.id = 1, .model = Button{"Fractional scale", "accept"}});
  UiMetrics metrics{.contentScale = 1.25F,
                    .screenWidth  = 641,
                    .screenHeight = 481,
                    .chrome       = {.top = 22.4F, .bottom = 18.6F}};
  const Theme theme;
  overlay.deviceReady(device, {});
  const auto scene = overlay.prepare(metrics, theme);
  ASSERT_NE(scene, nullptr);
  for (const auto &box : scene->layout.boxes) {
    expectContained(box.rect, metrics.pixelSafeArea());
    expectContained(box.contentRect, box.rect);
  }
  draw(overlay, metrics, theme);
  ASSERT_FALSE(device.drawn.empty());
  for (const auto &row : device.drawn) {
    expectContained(rectangleOf(row), metrics.pixelSafeArea());
  }
}

TEST_F(ScreenOverlayTest, drawnPickingIdentityActivatesOnlyItsOwningOverlay) {
  using namespace gleditor::ui;
  ScreenOverlay overlay({.id = 7, .model = Button{"Open publication", "open"}});
  ScreenOverlay other({.id = 7, .model = Button{"Other publication", "other"}});
  UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  const Theme theme;
  std::vector<WidgetAction> actions;
  overlay.setActionHandler(
      [&](const auto &action) { actions.push_back(action); });
  overlay.deviceReady(device, {});
  draw(overlay, metrics, theme);
  ASSERT_FALSE(device.drawn.empty());
  const auto &quad = device.drawn.front();
  render::PickingResult picked;
  picked.x = static_cast<int>(quad.pos[0]);
  picked.y = metrics.screenHeight - static_cast<int>(quad.pos[1]);
  const auto identity =
      device.identities.front() |
      ((quad.quad & 3U) << (render::tagDocBits + render::tagPageBits));
  picked.tag  = render::unpackPickingTag(identity, quad.paper & 65535U, 0);
  std::ignore = other.prepare(metrics, theme);
  EXPECT_FALSE(other.picked(picked, state));
  EXPECT_TRUE(overlay.picked(picked, state));
  ASSERT_EQ(actions.size(), 1U);
  EXPECT_EQ(actions.front().id, 7U);
  EXPECT_EQ(actions.front().action, "open");
}

TEST_F(ScreenOverlayTest,
       asynchronousPicksKeepFullIdsAcrossReorderingAndRemoval) {
  using namespace gleditor::ui;
  constexpr WidgetId largeId = 65543;
  Widget model{.id = 1, .model = Panel{"Stable widget identity"}};
  model.children = {
      {.id = 7, .model = Button{"Small identity", "small"}},
      {.id = largeId, .model = Button{"Large identity", "large"}},
  };
  ScreenOverlay overlay(model);
  const UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  const Theme theme;
  std::vector<WidgetAction> actions;
  overlay.setActionHandler(
      [&](const auto &action) { actions.push_back(action); });
  overlay.deviceReady(device, {});
  draw(overlay, metrics, theme);
  const auto scene = overlay.snapshot();
  ASSERT_NE(scene->find(7)->pickingId, scene->find(largeId)->pickingId);
  render::PickingResult picked;
  picked.requestId = 99;
  bool found       = false;
  for (std::size_t index = 0; index < device.drawn.size(); ++index) {
    const auto &quad = device.drawn[index];
    if (scene->resolvePickingId(quad.paper & 65535U) != largeId) continue;
    const auto identity =
        device.identities[index] |
        ((quad.quad & 3U) << (render::tagDocBits + render::tagPageBits));
    picked.tag = render::unpackPickingTag(identity, quad.paper & 65535U, 0);
    found      = true;
    break;
  }
  ASSERT_TRUE(found);
  const auto captured    = state.overlayPickScene;
  picked.overlayWidgetId = render::resolveOverlayWidget(captured, picked.tag);
  ASSERT_EQ(picked.overlayWidgetId, largeId);
  std::ranges::reverse(model.children);
  overlay.setModel(model);
  draw(overlay, metrics, theme);
  EXPECT_EQ(overlay.snapshot()->resolvePickingId(picked.tag.clusterIndex), 7U);
  EXPECT_TRUE(overlay.picked(picked, state));
  ASSERT_EQ(actions.size(), 1U);
  EXPECT_EQ(actions.front().id, largeId);
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 16);
  overlay.describe(builder);
  ASSERT_TRUE(tree.find(builder.id(largeId)));
  EXPECT_EQ(tree.find(builder.id(largeId))->label, "Large identity");
  model.children.erase(model.children.begin());
  overlay.setModel(model);
  draw(overlay, metrics, theme);
  EXPECT_TRUE(overlay.picked(picked, state));
  EXPECT_EQ(actions.size(), 1U);
  picked.overlayWidgetId.reset();
  EXPECT_TRUE(overlay.picked(picked, state));
  EXPECT_EQ(actions.size(), 1U);
  auto invalid         = picked.tag;
  invalid.clusterIndex = 0;
  EXPECT_FALSE(render::resolveOverlayWidget(captured, invalid));
  invalid.clusterIndex = 65535;
  EXPECT_FALSE(render::resolveOverlayWidget(captured, invalid));
  invalid = picked.tag;
  ++invalid.pageIndex;
  EXPECT_FALSE(render::resolveOverlayWidget(captured, invalid));
}

TEST_F(ScreenOverlayTest, accessibilityEditingUsesTheSameActionPathAsTyping) {
  using namespace gleditor::ui;
  ScreenOverlay overlay({.id    = 7,
                         .model = TextField{.value       = "old",
                                            .placeholder = "Publication title",
                                            .action      = "rename"}});
  UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  const Theme theme;
  std::vector<WidgetAction> actions;
  overlay.setActionHandler(
      [&](const auto &action) { actions.push_back(action); });
  std::ignore = overlay.prepare(metrics, theme);
  EXPECT_TRUE(overlay.performAction(gleditor::a11y::Ids::of(16, 7),
                                    gleditor::a11y::Action::SetValue,
                                    "Title caf\xC3\xA9"));
  ASSERT_EQ(actions.size(), 1U);
  EXPECT_EQ(actions.front().id, 7U);
  EXPECT_EQ(actions.front().action, "rename");
  EXPECT_EQ(actions.front().value, "Title caf\xC3\xA9");
  const auto updated = overlay.prepare(metrics, theme);
  ASSERT_NE(updated->find(7), nullptr);
  EXPECT_EQ(updated->find(7)->value, actions.front().value);
  EXPECT_EQ(updated->find(7)->accessibleLabel, "Publication title");
}

TEST_F(ScreenOverlayTest, scrolledTextFieldsCropInkAtTheirVisibleViewport) {
  using namespace gleditor::ui;
  const std::string value =
      "A very long editable identifier with trailing text beyond the viewport";
  ScreenOverlay overlay({.id        = 7,
                         .model     = TextField{.value       = value,
                                                .placeholder = "Identifier",
                                                .caret       = value.size()},
                         .preferred = {100, 60}});
  const UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  const Theme theme;
  overlay.deviceReady(device, {});
  draw(overlay, metrics, theme);
  const auto scene   = overlay.snapshot();
  const auto *box    = scene->layout.find(7);
  const auto *visual = scene->find(7);
  ASSERT_NE(box, nullptr);
  ASSERT_NE(visual, nullptr);
  EXPECT_GT(visual->textOffsetPx, 0);
  EXPECT_FALSE(visual->fitted.truncated);
  std::size_t inkQuads = 0;
  for (const auto &quad : device.drawn) {
    if ((quad.foreground & Doc::VBORow::solidFlag) == 0) {
      ++inkQuads;
      expectContained(rectangleOf(quad), box->contentRect);
    }
  }
  EXPECT_GT(inkQuads, 0U);
}

TEST_F(ScreenOverlayTest,
       legacyFrameContextsUseTheirScreenDimensionsForLayout) {
  using namespace gleditor::ui;
  ScreenOverlay overlay({.id = 7, .model = Button{"Visible action", "accept"}});
  overlay.deviceReady(device, {});
  gleditor::FrameContext context{.state          = state,
                                 .viewProjection = projection,
                                 .screenWidth    = 640,
                                 .screenHeight   = 480,
                                 .timeline       = timeline};
  overlay.drawFrame(context);
  const auto scene = overlay.snapshot();
  ASSERT_NE(scene, nullptr);
  EXPECT_GT(scene->layout.bounds.width, 0);
  EXPECT_GT(scene->layout.bounds.height, 0);
  ASSERT_FALSE(device.drawn.empty());
  for (const auto &row : device.drawn)
    expectContained(rectangleOf(row), {0, 0, 640, 480});
}

} // namespace
