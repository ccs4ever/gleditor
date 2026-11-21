// RP-PATH: a functor-tier rootless path as cells (RP1's step rank), and the
// bounded walker over it (P1b), both standalone. Nothing here goes through a
// compiled query: the compiler does not read a path value yet, so the tests
// drive the API and take their steps from the parser the compiler will use.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/vortex/vortex_host.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/vql/path_cells.hpp"
#include "common/xanadu/vql/shape_cells.hpp"
#include "common/xanadu/vql/shape_limits.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vql;
using zigzag::CellRef;
using zigzag::DimVector;

constexpr std::size_t ceiling = 8;

struct VQLPathCellsTest : testing::Test {
  zigzag::ArenaManifold arena;
  VQLEngine engine{arena};
  zigzag::DimRef fwd{engine.resolveDimension("d.fwd")};
  zigzag::DimRef up{engine.resolveDimension("d.up")};
  std::vector<CellRef> chain; ///< c0 -> c1 -> c2 -> c3 on d.fwd

  void SetUp() override {
    for (const char *text : {"c0", "c1", "c2", "c3"})
      chain.push_back(arena.makeCell(text));
    for (std::size_t i = 0; i + 1 < chain.size(); ++i)
      ASSERT_TRUE(arena.link(chain[i], fwd, DimVector::POS, chain[i + 1]));
  }

  [[nodiscard]] PathDimensions dims() const {
    const auto found = findPathDimensions(engine.core());
    EXPECT_TRUE(found.has_value());
    return found.value_or(PathDimensions{});
  }

  /// Steps as the parser makes them from rootless source.
  static std::expected<std::vector<PathStepSpec>, PathRefusal>
  parsed(const std::string &source) {
    Parser parser(source);
    const auto query = parser.parseQuery();
    return classifyFunctorPath(std::get<PathExpression>(query.expr));
  }

  CellRef mint(const std::string &source, std::size_t max = ceiling) {
    const auto steps = parsed(source);
    EXPECT_TRUE(steps.has_value()) << source;
    const auto path = mintPath(arena, dims(), *steps, max);
    EXPECT_TRUE(path.has_value()) << source;
    return path.value_or(zigzag::noCell);
  }

  std::vector<CellRef> walk(CellRef subject, CellRef path) {
    const auto result =
        walkPath(engine.core(), dims(), subject, path, {.maxSteps = ceiling});
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
    return result ? result->cells : std::vector<CellRef>{};
  }

  std::vector<std::string> texts(const std::vector<CellRef> &cells) {
    std::vector<std::string> out;
    for (const CellRef cell : cells) out.push_back(arena.textOf(cell));
    return out;
  }
};

using testing::ElementsAre;
using testing::IsEmpty;

TEST_F(VQLPathCellsTest, TheDimensionsAreNamedAtEngineConstruction) {
  const auto d     = dims();
  const auto shape = findShapeDimensions(engine.core());
  ASSERT_TRUE(shape.has_value());
  // Direction, bidirectionality and the witness are the shape rules' own
  // dimensions, not copies of them.
  EXPECT_EQ(d.dir, shape->dir);
  EXPECT_EQ(d.bidir, shape->bidir);
  EXPECT_EQ(d.epoch, shape->epoch);
  EXPECT_EQ(engine.core().findDimension(pathMarkerDimension), d.marker);
  EXPECT_EQ(engine.core().findDimension(pathStepDimension), d.step);
  EXPECT_EQ(engine.core().findDimension(pathClampDimension), d.clamp);
}

TEST_F(VQLPathCellsTest, AMintedPathDecodesBackToItsSteps) {
  const auto source = "/+d.fwd[1]/-d.up/~d.fwd[2, -1]/+d.up[-1]/d.fwd";
  const auto steps  = parsed(source);
  ASSERT_TRUE(steps.has_value());
  ASSERT_EQ(steps->size(), 5u);
  const auto path = mintPath(arena, dims(), *steps, ceiling);
  ASSERT_TRUE(path.has_value());
  const auto back = readPath(arena, dims(), *path, ceiling);
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(*back, *steps);
  ASSERT_TRUE((*back)[0].clamp.has_value());
  EXPECT_EQ((*back)[0].clamp->start, 1);
  // The parser writes `[1]` as the window [1, 1]; both spellings round-trip.
  EXPECT_EQ((*back)[0].clamp->end, 1);
  const std::vector<PathStepSpec> open{
      {.dimension = "d.fwd", .clamp = RangeClamp{.start = 2}}};
  const auto openPath = mintPath(arena, dims(), open, ceiling);
  ASSERT_TRUE(openPath.has_value());
  const auto openBack = readPath(arena, dims(), *openPath, ceiling);
  ASSERT_TRUE(openBack.has_value());
  EXPECT_EQ(*openBack, open);
  EXPECT_FALSE(openBack->front().clamp->end.has_value());
  EXPECT_EQ((*back)[1].direction, DimVector::NEG);
  EXPECT_TRUE((*back)[2].bidirectional);
  ASSERT_TRUE((*back)[2].clamp.has_value());
  EXPECT_EQ((*back)[2].clamp->start, 2);
  EXPECT_EQ((*back)[2].clamp->end, -1);
  EXPECT_FALSE((*back)[4].clamp.has_value());
}

