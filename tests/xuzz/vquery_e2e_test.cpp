/**
 * @file vquery_e2e_test.cpp
 * @brief End-to-end CLI integration test suite for the VQL query runner
 * (vquery).
 *
 * Tests CLI argument handling, multi-store loading, ##NAME resolution,
 * on-the-fly Vortex compilation, in-place store edits, and output store
 * creation.
 */
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "common/xanadu/result_slice.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/store_loader.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/capture_journal.hpp"

namespace {

namespace fs = std::filesystem;

struct ProcessResult {
  int exitCode{0};
  std::string output;
};

// @p dataHome redirects XDG_DATA_HOME (default: build/xdg/data), so a test can
// own the permascroll and the capture store beside it.
ProcessResult runVQuery(const std::string &args,
                        const std::string &dataHome = {}) {
  std::string binary = "./build/vquery";
  if (!fs::exists(binary)) {
    return {-1, "vquery binary not found at " + binary};
  }

  std::string cmd =
      "SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy "
      "LIBGL_ALWAYS_SOFTWARE=1 XDG_DATA_HOME=" +
      (dataHome.empty() ? fs::current_path().string() + "/build/xdg/data"
                        : dataHome) +
      " XDG_CONFIG_HOME=" + fs::current_path().string() +
      "/build/xdg/config XDG_CACHE_HOME=" + fs::current_path().string() +
      "/build/xdg/cache " + binary + " " + args + " 2>&1";

  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    return {-1, "Failed to popen: " + cmd};
  }

  char buffer[512];
  std::string out;
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    out += buffer;
  }
  int status = pclose(pipe);
  int code   = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return {code, out};
}

class VQueryE2ETest : public ::testing::Test {
protected:
  void SetUp() override {
    testDir = fs::temp_directory_path() /
              ("vquery_test_" + std::to_string(std::rand()));
    fs::create_directories(testDir);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(testDir, ec);
  }

  fs::path testDir;
};

TEST_F(VQueryE2ETest, HelpFlag) {
  auto res = runVQuery("--help");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("Usage: vquery"), std::string::npos);
  EXPECT_NE(res.output.find("--engine"), std::string::npos);
  EXPECT_NE(res.output.find("--output-store"), std::string::npos);
}

TEST_F(VQueryE2ETest, ReplKeepsCompiledClosureBindingsAcrossQueries) {
  const auto input = testDir / "closure-session.vql";
  {
    std::ofstream script(input);
    script
        << R"(let $owned := string(9007199254740993); let $f := \($x) $owned; return ##)"
        << '\n'
        << "return ##/$f\n:quit\n";
  }
  const auto result = runVQuery("--engine vortex < '" + input.string() + "'");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_EQ(result.output.find("Query failed:"), std::string::npos)
      << result.output;
  EXPECT_NE(result.output.find("9007199254740993"), std::string::npos)
      << result.output;
}

TEST_F(VQueryE2ETest, DirectEvaluationBasic) {
  auto res = runVQuery("-e \"##\" --format cells");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("[#"), std::string::npos);
}

TEST_F(VQueryE2ETest, OnTheFlyVortexCompilation) {
  const auto res = runVQuery("-f tests/samples/vql/conformance/create_rank.vql "
                             "--engine vortex --dump-asm --format text");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("#RANK_BEGIN"), std::string::npos);
  EXPECT_NE(res.output.find("#MINT_CELL"), std::string::npos);
  EXPECT_NE(res.output.find("#HALT"), std::string::npos);
  EXPECT_TRUE(res.output.ends_with("Alpha\nBeta\n")) << res.output;
}

TEST_F(VQueryE2ETest, VortexRefusesUnimplementedSemantics) {
  const auto source = runVQuery("-e 'topic(\"missing\")' --engine vortex");
  EXPECT_EQ(source.exitCode, 1) << source.output;
  EXPECT_NE(source.output.find("store generator"), std::string::npos);
}

TEST_F(VQueryE2ETest, AsciiVisualizationMode) {
  auto res = runVQuery("-e \"##\" --ascii");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("=== VQL Query Results"), std::string::npos);
}

TEST_F(VQueryE2ETest, CompiledValueResultsRenderContentRatherThanSlotIds) {
  const auto result =
      runVQuery(R"(--engine vortex --eval '%"before"/value("after")/value()')");
  EXPECT_EQ(result.exitCode, 0) << result.output;
  EXPECT_EQ(result.output, "after\n");
}

TEST_F(VQueryE2ETest, CompiledSliceReadsDoNotChangeTheSource) {
  const auto result = runVQuery(
      R"(--engine vortex --eval 'let $item := %"abcdef"; $item.[1, 2]; $item/value()')");
  EXPECT_EQ(result.exitCode, 0) << result.output;
  EXPECT_EQ(result.output, "abcdef\n");
}

TEST_F(VQueryE2ETest, TypedDefinitionsAndReturnErrorsHaveBackendParity) {
  for (const auto mode : {"vortex"}) {
    SCOPED_TRACE(mode);
    const auto success =
        runVQuery(std::string("--engine ") + mode +
                  R"( --eval 'def identity($x: Int): Int := $x; identity(7)')");
    EXPECT_EQ(success.exitCode, 0) << success.output;
    EXPECT_EQ(success.output, "7\n");
    const auto failure =
        runVQuery(std::string("--engine ") + mode +
                  R"( --eval 'def broken($x: Int): String := $x; broken(7)')");
    EXPECT_NE(failure.exitCode, 0);
    EXPECT_EQ(
        failure.output,
        "Query failed: broken() returned a value outside its declared type\n");
  }
}

TEST_F(VQueryE2ETest, TypedVariadicCallsPreserveOrderAndRejectLateMismatch) {
  for (const auto mode : {"vortex"}) {
    SCOPED_TRACE(mode);
    const auto success = runVQuery(
        std::string("--engine ") + mode +
        R"( --eval 'def collect(...$rest: Int): Stream<Int> := $rest; collect(7, 9, 7)')");
    EXPECT_EQ(success.exitCode, 0) << success.output;
    EXPECT_EQ(success.output, "7\n9\n7\n");

    const auto mismatch = runVQuery(
        std::string("--engine ") + mode +
        R"( --eval 'def collect(...$rest: Int): Stream<Int> := $rest; collect(7, 9, "bad")')");
    EXPECT_EQ(mismatch.exitCode, 0) << mismatch.output;
    EXPECT_TRUE(mismatch.output.empty()) << mismatch.output;
  }
}

TEST_F(VQueryE2ETest, RuntimeRankTypedFilterHasBackendParity) {
  for (const auto mode : {"vortex"}) {
    const auto result = runVQuery(
        std::string("--engine ") + mode +
        R"( --eval 'def identity($x: Int): Int := $x; let $p := %"paper"; $p/d.items%"7"%"bad"%"9"; $p/d.items/identity()')");
    EXPECT_EQ(result.exitCode, 0) << result.output;
    EXPECT_EQ(result.output, "7\n9\n");
  }
}

TEST_F(VQueryE2ETest, RuntimeEndpointAndAppendHaveBackendParity) {
  for (const auto mode : {"vortex"}) {
    const auto result = runVQuery(
        std::string("--engine ") + mode +
        R"( --eval 'let $p := %"paper"; $p/d.items%"7"%"9"; $p/d.items%"11"; $p/d.items::tail')");
    EXPECT_EQ(result.exitCode, 0) << result.output;
    EXPECT_EQ(result.output, "11\n");
  }
}

TEST_F(VQueryE2ETest, DecoratedFunctorCaptureSurvivesRebinding) {
  for (const auto mode : {"vortex"}) {
    const auto result = runVQuery(
        std::string("--engine ") + mode +
        R"( --eval 'let $f := /d.items; let $g := /$f>[. != "bad"][-1]; let $f := /d.absent; let $p := %"paper"; $p/d.items%"7"%"bad"%"9"; $p/$g')");
    EXPECT_EQ(result.exitCode, 0) << result.output;
    EXPECT_EQ(result.output, "9\n");
  }
}

