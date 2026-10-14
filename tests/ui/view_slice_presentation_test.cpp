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
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <choreograph/Choreograph.h>
#include <glm/ext/matrix_clip_space.hpp>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/form.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/text/diagnostics.hpp>

#include "../lib/mocks/device.hpp"
#include "allocation_count.hpp"
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
  const auto before  = presentation->focusCell();
  const auto opening = presentation->notice();
  std::thread eventThread([&] {
    EXPECT_TRUE(presentation->dispatchAction("std:nav/step_x_pos"));
    EXPECT_FALSE(presentation->dispatchAction("std:ui/toggle_palette"));
  });
  eventThread.join();
  // Nothing has happened yet: both wait for the owner.
  EXPECT_EQ(presentation->focusCell(), before);
  EXPECT_EQ(fixture.posted.size(), 2U);
  EXPECT_EQ(presentation->notice(), opening);
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
    const auto pattern =
        xanadu::view::dashPattern(xanadu::ViewDrawConfig{}, style);
    EXPECT_EQ(pattern.size() % 2, 0U);
    EXPECT_FALSE(pattern.empty());
  }
  EXPECT_TRUE(xanadu::view::dashPattern(xanadu::ViewDrawConfig{},
                                        xanadu::view::DashStyle::Solid)
                  .empty());
}

// -- the compass and the view commands (plan §4.6 U5a, U7a) ------------------

namespace {

/// What the compass says, one string a point, as a reader would hear it.
std::vector<std::string>
compassWords(const ViewSlicePresentation &presentation) {
  std::vector<std::string> words;
  for (const auto &arm : presentation.compass()) {
    words.push_back(arm.words());
  }
  return words;
}

/// A presentation drawn once, so its cues are minted, and then again.
std::unique_ptr<ViewSlicePresentation> settled(Fixture &fixture, Frame &frame) {
  auto presentation = fixture.presentation();
  presentation->deviceReady(frame.device, {});
  frame.draw(*presentation);
  fixture.drain();
  frame.draw(*presentation);
  return presentation;
}

} // namespace

TEST(ViewCompassTest, ShowsEveryPointWithWhatItIsBoundTo) {
  Fixture fixture;
  Frame frame;
  const auto presentation = settled(fixture, frame);
  EXPECT_EQ(
      compassWords(*presentation),
      (std::vector<std::string>{"x  d.across", "y  d.down", "z  nothing"}));
  // Each bound arm carries the cue the dimension has in system://ui.
  const auto ui = xanadu::SystemStoreModel::fromStore(
      *fixture.system.at(SystemDocKind::UI));
  const auto cues   = xanadu::view::DimensionCues::fromSettings(ui);
  const auto arms   = presentation->compass();
  const auto across = cues.cueOf("d.across");
  ASSERT_TRUE(across.has_value());
  ASSERT_TRUE(arms[0].cue.has_value());
  EXPECT_EQ(*arms[0].cue, *across);
  EXPECT_NE(arms[0].cue, arms[1].cue);
  EXPECT_FALSE(arms[2].cue.has_value());
}

TEST(ViewCompassTest, IsDrawnOnceAndNotAgainWhileNothingChanges) {
  Fixture fixture;
  Frame frame;
  const auto presentation = settled(fixture, frame);
  EXPECT_CALL(frame.device, updateBuffer).Times(0);
  EXPECT_CALL(frame.device, createBuffer).Times(0);
  for (int i = 0; i < 20; ++i) {
    frame.draw(*presentation);
  }
  testing::Mock::VerifyAndClearExpectations(&frame.device);

  // A binding is a change the compass draws again.
  EXPECT_CALL(frame.device, updateBuffer).Times(testing::AtLeast(1));
  EXPECT_TRUE(presentation->bindTyped("z d.across"));
  frame.draw(*presentation);
}

TEST(ViewCompassTest, TheSettingHidesItAndTheTreeFollows) {
  Fixture fixture;
  Frame frame;
  const auto presentation = settled(fixture, frame);
  auto tree               = treeOf(*presentation);
  const auto listed       = [&](const std::string_view label) {
    return std::ranges::any_of(
        tree.nodes, [&](const auto &node) { return node.label == label; });
  };
  EXPECT_TRUE(listed("Binding points"));
  EXPECT_TRUE(listed("x  d.across"));
  EXPECT_TRUE(listed("z  nothing"));

  auto &ui = *fixture.system.at(SystemDocKind::UI);
  ui.repointCurrentVersion(
      xanadu::setSetting(ui, ui.primaryCurrentVersion(),
                         xanadu::settings::kViewCompassVisible, false));
  presentation->applySettings();
  tree = treeOf(*presentation);
  EXPECT_FALSE(listed("Binding points"));
  // Still bound, still on offer to code that asks.
  EXPECT_EQ(compassWords(*presentation).size(), 3U);
}

TEST(ViewCommandsTest, ATypedBindingChangesTheCompassAndTheLayout) {
  Fixture fixture;
  Frame frame;
  const auto presentation = settled(fixture, frame);

  EXPECT_TRUE(presentation->dispatchAction("std:view/bind z d.down"));
  fixture.drain();
  EXPECT_EQ(compassWords(*presentation)[2], "z  d.down");
  // d.down is on y as well, which is allowed and said.
  EXPECT_EQ(presentation->notice(),
            "Showing d.down on axis z; axis y shows it too.");
  EXPECT_EQ(presentation->currentView().z_dimension, "d.down");

  // Typed over x: the same key now walks the other dimension.
  EXPECT_TRUE(presentation->bindTyped("x d.down"));
  frame.draw(*presentation);
  EXPECT_EQ(compassWords(*presentation)[0], "x  d.down");
  EXPECT_TRUE(presentation->dispatchAction("step-x-pos"));
  EXPECT_EQ(presentation->focusCell(), fixture.column[0]);
}

