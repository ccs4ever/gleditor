#include <gtest/gtest.h>

#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vql {
namespace {
using zigzag::CellRef;
using zigzag::DimVector;

struct TempDir {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_contract_scalars_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDir() { std::filesystem::create_directories(path); }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};

  CellRef items{coordinator.resolveDimension("d.items")};
  CellRef score{coordinator.resolveDimension("d.score")};

  CellRef addLocal(const std::string_view name, const std::string_view display,
                   const std::optional<ValueKind> kind = std::nullopt,
                   const std::uint64_t bits            = 0) {
    const auto row   = arena.makeCell(name);
    const auto value = arena.makeCell(display);
    if (kind) zigzag::expectWritten(arena.setValueBits(value, *kind, bits));
    zigzag::expectWritten(
        arena.link(zigzag::rankTail(arena, core.home(), items), items,
                   DimVector::POS, row));
    zigzag::expectWritten(arena.link(row, score, DimVector::POS, value));
    return row;
  }

  std::vector<CellRef> run(const std::string_view path,
                           const std::string_view type) {
    const std::string query =
        "def accepts($p: Cell { +d.score: " + std::string(type) +
        " }): Bool := true; " + std::string(path) + "[accepts(.)]";
    QueryExecutor executor(coordinator, vm);
    const auto result = executor.execute(query, {core.home()});
    EXPECT_TRUE(result) << result.error().message;
    return result ? *result : std::vector<CellRef>{};
  }
};

struct ForeignFixture {
  std::shared_ptr<UserPermascroll> scroll = std::make_shared<UserPermascroll>();
  std::shared_ptr<Store> store            = std::make_shared<Store>(scroll);
  MicroversionId version                  = store->sliceGenesis({});
  CellRef items{};
  CellRef score{};
  CellRef clone{};
  CellRef tail{};
  std::unordered_map<std::string, CellRef> rows;

  ForeignFixture() {
    const auto itemsDim = store->makeDimension(version, "d.items");
    version             = itemsDim.version;
    items               = itemsDim.dim;
    const auto scoreDim = store->makeDimension(version, "d.score");
    version             = scoreDim.version;
    score               = scoreDim.dim;
    const auto cloneDim = store->makeDimension(version, "d.clone");
    version             = cloneDim.version;
    clone               = cloneDim.dim;
    tail                = *store->homeCell();
  }

  CellRef add(const std::string &name, const std::string_view display,
              const ValueKind kind, const std::uint64_t bits,
              const bool cloneValue = false) {
    version        = store->makeCell(version, name);
    const auto row = store->cellRefOf(version).value();
    version        = store->makeCell(version, display);
    auto value     = store->cellRefOf(version).value();
    version =
        store->setValue(version, value, scroll->append(display), kind, bits);
    if (cloneValue) {
      version              = store->makeCell(version, "clone display");
      const auto cloneCell = store->cellRefOf(version).value();
      version =
          store->setLink(version, value, clone, DimVector::POS, cloneCell);
      value = cloneCell;
    }
    version = store->setLink(version, tail, items, DimVector::POS, row);
    version = store->setLink(version, row, score, DimVector::POS, value);
    tail    = row;
    rows.emplace(name, row);
    return row;
  }

  CellRef addText(const std::string &name, const std::string_view display) {
    version          = store->makeCell(version, name);
    const auto row   = store->cellRefOf(version).value();
    version          = store->makeCell(version, display);
    const auto value = store->cellRefOf(version).value();
    // Exercise the same text-only inference path for imported and scratch
    // values, even when Store::makeCell inferred numeric bits at mint time.
    version = store->setValue(version, value, scroll->append(display),
                              ValueKind::None, 0);
    version = store->setLink(version, tail, items, DimVector::POS, row);
    version = store->setLink(version, row, score, DimVector::POS, value);
    tail    = row;
    rows.emplace(name, row);
    return row;
  }
};

std::vector<CellRef> sourceIds(Harness &h, const std::vector<CellRef> &cells) {
  std::vector<CellRef> result;
  for (const auto cell : cells) {
    if (const auto foreign = h.arena.foreignOf(cell))
      result.push_back(foreign->index);
    else
      result.push_back(cell);
  }
  return result;
}

