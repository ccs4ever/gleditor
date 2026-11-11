#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    static std::atomic_uint64_t sequence{};
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned int attempt = 0; attempt < 100; ++attempt) {
      path_ = fs::temp_directory_path() /
              ("gleditor-stdlib-gen-" + std::to_string(stamp) + "-" +
               std::to_string(sequence.fetch_add(1)));
      std::error_code error;
      if (fs::create_directory(path_, error)) return;
      if (error && error != std::errc::file_exists) {
        throw fs::filesystem_error("create temporary test directory", path_,
                                   error);
      }
    }
    throw std::runtime_error("could not allocate a unique temporary directory");
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }

  [[nodiscard]] const fs::path &path() const noexcept { return path_; }

private:
  fs::path path_;
};

struct ProcessResult {
  int exitCode{-1};
  std::string output;
};

std::string shellQuote(const std::string_view value) {
  std::string quoted{"'"};
  for (const char c : value) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(c);
    }
  }
  quoted.push_back('\'');
  return quoted;
}

ProcessResult runGenerator(const std::vector<std::string> &arguments) {
  const fs::path binary = fs::current_path() / "build" / "vortex-stdlib-gen";
  std::string command   = shellQuote(binary.string());
  for (const auto &argument : arguments) {
    command += " ";
    command += shellQuote(argument);
  }
  command += " 2>&1";
  FILE *pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) return {};
  ProcessResult result;
  char buffer[512]{};
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    result.output += buffer;
  }
  const int status = pclose(pipe);
  result.exitCode  = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return result;
}

std::string readBytes(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void expectArtifactLayout(const fs::path &directory) {
  EXPECT_TRUE(fs::is_regular_file(directory / "store" / "ops.nodes"));
  EXPECT_TRUE(fs::is_regular_file(directory / "store" / "store.tables"));
  EXPECT_TRUE(
      fs::is_regular_file(directory / "permascroll" / "active.primedia"));
  EXPECT_FALSE(fs::exists(directory / "publication-state"));
}

TEST(VortexStdLibGeneratorTest, fullImageIsDeterministicAndVerifiable) {
  if (!fs::exists("./build/vortex-stdlib-gen")) {
    GTEST_SKIP() << "build vortex-stdlib-gen first";
  }
  TemporaryDirectory temporary;
  const auto first  = temporary.path() / "first";
  const auto second = temporary.path() / "second";
  const auto generatedFirst =
      runGenerator({"--out", first.string(), "--stdlib"});
  ASSERT_EQ(generatedFirst.exitCode, 0) << generatedFirst.output;
  const auto generatedSecond =
      runGenerator({"--out", second.string(), "--stdlib"});
  ASSERT_EQ(generatedSecond.exitCode, 0) << generatedSecond.output;
  expectArtifactLayout(first);
  expectArtifactLayout(second);

  for (const auto &relative : {"store/ops.nodes", "store/store.tables",
                               "permascroll/active.primedia"}) {
    EXPECT_EQ(readBytes(first / relative), readBytes(second / relative))
        << relative << " differed between identical generator runs";
  }
  const auto verified = runGenerator({"--verify", first.string()});
  EXPECT_EQ(verified.exitCode, 0) << verified.output;
  EXPECT_THAT(verified.output, ::testing::HasSubstr("stdlib image OK"));
}

TEST(VortexStdLibGeneratorTest, selectedModuleImageContainsOnlyThatModule) {
  if (!fs::exists("./build/vortex-stdlib-gen")) {
    GTEST_SKIP() << "build vortex-stdlib-gen first";
  }
  TemporaryDirectory temporary;
  const auto output = temporary.path() / "math";
  const auto generated =
      runGenerator({"--out", output.string(), "--module", "std:math"});
  ASSERT_EQ(generated.exitCode, 0) << generated.output;
  const auto verified = runGenerator({"--verify", output.string()});
  ASSERT_EQ(verified.exitCode, 0) << verified.output;
  EXPECT_THAT(verified.output, ::testing::HasSubstr("modules=std:math"));
  EXPECT_THAT(verified.output,
              ::testing::Not(::testing::HasSubstr("std:string")));
}

TEST(VortexStdLibGeneratorTest, invalidSelectionsNeverPublishAnImage) {
  if (!fs::exists("./build/vortex-stdlib-gen")) {
    GTEST_SKIP() << "build vortex-stdlib-gen first";
  }
  TemporaryDirectory temporary;
  const auto invalidArguments = temporary.path() / "invalid-arguments";
  const auto badArguments     = runGenerator(
      {"--out", invalidArguments.string(), "--stdlib", "--surprise"});
  EXPECT_NE(badArguments.exitCode, 0);
  EXPECT_THAT(badArguments.output, ::testing::HasSubstr("unknown argument"));
  EXPECT_FALSE(fs::exists(invalidArguments));

  const auto missingDependency = temporary.path() / "missing-dependency";
  const auto badDependency     = runGenerator(
      {"--out", missingDependency.string(), "--module", "std:goal"});
  EXPECT_NE(badDependency.exitCode, 0);
  EXPECT_THAT(badDependency.output, ::testing::HasSubstr("std:goal"));
  EXPECT_FALSE(fs::exists(missingDependency));
}

TEST(VortexStdLibGeneratorTest, populatedOutputIsRefusedWithoutChanges) {
  if (!fs::exists("./build/vortex-stdlib-gen")) {
    GTEST_SKIP() << "build vortex-stdlib-gen first";
  }
  TemporaryDirectory temporary;
  const auto output = temporary.path() / "occupied";
  ASSERT_TRUE(fs::create_directory(output));
  const auto sentinel = output / "preserve.txt";
  {
    std::ofstream contents(sentinel, std::ios::binary);
    contents << "concurrent user data";
  }
  const auto result = runGenerator({"--out", output.string(), "--stdlib"});
  EXPECT_NE(result.exitCode, 0);
  EXPECT_THAT(result.output, ::testing::HasSubstr("not empty"));
  EXPECT_EQ(readBytes(sentinel), "concurrent user data");
  EXPECT_EQ(
      std::distance(fs::directory_iterator(output), fs::directory_iterator{}),
      1);
}

} // namespace