TEST(ViewCommandsTest, ATypedBindingCanNameAGroup) {
  Fixture fixture;
  Frame frame;
  const auto presentation = settled(fixture, frame);
  auto *const axes        = presentation->axisSet();
  ASSERT_NE(axes, nullptr);
  const auto manifold =
      fixture.slice.rebuildManifold(fixture.slice.primaryCurrentVersion());
  const std::array members{*manifold.dimensionNamed("d.down", fixture.slice),
                           *manifold.dimensionNamed("d.across", fixture.slice)};
  ASSERT_TRUE(axes->createGroup("Both", members));

  EXPECT_TRUE(presentation->bindTyped("z Both"));
  EXPECT_EQ(compassWords(*presentation)[2], "z  Both (2)");
  EXPECT_TRUE(presentation->compass()[2].group);
}

TEST(ViewCommandsTest, ATypedBindingThatCannotBeMadeSaysWhy) {
  Fixture fixture;
  Frame frame;
  const auto presentation = settled(fixture, frame);
  const auto before       = compassWords(*presentation);

  EXPECT_FALSE(presentation->bindTyped("x d.nothing"));
  EXPECT_EQ(presentation->notice(),
            "There is no dimension or group called 'd.nothing' in this slice.");
  EXPECT_FALSE(presentation->bindTyped("q d.down"));
  EXPECT_EQ(presentation->notice(), "There is no axis q.");
  EXPECT_FALSE(presentation->bindTyped("x"));
  EXPECT_EQ(presentation->notice(),
            "Say which point and what it shows, as in 'x d.across'.");
  EXPECT_EQ(compassWords(*presentation), before);
  // Each is announced as well as shown.
  const auto tree = treeOf(*presentation);
  EXPECT_TRUE(std::ranges::any_of(tree.nodes, [&](const auto &node) {
    return node.label == presentation->notice() &&
           gleditor::a11y::Live::Polite == node.live;
  }));
}

TEST(ViewCommandsTest, SwapsTheTwoAxesOfThePlane) {
  Fixture fixture;
  Frame frame;
  const auto presentation = settled(fixture, frame);
  EXPECT_TRUE(presentation->dispatchAction("std:ui/swap_axes"));
  EXPECT_EQ(compassWords(*presentation)[0], "x  d.down");
  EXPECT_EQ(compassWords(*presentation)[1], "y  d.across");
  // The same keys now walk the other dimension.
  EXPECT_TRUE(presentation->dispatchAction("step-x-pos"));
  EXPECT_EQ(presentation->focusCell(), fixture.column[0]);
}

TEST(ViewCommandsTest, HopsToTheHeadAndTheTailOfTheRank) {
  Fixture fixture;
  Frame frame;
  const auto presentation = settled(fixture, frame);
  EXPECT_TRUE(presentation->dispatchAction("std:nav/hop_tail"));
  EXPECT_EQ(presentation->focusCell(), fixture.row.back());
  EXPECT_EQ(presentation->breadcrumbs().size(), 1U) << "a hop is one visit";
  EXPECT_TRUE(presentation->dispatchAction("hop-head"));
  EXPECT_EQ(presentation->focusCell(), fixture.row.front());
  // Already at the head: nothing further, said.
  EXPECT_TRUE(presentation->dispatchAction("hop-head"));
  EXPECT_EQ(presentation->notice(), "No cell further along d.across negward.");
}

TEST(ViewCommandsTest, TheSubviewIsSteppedAndKeptWithTheSlice) {
  Fixture fixture;
  Frame frame;
  const auto presentation = settled(fixture, frame);
  EXPECT_EQ(presentation->subviewId(), "ghosts");

  EXPECT_TRUE(presentation->dispatchAction("std:view/subview_next"));
  EXPECT_EQ(presentation->subviewId(), "plain");
  EXPECT_EQ(presentation->notice(),
            "Showing Stretch vanishing, Plain (2 of 2).");
  EXPECT_TRUE(presentation->dispatchAction("std:view/subview_next"));
  EXPECT_EQ(presentation->subviewId(), "ghosts") << "wraps";
  EXPECT_TRUE(presentation->dispatchAction("std:view/subview_prev"));
  EXPECT_EQ(presentation->subviewId(), "plain");

  // Kept in system://layout under the slice's own key, so a presentation
  // made afterwards over the same slice opens on it.
  const auto layout = xanadu::SystemStoreModel::fromStore(
      *fixture.system.at(SystemDocKind::Layout));
  EXPECT_EQ(layout.getString(xanadu::settings::sliceSubviewKey(
                                 fixture.slice.documentId().str(),
                                 xanadu::view::StretchVanishingView::kKind),
                             ""),
            "plain");
  const auto again = fixture.presentation();
  EXPECT_EQ(again->subviewId(), "plain");
}

TEST(ViewCommandsTest, TheSubviewIsAnInputToTheLayout) {
  Fixture fixture(21);
  Frame frame;
  const auto presentation = settled(fixture, frame);
  const auto ghosts       = [&] {
    std::size_t count = 0;
    for (std::size_t i = 0; i < presentation->presenter().drawnItems().size();
         ++i) {
      count += presentation->presenter().drawnAsGhost(i) ? 1U : 0U;
    }
    return count;
  };
  presentation->focusCell(fixture.row[10]);
  frame.draw(*presentation);
  const auto withGhosts = ghosts();
  ASSERT_GT(withGhosts, 0U) << "a long row runs out of the pane";

  EXPECT_TRUE(presentation->dispatchAction("std:view/subview_next"));
  frame.draw(*presentation);
  EXPECT_EQ(ghosts(), 0U);
  EXPECT_TRUE(presentation->dispatchAction("std:view/subview_prev"));
  frame.draw(*presentation);
  EXPECT_EQ(ghosts(), withGhosts);
}