TEST_F(VQLPathCellsTest, APathCarriesItsMarkerAndWitnessAndNoOperation) {
  const auto path = mint("/+d.fwd");
  EXPECT_TRUE(isPathCell(arena, dims(), path));
  EXPECT_FALSE(isPathCell(arena, dims(), chain[0]));
  const auto witness = arena.linked(path, dims().epoch, DimVector::POS);
  ASSERT_TRUE(witness.has_value());
  EXPECT_EQ(arena.asInt64(*witness),
            static_cast<std::int64_t>(*arena.epochOfCell(path)));
  EXPECT_FALSE(stalePathReason(arena, dims(), path).has_value());
}

TEST_F(VQLPathCellsTest, AFreeVariableRefusesByNameAndMintsNothing) {
  const auto before  = arena.cellCount();
  const auto refused = parsed("/$f");
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, PathRefusalKind::FreeVariable);
  EXPECT_THAT(refused.error().message, testing::HasSubstr("$f"));
  EXPECT_THAT(refused.error().message, testing::HasSubstr("closure"));

  // The mint refuses it too when handed the step directly, ahead of any cell.
  const std::vector<PathStepSpec> steps{{.dimension = "d.fwd"},
                                        {.dimension = "$n"}};
  const auto minted = mintPath(arena, dims(), steps, ceiling);
  ASSERT_FALSE(minted.has_value());
  EXPECT_EQ(minted.error().kind, PathRefusalKind::FreeVariable);
  EXPECT_THAT(minted.error().message, testing::HasSubstr("$n"));
  EXPECT_EQ(arena.cellCount(), before);
}

TEST_F(VQLPathCellsTest, AnythingBeyondTheFunctorTierRefusesByName) {
  for (const char *source :
       {"/+d.fwd[d.name = \"x\"]", "/+d.fwd::tail", "/+d.fwd>", "##/+d.fwd",
        "./+d.fwd", "/+d.fwd/count()"}) {
    const auto refused = parsed(source);
    ASSERT_FALSE(refused.has_value()) << source;
    EXPECT_EQ(refused.error().kind, PathRefusalKind::BeyondFunctorTier)
        << source << ": " << refused.error().message;
  }
  const std::vector<PathStepSpec> none;
  const auto empty = mintPath(arena, dims(), none, ceiling);
  ASSERT_FALSE(empty.has_value());
  EXPECT_EQ(empty.error().kind, PathRefusalKind::NoSteps);
}

TEST_F(VQLPathCellsTest, MoreStepsThanTheCeilingRefuseAndMintNothing) {
  const auto steps = parsed("/+d.fwd/+d.fwd/+d.fwd");
  ASSERT_TRUE(steps.has_value());
  const auto before  = arena.cellCount();
  const auto refused = mintPath(arena, dims(), *steps, 2);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, PathRefusalKind::TooManySteps);
  EXPECT_THAT(refused.error().message,
              testing::AllOf(testing::HasSubstr("3 steps"),
                             testing::HasSubstr("maximum of 2")));
  EXPECT_EQ(arena.cellCount(), before);
  EXPECT_TRUE(mintPath(arena, dims(), *steps, 3).has_value());

  // A ceiling lowered after the mint is obeyed by every later read.
  const auto path = mint("/+d.fwd/+d.fwd/+d.fwd");
  const auto read = readPath(arena, dims(), path, 2);
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error().kind, PathRefusalKind::TooManySteps);
  const auto walked =
      walkPath(engine.core(), dims(), chain[0], path, {.maxSteps = 2});
  ASSERT_FALSE(walked.has_value());
  EXPECT_EQ(walked.error().kind, PathRefusalKind::TooManySteps);
}