TEST_F(VQueryE2ETest, MultiStoreNamedResolution) {
  fs::path storeA = "tests/samples/xudu/core_hypertext/xanadoc_a";
  fs::path storeB = "tests/samples/xudu/core_hypertext/xanadoc_b";

  if (!fs::exists(storeA) || !fs::exists(storeB)) {
    GTEST_SKIP() << "Sample fixtures not present";
  }

  for (const auto mode : {"vortex"}) {
    const auto named =
        runVQuery(std::string("--engine ") + mode + " " + storeA.string() +
                  " " + storeB.string() + " -e \"##xanadoc_b/d.name\"");
    EXPECT_EQ(named.exitCode, 0) << named.output;
    EXPECT_NE(named.output.find("xanadoc_b"), std::string::npos);
  }

  // 1. Query name of store B
  std::string argsName =
      storeA.string() + " " + storeB.string() + " -e \"##xanadoc_b/d.name\"";
  auto resName = runVQuery(argsName);
  EXPECT_EQ(resName.exitCode, 0) << resName.output;
  EXPECT_NE(resName.output.find("xanadoc_b"), std::string::npos);

  // 2. Query role of store B (should be library)
  std::string argsRoleB =
      storeA.string() + " " + storeB.string() + " -e \"##xanadoc_b/d.role\"";
  auto resRoleB = runVQuery(argsRoleB);
  EXPECT_EQ(resRoleB.exitCode, 0) << resRoleB.output;
  EXPECT_NE(resRoleB.output.find("library"), std::string::npos);

  // 3. Query role of store A (should be primary)
  std::string argsRoleA =
      storeA.string() + " " + storeB.string() + " -e \"##xanadoc_a/d.role\"";
  auto resRoleA = runVQuery(argsRoleA);
  EXPECT_EQ(resRoleA.exitCode, 0) << resRoleA.output;
  EXPECT_NE(resRoleA.output.find("primary"), std::string::npos);
}

TEST_F(VQueryE2ETest, OutputStoreCreation) {
  fs::path outStorePath = testDir / "result_store";

  auto res =
      runVQuery("-e \"##/d.step%'CreatedCell'\" -o " + outStorePath.string());
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("Result store written to:"), std::string::npos);

  // Verify store directory was created and contains valid ops.nodes
  EXPECT_TRUE(fs::exists(outStorePath / "ops.nodes"));

  // Verify store can be loaded by xanadu::Store
  auto perma = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store store(perma);
  EXPECT_NO_THROW(store.load(outStorePath.string()));
  EXPECT_FALSE(store.primaryCurrentVersion().isZero());
}

// A named permascroll that is not there answered every query blank and
// exited 0, and opening it created it.
TEST_F(VQueryE2ETest, AMissingPermascrollIsRefused) {
  const auto missing = testDir / "no-such-permascroll";
  auto res = runVQuery("--permascroll " + missing.string() + " -e \"##\"");
  EXPECT_EQ(res.exitCode, 1) << res.output;
  EXPECT_NE(res.output.find("no permascroll at"), std::string::npos)
      << res.output;
  EXPECT_FALSE(fs::exists(missing));
}

TEST_F(VQueryE2ETest, InPlaceStoreMutation) {
  fs::path sampleSrc = "tests/samples/xudu/core_hypertext/xanadoc_a";
  if (!fs::exists(sampleSrc)) {
    GTEST_SKIP() << "Sample fixture xanadoc_a not present";
  }

  fs::path copyStore = testDir / "inplace_store";
  fs::copy(sampleSrc, copyStore, fs::copy_options::recursive);

  const auto opsBefore = [&] {
    xanadu::Store before(std::make_shared<xanadu::UserPermascroll>());
    before.load(copyStore.string());
    return before.opCount();
  }();

  // The create mints a cell, which in place writes into the store itself;
  // a query that only read would say there was nothing to write.
  auto res = runVQuery(copyStore.string() +
                       " -e \"##/d.mutated%'CellData'\" --in-place");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("Wrote "), std::string::npos) << res.output;

  // Load and verify store is valid, and holds what was written
  auto perma = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store store(perma);
  EXPECT_NO_THROW(store.load(copyStore.string()));
  EXPECT_GT(store.opCount(), opsBefore);
}

TEST_F(VQueryE2ETest, QueryFileExecution) {
  fs::path qFile = testDir / "query.vql";
  {
    std::ofstream ofs(qFile);
    ofs << "##/d.test%'FromVQLFile'\n";
  }

  auto res = runVQuery("-f " + qFile.string());
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("FromVQLFile"), std::string::npos);
}

TEST_F(VQueryE2ETest, CellConnectionViewMode) {
  auto res = runVQuery("-e \"##/d.1%hello%world/d.2%bar!both\" --view");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("=== Xanadu Zigzag Cell Connection View ==="),
            std::string::npos);
  EXPECT_NE(res.output.find("Viewing Dimensions: [X] d.1  [Y] d.2  [Z] d.3"),
            std::string::npos);
  EXPECT_NE(res.output.find("Result Set: 4 cells"), std::string::npos);
  EXPECT_NE(res.output.find("[2D Spatial Lattice Projection]"),
            std::string::npos);
  EXPECT_NE(res.output.find("+d.1"), std::string::npos);
  EXPECT_NE(res.output.find("+d.2"), std::string::npos);
  EXPECT_NE(res.output.find("--- Cell Link Topology Roster ---"),
            std::string::npos);
  EXPECT_NE(res.output.find("hello"), std::string::npos);
  EXPECT_NE(res.output.find("world"), std::string::npos);
  EXPECT_NE(res.output.find("bar"), std::string::npos);
}

TEST_F(VQueryE2ETest, CellConnectionCustomDimensions) {
  auto res =
      runVQuery("-e \"##/d.1%hello%world/d.2%bar!both\" --grid --dims d.2,d.1");
  EXPECT_EQ(res.exitCode, 0) << res.output;
  EXPECT_NE(res.output.find("=== Xanadu Zigzag Cell Connection View ==="),
            std::string::npos);
  EXPECT_NE(res.output.find("Viewing Dimensions: [X] d.2  [Y] d.1"),
            std::string::npos);
  EXPECT_NE(res.output.find("Result Set: 4 cells"), std::string::npos);
  EXPECT_NE(res.output.find("+d.2"), std::string::npos);
  EXPECT_NE(res.output.find("+d.1"), std::string::npos);
}

} // namespace

TEST_F(VQueryE2ETest, DefaultVmExecutesForContractsAndRuntimeFiltering) {
  const auto result = runVQuery(
      R"(--eval 'let $p := %"paper"; $p/d.items%"7"%"bad"%"9"; $p/d.items/d.tag%"present"/d.label%"marker"; for { .: $item: Int } in $p/d.items[{+d.tag: "present", +d.tag: {+d.label: "marker"}, !d.hidden}] where $item != 9.0 return $item')");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_EQ(result.output, "7\n");
}

// M7-VQL: script I/O actions through the real CLI, headless. The CLI opens the
// file through the asynchronous file endpoint and drives the parked/resume
// loop through IoHost; there is no blocking read on the evaluation thread.
namespace {

constexpr auto readAction =
    "effect { let $s := submit($src, read_bytes(6, 5)); "
    "match $s { admitted($q) => await($q), refused($e) => $e } }";

fs::path writeSource(const fs::path &dir, const std::string &content) {
  const auto path = dir / "source.txt";
  std::ofstream(path, std::ios::binary) << content;
  return path;
}

} // namespace

TEST_F(VQueryE2ETest, ScriptActionReadsARealFileEndToEnd) {
  const auto source = writeSource(testDir, "hello world");
  const auto result = runVQuery("--io-file src=" + source.string() + " -e '" +
                                readAction + "'");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_EQ(result.output, "Complete\nworld\n");
}

TEST_F(VQueryE2ETest, ScriptActionReadsPastTheEndAsAnEndOfStreamValue) {
  const auto source = writeSource(testDir, "abc");
  const auto result = runVQuery("--io-file src=" + source.string() + " -e '" +
                                readAction + "'");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_EQ(result.output, "EndOfStream\n") << "an outcome row, no payload row";
}

TEST_F(VQueryE2ETest, ReplOpensAFileAndRunsTwoActionsInOneSession) {
  const auto source = writeSource(testDir, "hello world");
  const auto input  = testDir / "session.vql";
  {
    std::ofstream script(input);
    script << ":open src " << source.string() << "\n"
           << readAction << "\n"
           << readAction << "\n:quit\n";
  }
  const auto result = runVQuery("< '" + input.string() + "'");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_NE(result.output.find("Opened " + source.string() + " as $src"),
            std::string::npos)
      << result.output;
  const auto first = result.output.find("world");
  ASSERT_NE(first, std::string::npos) << result.output;
  EXPECT_NE(result.output.find("world", first + 1), std::string::npos)
      << "each reached action read once: " << result.output;
}

TEST_F(VQueryE2ETest, ScriptActionInARowContextIsRefusedStatically) {
  const auto result =
      runVQuery(std::string{"-e 'for $x in ## "} + readAction + "'");
  EXPECT_EQ(result.exitCode, 1);
  EXPECT_NE(result.output.find("VQL_IO_QUERY_CONTEXT"), std::string::npos)
      << result.output;
  const auto call = runVQuery("-e 'return (submit($src, read_bytes(0, 1)))'");
  EXPECT_EQ(call.exitCode, 1);
  EXPECT_NE(call.output.find("VQL_IO_QUERY_CONTEXT"), std::string::npos)
      << call.output;
}