TEST(ViewCommandsTest, OnlyWhatItStillCannotDoIsRefused) {
  Fixture fixture;
  Frame frame;
  const auto presentation = settled(fixture, frame);
  EXPECT_FALSE(presentation->dispatchAction("std:ui/toggle_palette"));
  EXPECT_THAT(presentation->notice(), testing::HasSubstr("cannot"));
  for (const auto *const name :
       {"std:nav/step_y_pos", "std:nav/jump_home", "std:nav/hop_tail",
        "std:ui/swap_axes", "std:view/subview_next", "std:view/bind x d.down",
        "std:view/subview_prev"}) {
    EXPECT_TRUE(presentation->dispatchAction(name)) << name;
  }
}

TEST(SlicePresentationPaletteTest, ListsEachViewsSubviewsBeneathIt) {
  xanadu::view::ViewRegistry registry;
  ASSERT_TRUE(xanadu::view::registerBuiltinViews(registry));
  const auto field = xanadu::view::slicePresentationChoice(
      registry, xanadu::view::StretchVanishingView::kKind, "plain");
  const auto kind = std::string(xanadu::view::StretchVanishingView::kKind);
  const auto view = std::ranges::find(field.optionValues, kind);
  ASSERT_NE(view, field.optionValues.end());
  const auto at = static_cast<std::size_t>(view - field.optionValues.begin());
  ASSERT_GE(field.optionValues.size(), at + 3U);
  // Under the view, in the order its descriptor gives them, set in.
  EXPECT_EQ(field.optionValues[at + 1], kind + "#ghosts");
  EXPECT_EQ(field.optionValues[at + 2], kind + "#plain");
  // A row says whose sub-view it is, so the closed combo reads
  // "Stretch vanishing: Plain" and not an indented "Plain".
  EXPECT_EQ(field.options[at + 1], "Stretch vanishing: Ghosts");
  EXPECT_EQ(field.options[at + 2], "Stretch vanishing: Plain");
  EXPECT_EQ(field.options[at], "Stretch vanishing");
  // The sub-view showing is the row that is chosen.
  EXPECT_EQ(field.optionValues[field.chosen], kind + "#plain");

  const auto choice = xanadu::view::paletteChoiceOf(kind + "#plain");
  EXPECT_EQ(choice.presentation, kind);
  EXPECT_EQ(choice.subview, "plain");
  EXPECT_EQ(xanadu::view::paletteChoiceOf("legacy").subview, "");
  EXPECT_EQ(xanadu::view::paletteValue(choice), kind + "#plain");
}

TEST(SlicePresentationPaletteTest, RecordsTheChoiceInTheLayoutPerSlice) {
  xanadu::Store layout;
  xanadu::initializeSystemStore(layout, SystemDocKind::Layout);
  const auto head = xanadu::view::recordSlicePresentation(
      layout, "slice-a",
      {.presentation = "slice.stretch-vanishing", .subview = "plain"});
  layout.repointCurrentVersion(head);
  const auto model = xanadu::SystemStoreModel::fromStore(layout);
  EXPECT_EQ(
      model.getString(xanadu::settings::slicePresentationKey("slice-a"), ""),
      "slice.stretch-vanishing");
  EXPECT_EQ(model.getString(xanadu::settings::sliceSubviewKey(
                                "slice-a", "slice.stretch-vanishing"),
                            ""),
            "plain");
  // A choice with no sub-view leaves the one the slice has.
  const auto next = xanadu::view::recordSlicePresentation(
      layout, "slice-a", {.presentation = "legacy", .subview = {}});
  layout.repointCurrentVersion(next);
  EXPECT_EQ(xanadu::SystemStoreModel::fromStore(layout).getString(
                xanadu::settings::sliceSubviewKey("slice-a",
                                                  "slice.stretch-vanishing"),
                ""),
            "plain");
  // Another view's sub-view of the same id is its own, and leaves this one's.
  const auto other = xanadu::view::recordSlicePresentation(
      layout, "slice-a", {.presentation = "slice.other", .subview = "plain"});
  layout.repointCurrentVersion(other);
  const auto both = xanadu::SystemStoreModel::fromStore(layout);
  EXPECT_EQ(both.getString(xanadu::settings::sliceSubviewKey(
                               "slice-a", "slice.stretch-vanishing"),
                           "?"),
            "plain");
  EXPECT_EQ(
      both.getString(
          xanadu::settings::sliceSubviewKey("slice-a", "slice.other"), "?"),
      "plain");
  const auto kept = xanadu::view::recordSlicePresentation(
      layout, "slice-a",
      {.presentation = "slice.stretch-vanishing", .subview = "ghosts"});
  layout.repointCurrentVersion(kept);
  EXPECT_EQ(
      xanadu::SystemStoreModel::fromStore(layout).getString(
          xanadu::settings::sliceSubviewKey("slice-a", "slice.other"), "?"),
      "plain");
}

TEST(SlicePresentationPaletteTest, CyclingStepsThroughPresentationsAndWraps) {
  const std::vector<std::string> kinds{"legacy", "slice.a", "slice.b"};
  EXPECT_EQ(xanadu::view::steppedPresentation(kinds, "legacy", 1), "slice.a");
  EXPECT_EQ(xanadu::view::steppedPresentation(kinds, "slice.b", 1), "legacy");
  EXPECT_EQ(xanadu::view::steppedPresentation(kinds, "legacy", -1), "slice.b");
  EXPECT_EQ(xanadu::view::steppedPresentation(kinds, "unknown", 1), "slice.a");
  EXPECT_EQ(xanadu::view::steppedPresentation({}, "legacy", 1), "legacy");
}

