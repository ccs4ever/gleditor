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
#include <string>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace {

namespace fs = std::filesystem;

struct ProcessResult {
  int exitCode{0};
  std::string output;
};

ProcessResult runVQuery(const std::string &args) {
  std::string binary = "./build/vquery";
  if (!fs::exists(binary)) {
    return {-1, "vquery binary not found at " + binary};
  }

  std::string cmd =
      "SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy "
      "LIBGL_ALWAYS_SOFTWARE=1 XDG_DATA_HOME=" +
      fs::current_path().string() +
      "/build/xdg/data XDG_CONFIG_HOME=" + fs::current_path().string() +
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

TEST_F(VQueryE2ETest, AQueryAfterAnActionIsSavableAgain) {
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
  EXPECT_EQ(session.output.find("VQL_IO_PERSIST_NEEDS_CAPTURE"),
            std::string::npos)
      << session.output;
  EXPECT_TRUE(fs::exists(repl)) << session.output;
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
