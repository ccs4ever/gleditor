#include <gtest/gtest.h>

#include <tuple>

#include "common/xanadu/vortex/vortex_stdlib.hpp"

namespace {
using namespace zigzag::vortex;

TEST(VagentEpistemicExports, SymbolsAreRealExecutableNumericGraphs) {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  VortexStdLib library{core, vm};
  for (const auto &[name, kind, input, expected] :
       {std::tuple{"std:math/exp", OpcodeKind::Exp, 0.0, 1.0},
        std::tuple{"std:math/ln", OpcodeKind::Ln, 1.0, 0.0},
        std::tuple{"std:math/sqrt", OpcodeKind::Sqrt, 9.0, 3.0}}) {
    const auto entry = library.resolve(name);
    ASSERT_TRUE(entry) << name;
    EXPECT_EQ(vm.getOpcodeKind(*entry), kind);
    const auto args    = library.routineInputs(*entry);
    const auto outputs = library.routineOutputs(*entry);
    ASSERT_EQ(args.size(), 1U);
    ASSERT_EQ(outputs.size(), 1U);
    ASSERT_TRUE(core.writeValue(args.front(), 0, std::nullopt, input));
    const auto result = vm.run(vm.spawnCursor(*entry));
    ASSERT_TRUE(result.success) << result.errorMessage;
    const auto value = core.render(outputs.front());
    ASSERT_TRUE(value);
    ASSERT_TRUE(std::holds_alternative<double>(*value));
    EXPECT_DOUBLE_EQ(std::get<double>(*value), expected);
    // A host invocation may not reuse a previous argument on an arity error.
    EXPECT_TRUE(library.call(name, {}).empty());
    EXPECT_TRUE(library.call(name, {input, input}).empty());
  }
}
} // namespace
