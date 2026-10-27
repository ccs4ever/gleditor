/**
 * @file vagent_timer_test.cpp
 * @brief `Schedules`, `Clock` and `Logging` (design §10) driven end to end
 *        through `CompiledProgram` and `LocalHost` with a manual clock.
 */
#include <gtest/gtest.h>

#include <spdlog/sinks/ostream_sink.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <gleditor/logging.hpp>

#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/wire.hpp"

namespace {

using namespace xanadu::vagent::runtime;
using namespace std::chrono_literals;

constexpr const char *kTimers = R"vagent(
event Arm { }
event Disarm { which: Timer }
event Start { }
event StartAndCancel { }
event Stamp { }
event Say { secret: Str }
event Ping { n: Int }
event Beat { }
event Seen { at: Time }

agent Once {
  var pings: Int := 0
  on Arm {
    Schedules.after(5s, Ping{n: 1})
    Schedules.after(5s, Ping{n: 2})
  }
  on Ping as p { pings := pings + p.n }
}

agent Ticker {
  var pings: Int := 0
  var beats: Int := 0
  var stamp: Time := Clock.now()

  on Start { Schedules.periodic(1s, Beat{}) }
  on StartAndCancel {
    let t := Schedules.periodic(1s, Beat{})
    Schedules.cancel(t)
  }
  on Arm { Schedules.after(5s, Ping{n: 1}) }
  on Disarm as d { Schedules.cancel(d.which) }
  on Stamp { stamp := Clock.now() }
  on Ping as p { pings := pings + p.n }
  on Beat {
    beats := beats + 1
    emit Seen{at: Clock.now()} in outer
  }
  on Say as s {
    Logging.info("said hello")
    Logging.debug("said quietly")
  }
}
)vagent";

class VagentTimer : public testing::Test {
protected:
  std::unique_ptr<LocalHost> host;
  std::optional<xanadu::identity::ActorPid> pid;
  std::int64_t startMillis{0};

  void SetUp() override {
    auto program = CompiledProgram::fromSource(kTimers);
    ASSERT_TRUE(program) << program.error();
    LocalHost::Options options;
    options.manualClock = true;
    // A round starting instant near the present: the runtime refuses
    // messages and grants stamped far from real time.
    const auto base = std::chrono::floor<std::chrono::seconds>(
        std::chrono::system_clock::now());
    startMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
                      base.time_since_epoch())
                      .count();
    options.clock.wall = [base] { return base; };
    host = std::make_unique<LocalHost>(std::move(*program), options);
  }

  void start(std::string_view agent) {
    const auto launched = host->launch(agent, "t");
    ASSERT_TRUE(launched);
    pid = *launched;
    host->pump();
  }

  void send(std::string type, WireRecord fields = {}) {
    Envelope e;
    e.kind   = "event";
    e.type   = std::move(type);
    e.fields = std::move(fields);
    ASSERT_TRUE(host->send(*pid, std::move(e)));
    host->pump();
  }

  std::optional<WireValue> field(std::string_view name) {
    auto *ctx        = host->executor().context(*pid);
    const auto agent = host->executor().agentCell(*pid);
    if (!ctx || !agent) return std::nullopt;
    const auto vd   = valueDims(ctx->core);
    const auto cell = fieldValue(ctx->arena, vd, *agent, name);
    if (!cell) return std::nullopt;
    auto value = readValue(ctx->arena, vd, *cell);
    return value ? std::optional<WireValue>(std::move(*value)) : std::nullopt;
  }

  std::int64_t intField(std::string_view name) {
    const auto value = field(name);
    const auto *i = value ? std::get_if<std::int64_t>(&value->data) : nullptr;
    EXPECT_NE(i, nullptr) << name;
    return i == nullptr ? -1 : *i;
  }

  std::int64_t firstTimerId() {
    const auto value = field(kFieldTimers);
    const auto *list = value ? std::get_if<WireList>(&value->data) : nullptr;
    if (list == nullptr || list->empty()) return -1;
    const auto *first = std::get_if<WireRecord>(&list->front().data);
    const auto *id    = first ? first->find(kTimerId) : nullptr;
    const auto *n     = id ? std::get_if<std::int64_t>(&id->data) : nullptr;
    return n == nullptr ? -1 : *n;
  }

  void disarm(std::int64_t id) {
    WireRecord fields;
    fields.set("which", WireValue::integer(id));
    send("Disarm", std::move(fields));
  }

  std::size_t timerCount() {
    const auto value = field(kFieldTimers);
    const auto *list = value ? std::get_if<WireList>(&value->data) : nullptr;
    EXPECT_NE(list, nullptr);
    return list == nullptr ? 0 : list->size();
  }
};

TEST_F(VagentTimer, AfterFiresOnceAtTheRightTime) {
  start("Once");
  send("Arm");
  EXPECT_EQ(timerCount(), 2U);
  host->advance(4999ms);
  EXPECT_EQ(intField("pings"), 0);
  host->advance(1ms);
  // Both timers were due at the same instant: 1 + 2.
  EXPECT_EQ(intField("pings"), 3);
  EXPECT_EQ(timerCount(), 0U);
  host->advance(60s);
  EXPECT_EQ(intField("pings"), 3);
}

TEST_F(VagentTimer, TimersGetDistinctIds) {
  start("Once");
  send("Arm");
  const auto value = field(kFieldTimers);
  const auto *list = value ? std::get_if<WireList>(&value->data) : nullptr;
  ASSERT_NE(list, nullptr);
  ASSERT_EQ(list->size(), 2U);
  const auto id = [&](std::size_t i) {
    return *std::get_if<WireRecord>(&(*list)[i].data)->find(kTimerId);
  };
  EXPECT_NE(id(0), id(1));
}