// -- what the reader sets stays set (M2 closing round, A1-A13) ----------------

namespace {

/// The slice gains d.deep, a third dimension that the second cell of the row
/// links along to a cell of its own, and a cell linked to itself on it.
struct Deep {
  CellRef cell{}, loner{};
};

Deep addDeep(Fixture &fixture) {
  auto at       = fixture.slice.primaryCurrentVersion();
  const auto dz = fixture.slice.makeDimension(at, "d.deep");
  at            = dz.version;
  at            = fixture.slice.makeCell(at, "deep");
  Deep made;
  made.cell  = fixture.slice.cellRefOf(at);
  at         = fixture.slice.makeCell(at, "loner");
  made.loner = fixture.slice.cellRefOf(at);
  at = fixture.slice.setLink(at, fixture.row[1], dz.dim, zigzag::DimVector::POS,
                             made.cell);
  at = fixture.slice.setLink(at, made.loner, dz.dim, zigzag::DimVector::POS,
                             made.loner);
  fixture.slice.repointCurrentVersion(at);
  return made;
}

/// A group of d.down then d.across, named "Both", in the placement's arena.
void makeBoth(ViewSlicePresentation &presentation, Fixture &fixture) {
  auto *const axes = presentation.axisSet();
  ASSERT_NE(axes, nullptr);
  const auto manifold =
      fixture.slice.rebuildManifold(fixture.slice.primaryCurrentVersion());
  const std::array members{*manifold.dimensionNamed("d.down", fixture.slice),
                           *manifold.dimensionNamed("d.across", fixture.slice)};
  ASSERT_TRUE(axes->createGroup("Both", members));
}

/// A presentation over the fixture's slice that is not moved to the first
/// cell of the row: where it opens is what the layout setting says.
std::unique_ptr<ViewSlicePresentation> reopened(Fixture &fixture,
                                                Frame &frame) {
  auto made = std::make_unique<ViewSlicePresentation>(
      fixture.registry, xanadu::view::StretchVanishingView::kKind,
      fixture.host());
  made->bindXuduStore(fixture.slice, fixture.slice.primaryCurrentVersion());
  made->deviceReady(frame.device, {});
  frame.draw(*made);
  fixture.drain();
  frame.draw(*made);
  return made;
}

} // namespace

TEST(ViewBindingsStayTest, ATypedGroupSurvivesAMoveToANewOrigin) {
  Fixture fixture;
  Frame frame;
  const auto presentation = settled(fixture, frame);
  makeBoth(*presentation, fixture);
  ASSERT_TRUE(presentation->bindTyped("x Both"));
  ASSERT_EQ(compassWords(*presentation)[0], "x  Both (2)");

  // A group steps along its first member, d.down: a new origin.
  EXPECT_TRUE(presentation->dispatchAction("step-x-pos"));
  EXPECT_EQ(presentation->focusCell(), fixture.column[0]);
  EXPECT_EQ(compassWords(*presentation)[0], "x  Both (2)");
  EXPECT_TRUE(presentation->compass()[0].group);
  // Arriving is not an edit of the bindings, and does not take undo away.
  EXPECT_TRUE(presentation->axisSet()->canUndo());
}

TEST(ViewBindingsStayTest, APointLeftUnboundStaysUnboundWhereverTheReaderGoes) {
  Fixture fixture;
  const auto deep = addDeep(fixture);
  Frame frame;
  const auto presentation = settled(fixture, frame);
  ASSERT_EQ(compassWords(*presentation)[2], "z  nothing");
  const auto legacyZ = presentation->currentView().z_dimension;

  // The second cell of the row links along d.deep as well.
  EXPECT_TRUE(presentation->dispatchAction("step-x-pos"));
  EXPECT_EQ(presentation->focusCell(), fixture.row[1]);
  EXPECT_EQ(compassWords(*presentation)[2], "z  nothing");
  EXPECT_EQ(presentation->currentView().z_dimension, legacyZ);
  EXPECT_THAT(presentation->notice(), testing::Not(testing::HasSubstr("deep")));
  // What is there to be bound is one command away, not done for the reader.
  EXPECT_TRUE(presentation->bindTyped("z d.deep"));
  EXPECT_TRUE(presentation->dispatchAction("step-z-pos"));
  EXPECT_EQ(presentation->focusCell(), deep.cell);
}

TEST(ViewBindingsStayTest, ASliceThatOpenedOnNothingIsBoundAndTheReaderToldSo) {
  Fixture fixture;
  Frame frame;
  // Home links along nothing a view can look along, so nothing is bound until
  // the reader arrives somewhere that does.
  const auto presentation = reopened(fixture, frame);
  EXPECT_EQ(
      compassWords(*presentation),
      (std::vector<std::string>{"x  nothing", "y  nothing", "z  nothing"}));
  presentation->focusCell(fixture.row.front());
  EXPECT_EQ(compassWords(*presentation)[0], "x  d.across");
  EXPECT_THAT(presentation->notice(),
              testing::HasSubstr("Showing d.across on axis x."));
  EXPECT_THAT(presentation->notice(),
              testing::HasSubstr("Showing d.down on axis y."));
  const auto tree = treeOf(*presentation);
  EXPECT_TRUE(std::ranges::any_of(tree.nodes, [&](const auto &node) {
    return node.label == presentation->notice() &&
           gleditor::a11y::Live::Polite == node.live;
  }));

  // From then on the reader's: unbinding by binding elsewhere is theirs.
  EXPECT_TRUE(presentation->bindTyped("y d.across"));
  presentation->focusCell(fixture.column[0]);
  EXPECT_EQ(compassWords(*presentation)[0], "x  d.across");
  EXPECT_EQ(compassWords(*presentation)[1], "y  d.across");
}

