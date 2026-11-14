/**
 * @file vpl_lower_misc_test.cpp
 * @brief Card V13: named refusals for scrub, compress, rank, key, and quad.
 */
#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vpl/compiler.hpp"
#include "common/xanadu/vpl/execution.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vpl {
namespace {

constexpr std::size_t kBudget = 200000;

class VplMiscRefusalsTest : public ::testing::Test {
protected:
  void SetUp() override {
    core     = std::make_unique<zigzag::vortex::VortexCore>(arena);
    vm       = std::make_unique<zigzag::vortex::VortexVM>(*core);
    executor = std::make_unique<Executor>(*core, *vm);
  }

  void expectCompileRefusal(const std::string &source,
                            const std::string &text) {
    const auto result = executor->execute(source, kBudget);
    ASSERT_FALSE(result.has_value()) << source;
    EXPECT_EQ(result.error().stage, ExecutionError::Stage::Compile) << source;
    EXPECT_NE(result.error().message.find(text), std::string::npos)
        << source << ": " << result.error().message;
  }

  zigzag::ArenaManifold arena;
  std::unique_ptr<zigzag::vortex::VortexCore> core;
  std::unique_ptr<zigzag::vortex::VortexVM> vm;
  std::unique_ptr<Executor> executor;
};

TEST_F(VplMiscRefusalsTest, ScrubRefusesByName) {
  expectCompileRefusal("⍫ 1 2 3", "scrub (⍫) is unsupported");
  expectCompileRefusal("scrub. 1 2 3", "scrub (⍫) is unsupported");
  expectCompileRefusal("1 ⍫ 2", "scrub (⍫) is unsupported");
}

TEST_F(VplMiscRefusalsTest, CompressRefusesByName) {
  expectCompileRefusal("(1 0 1) ⌿ (10 20 30)", "compress (⌿) is unsupported");
  expectCompileRefusal("(1 0 1) copy. (10 20 30)",
                       "compress (⌿) is unsupported");
}

TEST_F(VplMiscRefusalsTest, ConjunctionsRefuseByName) {
  expectCompileRefusal("(1 2) +⍤1 (3 4)",
                       "rank conjunction (⍤) is unsupported");
  expectCompileRefusal("(1 2) +\"1 (3 4)",
                       "rank conjunction (⍤) is unsupported");
  expectCompileRefusal("(1 2 1) ⌸ (10 20 30)",
                       "key conjunction (⌸) is unsupported");
  expectCompileRefusal("(1 2 1) key. (10 20 30)",
                       "key conjunction (⌸) is unsupported");
}

TEST_F(VplMiscRefusalsTest, QuadRefusesByName) {
  expectCompileRefusal("⎕ 42", "quad IO is unsupported");
  expectCompileRefusal("⎕READ 'chapter.xanadoc'", "⎕READ is unsupported");
  expectCompileRefusal("⎕SPLIT ',' 'a,b,c'", "⎕SPLIT is unsupported");
}

} // namespace
} // namespace xanadu::vpl