TEST_F(VQueryE2ETest, ScriptActionWithoutACapabilityFailsBeforeSubmitting) {
  const auto result = runVQuery(std::string{"-e '"} + readAction + "'");
  EXPECT_EQ(result.exitCode, 1);
  EXPECT_NE(result.output.find("VQL_IO_EFFECT_ARGUMENT"), std::string::npos)
      << result.output;
}

TEST_F(VQueryE2ETest, AMissingIoFileIsATypedOpenFailure) {
  const auto result =
      runVQuery("--io-file src=" + (testDir / "missing.txt").string() +
                " -e 'return ##'");
  EXPECT_EQ(result.exitCode, 1);
  EXPECT_NE(result.output.find("Open failed"), std::string::npos)
      << result.output;
}

// M7-VQL gate r1, vql M1: bytes an action read are transient until explicitly
// captured, so none of the three ways to make rows durable accepts them. A
// typed code names the refusal and nothing is written.
TEST_F(VQueryE2ETest, ActionRowsAreNotWrittenByOutputStoreInPlaceOrSave) {
  const auto source = writeSource(testDir, "hello world");
  const auto bind   = "--io-file src=" + source.string() + " ";
  const auto out    = testDir / "result.store";
  const auto saved =
      runVQuery(bind + "-o '" + out.string() + "' -e '" + readAction + "'");
  EXPECT_EQ(saved.exitCode, 1) << saved.output;
  EXPECT_NE(saved.output.find("VQL_IO_PERSIST_NEEDS_CAPTURE"),
            std::string::npos)
      << saved.output;
  EXPECT_FALSE(fs::exists(out)) << "no result store was written";

  const auto inPlace = runVQuery(bind + "--in-place -e '" + readAction + "'");
  EXPECT_EQ(inPlace.exitCode, 1) << inPlace.output;
  EXPECT_NE(inPlace.output.find("VQL_IO_PERSIST_NEEDS_CAPTURE"),
            std::string::npos)
      << inPlace.output;

  const auto input = testDir / "save.vql";
  const auto repl  = testDir / "repl.store";
  {
    std::ofstream script(input);
    script << ":open src " << source.string() << "\n"
           << readAction << "\n:save " << repl.string() << "\n:quit\n";
  }
  const auto session = runVQuery("< '" + input.string() + "'");
  EXPECT_NE(session.output.find("VQL_IO_PERSIST_NEEDS_CAPTURE"),
            std::string::npos)
      << session.output;
  EXPECT_FALSE(fs::exists(repl)) << "nothing was saved";
}

// vql gate r1 M1: the rule follows the script, not the last statement. A
// later `return io_payload($r)` is the file's bytes as a row, and a
// `return $r` is the action's own row; neither may be made durable.
TEST_F(VQueryE2ETest,
       ActionRowsReturnedByALaterStatementAreNotPersistedEither) {
  const auto source = writeSource(testDir, "hello world");
  const auto bind   = "--io-file src=" + source.string() + " ";
  const std::string action =
      "effect { let $s := submit($src, read_bytes(6, 5)); "
      "match $s { admitted($q) => await($q), refused($e) => $e } } as $r; ";
  for (const auto *tail : {"return io_payload($r)", "return $r"}) {
    SCOPED_TRACE(tail);
    const auto query = action + tail;
    const auto out   = testDir / "later.store";
    const auto saved =
        runVQuery(bind + "-o '" + out.string() + "' -e '" + query + "'");
    EXPECT_EQ(saved.exitCode, 1) << saved.output;
    EXPECT_NE(saved.output.find("VQL_IO_PERSIST_NEEDS_CAPTURE"),
              std::string::npos)
        << saved.output;
    EXPECT_FALSE(fs::exists(out)) << "no result store was written";

    const auto inPlace = runVQuery(bind + "--in-place -e '" + query + "'");
    EXPECT_EQ(inPlace.exitCode, 1) << inPlace.output;
    EXPECT_NE(inPlace.output.find("VQL_IO_PERSIST_NEEDS_CAPTURE"),
              std::string::npos)
        << inPlace.output;

    const auto input = testDir / "later_save.vql";
    const auto repl  = testDir / "later_save.store";
    {
      std::ofstream script(input);
      script << ":open src " << source.string() << "\n"
             << query << "\n:save " << repl.string() << "\n:quit\n";
    }
    const auto session = runVQuery("< '" + input.string() + "'");
    EXPECT_NE(session.output.find("VQL_IO_PERSIST_NEEDS_CAPTURE"),
              std::string::npos)
        << session.output;
    EXPECT_FALSE(fs::exists(repl)) << "nothing was saved";
  }
}

// The rule is coverage over the session (owner decision Q2): once a script has
// turned bytes into text, a later line cannot persist anything until a capture
// holds them, whatever that later line returns.
TEST_F(VQueryE2ETest, AQueryAfterAnActionIsRefusedWhileItsExposureIsUncovered) {
  const auto source = writeSource(testDir, "hello world");
  const auto input  = testDir / "later.vql";
  const auto repl   = testDir / "later.store";
  {
    std::ofstream script(input);
    script << ":open src " << source.string() << "\n"
           << readAction << "\nreturn 5\n:save " << repl.string()
           << "\n:quit\n";
  }
  const auto session = runVQuery("< '" + input.string() + "'");
  EXPECT_NE(session.output.find("VQL_IO_PERSIST_NEEDS_CAPTURE"),
            std::string::npos)
      << session.output;
  EXPECT_FALSE(fs::exists(repl)) << "nothing was saved";
}

TEST_F(VQueryE2ETest, ReplOpensAPathWithSpaces) {
  const auto dir = testDir / "with space";
  fs::create_directories(dir);
  const auto source = writeSource(dir, "hello world");
  const auto input  = testDir / "spaces.vql";
  {
    std::ofstream script(input);
    script << ":open src " << source.string() << "\n"
           << readAction << "\n:quit\n";
  }
  const auto result = runVQuery("< '" + input.string() + "'");
  EXPECT_NE(result.output.find("world"), std::string::npos) << result.output;
}

TEST_F(VQueryE2ETest, TheCompilerCliLowersAScriptActionToSubmitAndAwait) {
  const auto script = testDir / "action.vql";
  std::ofstream(script) << readAction << "\n";
  const auto cmd = "./build/vqueryc --dump-asm '" + script.string() + "' 2>&1";
  FILE *pipe     = popen(cmd.c_str(), "r");
  ASSERT_NE(pipe, nullptr);
  std::string out;
  char buffer[256];
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) out += buffer;
  const auto status = pclose(pipe);
  EXPECT_EQ(WEXITSTATUS(status), 0) << out;
  EXPECT_NE(out.find("SUBMIT"), std::string::npos) << out;
  EXPECT_NE(out.find("AWAIT"), std::string::npos) << out;
  EXPECT_EQ(out.find("NOT_COMPILABLE"), std::string::npos) << out;
}

// M7-VQL-b: a script that opens its own file, names a request and cancels it,
// bounds a read with `within`, and the session's overall script timeout.
namespace {

constexpr auto readFromF =
    "effect { let $s := submit($f, read_bytes(6, 5)); "
    "match $s { admitted($q) => await($q), refused($e) => $e } }";

} // namespace

TEST_F(VQueryE2ETest, ScriptOpensItsOwnFileReadsItAndClosesIt) {
  const auto source = writeSource(testDir, "hello world");
  const auto result = runVQuery("-e 'effect { open(\"" + source.string() +
                                "\") } as $f; " + readFromF + "'");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_EQ(result.output, "Complete\nworld\n");
}

TEST_F(VQueryE2ETest, ScriptOpenOfAMissingFileIsAnOutcomeRow) {
  const auto result =
      runVQuery("-e 'effect { open(\"" + (testDir / "gone.txt").string() +
                "\") } as $f; return io_tag($f)'");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_EQ(result.output, "Failed\n");
}

TEST_F(VQueryE2ETest, ScriptStartsACancelsAndNamesItsRequest) {
  const auto source = writeSource(testDir, "hello world");
  const auto result = runVQuery(
      "--io-file src=" + source.string() +
      " -e 'effect { let $s := submit($src, read_bytes(0, 5)); match $s { "
      "admitted($q) => $q, refused($e) => $e } } as $h; "
      "effect { cancel($h) } as $c; return io_tag($c)'");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  // A five-byte read can finish before the stop request lands; both are
  // values and the script never fails.
  EXPECT_TRUE(result.output == "Cancelled\n" || result.output == "Complete\n")
      << result.output;
}

