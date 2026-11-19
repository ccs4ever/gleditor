// What these tests assert about GroupBegin, GroupNext and RestoreBindings:
//
//  - Cost. Each of the three used to copy up to eight whole VM tables on every
//    call. The undo journal they use now (vortex/table_undo.hpp) records only
//    the keys a call touches, so the entries
//    VortexVM::frameTableEntriesCopied() counts for one call is bounded by
//    those keys and does not grow with what earlier work left in the tables.
//    Counts only; nothing here asserts time.
//  - Equivalence. Seeded random sequences of group, stream, choice and binding
//    opcodes, with stale entries seeded where a call writes, produce the same
//    results, refusal messages and table contents as before the change. The
//    expected transcript (tests/samples/vortex/group_opcode_transcript.txt) was
//    captured from the code before the journal. Set GLEDITOR_GROUP_TRANSCRIPT
//    to a path to write a transcript there instead of comparing.
//  - Rollback. With the frame failpoint armed at every statement of a call, a
//    failed call leaves every table, the arena and the counters as it found
//    them.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "vortex_frame_test_access.hpp"

namespace zigzag::vortex {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;

/// Entries seeded into the tables that no call under test touches.
constexpr std::size_t bulkEntries = 10'000;
constexpr std::size_t smallBulk   = bulkEntries / 4;
/// Keys far above any cell the scenarios mint, so bulk entries never collide.
constexpr CellRef bulkFirstKey = 1'000'000;
/// Statement sweeps stop here; a call has far fewer journalled statements.
constexpr std::size_t sweepLimit = 256;
constexpr std::size_t sequences  = 32;
constexpr std::size_t stepsEach  = 100;
constexpr std::uint64_t baseSeed = 0x9e3779b97f4a7c15ULL;

struct Scenario {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  CellRef cursor{};
  CellRef other{}; // a finished cursor that can own the streams
  CellRef groups{}, it1{}, it2{}, m1{}, p1{}, m2{}, p2{}, value{}, plain{};
  CellRef begin{}, childNext{}, outerDescriptor{};
  std::vector<std::pair<std::string, CellRef>> pool;

  CellRef op(const OpcodeKind kind) { return vm.mintOpcode(kind); }
  void next(const CellRef from, const CellRef to) {
    ASSERT_TRUE(arena.link(from, core.dims().spin, DimVector::POS, to));
  }
  CellRef literal(const std::int64_t number) {
    return arena.makeScalarCell(number);
  }
  CellRef beginOp(const CellRef from, const CellRef to) {
    const auto made = op(OpcodeKind::GroupBegin);
    core.bindInput(made, from);
    core.bindOutput(made, to);
    return made;
  }
  CellRef nextOp(const CellRef from, const CellRef members,
                 const CellRef present) {
    const auto made = op(OpcodeKind::GroupNext);
    core.bindInput(made, from);
    core.bindOutput(made, members);
    core.bindOutput(made, present);
    return made;
  }
  CellRef restoreOp(const RuntimeBindingSnapshot &snapshot) {
    const auto descriptor = vm.makeBindingsDescriptor(snapshot);
    EXPECT_TRUE(descriptor);
    const auto made = op(OpcodeKind::RestoreBindings);
    core.bindInput(made, descriptor.value_or(noCell));
    return made;
  }

