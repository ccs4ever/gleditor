/**
 * @file doc_page_pose_test.cpp
 * @brief A page moved from outside the document keeps where it was put
 * through a reflow, and the pages after it stay where the column puts them.
 *
 * design/view-system-implementation-plan.md, finding F2 and spike R4: a
 * page's matrix is today both where reflow stacks it and where it is drawn.
 * Reflow rewrites every page from the edited one onwards from the column
 * formula (src/doc.cpp, reflowFrom()'s rebuilt and tail loops), and places
 * the first rebuilt page beneath the *live* matrix of the page before it. So
 * a page posed from outside is snapped back to the column by the next edit
 * at or above it, and a posed page directly above an edit drags everything
 * after the edit with it. These tests say what L7 must make true -- a pose
 * is separate from the flow matrix reflow owns -- and fail until it does,
 * so they are disabled until L7 lands; L7 removes the DISABLED_ prefixes.
 *
 * Page::setModel is public, but Doc hands out only const pages, so nothing
 * outside can reach a page to pose it without a cast; this fixture is a
 * friend of Doc (see doc.hpp) and poses through the page directly, which is
 * the race F2 describes. L7's setPose() replaces poseFromOutside().
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/vector_float3.hpp>
#include <glm/gtx/string_cast.hpp>
#include <glm/trigonometric.hpp>

#include <gleditor/doc.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/state.hpp>
#include <gleditor/text_source.hpp>

#include "mocks/device.hpp"

using gleditor::MemoryTextSource;
using testing::NiceMock;
using testing::Return;

namespace {

/// Enough text for a dozen or so Letter-geometry pages: room for an edit
/// with pages on both sides of it and a tail after.
std::string severalPagesOfText() {
  const std::string paragraph =
      "The quick brown fox jumps over the lazy dog. Pack my box with five "
      "dozen liquor jugs. How vexingly quick daft zebras jump!\n\n";
  std::string out;
  out.reserve(48U * 1024U);
  while (out.size() < 48U * 1024U) {
    out += paragraph;
  }
  return out;
}

/// Pages the fixture builds. Enough that an edit on page 2 leaves pages
/// after it that reflow only renumbers (the tail).
constexpr std::size_t builtPages = 6;

/// A pose nothing in the column could produce: off to the side and turned
/// about the vertical, the shape a view's page arrangement gives a page.
glm::mat4 poseBesideTheColumn(const glm::mat4 &columnPlace) {
  const auto aside =
      glm::translate(glm::mat4(1.0F), glm::vec3(900.0F, 250.0F, -40.0F));
  return glm::rotate(aside, glm::radians(30.0F), glm::vec3(0.0F, 1.0F, 0.0F)) *
         columnPlace;
}

/// Element-wise equality, with both matrices printed when they differ: a
/// failure has to show whether the page went back to the column or
/// somewhere else.
testing::AssertionResult sameMatrix(const glm::mat4 &actual,
                                    const glm::mat4 &expected) {
  constexpr float tolerance = 1e-4F;
  for (int col = 0; col < 4; col++) {
    for (int row = 0; row < 4; row++) {
      const float diff = actual[col][row] - expected[col][row];
      if (diff > tolerance || diff < -tolerance) {
        return testing::AssertionFailure()
               << "\n  actual   " << glm::to_string(actual) << "\n  expected "
               << glm::to_string(expected);
      }
    }
  }
  return testing::AssertionSuccess();
}

} // namespace

class DocPagePoseTest : public testing::Test {
protected:
  std::unique_ptr<NiceMock<MockRenderDevice>> device;
  std::unique_ptr<RenderState> state;
  AppStateRef appState;
  RendererRef renderer;

  void SetUp() override {
    device = std::make_unique<NiceMock<MockRenderDevice>>();
    ON_CALL(*device, textureLimits())
        .WillByDefault(Return(render::TextureLimits{2048, 10}));
    ON_CALL(*device,
            createTextureArray(testing::_, testing::_, testing::_, testing::_))
        .WillByDefault(Return(render::TextureHandle{1}));
    ON_CALL(*device, createBuffer(testing::_, testing::_))
        .WillByDefault(Return(render::BufferHandle{1}));
    ON_CALL(*device, resizeBuffer(testing::_, testing::_))
        .WillByDefault(Return(render::BufferHandle{1}));

    state = std::make_unique<RenderState>(device.get());

    appState                  = std::make_shared<AppState>();
    appState->defaultFontName = "Monospace 10";
    renderer = Renderer::create(appState, render::Backend::OpenGL);
  }

  /// A document with its first builtPages pages built in order, each placed
  /// by the column formula as the render thread would place it.
  std::shared_ptr<Doc> columnOfPages() {
    auto doc             = Doc::create(renderer, device.get(), glm::mat4(1.0F),
                                       MemoryTextSource(severalPagesOfText(), "pose-test"));
    std::uint32_t offset = 0;
    for (std::size_t i = 0; i < builtPages; i++) {
      auto shaping        = doc->layoutFrom(offset);
      const auto consumed = static_cast<std::uint32_t>(shaping.limit);
      EXPECT_GT(consumed, 0U) << "the text ran out before page " << i;
      doc->newPage(*state, std::move(shaping), offset);
      offset += consumed;
    }
    return doc;
  }

  /// The first byte of page @p index.
  static std::uint32_t startOf(const Doc &doc, const std::size_t index) {
    const auto page = doc.page(index);
    EXPECT_TRUE(page.has_value()) << "page " << index << " is not built";
    return page ? page->baseOffset() : 0U;
  }

  static glm::mat4 modelOf(const Doc &doc, const std::size_t index) {
    const auto page = doc.page(index);
    EXPECT_TRUE(page.has_value()) << "page " << index << " is not built";
    return page ? page->getModel() : glm::mat4(1.0F);
  }

  /// Set a page's matrix the only way there is today. Replaced by L7's
  /// setPose().
  static void poseFromOutside(Doc &doc, const std::size_t index,
                              const glm::mat4 &pose) {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    doc.pages[index]->setModel(pose);
  }

  /// Doc::insert() with its reflow run here rather than handed to the render
  /// thread's queue, which a test without a render loop has no way to pump.
  /// Everything else is insert()'s own: the same line starts recorded before
  /// the splice, the same page found to hold the edit, the same reflowFrom().
  void typeAt(Doc &doc, const std::uint32_t at, const std::string &utf8) {
    const auto oldStarts = doc.lineBreaksAround(at);
    const auto holding   = doc.pageHolding(at);
    ASSERT_TRUE(holding.has_value()) << "no built page holds byte " << at;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    const auto oldConsumed = doc.pages[*holding]->textLength();
    doc.text.insert(at, utf8);
    doc.edits++;
    doc.reflowFrom(*state, *holding, at, static_cast<std::int32_t>(utf8.size()),
                   oldStarts, oldConsumed);
  }
};

// The edited page itself is rebuilt from the column formula, so a pose on it
// is lost to the first keystroke typed into it.
TEST_F(DocPagePoseTest, DISABLED_APosedPageKeepsItsPoseWhenTextIsTypedIntoIt) {
  auto doc        = columnOfPages();
  const auto pose = poseBesideTheColumn(modelOf(*doc, 2));
  poseFromOutside(*doc, 2, pose);

  typeAt(*doc, startOf(*doc, 2) + 10, "inserted ");

  EXPECT_TRUE(sameMatrix(modelOf(*doc, 2), pose))
      << "F2: reflowFrom() rebuilt page 2 at the column formula's place, "
         "discarding the pose set on it";
}

// A page after the edit is only renumbered, but reflowFrom()'s tail loop
// still writes its matrix from the column formula.
TEST_F(DocPagePoseTest,
       DISABLED_APosedPageKeepsItsPoseWhenAPageAboveItReflows) {
  auto doc        = columnOfPages();
  const auto pose = poseBesideTheColumn(modelOf(*doc, 4));
  poseFromOutside(*doc, 4, pose);

  typeAt(*doc, startOf(*doc, 1) + 10, "inserted ");

  EXPECT_TRUE(sameMatrix(modelOf(*doc, 4), pose))
      << "F2: reflowFrom() rewrote page 4's matrix from the column formula "
         "while renumbering the pages after the edit";
}

// The first rebuilt page is stacked beneath the live matrix of the page
// before it. A pose on that page is a place in the scene, not in the column,
// and must not move the column after it.
TEST_F(DocPagePoseTest,
       DISABLED_TheColumnBelowAPosedPageIsWhereItWouldBeWithoutIt) {
  auto unposed = columnOfPages();
  typeAt(*unposed, startOf(*unposed, 2) + 10, "inserted ");

  auto posed = columnOfPages();
  poseFromOutside(*posed, 1, poseBesideTheColumn(modelOf(*posed, 1)));
  typeAt(*posed, startOf(*posed, 2) + 10, "inserted ");

  ASSERT_EQ(posed->numPages(), unposed->numPages());
  for (std::size_t i = 2; i < builtPages; i++) {
    EXPECT_TRUE(sameMatrix(modelOf(*posed, i), modelOf(*unposed, i)))
        << "F2: page " << i << " was stacked beneath posed page 1's matrix "
        << "rather than beneath its place in the column";
  }
}
