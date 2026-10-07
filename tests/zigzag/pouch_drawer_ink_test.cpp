#include "../lib/mocks/device.hpp"
#include "xudu/pouch_drawer.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <gleditor/render_state.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <map>
#include <optional>
#include <set>
#include <unistd.h>

namespace {
class IsolatedPouchConfig {
public:
  IsolatedPouchConfig() {
    if (const auto *value = std::getenv("XDG_CONFIG_HOME")) previous_ = value;
    directory_ = std::filesystem::temp_directory_path() /
                 ("pouch-ink-config-" + std::to_string(getpid()));
    std::filesystem::remove_all(directory_);
    std::filesystem::create_directories(directory_);
    setenv("XDG_CONFIG_HOME", directory_.c_str(), 1);
  }
  ~IsolatedPouchConfig() {
    if (previous_)
      setenv("XDG_CONFIG_HOME", previous_->c_str(), 1);
    else
      unsetenv("XDG_CONFIG_HOME");
    std::filesystem::remove_all(directory_);
  }

private:
  std::optional<std::string> previous_;
  std::filesystem::path directory_;
};

class PouchInkDevice : public testing::NiceMock<MockRenderDevice> {
public:
  std::uint32_t next{1};
  std::size_t uploads{}, draws{};
  std::map<std::uint32_t, std::vector<std::byte>> buffers;
  struct Drawn {
    Doc::VBORow row;
    std::uint32_t identity;
  };
  std::vector<Drawn> drawn;
  PouchInkDevice() {
    ON_CALL(*this, textureLimits())
        .WillByDefault(testing::Return(render::TextureLimits{2048, 10}));
    ON_CALL(*this, createTextureArray)
        .WillByDefault(testing::Return(render::TextureHandle{1}));
    ON_CALL(*this, createBuffer)
        .WillByDefault([this](render::BufferKind, std::size_t size) {
          const auto id = next++;
          buffers[id].resize(size);
          return render::BufferHandle{id};
        });
    ON_CALL(*this, resizeBuffer)
        .WillByDefault([this](render::BufferHandle handle, std::size_t size) {
          buffers[handle.id].resize(size);
          return handle;
        });
    ON_CALL(*this, createPipeline)
        .WillByDefault(testing::Return(render::PipelineHandle{1}));
    ON_CALL(*this, updateBuffer)
        .WillByDefault([this](render::BufferHandle buffer, std::size_t offset,
                              std::span<const std::byte> data) {
          ++uploads;
          auto &storage = buffers[buffer.id];
          storage.resize(std::max(storage.size(), offset + data.size()));
          std::memcpy(storage.data() + offset, data.data(), data.size());
        });
    ON_CALL(*this, drawGlyphs)
        .WillByDefault([this](const render::DrawUniforms &uniforms,
                              render::BufferHandle buffer, std::size_t offset,
                              std::uint32_t count) {
          if (count) ++draws;
          const auto &storage = buffers.at(buffer.id);
          for (std::uint32_t index = 0; index < count; ++index) {
            Doc::VBORow row{};
            std::memcpy(&row, storage.data() + offset + index * sizeof(row),
                        sizeof(row));
            drawn.push_back({row, uniforms.identity});
          }
        });
  }
};

TEST(PouchDrawerInkTest,
     StandardMatrixDrawsEveryVisibleLabelInsideItsSharedBox) {
  IsolatedPouchConfig isolatedConfig;
  PouchInkDevice device;
  RenderState state{&device};
  xanadu::Session session{"", std::make_shared<xanadu::UserPermascroll>()};
  xanadu::PouchDrawer drawer{session, {}};
  std::uint64_t itemId = 0x1'0000'0012ULL;
  for (const auto &zone : drawer.zones())
    zone->addItem({.itemId      = itemId++,
                   .previewText = "A full Unicode preview: 日本語 "
                                  "👩‍👩‍👧‍👦 café"});
  drawer.forge().dropLeft(
      {.span            = {.length = 100},
       .originKind      = xanadu::PouchOriginKind::ZigzagCell,
       .originCell      = 112,
       .originRankCoord = "d.sequence: a long rank coordinate"});
  drawer.forge().dropRight({.span = {.length = 100}});
  drawer.setOpen(true, false);
  drawer.deviceReady(device, {});
  ch::Timeline timeline;
  glm::mat4 projection{1};
  const auto check = [&](const gleditor::ui::UiMetrics &metrics,
                         const gleditor::ui::Theme &theme) {
    gleditor::FrameContext ctx{.state          = state,
                               .viewProjection = projection,
                               .screenWidth    = metrics.screenWidth,
                               .screenHeight   = metrics.screenHeight,
                               .timeline       = timeline,
                               .chrome         = metrics.chrome,
                               .metrics        = metrics,
                               .theme          = theme};
    device.drawn.clear();
    state.beginPickScene();
    drawer.drawFrame(ctx);
    const auto layout = drawer.focusLayout();
    ASSERT_TRUE(layout);
    std::set<std::uint32_t> layoutIds;
    for (const auto &box : layout->boxes) {
      EXPECT_TRUE(layoutIds.insert(box.id).second) << box.id;
      if (!box.parentId) continue;
      const auto *parent = layout->find(box.parentId);
      ASSERT_NE(parent, nullptr);
      EXPECT_GE(box.rect.left, parent->contentRect.left - .01F);
      EXPECT_GE(box.rect.bottom, parent->contentRect.bottom - .01F);
      EXPECT_LE(box.rect.left + box.rect.width,
                parent->contentRect.left + parent->contentRect.width + .01F);
      EXPECT_LE(box.rect.bottom + box.rect.height,
                parent->contentRect.bottom + parent->contentRect.height + .01F);
    }
    std::map<std::uint32_t, std::size_t> glyphs;
    for (const auto &drawn : device.drawn) {
      const auto &row = drawn.row;
      if (row.foreground & Doc::VBORow::solidFlag) continue;
      auto tag =
          render::unpackPickingTag(drawn.identity, row.paper & 0xFFFFU, 0);
      tag.kind = row.quad & 3U;
      const auto identity =
          render::resolveOverlayWidget(state.overlayPickScene, tag);
      ASSERT_TRUE(identity);
      const auto *box = layout->find(*identity);
      ASSERT_NE(box, nullptr);
      ++glyphs[*identity];
      const float width  = (row.quad >> 20U) & 4095U;
      const float height = (row.quad >> 8U) & 4095U;
      const float left   = row.pos[0] - width * .5F;
      const float bottom = row.pos[1] - height * .5F;
      EXPECT_GE(left, box->contentRect.left - .01F);
      EXPECT_GE(bottom, box->contentRect.bottom - .01F);
      EXPECT_LE(left + width,
                box->contentRect.left + box->contentRect.width + .01F);
      EXPECT_LE(bottom + height,
                box->contentRect.bottom + box->contentRect.height + .01F);
    }
    gleditor::a11y::Tree tree;
    gleditor::a11y::Builder builder(tree, 37);
    drawer.describe(builder);
    bool preview = false, forge = false, missingInk = false;
    std::set<std::uint64_t> nodeIds;
    for (const auto &node : tree.nodes) {
      EXPECT_TRUE(nodeIds.insert(node.id).second) << node.label;
      if (node.label.empty() || node.role == gleditor::a11y::Role::Group)
        continue;
      const auto identity = static_cast<std::uint32_t>(node.id);
      const auto *box     = layout->find(identity);
      ASSERT_NE(box, nullptr) << node.label;
      missingInk |= glyphs[identity] == 0;
      EXPECT_GT(glyphs[identity], 0U)
          << node.label << " content=" << box->contentRect.width << "x"
          << box->contentRect.height << " role=" << static_cast<int>(node.role);
      ASSERT_TRUE(node.bounds);
      EXPECT_DOUBLE_EQ(node.bounds->left, box->rect.left);
      EXPECT_DOUBLE_EQ(node.bounds->right, box->rect.left + box->rect.width);
      EXPECT_DOUBLE_EQ(node.bounds->top, metrics.screenHeight -
                                             box->rect.bottom -
                                             box->rect.height);
      EXPECT_DOUBLE_EQ(node.bounds->bottom,
                       metrics.screenHeight - box->rect.bottom);
      preview |= node.label.find("A full Unicode preview") != std::string::npos;
      forge |= node.label == "Forge Clasp";
    }
    std::string diagnostic;
    if (!preview || missingInk) {
      for (const auto &node : tree.nodes) {
        const auto *box = layout->find(static_cast<std::uint32_t>(node.id));
        if (!box) continue;
        diagnostic += "\n" + node.label + " id=" + std::to_string(box->id) +
                      " parent=" + std::to_string(box->parentId) +
                      " rect=" + std::to_string(box->rect.bottom) + "," +
                      std::to_string(box->rect.height) +
                      " content=" + std::to_string(box->contentRect.height);
      }
    }
    EXPECT_TRUE(preview) << diagnostic;
    EXPECT_FALSE(missingInk) << diagnostic;
    EXPECT_TRUE(forge);
  };
  for (const auto size :
       {std::pair{640, 480}, std::pair{1280, 800}, std::pair{2560, 1440}})
    for (const auto scale : {.8F, 1.F, 1.5F, 2.F})
      for (const auto *family :
           {"Sans", "Serif", "Monospace", "Noto Sans CJK JP"}) {
        SCOPED_TRACE(std::to_string(size.first) + "x" +
                     std::to_string(size.second) +
                     " scale=" + std::to_string(scale) + " family=" + family);
        drawer.setConfig({});
        drawer.setOpen(true, false);
        gleditor::ui::UiMetrics metrics{.fontScale    = scale,
                                        .screenWidth  = size.first,
                                        .screenHeight = size.second,
                                        .chrome = {.top = 30, .bottom = 20}};
        gleditor::ui::Theme theme;
        for (auto &font : theme.fonts) font.family = family;
        check(metrics, theme);
      }
  for (const auto size : {std::pair{640, 480}, std::pair{1280, 800}})
    for (const auto *family : {"Monospace", "Noto Sans CJK JP"}) {
      SCOPED_TRACE("narrow resize " + std::to_string(size.first) + "x" +
                   std::to_string(size.second) + " family=" + family);
      drawer.setConfig({});
      drawer.setOpen(true, false);
      gleditor::ui::UiMetrics metrics{.fontScale    = 2.F,
                                      .screenWidth  = size.first,
                                      .screenHeight = size.second,
                                      .chrome = {.top = 30, .bottom = 20}};
      gleditor::ui::Theme theme;
      for (auto &font : theme.fonts) font.family = family;
      check(metrics, theme);
      gleditor::a11y::Tree tree;
      gleditor::a11y::Builder builder(tree, 37);
      drawer.describe(builder);
      const auto resize =
          std::ranges::find_if(tree.nodes, [](const auto &node) {
            return node.label == "Resize pouch drawer";
          });
      ASSERT_NE(resize, tree.nodes.end());
      ASSERT_TRUE(drawer.performAction(resize->id,
                                       gleditor::a11y::Action::SetValue, "1"));
      check(metrics, theme);
    }
}
} // namespace