TEST_F(VQLPathCellsTest, ARankRingedBackThroughItsHeadEnds) {
  const auto path = mint("/+d.fwd/+d.up");
  ASSERT_TRUE(readPath(arena, dims(), path, ceiling).has_value());
  CellRef last = path;
  while (const auto next = arena.linked(last, dims().step, DimVector::POS))
    last = *next;
  ASSERT_NE(last, path);
  ASSERT_TRUE(arena.link(last, dims().step, DimVector::POS, path));
  const auto read = readPath(arena, dims(), path, ceiling);
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->size(), 2u);
}

TEST_F(VQLPathCellsTest, AForwardStepWalksTheRankAndAClampWindowsIt) {
  EXPECT_THAT(texts(walk(chain[0], mint("/+d.fwd"))),
              ElementsAre("c1", "c2", "c3"));
  EXPECT_THAT(texts(walk(chain[0], mint("/+d.fwd[1]"))), ElementsAre("c1"));
  EXPECT_THAT(texts(walk(chain[0], mint("/+d.fwd[-1]"))), ElementsAre("c3"));
  EXPECT_THAT(texts(walk(chain[0], mint("/+d.fwd[2, 3]"))),
              ElementsAre("c2", "c3"));
  EXPECT_THAT(texts(walk(chain[0], mint("/+d.fwd[1, -2]"))),
              ElementsAre("c1", "c2"));
  // An index off the end is clamped into the stream, as the compiled clamp
  // does.
  EXPECT_THAT(texts(walk(chain[0], mint("/+d.fwd[9]"))), ElementsAre("c3"));
  EXPECT_THAT(texts(walk(chain[0], mint("/+d.fwd[3, 1]"))), IsEmpty());
}

TEST_F(VQLPathCellsTest, ABackwardStepWalksTheOtherWay) {
  EXPECT_THAT(texts(walk(chain[3], mint("/-d.fwd"))),
              ElementsAre("c2", "c1", "c0"));
  EXPECT_THAT(texts(walk(chain[3], mint("/-d.fwd[1]"))), ElementsAre("c2"));
}

TEST_F(VQLPathCellsTest, ABidirectionalStepWalksPositiveThenNegative) {
  EXPECT_THAT(texts(walk(chain[1], mint("/~d.fwd"))),
              ElementsAre("c2", "c3", "c0"));
  EXPECT_THAT(texts(walk(chain[1], mint("/~d.fwd[-1]"))), ElementsAre("c0"));
}

TEST_F(VQLPathCellsTest, StepsComposeAndAClampWindowsEachStepsWholeOutput) {
  EXPECT_THAT(texts(walk(chain[0], mint("/+d.fwd[1]/+d.fwd[1]"))),
              ElementsAre("c2"));
  // From c1 and c2 both, the second step's output is c2,c3 then c3: duplicates
  // are kept, and the clamp counts them.
  EXPECT_THAT(texts(walk(chain[0], mint("/+d.fwd[1, 2]/+d.fwd"))),
              ElementsAre("c2", "c3", "c3"));
  EXPECT_THAT(texts(walk(chain[0], mint("/+d.fwd[1, 2]/+d.fwd[-1]"))),
              ElementsAre("c3"));
}

TEST_F(VQLPathCellsTest, ACyclicDataRankEndsWhereItBegan) {
  ASSERT_TRUE(arena.link(chain[3], fwd, DimVector::POS, chain[0]));
  EXPECT_THAT(texts(walk(chain[0], mint("/+d.fwd"))),
              ElementsAre("c1", "c2", "c3"));
}

TEST_F(VQLPathCellsTest, ResultsAreZeroOneOrMany) {
  const auto none = mint("/+d.up");
  EXPECT_THAT(walk(chain[0], none), IsEmpty());
  EXPECT_THAT(walk(chain[3], mint("/+d.fwd")), IsEmpty());
  EXPECT_EQ(walk(chain[0], mint("/+d.fwd[1]")).size(), 1u);
  EXPECT_EQ(walk(chain[0], mint("/+d.fwd")).size(), 3u);
  // A dimension no one has named reaches nothing and mints nothing.
  const auto dims = arena.cellCount();
  EXPECT_THAT(walk(chain[0], mint("/+d.never_named")), IsEmpty());
  EXPECT_FALSE(engine.core().findDimension("d.never_named").has_value());
  (void)dims;
}