  /// Builds the closure call that enters a frame whose body starts at a
  /// GroupBegin over @p valueCount grouped values, and steps to that
  /// GroupBegin.
  void enterFrame(const std::size_t valueCount) {
    groups                 = arena.makeCell("rest groups");
    it1                    = arena.makeCell("iterator one");
    it2                    = arena.makeCell("iterator two");
    m1                     = arena.makeCell("members one");
    p1                     = arena.makeCell("present one");
    m2                     = arena.makeCell("members two");
    p2                     = arena.makeCell("present two");
    plain                  = arena.makeCell("plain");
    value                  = arena.makeCell("grouped value");
    begin                  = beginOp(groups, it1);
    childNext              = nextOp(it1, m1, p1);
    const auto childReturn = op(OpcodeKind::ReturnFrame);
    next(childNext, childReturn);
    const auto child =
        vm.makeFrameDescriptor(FrameDescriptorSpec{.entry = childNext});
    ASSERT_TRUE(child);
    core.bindInput(childReturn, *child);
    const auto invokeChild = op(OpcodeKind::InvokeFrame);
    core.bindInput(invokeChild, *child);
    const auto outerReturn = op(OpcodeKind::ReturnFrame);
    next(begin, invokeChild);
    next(invokeChild, outerReturn);
    const auto outer = vm.makeFrameDescriptor(FrameDescriptorSpec{
        .entry         = begin,
        .locals        = {groups, it1, it2, m1, p1, m2, p2, plain},
        .argumentPorts = {{groups, FramePortKind::Groups}},
    });
    ASSERT_TRUE(outer);
    core.bindInput(outerReturn, *outer);
    outerDescriptor    = *outer;
    const auto closure = vm.makeClosureDescriptor(ClosureDescriptorSpec{
        .frameDescriptor     = *outer,
        .minFormalCount      = 0,
        .maxFormalCount      = 1,
        .frameInputs         = {{ClosureInputSource::AppliedRestGroups, 0}},
        .variadicFormalIndex = 0,
        .formalNames         = std::vector<std::optional<std::string>>{"rest"},
    });
    ASSERT_TRUE(closure);
    const auto handle = arena.makeCell("closure");
    const auto create = op(OpcodeKind::CreateClosure);
    core.bindInput(create, *closure);
    core.bindOutput(create, handle);
    const auto checked = arena.makeCell("checked closure");
    const auto check   = op(OpcodeKind::CheckClosureCall);
    core.bindInput(check, handle);
    core.bindInput(
        check, literal(static_cast<std::int64_t>(ClosureCallMode::Explicit)));
    core.bindInput(check, literal(1));
    core.bindInput(check, arena.makeCell("rest"));
    core.bindOutput(check, checked);
    const auto context      = arena.makeCell("empty context");
    const auto clearContext = op(OpcodeKind::StreamClear);
    core.bindOutput(clearContext, context);
    const auto actual      = arena.makeCell("rest actual");
    const auto clearActual = op(OpcodeKind::StreamClear);
    core.bindOutput(clearActual, actual);
    std::vector<CellRef> chain{create, check, clearContext, clearActual};
    for (std::size_t index = 0; index < valueCount; ++index) {
      const auto append = op(OpcodeKind::StreamAppend);
      core.bindInput(append, value);
      core.bindOutput(append, actual);
      chain.push_back(append);
    }
    const auto apply = op(OpcodeKind::ApplyClosure);
    core.bindInput(apply, checked);
    core.bindInput(
        apply, literal(static_cast<std::int64_t>(ClosureCallMode::Explicit)));
    core.bindInput(apply, context);
    core.bindInput(apply, literal(1));
    core.bindInput(apply, arena.makeCell("rest"));
    core.bindInput(apply, actual);
    chain.push_back(apply);
    for (std::size_t index = 0; index + 1 < chain.size(); ++index)
      next(chain[index], chain[index + 1]);
    const auto halt = op(OpcodeKind::Halt);
    next(apply, halt);
    other  = vm.spawnCursor(halt, "finished_owner");
    cursor = vm.spawnCursor(create, "group_journal");
    for (std::size_t index = 0; index < chain.size(); ++index)
      ASSERT_TRUE(vm.step(cursor).success) << "setup step " << index;
    ASSERT_EQ(vm.getCursorOpcode(cursor), begin);
  }

