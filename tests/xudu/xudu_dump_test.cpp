/**
 * @file xudu_dump_test.cpp
 * @brief The tool that reads a store without the loader's help.
 *
 * R11 lets a format stop being human-readable on the condition that a tool can
 * show it. The condition is not met by a tool that renders a *healthy* store:
 * the case it exists for is one the loader refuses, which is why the second
 * test here is the one that matters. See design R11 and migration step 9.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <xudu/core/compact_op.hpp>
#include <xudu/core/segmented_ops_spool.hpp>
#include <xudu/core/store.hpp>

namespace {

namespace fs = std::filesystem;
using xudu::CompactOpNode;
using xudu::MicroversionId;
using xudu::Store;

struct Run {
  int exitCode{-1};
  std::string output;
};

Run runDump(const std::string &args) {
  const std::string cmd = "./build/xudu-dump " + args + " 2>&1";
  Run result;
  FILE *const pipe = popen(cmd.c_str(), "r");
  if (nullptr == pipe) {
    return result;
  }
  std::array<char, 512> buffer{};
  while (nullptr != fgets(buffer.data(), buffer.size(), pipe)) {
    result.output += buffer.data();
  }
  const int status = pclose(pipe);
  result.exitCode  = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return result;
}

fs::path scratch(const std::string &name) {
  const auto dir = fs::temp_directory_path() / ("xudu_dump_" + name);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

/// A store with a branch in it, saved.
fs::path savedStore(const fs::path &dir) {
  Store store;
  const auto one = store.insert(MicroversionId{}, 0, "hello");
  const auto two = store.insert(one, 5, " world");
  static_cast<void>(store.erase(two, 0, 1));
  static_cast<void>(store.insert(one, 5, " there"));
  store.save(dir.string());
  return dir;
}

class XuduDumpTest : public ::testing::Test {
protected:
  void SetUp() override {
    if (!fs::exists("./build/xudu-dump")) {
      GTEST_SKIP() << "xudu-dump not built; run make -j$(nproc) xudu-dump";
    }
  }
};

TEST_F(XuduDumpTest, aHealthyStoreRendersEveryOperationAndWhatItSays) {
  const auto dir = savedStore(scratch("healthy"));

  const auto run = runDump(dir.string());
  EXPECT_EQ(run.exitCode, 0) << run.output;

  // The header, including the field whose silent change caused all of this.
  EXPECT_THAT(run.output, testing::HasSubstr("signature=ok"));
  EXPECT_THAT(
      run.output,
      testing::HasSubstr("nodeSize=" + std::to_string(sizeof(CompactOpNode))));

  // One line per operation, each named the way the store names it -- the
  // branch included, whose name is derived from the tree rather than stored.
  EXPECT_THAT(run.output, testing::HasSubstr("op 1  produces=1 "));
  EXPECT_THAT(run.output, testing::HasSubstr("op 4  produces=1a1 "));

  // And what the operation's span actually says, which is the point of
  // rendering an operation rather than hexdumping it: a layout change that
  // shifted a field puts garbage here instead, on the line it happened.
  EXPECT_THAT(run.output, testing::HasSubstr(R"(text="hello")"));
  EXPECT_THAT(run.output, testing::HasSubstr(R"(text=" world")"));
  EXPECT_THAT(run.output, testing::HasSubstr(R"(text=" there")"));
}

TEST_F(XuduDumpTest, aStoreTheLoaderRefusesIsStillReadable) {
  // The requirement R11 actually imposes. A store from before ops.nodes had a
  // header does not open -- Store::load throws OpsSegmentUnreadable -- and if
  // that were also the end of being able to look at it, the format would have
  // stopped being debuggable rather than stopped being plaintext.
  const auto dir = savedStore(scratch("refused"));

  // Put the operations back the way a store written before headers had them:
  // a bare run of nodes, no header.
  {
    std::ifstream in(dir / "ops.nodes", std::ios::binary);
    const std::string held{std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>()};
    in.close();
    ASSERT_GT(held.size(), xudu::opsSegmentHeaderBytes);
    std::ofstream out(dir / "ops.nodes", std::ios::binary | std::ios::trunc);
    out.write(held.data() + xudu::opsSegmentHeaderBytes,
              static_cast<std::streamsize>(held.size() -
                                           xudu::opsSegmentHeaderBytes));
  }

  // The loader refuses it, which is step 8 working.
  {
    Store reopened;
    EXPECT_THROW(reopened.load(dir.string()), xudu::OpsSegmentUnreadable);
  }

  // The tool does not.
  const auto run = runDump(dir.string());
  EXPECT_EQ(run.exitCode, 1)
      << "a dump with something wrong in it must say so" << run.output;
  EXPECT_THAT(run.output, testing::HasSubstr("no operations segment signature"))
      << "the dump has to say why, not merely decline";
  EXPECT_THAT(run.output, testing::HasSubstr("header    absent"));

  // Everything it *can* read is still rendered, which is what makes it worth
  // running on a broken store at all.
  EXPECT_THAT(run.output, testing::HasSubstr("primedia  bytes="));
  // Everything typed, the branch's text included: a permascroll is append-only
  // and holds what was written rather than what any one version reads.
  EXPECT_THAT(run.output, testing::HasSubstr(R"(head="hello world there")"));
}

TEST_F(XuduDumpTest, sectionsAreAddressableSoAFormatChangeCanBeDiffed) {
  // Migration steps 10 and 11 change how a store is written without changing
  // what it says, so --section=ops has to be the part that stays identical
  // across them and --section=header the part where a version bump shows.
  const auto dir = savedStore(scratch("sections"));

  const auto ops = runDump("--section=ops " + dir.string());
  EXPECT_EQ(ops.exitCode, 0) << ops.output;
  EXPECT_THAT(ops.output, testing::HasSubstr("op 1  produces=1 "));
  EXPECT_THAT(ops.output, testing::Not(testing::HasSubstr("header ")));
  EXPECT_THAT(ops.output, testing::Not(testing::HasSubstr("primedia ")));

  const auto header = runDump("--section=header " + dir.string());
  EXPECT_EQ(header.exitCode, 0) << header.output;
  EXPECT_THAT(header.output, testing::HasSubstr("signature=ok"));
  EXPECT_THAT(header.output, testing::Not(testing::HasSubstr("op 1")));

  // Same store, same bytes out: a baseline nothing can diff against is no
  // baseline.
  EXPECT_EQ(runDump("--section=ops " + dir.string()).output, ops.output);

  EXPECT_EQ(runDump("--section=nonsense " + dir.string()).exitCode, 2);
}

TEST_F(XuduDumpTest, aBareSegmentFileCanBePointedAtDirectly) {
  // The common shape of the problem: one file is suspect and the store around
  // it is beside the point.
  const auto dir = savedStore(scratch("barefile"));
  const auto run = runDump((dir / "ops.nodes").string());
  EXPECT_EQ(run.exitCode, 0) << run.output;
  EXPECT_THAT(run.output, testing::HasSubstr("signature=ok"));
  EXPECT_THAT(run.output, testing::HasSubstr("op 1  produces=1 "));
  // No store around it, so no primedia to quote from and no text= to render.
  EXPECT_THAT(run.output, testing::Not(testing::HasSubstr("text=")));
}

} // namespace