TEST_F(VQLPathCellsTest, TheWalkReadsOnlyAndReportsItsVisits) {
  const auto path   = mint("/+d.fwd");
  const auto before = arena.cellCount();
  const auto walked =
      walkPath(engine.core(), dims(), chain[0], path, {.maxSteps = ceiling});
  ASSERT_TRUE(walked.has_value());
  EXPECT_EQ(arena.cellCount(), before);
  // Three neighbours and the read that found the end.
  EXPECT_EQ(walked->visits, 4u);
}

TEST_F(VQLPathCellsTest, TheArenasWorkBudgetIsChargedAndRefusesWhenSpent) {
  const auto path = mint("/+d.fwd");
  arena.setExecutionLimits(1u << 20, 100, 1024);
  ASSERT_TRUE(
      walkPath(engine.core(), dims(), chain[0], path, {.maxSteps = ceiling})
          .has_value());
  arena.setExecutionLimits(1u << 20, 3, 1024);
  const auto spent =
      walkPath(engine.core(), dims(), chain[0], path, {.maxSteps = ceiling});
  ASSERT_FALSE(spent.has_value());
  EXPECT_EQ(spent.error().kind, PathRefusalKind::FuelExhausted);
}

TEST_F(VQLPathCellsTest, AVisitCeilingBoundsADuplicatingFanOut) {
  const auto path = mint("/+d.fwd/+d.fwd/+d.fwd");
  const auto cut  = walkPath(engine.core(), dims(), chain[0], path,
                             {.maxSteps = ceiling, .maxVisits = 5});
  ASSERT_FALSE(cut.has_value());
  EXPECT_EQ(cut.error().kind, PathRefusalKind::VisitBudget);
}

TEST_F(VQLPathCellsTest, TheMatchRefusesSeveralByNameAndNoneNeverMatches) {
  const auto neighbour = arena.makeCell("c1");
  const auto several   = walk(chain[0], mint("/+d.fwd"));
  const auto refused   = pathTargetMatches(engine.core(), several, neighbour);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, PathRefusalKind::SeveralResults);
  EXPECT_THAT(refused.error().message, testing::HasSubstr("3 cells"));

  const std::vector<CellRef> none;
  EXPECT_EQ(pathTargetMatches(engine.core(), none, neighbour), false);
  const std::vector<CellRef> one{chain[1]};
  EXPECT_EQ(pathTargetMatches(engine.core(), one, std::nullopt), false);
}

TEST_F(VQLPathCellsTest, TheMatchComparesOneResultByValueNotIdentity) {
  const auto reached = walk(chain[0], mint("/+d.fwd[1]"));
  ASSERT_EQ(reached.size(), 1u);
  // A clone with the same text is a different cell and an equal value.
  const auto clone = arena.makeCell("c1");
  ASSERT_NE(clone, reached.front());
  EXPECT_EQ(pathTargetMatches(engine.core(), reached, clone), true);
  EXPECT_EQ(pathTargetMatches(engine.core(), reached, arena.makeCell("c2")),
            false);
  // Same cell is no special case: it is equal because its value is.
  EXPECT_EQ(pathTargetMatches(engine.core(), reached, reached.front()), true);

  // Numbers compare as numbers: a typed 5 equals the text "5" and 5.0.
  const std::vector<CellRef> five{arena.makeScalarCell(std::int64_t{5})};
  EXPECT_EQ(pathTargetMatches(engine.core(), five, arena.makeCell("5")), true);
  EXPECT_EQ(pathTargetMatches(engine.core(), five, arena.makeScalarCell(5.0)),
            true);
  EXPECT_EQ(pathTargetMatches(engine.core(), five, arena.makeScalarCell(6.0)),
            false);
  // Two plain texts stay texts: "1" is not "1.0".
  const std::vector<CellRef> text{arena.makeCell("1")};
  EXPECT_EQ(pathTargetMatches(engine.core(), text, arena.makeCell("1.0")),
            false);
}

TEST_F(VQLPathCellsTest, AStaleWitnessRefusesEveryReader) {
  const auto path    = mint("/+d.fwd");
  const auto witness = *arena.linked(path, dims().epoch, DimVector::POS);
  ASSERT_TRUE(arena.setValueBits(witness, xanadu::ValueKind::Int64, 12345));
  EXPECT_THAT(*stalePathReason(arena, dims(), path),
              testing::HasSubstr("released and its number reused"));
  const auto read = readPath(arena, dims(), path, ceiling);
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error().kind, PathRefusalKind::Stale);
  const auto walked =
      walkPath(engine.core(), dims(), chain[0], path, {.maxSteps = ceiling});
  ASSERT_FALSE(walked.has_value());
  EXPECT_EQ(walked.error().kind, PathRefusalKind::Stale);
}