TEST_F(VagentTimer, PeriodicFiresOncePerPeriod) {
  start("Ticker");
  send("Start");
  EXPECT_EQ(timerCount(), 1U);
  for (int n = 1; n <= 4; ++n) {
    host->advance(1s);
    EXPECT_EQ(intField("beats"), n);
  }
  EXPECT_EQ(timerCount(), 1U);
}

TEST_F(VagentTimer, PeriodicCatchesUpMissedPeriods) {
  start("Ticker");
  send("Start");
  host->advance(3s);
  EXPECT_EQ(intField("beats"), 3);
  EXPECT_EQ(host->emissions().size(), 3U);
}

TEST_F(VagentTimer, CancelStopsAPeriodicTimer) {
  start("Ticker");
  send("Start");
  host->advance(2s);
  EXPECT_EQ(intField("beats"), 2);
  disarm(firstTimerId());
  EXPECT_EQ(timerCount(), 0U);
  host->advance(10s);
  EXPECT_EQ(intField("beats"), 2);
}

TEST_F(VagentTimer, CancelLeavesOtherTimersAlone) {
  start("Ticker");
  send("Start");
  const auto periodic = firstTimerId();
  send("Arm");
  EXPECT_EQ(timerCount(), 2U);
  disarm(periodic);
  EXPECT_EQ(timerCount(), 1U);
  host->advance(5s);
  EXPECT_EQ(intField("pings"), 1);
  EXPECT_EQ(intField("beats"), 0);
}

TEST_F(VagentTimer, ACancelInTheSameTurnLeavesNothingToFire) {
  start("Ticker");
  send("StartAndCancel");
  EXPECT_EQ(timerCount(), 0U);
  host->advance(5s);
  EXPECT_EQ(intField("beats"), 0);
}

TEST_F(VagentTimer, ClockNowIsTheTurnTime) {
  start("Ticker");
  EXPECT_EQ(intField("stamp"), startMillis);
  host->advance(2500ms);
  send("Stamp");
  EXPECT_EQ(intField("stamp"), startMillis + 2500);
}

TEST_F(VagentTimer, ATimerEventSeesTheTimeItFiredAt) {
  start("Ticker");
  send("Start");
  host->advance(1s);
  ASSERT_EQ(host->emissions().size(), 1U);
  const auto *at = host->emissions()[0].fields.find("at");
  ASSERT_NE(at, nullptr);
  EXPECT_EQ(*at, WireValue::integer(startMillis + 1000));
}

TEST_F(VagentTimer, LoggingReachesTheCategoryAndNeverTheEventPayload) {
  std::ostringstream out;
  auto sink   = std::make_shared<spdlog::sinks::ostream_sink_mt>(out);
  auto logger = gleditor::logging::category("vortex.agent");
  logger->sinks().push_back(sink);
  const auto previous = logger->level();
  logger->set_level(spdlog::level::debug);

  WireRecord fields;
  fields.set("secret", WireValue::text("hunter2-payload"));
  start("Ticker");
  send("Say", std::move(fields));

  logger->set_level(previous);
  std::erase(logger->sinks(), sink);
  const auto text = out.str();
  EXPECT_NE(text.find("t: said hello"), std::string::npos) << text;
  EXPECT_NE(text.find("t: said quietly"), std::string::npos) << text;
  EXPECT_EQ(text.find("hunter2"), std::string::npos) << text;
}

TEST_F(VagentTimer, DebugIsDroppedWhileTheCategoryIsAtInfo) {
  std::ostringstream out;
  auto sink   = std::make_shared<spdlog::sinks::ostream_sink_mt>(out);
  auto logger = gleditor::logging::category("vortex.agent");
  logger->sinks().push_back(sink);
  const auto previous = logger->level();
  logger->set_level(spdlog::level::info);

  start("Ticker");
  send("Say", {});

  logger->set_level(previous);
  std::erase(logger->sinks(), sink);
  EXPECT_NE(out.str().find("said hello"), std::string::npos);
  EXPECT_EQ(out.str().find("said quietly"), std::string::npos);
}

// -- vagent run --advance
// ------------------------------------------------------

TEST(VagentTimerCli, AdvanceDrivesAPeriodicTimerFromTheCommandLine) {
  if (!std::filesystem::exists("build/vagent"))
    GTEST_SKIP() << "no build/vagent";
  const auto path = std::filesystem::temp_directory_path() / "timer_cli.vagent";
  std::ofstream(path) << R"vagent(
event Beat { }
event Seen { n: Int }
agent Metronome {
  var n: Int := 0
  on Initialize { Schedules.periodic(1s, Beat{}) }
  on Beat {
    n := n + 1
    emit Seen{n: n} in outer
  }
}
)vagent";
  const std::string cmd = "build/vagent run " + path.string() +
                          " --agent Metronome --advance 1s --advance 2s 2>&1";
  FILE *pipe = popen(cmd.c_str(), "r");
  ASSERT_NE(pipe, nullptr);
  std::string output;
  char buffer[256];
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) output += buffer;
  EXPECT_EQ(pclose(pipe), 0) << output;
  EXPECT_NE(output.find("Seen {n: 1}"), std::string::npos) << output;
  EXPECT_NE(output.find("Seen {n: 3}"), std::string::npos) << output;
}

} // namespace
