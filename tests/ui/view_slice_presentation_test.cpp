/**
 * @file view_slice_presentation_test.cpp
 * @brief The new slice presentation and its presenter (plan §4.6, U2):
 *        retained by stamp, picks and accessibility by SubjectId, commands on
 *        the owner thread, the switch beside the legacy visualizer, and the
 *        settings and cues it mints.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <choreograph/Choreograph.h>
#include <glm/ext/matrix_clip_space.hpp>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/text/diagnostics.hpp>

#include "../lib/mocks/device.hpp"
#include "common/ui/slice/zigzag_visualizer.hpp"
#include "common/ui/view/presentation_palette.hpp"
#include "common/ui/view/slice_presentation_switch.hpp"
#include "common/ui/view/view_presenter.hpp"
#include "common/ui/view/view_slice_presentation.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/view/builtin_views.hpp"
#include "common/xanadu/view/slice/stretch_vanishing_view.hpp"

namespace {

using xanadu::SystemDocKind;
using xanadu::view::ContentMode;
using xanadu::view::SubjectId;
using xanadu::view::SubjectKind;
using xanadu::view::ViewSlicePresentation;
using zigzag::CellRef;

constexpr int kWidth  = 900;
constexpr int kHeight = 600;

class PresenterDevice : public testing::NiceMock<MockRenderDevice> {
public:
  PresenterDevice() {
    ON_CALL(*this, textureLimits())
        .WillByDefault(testing::Return(render::TextureLimits{2048, 10}));
    ON_CALL(*this, createTextureArray)
        .WillByDefault(testing::Return(render::TextureHandle{1}));
    ON_CALL(*this, createBuffer)
        .WillByDefault(testing::Return(render::BufferHandle{1}));
    ON_CALL(*this, createPipeline)
        .WillByDefault(testing::Return(render::PipelineHandle{1}));
  }
};

/// A row of cells along d.across and a column along d.down from the first,
/// and the three system xanadocs the presentation reads.
struct Fixture {
  xanadu::Store slice;
  std::vector<CellRef> row, column;
  std::map<SystemDocKind, std::unique_ptr<xanadu::Store>> system;
  std::deque<std::function<void()>> posted;
  xanadu::view::ViewRegistry registry;

  explicit Fixture(const std::size_t across = 4, const bool reduced = true) {
    auto at        = slice.sliceGenesis(xanadu::MicroversionId{});
    const auto dx  = slice.makeDimension(at, "d.across");
    at             = dx.version;
    const auto dy  = slice.makeDimension(at, "d.down");
    at             = dy.version;
    const auto add = [&](const std::string &words) {
      at = slice.makeCell(at, words);
      return slice.cellRefOf(at);
    };
    for (std::size_t i = 0; i < across; ++i) {
      row.push_back(add(i == 0 ? "origin"
                               : "across " + std::to_string(i) +
                                     " with words enough to wrap a line or "
                                     "two in a cell of the row"));
    }
    for (std::size_t i = 1; i < 3; ++i) {
      column.push_back(add("down " + std::to_string(i)));
    }
    for (std::size_t i = 0; i + 1 < row.size(); ++i) {
      at =
          slice.setLink(at, row[i], dx.dim, zigzag::DimVector::POS, row[i + 1]);
    }
    CellRef above = row.front();
    for (const auto cell : column) {
      at    = slice.setLink(at, above, dy.dim, zigzag::DimVector::POS, cell);
      above = cell;
    }
    slice.repointCurrentVersion(at);
    for (const auto kind :
         {SystemDocKind::Settings, SystemDocKind::Layout, SystemDocKind::UI}) {
      auto store = std::make_unique<xanadu::Store>();
      xanadu::initializeSystemStore(*store, kind);
      system.emplace(kind, std::move(store));
    }
    if (reduced) {
      auto &ui = *system.at(SystemDocKind::UI);
      ui.repointCurrentVersion(
          xanadu::setSetting(ui, ui.primaryCurrentVersion(),
                             xanadu::settings::kViewMotionReduced, true));
    }
    EXPECT_TRUE(xanadu::view::registerBuiltinViews(registry));
  }

  ViewSlicePresentation::Host host() {
    return {.post =
                [this](std::function<void()> task) {
                  posted.push_back(std::move(task));
                },
            .systemStore = [this](const SystemDocKind kind) -> xanadu::Store * {
              const auto found = system.find(kind);
              return found == system.end() ? nullptr : found->second.get();
            },
            .repoint =
                [this](const SystemDocKind kind,
                       const xanadu::MicroversionId &version) {
                  system.at(kind)->repointCurrentVersion(version);
                }};
  }

  std::unique_ptr<ViewSlicePresentation> presentation() {
    auto made = std::make_unique<ViewSlicePresentation>(
        registry, xanadu::view::StretchVanishingView::kKind, host());
    made->bindXuduStore(slice, slice.primaryCurrentVersion());
    // The slice's home links along d.dims alone; the reader starts where
    // the content is.
    made->focusCell(row.front());
    return made;
  }

  void drain() {
    while (!posted.empty()) {
      auto task = std::move(posted.front());
      posted.pop_front();
      task();
    }
  }
};

/// A camera on +z that shows the plane z = 0 one unit per pixel.
glm::mat4 flatCamera(const int width = kWidth, const int height = kHeight) {
  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);
  return glm::ortho(-w / 2.0F, w / 2.0F, -h / 2.0F, h / 2.0F, -1000.0F,
                    1000.0F);
}

struct Frame {
  PresenterDevice device;
  RenderState state{&device};
  ch::Timeline timeline;
  glm::mat4 camera;

  explicit Frame(const glm::mat4 &aCamera = flatCamera()) : camera(aCamera) {}

  void draw(ViewSlicePresentation &presentation, const int width = kWidth,
            const int height = kHeight) {
    state.beginPickScene();
    gleditor::FrameContext ctx{state, camera, width, height, timeline};
    presentation.drawScene(ctx);
    presentation.drawFrame(ctx);
  }
};

std::optional<std::size_t> itemOf(const ViewSlicePresentation &presentation,
                                  const SubjectId subject) {
  const auto items = presentation.presenter().drawnItems();
  const auto found =
      std::ranges::find(items, subject, &xanadu::view::PlacedItem::id);
  if (found == items.end()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(found - items.begin());
}

gleditor::a11y::Tree treeOf(ViewSlicePresentation &presentation) {
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 40);
  presentation.describe(builder);
  return tree;
}

} // namespace

TEST(ViewSlicePresentationTest, SeedsTheViewsSettingsAndMintsCuesWhenBound) {
  Fixture fixture;
  const auto presentation = fixture.presentation();
  const auto settings     = xanadu::SystemStoreModel::fromStore(
      *fixture.system.at(SystemDocKind::Settings));
  for (const auto &spec : xanadu::view::stretchSettingSpecs()) {
    EXPECT_TRUE(settings.find(spec.name)) << spec.name;
  }
  Frame frame;
  presentation->deviceReady(frame.device, {});
  frame.draw(*presentation);
  fixture.drain();
  const auto ui = xanadu::SystemStoreModel::fromStore(
      *fixture.system.at(SystemDocKind::UI));
  EXPECT_TRUE(ui.find(xanadu::settings::dimensionColourKey("d.across")));
  EXPECT_TRUE(ui.find(xanadu::settings::dimensionColourKey("d.down")));
}

TEST(ViewSlicePresentationTest, LaysOutTheSliceAroundItsHome) {
  Fixture fixture;
  const auto presentation = fixture.presentation();
  Frame frame;
  presentation->deviceReady(frame.device, {});
  frame.draw(*presentation);
  EXPECT_EQ(presentation->focusCell(), fixture.row.front());
  EXPECT_EQ(presentation->currentView().x_dimension, "d.across");
  EXPECT_EQ(presentation->currentView().y_dimension, "d.down");
  for (const auto cell : {fixture.row[0], fixture.row[1], fixture.column[0]}) {
    EXPECT_TRUE(itemOf(*presentation, SubjectId::cell(cell))) << cell;
  }
  // The focus is drawn at the placement's origin, where the host centres.
  const auto anchor = presentation->cellAnchor(fixture.row.front());
  ASSERT_TRUE(anchor);
  EXPECT_FLOAT_EQ(anchor->position.x, presentation->focusCentre()->x);
}

// V-R44: a frame in which nothing changed lays out nothing, shapes nothing
// and uploads nothing; it re-submits what is on the device.
TEST(ViewSlicePresentationTest, ASettledFrameShapesAndUploadsNothing) {
  Fixture fixture;
  const auto presentation = fixture.presentation();
  Frame frame;
  presentation->deviceReady(frame.device, {});
  frame.draw(*presentation);
  fixture.drain();
  frame.draw(*presentation);
  ASSERT_FALSE(presentation->busy());
  const auto layouts  = presentation->layouts();
  const auto rebuilds = presentation->presenter().rebuilds();
  const auto misses   = presentation->presenter().shapingStats().misses;
  {
    gleditor::text::ShapingStatsScope counters;
    EXPECT_CALL(frame.device, updateBuffer).Times(0);
    EXPECT_CALL(frame.device, createBuffer).Times(0);
    for (int i = 0; i < 50; ++i) {
      frame.draw(*presentation);
    }
    EXPECT_EQ(counters.stats().layoutCalls, 0U);
    EXPECT_EQ(counters.stats().harfbuzzCalls, 0U);
    testing::Mock::VerifyAndClearExpectations(&frame.device);
  }
  EXPECT_EQ(presentation->layouts(), layouts);
  EXPECT_EQ(presentation->presenter().rebuilds(), rebuilds);
  EXPECT_EQ(presentation->presenter().shapingStats().misses, misses);

  // A step is a change: laid out, rebuilt and uploaded again.
  EXPECT_CALL(frame.device, updateBuffer).Times(testing::AtLeast(1));
  EXPECT_TRUE(presentation->dispatchAction("step-x-pos"));
  frame.draw(*presentation);
  EXPECT_GT(presentation->layouts(), layouts);
  EXPECT_GT(presentation->presenter().rebuilds(), rebuilds);
}

TEST(ViewSlicePresentationTest, MovesAreAnimatedUntilTheySettle) {
  Fixture fixture(4, false);
  const auto presentation = fixture.presentation();
  Frame frame;
  presentation->deviceReady(frame.device, {});
  frame.draw(*presentation);
  EXPECT_TRUE(presentation->busy()) << "the first layout fades in";
  for (int i = 0; i < 200 && presentation->busy(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    frame.draw(*presentation);
  }
  EXPECT_FALSE(presentation->busy());
  EXPECT_TRUE(presentation->dispatchAction("step-x-pos"));
  frame.draw(*presentation);
  EXPECT_TRUE(presentation->busy());
}

// V-R43: fitting changes what is painted, never the name.
TEST(ViewSlicePresentationTest, AShortenedCellKeepsItsFullTextAsItsName) {
  Fixture fixture(12);
  // A wide fade band and an abbreviation threshold just under full opacity,
  // so every cell that fades at all is drawn shortened.
  auto &settings = *fixture.system.at(SystemDocKind::Settings);
  auto head      = settings.primaryCurrentVersion();
  for (const auto &spec : xanadu::view::stretchSettingSpecs()) {
    head = xanadu::ensureSetting(settings, head, spec);
  }
  head = xanadu::setSetting(settings, head,
                            xanadu::view::stretch_settings::kFadeBand, 0.9);
  head = xanadu::setSetting(
      settings, head, xanadu::view::stretch_settings::kAbbreviateBelow, 0.99);
  settings.repointCurrentVersion(head);
  const auto presentation = fixture.presentation();
  Frame frame(flatCamera(420, kHeight));
  presentation->deviceReady(frame.device, {});
  frame.draw(*presentation, 420, kHeight);
  const auto &presenter = presentation->presenter();
  const auto items      = presenter.drawnItems();
  std::size_t shortened = 0;
  const auto tree       = treeOf(*presentation);
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (SubjectKind::Cell != items[i].id.kind || presenter.drawnAsGhost(i) ||
        ContentMode::Full == presenter.drawnContent(i)) {
      continue;
    }
    ++shortened;
    const auto full =
        fixture.slice.rebuildManifold(fixture.slice.primaryCurrentVersion())
            .textOf(static_cast<CellRef>(items[i].id.value), fixture.slice);
    EXPECT_TRUE(std::ranges::any_of(tree.nodes, [&](const auto &node) {
      return node.label == full;
    })) << full;
  }
  EXPECT_GT(shortened, 0U) << "nothing was drawn shortened; the test proves "
                              "nothing";
}

TEST(ViewSlicePresentationTest, TheAccessibilityTreeListsThePlacedCells) {
  Fixture fixture;
  const auto presentation = fixture.presentation();
  Frame frame;
  presentation->deviceReady(frame.device, {});
  frame.draw(*presentation);
  const auto tree = treeOf(*presentation);
  const auto manifold =
      fixture.slice.rebuildManifold(fixture.slice.primaryCurrentVersion());
  const auto &presenter = presentation->presenter();
  std::size_t cells     = 0;
  for (std::size_t i = 0; i < presenter.drawnItems().size(); ++i) {
    const auto &item = presenter.drawnItems()[i];
    if (SubjectKind::Cell != item.id.kind || presenter.drawnAsGhost(i)) {
      continue;
    }
    ++cells;
    const auto words =
        manifold.textOf(static_cast<CellRef>(item.id.value), fixture.slice);
    const auto node = std::ranges::find_if(tree.nodes, [&](const auto &each) {
      return each.label == words && gleditor::a11y::Role::ListItem == each.role;
    });
    ASSERT_NE(node, tree.nodes.end()) << words;
    EXPECT_TRUE(node->bounds.has_value()) << words;
    EXPECT_THAT(node->description, testing::StartsWith("cell"));
  }
  EXPECT_GE(cells, 3U);
  // The focus's node names its neighbours along the bound dimensions.
  const auto origin = std::ranges::find(tree.nodes, std::string("origin"),
                                        &gleditor::a11y::Node::label);
  ASSERT_NE(origin, tree.nodes.end());
  EXPECT_THAT(origin->description, testing::HasSubstr("d.across posward"));
  // Nothing drawn, nothing in the tree.
  EXPECT_FALSE(std::ranges::any_of(tree.nodes, [&](const auto &node) {
    return node.label == "down 2" &&
           !itemOf(*presentation, SubjectId::cell(fixture.column[1]));
  }));
}

TEST(ViewSlicePresentationTest, PicksMapBackToSubjectsAndStaleOnesAreDropped) {
  Fixture fixture;
  const auto presentation = fixture.presentation();
  Frame frame;
  presentation->deviceReady(frame.device, {});
  frame.draw(*presentation);
  const auto target = fixture.row[1];
  render::PickingResult pick;
  for (const auto &[key, semantic] : frame.state.overlayPickScene.overlays) {
    if (semantic && semantic->cellRef == target) {
      pick.tag.kind      = static_cast<std::uint32_t>(key >> 60U);
      pick.tag.docIndex  = static_cast<std::uint32_t>((key >> 46U) & 0x3FFFU);
      pick.tag.pageIndex = static_cast<std::uint32_t>((key >> 32U) & 0x3FFFU);
      pick.tag.clusterIndex = static_cast<std::uint32_t>(key & 0xFFFFFFFFU);
      pick.semanticTarget   = semantic;
    }
  }
  ASSERT_TRUE(pick.semanticTarget) << "no pick was bound for the neighbour";
  EXPECT_EQ(presentation->presenter().subjectOf(pick), SubjectId::cell(target));

  // Requested before a move and answered after it: names nothing.
  auto stale = pick;
  EXPECT_TRUE(presentation->dispatchAction("step-y-pos"));
  frame.draw(*presentation);
  EXPECT_FALSE(presentation->presenter().subjectOf(stale));
  EXPECT_FALSE(presentation->picked(stale, frame.state));
  EXPECT_EQ(presentation->focusCell(), fixture.column[0]);

  // A fresh pick on a cell focuses it.
  render::PickingResult fresh;
  for (const auto &[key, semantic] : frame.state.overlayPickScene.overlays) {
    if (semantic && semantic->cellRef == fixture.row.front()) {
      fresh.tag.kind     = render::tagKindOverlay;
      fresh.tag.docIndex = static_cast<std::uint32_t>((key >> 46U) & 0x3FFFU);
      fresh.tag.clusterIndex = static_cast<std::uint32_t>(key & 0xFFFFFFFFU);
      fresh.semanticTarget   = semantic;
    }
  }
  ASSERT_TRUE(fresh.semanticTarget);
  EXPECT_TRUE(presentation->picked(fresh, frame.state));
  EXPECT_EQ(presentation->focusCell(), fixture.row.front());
}

TEST(ViewSlicePresentationTest, CommandsRunOnTheOwnerThread) {
  Fixture fixture;
  const auto presentation = fixture.presentation();
  Frame frame;
  presentation->deviceReady(frame.device, {});
  frame.draw(*presentation); // this thread is the owner from here on
  fixture.drain();
  const auto before = presentation->focusCell();
  std::thread eventThread([&] {
    EXPECT_TRUE(presentation->dispatchAction("std:nav/step_x_pos"));
    EXPECT_FALSE(presentation->dispatchAction("std:ui/toggle_palette"));
  });
  eventThread.join();
  // Nothing has happened yet: both wait for the owner.
  EXPECT_EQ(presentation->focusCell(), before);
  EXPECT_EQ(fixture.posted.size(), 2U);
  EXPECT_TRUE(presentation->notice().empty());
  fixture.drain();
  EXPECT_EQ(presentation->focusCell(), fixture.row[1]);
  EXPECT_THAT(presentation->notice(),
              testing::HasSubstr("cannot toggle palette yet"));
  EXPECT_EQ(
      presentation->breadcrumbs(),
      (std::vector<std::string>{std::string(
          fixture.slice.rebuildManifold(fixture.slice.primaryCurrentVersion())
              .textOf(fixture.row[1], fixture.slice))}));
  // On the owner itself a command runs at once.
  EXPECT_TRUE(presentation->dispatchAction("step-x-neg"));
  EXPECT_EQ(presentation->focusCell(), fixture.row.front());
  EXPECT_TRUE(fixture.posted.empty());
}

TEST(ViewSlicePresentationTest, NothingThatWayIsSaidNotSwallowed) {
  Fixture fixture;
  const auto presentation = fixture.presentation();
  Frame frame;
  presentation->deviceReady(frame.device, {});
  frame.draw(*presentation);
  EXPECT_TRUE(presentation->dispatchAction("step-x-neg"));
  EXPECT_EQ(presentation->notice(), "No cell further along d.across negward.");
  const auto tree = treeOf(*presentation);
  const auto said = std::ranges::find(tree.nodes, presentation->notice(),
                                      &gleditor::a11y::Node::label);
  ASSERT_NE(said, tree.nodes.end());
  EXPECT_EQ(said->live, gleditor::a11y::Live::Polite);
}

TEST(SlicePresentationSwitchTest, TheLegacyIsShownUntilAnotherIsChosen) {
  Fixture fixture;
  const auto legacy = std::make_shared<zigzag::ZigzagVisualizer>("");
  legacy->bindXuduStore(fixture.slice, fixture.slice.primaryCurrentVersion());
  std::shared_ptr<ViewSlicePresentation> fresh = fixture.presentation();
  xanadu::view::SlicePresentationSwitch presentations(legacy);
  ASSERT_TRUE(presentations.add(
      std::string(xanadu::view::StretchVanishingView::kKind), fresh,
      [fresh](xanadu::view::SlicePresentation &,
              const xanadu::view::SlicePresentation &leaving) {
        fresh->adoptAxes(leaving.currentView());
      }));
  EXPECT_EQ(presentations.shownKind(),
            xanadu::settings::kSlicePresentationLegacy);
  EXPECT_FALSE(fresh->presentationVisible());
  EXPECT_FALSE(presentations.show("slice.nothing-installed"));

  legacy->focusCell(fixture.row[1]);
  ASSERT_TRUE(presentations.show(xanadu::view::StretchVanishingView::kKind));
  EXPECT_TRUE(fresh->presentationVisible());
  EXPECT_FALSE(legacy->presentationVisible());
  EXPECT_EQ(presentations.focusCell(), fixture.row[1]);
  EXPECT_EQ(&presentations.manifold(), &fresh->manifold());
  // The renderer's roles follow what is shown, and the keyboard's scope
  // stays the visualizer's own.
  EXPECT_EQ(presentations.focusScope(), legacy->focusScope());

  EXPECT_TRUE(presentations.dispatchAction("step-x-pos"));
  EXPECT_EQ(presentations.focusCell(), fixture.row[2]);
  EXPECT_FALSE(presentations.dispatchAction("toggle-palette"));
  EXPECT_THAT(fresh->notice(), testing::HasSubstr("Stretch vanishing cannot"));

  ASSERT_TRUE(presentations.show(xanadu::settings::kSlicePresentationLegacy));
  EXPECT_EQ(legacy->focusCellId(), fixture.row[2]);
}

TEST(SlicePresentationPaletteTest, ListsTheVisualizerAndEverySliceView) {
  xanadu::view::ViewRegistry registry;
  ASSERT_TRUE(xanadu::view::registerBuiltinViews(registry));
  const auto field = xanadu::view::slicePresentationChoice(
      registry, xanadu::view::StretchVanishingView::kKind);
  ASSERT_GE(field.optionValues.size(), 2U);
  EXPECT_EQ(field.optionValues.front(),
            xanadu::settings::kSlicePresentationLegacy);
  EXPECT_EQ(field.optionValues[field.chosen],
            xanadu::view::StretchVanishingView::kKind);
  for (const auto &view : registry.views()) {
    EXPECT_EQ(std::ranges::contains(field.optionValues, view.kind),
              xanadu::view::ViewSubject::Slice == view.subject)
        << view.kind;
  }
}

TEST(ViewPresenterTest, MessagesFillTheirFields) {
  const std::array fields{xanadu::view::MessageField{"dimension", "d.x"},
                          xanadu::view::MessageField{"direction", "posward"}};
  EXPECT_EQ(xanadu::view::spokenMessage(
                xanadu::view::ViewMessage::NothingThatWay, fields),
            "No cell further along d.x posward.");
  EXPECT_EQ(
      xanadu::view::spokenMessage(xanadu::view::ViewMessage::AxisUnknown, {}),
      "There is no axis {axis}.");
  for (const auto style :
       {xanadu::view::DashStyle::Dash, xanadu::view::DashStyle::Dot,
        xanadu::view::DashStyle::DashDotDot, xanadu::view::DashStyle::DashDot,
        xanadu::view::DashStyle::LongShort}) {
    const auto pattern = xanadu::view::dashPattern(style);
    EXPECT_EQ(pattern.size() % 2, 0U);
    EXPECT_FALSE(pattern.empty());
  }
  EXPECT_TRUE(
      xanadu::view::dashPattern(xanadu::view::DashStyle::Solid).empty());
}
