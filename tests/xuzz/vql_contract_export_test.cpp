/**
 * @file vql_contract_export_test.cpp
 * @brief Reopen and execute compiled typed VQL calls from a Store.
 */
#include <chrono>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

namespace fs = std::filesystem;
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::ContractViolationKind;
using zigzag::vortex::OpcodeKind;

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = fs::temp_directory_path() /
            ("vql_contract_export_" + std::to_string(stamp));
    fs::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }

  [[nodiscard]] const fs::path &path() const noexcept { return path_; }

private:
  fs::path path_;
};

struct Case {
  std::string query;
  bool expectRuntimeFailure{false};
  bool expectLinkedFanout{false};
  bool expectContract{true};
  std::string expectedValue;
  std::vector<std::string> expectedValues;
  std::string expectedError;
};

void compileExportReloadAndRun(const Case &testCase, const fs::path &root) {
  const auto permascrollPath = root / "permascroll";
  const auto storePath       = root / "compiled";
  auto permascroll           = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = permascrollPath});

  zigzag::ArenaManifold sourceArena;
  zigzag::vortex::VortexCore sourceCore(sourceArena);
  xanadu::MultiStoreCoordinator sourceCoordinator(sourceCore);
  zigzag::vortex::VortexVM sourceVm(sourceCore);
  xanadu::vql::VQLCompiler sourceCompiler(sourceCoordinator, sourceVm);
  const auto compilation = sourceCompiler.compile(testCase.query);
  ASSERT_TRUE(compilation.success) << compilation.errorMessage;
  ASSERT_TRUE(compilation.entryOpcode);

  xanadu::Store sourceStore(permascroll);
  const auto firstExport     = sourceCompiler.exportToStore(sourceStore);
  const auto exportedVersion = sourceCompiler.exportToStore(sourceStore);
  EXPECT_TRUE(firstExport.isAncestorOf(exportedVersion));
  ASSERT_FALSE(exportedVersion.isZero());
  sourceStore.save(storePath.string());

  // Reload through the on-disk reader and execute from the reconstructed
  // manifold, with no opcode or type AST retained from the compiling VM.
  xanadu::Store reloadedStore(permascroll);
  ASSERT_NO_THROW(reloadedStore.load(storePath.string()));
  ASSERT_TRUE(reloadedStore.structureHead());
  const auto reopenedManifold =
      reloadedStore.rebuildManifold(*reloadedStore.structureHead());
  zigzag::ArenaManifold reopenedArena(&reopenedManifold, reloadedStore);
  zigzag::vortex::VortexCore reopenedCore(reopenedArena);
  xanadu::MultiStoreCoordinator reopenedCoordinator(reopenedCore);
  zigzag::vortex::VortexVM reopenedVm(reopenedCore);
  reopenedVm.setContractCoordinator(reopenedCoordinator);

  const auto entry = reopenedArena.linked(
      reopenedCore.home(), reopenedCore.dims().spin, DimVector::POS);
  ASSERT_TRUE(entry) << "exported Store has no executable entry in +d.spin";

  // The branch target is an actual instruction cell in the persisted spin
  // chain, rather than a process-local PC or a stale CellRef.
  std::unordered_set<CellRef> instructions;
  auto instruction = entry;
  while (instruction && instructions.insert(*instruction).second) {
    instruction = reopenedArena.linked(*instruction, reopenedCore.dims().spin,
                                       DimVector::POS);
  }
  bool sawParameterBranch = false;
  bool sawMatchContract   = false;
  for (const CellRef opcode : instructions) {
    sawMatchContract |=
        reopenedVm.getOpcodeKind(opcode) == OpcodeKind::MatchContract;
    if (reopenedVm.getOpcodeKind(opcode) != OpcodeKind::BranchFalseCell)
      continue;
    sawParameterBranch = true;
    const auto inputs  = reopenedCore.inputsOf(opcode);
    ASSERT_EQ(inputs.size(), 2u);
    EXPECT_TRUE(instructions.contains(inputs[1]));
    EXPECT_TRUE(reopenedVm.getOpcodeKind(inputs[1]) == OpcodeKind::Nop ||
                reopenedVm.getOpcodeKind(inputs[1]) ==
                    OpcodeKind::IteratorNext);
  }
  if (testCase.expectContract) {
    EXPECT_TRUE(sawMatchContract)
        << "typed parameter MatchContract was not exported";
    EXPECT_TRUE(sawParameterBranch)
        << "typed parameter MatchContract lost its BranchFalseCell";
  }

  const auto cursor    = reopenedVm.spawnCursor(*entry, "reloaded_typed_call");
  const auto execution = reopenedVm.run(cursor, 10000);
  if (testCase.expectRuntimeFailure) {
    EXPECT_FALSE(execution.success);
    EXPECT_EQ(execution.contractViolation,
              ContractViolationKind::Postcondition);
    EXPECT_EQ(execution.errorMessage, testCase.expectedError);
    return;
  }

  ASSERT_TRUE(execution.success) << execution.errorMessage;
  CellRef halt = *entry;
  auto next =
      reopenedArena.linked(halt, reopenedCore.dims().spin, DimVector::POS);
  while (next) {
    halt = *next;
    next = reopenedArena.linked(halt, reopenedCore.dims().spin, DimVector::POS);
  }
  ASSERT_EQ(reopenedVm.getOpcodeKind(halt), OpcodeKind::Halt);
  const auto resultSlots = reopenedCore.inputsOf(halt);
  std::vector<CellRef> actualCells;
  std::vector<std::string> actual;
  for (const auto slot : resultSlots) {
    for (const auto result : reopenedVm.resultCells(slot)) {
      actualCells.push_back(result);
      const auto rendered = reopenedCore.render(result);
      ASSERT_TRUE(rendered);
      actual.push_back(std::visit(
          [](const auto &value) -> std::string {
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::same_as<Value, std::string>)
              return value;
            else if constexpr (std::same_as<Value, bool>)
              return value ? "true" : "false";
            else
              return std::to_string(value);
          },
          *rendered));
    }
  }
  auto expected = testCase.expectedValues;
  if (expected.empty() && !testCase.expectedValue.empty())
    expected.push_back(testCase.expectedValue);
  EXPECT_EQ(actual, expected);

  if (testCase.expectLinkedFanout) {
    ASSERT_EQ(actualCells.size(), 4u);
    EXPECT_NE(actualCells[2], actualCells[3]);
    const auto fanout = reopenedCoordinator.resolveDimension("d.fanout");
    EXPECT_EQ(reopenedArena.linked(actualCells[0], fanout, DimVector::POS),
              actualCells[2]);
    EXPECT_EQ(reopenedArena.linked(actualCells[1], fanout, DimVector::POS),
              actualCells[3]);
    EXPECT_EQ(reopenedArena.linked(actualCells[2], reopenedCore.dims().clone,
                                   DimVector::POS),
              actualCells[3]);
  }
}