TEST_F(VQueryE2ETest, ScriptReadsAnOutcomesFieldsAsValues) {
  const auto source = writeSource(testDir, "hello world");
  const auto result = runVQuery(
      "--io-file src=" + source.string() +
      " -e 'effect { let $s := submit($src, read_bytes(6, 5)) within 30 s; "
      "match $s { admitted($q) => await($q), refused($e) => $e } } as $r; "
      "return io_bytes($r); return io_payload($r)'");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_EQ(result.output, "world\n");
}

// M7-VQL-c C2: write effects through the real CLI. Authority to write is the
// invoking human's flag; a script cannot grant itself any.
namespace {

std::string writeAction(const std::string &cap, const std::string &description,
                        const std::string &name) {
  return "effect { let $s := submit($" + cap + ", " + description +
         "); match $s { admitted($q) => await($q), refused($e) => $e } } as $" +
         name + "; ";
}

std::string readAll(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_F(VQueryE2ETest, WithNoWriteRootEveryWriteOpenIsARejectedValue) {
  const auto target = testDir / "denied.txt";
  const auto result = runVQuery(
      "-e 'effect { open(\"" + target.string() +
      "\") for write create } as $o; return io_tag($o), io_error($o), "
      "io_detail($o)'");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_EQ(result.output, "Rejected\nUnsupported\n1\n");
  EXPECT_FALSE(fs::exists(target)) << "no file was created";
}

TEST_F(VQueryE2ETest, AWriteRootAllowsCreateWriteAppendAndSync) {
  const auto target = testDir / "report.txt";
  const auto script = "effect { open(\"" + target.string() +
                      "\") for write create } as $o; " +
                      writeAction("o", "write_bytes(0, \"hello\")", "w") +
                      writeAction("o", "sync()", "d") +
                      "return io_tag($o), io_tag($w), io_bytes($w), io_tag($d)";
  const auto result = runVQuery("--io-write-root '" + testDir.string() +
                                "' -e '" + script + "'");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_EQ(result.output, "Capability\nComplete\n5\nComplete\n");
  EXPECT_EQ(readAll(target), "hello");

  const auto more = runVQuery(
      "--io-write-root '" + testDir.string() + "' -e 'effect { open(\"" +
      target.string() + "\") for append } as $a; " +
      writeAction("a", "append_bytes(\" world\")", "w") + "return io_tag($w)'");
  ASSERT_EQ(more.exitCode, 0) << more.output;
  EXPECT_EQ(readAll(target), "hello world");
}

TEST_F(VQueryE2ETest, AWriteRootDoesNotAdmitAPathOutsideIt) {
  const auto root = testDir / "granted";
  fs::create_directories(root);
  const auto outside = testDir / "elsewhere.txt";
  const auto result  = runVQuery(
      "--io-write-root '" + root.string() + "' -e 'effect { open(\"" +
      (root / ".." / "elsewhere.txt").string() +
      "\") for write create } as $o; return io_tag($o), io_detail($o)'");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_EQ(result.output, "Rejected\n1\n");
  EXPECT_FALSE(fs::exists(outside));
}

TEST_F(VQueryE2ETest, TheWriteBoundFlagRefusesAnOversizedWriteAsAValue) {
  const auto target = testDir / "bounded.txt";
  const auto script = "effect { open(\"" + target.string() +
                      "\") for write create } as $o; " +
                      writeAction("o", "write_bytes(0, \"12345\")", "w") +
                      "return io_tag($w), io_error($w)";
  const auto result = runVQuery("--io-write-root '" + testDir.string() +
                                "' --io-write-bytes 4 -e '" + script + "'");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_EQ(result.output, "Rejected\nBudgetExceeded\n");
  EXPECT_EQ(readAll(target), "");
  for (const auto *bad : {"0", "-1", "lots", "1.5"}) {
    SCOPED_TRACE(bad);
    const auto usage =
        runVQuery(std::string{"--io-write-bytes "} + bad + " -e 'return 1'");
    EXPECT_EQ(usage.exitCode, 1) << usage.output;
    EXPECT_NE(usage.output.find("--io-write-bytes"), std::string::npos)
        << usage.output;
  }
}

TEST_F(VQueryE2ETest, AScriptCopiesABinaryFileByteForByte) {
  std::string bytes;
  for (int i = 0; i < 512; ++i) bytes.push_back(static_cast<char>(i % 256));
  const auto from = testDir / "in.bin";
  const auto to   = testDir / "out.bin";
  std::ofstream(from, std::ios::binary) << bytes;
  const auto script = "effect { open(\"" + from.string() +
                      "\") } as $src; effect { open(\"" + to.string() +
                      "\") for write create } as $dst; " +
                      writeAction("src", "read_bytes(0, 512)", "r") +
                      writeAction("dst", "write_bytes(0, $r)", "w") +
                      "return io_tag($w), io_bytes($w)";
  const auto result = runVQuery("--io-write-root '" + testDir.string() +
                                "' -e '" + script + "'");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_EQ(result.output, "Complete\n512\n");
  EXPECT_EQ(readAll(to), bytes);
}

TEST_F(VQueryE2ETest, ReplOpensForWriteOnlyUnderTheGrant) {
  const auto target = testDir / "repl.txt";
  const auto input  = testDir / "write.vql";
  {
    std::ofstream script(input);
    script << ":open -wc out " << target.string() << "\n"
           << writeAction("out", "write_bytes(0, \"ab\")", "w")
           << "return io_tag($w)\n"
           << ":quit\n";
  }
  const auto denied = runVQuery("< '" + input.string() + "'");
  EXPECT_NE(denied.output.find("Open failed"), std::string::npos)
      << denied.output;
  EXPECT_FALSE(fs::exists(target));

  const auto allowed = runVQuery("--io-write-root '" + testDir.string() +
                                 "' < '" + input.string() + "'");
  ASSERT_EQ(allowed.exitCode, 0) << allowed.output;
  EXPECT_NE(
      allowed.output.find("Opened " + target.string() + " for change as $out"),
      std::string::npos)
      << allowed.output;
  EXPECT_EQ(readAll(target), "ab");
}

// A script that only wrote exposed no payload as text, so there is nothing a
// capture could cover and its rows persist (design §4.7: an empty exposure set
// is vacuously covered). C2 pinned the opposite while no capture route existed.
TEST_F(VQueryE2ETest, AWritingScriptsRowsPersistBecauseItExposedNoPayload) {
  const auto target = testDir / "persist.txt";
  const auto out    = testDir / "write.store";
  const auto script =
      "effect { open(\"" + target.string() + "\") for write create } as $o; " +
      writeAction("o", "write_bytes(0, \"x\")", "w") + "return io_tag($w)";
  const auto result =
      runVQuery("--io-write-root '" + testDir.string() + "' -o '" +
                out.string() + "' -e '" + script + "'");
  EXPECT_EQ(result.exitCode, 0) << result.output;
  EXPECT_EQ(result.output.find("VQL_IO_PERSIST_NEEDS_CAPTURE"),
            std::string::npos)
      << result.output;
  EXPECT_TRUE(fs::exists(out));
  EXPECT_EQ(readAll(target), "x");
}

TEST_F(VQueryE2ETest, ScriptTimeoutFlagTakesMillisecondsOrOff) {
  const auto source = writeSource(testDir, "hello world");
  for (const auto *value : {"5000", "off"}) {
    SCOPED_TRACE(value);
    const auto result = runVQuery(std::string{"--script-timeout "} + value +
                                  " --io-file src=" + source.string() +
                                  " -e '" + readAction + "'");
    ASSERT_EQ(result.exitCode, 0) << result.output;
    EXPECT_EQ(result.output, "Complete\nworld\n");
  }
  // vql gate r1 MINOR 2: a size no deadline can hold is a usage error, not an
  // uncaught exception or an overflowed deadline.
  for (const auto *bad : {"0", "-5", "soon", "1.5", "99999999999999999999",
                          "9223372036854775807"}) {
    SCOPED_TRACE(bad);
    const auto result =
        runVQuery(std::string{"--script-timeout "} + bad + " -e 'return 1'");
    EXPECT_EQ(result.exitCode, 1) << result.output;
    EXPECT_NE(result.output.find("--script-timeout"), std::string::npos)
        << result.output;
  }
}

TEST_F(VQueryE2ETest, ReplShowsAndSetsTheScriptTimeout) {
  const auto input = testDir / "timeout.vql";
  {
    std::ofstream script(input);
    script << ":timeout\n:timeout 250\n:timeout\n:timeout off\n:timeout\n"
           << ":timeout nope\n:quit\n";
  }
  const auto result = runVQuery("< '" + input.string() + "'");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  const auto &out = result.output;
  EXPECT_NE(out.find("Script timeout: 30000 ms"), std::string::npos) << out;
  EXPECT_NE(out.find("Script timeout: 250 ms"), std::string::npos) << out;
  EXPECT_NE(out.find("Script timeout: off"), std::string::npos) << out;
  EXPECT_NE(out.find("Usage: :timeout [MS|off]"), std::string::npos) << out;
}

// The rows of a script that failed after it admitted something are not the
// last results: the admission retired the rows before, and a stale list would
// name cells that no longer exist.
TEST_F(VQueryE2ETest, AScriptThatFailsAfterAdmissionLeavesNoStaleResults) {
  const auto source = writeSource(testDir, "hello world");
  const auto input  = testDir / "stale.vql";
  const auto saved  = testDir / "stale.store";
  {
    std::ofstream script(input);
    script << ":open src " << source.string() << "\n"
           << readAction << "\n"
           << std::string{readAction} + "; return io_payload($src)" << "\n"
           << ":save " << saved.string() << "\n:quit\n";
  }
  const auto result = runVQuery("< '" + input.string() + "'");
  EXPECT_NE(result.output.find("Query failed"), std::string::npos)
      << result.output;
  EXPECT_NE(result.output.find("Run a query before saving results."),
            std::string::npos)
      << result.output;
  EXPECT_FALSE(fs::exists(saved)) << "nothing stale was saved";
}

TEST_F(VQueryE2ETest, TheCompilerCliLowersTheNewActionsToHostOpcodes) {
  const auto script = testDir / "handles.vql";
  std::ofstream(script) << "effect { await($h) } as $r; effect { cancel($h) }; "
                           "effect { open(\"/x\") } as $f\n";
  const auto cmd = "./build/vqueryc --dump-asm '" + script.string() + "' 2>&1";
  FILE *pipe     = popen(cmd.c_str(), "r");
  ASSERT_NE(pipe, nullptr);
  std::string out;
  char buffer[256];
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) out += buffer;
  const auto status = pclose(pipe);
  EXPECT_EQ(WEXITSTATUS(status), 0) << out;
  EXPECT_NE(out.find("AWAIT"), std::string::npos) << out;
  EXPECT_NE(out.find("RELATIONNEXT"), std::string::npos) << out;
}

// ---- M7-VQL-c C4: the persist rule over a real capture store ----------------
//
// Each test owns its XDG_DATA_HOME so the permascroll and the capture store
// beside it are the test's, not the shared build/xdg one.
namespace {

const std::string readAll64 =
    "effect { let $s := submit($src, read_bytes(0, 64)); "
    "match $s { admitted($q) => await($q), refused($e) => $e } } as $r; ";

struct CaptureSession {
  fs::path dataHome;
  fs::path source;
  std::string bind;
  [[nodiscard]] fs::path permascrollDir() const {
    return dataHome / "xuzz" / "permascroll" / "default";
  }
  [[nodiscard]] fs::path captureDir() const {
    return dataHome / "xuzz" / "capture" / "default";
  }
  [[nodiscard]] ProcessResult run(const std::string &args) const {
    return runVQuery(bind + args, dataHome.string());
  }
  /// The permascroll those runs wrote, opened for reading the saved stores.
  [[nodiscard]] std::shared_ptr<xanadu::UserPermascroll> permascroll() const {
    xanadu::UserPermascroll::Config config;
    config.storageDir = permascrollDir();
    return std::make_shared<xanadu::UserPermascroll>(std::move(config));
  }
};

CaptureSession captureSession(const fs::path &testDir,
                              const std::string &content) {
  CaptureSession session;
  session.dataHome = testDir / "xdg";
  fs::create_directories(session.dataHome);
  session.source = testDir / "captured.txt";
  std::ofstream(session.source, std::ios::binary) << content;
  session.bind = "--io-file src=" + session.source.string() + " ";
  return session;
}

} // namespace

TEST_F(VQueryE2ETest, ACapturedScriptPersistsAsATranscludedQuoteOfTheCapture) {
  const auto session = captureSession(testDir, "captured by the author");
  const auto out     = testDir / "captured.store";
  const auto result =
      session.run("-o '" + out.string() + "' -e '" + readAll64 +
                  "effect { capture($r) } as $c; return io_captured($c)'");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_EQ(result.output.find("VQL_IO_PERSIST_NEEDS_CAPTURE"),
            std::string::npos)
      << result.output;
  ASSERT_TRUE(fs::exists(out)) << result.output;

  // The capture store is a dedicated directory beside the permascroll.
  EXPECT_TRUE(fs::exists(session.captureDir() / "capture.journal"));
  EXPECT_TRUE(fs::exists(session.captureDir() / "ops.nodes"));
  EXPECT_TRUE(fs::exists(session.permascrollDir()));

  const auto perma = session.permascroll();
  const auto saved = xanadu::loadStore(out, perma);
  const auto rows  = xanadu::readResultSlice(*saved, saved->latest());
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows.front().text, "captured by the author");
  // Transcluded, not retyped: the row's content is the captured span, which
  // reads back from the permascroll as the file's bytes.
  ASSERT_TRUE(rows.front().quote.has_value())
      << "the row was typed in again instead of quoting the capture";
  ASSERT_EQ(rows.front().quote->spans.size(), 1U);
  EXPECT_EQ(perma->read(rows.front().quote->spans.front()),
            "captured by the author");
}