TEST(ViewBindingsStayTest,
     TheKeysAndTheLegacyBindingFollowTheNamesOfThePoints) {
  Fixture fixture;
  auto &layout = *fixture.system.at(SystemDocKind::Layout);
  layout.repointCurrentVersion(xanadu::setSetting(
      layout, layout.primaryCurrentVersion(), xanadu::settings::kBindingPoints,
      std::string{"a"}, std::string{"spatial"}, std::string{"b"},
      std::string{"spatial"}, std::string{"c"}, std::string{"spatial"}));
  Frame frame;
  const auto presentation = settled(fixture, frame);
  ASSERT_EQ(
      compassWords(*presentation),
      (std::vector<std::string>{"a  d.across", "b  d.down", "c  nothing"}));
  EXPECT_EQ(presentation->currentView().x_dimension, "d.across");

  // The hop goes along the first point of the plane, whatever it is called.
  EXPECT_TRUE(presentation->dispatchAction("std:nav/hop_tail"));
  EXPECT_EQ(presentation->focusCell(), fixture.row.back());
  EXPECT_TRUE(presentation->dispatchAction("step-A-neg"))
      << "case is not the reader's to get right";
  EXPECT_EQ(presentation->focusCell(), fixture.row[fixture.row.size() - 2]);
  // And so does the swap, which the legacy binding follows.
  EXPECT_TRUE(presentation->dispatchAction("swap-xy"));
  EXPECT_EQ(compassWords(*presentation)[0], "a  d.down");
  EXPECT_EQ(compassWords(*presentation)[1], "b  d.across");
  EXPECT_EQ(presentation->currentView().x_dimension, "d.down");
}

TEST(ViewBindingsStayTest, AHopAroundARingEndsBesideWhereItBegan) {
  Fixture fixture(3);
  {
    auto at           = fixture.slice.primaryCurrentVersion();
    const auto across = *fixture.slice.rebuildManifold(at).dimensionNamed(
        "d.across", fixture.slice);
    at = fixture.slice.setLink(at, fixture.row[2], across,
                               zigzag::DimVector::POS, fixture.row[0]);
    fixture.slice.repointCurrentVersion(at);
  }
  Frame frame;
  const auto presentation = settled(fixture, frame);
  EXPECT_TRUE(presentation->dispatchAction("hop-tail"));
  EXPECT_EQ(presentation->focusCell(), fixture.row[2])
      << "the cell before the start";
  EXPECT_TRUE(presentation->dispatchAction("hop-head"));
  // From the last cell the head is the one just after it, going round.
  EXPECT_EQ(presentation->focusCell(), fixture.row[0]);
  EXPECT_EQ(presentation->breadcrumbs().size(), 2U) << "a hop is one visit";
}

TEST(ViewBindingsStayTest, ACellLinkedToItselfIsNotAMoveAndNotAVisit) {
  Fixture fixture;
  const auto deep = addDeep(fixture);
  Frame frame;
  const auto presentation = settled(fixture, frame);
  presentation->focusCell(deep.loner);
  ASSERT_TRUE(presentation->bindTyped("x d.deep"));
  const auto walked = presentation->breadcrumbs();

  EXPECT_TRUE(presentation->dispatchAction("step-x-pos"));
  EXPECT_EQ(presentation->focusCell(), deep.loner);
  EXPECT_EQ(presentation->notice(), "No cell further along d.deep posward.");
  EXPECT_EQ(presentation->breadcrumbs(), walked);
}

TEST(ViewBindingsStayTest, ASubviewBelongsToTheViewThatOffersIt) {
  Fixture fixture;
  const auto sliceId = fixture.slice.documentId().str();
  auto &layout       = *fixture.system.at(SystemDocKind::Layout);
  const auto keep    = [&](const std::string &kind, const std::string &id) {
    auto head =
        xanadu::ensureSetting(layout, layout.primaryCurrentVersion(),
                                 xanadu::sliceSubviewSettingSpec(sliceId, kind));
    head = xanadu::setSetting(
        layout, head, xanadu::settings::sliceSubviewKey(sliceId, kind), id);
    layout.repointCurrentVersion(head);
  };
  // Another view's choice of "plain" is not this view's.
  keep("slice.another-view", "plain");
  EXPECT_EQ(fixture.presentation()->subviewId(), "ghosts");
  keep(std::string(xanadu::view::StretchVanishingView::kKind), "plain");
  EXPECT_EQ(fixture.presentation()->subviewId(), "plain");
}

TEST(ViewBindingsStayTest, BindingsAndTheFocusAreKeptWithTheSlice) {
  Fixture fixture;
  Frame frame;
  {
    const auto presentation = settled(fixture, frame);
    makeBoth(*presentation, fixture);
    ASSERT_TRUE(presentation->bindTyped("x Both"));
    ASSERT_TRUE(presentation->bindTyped("z d.across"));
    EXPECT_TRUE(presentation->dispatchAction("step-y-pos"));
    ASSERT_EQ(presentation->focusCell(), fixture.column[0]);
    frame.draw(*presentation);
  }
  Frame second;
  const auto again = reopened(fixture, second);
  EXPECT_EQ(
      compassWords(*again),
      (std::vector<std::string>{"x  Both (2)", "y  d.down", "z  d.across"}));
  EXPECT_TRUE(again->compass()[0].group);
  EXPECT_EQ(again->focusCell(), fixture.column[0]);
  EXPECT_EQ(again->notice(), "") << "nothing was lost, so nothing is said";
}

