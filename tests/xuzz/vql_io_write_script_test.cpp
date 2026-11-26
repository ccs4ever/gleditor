// M7-VQL-c C2: VQL write effects (design/vortex-io-m7-vql-c.md §3, §4.1-§4.4,
// §6.1, §16 criteria 1, 2, 3, 5 and 14 for writes). `open(p) for write|append
// [create]`, `write_bytes`, `append_bytes` and `sync()` are request
// descriptions through the one submit, so `within`, `as`, handles and `cancel`
// need no change; the host's grant, never the script, decides what may be
// opened for writing. Everything here is headless: real files in a temp
// directory, and a fake backend where an outcome has to be forced.

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include "common/xanadu/vortex/file_read_host.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/vql/script_runner.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "io_frontend_conformance.hpp"

namespace {

using namespace xuzz_conformance;
namespace fs = std::filesystem;
using xanadu::vql::ParseError;
using xanadu::vql::Parser;
using xanadu::vql::ScriptIoPorts;
using xanadu::vql::ScriptRunner;
using xanadu::vql::ScriptRunResult;
using xanadu::vql::StageFailure;
using xanadu::vql::VQLEngine;

/// One submit of @p description on $cap, awaited, bound to $name.
auto act(const std::string &cap, const std::string &description,
         const std::string &name, const std::string &within = {})
    -> std::string {
  return "effect { let $s := submit($" + cap + ", " + description + ")" +
         (within.empty() ? "" : " within " + within) +
         "; match $s { admitted($q) => await($q), refused($e) => $e } } as $" +
         name + "; ";
}

/// The same, started and not awaited: `$name` is the request handle.
auto detached(const std::string &cap, const std::string &description,
              const std::string &name) -> std::string {
  return "effect { let $s := submit($" + cap + ", " + description +
         "); match $s { admitted($q) => $q, refused($e) => $e } } as $" + name +
         "; ";
}

auto open(const std::string &path, const std::string &mode,
          const std::string &name) -> std::string {
  return "effect { open(\"" + path + "\") " + mode + " } as $" + name + "; ";
}

auto slurp(const fs::path &path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

auto parseFailure(const std::string &source) -> std::string {
  try {
    Parser(source).parseQuery();
  } catch (const ParseError &error) {
    return error.what();
  }
  return {};
}

// ---- Grammar ----------------------------------------------------------------

TEST(VqlIoWriteGrammarTest, WriteAppendAndSyncAreSubmitDescriptions) {
  using Request    = xanadu::vql::IoRequestKind;
  const auto query = Parser(act("o", "write_bytes(4, \"abc\")", "w") +
                            act("o", "append_bytes($line)", "a") +
                            act("o", "sync()", "d", "2 s"))
                         .parseQuery();
  ASSERT_EQ(query.body.statements.size(), 3U);
  const auto &write =
      std::get<xanadu::vql::IoEffectStatement>(query.body.statements[0]);
  EXPECT_EQ(write.request, Request::WriteBytes);
  EXPECT_EQ(std::get<std::int64_t>(write.offset), 4);
  EXPECT_EQ(write.source.text, "abc");
  EXPECT_FALSE(write.source.isVariable);
  const auto &append =
      std::get<xanadu::vql::IoEffectStatement>(query.body.statements[1]);
  EXPECT_EQ(append.request, Request::AppendBytes);
  EXPECT_EQ(append.source.text, "line");
  EXPECT_TRUE(append.source.isVariable);
  const auto &sync =
      std::get<xanadu::vql::IoEffectStatement>(query.body.statements[2]);
  EXPECT_EQ(sync.request, Request::Sync);
  EXPECT_TRUE(sync.within.has_value());
}

TEST(VqlIoWriteGrammarTest, OpenModesParse) {
  using Mode = xanadu::vql::IoOpenMode;
  const std::vector<std::pair<std::string, Mode>> cases{
      {"", Mode::Read},
      {"for write", Mode::Write},
      {"for write create", Mode::WriteCreate},
      {"for append", Mode::Append},
      {"for append create", Mode::AppendCreate},
  };
  for (const auto &[text, mode] : cases) {
    SCOPED_TRACE(text);
    const auto query = Parser(open("x", text, "o")).parseQuery();
    EXPECT_EQ(std::get<xanadu::vql::IoEffectStatement>(query.body.statements[0])
                  .openMode,
              mode);
  }
  EXPECT_FALSE(parseFailure(open("x", "for sideways", "o")).empty());
  EXPECT_FALSE(parseFailure(open("x", "for create", "o")).empty());
}

TEST(VqlIoWriteGrammarTest, EveryNewFormIsRefusedWhereSubmitIs) {
  const std::string write = act("o", "write_bytes(0, \"x\")", "w");
  for (const auto &source : {
           "for $x in ## " + write,
           "def f() := { " + write + " }; f()",
           "watch ##/d.x as $v { " + write + " }",
           std::string{"let $x := ## "} + write,
       }) {
    SCOPED_TRACE(source);
    EXPECT_NE(parseFailure(source).find("VQL_IO_QUERY_CONTEXT"),
              std::string::npos);
  }
}

TEST(VqlIoWriteGrammarTest, TheNewNamesAreReservedInExpressions) {
  for (const auto *name : {"write_bytes", "append_bytes", "sync"}) {
    SCOPED_TRACE(name);
    EXPECT_NE(parseFailure(std::string{"return ("} + name + "(1, 2))")
                  .find("VQL_IO_QUERY_CONTEXT"),
              std::string::npos);
    EXPECT_NE(parseFailure(std::string{"def "} + name + "() := 1; 1")
                  .find("VQL_IO_QUERY_CONTEXT"),
              std::string::npos);
  }
}

// ---- Real files
// ---------------------------------------------------------------

/// A runner over a real file host whose write grant is the directory `root/`
/// of a fresh tree; `outside/` is not granted.
struct FileWorld {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  VQLEngine engine{core};
  fs::path base;
  fs::path root;
  fs::path outside;
  std::unique_ptr<xanadu::vortex::FileReadHost> host;
  std::unique_ptr<ScriptRunner> runner;

  explicit FileWorld(
      const bool grant = true,
      const std::size_t writeBytes =
          xanadu::vortex::FileReadHostOptions::defaultWriteBytes) {
    static std::uint32_t counter = 0;
    base = fs::weakly_canonical(fs::temp_directory_path()) /
           ("vql_io_write_" + std::to_string(::getpid()) + "_" +
            std::to_string(counter++));
    root    = base / "root";
    outside = base / "outside";
    fs::create_directories(root);
    fs::create_directories(outside);
    xanadu::vortex::FileReadHostOptions options;
    options.writeBytes = writeBytes;
    if (grant) options.writeGrant = xanadu::vortex::writeRootGrant({root});
    host   = std::make_unique<xanadu::vortex::FileReadHost>(vm, arena, options);
    runner = std::make_unique<ScriptRunner>(
        engine, vm, xanadu::vql::fileScriptPorts(*host));
  }
  ~FileWorld() {
    runner.reset();
    host.reset();
    std::error_code ec;
    fs::remove_all(base, ec);
  }
  auto put(const fs::path &path, const std::string &content) -> fs::path {
    std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
    return path;
  }
  auto start(const std::string &source) -> ScriptRunResult {
    return runner->run(Parser(source).parseQuery());
  }
  auto run(const std::string &source)
      -> std::expected<std::vector<zigzag::CellRef>, FrontendDiagnostic> {
    return xanadu::vql::driveToCompletion(
        *runner, start(source), [&] { host->pump(16); },
        [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
  }
  /// The rows of @p source as text; a failed script fails the test.
  auto rows(const std::string &source) -> std::vector<std::string> {
    const auto result = run(source);
    EXPECT_TRUE(result.has_value())
        << (result ? "" : result.error().code + ": " + result.error().message);
    std::vector<std::string> out;
    if (result)
      for (const auto cell : *result) out.push_back(arena.textOf(cell));
    return out;
  }
  auto failure(const std::string &source) -> std::string {
    const auto result = run(source);
    return result ? std::string{} : result.error().code;
  }
};

TEST(VqlIoWriteFileTest, CreateWriteSyncAndReadBackThroughOneCapability) {
  FileWorld f;
  const auto path = (f.root / "report.txt").string();
  const auto rows =
      f.rows(open(path, "for write create", "o") +
             act("o", "write_bytes(0, \"hello world\")", "w") +
             act("o", "sync()", "d") + act("o", "read_bytes(6, 5)", "r") +
             "return io_tag($o), io_tag($w), io_bytes($w), io_tag($d), "
             "io_tag($r), io_payload($r)");
  EXPECT_EQ(rows, (std::vector<std::string>{"Capability", "Complete", "11",
                                            "Complete", "Complete", "world"}));
  EXPECT_EQ(slurp(path), "hello world");
}

TEST(VqlIoWriteFileTest, APositionalWriteOverwritesInPlace) {
  FileWorld f;
  const auto path = f.put(f.root / "grid.txt", "aaaaaaaa").string();
  const auto rows = f.rows(open(path, "for write", "o") +
                           act("o", "write_bytes(2, \"XY\")", "w") +
                           "return io_tag($w), io_bytes($w)");
  EXPECT_EQ(rows, (std::vector<std::string>{"Complete", "2"}));
  EXPECT_EQ(slurp(path), "aaXYaaaa");
}

TEST(VqlIoWriteFileTest, AppendsLandInAdmissionOrderAtTheEnd) {
  FileWorld f;
  const auto path = f.put(f.root / "log.txt", "start;").string();
  const auto rows = f.rows(open(path, "for append", "o") +
                           act("o", "append_bytes(\"one;\")", "a") +
                           act("o", "append_bytes(\"two;\")", "b") +
                           "return io_tag($a), io_bytes($b)");
  EXPECT_EQ(rows, (std::vector<std::string>{"Complete", "4"}));
  EXPECT_EQ(slurp(path), "start;one;two;");
}

TEST(VqlIoWriteFileTest, AppendWithCreateStartsAnEmptyFile) {
  FileWorld f;
  const auto path = (f.root / "fresh.log").string();
  static_cast<void>(f.rows(open(path, "for append create", "o") +
                           act("o", "append_bytes(\"first\")", "a") +
                           "return io_tag($a)"));
  EXPECT_EQ(slurp(path), "first");
}

TEST(VqlIoWriteFileTest, ASyncAfterDetachedWritesCompletesAfterBoth) {
  FileWorld f;
  const auto path = (f.root / "ordered.txt").string();
  const auto rows = f.rows(open(path, "for append create", "o") +
                           detached("o", "append_bytes(\"a\")", "h1") +
                           detached("o", "append_bytes(\"b\")", "h2") +
                           act("o", "sync()", "d") +
                           "effect { await($h1) } as $r1; "
                           "effect { await($h2) } as $r2; "
                           "return io_tag($d), io_tag($r1), io_tag($r2)");
  EXPECT_EQ(rows,
            (std::vector<std::string>{"Complete", "Complete", "Complete"}));
  EXPECT_EQ(slurp(path), "ab");
}

TEST(VqlIoWriteFileTest, ABinaryPayloadIsCopiedByteForByte) {
  FileWorld f;
  std::string every(1U << 10U, '\0');
  for (std::size_t i = 0; i < every.size(); ++i)
    every[i] = static_cast<char>(i % 256);
  every += std::string{"\xff\xfe\x00\x80", 4}; // not UTF-8, with a NUL
  const auto from = f.put(f.root / "src.bin", every).string();
  const auto to   = (f.root / "dst.bin").string();
  const auto size = std::to_string(every.size());
  const auto rows =
      f.rows(open(from, "", "src") + open(to, "for write create", "dst") +
             act("src", "read_bytes(0, " + size + ")", "r") +
             act("dst", "write_bytes(0, $r)", "w") +
             "return io_tag($r), io_tag($w), io_bytes($w)");
  EXPECT_EQ(rows, (std::vector<std::string>{"Complete", "Complete", size}));
  EXPECT_EQ(slurp(to), every) << "the copy is byte for byte";
}

TEST(VqlIoWriteFileTest, ACopyOfTheWholeWriteBoundIsAccepted) {
  FileWorld f;
  const auto bound =
      xanadu::vortex::FileReadHostOptions::defaultWriteBytes; // 1 MiB
  std::string every(bound, '\0');
  for (std::size_t i = 0; i < every.size(); ++i)
    every[i] = static_cast<char>((i * 7U) % 256);
  const auto from = f.put(f.root / "big.bin", every).string();
  const auto to   = (f.root / "big.copy").string();
  const auto size = std::to_string(bound);
  const auto rows =
      f.rows(open(from, "", "src") + open(to, "for write create", "dst") +
             act("src", "read_bytes(0, " + size + ")", "r") +
             act("dst", "write_bytes(0, $r)", "w") + "return io_bytes($w)");
  EXPECT_EQ(rows, (std::vector<std::string>{size}));
  EXPECT_EQ(slurp(to), every);
}

TEST(VqlIoWriteFileTest, ATextVariableIsTheBytesAndIsFrozenAtSubmit) {
  FileWorld f;
  const auto path = (f.root / "text.txt").string();
  f.engine.setVariable("line", f.arena.makeCell(std::string{"héllo\n"}));
  static_cast<void>(f.rows(open(path, "for write create", "o") +
                           act("o", "write_bytes(0, $line)", "w") +
                           "return io_tag($w)"));
  EXPECT_EQ(slurp(path), "héllo\n");
}

TEST(VqlIoWriteFileTest, AnEmptySourceIsAZeroByteWriteNotAnError) {
  FileWorld f;
  const auto path = (f.root / "empty.txt").string();
  const auto rows = f.rows(open(path, "for write create", "o") +
                           act("o", "write_bytes(0, \"\")", "w") +
                           "return io_tag($w), io_bytes($w)");
  EXPECT_EQ(rows, (std::vector<std::string>{"Complete", "0"}));
  EXPECT_EQ(slurp(path), "");
}

// ---- Authority and refusals, each a value
// -------------------------------------

TEST(VqlIoWriteFileTest, WithNoGrantEveryWriteOpenIsARejectedValue) {
  FileWorld f{/*grant=*/false};
  for (const auto *mode :
       {"for write", "for write create", "for append", "for append create"}) {
    SCOPED_TRACE(mode);
    const auto path = (f.root / "denied.txt").string();
    const auto rows = f.rows(open(path, mode, "o") +
                             "return io_tag($o), io_error($o), io_detail($o)");
    EXPECT_EQ(rows,
              (std::vector<std::string>{
                  "Rejected", "Unsupported",
                  std::to_string(xanadu::vortex::fileDetailNoWriteGrant)}));
    EXPECT_FALSE(fs::exists(path)) << "nothing was created";
  }
}

TEST(VqlIoWriteFileTest, APathOutsideTheRootsOrEscapingThemIsRejected) {
  FileWorld f;
  fs::create_symlink(f.outside, f.root / "link");
  const auto cases = std::vector<std::string>{
      (f.outside / "x.txt").string(),
      (f.root / ".." / "outside" / "x.txt").string(),
      (f.root / "link" / "x.txt").string(),
  };
  for (const auto &path : cases) {
    SCOPED_TRACE(path);
    const auto rows = f.rows(open(path, "for write create", "o") +
                             "return io_tag($o), io_detail($o)");
    EXPECT_EQ(rows,
              (std::vector<std::string>{
                  "Rejected",
                  std::to_string(xanadu::vortex::fileDetailNoWriteGrant)}));
  }
  EXPECT_FALSE(fs::exists(f.outside / "x.txt"));
}

TEST(VqlIoWriteFileTest, CreateOnAnExistingFileFailsAndNeverClobbers) {
  FileWorld f;
  const auto path = f.put(f.root / "there.txt", "keep").string();
  const auto rows = f.rows(open(path, "for write create", "o") +
                           "return io_tag($o), io_error($o), io_detail($o)");
  EXPECT_EQ(rows, (std::vector<std::string>{"Failed", "BackendFailure",
                                            std::to_string(EEXIST)}));
  EXPECT_EQ(slurp(path), "keep");
}

TEST(VqlIoWriteFileTest, AFileSymlinkIsJudgedByItsTarget) {
  FileWorld f;
  const auto target = f.put(f.root / "target.txt", "safe").string();
  fs::create_symlink(target, f.root / "alias.txt");
  const auto escaped = f.put(f.outside / "elsewhere.txt", "other");
  fs::create_symlink(escaped, f.root / "escape.txt");
  // The host resolves the path before the grant judges it, so an alias inside
  // the root opens its target, and one pointing out of the root is refused.
  EXPECT_EQ(f.rows(open((f.root / "alias.txt").string(), "for write", "o") +
                   act("o", "write_bytes(0, \"S\")", "w") +
                   "return io_tag($o), io_tag($w)"),
            (std::vector<std::string>{"Capability", "Complete"}));
  EXPECT_EQ(slurp(target), "Safe");
  EXPECT_EQ(f.rows(open((f.root / "escape.txt").string(), "for write", "p") +
                   "return io_tag($p)"),
            (std::vector<std::string>{"Rejected"}));
  EXPECT_EQ(slurp(escaped), "other");
}

TEST(VqlIoWriteFileTest, TheWrongDisciplineForACapabilityIsRejected) {
  FileWorld f;
  const auto path  = f.put(f.root / "mode.txt", "abc").string();
  const auto wrong = std::to_string(xanadu::vortex::fileDetailWrongMode);
  // positional write on an append capability
  EXPECT_EQ(f.rows(open(path, "for append", "o") +
                   act("o", "write_bytes(0, \"x\")", "w") +
                   "return io_tag($w), io_error($w), io_detail($w)"),
            (std::vector<std::string>{"Rejected", "Unsupported", wrong}));
  // append on a positional capability
  EXPECT_EQ(f.rows(open(path, "for write", "p") +
                   act("p", "append_bytes(\"x\")", "a") +
                   "return io_tag($a), io_detail($a)"),
            (std::vector<std::string>{"Rejected", wrong}));
  // any write or sync on a read-only capability
  EXPECT_EQ(
      f.rows(open(path, "", "q") + act("q", "write_bytes(0, \"x\")", "w") +
             act("q", "append_bytes(\"x\")", "a") + act("q", "sync()", "d") +
             "return io_tag($w), io_tag($a), io_tag($d), io_detail($w)"),
      (std::vector<std::string>{"Rejected", "Rejected", "Rejected", wrong}));
  EXPECT_EQ(slurp(path), "abc") << "nothing was written";
}

TEST(VqlIoWriteFileTest, AWriteOverTheHostBoundIsRejectedAndStagesNothing) {
  constexpr std::size_t bound = 4;
  FileWorld f{/*grant=*/true, bound};
  const auto path = (f.root / "bounded.txt").string();
  const auto rows = f.rows(open(path, "for write create", "o") +
                           act("o", "write_bytes(0, \"12345\")", "w") +
                           act("o", "append_bytes(\"12345\")", "a") +
                           act("o", "write_bytes(0, \"1234\")", "ok") +
                           "return io_tag($w), io_error($w), io_detail($w), "
                           "io_tag($a), io_tag($ok), io_bytes($ok)");
  EXPECT_EQ(rows, (std::vector<std::string>{
                      "Rejected", "BudgetExceeded",
                      std::to_string(xanadu::vortex::fileDetailWriteBound),
                      "Rejected", "Complete", "4"}));
  EXPECT_EQ(slurp(path), "1234");
}

TEST(VqlIoWriteFileTest, ASourceThatIsNotBytesIsADiagnosticBeforeAnySubmit) {
  FileWorld f;
  const auto path  = (f.root / "never.txt").string();
  const auto other = (f.root / "other.txt").string();
  // An unbound name would otherwise be written as the text "$nobody".
  EXPECT_EQ(f.failure(open(path, "for write create", "o") +
                      act("o", "write_bytes(0, $nobody)", "w")),
            "VQL_IO_EFFECT_ARGUMENT");
  // The capability itself is not a byte source.
  EXPECT_EQ(f.failure(open(path, "for write create", "o") +
                      act("o", "write_bytes(0, $o)", "w")),
            "VQL_IO_WRITE_SOURCE");
  // An outcome that retained no payload has nothing to write.
  EXPECT_EQ(f.failure(open(other, "for write create", "p") +
                      act("p", "write_bytes(0, \"x\")", "w") +
                      act("p", "write_bytes(0, $w)", "again")),
            "VQL_IO_WRITE_SOURCE");
  EXPECT_FALSE(fs::exists(path) && !slurp(path).empty());
  EXPECT_EQ(slurp(other), "x") << "only the valid write happened";
}

// ---- within, handles, cancel, owned records
// -----------------------------------

TEST(VqlIoWriteFileTest, WithinAndDetachedWritesUseTheSameMachinery) {
  FileWorld f;
  const auto path = (f.root / "within.txt").string();
  const auto rows =
      f.rows(open(path, "for write create", "o") +
             act("o", "write_bytes(0, \"abc\")", "w", "30 s") +
             detached("o", "write_bytes(3, \"def\")", "h") +
             "effect { await($h) } as $r; "
             "return io_tag($w), io_uncertain($w), io_tag($h), io_tag($r)");
  // `io_uncertain` names a field only Cancelled and TimedOut carry.
  EXPECT_EQ(rows,
            (std::vector<std::string>{"Complete", "Request", "Complete"}));
  EXPECT_EQ(slurp(path), "abcdef");
}

TEST(VqlIoWriteFileTest, CancelOfADetachedWriteIsAValueEitherWay) {
  FileWorld f;
  const auto path = (f.root / "cancel.txt").string();
  const auto rows = f.rows(open(path, "for write create", "o") +
                           detached("o", "write_bytes(0, \"abc\")", "h") +
                           "effect { cancel($h) } as $c; return io_tag($c)");
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_TRUE(rows[0] == "Cancelled" || rows[0] == "Complete") << rows[0];
}

TEST(VqlIoWriteFileTest, ANamedWriteOutcomeIsAnOwnedRecordAndFreesItsSlot) {
  FileWorld f;
  const auto path             = (f.root / "many.log").string();
  constexpr std::size_t named = 100; // the service holds 16 requests
  std::string source          = open(path, "for append create", "o");
  for (std::size_t i = 0; i < named; ++i)
    source += act("o", "append_bytes(\"x\")", "w" + std::to_string(i));
  source += "return io_tag($w0), io_bytes($w99), io_tag($w50)";
  const auto rows = f.rows(source);
  EXPECT_EQ(rows, (std::vector<std::string>{"Complete", "1", "Complete"}));
  EXPECT_EQ(slurp(path), std::string(named, 'x'));
  EXPECT_EQ(f.runner->ownedRecordCount(), named);
  // What is left is the open and its close, awaiting the next admission.
  EXPECT_LE(f.host->service().requestCount(), 2U);
}

TEST(VqlIoWriteFileTest, ARejectedWriteIsAlsoAnOwnedRecord) {
  FileWorld f{/*grant=*/true, 2};
  const auto path = (f.root / "rec.txt").string();
  const auto rows = f.rows(open(path, "for write create", "o") +
                           act("o", "write_bytes(0, \"toolong\")", "w") +
                           "return $w, io_error($w)");
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0], "Rejected");
  EXPECT_EQ(rows[1], "BudgetExceeded");
  EXPECT_LE(f.host->service().requestCount(), 2U);
}

// An `open` leaves cells behind per run whatever follows it (ten per run at
// C1b, found while building C2); the criterion here is that a write, an append
// and a sync add nothing beyond what a read of the same shape does.
TEST(VqlIoWriteFileTest, AWriteRunLeavesNoMoreBehindThanAReadRun) {
  FileWorld f;
  const auto path = (f.root / "bounded.log").string();
  static_cast<void>(
      f.rows(open(path, "for append create", "seed") + "return io_tag($seed)"));
  const auto perRun = [&](const std::string &script) {
    constexpr std::size_t warmRuns = 8;
    constexpr std::size_t runs     = 200;
    for (std::size_t i = 0; i < warmRuns; ++i) EXPECT_TRUE(f.run(script));
    const auto warm = f.arena.liveCells();
    for (std::size_t i = 0; i < runs; ++i) EXPECT_TRUE(f.run(script));
    return (f.arena.liveCells() - warm) / runs;
  };
  const auto read =
      perRun(open(path, "", "o") + act("o", "read_bytes(0, 1)", "w") +
             act("o", "read_bytes(0, 1)", "d") + "return io_tag($w)");
  const auto write = perRun(open(path, "for append", "o") +
                            act("o", "append_bytes(\"y\")", "w") +
                            act("o", "sync()", "d") + "return io_tag($w)");
  EXPECT_LE(write, read);
}

// ---- Fake backend: outcomes a file cannot be made to give
// ----------------------

constexpr IoProgress terminal{.transferredBytes = 0,
                              .phase            = IoPhase::Terminal};

struct FakeWriteWorld {
  struct Staged {
    std::string kind;
    std::uint64_t offset{};
    std::vector<std::byte> bytes;
  };
  World w;
  VQLEngine engine{w.core};
  std::vector<Staged> staged;
  std::vector<CellRef> specs;
  std::unique_ptr<ScriptRunner> runner;

  FakeWriteWorld() {
    ScriptIoPorts ports{w.bridge, w.host, w.payloads, w.diagnostics,
                        [this](CapabilityCell, std::uint64_t, std::uint64_t)
                            -> std::expected<CellRef, FrontendDiagnostic> {
                          return specs.emplace_back(w.specification());
                        }};
    ports.stageWrite = [this](CapabilityCell, std::uint64_t offset,
                              std::vector<std::byte> bytes)
        -> std::expected<CellRef, StageFailure> {
      staged.push_back({"write", offset, std::move(bytes)});
      return specs.emplace_back(w.specification());
    };
    ports.stageAppend = [this](CapabilityCell, std::vector<std::byte> bytes)
        -> std::expected<CellRef, StageFailure> {
      staged.push_back({"append", 0, std::move(bytes)});
      return specs.emplace_back(w.specification());
    };
    ports.stageSync =
        [this](CapabilityCell) -> std::expected<CellRef, StageFailure> {
      staged.push_back({"sync", 0, {}});
      return specs.emplace_back(w.specification());
    };
    runner = std::make_unique<ScriptRunner>(engine, w.vm, std::move(ports));
    engine.setVariable("cap", w.capability.cell);
  }
  auto finish(IoOutcome outcome) -> ScriptRunResult {
    w.complete(w.submissions() - 1, std::move(outcome));
    static_cast<void>(w.pump());
    auto woken = runner->takeWake();
    EXPECT_TRUE(woken.has_value());
    if (!woken) return ScriptRunResult::failed({"TEST_NO_WAKE", "none", {}});
    return runner->resume(*std::move(woken));
  }
  static auto rows(const ScriptRunResult &result)
      -> const std::vector<CellRef> & {
    static const std::vector<CellRef> none;
    const auto *done = std::get_if<ScriptRunResult::Completed>(&result.value);
    return done ? done->value : none;
  }
  auto text(const CellRef cell) -> std::string { return w.arena.textOf(cell); }
};

TEST(VqlIoWriteFakeTest, EveryTerminalOfAWriteIsAValueTheScriptReads) {
  struct Case {
    const char *name;
    IoOutcome outcome;
    std::vector<std::string> expected;
  };
  // io_uncertain, io_error and io_detail answer only where the outcome has
  // such a field; the rest answer nothing, never a default.
  const std::vector<Case> cases{
      {"Complete", Complete{{}, {}, terminal}, {"Complete"}},
      {"Failed",
       Failed{{IoErrorCode::BackendFailure, ENOSPC}, {}, terminal},
       {"Failed", "BackendFailure", std::to_string(ENOSPC)}},
      {"Cancelled", Cancelled{{}, terminal, true}, {"Cancelled", "true"}},
      {"CancelledClean",
       Cancelled{{}, terminal, false},
       {"Cancelled", "false"}},
      {"TimedOut", TimedOut{{}, terminal, true}, {"TimedOut", "true"}},
      {"Unknown", Unknown{terminal}, {"Unknown"}},
  };
  for (const auto &test : cases) {
    SCOPED_TRACE(test.name);
    FakeWriteWorld s;
    auto result = s.runner->run(
        Parser(act("cap", "write_bytes(0, \"abc\")", "w") +
               "return io_tag($w), io_uncertain($w), io_error($w), "
               "io_detail($w)")
            .parseQuery());
    ASSERT_NE(std::get_if<ScriptRunResult::Parked>(&result.value), nullptr);
    result = s.finish(test.outcome);
    std::vector<std::string> texts;
    for (const auto cell : FakeWriteWorld::rows(result))
      texts.push_back(s.text(cell));
    EXPECT_EQ(texts, test.expected);
  }
}

TEST(VqlIoWriteFakeTest, TheFrozenBytesReachTheBackendOnceAcrossAPark) {
  FakeWriteWorld s;
  s.engine.setVariable("payload", s.w.arena.makeCell(std::string{"abc"}));
  auto result = s.runner->run(
      Parser(act("cap", "write_bytes(7, $payload)", "w") + "return io_tag($w)")
          .parseQuery());
  ASSERT_NE(std::get_if<ScriptRunResult::Parked>(&result.value), nullptr);
  ASSERT_EQ(s.staged.size(), 1U);
  EXPECT_EQ(s.staged[0].kind, "write");
  EXPECT_EQ(s.staged[0].offset, 7U);
  EXPECT_EQ(
      s.staged[0].bytes,
      (std::vector<std::byte>{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}}));
  result = s.finish(Complete{{}, {}, terminal});
  EXPECT_EQ(s.staged.size(), 1U) << "a resume never submits again";
  ASSERT_EQ(FakeWriteWorld::rows(result).size(), 1U);
}

TEST(VqlIoWriteFakeTest, AnOutcomePayloadIsTheSourceBytes) {
  FakeWriteWorld s;
  auto result = s.runner->run(Parser(act("cap", "read_bytes(0, 16)", "r") +
                                     act("cap", "append_bytes($r)", "a") +
                                     "return io_tag($a)")
                                  .parseQuery());
  ASSERT_NE(std::get_if<ScriptRunResult::Parked>(&result.value), nullptr);
  result = s.finish(Complete{s.w.payloadRange(), {}, terminal});
  ASSERT_NE(std::get_if<ScriptRunResult::Parked>(&result.value), nullptr);
  ASSERT_EQ(s.staged.size(), 1U);
  EXPECT_EQ(s.staged[0].kind, "append");
  EXPECT_EQ(s.staged[0].bytes,
            (std::vector<std::byte>{std::byte{'o'}, std::byte{'k'}}));
  result = s.finish(Complete{{}, {}, terminal});
  EXPECT_EQ(s.text(FakeWriteWorld::rows(result).front()), "Complete");
}

TEST(VqlIoWriteFakeTest, ASyncStagesOnceAndANonPayloadOutcomeIsNotASource) {
  FakeWriteWorld s;
  auto result = s.runner->run(
      Parser(act("cap", "sync()", "d") + act("cap", "write_bytes(0, $d)", "w"))
          .parseQuery());
  ASSERT_NE(std::get_if<ScriptRunResult::Parked>(&result.value), nullptr);
  ASSERT_EQ(s.staged.size(), 1U);
  EXPECT_EQ(s.staged[0].kind, "sync");
  result             = s.finish(Complete{{}, {}, terminal});
  const auto *failed = std::get_if<ScriptRunResult::Failed>(&result.value);
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->diagnostic.code, "VQL_IO_WRITE_SOURCE");
  EXPECT_EQ(s.staged.size(), 1U) << "the second write never staged";
}

TEST(VqlIoWriteFakeTest, AHostThatStagesNoWritesFailsTheScriptByName) {
  World w;
  VQLEngine engine{w.core};
  ScriptRunner runner(
      engine, w.vm,
      ScriptIoPorts{w.bridge, w.host, w.payloads, w.diagnostics,
                    [&](CapabilityCell, std::uint64_t, std::uint64_t)
                        -> std::expected<CellRef, FrontendDiagnostic> {
                      return w.specification();
                    }});
  engine.setVariable("cap", w.capability.cell);
  const auto result =
      runner.run(Parser(act("cap", "append_bytes(\"x\")", "a")).parseQuery());
  const auto *failed = std::get_if<ScriptRunResult::Failed>(&result.value);
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->diagnostic.code, "VQL_IO_EFFECT_ARGUMENT");
}

} // namespace