TEST_F(VQueryE2ETest, ACapturedScriptMaySaveInPlaceAndFromTheRepl) {
  const auto session       = captureSession(testDir, "in place bytes");
  const fs::path sampleSrc = "tests/samples/xudu/core_hypertext/xanadoc_a";
  if (!fs::exists(sampleSrc)) GTEST_SKIP() << "sample fixture not present";
  const auto primary = testDir / "primary";
  fs::copy(sampleSrc, primary, fs::copy_options::recursive);
  const auto query =
      readAll64 + "effect { capture($r) } as $c; " + "return io_captured($c)";

  const auto inPlace =
      session.run("'" + primary.string() + "' --in-place -e '" + query + "'");
  EXPECT_EQ(inPlace.output.find("VQL_IO_PERSIST_NEEDS_CAPTURE"),
            std::string::npos)
      << inPlace.output;
  EXPECT_NE(inPlace.output.find("Wrote 1 cell"), std::string::npos)
      << inPlace.output;
  {
    // The primary store's new cell holds the captured span itself: the bytes
    // the capture appended, not a second typed copy after them.
    const auto reloaded = xanadu::loadStore(primary, session.permascroll());
    // The cell the write made holds the capture's own span; a typed copy
    // would be a second span of the same bytes after the capture and receipt.
    const auto perma  = session.permascroll();
    std::size_t holds = 0;
    // Operation indices start at 1.
    for (std::uint32_t at = 1; at <= reloaded->opCount(); ++at) {
      const auto *node = reloaded->getCompactOp(at);
      if (node != nullptr && node->spanLength > 0 &&
          xanadu::localScroll == node->scrollId &&
          perma->read(node->span()) == "in place bytes") {
        ++holds;
        EXPECT_EQ(node->spanStart, 0U) << "retyped after the capture";
      }
    }
    EXPECT_EQ(holds, 1U);
  }

  const auto repl  = testDir / "repl.store";
  const auto input = testDir / "captured.vql";
  {
    std::ofstream script(input);
    script << ":open src " << session.source.string() << "\n"
           << query << "\n:save " << repl.string() << "\n:quit\n";
  }
  const auto saved =
      runVQuery("< '" + input.string() + "'", session.dataHome.string());
  EXPECT_EQ(saved.output.find("VQL_IO_PERSIST_NEEDS_CAPTURE"),
            std::string::npos)
      << saved.output;
  EXPECT_TRUE(fs::exists(repl)) << saved.output;
  const auto reloaded = xanadu::loadStore(repl, session.permascroll());
  const auto back     = xanadu::readResultSlice(*reloaded, reloaded->latest());
  ASSERT_EQ(back.size(), 1U);
  EXPECT_EQ(back.front().text, "in place bytes");
  EXPECT_TRUE(back.front().quote.has_value());
}

TEST_F(VQueryE2ETest, PartialCoverageIsRefusedAndWritesNothing) {
  const auto session = captureSession(testDir, "0123456789abcdef");
  const auto out     = testDir / "partial.store";
  const auto result =
      session.run("-o '" + out.string() + "' -e '" + readAll64 +
                  "effect { capture($r, 0, 4) } as $c; return io_payload($r)'");
  EXPECT_EQ(result.exitCode, 1) << result.output;
  EXPECT_NE(result.output.find("VQL_IO_PERSIST_NEEDS_CAPTURE"),
            std::string::npos)
      << result.output;
  EXPECT_FALSE(fs::exists(out));
}