TEST(ViewBindingsStayTest, ANameThatNoLongerResolvesIsSaidWhenTheyAreRestored) {
  Fixture fixture;
  const auto sliceId = fixture.slice.documentId().str();
  auto &layout       = *fixture.system.at(SystemDocKind::Layout);
  xanadu::view::SavedBindings saved;
  saved.axes       = {{.point = "x",
                       .shows =
                           xanadu::view::SavedTarget{
                         xanadu::view::SavedDimension{.name = "d.gone"}}},
                      {.point = "y",
                       .shows = xanadu::view::SavedTarget{
                     xanadu::view::SavedDimension{.name = "d.down"}}}};
  const auto cells = xanadu::view::cellsOf(saved);
  auto head        = layout.primaryCurrentVersion();
  for (const auto &spec : xanadu::sliceBindingSettingSpecs(sliceId)) {
    head = xanadu::ensureSetting(layout, head, spec);
  }
  std::vector<xanadu::CellValue> values(cells.axes.begin(), cells.axes.end());
  head =
      xanadu::setSetting(layout, head, xanadu::settings::sliceAxesKey(sliceId),
                         std::span<const xanadu::CellValue>{values});
  layout.repointCurrentVersion(head);

  Frame frame;
  const auto presentation = reopened(fixture, frame);
  EXPECT_THAT(presentation->notice(),
              testing::HasSubstr("Dimension 'd.gone' is gone; axis x is "
                                 "unbound."));
  EXPECT_THAT(presentation->notice(),
              testing::HasSubstr("Restored 1 bindings and groups; 1 saved "
                                 "names are gone."));
  EXPECT_EQ(compassWords(*presentation)[0], "x  nothing");
  EXPECT_EQ(compassWords(*presentation)[1], "y  d.down");
}

TEST(ViewBindingsStayTest, EveryDeadEndOfAStepSaysSoInTheStatusLine) {
  Fixture fixture;
  Frame frame;
  const auto presentation = settled(fixture, frame);
  // Up and Down are steps on y, Page Up and Page Down on z.
  EXPECT_TRUE(presentation->dispatchAction("std:nav/step_y_pos"));
  EXPECT_TRUE(presentation->dispatchAction("std:nav/step_y_pos"));
  EXPECT_TRUE(presentation->dispatchAction("std:nav/step_y_pos"));
  EXPECT_EQ(presentation->focusCell(), fixture.column[1]);
  EXPECT_EQ(presentation->notice(), "No cell further along d.down posward.");
  EXPECT_TRUE(presentation->dispatchAction("std:nav/step_z_pos"));
  EXPECT_EQ(presentation->notice(), "z shows nothing; bind a dimension to it "
                                    "first.");
  EXPECT_TRUE(presentation->dispatchAction("std:nav/step_z_neg"));
  EXPECT_EQ(presentation->notice(), "z shows nothing; bind a dimension to it "
                                    "first.");
  EXPECT_EQ(presentation->focusCell(), fixture.column[1]);
  // Said to the ear as well as the eye.
  const auto tree = treeOf(*presentation);
  EXPECT_TRUE(std::ranges::any_of(tree.nodes, [&](const auto &node) {
    return node.label == presentation->notice() &&
           gleditor::a11y::Live::Polite == node.live;
  }));
  EXPECT_TRUE(presentation->bindTyped("z d.across"));
  EXPECT_TRUE(presentation->dispatchAction("std:nav/step_z_neg"));
  EXPECT_EQ(presentation->notice(), "No cell further along d.across negward.");
}

TEST(ViewBindingsStayTest, ATypedPointIsFoundWhateverTheCase) {
  Fixture fixture;
  Frame frame;
  const auto presentation = settled(fixture, frame);
  EXPECT_TRUE(presentation->bindTyped("X d.down"));
  EXPECT_EQ(compassWords(*presentation)[0], "x  d.down");
  EXPECT_TRUE(presentation->dispatchAction("step-X-pos"));
  EXPECT_EQ(presentation->focusCell(), fixture.column[0]);
}

TEST(ViewBindingsStayTest, OneDimensionOnTwoPointsIsAllowedAndSaid) {
  Fixture fixture;
  Frame frame;
  const auto presentation = settled(fixture, frame);
  EXPECT_TRUE(presentation->bindTyped("z d.across"));
  EXPECT_EQ(compassWords(*presentation)[0], "x  d.across");
  EXPECT_EQ(compassWords(*presentation)[2], "z  d.across");
  EXPECT_EQ(presentation->notice(),
            "Showing d.across on axis z; axis x shows it too.");
  // A step on either moves along it (§7.3).
  EXPECT_TRUE(presentation->dispatchAction("step-z-pos"));
  EXPECT_EQ(presentation->focusCell(), fixture.row[1]);
}

TEST(ViewBindingsStayTest, TheBindingDialogCanListWhatCanBeNamed) {
  Fixture fixture;
  Frame frame;
  const auto presentation = settled(fixture, frame);
  makeBoth(*presentation, fixture);
  const auto choices = presentation->bindingChoices();
  EXPECT_EQ(choices.points, (std::vector<std::string>{"x", "y", "z"}));
  EXPECT_EQ(choices.dimensions,
            (std::vector<std::string>{"d.across", "d.down"}));
  EXPECT_EQ(choices.groups, (std::vector<std::string>{"Both"}));
  const auto note = xanadu::view::bindingNote(choices, "Try again.");
  EXPECT_EQ(note, "Try again. Points: x, y, z. Dimensions: d.across, d.down. "
                  "Groups: Both.");
}

