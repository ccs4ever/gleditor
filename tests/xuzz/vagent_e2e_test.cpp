/**
 * @file vagent_e2e_test.cpp
 * @brief Compiled vagent programs driven through `CompiledProgram` and the
 *        executor adapter in-process (design §12.1 and its pieces), plus the
 *        value-layout tests that keep `()`, the empty string and an empty
 *        record apart in an arena and on the wire.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "common/xanadu/vagent/program.hpp"
#include "common/xanadu/vagent/wire.hpp"

namespace {

using namespace xanadu::vagent;
using zigzag::vortex::WorkerContext;

// -- Value layout ------------------------------------------------------------

TEST(VagentValueLayout, UnitEmptyStrAndEmptyRecordsStayApart) {
  WorkerContext ctx;
  const auto vd = valueDims(ctx.core);
  WireRecord empty;
  WireRecord typed;
  typed.type = "Marker";
  WireRecord withField;
  withField.type = "Pair";
  withField.set("a", WireValue::integer(1));
  const std::vector<WireValue> values{
      WireValue::unit(),         WireValue::text(""),
      WireValue::text("Marker"), WireValue::record(empty),
      WireValue::record(typed),  WireValue::record(withField),
      WireValue::list({}),       WireValue::list({WireValue::unit()}),
  };
  for (const auto &value : values) {
    const auto cell = writeValue(ctx.arena, vd, value);
    const auto back = readValue(ctx.arena, vd, cell);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->data.index(), value.data.index());
    EXPECT_EQ(*back, value);
    // The wire form must tell them apart as well as the arena does.
    const auto bytes = encodeValue(value);
    ASSERT_TRUE(bytes);
    EXPECT_EQ(decodeValue(*bytes), value);
  }
}

TEST(VagentValueLayout, FieldsOfSkipsTheRecordMarker) {
  WorkerContext ctx;
  const auto vd = valueDims(ctx.core);
  WireRecord record;
  record.type = "Pair";
  record.set("a", WireValue::integer(1));
  record.set("b", WireValue::text("x"));
  const auto cell   = writeValue(ctx.arena, vd, WireValue::record(record));
  const auto fields = fieldsOf(ctx.arena, vd, cell);
  ASSERT_EQ(fields.size(), 2U);
  EXPECT_EQ(ctx.arena.textOf(fields[0].first), "a");
  EXPECT_EQ(ctx.arena.textOf(fields[1].first), "b");
}

// -- Program -----------------------------------------------------------------

constexpr const char *kCounter = R"vagent(
event Bump { amount: Int }
event Report { total: Int }

agent Counter(start: Int) {
  var total: Int := 0
  on Initialize { total := start }
  on Bump as b {
    total := total + b.amount
    emit Report{total: total} in outer
  }
}
)vagent";

TEST(VagentProgram, ProgramRefIsStableAndTracksTheSource) {
  const auto a = CompiledProgram::fromSource(kCounter);
  const auto b = CompiledProgram::fromSource(kCounter);
  const auto c = CompiledProgram::fromSource(std::string(kCounter) + "\n");
  ASSERT_TRUE(a && b && c);
  EXPECT_EQ(a->programRef(), b->programRef());
  EXPECT_NE(a->programRef(), c->programRef());
  EXPECT_EQ(a->programRef().size(), 64U);
}

TEST(VagentProgram, ProgramRefusesBadSourceAndUnknownAgents) {
  const auto bad = CompiledProgram::fromSource("agent {", "bad.vagent");
  ASSERT_FALSE(bad);
  EXPECT_NE(bad.error().find("bad.vagent:"), std::string::npos) << bad.error();

  auto program = CompiledProgram::fromSource(kCounter);
  ASSERT_TRUE(program);
  WorkerContext ctx;
  const auto missing = program->instantiate(ctx, "Nope");
  ASSERT_FALSE(missing);
  EXPECT_NE(missing.error().find("Nope"), std::string::npos);

  LocalHost host(std::move(*program));
  EXPECT_FALSE(host.launch("Nope", "x"));
}

class VagentE2E : public testing::Test {
protected:
  std::unique_ptr<LocalHost> host;

  void load(const char *source) {
    auto program = CompiledProgram::fromSource(source);
    ASSERT_TRUE(program) << program.error();
    host = std::make_unique<LocalHost>(std::move(*program));
  }

  static Envelope event(std::string type,
                        std::vector<std::pair<std::string, WireValue>> fields) {
    Envelope e;
    e.kind = "event";
    e.type = std::move(type);
    for (auto &[name, value] : fields) e.fields.set(name, std::move(value));
    return e;
  }

  std::optional<WireValue> field(const xanadu::identity::ActorPid &pid,
                                 std::string_view name) {
    auto *ctx        = host->executor().context(pid);
    const auto agent = host->executor().agentCell(pid);
    if (!ctx || !agent) return std::nullopt;
    const auto vd   = valueDims(ctx->core);
    const auto cell = fieldValue(ctx->arena, vd, *agent, name);
    if (!cell) return std::nullopt;
    auto value = readValue(ctx->arena, vd, *cell);
    return value ? std::optional<WireValue>(std::move(*value)) : std::nullopt;
  }

  std::int64_t intField(const xanadu::identity::ActorPid &pid, std::string_view name) {
    const auto value = field(pid, name);
    const auto *i = value ? std::get_if<std::int64_t>(&value->data) : nullptr;
    EXPECT_NE(i, nullptr) << name;
    return i == nullptr ? -1 : *i;
  }
};

TEST_F(VagentE2E, FieldsHandlerAndEmitToTheLauncher) {
  load(kCounter);
  WireRecord args;
  args.set("start", WireValue::integer(10));
  const auto pid = host->launch("Counter", "c", std::move(args));
  ASSERT_TRUE(pid);
  host->pump();
  EXPECT_EQ(intField(*pid, "total"), 10);
  EXPECT_TRUE(host->emissions().empty());

  ASSERT_TRUE(host->send(*pid, event("Bump", {{"amount", WireValue::integer(5)}})));
  host->pump();
  ASSERT_TRUE(host->send(*pid, event("Bump", {{"amount", WireValue::integer(2)}})));
  host->pump();
  EXPECT_EQ(intField(*pid, "total"), 17);

  ASSERT_EQ(host->emissions().size(), 2U);
  EXPECT_EQ(host->emissions()[0].type, "Report");
  EXPECT_EQ(host->emissions()[0].from, "c");
  const auto *first = host->emissions()[0].fields.find("total");
  const auto *last  = host->emissions()[1].fields.find("total");
  ASSERT_TRUE(first && last);
  EXPECT_EQ(*first, WireValue::integer(15));
  EXPECT_EQ(*last, WireValue::integer(17));
}

// Section 12.1 with the beliefs replaced by fields: beliefs are a later task.
constexpr const char *kScatterGatherFields = R"vagent(
event SearchQuery { query_id: Str, pattern: Str }
event SubTask { query_id: Str, partition: Int, pattern: Str }
event PartitionResult { query_id: Str, partition: Int, weight: Int }
event SearchCompleted { query_id: Str, total: Int }

agent Worker {
  on SubTask as t {
    emit PartitionResult{query_id: t.query_id, partition: t.partition,
                         weight: (t.partition + 1) * 10} for t.source
  }
}

agent SearchCoordinator(partitions: Int) {
  var workers: [Agent] := []
  var received: Int := 0
  var total: Int := 0

  on Initialize {
    for _ in 1 to partitions do workers := workers ++ [spawn Worker() in inner]
  }

  on SearchQuery as q [q.space == outer] {
    for i in 0 to partitions - 1 do
      emit SubTask{query_id: q.query_id, partition: i, pattern: q.pattern} in inner for workers[i]
  }

  on PartitionResult as r [r.space == inner] {
    received := received + 1
    total := total + r.weight
    if received == partitions then {
      emit SearchCompleted{query_id: r.query_id, total: total} in outer
    }
  }
}
)vagent";

TEST_F(VagentE2E, CoordinatorScattersToWorkersAndGathersTheReplies) {
  load(kScatterGatherFields);
  constexpr std::int64_t kWorkers = 3;
  WireRecord args;
  args.set("partitions", WireValue::integer(kWorkers));
  const auto pid = host->launch("SearchCoordinator", "coord", std::move(args));
  ASSERT_TRUE(pid);
  host->pump();
  EXPECT_EQ(host->runtime().actorCount(), 1U + kWorkers);

  ASSERT_TRUE(host->send(
      *pid, event("SearchQuery", {{"query_id", WireValue::text("q1")},
                                  {"pattern", WireValue::text("x")}})));
  host->pump();

  ASSERT_EQ(host->emissions().size(), 1U);
  const auto &done = host->emissions().front();
  EXPECT_EQ(done.type, "SearchCompleted");
  ASSERT_TRUE(done.fields.find("query_id"));
  EXPECT_EQ(*done.fields.find("query_id"), WireValue::text("q1"));
  // Weights 10 + 20 + 30: every worker answered once.
  ASSERT_TRUE(done.fields.find("total"));
  EXPECT_EQ(*done.fields.find("total"), WireValue::integer(60));
  EXPECT_EQ(intField(*pid, "received"), kWorkers);
}

constexpr const char *kFailing = R"vagent(
event Good { }
event Bad { }

agent Fragile {
  var good: Int := 0
  var bad: Int := 0
  on Good { good := good + 1 }
  on Bad {
    bad := bad + 1
    fail
  }
}
)vagent";

TEST_F(VagentE2E, AFailingHandlerUndoesOnlyItsOwnTurn) {
  load(kFailing);
  const auto pid = host->launch("Fragile", "f");
  ASSERT_TRUE(pid);
  host->pump();
  ASSERT_TRUE(host->send(*pid, event("Good", {})));
  host->pump();
  ASSERT_TRUE(host->send(*pid, event("Bad", {})));
  host->pump();
  EXPECT_EQ(intField(*pid, "good"), 1);
  EXPECT_EQ(intField(*pid, "bad"), 0);
  // The agent lives on and keeps serving.
  ASSERT_TRUE(host->send(*pid, event("Good", {})));
  host->pump();
  EXPECT_EQ(intField(*pid, "good"), 2);
  EXPECT_EQ(host->executor().stat(*pid, kStatDropped), 1U);
}

// The literal program of section 12.1. It needs beliefs (`believe`, `forget`,
// `findall`, `not believes`), which task K lands; enable this test at merge.
constexpr const char *kSection12_1 = R"vagent(
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

TEST_F(VagentE2E, DISABLED_LiteralSection12_1) {
  load(kSection12_1);
  constexpr std::int64_t kWorkers = 2;
  WireRecord args;
  args.set("partitions", WireValue::integer(kWorkers));
  const auto pid = host->launch("SearchCoordinator", "coord", std::move(args));
  ASSERT_TRUE(pid);
  host->pump();
  ASSERT_TRUE(host->send(
      *pid, event("SearchQuery", {{"query_id", WireValue::text("q1")},
                                  {"pattern", WireValue::text("x")}})));
  host->pump();
  ASSERT_EQ(host->emissions().size(), 1U);
  const auto *hits = host->emissions().front().fields.find("hits");
  ASSERT_NE(hits, nullptr);
  const auto *list = std::get_if<WireList>(&hits->data);
  ASSERT_NE(list, nullptr);
  EXPECT_EQ(list->size(), 2U * kWorkers);
}

} // namespace
