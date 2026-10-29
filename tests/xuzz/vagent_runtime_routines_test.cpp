/**
 * @file vagent_runtime_routines_test.cpp
 * @brief Runtime routines the compiler emits only for a unit that calls them.
 */
#include <gtest/gtest.h>

#include <string>

#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"

namespace {

using namespace xanadu::vagent::runtime;

// Record equality calls the equality routine, which run() emits on first use;
// before that it compiled to a call of a label that was never placed.
TEST(VagentRuntimeRoutines, RecordEqualityRunsTheEqualityRoutine) {
  auto program = CompiledProgram::fromSource(R"vagent(
record Pt { x: Int, y: Int }
event Ping { }
event Result { same: Int, differ: Int }
agent A {
  var same: Int := 0
  var differ: Int := 0
  on Ping {
    if Pt{x: 1, y: 2} == Pt{x: 1, y: 2} then same := 1
    if Pt{x: 1, y: 2} == Pt{x: 1, y: 3} then differ := 1
    emit Result{same: same, differ: differ} in outer
  }
}
)vagent");
  ASSERT_TRUE(program) << program.error();
  LocalHost host(std::move(*program));
  const auto pid = host.launch("A", "a");
  ASSERT_TRUE(pid);
  host.pump();
  Envelope ping;
  ping.kind = std::string(kKindEvent);
  ping.type = "Ping";
  ASSERT_TRUE(host.send(*pid, ping));
  host.pump();
  ASSERT_EQ(host.emissions().size(), 1U);
  const auto &result = host.emissions().back().fields;
  EXPECT_EQ(*result.find("same"), WireValue::integer(1));
  EXPECT_EQ(*result.find("differ"), WireValue::integer(0));
}

} // namespace
