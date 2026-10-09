#include "mocks/world_device.hpp"
#include <cstring>
#include <gleditor/render_state.hpp>
#include <gleditor/text/diagnostics.hpp>
#include <gleditor/translucent_list.hpp>
#include <gleditor/ui/world_panel.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <limits>
#include <vector>

namespace {
using namespace gleditor::ui;
glm::mat4 camera(float zoom) {
  return glm::ortho(-320.F / zoom, 320.F / zoom, -240.F / zoom, 240.F / zoom,
                    -100.F, 100.F);
}
TEST(WorldPanelTest, ProjectionAndLegibilityAtThreeZoomLevels) {
  const Rect box{-50, -30, 100, 60};
  for (float zoom : {.25F, 1.F, 2.F}) {
    auto plane = projectPlane(box, camera(zoom), {640, 480});
    ASSERT_TRUE(plane);
    EXPECT_NEAR(plane->bounds.width, box.width * zoom, .001F);
    EXPECT_NEAR(plane->bounds.height, box.height * zoom, .001F);
    EXPECT_NEAR(plane->minPixelsPerUnit, zoom, .001F);
    EXPECT_EQ(labelLOD(*plane, 16, {20, 8}), zoom >= 1);
  }
  EXPECT_FALSE(projectPlane({1000, 1000, 100, 60}, camera(1), {640, 480}));
  EXPECT_FALSE(projectPlane(box, camera(1), {0, 480}));
  auto invalid  = camera(1);
  invalid[0][0] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(projectPlane(box, invalid, {640, 480}));
  const auto perspective =
      glm::perspective(glm::radians(60.F), 1.F, 1.F, 100.F);
  EXPECT_FALSE(projectPlane(box, perspective, {640, 480}));
  EXPECT_FALSE(projectPlane(
      box, perspective * glm::translate(glm::mat4(1), glm::vec3(0, 0, 2)),
      {640, 480}));
  EXPECT_TRUE(projectPlane(
      {-1, -1, 2, 2},
      perspective * glm::translate(glm::mat4(1), glm::vec3(0, 0, -10)),
      {640, 480}));
  EXPECT_FALSE(projectPlane(
      {-1, -1, 2, 2},
      perspective * glm::translate(glm::mat4(1), glm::vec3(0, 0, -.5F)),
      {640, 480}));
}
TEST(WorldPanelTest, LowLodRetainsPicksFullNamesAndProjectedAccessibility) {
  WorldRecordingDevice device;
  RenderState state(&device);
  const std::string full = "Long multilingual title مرحبا 世界 "
                           "👨‍👩‍👧‍👦 ending";
  WorldPanel panel(
      {.id       = 1,
       .model    = Panel{},
       .children = {{.id       = 2,
                     .model    = Label{full, TextPurpose::Description},
                     .maxLines = 1}}});
  panel.setBounds({-100, -50, 200, 100});
  panel.setLabelLod({0, 8});
  UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  Theme theme;
  const auto scene = panel.prepare(metrics, theme);
  ASSERT_TRUE(scene);
  EXPECT_EQ(scene->find(2)->accessibleLabel, full);
  EXPECT_LE(scene->find(2)->fitted.widthPx,
            scene->layout.find(2)->contentRect.width + .01F);
  panel.deviceReady(device, {});
  const render::PickingTag tag{.kind         = render::tagKindOverlay,
                               .docIndex     = 77,
                               .pageIndex    = 0,
                               .clusterIndex = 42};
  panel.draw(state, camera(.25F), {640, 480}, tag);
  ASSERT_FALSE(device.drawn.empty());
  for (const auto &quad : device.drawn) {
    EXPECT_NE(quad.foreground & Doc::VBORow::solidFlag, 0U);
    EXPECT_EQ(quad.paper & 65535U, 42U);
  }
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 16);
  panel.describe(builder, camera(.25F), {640, 480});
  auto node = tree.find(builder.id(2));
  ASSERT_TRUE(node);
  EXPECT_EQ(node->label, full);
  ASSERT_TRUE(node->bounds);
  auto projected = panel.projected(2, camera(.25F), {640, 480});
  ASSERT_TRUE(projected);
  EXPECT_FLOAT_EQ(node->bounds->left, projected->bounds.left);
  EXPECT_FLOAT_EQ(node->bounds->top,
                  480 - projected->bounds.bottom - projected->bounds.height);
  panel.draw(state, camera(1), {640, 480}, tag);
  const auto pipelines = device.pipelines;
  device.uploads       = 0;
  gleditor::text::ShapingStatsScope capture;
  for (unsigned i = 0; i < 10; ++i) {
    EXPECT_EQ(panel.prepare(metrics, theme), scene);
    panel.draw(state, camera(1.1F), {640, 480}, tag, .8F);
  }
  EXPECT_EQ(capture.stats().layoutCalls, 0U);
  EXPECT_EQ(capture.stats().harfbuzzCalls, 0U);
  EXPECT_EQ(device.uploads, 0U);
  theme.fonts[static_cast<std::size_t>(FontRole::Body)].family = "Serif";
  (void)panel.prepare(metrics, theme);
  panel.draw(state, camera(1), {640, 480}, tag);
  EXPECT_EQ(device.pipelines, pipelines);
}
// Handed to a translucent list, a panel draws exactly what draw() draws, in
// the same order: the list sorts the panel's sheets as one, since they share
// a plane, and keeps the order they were added in.
TEST(WorldPanelTest, AddToDrawsThroughAListWhatDrawDraws) {
  WorldRecordingDevice device;
  RenderState state(&device);
  WorldPanel panel(
      {.id       = 1,
       .model    = Panel{},
       .children = {
           {.id = 2, .model = Label{"Sheet", TextPurpose::Description}}}});
  panel.setBounds({-100, -50, 200, 100});
  const UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  ASSERT_TRUE(panel.prepare(metrics, Theme{}));
  panel.deviceReady(device, {});
  const render::PickingTag tag{.kind = render::tagKindOverlay, .docIndex = 5};
  panel.draw(state, camera(1), {640, 480}, tag, .6F);
  const auto drawn      = device.drawn;
  const auto identities = device.identities;
  ASSERT_FALSE(drawn.empty());

  device.drawn.clear();
  device.identities.clear();
  gleditor::TranslucentList list(&device);
  panel.addTo(list, render::PipelineHandle{99}, state, camera(1), {640, 480},
              tag, .6F);
  EXPECT_FALSE(list.empty());
  list.draw(state);
  ASSERT_EQ(device.drawn.size(), drawn.size());
  EXPECT_EQ(0, std::memcmp(device.drawn.data(), drawn.data(),
                           drawn.size() * sizeof(Doc::VBORow)));
  EXPECT_EQ(device.identities, identities);
  EXPECT_EQ(list.lastDraw().beams, 0U);
}
TEST(WorldPanelTest, AnEmptyCanvasHasNoBatch) {
  WorldRecordingDevice device;
  gleditor::Canvas canvas(&device, "Sans 10");
  canvas.createPipeline({}, true);
  EXPECT_FALSE(canvas.batch(camera(1)));
  canvas.addRect(0, 0, 10, 10, 0xFFFFFFFFU);
  canvas.commit();
  const auto batch = canvas.batch(camera(1), .5F);
  ASSERT_TRUE(batch);
  EXPECT_EQ(batch->instanceCount, 1U);
  EXPECT_FLOAT_EQ(batch->uniforms.opacity, .5F);
}
} // namespace
