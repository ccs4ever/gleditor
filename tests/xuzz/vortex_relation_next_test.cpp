/**
 * @file vortex_relation_next_test.cpp
 * @brief RelationNext: the read-only accessor over a registered relation
 *        provider -- success, exhaustion, retry chaining and each explicit
 *        failure.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag::vortex {
namespace {

using Tuple = std::pair<std::string, std::string>;

struct Harness {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  std::vector<CellRef> chain;

  // The relation edge/2 over a fixed list. An argument with empty text is a
  // wildcard; a bound one filters. The limit is fixed on the first call.
  std::vector<Tuple> tuples{{"a", "b"}, {"b", "c"}, {"b", "d"}};
  std::vector<std::pair<std::int64_t, std::int64_t>> seen;

  void registerEdge() {
    vm.setRelationProvider(
        "edge", [this](const RelationQuery &query) -> RelationResult {
          seen.emplace_back(query.position, query.limit);
          if (query.arguments.size() != 2)
            return std::unexpected("edge/2 expects two arguments");
          const auto limit = query.limit == 0
                                 ? static_cast<std::int64_t>(tuples.size())
                                 : query.limit;
          const auto wants = [&](const std::size_t i, const std::string &v) {
            const auto text = query.arena.textOf(query.arguments[i]);
            return text.empty() || text == v;
          };
          for (auto at = query.position; at < limit; ++at) {
            const auto &[from, to] = tuples[static_cast<std::size_t>(at)];
            if (!wants(0, from) || !wants(1, to)) continue;
            return std::optional<RelationAnswer>{
                RelationAnswer{.nextPosition = at + 1,
                               .limit        = limit,
                               .answers      = {query.arena.makeCell(from),
                                                query.arena.makeCell(to)}}};
          }
          return std::optional<RelationAnswer>{};
        });
  }

  struct Call {
    CellRef op;
    CellRef found, position, limit;
    std::vector<CellRef> answers;
  };

  // Appends a RelationNext to the straight-line program.
  Call emit(const std::string &relation, const CellRef position,
            const CellRef limit, const std::vector<CellRef> &arguments) {
    Call call;
    call.op = vm.mintOpcode(OpcodeKind::RelationNext);
    core.bindInput(call.op, arena.makeCell(relation));
    core.bindInput(call.op, position);
    core.bindInput(call.op, limit);
    for (const auto argument : arguments) core.bindInput(call.op, argument);
    call.found    = arena.makeCell("found");
    call.position = arena.makeCell("next position");
    call.limit    = arena.makeCell("next limit");
    core.bindOutput(call.op, call.found);
    core.bindOutput(call.op, call.position);
    core.bindOutput(call.op, call.limit);
    for (std::size_t i = 0; i < arguments.size(); ++i) {
      call.answers.push_back(arena.makeCell("answer"));
      core.bindOutput(call.op, call.answers.back());
    }
    if (!chain.empty())
      EXPECT_TRUE(
          arena.link(chain.back(), core.dims().spin, DimVector::POS, call.op));
    chain.push_back(call.op);
    return call;
  }

  ExecutionResult run() {
    const auto halt = vm.mintOpcode(OpcodeKind::Halt);
    EXPECT_TRUE(
        arena.link(chain.back(), core.dims().spin, DimVector::POS, halt));
    return vm.run(vm.spawnCursor(chain.front()), 100);
  }

  CellRef integer(const std::int64_t v) { return arena.makeScalarCell(v); }
  CellRef wildcard() { return arena.makeCell(); }

  // The text of the cell an answer slot denotes.
  std::string answerText(const CellRef slot) {
    const auto value = core.render(slot);
    const auto *const cell =
        value ? std::get_if<std::int64_t>(&*value) : nullptr;
    if (cell == nullptr) return "<no int>";
    return arena.contains(static_cast<CellRef>(*cell))
               ? arena.textOf(static_cast<CellRef>(*cell))
               : "<absent>";
  }
};

TEST(VortexRelationNextTest, ReturnsTheFirstMatchAndAdvancesThePosition) {
  Harness h;
  h.registerEdge();
  const auto call =
      h.emit("edge", h.integer(0), h.integer(0), {h.wildcard(), h.wildcard()});
  const auto executed = h.run();
  ASSERT_TRUE(executed.success) << executed.errorMessage;
  EXPECT_EQ(h.core.render(call.found), CellValue{true});
  EXPECT_EQ(h.core.render(call.position), CellValue{std::int64_t{1}});
  EXPECT_EQ(h.core.render(call.limit), CellValue{std::int64_t{3}});
  EXPECT_EQ(h.answerText(call.answers[0]), "a");
  EXPECT_EQ(h.answerText(call.answers[1]), "b");
}

TEST(VortexRelationNextTest, RetryChainWalksEveryMatchThenExhausts) {
  Harness h;
  h.registerEdge();
  auto call =
      h.emit("edge", h.integer(0), h.integer(0), {h.wildcard(), h.wildcard()});
  std::vector<Harness::Call> calls{call};
  for (int i = 0; i < 3; ++i) {
    call =
        h.emit("edge", call.position, call.limit, {h.wildcard(), h.wildcard()});
    calls.push_back(call);
  }
  const auto executed = h.run();
  ASSERT_TRUE(executed.success) << executed.errorMessage;
  EXPECT_EQ(h.core.render(calls[0].found), CellValue{true});
  EXPECT_EQ(h.core.render(calls[1].found), CellValue{true});
  EXPECT_EQ(h.core.render(calls[2].found), CellValue{true});
  EXPECT_EQ(h.answerText(calls[2].answers[1]), "d");
  // Exhaustion: not found, the position and bound are unchanged, and the
  // answer slots denote no cell.
  EXPECT_EQ(h.core.render(calls[3].found), CellValue{false});
  EXPECT_EQ(h.core.render(calls[3].position), CellValue{std::int64_t{3}});
  EXPECT_EQ(h.core.render(calls[3].limit), CellValue{std::int64_t{3}});
  EXPECT_EQ(h.answerText(calls[3].answers[0]), "<absent>");
  // The bound fixed by the first call came back to the provider unchanged.
  ASSERT_EQ(h.seen.size(), 4u);
  EXPECT_EQ(h.seen[0], (std::pair<std::int64_t, std::int64_t>{0, 0}));
  EXPECT_EQ(h.seen[1], (std::pair<std::int64_t, std::int64_t>{1, 3}));
  EXPECT_EQ(h.seen[3], (std::pair<std::int64_t, std::int64_t>{3, 3}));
}

TEST(VortexRelationNextTest, BoundArgumentsFilterAndNoMatchIsExhaustion) {
  Harness h;
  h.registerEdge();
  const auto hit      = h.emit("edge", h.integer(0), h.integer(0),
                               {h.arena.makeCell("b"), h.wildcard()});
  const auto miss     = h.emit("edge", h.integer(0), h.integer(0),
                               {h.arena.makeCell("z"), h.wildcard()});
  const auto executed = h.run();
  ASSERT_TRUE(executed.success) << executed.errorMessage;
  EXPECT_EQ(h.core.render(hit.found), CellValue{true});
  EXPECT_EQ(h.answerText(hit.answers[1]), "c");
  EXPECT_EQ(h.core.render(miss.found), CellValue{false});
}

TEST(VortexRelationNextTest, AnswerSlotsFeedUnifyAsTheAnswerCells) {
  Harness h;
  h.registerEdge();
  const auto call =
      h.emit("edge", h.integer(0), h.integer(0), {h.wildcard(), h.wildcard()});
  const auto ok    = h.arena.makeCell("unified");
  const auto unify = h.vm.mintOpcode(OpcodeKind::Unify);
  h.core.bindInput(unify, call.answers[0]);
  h.core.bindInput(unify, h.arena.makeCell("a"));
  h.core.bindOutput(unify, ok);
  ASSERT_TRUE(h.arena.link(call.op, h.core.dims().spin, DimVector::POS, unify));
  h.chain.push_back(unify);
  const auto executed = h.run();
  ASSERT_TRUE(executed.success) << executed.errorMessage;
  EXPECT_EQ(h.core.render(ok), CellValue{true});
}

TEST(VortexRelationNextTest, FailsExplicitlyWithNoProviderRegistered) {
  Harness h;
  h.emit("edge", h.integer(0), h.integer(0), {h.wildcard(), h.wildcard()});
  const auto executed = h.run();
  EXPECT_FALSE(executed.success);
  EXPECT_NE(executed.errorMessage.find("no relation provider"),
            std::string::npos);
  EXPECT_NE(executed.errorMessage.find("'edge'"), std::string::npos);
}

TEST(VortexRelationNextTest, AnEmptyProviderUnregisters) {
  Harness h;
  h.registerEdge();
  h.vm.setRelationProvider("edge", {});
  h.emit("edge", h.integer(0), h.integer(0), {h.wildcard(), h.wildcard()});
  EXPECT_FALSE(h.run().success);
}

TEST(VortexRelationNextTest, ProviderErrorsFailTheStepWithTheirMessage) {
  Harness h;
  h.registerEdge();
  h.emit("edge", h.integer(0), h.integer(0), {h.wildcard()});
  const auto executed = h.run();
  EXPECT_FALSE(executed.success);
  EXPECT_NE(executed.errorMessage.find("edge/2 expects two arguments"),
            std::string::npos);
}

TEST(VortexRelationNextTest, AProviderReturningTheWrongAnswerCountFails) {
  Harness h;
  h.vm.setRelationProvider(
      "short", [](const RelationQuery &query) -> RelationResult {
        return std::optional<RelationAnswer>{
            RelationAnswer{.answers = {query.arena.makeCell("only one")}}};
      });
  h.emit("short", h.integer(0), h.integer(0), {h.wildcard(), h.wildcard()});
  const auto executed = h.run();
  EXPECT_FALSE(executed.success);
  EXPECT_NE(executed.errorMessage.find("returned 1 answers for 2 arguments"),
            std::string::npos);
}

TEST(VortexRelationNextTest, AProviderReturningAForeignCellFails) {
  Harness h;
  h.vm.setRelationProvider("bad", [](const RelationQuery &) -> RelationResult {
    return std::optional<RelationAnswer>{RelationAnswer{.answers = {noCell}}};
  });
  h.emit("bad", h.integer(0), h.integer(0), {h.wildcard()});
  const auto executed = h.run();
  EXPECT_FALSE(executed.success);
  EXPECT_NE(executed.errorMessage.find("a cell the arena does not hold"),
            std::string::npos);
}

TEST(VortexRelationNextTest, RefusesAPositionThatIsNotAnInt64) {
  Harness h;
  h.registerEdge();
  h.emit("edge", h.arena.makeCell("zero"), h.integer(0),
         {h.wildcard(), h.wildcard()});
  const auto executed = h.run();
  EXPECT_FALSE(executed.success);
  EXPECT_NE(executed.errorMessage.find("position and limit must be Int64"),
            std::string::npos);
}

TEST(VortexRelationNextTest, RefusesAnOutputCountThatDoesNotMatchTheArity) {
  Harness h;
  h.registerEdge();
  const auto op = h.vm.mintOpcode(OpcodeKind::RelationNext);
  h.core.bindInput(op, h.arena.makeCell("edge"));
  h.core.bindInput(op, h.integer(0));
  h.core.bindInput(op, h.integer(0));
  h.core.bindInput(op, h.wildcard());
  h.core.bindOutput(op, h.arena.makeCell("found"));
  h.core.bindOutput(op, h.arena.makeCell("position"));
  h.core.bindOutput(op, h.arena.makeCell("limit"));
  const auto result = h.vm.step(h.vm.spawnCursor(op));
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("one answer slot per argument"),
            std::string::npos);
}

TEST(VortexRelationNextTest, RefusesTooFewInputs) {
  Harness h;
  const auto op = h.vm.mintOpcode(OpcodeKind::RelationNext);
  h.core.bindInput(op, h.arena.makeCell("edge"));
  const auto result = h.vm.step(h.vm.spawnCursor(op));
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("RelationNext expects"),
            std::string::npos);
}

TEST(VortexRelationNextTest, IsNeverMemoized) {
  Harness h;
  h.registerEdge();
  const auto call =
      h.emit("edge", h.integer(0), h.integer(0), {h.wildcard(), h.wildcard()});
  h.vm.enableMemoization(call.op, "relation-next");
  const auto executed = h.run();
  EXPECT_FALSE(executed.success);
  EXPECT_NE(executed.errorMessage.find("cannot be memoized"),
            std::string::npos);
}

TEST(VortexRelationNextTest, CannotRunAsACondition) {
  Harness h;
  h.registerEdge();
  const auto condition = h.vm.mintOpcode(OpcodeKind::RelationNext);
  h.core.bindInput(condition, h.arena.makeCell("edge"));
  h.core.bindInput(condition, h.integer(0));
  h.core.bindInput(condition, h.integer(0));
  const auto guarded = h.vm.mintOpcode(OpcodeKind::Nop);
  h.core.attachPrecondition(guarded, condition);
  const auto result = h.vm.step(h.vm.spawnCursor(guarded));
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("cannot run as conditions"),
            std::string::npos);
}

TEST(VortexRelationNextTest, ExhaustionClearsAReferenceFromAnEarlierHit) {
  Harness h;
  h.registerEdge();
  const auto first =
      h.emit("edge", h.integer(0), h.integer(0), {h.wildcard(), h.wildcard()});
  // A second lookup past the end, writing the same output slots.
  const auto second = h.vm.mintOpcode(OpcodeKind::RelationNext);
  h.core.bindInput(second, h.arena.makeCell("edge"));
  h.core.bindInput(second, h.integer(3));
  h.core.bindInput(second, h.integer(3));
  h.core.bindInput(second, h.wildcard());
  h.core.bindInput(second, h.wildcard());
  for (const auto slot : {first.found, first.position, first.limit})
    h.core.bindOutput(second, slot);
  for (const auto slot : first.answers) h.core.bindOutput(second, slot);
  ASSERT_TRUE(
      h.arena.link(first.op, h.core.dims().spin, DimVector::POS, second));
  h.chain.push_back(second);
  // Deref of a slot that denotes no cell reports absence.
  const auto derefed = h.arena.makeCell("deref");
  const auto deref   = h.vm.mintOpcode(OpcodeKind::Deref);
  h.core.bindInput(deref, first.answers[0]);
  h.core.bindOutput(deref, derefed);
  ASSERT_TRUE(h.arena.link(second, h.core.dims().spin, DimVector::POS, deref));
  h.chain.push_back(deref);
  const auto executed = h.run();
  ASSERT_TRUE(executed.success) << executed.errorMessage;
  EXPECT_EQ(h.core.render(first.found), CellValue{false});
  EXPECT_EQ(h.answerText(first.answers[0]), "<absent>");
  EXPECT_EQ(h.core.render(derefed),
            CellValue{static_cast<std::int64_t>(noCell)});
}

TEST(VortexRelationNextTest, AHitUnderAChoiceIsUndoneByFail) {
  Harness h;
  h.registerEdge();
  const auto choice = h.vm.mintOpcode(OpcodeKind::Choice);
  const auto alt    = h.vm.mintOpcode(OpcodeKind::Halt);
  h.core.bindInput(choice,
                   h.arena.makeScalarCell(static_cast<std::int64_t>(alt)));
  h.chain.push_back(choice);
  const auto call =
      h.emit("edge", h.integer(0), h.integer(0), {h.wildcard(), h.wildcard()});
  const auto fail = h.vm.mintOpcode(OpcodeKind::Fail);
  ASSERT_TRUE(h.arena.link(call.op, h.core.dims().spin, DimVector::POS, fail));
  const auto cursor   = h.vm.spawnCursor(choice);
  const auto cells    = h.arena.cellCount();
  const auto executed = h.vm.run(cursor, 20);
  ASSERT_TRUE(executed.success) << executed.errorMessage;
  EXPECT_EQ(h.arena.cellCount(), cells);
  // The answer slot is back to a plain cell: Deref names the slot itself, not
  // a cell the rolled-back provider minted.
  const auto derefed = h.arena.makeCell("deref");
  const auto deref   = h.vm.mintOpcode(OpcodeKind::Deref);
  h.core.bindInput(deref, call.answers[0]);
  h.core.bindOutput(deref, derefed);
  ASSERT_TRUE(h.vm.step(h.vm.spawnCursor(deref, "after")).success);
  EXPECT_EQ(h.core.render(derefed),
            CellValue{static_cast<std::int64_t>(call.answers[0])});
}

TEST(VortexRelationNextTest, AFailedPublishKeepsTheEarlierReferences) {
  Harness h;
  h.registerEdge();
  const auto first =
      h.emit("edge", h.integer(0), h.integer(0), {h.wildcard(), h.wildcard()});
  // The found output is a registered opcode cell, so the first write is
  // refused after the provider has already answered.
  const auto victim = h.vm.mintOpcode(OpcodeKind::Nop);
  const auto second = h.vm.mintOpcode(OpcodeKind::RelationNext);
  h.core.bindInput(second, h.arena.makeCell("edge"));
  h.core.bindInput(second, h.integer(1));
  h.core.bindInput(second, h.integer(3));
  h.core.bindInput(second, h.wildcard());
  h.core.bindInput(second, h.wildcard());
  h.core.bindOutput(second, victim);
  h.core.bindOutput(second, h.arena.makeCell("position"));
  h.core.bindOutput(second, h.arena.makeCell("limit"));
  for (const auto slot : first.answers) h.core.bindOutput(second, slot);
  ASSERT_TRUE(
      h.arena.link(first.op, h.core.dims().spin, DimVector::POS, second));
  h.chain.push_back(second);
  const auto executed = h.run();
  EXPECT_FALSE(executed.success);
  EXPECT_NE(executed.errorMessage.find("P15"), std::string::npos);
  EXPECT_EQ(h.answerText(first.answers[0]), "a");
  EXPECT_EQ(h.answerText(first.answers[1]), "b");
}

TEST(VortexRelationNextTest, AProviderMayReplaceItselfWhileRunning) {
  Harness h;
  h.vm.setRelationProvider(
      "edge", [&h](const RelationQuery &query) -> RelationResult {
        h.vm.setRelationProvider("edge", {});
        h.vm.setRelationProvider("other", [](const RelationQuery &) {
          return RelationResult{std::optional<RelationAnswer>{}};
        });
        return std::optional<RelationAnswer>{
            RelationAnswer{.answers = {query.arena.makeCell("x")}}};
      });
  const auto call = h.emit("edge", h.integer(0), h.integer(0), {h.wildcard()});
  const auto executed = h.run();
  ASSERT_TRUE(executed.success) << executed.errorMessage;
  EXPECT_EQ(h.core.render(call.found), CellValue{true});
  EXPECT_EQ(h.answerText(call.answers[0]), "x");
}

TEST(VortexRelationNextTest, AProviderCannotReEnterTheVm) {
  Harness h;
  std::string reentry;
  CellRef probeCursor = noCell;
  h.vm.setRelationProvider(
      "edge", [&](const RelationQuery &query) -> RelationResult {
        reentry = h.vm.step(probeCursor).errorMessage;
        return std::optional<RelationAnswer>{
            RelationAnswer{.answers = {query.arena.makeCell("x")}}};
      });
  probeCursor = h.vm.spawnCursor(h.vm.mintOpcode(OpcodeKind::Nop), "probe");
  h.emit("edge", h.integer(0), h.integer(0), {h.wildcard()});
  const auto executed = h.run();
  ASSERT_TRUE(executed.success) << executed.errorMessage;
  EXPECT_NE(reentry.find("cannot re-enter"), std::string::npos);
}
} // namespace
} // namespace zigzag::vortex