TEST(VQLContractScalarVMTest,
     TypedAndTextScalarContractsAgreeForLocalAndForeignCells) {
  Harness h;
  const auto localInt =
      h.addLocal("local int", "not-int", ValueKind::Int64,
                 std::bit_cast<std::uint64_t>(INT64_C(9007199254740993)));
  const auto localDouble =
      h.addLocal("local double", "not-float", ValueKind::Double,
                 std::bit_cast<std::uint64_t>(42.25));
  const auto localBool = h.addLocal("local bool", "true", ValueKind::Bool, 0);
  const auto localText = h.addLocal("local text", "95");
  const auto localHuge = h.addLocal("local huge", "9223372036854775808");
  const auto localBitsOnly = h.arena.makeScalarCell(INT64_C(9));
  const auto localBitsRow  = h.arena.makeCell("local bits only");
  zigzag::expectWritten(
      h.arena.link(zigzag::rankTail(h.arena, h.core.home(), h.items), h.items,
                   DimVector::POS, localBitsRow));
  zigzag::expectWritten(
      h.arena.link(localBitsRow, h.score, DimVector::POS, localBitsOnly));

  ForeignFixture foreign;
  foreign.add("foreign int", "not-int", ValueKind::Int64,
              std::bit_cast<std::uint64_t>(INT64_C(9007199254740993)), true);
  foreign.add("foreign double", "not-float", ValueKind::Double,
              std::bit_cast<std::uint64_t>(42.25));
  foreign.add("foreign bool", "true", ValueKind::Bool, 0);
  foreign.addText("foreign text", "95");
  foreign.addText("foreign huge", "9223372036854775808");
  const auto sourceHead = foreign.store->latest();
  h.coordinator.addStore("foreign", "primary", foreign.store, foreign.version);

  const auto assertLocal = [&](const std::string_view type,
                               const std::vector<CellRef> &expected) {
    EXPECT_EQ(h.run("./d.items", type), expected);
  };
  const auto assertForeign = [&](const std::string_view type,
                                 std::vector<CellRef> expected) {
    const auto result = h.run("##foreign/d.items", type);
    EXPECT_EQ(sourceIds(h, result), expected);
  };
  const std::vector<CellRef> localIntegers{localInt, localText, localBitsRow};
  const std::vector<CellRef> foreignIntegers{foreign.rows.at("foreign int"),
                                             foreign.rows.at("foreign text")};
  const std::vector<CellRef> localFloats{localInt, localDouble, localText,
                                         localBitsRow};
  const std::vector<CellRef> foreignFloats{foreign.rows.at("foreign int"),
                                           foreign.rows.at("foreign double"),
                                           foreign.rows.at("foreign text")};
  const std::vector<CellRef> localStrings{localText, localHuge};
  const std::vector<CellRef> foreignStrings{foreign.rows.at("foreign text"),
                                            foreign.rows.at("foreign huge")};
  const std::vector<CellRef> localBools{localBool};
  const std::vector<CellRef> foreignBools{foreign.rows.at("foreign bool")};

  assertLocal("Int", localIntegers);
  assertForeign("Int", foreignIntegers);
  assertLocal("Float", localFloats);
  assertForeign("Float", foreignFloats);
  assertLocal("String", localStrings);
  assertForeign("String", foreignStrings);
  assertLocal("Bool", localBools);
  assertForeign("Bool", foreignBools);
  EXPECT_EQ(foreign.store->latest(), sourceHead);
  EXPECT_TRUE(h.arena.contains(localBitsOnly));
  EXPECT_FALSE(h.run("./d.items", "String").empty());
}

TEST(VQLContractScalarVMTest,
     EmptyTextRemainsStringAndIntegerOverflowIsNotFloat) {
  Harness h;
  const auto empty     = h.addLocal("empty", "");
  const auto oversized = h.addLocal("oversized", "9223372036854775808");
  const auto integer   = h.addLocal("integer", "95");
  EXPECT_EQ(h.run("./d.items", "String"),
            (std::vector<CellRef>{empty, oversized, integer}));
  EXPECT_EQ(h.run("./d.items", "Float"), (std::vector<CellRef>{integer}));
  EXPECT_EQ(h.run("./d.items", "Int"), (std::vector<CellRef>{integer}));
}