TEST(VQLContractExportTest, TypedCallGraphSurvivesStoreReloadAndFreshVM) {
  TemporaryDirectory temporary;
  const std::vector<Case> cases{
      {.query = "def identity($x: String): String := $x; identity(\"\")",
       .expectedValues = {""}},
      {.query = "def identity($x: Float): Cell := $x; "
                "identity(\"9223372036854775808\")"},
      {.query          = "let $p := %\"paper\"; $p/d.items%\"one\"%\"two\"; "
                         "$p/d.items/d.video%\"video\"/d.codec%\"h264\"; "
                         "let $f := /d.items[{~d.video: {+d.codec: \"h264\"}}]; "
                         "let $g := /$f; let $f := /d.absent; $p/$g[-1]",
       .expectContract = false,
       .expectedValue  = "two"},
      {.query          = "let $p := %\"paper\"; $p/d.items%\"one\"%\"two\"; "
                         "$p/d.items/d.score%9007199254740993; "
                         "$p/d.items[{+d.score: 9007199254740992.0}]",
       .expectContract = false},
      {.query          = "let $p := %\"paper\"; $p/d.items%\"one\"%\"two\"; "
                         "$p/d.items/d.tag%\"tagged\"; "
                         "$p/d.items[{ +d.tag, !d.hidden }][-1]",
       .expectContract = false,
       .expectedValue  = "two"},
      {.query          = "let $p := %\"paper\"; $p/d.items%\"one\"%\"two\"; "
                         "$p/d.items/d.tag%\"tagged\"; "
                         "let $f := /d.items[{ ~d.tag, !d.hidden }]; "
                         "let $g := /$f; let $f := /d.absent; $p/$g",
       .expectContract = false,
       .expectedValues = {"one", "two"}},
      {.query = "def reveal($items: Stream<Cell { .: $title: String }>): "
                "String := $title; let $title := \"outer\"; "
                "let $p := %\"paper\"; $p/d.items%\"first\"%\"second\"; "
                "reveal($p/d.items)",
       .expectedValue = "outer"},
      {.query          = "let $p := %\"paper\"; $p/d.parents%\"one\"%\"two\"; "
                         "$p/d.parents/d.children%\"A\"%\"B\"!last",
       .expectContract = false,
       .expectedValue  = "B"},
      {.query          = "let $p := %\"paper\"; $p/d.parents%\"one\"%\"two\"; "
                         "$p/d.parents/d.children%\"A\"%\"B\"!both",
       .expectContract = false,
       .expectedValues = {"one", "two", "A", "B", "A", "B"}},
      {.query = "let $p := %\"paper\"; $p/d.parents%\"one\"%\"two\"; "
                "let $target := %\"Target\"; "
                "$p/d.parents/link(+d.fanout, $target)!both",
       .expectLinkedFanout = true,
       .expectContract     = false,
       .expectedValues     = {"one", "two", "Target", "Target"}},
      {.query = "def reveal($items: Stream<Cell { .: $title: String }>): "
                "String := $title; let $title := \"outer\"; "
                "let $p := %\"paper\"; reveal($p/d.empty)",
       .expectedValue = "outer"},
      {.query         = "def identity($x: Int): Int := $x; "
                        "if 1 = 1.0 return identity(7) else return identity(9)",
       .expectedValue = "7"},
      {.query         = "def identity($x: Int): Int := $x; "
                        "if 9007199254740993 > 9007199254740992.0 return identity(7) "
                        "else return identity(9)",
       .expectedValue = "7"},
      {.query         = "def identity($x: Int): Int := $x; "
                        "if 9007199254740992.0 < 9007199254740993 return identity(7) "
                        "else return identity(9)",
       .expectedValue = "7"},
      {.query         = "def identity($x: Int): Int := $x; "
                        "if 9223372036854775807 < 9223372036854775808.0 return "
                        "identity(7) else return identity(9)",
       .expectedValue = "7"},
      {.query         = "def identity($x: Int): Int := $x; "
                        "if -9223372036854775807 > -9223372036854775808.0 return "
                        "identity(7) else return identity(9)",
       .expectedValue = "7"},
      {.query         = "def identity($x: Int): Int := $x; "
                        "if 9007199254740993 > 9007199254740992 "
                        "return identity(7) else return identity(9)",
       .expectedValue = "7"},
      {.query         = "def identity($x: Int): Int := $x; "
                        "if \"95\" = 95 return identity(7) else return identity(9)",
       .expectedValue = "7"},
      {.query         = "def identity($x: Int): Int := $x; "
                        "if true return identity(7) else return identity(9)",
       .expectedValue = "7"},
      {.query         = "def identity($x: Int): Int := $x; "
                        "if false return identity(7) else return identity(9)",
       .expectedValue = "9"},
      {.query =
           "let $p := %\"paper\"; $p/d.items%\"first\"%\"second\"; "
           "for { ., +d.score: $score: Int := 7 } in $p/d.items return $score",
       .expectedValues = {"7", "7"}},
      {.query = "let $p := %\"paper\"; $p/d.items%\"7\"%\"bad\"%\"9\"; "
                "for { .: $item: Int } in $p/d.items where $item != 9.0 "
                "return $item",
       .expectedValues = {"7"}},
      {.query         = "def identity($x: Int): Int := $x; "
                        "let $f := /d.items; let $g := /$f>[. != \"bad\"][-1]; "
                        "let $f := /d.absent; let $p := %\"paper\"; "
                        "$p/d.items%\"7\"%\"bad\"%\"9\"; $p/$g/identity()",
       .expectedValue = "9"},

      {.query          = "def identity($x: String): String := $x; "
                         "let $p := %\"paper\"; $p/d.items%\"7\"%\"9\"; "
                         "$p/-d.items::rank/identity()",
       .expectedValues = {"9", "7", "paper"}},

      {.query         = "def identity($x: Int): Int := $x; "
                        "let $p := %\"paper\"; $p/d.items%\"7\"%\"9\"; "
                        "$p/d.items%\"11\"; $p/d.items::tail/identity()",
       .expectedValue = "11"},

      {.query          = "def identity($x: Int): Int := $x; "
                         "let $p := %\"paper\"; $p/+d.items%\"7\"%\"bad\"%\"9\"; "
                         "$p/+d.items/identity()[-1]",
       .expectedValues = {"9"}},
      {.query          = "def identity($x: Int): Int := $x; "
                         "let $p := %\"paper\"; $p/+d.items%\"7\"%\"bad\"%\"9\"; "
                         "$p/+d.items/identity()",
       .expectedValues = {"7", "9"}},
      {.query          = "def identity($x: Cell): Cell := $x; "
                         "let $p := %\"paper\"; $p/+d.items%\"7\"%\"bad\"%\"9\"; "
                         "$p/+d.items/identity()",
       .expectedValues = {"7", "bad", "9"}},
      {.query = "def identity($x: String): String := $x; identity(\"95\")",
       .expectedValue = "95"},
      {.query         = "def inner($x: Int): Int := $x; "
                        "def outer($x: Int := inner(9)): Int := $x; outer()",
       .expectedValue = "9"},
      {.query         = "def apply($c: Cell, $f): Cell := $c/$f; "
                        "let $p := %\"paper\"; $p/+d.score%\"95\"; "
                        "apply($p, /+d.score)",
       .expectedValue = "95"},
      {.query         = "def identity($x: Int): Int := $x; identity(7)",
       .expectedValue = "7"},
      {.query         = "def choose($item: Cell { +d.value: $value: Cell := "
                        "$fallback }): Cell := $value; let $fallback := "
                        "%\"fallback\"; let $item := %\"item\"; choose($item)",
       .expectedValue = "fallback"},
      {.query          = "def collect(...$rest: Int): Stream<Int> := $rest; "
                         "collect(7, 9, 7)",
       .expectedValues = {"7", "9", "7"}},
      {.query = "def collect(...$rest: Stream<Cell>): Stream<Cell> := $rest; "
                "let $p := %\"paper\"; "
                "$p/+d.left%\"7\"%\"9\"; "
                "$p/+d.right%\"9\"%\"7\"; "
                "collect($p/+d.left, $p/+d.right)",
       .expectedValues = {"7", "9", "9", "7"}},
      {.query = "def collect(...$rest: Int): Stream<Int> := $rest; "
                "collect(7, 9, \"bad\")"},
      {.query = "def identity($x: Int): Int := $x; identity(2147483650)",
       .expectedValue = "2147483650"},
      {.query         = "def identity($x: Cell): Cell := $x; ##/identity()",
       .expectedValue = "home"},
      {.query = "def score($p: Cell { +d.score: Int }): Int := $p/+d.score; "
                "let $p := %\"paper\"; $p/+d.score%\"95\"; score($p)",
       .expectedValue = "95"},
      {.query = "def accept($x: Cell): Bool := false; "
                "let $p := %\"paper\"; $p/+d.score%\"95\"; "
                "$p/+d.score[accept(.)]"},
      {.query = "def identity($x: Int): Int := $x; identity(\"not-int\")"},
      {.query                = "def broken($x: Int): String := $x; broken(7)",
       .expectRuntimeFailure = true,
       .expectedError = "broken() returned a value outside its declared type"},
  };
  for (std::size_t i = 0; i < cases.size(); ++i) {
    SCOPED_TRACE(cases[i].query);
    compileExportReloadAndRun(cases[i], temporary.path() / std::to_string(i));
  }
}