  void buildPool() {
    const auto seeded = [this](std::string name, const CellRef made) {
      pool.emplace_back(std::move(name), made);
    };
    seeded("begin1", begin);
    seeded("begin2", beginOp(groups, it2));
    seeded("beginSelf", beginOp(groups, groups));
    seeded("beginPlain", beginOp(plain, it2));
    seeded("beginOverIt1", beginOp(groups, it1));
    seeded("next1", childNext);
    seeded("next1b", nextOp(it1, m1, p1));
    seeded("next2", nextOp(it2, m2, p2));
    seeded("next1to2", nextOp(it1, m2, p2));
    seeded("next2over1", nextOp(it2, it1, p1));
    seeded("nextSame", nextOp(it1, m1, m1));
    seeded("nextPlain", nextOp(plain, m1, p1));
    const auto clear1 = op(OpcodeKind::StreamClear);
    core.bindOutput(clear1, m1);
    seeded("clear1", clear1);
    const auto clearIt = op(OpcodeKind::StreamClear);
    core.bindOutput(clearIt, it1);
    seeded("clearIt1", clearIt);
    const auto append = op(OpcodeKind::StreamAppend);
    core.bindInput(append, value);
    core.bindOutput(append, m2);
    seeded("append2", append);
    const auto ordinary = op(OpcodeKind::IteratorNext);
    core.bindInput(ordinary, it1);
    core.bindOutput(ordinary, m1);
    core.bindOutput(ordinary, p1);
    seeded("iteratorNext1", ordinary);
    const auto copy = op(OpcodeKind::CopyCellRefs);
    core.bindInput(copy, it1);
    core.bindOutput(copy, plain);
    seeded("copy", copy);
    for (const auto &[label, target] :
         std::vector<std::pair<const char *, CellRef>>{
             {"choiceBegin2", pool[1].second},
             {"choiceNext1", childNext},
             {"choiceNext2", pool[7].second}}) {
      const auto choice = op(OpcodeKind::Choice);
      core.bindInput(choice, literal(static_cast<std::int64_t>(target)));
      seeded(label, choice);
    }
    seeded("fail", op(OpcodeKind::Fail));
    seeded("cut", op(OpcodeKind::Cut));
    RuntimeBindingSnapshot snapshot;
    snapshot.cellReferences.emplace(plain, value);
    snapshot.streams.emplace(m1, std::vector<CellRef>{value, plain});
    snapshot.iterators.emplace(it2, ClosedIteratorState::Exhausted);
    seeded("restore", restoreOp(snapshot));
    const auto ret = op(OpcodeKind::ReturnFrame);
    core.bindInput(ret, outerDescriptor);
    seeded("return", ret);
    // A body that ran out of successors is a failure of its own; every pool
    // entry gets one so that the opcode under test decides the outcome.
    for (const auto &entry : pool)
      if (!arena.linked(entry.second, core.dims().spin, DimVector::POS))
        next(entry.second, op(OpcodeKind::Nop));
  }
};

/// Pointers print as 0x...; the same object must read the same in two runs.
std::string stableDigest(const VortexVM &vm) {
  static const std::regex pointer{"0x[0-9a-f]+"};
  const auto text = FrameTestAccess::digest(vm);
  std::vector<std::string> seen;
  std::string out;
  auto last = text.cbegin();
  for (std::sregex_iterator it{text.begin(), text.end(), pointer}, end;
       it != end; ++it) {
    out.append(last, text.cbegin() + it->position());
    const auto found = std::ranges::find(seen, it->str());
    const auto index = static_cast<std::size_t>(found - seen.begin());
    if (found == seen.end()) seen.push_back(it->str());
    out += "p" + std::to_string(index);
    last = text.cbegin() + it->position() + it->length();
  }
  out.append(last, text.cend());
  return out;
}

std::uint64_t fnv(const std::string &text) {
  constexpr std::uint64_t offset = 14695981039346656037ULL;
  constexpr std::uint64_t prime  = 1099511628211ULL;
  std::uint64_t hash             = offset;
  for (const unsigned char byte : text) hash = (hash ^ byte) * prime;
  return hash;
}

struct Rng {
  std::uint64_t state;
  std::uint64_t operator()() {
    state += baseSeed;
    auto z = state;
    z      = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z      = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }
  std::size_t below(const std::size_t bound) {
    return static_cast<std::size_t>((*this)() % bound);
  }
};

/// Refusal messages in order of first appearance; the transcript names each by
/// number and lists the table once, so the file stays short and still readable.
using MessageTable = std::vector<std::string>;

std::size_t messageId(MessageTable &table, const std::string &message) {
  const auto found = std::ranges::find(table, message);
  if (found != table.end())
    return static_cast<std::size_t>(found - table.begin());
  table.push_back(message);
  return table.size() - 1;
}

std::string runSequence(const std::uint64_t seed, MessageTable &messages) {
  Rng rng{seed};
  Scenario s;
  s.enterFrame(1 + rng.below(3));
  s.buildPool();
  std::ostringstream log;
  log << "sequence " << seed << '\n';
  for (std::size_t index = 0; index < stepsEach; ++index) {
    // Stale entries where a call will write or erase them, so a restore of a
    // prior value and not only of an absent key is checked.
    switch (rng.below(24)) {
    case 0:
      FrameTestAccess::seedRegisters(
          s.vm, s.m1, FrameTestAccess::top(s.vm, s.cursor), false);
      break;
    case 1:
      FrameTestAccess::seedStaleReference(s.vm, s.it2);
      break;
    case 2:
      FrameTestAccess::seedStaleStreamArgument(s.vm, s.p1);
      break;
    case 3:
      FrameTestAccess::setStreamCursor(s.vm, s.other);
      break;
    case 4:
      FrameTestAccess::setStreamCursor(s.vm, std::nullopt);
      break;
    default:
      break;
    }
    if (rng.below(9) == 0)
      s.vm.pushChoicePoint(s.cursor, s.pool[rng.below(s.pool.size())].second);
    const auto &[label, chosen] = s.pool[rng.below(s.pool.size())];
    s.vm.setCursorOpcode(s.cursor, chosen);
    const auto result = s.vm.step(s.cursor);
    log << index << ' ' << label << ' ' << (result.success ? "ok" : "fail")
        << " m" << messageId(messages, result.errorMessage)
        << " cells=" << s.arena.cellCount()
        << " depth=" << s.vm.activationDepth(s.cursor) << " h=" << std::hex
        << fnv(stableDigest(s.vm)) << std::dec << '\n';
  }
  return log.str();
}

/// RestoreBindings alone, with no frame around it: random snapshots over a
/// small set of cells replace tables that stale entries and earlier restores
/// have filled, and some are refused by the (tightened) limits.
std::string runRestoreSequence(const std::uint64_t seed,
                               MessageTable &messages) {
  constexpr std::size_t cellCount      = 8;
  constexpr std::size_t restoreChoices = 10;
  Rng rng{seed};
  Scenario s;
  s.vm.setBindingsLimits(LogicalBindingsLimits{.maxSlots         = 6,
                                               .maxReferences    = 40,
                                               .maxRestores      = 30,
                                               .maxRestoredSlots = 40});
  std::vector<CellRef> cells;
  for (std::size_t index = 0; index < cellCount; ++index)
    cells.push_back(s.arena.makeCell("bound " + std::to_string(index)));
  const auto pick = [&] { return cells[rng.below(cells.size())]; };
  std::vector<CellRef> restores;
  for (std::size_t index = 0; index < restoreChoices; ++index) {
    RuntimeBindingSnapshot snapshot;
    for (std::size_t slot = rng.below(4); slot > 0; --slot)
      snapshot.cellReferences.emplace(
          pick(), rng.below(3) == 0 ? std::nullopt : std::optional{pick()});
    for (std::size_t slot = rng.below(3); slot > 0; --slot) {
      std::vector<CellRef> members;
      for (std::size_t member = rng.below(4); member > 0; --member)
        members.push_back(pick());
      snapshot.streams.emplace(pick(), std::move(members));
    }
    if (rng.below(2) == 0)
      snapshot.iterators.emplace(
          pick(), rng.below(2) == 0 ? ClosedIteratorState::Exhausted
                                    : ClosedIteratorState::Unavailable);
    const auto descriptor = s.vm.makeBindingsDescriptor(snapshot);
    if (!descriptor) continue;
    const auto made = s.op(OpcodeKind::RestoreBindings);
    s.core.bindInput(made, *descriptor);
    s.next(made, s.op(OpcodeKind::Nop));
    restores.push_back(made);
  }
  const auto clear = s.op(OpcodeKind::StreamClear);
  s.core.bindOutput(clear, cells[0]);
  s.next(clear, s.op(OpcodeKind::Nop));
  restores.push_back(clear);
  s.cursor = s.vm.spawnCursor(restores.front(), "restore_sequence");
  std::ostringstream log;
  log << "restore sequence " << seed << '\n';
  for (std::size_t index = 0; index < stepsEach; ++index) {
    if (rng.below(3) == 0)
      FrameTestAccess::seedRegisters(s.vm, pick(), nullptr, false);
    s.vm.setCursorOpcode(s.cursor, restores[rng.below(restores.size())]);
    const auto result = s.vm.step(s.cursor);
    log << index << " restore " << (result.success ? "ok" : "fail") << " m"
        << messageId(messages, result.errorMessage)
        << " cells=" << s.arena.cellCount() << " h=" << std::hex
        << fnv(stableDigest(s.vm)) << std::dec << '\n';
  }
  return log.str();
}

std::string transcript() {
  std::string all;
  MessageTable messages;
  for (std::size_t index = 0; index < sequences; ++index)
    all += runSequence(baseSeed + index, messages);
  for (std::size_t index = 0; index < sequences / 2; ++index)
    all += runRestoreSequence(baseSeed ^ (index + 1), messages);
  for (std::size_t index = 0; index < messages.size(); ++index)
    all += "message m" + std::to_string(index) + " [" + messages[index] + "]\n";
  return all;
}

TEST(VortexGroupJournalTest,
     RandomisedGroupSequencesMatchTheCapturedTranscript) {
  const auto actual = transcript();
  if (const char *path = std::getenv("GLEDITOR_GROUP_TRANSCRIPT")) {
    std::ofstream(path, std::ios::binary) << actual;
    GTEST_SKIP() << "wrote " << path;
  }
  std::ifstream in("tests/samples/vortex/group_opcode_transcript.txt",
                   std::ios::binary);
  ASSERT_TRUE(in) << "transcript fixture missing";
  std::stringstream expected;
  expected << in.rdbuf();
  EXPECT_EQ(actual, expected.str());
  EXPECT_NE(actual.find(" ok m"), std::string::npos);
  EXPECT_NE(actual.find(" fail m"), std::string::npos);
}

TEST(VortexGroupJournalTest,
     TheTranscriptExercisesBothOutcomesOfEveryGroupOpcode) {
  const auto actual = transcript();
  for (const auto *name : {"begin1 ok", "next1 ok", "next2 ok", "begin2 ok",
                           "restore fail", "beginSelf fail", "nextPlain fail"})
    EXPECT_NE(actual.find(name), std::string::npos) << name;
}

// ---- cost -------------------------------------------------------------

std::size_t copiedBy(Scenario &s, const CellRef opcode) {
  s.vm.setCursorOpcode(s.cursor, opcode);
  const auto before = s.vm.frameTableEntriesCopied();
  const auto result = s.vm.step(s.cursor);
  EXPECT_TRUE(result.success) << result.errorMessage;
  return s.vm.frameTableEntriesCopied() - before;
}

struct Counts {
  std::size_t begin{}, next{}, restore{};
};

Counts countsWith(const std::size_t bulk) {
  Counts counts;
  {
    Scenario s;
    s.enterFrame(2);
    s.buildPool();
    FrameTestAccess::seedBulk(s.vm, bulk, bulkFirstKey);
    counts.begin = copiedBy(s, s.begin);
    counts.next  = copiedBy(s, s.childNext);
  }
  {
    Scenario s;
    RuntimeBindingSnapshot snapshot;
    const auto key = s.arena.makeCell("bound");
    snapshot.cellReferences.emplace(key, key);
    const auto restore = s.restoreOp(snapshot);
    const auto halt    = s.op(OpcodeKind::Halt);
    s.next(restore, halt);
    s.cursor = s.vm.spawnCursor(restore, "restore_cost");
    FrameTestAccess::seedBulk(s.vm, bulk, bulkFirstKey);
    counts.restore = copiedBy(s, restore);
  }
  return counts;
}

/// Entries one call touches: its own keys, never the table sizes.
constexpr std::size_t keysTouchedBound = 12;

TEST(VortexGroupJournalTest, GroupBeginCopiesOnlyTheKeysItTouches) {
  EXPECT_LE(countsWith(bulkEntries).begin, keysTouchedBound);
}
TEST(VortexGroupJournalTest, GroupNextCopiesOnlyTheKeysItTouches) {
  EXPECT_LE(countsWith(bulkEntries).next, keysTouchedBound);
}
TEST(VortexGroupJournalTest, RestoreBindingsCopiesNoWholeTable) {
  EXPECT_LE(countsWith(bulkEntries).restore, keysTouchedBound);
}

TEST(VortexGroupJournalTest, TheCostDoesNotScaleWithTheTableSize) {
  const auto small = countsWith(smallBulk);
  const auto large = countsWith(bulkEntries);
  EXPECT_EQ(small.begin, large.begin);
  EXPECT_EQ(small.next, large.next);
  EXPECT_EQ(small.restore, large.restore);
}

/// Prints, and asserts nothing: wall time of one call of each opcode with the
/// tables full, for the commit message and reports.
TEST(VortexGroupJournalTest, ReportsMicrosecondsPerCallWithFullTables) {
  const auto time = [](const auto &call) {
    const auto start = std::chrono::steady_clock::now();
    call();
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
  };
  Scenario s;
  s.enterFrame(2);
  s.buildPool();
  FrameTestAccess::seedBulk(s.vm, bulkEntries, bulkFirstKey);
  const auto step = [&](const CellRef opcode) {
    s.vm.setCursorOpcode(s.cursor, opcode);
    return [&s] { s.vm.step(s.cursor); };
  };
  const auto begin = time(step(s.begin));
  const auto next  = time(step(s.childNext));
  GTEST_LOG_(INFO) << "GroupBegin " << begin << " us, GroupNext " << next
                   << " us with " << bulkEntries << " entries per table";
}

// ---- rollback ---------------------------------------------------------

TEST(VortexGroupJournalTest, AFailedGroupCallLeavesEveryTableAsItFoundIt) {
  for (const char *which : {"begin1", "next1"}) {
    Scenario s;
    s.enterFrame(2);
    s.buildPool();
    if (std::string_view{which} == "next1") {
      s.vm.setCursorOpcode(s.cursor, s.begin);
      ASSERT_TRUE(s.vm.step(s.cursor).success);
    }
    const auto chosen = std::ranges::find(s.pool, std::string{which},
                                          &decltype(s.pool)::value_type::first);
    ASSERT_NE(chosen, s.pool.end());
    // Stale entries in the tables the call writes, so prior values are
    // restored.
    FrameTestAccess::seedRegisters(s.vm, s.m1,
                                   FrameTestAccess::top(s.vm, s.cursor), false);
    FrameTestAccess::seedRegisters(s.vm, s.p1,
                                   FrameTestAccess::top(s.vm, s.cursor), false);
    FrameTestAccess::seedRegisters(s.vm, s.it1,
                                   FrameTestAccess::top(s.vm, s.cursor), false);
    const auto digest = FrameTestAccess::digest(s.vm);
    const auto cells  = s.arena.cellCount();
    std::size_t stops = 0;
    for (std::size_t nth = 1; nth <= sweepLimit; ++nth) {
      s.vm.setCursorOpcode(s.cursor, chosen->second);
      FrameTestAccess::arm(s.vm, nth);
      const auto result = s.vm.step(s.cursor);
      FrameTestAccess::arm(s.vm, std::nullopt);
      if (result.success) break;
      EXPECT_NE(result.errorMessage.find("failpoint"), std::string::npos)
          << which << " statement " << nth << ": " << result.errorMessage;
      EXPECT_EQ(FrameTestAccess::digest(s.vm), digest)
          << which << " failing after statement " << nth;
      EXPECT_EQ(s.arena.cellCount(), cells);
      ++stops;
    }
    EXPECT_GE(stops, 3u) << which;
  }
}

TEST(VortexGroupJournalTest, AFailedRestoreLeavesEveryTableAsItFoundIt) {
  Scenario s;
  RuntimeBindingSnapshot snapshot;
  const auto key    = s.arena.makeCell("bound");
  const auto stream = s.arena.makeCell("stream");
  snapshot.cellReferences.emplace(key, key);
  snapshot.streams.emplace(stream, std::vector<CellRef>{key});
  const auto restore = s.restoreOp(snapshot);
  const auto halt    = s.op(OpcodeKind::Halt);
  s.next(restore, halt);
  s.cursor = s.vm.spawnCursor(restore, "restore_rollback");
  FrameTestAccess::seedBulk(s.vm, 20, bulkFirstKey);
  FrameTestAccess::seedRegisters(s.vm, key, nullptr, false);
  const auto digest = FrameTestAccess::digest(s.vm);
  std::size_t stops = 0;
  for (std::size_t nth = 1; nth <= sweepLimit; ++nth) {
    s.vm.setCursorOpcode(s.cursor, restore);
    FrameTestAccess::arm(s.vm, nth);
    const auto result = s.vm.step(s.cursor);
    FrameTestAccess::arm(s.vm, std::nullopt);
    if (result.success) break;
    EXPECT_NE(result.errorMessage.find("failpoint"), std::string::npos)
        << result.errorMessage;
    EXPECT_EQ(FrameTestAccess::digest(s.vm), digest) << "statement " << nth;
    ++stops;
  }
  EXPECT_GE(stops, 1u);
}

} // namespace
} // namespace zigzag::vortex