TEST(ViewStatusTest, TheViewAndItsSubviewAreShownWithoutAMessage) {
  Fixture fixture;
  Frame frame;
  const auto presentation = settled(fixture, frame);
  EXPECT_EQ(presentation->status(), "Stretch vanishing: Ghosts");
  const auto named = [&](const std::string_view label) {
    const auto tree = treeOf(*presentation);
    return std::ranges::any_of(
        tree.nodes, [&](const auto &node) { return node.label == label; });
  };
  EXPECT_TRUE(named("Stretch vanishing: Ghosts"));

  EXPECT_TRUE(presentation->dispatchAction("std:view/subview_next"));
  EXPECT_EQ(presentation->status(), "Stretch vanishing: Plain");
  EXPECT_TRUE(named("Stretch vanishing: Plain"));
  EXPECT_FALSE(named("Stretch vanishing: Ghosts"));
  // The transient announcement is still made, and the status stays under it.
  EXPECT_EQ(presentation->notice(),
            "Showing Stretch vanishing, Plain (2 of 2).");
  EXPECT_TRUE(named(presentation->notice()));
}

// -- the dialog
// ----------------------------------------------------------------

namespace {

/// Types @p text into the open form and accepts it.
void answer(gleditor::Form &form, const std::string_view text) {
  form.textTyped(text);
  ASSERT_TRUE(form.keyPressed(gleditor::Key::Return, gleditor::KeyMods::None));
}

std::string noteOf(gleditor::Form &form) {
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 40);
  form.describe(builder);
  std::string note;
  for (const auto &node : tree.nodes) {
    if (gleditor::a11y::Role::Label == node.role) {
      note += node.value;
    }
  }
  return note;
}

} // namespace