TEST(VQLContractExportTest, PreservesIndependentContentAndValueBits) {
  TemporaryDirectory temporary;
  auto permascroll =
      std::make_shared<xanadu::UserPermascroll>(xanadu::UserPermascroll::Config{
          .storageDir = temporary.path() / "scroll"});
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  xanadu::MultiStoreCoordinator coordinator(core);
  zigzag::vortex::VortexVM vm(core);
  xanadu::vql::VQLCompiler compiler(coordinator, vm);
  ASSERT_TRUE(compiler
                  .compile("def identity($x: String): String := $x; "
                           "identity(\"95\")")
                  .success);
  const auto text = arena.makeCell("95");
  zigzag::expectWritten(arena.setValueBits(text, xanadu::ValueKind::None, 0));
  const auto scalar = arena.makeCell("custom integer display");
  zigzag::expectWritten(
      arena.setValueBits(scalar, xanadu::ValueKind::Int64, 42));
  zigzag::expectWritten(
      arena.setValueBits(core.home(), xanadu::ValueKind::Int64, 73));
  const auto textAxis   = core.mintDimension("d.export_text");
  const auto scalarAxis = core.mintDimension("d.export_scalar");
  zigzag::expectWritten(
      arena.link(core.home(), textAxis, DimVector::POS, text));
  zigzag::expectWritten(
      arena.link(core.home(), scalarAxis, DimVector::POS, scalar));
  xanadu::Store store(permascroll);
  compiler.exportToStore(store);
  const auto path = temporary.path() / "compiled";
  store.save(path.string());
  xanadu::Store reloaded(permascroll);
  reloaded.load(path.string());
  const auto manifold = reloaded.rebuildManifold(*reloaded.structureHead());
  zigzag::ArenaManifold reopened(&manifold, reloaded);
  zigzag::vortex::VortexCore reopenedCore(reopened);
  const auto check = [&](CellRef cell, std::string_view content,
                         xanadu::ValueKind kind, std::uint64_t bits) {
    EXPECT_EQ(reopened.textOf(cell), content);
    const auto slot = reopened.slot(cell);
    ASSERT_TRUE(slot);
    EXPECT_EQ(static_cast<xanadu::ValueKind>(slot->valueKind), kind);
    EXPECT_EQ(slot->valueBits, bits);
  };
  check(reopenedCore.home(), "home", xanadu::ValueKind::Int64, 73);
  for (const auto &[name, content, kind, bits] :
       std::vector<std::tuple<std::string, std::string, xanadu::ValueKind,
                              std::uint64_t>>{
           {"d.export_text", "95", xanadu::ValueKind::None, 0},
           {"d.export_scalar", "custom integer display",
            xanadu::ValueKind::Int64, 42}}) {
    const auto axis = reopenedCore.findDimension(name);
    ASSERT_TRUE(axis);
    const auto cell =
        reopened.linked(reopenedCore.home(), *axis, DimVector::POS);
    ASSERT_TRUE(cell);
    check(*cell, content, kind, bits);
  }
}

} // namespace