TEST(VQLContractScalarVMTest, TimestampRetainsItsTextFallback) {
  constexpr auto text = "2026-10-29T12:00:00Z";
  Harness h;
  const auto local =
      h.addLocal("local timestamp", text, ValueKind::Timestamp, 17);
  ForeignFixture foreign;
  const auto row =
      foreign.add("foreign timestamp", text, ValueKind::Timestamp, 17);
  h.coordinator.addStore("foreign", "primary", foreign.store, foreign.version);
  EXPECT_EQ(h.run("./d.items", "String"), (std::vector<CellRef>{local}));
  EXPECT_EQ(sourceIds(h, h.run("##foreign/d.items", "String")),
            (std::vector<CellRef>{row}));
  QueryExecutor executor(h.coordinator, h.vm);
  const auto comparison = executor.execute(
      "##foreign/d.items/d.score[. = \"2026-10-29T12:00:00Z\"]");
  ASSERT_TRUE(comparison) << comparison.error().message;
  EXPECT_EQ(comparison->size(), 1u);
}

TEST(VQLContractScalarVMTest,
     DanglingForeignScalarIsAnErrorRatherThanAMismatch) {
  Harness h;
  ForeignFixture foreign;
  h.coordinator.addStore("foreign", "primary", foreign.store, foreign.version);
  const auto home  = *h.coordinator.resolveNamedStore("foreign");
  const auto space = h.arena.foreignOf(home)->space;
  const auto invalid =
      h.arena.proxyFor(space, std::numeric_limits<CellRef>::max());
  QueryExecutor executor(h.coordinator, h.vm, {}, {{"invalid", {invalid}}});
  const auto result =
      executor.execute("def require($x: Int): Int := $x; require($invalid)");
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().message.find("valid source cell"),
            std::string::npos);
}

TEST(VQLContractScalarVMTest, TypedContractCallsSurviveFreshVMExport) {
  TempDir temporary;
  auto scroll = std::make_shared<UserPermascroll>(
      UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  zigzag::ArenaManifold sourceArena;
  zigzag::vortex::VortexCore sourceCore(sourceArena);
  MultiStoreCoordinator sourceCoordinator(sourceCore);
  zigzag::vortex::VortexVM sourceVm(sourceCore);
  VQLCompiler compiler(sourceCoordinator, sourceVm);
  const auto compiled = compiler.compile(
      "def accepts($x: Int): Bool := true; "
      "if accepts(95) return %\"accepted\" else return %\"rejected\"");
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  Store artifact(scroll);
  const auto version = compiler.exportToStore(artifact);
  artifact.save((temporary.path / "artifact").string());

  Store reloaded(scroll);
  ASSERT_NO_THROW(reloaded.load((temporary.path / "artifact").string()));
  const auto manifold = reloaded.rebuildManifold(*reloaded.structureHead());
  zigzag::ArenaManifold arena(&manifold, reloaded);
  zigzag::vortex::VortexCore core(arena);
  MultiStoreCoordinator coordinator(core);
  zigzag::vortex::VortexVM vm(core);
  vm.setContractCoordinator(coordinator);
  const auto entry =
      arena.linked(core.home(), core.dims().spin, DimVector::POS);
  ASSERT_TRUE(entry);
  const auto cursor    = vm.spawnCursor(*entry, "foreign_contract_export");
  const auto execution = vm.run(cursor, 10000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  (void)version;

  std::vector<CellRef> results;
  for (auto instruction = entry; instruction;
       instruction =
           arena.linked(*instruction, core.dims().spin, DimVector::POS)) {
    if (vm.getOpcodeKind(*instruction) != zigzag::vortex::OpcodeKind::Halt)
      continue;
    for (const auto slot : core.inputsOf(*instruction)) {
      const auto values = vm.resultCells(slot);
      results.insert(results.end(), values.begin(), values.end());
    }
  }
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(core.render(results.front()),
            std::optional<zigzag::vortex::CellValue>{std::string{"accepted"}});
}
} // namespace
} // namespace xanadu::vql
