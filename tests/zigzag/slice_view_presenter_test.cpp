#include "../lib/mocks/world_device.hpp"
#include <gleditor/a11y/tree.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>

#include "common/ui/view/slice/acme_presenter.hpp"
#include "common/ui/view/slice/slice_view_presenter.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/view/slice/acme_view.hpp"
#include "common/xanadu/view/slice/lattice_view.hpp"
#include "common/xanadu/view/view_manifold.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

using namespace xanadu;
using namespace xanadu::ui;
using namespace xanadu::view;

TEST(SliceViewPresenterTest, LayoutUpdatePickingAndAccessibility) {
  zigzag::ArenaManifold arena;
  const auto d1 = arena.ensureDimension("d.1");
  const auto d2 = arena.ensureDimension("d.2");

  const auto c0 = arena.makeCell();
  const auto c1 = arena.makeCell();
  zigzag::expectWritten(arena.link(c0, d1, zigzag::DimVector::POS, c1));

  ViewManifold space(arena);
  auto lattice = std::make_shared<LatticeSliceView>(d1, d2);
  SliceViewPresenter presenter(lattice);

  SliceCursor cursor{.origin = c0};
  PaneFrame frame{.widthPx = 800.0F, .heightPx = 600.0F};
  auto measure = [](SubjectId, float) -> ContentExtent {
    return ContentExtent{.width = 120.0F, .height = 40.0F};
  };

  SliceLayoutInput input{space, cursor, frame, measure, 0, std::nullopt};
  presenter.updateLayout(input);

  EXPECT_FALSE(presenter.sink().items().empty());

  // 1. Picking hit-test verification
  const auto targetItem = presenter.sink().items()[0];
  render::PickingResult pick{};
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = static_cast<std::uint32_t>(targetItem.id.value);

  WorldRecordingDevice device;
  RenderState dummyState(&device);
  EXPECT_TRUE(presenter.picked(pick, dummyState));
  ASSERT_TRUE(presenter.selected().has_value());
  EXPECT_EQ(presenter.selected()->value, targetItem.id.value);

  // 2. Accessibility tree description verification
  gleditor::a11y::Tree tree{};
  gleditor::a11y::Builder builder(tree, 1);
  presenter.describe(builder);

  EXPECT_FALSE(builder.roots().empty());
  EXPECT_EQ(tree.focus, builder.roots()[0]);
  EXPECT_EQ(tree.focus & 0xFFFFFFFFFFFFULL, targetItem.id.value);
}

TEST(AcmePresenterTest, PointerChordsHitTesting) {
  zigzag::ArenaManifold arena;
  const auto dLayoutX = arena.ensureDimension("d.layout_x");
  const auto dLayoutY = arena.ensureDimension("d.layout_y");
  const auto dTag     = arena.ensureDimension("d.tag");
  const auto dBody    = arena.ensureDimension("d.body");
  const auto dEvent   = arena.ensureDimension("d.event");

  const auto pane    = arena.makeCell();
  const auto tagCell = arena.makeCell();
  zigzag::expectWritten(
      arena.link(pane, dTag, zigzag::DimVector::POS, tagCell));

  ViewManifold space(arena);
  auto acmeView = std::make_shared<AcmeSliceView>(dLayoutX, dLayoutY, dTag,
                                                  dBody, dEvent, 24.0F, 4.0F);
  AcmePresenter presenter(acmeView);

  SliceCursor cursor{.origin = pane};
  PaneFrame frame{.widthPx = 800.0F, .heightPx = 600.0F};
  auto measure = [](SubjectId, float) -> ContentExtent {
    return ContentExtent{.width = 100.0F, .height = 20.0F};
  };

  SliceLayoutInput input{space, cursor, frame, measure, 0, std::nullopt};
  presenter.updateLayout(input);

  EXPECT_FALSE(presenter.sink().frames().empty());

  // Frame centre should be (400, 300)
  const auto f     = presenter.sink().frames()[0];
  const float hitX = f.centre.x;
  const float hitY = f.centre.y;

  // B1 select chord
  auto resB1 =
      presenter.handlePointerChord(AcmeChord::B1_Select, hitX, hitY, space);
  EXPECT_EQ(resB1.action, AcmeChordResult::Action::HandledBuiltin);

  // B3 look chord (no link, should fallback to search)
  auto resB3 =
      presenter.handlePointerChord(AcmeChord::B3_Look, hitX, hitY, space);
  EXPECT_EQ(resB3.action, AcmeChordResult::Action::TextSearchFallback);
}