TEST_F(VQLPathCellsTest, ANumberReleasedAndReusedIsNotThePath) {
  const auto mark = arena.mark();
  const auto path = mint("/+d.fwd");
  ASSERT_TRUE(arena.release(mark));
  // The path's cells are gone; its number now names whatever is minted there.
  const auto reused = arena.makeCell("not a path");
  EXPECT_EQ(reused, path);
  const auto read = readPath(arena, dims(), path, ceiling);
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error().kind, PathRefusalKind::NotAPath);
}

TEST_F(VQLPathCellsTest, ADamagedStepRefusesByNameRatherThanReadingEmpty) {
  const auto path  = mint("/+d.fwd");
  const auto first = *arena.linked(path, dims().step, DimVector::POS);
  ASSERT_TRUE(arena.link(first, dims().dir, DimVector::POS, std::nullopt));
  const auto read = readPath(arena, dims(), path, ceiling);
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error().kind, PathRefusalKind::NotAPath);
  EXPECT_THAT(read.error().message, testing::HasSubstr("damaged"));
}

TEST(VQLPathSettings, TheCeilingIsASettingSeededWithItsDefault) {
  const auto specs =
      xanadu::defaultSettingSpecs(xanadu::SystemDocKind::Settings);
  std::optional<std::int64_t> seeded;
  for (const auto &spec : specs)
    if (spec.name == xanadu::settings::kVqlPathMaxSteps &&
        !spec.schemas.empty() && !spec.schemas.front().defaultValues.empty()) {
      EXPECT_FALSE(spec.notes.empty());
      EXPECT_EQ(spec.schemas.front().expectedTypes,
                std::vector<std::string>{"integer"});
      if (const auto *value = std::get_if<std::int64_t>(
              &spec.schemas.front().defaultValues.front()))
        seeded = *value;
    }
  EXPECT_EQ(seeded, static_cast<std::int64_t>(defaultPathMaxSteps));
  EXPECT_EQ(xanadu::settings::kVqlPathMaxSteps, "vql.path.max_steps");
}

TEST(VQLPathSettings, AHostReadsTheCeilingFromTheStoreAndHandsItToTheMint) {
  xanadu::Store store;
  xanadu::initializeSystemStore(store, xanadu::SystemDocKind::Settings);
  EXPECT_EQ(zigzag::vortex::VortexHostConfig::fromStore(store)
                .shapeLimits.maxPathSteps,
            defaultPathMaxSteps);

  auto head = store.primaryCurrentVersion();
  head = xanadu::setSetting(store, head, xanadu::settings::kVqlPathMaxSteps,
                            std::int64_t{3});
  store.repointCurrentVersion(head);
  const auto config = zigzag::vortex::VortexHostConfig::fromStore(store);
  ASSERT_EQ(config.shapeLimits.maxPathSteps, 3u);

  zigzag::Manifold base;
  zigzag::vortex::VortexHost host(&base);
  host.setConfig(config);
  EXPECT_EQ(host.vqlEngine().shapeLimits().maxPathSteps, 3u);

  auto &core      = host.vqlEngine().core();
  const auto dims = ensurePathDimensions(core);
  const auto three =
      mintPath(core.arena(), dims,
               std::vector<PathStepSpec>(3, PathStepSpec{.dimension = "d.fwd"}),
               host.vqlEngine().shapeLimits().maxPathSteps);
  EXPECT_TRUE(three.has_value());
  const auto four =
      mintPath(core.arena(), dims,
               std::vector<PathStepSpec>(4, PathStepSpec{.dimension = "d.fwd"}),
               host.vqlEngine().shapeLimits().maxPathSteps);
  ASSERT_FALSE(four.has_value());
  EXPECT_EQ(four.error().kind, PathRefusalKind::TooManySteps);

  // A ceiling of zero would refuse every path, so it is not taken.
  head = xanadu::setSetting(store, head, xanadu::settings::kVqlPathMaxSteps,
                            std::int64_t{0});
  store.repointCurrentVersion(head);
  EXPECT_EQ(zigzag::vortex::VortexHostConfig::fromStore(store)
                .shapeLimits.maxPathSteps,
            defaultPathMaxSteps);
}

} // namespace
