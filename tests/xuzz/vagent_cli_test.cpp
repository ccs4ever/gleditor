/**
 * @file vagent_cli_test.cpp
 * @brief Runs the built `vagent` program: exit codes and diagnostic format.
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

struct ProcessResult {
  int exitCode{0};
  std::string output;
};

ProcessResult runProcess(const std::string &cmd) {
  const std::string fullCmd = cmd + " 2>&1";
  FILE *pipe                = popen(fullCmd.c_str(), "r");
  if (pipe == nullptr) {
    return {-1, "Failed to popen: " + cmd};
  }
  char buffer[512];
  std::string output;
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    output += buffer;
  }
  const int status = pclose(pipe);
  return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, output};
}

std::string vagentBin() {
  return fs::exists("build/vagent") ? "build/vagent" : "";
}

fs::path writeSource(const std::string &name, const std::string &text) {
  const fs::path path = fs::temp_directory_path() / name;
  std::ofstream(path) << text;
  return path;
}

// Design section 12.1, the same text the checker suite holds clean.
constexpr const char *scatterGather = R"vagent(
module demo:search
import std:agent

predicate pending(query_id: Str)
predicate reported(query_id: Str, partition: Int)
predicate hit(query_id: Str, text: Str)

event SearchQuery { query_id: Str, pattern: Str }
event SubTask { query_id: Str, partition: Int, pattern: Str }
event PartitionResult { query_id: Str, partition: Int, hits: [Str] }
event SearchCompleted { query_id: Str, hits: [Str] }

agent Worker {
  on SubTask as t {
    emit PartitionResult{query_id: t.query_id, partition: t.partition,
                         hits: scan_partition(t.partition, t.pattern)} for t.source
  }
  def scan_partition(index: Int, pattern: Str): [Str] {
    ["alpha-" ++ text(index), "beta-" ++ text(index)]
  }
}

agent SearchCoordinator(partitions: Int) {
  var workers: [Agent] := []

  on Initialize {
    for _ in 1 to partitions do workers := workers ++ [spawn Worker() in inner]
  }

  on SearchQuery as q [q.space == outer] {
    believe pending(q.query_id)
    for i in 0 to partitions - 1 do
      emit SubTask{query_id: q.query_id, partition: i, pattern: q.pattern} in inner for workers[i]
  }

  // A duplicate delivery of one partition fails the guard and changes nothing.
  on PartitionResult as r [r.space == inner & not believes reported(r.query_id, r.partition)] {
    believe reported(r.query_id, r.partition)
    for h in r.hits do believe hit(r.query_id, h)
    if *findall(?p, reported(r.query_id, ?p)) == partitions then {
      emit SearchCompleted{query_id: r.query_id, hits: findall(?h, hit(r.query_id, ?h))} in outer
      forget pending(r.query_id)
    }
  }
}
)vagent";

constexpr const char *undeclared = R"vagent(
event Ping { n: Int }

agent A {
  on Ping as p {
    let x := missing_name
  }
}
)vagent";

#define REQUIRE_VAGENT(bin)                                                    \
  const std::string bin = vagentBin();                                         \
  if ((bin).empty()) {                                                         \
    GTEST_SKIP() << "build/vagent not found; run `make vagent`";               \
  }

TEST(VagentCli, ValidProgramExitsZero) {
  REQUIRE_VAGENT(bin);
  const auto path = writeSource("vagent_cli_valid.vagent", scatterGather);
  const auto res  = runProcess(bin + " check " + path.string());
  EXPECT_EQ(res.exitCode, 0) << res.output;
  fs::remove(path);
}

TEST(VagentCli, UndeclaredNameExitsOneWithPositionAndCode) {
  REQUIRE_VAGENT(bin);
  const auto path = writeSource("vagent_cli_bad.vagent", undeclared);
  const auto res  = runProcess(bin + " check " + path.string());
  EXPECT_EQ(res.exitCode, 1) << res.output;
  EXPECT_NE(res.output.find(path.string() + ":6:"), std::string::npos)
      << res.output;
  EXPECT_NE(res.output.find("UnknownName"), std::string::npos) << res.output;
  fs::remove(path);
}

TEST(VagentCli, MissingFileExitsTwo) {
  REQUIRE_VAGENT(bin);
  const auto res = runProcess(bin + " check /nonexistent/none.vagent");
  EXPECT_EQ(res.exitCode, 2) << res.output;
}

TEST(VagentCli, RunIsReservedAndExitsTwo) {
  REQUIRE_VAGENT(bin);
  const auto res = runProcess(bin + " run x.vagent --agent A");
  EXPECT_EQ(res.exitCode, 2) << res.output;
  EXPECT_NE(res.output.find("not built yet"), std::string::npos);
}

TEST(VagentCli, HelpListsBothCommands) {
  REQUIRE_VAGENT(bin);
  const auto res = runProcess(bin + " --help");
  EXPECT_EQ(res.exitCode, 0);
  EXPECT_NE(res.output.find("check"), std::string::npos);
  EXPECT_NE(res.output.find("run"), std::string::npos);
}

} // namespace