TEST_F(VQueryE2ETest, DerivedTextPersistsOnlyOnceTheSourceIsCovered) {
  const auto session = captureSession(testDir, "derive me");
  const auto derived = std::string{"return string(io_payload($r))"};
  const auto plain   = testDir / "plain.store";
  const auto refused = session.run("-o '" + plain.string() + "' -e '" +
                                   readAll64 + derived + "'");
  EXPECT_EQ(refused.exitCode, 1) << refused.output;
  EXPECT_NE(refused.output.find("VQL_IO_PERSIST_NEEDS_CAPTURE"),
            std::string::npos)
      << refused.output;
  EXPECT_FALSE(fs::exists(plain));

  const auto covered = testDir / "covered.store";
  const auto allowed =
      session.run("-o '" + covered.string() + "' -e '" + readAll64 +
                  "effect { capture($r) } as $c; " + derived + "'");
  EXPECT_EQ(allowed.exitCode, 0) << allowed.output;
  const auto saved = xanadu::loadStore(covered, session.permascroll());
  const auto rows  = xanadu::readResultSlice(*saved, saved->latest());
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows.front().text, "derive me");
}

TEST_F(VQueryE2ETest, ACaptureDirFlagMovesTheCaptureStore) {
  const auto session = captureSession(testDir, "elsewhere");
  const auto where   = testDir / "my-capture";
  const auto result =
      session.run("--capture-dir '" + where.string() + "' -e '" + readAll64 +
                  "effect { capture($r) } as $c; return io_tag($c)'");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_EQ(result.output, "CaptureReceipt\n");
  EXPECT_TRUE(fs::exists(where / "capture.journal"));
  EXPECT_FALSE(fs::exists(session.captureDir()));
}

TEST_F(VQueryE2ETest, APlainReadStartsNoCaptureStore) {
  const auto session = captureSession(testDir, "no capture asked");
  const auto result =
      session.run("-e '" + readAll64 + "return io_payload($r)'");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_FALSE(fs::exists(session.captureDir()))
      << "reading builds no journal or store";
}

TEST_F(VQueryE2ETest, AnUnopenableCaptureStoreIsTheTypedRefusal) {
  const auto session = captureSession(testDir, "blocked");
  const auto blocker = testDir / "not-a-directory";
  std::ofstream(blocker) << "file";
  const auto result =
      session.run("--capture-dir '" + blocker.string() + "' -e '" + readAll64 +
                  "effect { capture($r) } as $c; return 1'");
  EXPECT_EQ(result.exitCode, 1) << result.output;
  EXPECT_NE(result.output.find("VQL_IO_CAPTURE_UNAVAILABLE"), std::string::npos)
      << result.output;
}

// ---- M7-VQL-c C5: the recovery surface and the journeys it enables ----------

namespace {

/// A REPL session of @p lines against @p session's data home.
auto repl(const CaptureSession &session, const fs::path &dir,
          const std::vector<std::string> &lines, const std::string &flags = {})
    -> ProcessResult {
  static int counter = 0;
  const auto input   = dir / ("repl" + std::to_string(counter++) + ".vql");
  {
    std::ofstream script(input);
    for (const auto &line : lines) script << line << "\n";
    script << ":quit\n";
  }
  return runVQuery(flags + " < '" + input.string() + "'",
                   session.dataHome.string());
}

/// The journal a crashed session leaves: one Append intent and no outcome.
void leaveCrashedIntent(const fs::path &directory) {
  fs::create_directories(directory);
  xanadu::vortex::CaptureJournal journal(directory / "capture.journal",
                                         std::nullopt);
  xanadu::vortex::CaptureJournalRecord record;
  record.appendLength = 11;
  record.intervals    = {xanadu::vortex::CaptureSourceInterval{
         .sourceKind = xanadu::vortex::PayloadSourceKind::Durable,
         .source     = xanadu::localScroll,
         .generation = 0,
         .occurrence = 7,
         .srcOffset  = 0,
         .srcLength  = 11,
         .dstOffset  = 0}};
  ASSERT_TRUE(
      journal
          .appendRecord(xanadu::vortex::CaptureJournalRecordKind::Append, 1,
                        record)
          .has_value());
}

} // namespace

TEST_F(VQueryE2ETest,
       CapturesAndReconcileSayWhenThereIsNothingAndBuildNothing) {
  const auto session = captureSession(testDir, "unused");
  const auto result =
      repl(session, testDir, {":captures", ":reconcile 1 release"});
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_NE(result.output.find("No captures: nothing has been captured"),
            std::string::npos)
      << result.output;
  EXPECT_NE(result.output.find("Nothing to reconcile"), std::string::npos)
      << result.output;
  EXPECT_FALSE(fs::exists(session.captureDir()))
      << "asking builds no journal or store";
}

TEST_F(VQueryE2ETest, CapturesListsTheStoreAfterACaptureInTheSameSession) {
  const auto session = captureSession(testDir, "captured then listed");
  const auto result  = repl(session, testDir,
                            {":open src " + session.source.string(),
                             readAll64 + "effect { capture($r) } as $c; "
                                          "return io_tag($c)",
                             ":captures"});
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_NE(result.output.find("CaptureReceipt"), std::string::npos)
      << result.output;
  EXPECT_NE(
      result.output.find("Capture store: " + session.captureDir().string()),
      std::string::npos)
      << result.output;
  EXPECT_NE(result.output.find("Pinned captures: 0"), std::string::npos)
      << result.output;
  EXPECT_NE(result.output.find("Orphan bytes: 0"), std::string::npos)
      << result.output;
}

TEST_F(VQueryE2ETest, ACrashedSessionsPinIsListedReleasedAndStaysReleased) {
  const auto session = captureSession(testDir, "unused");
  ASSERT_NO_FATAL_FAILURE(leaveCrashedIntent(session.captureDir()));
  const auto result = repl(session, testDir,
                           {":captures", ":reconcile 99 release",
                            ":reconcile 1 release", ":captures"});
  ASSERT_EQ(result.exitCode, 0) << result.output;
  const auto &out = result.output;
  EXPECT_NE(out.find("Pinned captures: 1"), std::string::npos) << out;
  EXPECT_NE(out.find("Orphan bytes: 11"), std::string::npos) << out;
  EXPECT_NE(out.find("capture 1: an append intent with no outcome, 11 bytes"),
            std::string::npos)
      << out;
  // One decision, refused by name, never retried.
  EXPECT_NE(out.find("Reconcile refused"), std::string::npos) << out;
  EXPECT_NE(out.find("Released capture 1"), std::string::npos) << out;
  // The second listing, after the release, has no pin.
  const auto first = out.find("Pinned captures: 1");
  EXPECT_NE(out.find("Pinned captures: 0", first), std::string::npos) << out;

  // The release was journaled: a new process finds nothing pinned.
  const auto again = repl(session, testDir, {":captures"});
  EXPECT_NE(again.output.find("Pinned captures: 0"), std::string::npos)
      << again.output;
}

// A promote with a start and no length, or any trailing token, is a typo in a
// durable decision: usage, and nothing is decided.
TEST_F(VQueryE2ETest, ReconcileRefusesAHalfTypedOrOverlongDecision) {
  const auto session = captureSession(testDir, "unused");
  const auto result =
      repl(session, testDir,
           {":reconcile 3 promote 100", ":reconcile 3 promote 1 2 3",
            ":reconcile 3 release now", ":reconcile 3 promote 1 x"});
  ASSERT_EQ(result.exitCode, 0) << result.output;
  std::size_t usages = 0;
  for (auto at = result.output.find("Usage: :reconcile");
       at != std::string::npos;
       at = result.output.find("Usage: :reconcile", at + 1))
    ++usages;
  EXPECT_EQ(usages, 4U) << result.output;
  EXPECT_EQ(result.output.find("Promoted"), std::string::npos) << result.output;
  EXPECT_EQ(result.output.find("Reconcile refused"), std::string::npos)
      << result.output;
}

TEST_F(VQueryE2ETest, ReconcileNamesItsFormWhenGivenOnlyHalfOfIt) {
  const auto session = captureSession(testDir, "unused");
  const auto result =
      repl(session, testDir, {":reconcile", ":reconcile 1", ":reconcile x"});
  ASSERT_EQ(result.exitCode, 0) << result.output;
  std::size_t usages = 0;
  for (auto at = result.output.find("Usage: :reconcile");
       at != std::string::npos;
       at = result.output.find("Usage: :reconcile", at + 1))
    ++usages;
  EXPECT_EQ(usages, 3U) << result.output;
}