TEST(BindingDialogTest, ClosesOnABindingMadeAndComesBackOnARefusal) {
  gleditor::Form form("Sans 12");
  const xanadu::view::BindingChoices choices{
      .points = {"x", "y"}, .dimensions = {"d.across"}, .groups = {"Both"}};
  std::vector<std::string> tried;
  xanadu::view::openBindingCommand(
      form, choices,
      [&tried](const std::string &typed, xanadu::view::BindingResult done) {
        tried.push_back(typed);
        if ("x d.across" == typed) {
          done(std::nullopt);
        } else {
          done("There is no dimension or group called 'd.nothing' in this "
               "slice.");
        }
      });
  ASSERT_TRUE(form.isOpen());
  // What can be named is listed before anything is typed.
  EXPECT_THAT(noteOf(form), testing::HasSubstr("Points: x, y."));
  EXPECT_THAT(noteOf(form), testing::HasSubstr("Dimensions: d.across."));
  EXPECT_THAT(noteOf(form), testing::HasSubstr("Groups: Both."));

  answer(form, "x d.nothing");
  // Refused: still there, with what was typed and why it did not work.
  ASSERT_TRUE(form.isOpen());
  EXPECT_EQ(form.current().front().value, "x d.nothing");
  EXPECT_THAT(noteOf(form),
              testing::HasSubstr("There is no dimension or group called "
                                 "'d.nothing' in this slice."));
  EXPECT_THAT(noteOf(form), testing::HasSubstr("Dimensions: d.across."));

  // Mend the line and it is accepted, and the dialog goes.
  for (int i = 0; i < 11; ++i) {
    ASSERT_TRUE(
        form.keyPressed(gleditor::Key::Backspace, gleditor::KeyMods::None));
  }
  answer(form, "x d.across");
  EXPECT_FALSE(form.isOpen());
  EXPECT_EQ(tried, (std::vector<std::string>{"x d.nothing", "x d.across"}));

// -- drawing (M2 findings B2, B3, B6) ---------------------------------------

namespace {

using xanadu::view::DashStyle;
using xanadu::view::DimensionCue;
using xanadu::view::PlacedEdge;
using xanadu::view::PlacedItem;
using xanadu::view::PresentedPlacement;
using xanadu::view::PresentedWords;
using xanadu::view::SlicePresenter;

constexpr std::uint32_t kStrandColour = 0xFF0000FFU;

/// Two cells a hundred units either side of the origin joined by one strand,
/// and optionally a label lying across it.
struct StrandScene {
  PresenterDevice device;
  RenderState state{&device};
  ch::Timeline timeline;
  SlicePresenter presenter;
  xanadu::view::LayoutSink sink;

  explicit StrandScene(const bool withLabel) {
    presenter.deviceReady(device, {});
    for (const float at : {-100.0F, 100.0F}) {
      sink.push(PlacedItem{.id     = SubjectId::cell(at < 0 ? 1U : 2U),
                           .centre = {at, 0.0F, 0.0F},
                           .width  = 40.0F,
                           .height = 20.0F});
    }
    std::optional<std::uint32_t> label;
    if (withLabel) {
      label = sink.push(PlacedItem{.id     = SubjectId::label(5),
                                   .centre = {0.0F, 0.0F, 0.0F},
                                   .width  = 30.0F,
                                   .height = 12.0F});
    }
    sink.push(PlacedEdge{.from     = SubjectId::cell(1),
                         .to       = SubjectId::cell(2),
                         .a        = {-80.0F, 0.0F, 0.0F},
                         .b        = {80.0F, 0.0F, 0.0F},
                         .relation = 5,
                         .label    = label});
  }

  void draw(const DashStyle dash = DashStyle::Solid) {
    const auto text = [](const SubjectId) { return std::string_view{"x"}; };
    const auto cue  = [dash](const std::uint64_t) {
      return DimensionCue{.colour = kStrandColour, .dash = dash};
    };
    const auto camera = flatCamera();
    state.beginPickScene();
    gleditor::FrameContext ctx{state, camera, kWidth, kHeight, timeline};
    presenter.drawScene(ctx,
                        PresentedPlacement{.records      = sink,
                                           .stamp        = ++stamp_,
                                           .edgeWidth    = 1.0F,
                                           .clipMarginPx = 0.0F,
                                           .ghostOpacity = 0.09F},
                        PresentedWords{.text = text, .cue = cue});
  }

private:
  std::uint64_t stamp_{};
};

} // namespace

TEST(ViewPresenterTest, AStrandStopsShortOfALabelAcrossItsWay) {
  StrandScene bare(false);
  bare.draw();
  ASSERT_EQ(bare.presenter.strands().size(), 1U);

  StrandScene labelled(true);
  labelled.draw();
  const auto strands = labelled.presenter.strands();
  ASSERT_EQ(strands.size(), 2U) << "one strand either side of the label";
  const float clear = labelled.presenter.metrics().px(
      labelled.presenter.drawConfig().strandLabelClearPx);
  for (const auto &row : strands) {
    // The label spans x in [-15, 15], and the strand keeps its clearance.
    const float near = std::min(std::abs(row.from[0]), std::abs(row.to[0]));
    EXPECT_GE(near, 15.0F + clear - 1.0e-3F);
  }
}

TEST(ViewPresenterTest, ADashedStrandKeepsItsDashesAroundALabel) {
  StrandScene dashed(true);
  dashed.draw(DashStyle::Dash);
  StrandScene bare(false);
  bare.draw(DashStyle::Dash);
  EXPECT_GT(bare.presenter.strands().size(), 2U);
  EXPECT_LT(dashed.presenter.strands().size(), bare.presenter.strands().size());
  EXPECT_GT(dashed.presenter.strands().size(), 2U);
  for (const auto &row : dashed.presenter.strands()) {
    const float lo = std::min(row.from[0], row.to[0]);
    const float hi = std::max(row.from[0], row.to[0]);
    EXPECT_FALSE(lo < 0.0F && hi > 0.0F) << "a dash crosses the label";
  }
}

TEST(ViewPresenterTest, AStrandIsNeverThinnerOrFainterThanItsFloors) {
  StrandScene scene(false);
  // A hairline edge between cells that fade: the layout's own opacity.
  scene.sink.clear();
  for (const float at : {-100.0F, 100.0F}) {
    scene.sink.push(PlacedItem{.id      = SubjectId::cell(at < 0 ? 1U : 2U),
                               .centre  = {at, 0.0F, 0.0F},
                               .width   = 40.0F,
                               .height  = 20.0F,
                               .opacity = 0.3F});
  }
  scene.sink.push(PlacedEdge{.from     = SubjectId::cell(1),
                             .to       = SubjectId::cell(2),
                             .a        = {-80.0F, 0.0F, 0.0F},
                             .b        = {80.0F, 0.0F, 0.0F},
                             .relation = 5,
                             .opacity  = 0.3F});
  scene.draw();
  ASSERT_EQ(scene.presenter.strands().size(), 1U);
  const auto &row  = scene.presenter.strands().front();
  const auto &draw = scene.presenter.drawConfig();
  // One unit a pixel on the flat camera, so the least width is in pixels.
  EXPECT_GE(row.width,
            scene.presenter.metrics().px(draw.strandMinWidthPx) - 1.0e-3F);
  const float alpha = static_cast<float>(row.colour & 0xFFU) / 255.0F;
  EXPECT_GE(alpha, draw.strandOpacityFloor - 0.01F);

  // A ghost's strand keeps its own faintness: it must not outshine a cell.
  scene.sink.clear();
  scene.sink.push(PlacedEdge{.from     = SubjectId::cell(1),
                             .to       = SubjectId::cell(2),
                             .a        = {-80.0F, 0.0F, 0.0F},
                             .b        = {80.0F, 0.0F, 0.0F},
                             .relation = 5,
                             .opacity  = 0.05F});
  scene.draw();
  ASSERT_EQ(scene.presenter.strands().size(), 1U);
  EXPECT_LT(
      static_cast<float>(scene.presenter.strands().front().colour & 0xFFU) /
          255.0F,
      draw.strandOpacityFloor);
}

// Governance: a hot loop allocates nothing. What a frame that animates costs
// beyond one that does not is what the presenter and the animation add; the
// picking scene the host owns is rebuilt, node by node, by both.
TEST(ViewPresenterTest, AnAnimatingFrameAllocatesNoMoreThanASettledOne) {
  Fixture fixture(6, false);
  auto &ui = *fixture.system.at(SystemDocKind::UI);
  ui.repointCurrentVersion(
      xanadu::setSetting(ui, ui.primaryCurrentVersion(),
                         xanadu::settings::kViewMotionStepMs, 60000.0));
  const auto presentation = fixture.presentation();
  Frame frame;
  presentation->deviceReady(frame.device, {});
  for (int i = 0; i < 400 && (i < 2 || presentation->busy()); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    frame.draw(*presentation);
  }
  ASSERT_FALSE(presentation->busy());
  frame.draw(*presentation);
  std::size_t settled = 0;
  {
    allocation_count::Scope counted;
    frame.draw(*presentation);
    settled = allocation_count::Scope::made();
  }

  ASSERT_TRUE(presentation->dispatchAction("step-x-pos"));
  // The first frames of a move size what the presenter keeps.
  for (int i = 0; i < 3; ++i) {
    frame.draw(*presentation);
  }
  ASSERT_TRUE(presentation->busy());
  const auto rebuilds = presentation->presenter().rebuilds();
  for (int i = 0; i < 20; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    allocation_count::Scope counted;
    frame.draw(*presentation);
    EXPECT_LE(allocation_count::Scope::made(), settled) << "frame " << i;
  }
  EXPECT_GE(presentation->presenter().rebuilds(), rebuilds + 20)
      << "every one of those frames rebuilt";
}