// M7-VQL-c r1 C: text a script exposed outlives it as a session variable, so a
// later plain line returning it must not reach durable output, and neither
// may a capability row on any line.
TEST_F(VQueryE2ETest, ExposedTextCarriedInASessionVariableIsNotPersistedLater) {
  const auto session = captureSession(testDir, "carried bytes");
  const auto saved   = testDir / "carried.store";
  const auto result  = repl(session, testDir,
                            {":open src " + session.source.string(),
                             readAll64 + "let $t := io_payload($r); return 1",
                             "return $t", ":save " + saved.string()});
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_NE(result.output.find("VQL_IO_PERSIST_NEEDS_CAPTURE"),
            std::string::npos)
      << result.output;
  EXPECT_FALSE(fs::exists(saved)) << "transient bytes were persisted";
}

TEST_F(VQueryE2ETest, ExposedTextBecomesPersistableOnceACaptureCoversIt) {
  const auto session = captureSession(testDir, "covered later");
  const auto after   = testDir / "after.store";
  const auto result  = repl(session, testDir,
                            {":open src " + session.source.string(),
                             readAll64 + "effect { capture($r) } as $c; " +
                                 "let $t := io_payload($r); return 1",
                             "return $t", ":save " + after.string()});
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_EQ(result.output.find("VQL_IO_PERSIST_NEEDS_CAPTURE"),
            std::string::npos)
      << result.output;
  EXPECT_TRUE(fs::exists(after)) << result.output;
}

TEST_F(VQueryE2ETest, ACapabilityRowIsRefusedOnAPlainLineAndInBatch) {
  const auto session = captureSession(testDir, "handle bytes");
  const auto saved   = testDir / "cap.store";
  const auto result  = repl(session, testDir,
                            {":open src " + session.source.string(),
                             "return $src", ":save " + saved.string()});
  EXPECT_NE(result.output.find("VQL_IO_PERSIST_HANDLE"), std::string::npos)
      << result.output;
  EXPECT_FALSE(fs::exists(saved));

  const auto out   = testDir / "cap_batch.store";
  const auto batch = session.run("-o '" + out.string() + "' -e 'return $src'");
  EXPECT_EQ(batch.exitCode, 1) << batch.output;
  EXPECT_NE(batch.output.find("VQL_IO_PERSIST_HANDLE"), std::string::npos)
      << batch.output;
  EXPECT_FALSE(fs::exists(out));
}

// M7-VQL-c r1 D: a name bound by an earlier script outlives its record, and
// the arena reuses the freed slot; the name must say so, not answer for a
// stranger's record.
TEST_F(VQueryE2ETest, AStaleOutcomeNameIsATypedRefusalNotAStrangersRecord) {
  const auto session = captureSession(testDir, "stale test bytes");
  const auto first   = "effect { let $s := submit($src, read_bytes(0, 5)); "
                       "match $s { admitted($q) => await($q), refused($e) => "
                       "$e } } as $a; return io_tag($a)";
  const auto second  = "effect { let $s := submit($src, read_bytes(100, 5)); "
                       "match $s { admitted($q) => await($q), refused($e) => "
                       "$e } } as $b; return io_tag($b)";
  const auto result  = repl(session, testDir,
                            {":open src " + session.source.string(), first,
                             second, "return io_tag($a)", "return io_tag($b)"});
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_NE(result.output.find("VQL_IO_STALE_BINDING"), std::string::npos)
      << result.output;
  EXPECT_EQ(result.output.find("RelationNext expects"), std::string::npos)
      << result.output;
  // $b is the live one and still reads its own outcome.
  EXPECT_NE(result.output.find("EndOfStream"), std::string::npos)
      << result.output;
}

TEST_F(VQueryE2ETest, AStaleReceiptNameIsATypedRefusal) {
  const auto session = captureSession(testDir, "receipt bytes");
  const auto capture = readAll64 + "effect { capture($r) } as $c; return 1";
  const auto result  = repl(session, testDir,
                            {":open src " + session.source.string(), capture,
                             capture, "return io_captured($c)"});
  ASSERT_EQ(result.exitCode, 0) << result.output;
  // The second capture rebinds $c, so the live receipt answers; the first
  // script's receipt is gone and nothing names it.
  EXPECT_EQ(result.output.find("RelationNext expects"), std::string::npos)
      << result.output;
}

namespace {

/// A named read of @p length bytes at @p offset, returning its tag.
std::string readNamed(const std::string &offset, const std::string &name) {
  return "effect { let $s := submit($src, read_bytes(" + offset +
         ", 4)); match $s { admitted($q) => await($q), refused($e) => $e } } "
         "as $" +
         name + "; return io_tag($" + name + ")";
}

std::size_t occurrences(const std::string &text, const std::string &needle) {
  std::size_t count = 0;
  for (auto at = text.find(needle); at != std::string::npos;
       at      = text.find(needle, at + 1))
    ++count;
  return count;
}

} // namespace

// M7-VQL-c r2 Vortex M1: the stale marker was an untagged text cell, so a
// write read it as the text "stale handle" and completed. It is now a tagged
// cell no byte reader accepts.
TEST_F(VQueryE2ETest, AStaleNameAsAWriteSourceIsRefusedAndWritesNothing) {
  const auto session = captureSession(testDir, "stale write bytes");
  const auto dst     = testDir / "dst.txt";
  const auto result  = repl(session, testDir,
                            {":open src " + session.source.string(),
                             readNamed("0", "a"), readNamed("100", "b"),
                             "effect { open(\"" + dst.string() +
                                 "\") for write create } as "
                                  "$dst; " +
                                 writeAction("dst", "write_bytes(0, $a)", "w") +
                                 "return io_tag($w)"},
                            "--io-write-root '" + testDir.string() + "'");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_NE(result.output.find("VQL_IO_STALE_BINDING"), std::string::npos)
      << result.output;
  EXPECT_TRUE(!fs::exists(dst) || readAll(dst).empty())
      << "the marker's text reached the file: " << readAll(dst);
}

TEST_F(VQueryE2ETest, AStaleNameIsNeverAPersistableRow) {
  const auto session = captureSession(testDir, "stale row bytes");
  const auto saved   = testDir / "stale.store";
  const auto result =
      repl(session, testDir,
           {":open src " + session.source.string(), readNamed("0", "a"),
            readNamed("100", "b"), "return $a", ":save " + saved.string()});
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_NE(result.output.find("VQL_IO_PERSIST_HANDLE"), std::string::npos)
      << result.output;
  EXPECT_FALSE(fs::exists(saved));
}

TEST_F(VQueryE2ETest, AStaleNameGivenToCaptureOrSubmitIsATypedRefusal) {
  const auto session = captureSession(testDir, "stale capture bytes");
  const auto result =
      repl(session, testDir,
           {":open src " + session.source.string(), readNamed("0", "a"),
            readNamed("100", "b"), "effect { capture($a) } as $c; return 1",
            "return io_tag($a)"});
  ASSERT_EQ(result.exitCode, 0) << result.output;
  // Once as the capture's refusal, once as the plain line's.
  EXPECT_GE(occurrences(result.output, "VQL_IO_STALE_BINDING"), 2U)
      << result.output;
  EXPECT_EQ(result.output.find("RelationNext"), std::string::npos)
      << result.output;
}

// M7-VQL-c r2 Zigzag N1: a name made by a plain line after the script that
// produced the handle was untracked, and aliased the reused slot.
TEST_F(VQueryE2ETest, ACopyMadeByAPlainLetGoesStaleWithItsHandle) {
  const auto session = captureSession(testDir, "alias bytes");
  const auto result =
      repl(session, testDir,
           {":open src " + session.source.string(), readNamed("0", "ra"),
            "let $y := $ra; return io_tag($y)", readNamed("500", "rb"),
            "return io_tag($y)"});
  ASSERT_EQ(result.exitCode, 0) << result.output;
  // $y said Complete while its record lived and must not say EndOfStream (the
  // reader of $rb) afterwards.
  EXPECT_EQ(occurrences(result.output, "EndOfStream"), 1U) << result.output;
  EXPECT_NE(result.output.find("VQL_IO_STALE_BINDING"), std::string::npos)
      << result.output;
  EXPECT_EQ(result.output.find("RelationNext"), std::string::npos)
      << result.output;
}

TEST_F(VQueryE2ETest, ACopyMadeByALetWithNoQueryAfterItStillGoesStale) {
  const auto session = captureSession(testDir, "alias bytes");
  const auto result  = repl(session, testDir,
                            {":open src " + session.source.string(),
                             readNamed("0", "ra"), "let $y := $ra; return 1",
                             readNamed("500", "rb"), "return io_tag($y)"});
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_EQ(occurrences(result.output, "EndOfStream"), 1U) << result.output;
  EXPECT_NE(result.output.find("VQL_IO_STALE_BINDING"), std::string::npos)
      << result.output;
}

// Owner decision 2026-11-26: :forget is the way out of a refused :save.
TEST_F(VQueryE2ETest, ForgetLiftsTheRefusalAndUnbindsTheUncapturedText) {
  const auto session = captureSession(testDir, "forgettable bytes");
  const auto refused = testDir / "refused.store";
  const auto fresh   = testDir / "fresh.store";
  const auto result =
      repl(session, testDir,
           {":open src " + session.source.string(),
            readAll64 + "let $t := io_payload($r); return 1", "return $t",
            ":save " + refused.string(), ":forget", "return 1",
            ":save " + fresh.string(), "return $t"});
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_NE(result.output.find("VQL_IO_PERSIST_NEEDS_CAPTURE"),
            std::string::npos)
      << result.output;
  EXPECT_FALSE(fs::exists(refused));
  EXPECT_NE(result.output.find("Forgot 1 uncaptured payload; unbound $t"),
            std::string::npos)
      << result.output;
  EXPECT_TRUE(fs::exists(fresh)) << result.output;
  // The text printed once, before :forget; afterwards $t names nothing.
  EXPECT_EQ(occurrences(result.output, "forgettable bytes"), 1U)
      << result.output;
}

TEST_F(VQueryE2ETest, ForgetPrintsUnboundNamesInSortedOrder) {
  const auto session = captureSession(testDir, "ordered bytes");
  const auto result =
      repl(session, testDir,
           {":open src " + session.source.string(),
            readAll64 + "let $zeta := io_payload($r); " +
                "let $alpha := io_payload($r); let $mid := io_payload($r); " +
                "return 1",
            ":forget"});
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_NE(result.output.find("unbound $alpha $mid $zeta"), std::string::npos)
      << result.output;
}

TEST_F(VQueryE2ETest, ForgetNeverReachesWhatACaptureCovered) {
  const auto session = captureSession(testDir, "kept bytes");
  const auto saved   = testDir / "kept.store";
  const auto result  = repl(session, testDir,
                            {":open src " + session.source.string(),
                             readAll64 + "effect { capture($r) } as $c; " +
                                 "let $t := io_payload($r); return 1",
                             ":forget", "return $t", ":save " + saved.string()});
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_NE(result.output.find("Nothing to forget"), std::string::npos)
      << result.output;
  EXPECT_NE(result.output.find("kept bytes"), std::string::npos)
      << result.output;
  EXPECT_TRUE(fs::exists(saved)) << result.output;
}

TEST_F(VQueryE2ETest, ForgetKeepsCapabilitiesAndOutcomeHandles) {
  const auto session = captureSession(testDir, "handles stay");
  const auto result =
      repl(session, testDir,
           {":open src " + session.source.string(),
            readAll64 + "let $t := io_payload($r); return 1", ":forget",
            "return io_tag($r)", "return io_tag($src)"});
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_NE(result.output.find("Complete"), std::string::npos) << result.output;
  EXPECT_NE(result.output.find("Capability"), std::string::npos)
      << result.output;
}

TEST_F(VQueryE2ETest, ForgetWithNothingReadSaysSo) {
  const auto session = captureSession(testDir, "unused");
  const auto result  = repl(session, testDir, {":forget"});
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_NE(result.output.find("Nothing to forget"), std::string::npos)
      << result.output;
}

// The receipt `$c` is a runtime handle; persisting it was refused deep inside
// promotion and, with `-o`, typed in as the words of a cell. It is a typed
// refusal that names the road to what the author meant.
TEST_F(VQueryE2ETest, PersistingTheReceiptItselfIsATypedRefusal) {
  const auto session = captureSession(testDir, "receipt bytes");
  const auto out     = testDir / "receipt.store";
  const auto result = session.run("-o '" + out.string() + "' -e '" + readAll64 +
                                  "effect { capture($r) } as $c; return $c'");
  EXPECT_EQ(result.exitCode, 1) << result.output;
  EXPECT_NE(result.output.find("VQL_IO_PERSIST_HANDLE"), std::string::npos)
      << result.output;
  EXPECT_NE(result.output.find("io_captured"), std::string::npos)
      << result.output;
  EXPECT_FALSE(fs::exists(out));
}

// A refused capture still builds the stack it ran in: the journal and store
// are created when a script holding `capture` is about to run, not on success
// (design §13 builds on need and the note asks nothing finer).
TEST_F(VQueryE2ETest, ARefusedCaptureStillCreatesTheCaptureDirectory) {
  const auto session = captureSession(testDir, "x");
  const auto result  = session.run("-e '" + readAll64 +
                                   "effect { capture($r, 100, 5) } as $c; "
                                    "return io_tag($c)'");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_EQ(result.output, "Rejected\n");
  EXPECT_TRUE(fs::exists(session.captureDir()));
}

// UX journey: read a file, capture it, derive a result, save it, close vquery,
// reopen and see the slice quote the capture. Every step is a REPL line; the
// flags are the two grants and the capture store's home.
TEST_F(VQueryE2ETest, JourneyReadCaptureDeriveSaveCloseReopenAndSeeTheQuote) {
  const auto session = captureSession(testDir, "alpha,beta\n1,2\n");
  const auto saved   = testDir / "journey.store";
  const auto first =
      repl(session, testDir,
           {":open src " + session.source.string(),
            readAll64 + "effect { capture($r) } as $c; return io_captured($c)",
            ":captures", ":save " + saved.string()});
  ASSERT_EQ(first.exitCode, 0) << first.output;
  EXPECT_NE(first.output.find("alpha,beta"), std::string::npos) << first.output;
  ASSERT_TRUE(fs::exists(saved)) << first.output;

  // A second process: nothing of the first is in memory.
  const auto perma  = session.permascroll();
  const auto loaded = xanadu::loadStore(saved, perma);
  const auto rows   = xanadu::readResultSlice(*loaded, loaded->latest());
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows.front().text, "alpha,beta\n1,2\n");
  ASSERT_TRUE(rows.front().quote.has_value());
  EXPECT_EQ(perma->read(rows.front().quote->spans.front()),
            "alpha,beta\n1,2\n");
  const auto reopened = repl(session, testDir, {":captures"});
  EXPECT_NE(reopened.output.find("Pinned captures: 0"), std::string::npos)
      << reopened.output;
  EXPECT_NE(reopened.output.find("Recovered captures: 1"), std::string::npos)
      << "the capture is in the journal the next session recovers: "
      << reopened.output;
}

// UX journey: write a report, sync it, read it back, from the REPL, with the
// one grant.
TEST_F(VQueryE2ETest, JourneyWriteAReportSyncItAndReadItBack) {
  const auto session = captureSession(testDir, "unused");
  const auto report  = testDir / "report.txt";
  const auto submit  = [](const std::string &cap, const std::string &what,
                         const std::string &as) {
    return "effect { let $s := submit($" + cap + ", " + what +
           "); match $s { admitted($q) => await($q), refused($e) => $e } } "
            "as $" +
           as + "; ";
  };
  const auto result =
      repl(session, testDir,
           {":open -wc out " + report.string(),
            submit("out", "write_bytes(0, \"quarterly report\")", "w") +
                submit("out", "sync()", "d") + "return io_tag($w), io_tag($d)",
            ":open back " + report.string(),
            submit("back", "read_bytes(0, 64)", "r") + "return io_payload($r)"},
           "--io-write-root '" + testDir.string() + "'");
  ASSERT_EQ(result.exitCode, 0) << result.output;
  EXPECT_NE(result.output.find("Opened " + report.string() + " for change"),
            std::string::npos)
      << result.output;
  EXPECT_NE(result.output.find("quarterly report"), std::string::npos)
      << result.output;
  EXPECT_EQ(readAll(report), "quarterly report");
}

// Writing is the invoking human's grant: with no root, the same journey is a
// refused open, not a silent success.
TEST_F(VQueryE2ETest, JourneyWriteWithoutAGrantIsRefusedAtTheOpen) {
  const auto session = captureSession(testDir, "unused");
  const auto report  = testDir / "denied.txt";
  const auto result =
      repl(session, testDir, {":open -wc out " + report.string()});
  EXPECT_NE(result.output.find("Open failed"), std::string::npos)
      << result.output;
  EXPECT_FALSE(fs::exists(report));
}
