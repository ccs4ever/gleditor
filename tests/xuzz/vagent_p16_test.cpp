// Phase-6 P16 checkpoint-ceremony tests (design/vagent-metaprogramming-
// implementation.md :526-575; spec :2163-2179's flag windows, :2394-2406's
// arm split, :2422-2432's donor/copy kind split). `p16.cpp`'s concrete
// object is driven here the way the mesh's `pumpOne` drives it: flag →
// re-kind → publish → restore, with `regenesisCycle` standing in for the
// bound publish command so the promote runs under re-kinded handles.
//
// Two fixture shapes cover the exit criteria:
//
//  - Session-side authoring: registered heads and +d.refs records built
//    directly on the arena — the kind-guard arms in isolation (ephemeral
//    vs persistent positions, name-records, the malformed-record veto,
//    restoreAll's idempotency, installedCopies joining the sweep).
//
//  - The acceptance journey: learn_plan → onFlagged → publish inside
//    onFlaggedTurnEnd (a real promote+rebuild) → runGenesis → learn_where
//    → the same again — two checkpoint/resume cycles whose recipes carry
//    *real* promoted positions instead of the stale arena int64s a
//    checkpoint-free promote leaves behind.
#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/vagent/genesis.hpp"
#include "common/xanadu/vagent/lineage.hpp"
#include "common/xanadu/vagent/meta.hpp"
#include "common/xanadu/vagent/p16.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vortex/worker_context.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace {

using namespace xanadu::vagent;
using xanadu::MicroversionId;
using xanadu::Store;
using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::noCell;
using zigzag::Posward;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::WorkerContext;

/// The fixed filing list: the three name arrays, verbatim — the same set
/// the genesis fixture files.
std::vector<std::string_view> filedNames() {
  std::vector<std::string_view> out;
  for (const auto name : kGenesisDimensionNames) out.push_back(name);
  for (const auto name : kMetaDimensionNames) out.push_back(name);
  for (const auto name : kAgentDimensionNames) out.push_back(name);
  return out;
}

/// Store-side authoring, same shape as the genesis fixture's Author.
struct Author {
  Store &store;

  CellRef cell(std::string_view text, MicroversionId &at) {
    at             = store.makeCell(at, text);
    const auto ref = store.cellRefOf(at);
    EXPECT_TRUE(ref.has_value()) << "makeCell produced no cell";
    return ref.value_or(noCell);
  }
  CellRef scalar(std::int64_t value, MicroversionId &at) {
    at             = store.makeScalarCell(at, value);
    const auto ref = store.cellRefOf(at);
    EXPECT_TRUE(ref.has_value()) << "makeScalarCell produced no cell";
    return ref.value_or(noCell);
  }
  void link(CellRef from, DimRef dim, CellRef to, MicroversionId &at) {
    at = store.setLink(at, from, dim, Posward, to);
  }
};

/// A minimal program image: dims filed, program root + stdlib chain, a
/// "boot" host symbol, and two heads in the [literal(H), literal(O),
/// tuple(H, d.entry, +, O)] recipe shape learn_*'s reify replays.
struct Image {
  Store store;
  MicroversionId at;
  std::map<std::string, DimRef> dim;
  CellRef programRoot{noCell}, sysModule{noCell}, libModule{noCell};
  CellRef hostSym{noCell};
  CellRef progHeadA{noCell}, progHeadB{noCell};
  CellRef opA{noCell}, opB{noCell}, imageRecord{noCell};
  std::uint32_t imageExtent{0};
};

DimRef dimAt(const std::map<std::string, DimRef> &dim, std::string_view name) {
  return dim.at(std::string{name});
}

template <typename F> DimRef dimAt(const F &f, std::string_view name) {
  return dimAt(f.dim, name);
}

void buildImage(Image &f) {
  Author a{f.store};
  f.at        = a.store.sliceGenesis({});
  auto filing = fileDimensionNames(a.store, f.at, filedNames());
  ASSERT_TRUE(filing.has_value()) << (filing ? "" : filing.error());
  for (const auto name : filedNames())
    f.dim[std::string{name}] =
        filing->fold.dimensionNamed(name, a.store).value_or(noCell);
  f.at = filing->head;

  f.programRoot = a.cell("program-root", f.at);
  f.sysModule   = a.cell("sys", f.at);
  f.libModule   = a.cell("lib", f.at);
  a.link(f.programRoot, dimAt(f, "d.stdlib"), f.sysModule, f.at);
  a.link(f.sysModule, dimAt(f, "d.stdlib"), f.libModule, f.at);
  f.hostSym = a.cell("boot", f.at);
  a.link(f.sysModule, dimAt(f, "d.vars"), f.hostSym, f.at);

  f.opA = a.scalar(static_cast<std::int64_t>(OpcodeKind::Nop), f.at);
  f.opB = a.scalar(static_cast<std::int64_t>(OpcodeKind::Return), f.at);

  const auto progHead = [&](std::string_view label, CellRef op) {
    const auto head  = a.cell("", f.at);
    const auto entry = dimAt(f, "d.entry");
    a.link(head, entry, op, f.at);
    a.link(head, dimAt(f, "d.name"), a.cell(label, f.at), f.at);
    const auto desc = a.cell("", f.at);
    a.link(head, dimAt(f, "d.values"), desc, f.at);
    CellRef tail    = desc;
    const auto pure = a.cell("pure", f.at);
    a.link(pure, dimAt(f, "d.values"), a.scalar(1, f.at), f.at);
    a.link(tail, dimAt(f, "d.vars"), pure, f.at);
    tail              = pure;
    const auto result = a.cell("result", f.at);
    a.link(result, dimAt(f, "d.name"), a.cell("()", f.at), f.at);
    a.link(tail, dimAt(f, "d.vars"), result, f.at);
    const auto mH = a.scalar(static_cast<std::int64_t>(head), f.at);
    const auto mO = a.scalar(static_cast<std::int64_t>(op), f.at);
    const auto mF = a.scalar(static_cast<std::int64_t>(head), f.at);
    const auto an = a.scalar(static_cast<std::int64_t>(entry), f.at);
    a.link(an, dimAt(f, "d.name"), a.cell("d.entry", f.at), f.at);
    const auto mD = a.scalar(1, f.at);
    const auto mT = a.scalar(static_cast<std::int64_t>(op), f.at);
    a.link(head, dimAt(f, "d.recipe"), mH, f.at);
    a.link(mH, dimAt(f, "d.step"), mO, f.at);
    a.link(mO, dimAt(f, "d.step"), mF, f.at);
    a.link(mF, dimAt(f, "d.step"), an, f.at);
    a.link(an, dimAt(f, "d.step"), mD, f.at);
    a.link(mD, dimAt(f, "d.step"), mT, f.at);
    return head;
  };
  f.progHeadA = progHead("frag-a", f.opA);
  f.progHeadB = progHead("frag-b", f.opB);

  f.imageRecord      = a.cell("image-record", f.at);
  CellRef imgVars    = f.imageRecord;
  const auto opsRec  = a.cell("ops", f.at);
  const auto opsHead = a.cell("", f.at);
  a.link(opsRec, dimAt(f, "d.values"), opsHead, f.at);
  a.link(imgVars, dimAt(f, "d.vars"), opsRec, f.at);
  CellRef tail = opsHead;
  for (const auto op : {f.opA, f.opB}) {
    const auto m = a.scalar(static_cast<std::int64_t>(op), f.at);
    a.link(tail, dimAt(f, "d.step"), m, f.at);
    tail = m;
  }
  f.imageExtent = static_cast<std::uint32_t>(a.store.segmentedOps().size());
}

/// One session: a fold, its fresh WorkerContext, an agent cell, and the
/// bound set the ceremony factory takes.
struct Session {
  std::unique_ptr<zigzag::Manifold> fold;
  std::unique_ptr<WorkerContext> ctx;
  CellRef agentCell{noCell};
  MetaBindSet binds;

  ArenaManifold &arena() { return ctx->arena; }
  VortexCore &core() { return ctx->core; }
};

Session makeSession(Image &f) {
  Session s;
  s.fold = std::make_unique<zigzag::Manifold>(
      f.store.rebuildManifold(f.store.latest()));
  s.ctx       = std::make_unique<WorkerContext>(s.fold.get(), &f.store);
  s.agentCell = s.ctx->arena.makeCell("agent");
  auto binds =
      mintMetaBindSet(s.ctx->arena, s.ctx->core, f.programRoot, s.agentCell);
  EXPECT_TRUE(binds.has_value()) << (binds ? "" : binds.error());
  binds->ownModule = f.sysModule;
  binds->lineage   = MetaLineage{
      .programRef  = f.programRoot,
      .imageExtent = f.imageExtent,
  };
  s.binds = *binds;
  return s;
}

// ---------------------------------------------------------------------------
// Arena-side record authoring — the same +d.refs shape emission/reify mint
// ---------------------------------------------------------------------------

/// A marked cell: Int64 payload plus its own freshly minted
/// `d.mesh.relocation → "cell"` marker edge.
CellRef markCell(ArenaManifold &arena, DimRef meshRelocation,
                 std::int64_t value) {
  const auto cell = arena.makeScalarCell(value);
  zigzag::expectWritten(
      arena.link(cell, meshRelocation, Posward, arena.makeCell("cell")));
  return cell;
}

/// Append `rec` to `head`'s +d.refs chain: head `+d.refs → first`, then
/// `+d.step` — reify/fragment's chain shape.
void pushRefRecord(ArenaManifold &arena, const MetaBindSet &binds, DimRef step,
                   CellRef head, std::optional<CellRef> &tail, CellRef rec) {
  if (!tail)
    zigzag::expectWritten(arena.link(head, binds.dims.refs, Posward, rec));
  else
    zigzag::expectWritten(arena.link(*tail, step, Posward, rec));
  tail = rec;
}

/// A ref-record (no `+d.name`) marking `marked`.
CellRef refRecord(ArenaManifold &arena, const MetaBindSet &binds,
                  CellRef marked) {
  const auto rec = arena.makeCell();
  zigzag::expectWritten(arena.link(rec, binds.dims.ref, Posward, marked));
  return rec;
}

/// A name-record (`+d.name`, optional `+d.grab` role) marking `marked` —
/// never carries a marker itself.
CellRef nameRecord(ArenaManifold &arena, const MetaBindSet &binds,
                   VortexCore &core, CellRef marked, std::string_view name,
                   std::optional<std::string_view> role = std::nullopt) {
  const auto rec = refRecord(arena, binds, marked);
  zigzag::expectWritten(
      arena.link(rec, core.dims().name, Posward, arena.makeCell(name)));
  if (role)
    zigzag::expectWritten(
        arena.link(rec, core.dims().grab, Posward, arena.makeCell(*role)));
  return rec;
}

/// Every marked cell the ceremony's sweep enumerates on `head`: ref-
/// records' `+d.ref` targets, name-records excluded — mirrors the
/// object's own inventory so a test can diff the two sets by hand.
std::vector<CellRef> markedOnHead(ArenaManifold &arena,
                                  const MetaBindSet &binds, VortexCore &core,
                                  CellRef head) {
  std::vector<CellRef> out;
  for (auto rec = arena.linked(head, binds.dims.refs, Posward);
       rec && arena.contains(*rec);
       rec = arena.linked(*rec, core.dims().step, Posward)) {
    const auto marked = arena.linked(*rec, binds.dims.ref, Posward);
    if (!marked || arena.linked(*rec, core.dims().name, Posward)) continue;
    out.push_back(*marked);
  }
  return out;
}

/// Register `head` on the agent's `+d.code` rank (append order: each
/// head's own +d.code edge names the next).
void registerHead(ArenaManifold &arena, const MetaBindSet &binds, CellRef head,
                  CellRef &codeTail) {
  zigzag::expectWritten(arena.link(codeTail, binds.dims.code, Posward, head));
  codeTail = head;
}

/// A minimal fragment head arena-side: `+d.entry` only — the sweep's
/// inventory hangs off `+d.refs`, not the recipe, for the unit arms.
CellRef arenaHead(ArenaManifold &arena, const MetaBindSet &binds, CellRef op) {
  const auto head = arena.makeCell();
  zigzag::expectWritten(arena.link(head, binds.dims.entry, Posward, op));
  return head;
}

// ---------------------------------------------------------------------------
// The kind-guard arms in isolation
// ---------------------------------------------------------------------------

TEST(VagentP16Test, OnFlaggedRekindsOnlyMarkedEphemeralPositions) {
  Image f;
  buildImage(f);
  auto s   = makeSession(f);
  auto &ar = s.arena();
  auto &co = s.core();

  const auto head = arenaHead(ar, s.binds, f.opA);
  auto codeTail   = s.agentCell;
  registerHead(ar, s.binds, head, codeTail);

  // Four marked cells, one per arm:
  //   eph  — Int64 naming an ephemeral arena cell → re-kinds.
  //   pers — Int64 naming a persistent fold cell → stays Int64.
  //   dir  — Int64(-1), a tuple's sign field → stays Int64.
  //   name — the name-record's slot → the sweep never sees it.
  const auto referent = ar.makeCell("referent");
  const auto eph      = markCell(ar, s.binds.dims.meshRelocation,
                                 static_cast<std::int64_t>(referent));
  const auto pers     = markCell(ar, s.binds.dims.meshRelocation,
                                 static_cast<std::int64_t>(f.programRoot));
  const auto dir      = markCell(ar, s.binds.dims.meshRelocation, -1);
  const auto nameSlot = ar.makeScalarCell(
      static_cast<std::int64_t>(referent)); // unmarked, by design

  std::optional<CellRef> tail;
  pushRefRecord(ar, s.binds, co.dims().step, head, tail,
                refRecord(ar, s.binds, eph));
  pushRefRecord(ar, s.binds, co.dims().step, head, tail,
                refRecord(ar, s.binds, pers));
  pushRefRecord(ar, s.binds, co.dims().step, head, tail,
                refRecord(ar, s.binds, dir));
  pushRefRecord(ar, s.binds, co.dims().step, head, tail,
                nameRecord(ar, s.binds, co, nameSlot, "reify:src:x:0",
                           std::string_view{"run"}));

  int calls = 0;
  auto c    = makeP16Ceremony(ar, co, s.binds,
                              [&](void) -> std::expected<void, std::string> {
                             ++calls;
                             return {};
                              });
  ASSERT_TRUE(c->onFlagged());

  EXPECT_EQ(ar.valueKindOf(eph), xanadu::ValueKind::OpHandle);
  EXPECT_EQ(ar.handleTarget(eph).value_or(noCell), referent)
      << "the handle names the position the Int64 spelled";
  EXPECT_EQ(ar.valueKindOf(pers), xanadu::ValueKind::Int64)
      << "a persistent referent already spells its durable name";
  EXPECT_EQ(ar.valueKindOf(dir), xanadu::ValueKind::Int64)
      << "a ±1 sign field is data, not a position";
  EXPECT_EQ(ar.valueKindOf(nameSlot), xanadu::ValueKind::Int64)
      << "the name-record's slot is the name arm's, never re-kinded";
  EXPECT_EQ(calls, 0);

  ASSERT_TRUE(c->onFlaggedTurnEnd().has_value());
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(ar.valueKindOf(eph), xanadu::ValueKind::Int64);
  EXPECT_EQ(ar.asInt64(eph).value_or(0), static_cast<std::int64_t>(referent))
      << "restore re-spells the original position";
}

TEST(VagentP16Test, TurnEndPublishesWhileMarkedCellsStandOpHandle) {
  Image f;
  buildImage(f);
  auto s   = makeSession(f);
  auto &ar = s.arena();
  auto &co = s.core();

  const auto head = arenaHead(ar, s.binds, f.opA);
  auto codeTail   = s.agentCell;
  registerHead(ar, s.binds, head, codeTail);

  const auto referent = ar.makeCell();
  const auto marked   = markCell(ar, s.binds.dims.meshRelocation,
                                 static_cast<std::int64_t>(referent));
  std::optional<CellRef> tail;
  pushRefRecord(ar, s.binds, co.dims().step, head, tail,
                refRecord(ar, s.binds, marked));

  // The publish command's whole point: it runs *inside* the window —
  // observe the re-kinded kind at call time, not the restored one.
  xanadu::ValueKind kindAtPublish = xanadu::ValueKind::Int64;
  auto c = makeP16Ceremony(ar, co, s.binds,
                           [&](void) -> std::expected<void, std::string> {
                             kindAtPublish = ar.valueKindOf(marked);
                             return {};
                           });
  ASSERT_TRUE(c->onFlagged());
  ASSERT_TRUE(c->onFlaggedTurnEnd().has_value());
  EXPECT_EQ(kindAtPublish, xanadu::ValueKind::OpHandle)
      << "the publish ran before the restore, under re-kinded handles";
  EXPECT_EQ(ar.valueKindOf(marked), xanadu::ValueKind::Int64);
}

TEST(VagentP16Test, PublishRefusalRestoresAndReArms) {
  Image f;
  buildImage(f);
  auto s   = makeSession(f);
  auto &ar = s.arena();
  auto &co = s.core();

  const auto head = arenaHead(ar, s.binds, f.opA);
  auto codeTail   = s.agentCell;
  registerHead(ar, s.binds, head, codeTail);

  const auto referent = ar.makeCell();
  const auto marked   = markCell(ar, s.binds.dims.meshRelocation,
                                 static_cast<std::int64_t>(referent));
  std::optional<CellRef> tail;
  pushRefRecord(ar, s.binds, co.dims().step, head, tail,
                refRecord(ar, s.binds, marked));

  int attempts = 0;
  auto c       = makeP16Ceremony(ar, co, s.binds,
                                 [&](void) -> std::expected<void, std::string> {
                             ++attempts;
                             if (attempts == 1)
                               return std::unexpected("transient publish "
                                                      "refusal");
                             return {};
                                 });
  ASSERT_TRUE(c->onFlagged());
  const auto first = c->onFlaggedTurnEnd();
  ASSERT_FALSE(first.has_value());
  EXPECT_EQ(ar.valueKindOf(marked), xanadu::ValueKind::Int64)
      << "a publish refusal still restores the live arena";

  // The mesh leaves the flag Raised: the next sole-queued turn runs the
  // whole window again.
  ASSERT_TRUE(c->onFlagged());
  EXPECT_TRUE(c->onFlaggedTurnEnd().has_value());
  EXPECT_EQ(attempts, 2);
  EXPECT_EQ(ar.asInt64(marked).value_or(0),
            static_cast<std::int64_t>(referent));
}

TEST(VagentP16Test, RestoreAllIsIdempotentAcrossEveryExit) {
  Image f;
  buildImage(f);
  auto s   = makeSession(f);
  auto &ar = s.arena();
  auto &co = s.core();

  const auto head = arenaHead(ar, s.binds, f.opA);
  auto codeTail   = s.agentCell;
  registerHead(ar, s.binds, head, codeTail);

  const auto referent = ar.makeCell();
  const auto marked   = markCell(ar, s.binds.dims.meshRelocation,
                                 static_cast<std::int64_t>(referent));
  std::optional<CellRef> tail;
  pushRefRecord(ar, s.binds, co.dims().step, head, tail,
                refRecord(ar, s.binds, marked));

  auto c = makeP16Ceremony(
      ar, co, s.binds,
      [&](void) -> std::expected<void, std::string> { return {}; });
  // No window open: the drain is a no-op.
  c->restoreAll();
  EXPECT_EQ(ar.valueKindOf(marked), xanadu::ValueKind::Int64);

  // An aborted window: re-kind, then drain twice — the second call
  // proves the kind guard.
  ASSERT_TRUE(c->onFlagged());
  ASSERT_EQ(ar.valueKindOf(marked), xanadu::ValueKind::OpHandle);
  c->restoreAll();
  EXPECT_EQ(ar.asInt64(marked).value_or(0),
            static_cast<std::int64_t>(referent));
  c->restoreAll();
  EXPECT_EQ(ar.asInt64(marked).value_or(0),
            static_cast<std::int64_t>(referent));

  // The window is reusable afterwards: flag → publish → restore.
  ASSERT_TRUE(c->onFlagged());
  EXPECT_TRUE(c->onFlaggedTurnEnd().has_value());
  EXPECT_EQ(ar.valueKindOf(marked), xanadu::ValueKind::Int64);
}

TEST(VagentP16Test, MissingRefCellVetoesTheFlag) {
  Image f;
  buildImage(f);
  auto s   = makeSession(f);
  auto &ar = s.arena();
  auto &co = s.core();

  const auto head = arenaHead(ar, s.binds, f.opA);
  auto codeTail   = s.agentCell;
  registerHead(ar, s.binds, head, codeTail);

  const auto referent = ar.makeCell();
  const auto marked   = markCell(ar, s.binds.dims.meshRelocation,
                                 static_cast<std::int64_t>(referent));
  std::optional<CellRef> tail;
  pushRefRecord(ar, s.binds, co.dims().step, head, tail,
                refRecord(ar, s.binds, marked));
  // A record with no +d.ref — the inventory is broken, so the flag
  // vetoes rather than checkpoint a silently-short inventory.
  const auto broken = ar.makeCell();
  pushRefRecord(ar, s.binds, co.dims().step, head, tail, broken);

  auto c = makeP16Ceremony(
      ar, co, s.binds,
      [&](void) -> std::expected<void, std::string> { return {}; });
  EXPECT_FALSE(c->onFlagged());
  EXPECT_EQ(ar.valueKindOf(marked), xanadu::ValueKind::Int64)
      << "the veto happens before any cell changes kind";
}

TEST(VagentP16Test, InstalledCopiesJoinTheSweep) {
  Image f;
  buildImage(f);
  auto s   = makeSession(f);
  auto &ar = s.arena();
  auto &co = s.core();

  // One head on the +d.code rank, one only reachable through the
  // installed-copy list — replay-installed copies hang off program-side
  // anchors a head-only sweep never reaches.
  const auto head = arenaHead(ar, s.binds, f.opA);
  auto codeTail   = s.agentCell;
  registerHead(ar, s.binds, head, codeTail);
  const auto copy = arenaHead(ar, s.binds, f.opB);

  const auto ref0     = ar.makeCell();
  const auto onHead   = markCell(ar, s.binds.dims.meshRelocation,
                                 static_cast<std::int64_t>(ref0));
  const auto ref1     = ar.makeCell();
  const auto onCopy   = markCell(ar, s.binds.dims.meshRelocation,
                                 static_cast<std::int64_t>(ref1));
  const auto ref2     = ar.makeCell();
  const auto unlisted = markCell(ar, s.binds.dims.meshRelocation,
                                 static_cast<std::int64_t>(ref2));

  std::optional<CellRef> tail;
  pushRefRecord(ar, s.binds, co.dims().step, head, tail,
                refRecord(ar, s.binds, onHead));
  tail.reset();
  pushRefRecord(ar, s.binds, co.dims().step, copy, tail,
                refRecord(ar, s.binds, onCopy));
  // A third head's record stays outside the inventory entirely — no
  // +d.code registration, not in installedCopies.
  const auto stray = arenaHead(ar, s.binds, f.opA);
  tail.reset();
  pushRefRecord(ar, s.binds, co.dims().step, stray, tail,
                refRecord(ar, s.binds, unlisted));

  auto c = makeP16Ceremony(
      ar, co, s.binds,
      [&](void) -> std::expected<void, std::string> { return {}; }, {copy});
  ASSERT_TRUE(c->onFlagged());
  EXPECT_EQ(ar.valueKindOf(onHead), xanadu::ValueKind::OpHandle);
  EXPECT_EQ(ar.valueKindOf(onCopy), xanadu::ValueKind::OpHandle)
      << "the installed copy's inventory is the sweep's second half";
  EXPECT_EQ(ar.valueKindOf(unlisted), xanadu::ValueKind::Int64);
}

TEST(VagentP16Test, ReifyRegeneratesOneMarkerPerRefCell) {
  Image f;
  buildImage(f);
  auto s   = makeSession(f);
  auto &ar = s.arena();
  auto &co = s.core();
  MetaRuntime meta(co, s.ctx->vm, s.binds);

  const auto copy = meta.reify(f.progHeadA);
  ASSERT_TRUE(copy.has_value()) << (copy ? "" : copy.error());

  // Every +d.refs record's marked cell carries exactly the marker edge
  // (`d.mesh.relocation` is single-valued, so presence is the count);
  // name-records carry none by definition of the arm split.
  std::size_t refs = 0;
  for (const auto marked : markedOnHead(ar, s.binds, co, *copy)) {
    const auto marker = ar.linked(marked, s.binds.dims.meshRelocation, Posward);
    ASSERT_TRUE(marker.has_value()) << "a ref cell missed its marker";
    EXPECT_EQ(ar.textOf(*marker), "cell");
    ++refs;
  }
  EXPECT_GT(refs, 0U) << "the copy's own recipe members join +d.refs";
}

/// The clauses a host symbol carries whose `+d.name` starts `patch:` —
/// the patch serial is a session draw, so tests key on the prefix, never
/// the number.
std::vector<CellRef> patchClauses(ArenaManifold &arena, CellRef host,
                                  DimRef clauseDim, DimRef name) {
  std::vector<CellRef> out;
  for (const auto c : zigzag::rankAfter(arena, host, clauseDim)) {
    const auto label = arena.linked(c, name, Posward);
    if (label && arena.textOf(*label).starts_with("patch:")) out.push_back(c);
  }
  return out;
}

/// Follow a cell's `+d.entry` chain to the first Int64-bearing cell —
/// the head-private op twin its recorded kind rode in on.
std::optional<std::int64_t> resolveOpInt(ArenaManifold &arena, CellRef at,
                                         DimRef entry) {
  for (std::size_t hops = 0; hops < 8; ++hops) {
    if (const auto v = arena.asInt64(at)) return *v;
    const auto next = arena.linked(at, entry, Posward);
    if (!next) return std::nullopt;
    at = *next;
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// The acceptance journey: learn → checkpoint → resume, twice
// ---------------------------------------------------------------------------

TEST(VagentP16Test, LearnCheckpointResumeLearnCheckpointResume) {
  Image f;
  buildImage(f);

  // -- Session 1: learn_plan over the image fold -----------------------
  auto s0   = makeSession(f);
  auto &a0  = s0.arena();
  auto &co0 = s0.core();
  MetaRuntime meta0(co0, s0.ctx->vm, s0.binds);
  const auto clause = meta0.learnPlan("boot", "main", f.progHeadA, f.progHeadB);
  ASSERT_TRUE(clause.has_value()) << (clause ? "" : clause.error());

  // The session-minted registered heads carry marked cells whose int64s
  // are *ephemeral* positions — the re-kind is what makes them real.
  auto heads0 = zigzag::rankAfter(a0, s0.agentCell, s0.binds.dims.code);
  ASSERT_FALSE(heads0.empty()) << "learn_plan registered no heads";
  bool sawEphemeral = false;
  for (const auto head : heads0)
    for (const auto marked : markedOnHead(a0, s0.binds, co0, head))
      if (const auto v = a0.asInt64(marked);
          v && zigzag::isEphemeral(static_cast<CellRef>(*v)))
        sawEphemeral = true;
  ASSERT_TRUE(sawEphemeral)
      << "fixture drift: no ephemeral positions to checkpoint";

  // Cycle 1's publish command IS the re-genesis: promote under the
  // re-kinded handles, file, rebuild, fresh context — bound the way the
  // adapter binds publishValue.
  std::optional<RegenesisResult> cycle;
  auto c1 = makeP16Ceremony(
      a0, co0, s0.binds, [&](void) -> std::expected<void, std::string> {
        auto c = regenesisCycle(f.store, a0, s0.agentCell, f.imageExtent);
        if (!c) return std::unexpected(c.error());
        cycle = std::move(*c);
        return {};
      });
  ASSERT_TRUE(c1->onFlagged()) << "the flag-time re-kind refused";
  ASSERT_TRUE(c1->onFlaggedTurnEnd().has_value());
  ASSERT_TRUE(cycle.has_value()) << "no publish command ran";

  // The live session restored: every marked cell reads Int64 again.
  for (const auto head : heads0)
    for (const auto marked : markedOnHead(a0, s0.binds, co0, head))
      EXPECT_EQ(a0.valueKindOf(marked), xanadu::ValueKind::Int64)
          << "a marked cell stayed re-kinded past the window";

  // -- Resume: genesis over the checkpoint's fold ----------------------
  const GenesisRoots roots{
      .programRoot = f.programRoot,
      .agentCell   = cycle->agentCell,
      .ownModule   = f.sysModule,
      .imageRecord = f.imageRecord,
      .imageExtent = f.imageExtent,
  };
  auto &a1  = cycle->context->arena;
  auto out1 = runGenesis(f.store, f.store, a1, cycle->context->core,
                         cycle->context->vm, roots);
  ASSERT_TRUE(out1.has_value()) << (out1 ? "" : out1.error());
  EXPECT_EQ(out1->patchesReplayed, 1U);
  EXPECT_EQ(out1->patchesDropped, 0U);
  EXPECT_GT(out1->refRecordsRebound, 0U)
      << "no OpHandle writeback ran — the checkpoint's handles never "
         "reached the fold";

  // The resumed heads' marked cells all rehydrated to *persistent*
  // positions — the discriminator against the stale-int64 flow.
  for (const auto head :
       zigzag::rankAfter(a1, cycle->agentCell, out1->binds.dims.code))
    for (const auto marked :
         markedOnHead(a1, out1->binds, cycle->context->core, head)) {
      const auto v = a1.asInt64(marked);
      ASSERT_TRUE(v.has_value()) << "a marked cell is not Int64-kind";
      // Verbatim sign fields (±1) are non-positions and fine; an
      // arena-local position would have resumed with its ephemeral
      // bit still set — the re-kind's whole point is that none do.
      EXPECT_FALSE(zigzag::isEphemeral(static_cast<CellRef>(*v)))
          << "a re-kinded position resumed as a stale ephemeral";
    }

  // The replayed clause resolves to the recorded op kind — the isomorph
  // is real: its recipe replayed positions, not delegation fall-through.
  const auto &dims1   = cycle->context->core.dims();
  const auto clauses1 = patchClauses(a1, f.hostSym, dims1.clause, dims1.name);
  ASSERT_EQ(clauses1.size(), 1U) << "the patch clause never installed";
  const auto clauseOnRank = clauses1.front();
  const auto grabCopy     = a1.linked(clauseOnRank, dims1.grab, Posward);
  ASSERT_TRUE(grabCopy.has_value());
  EXPECT_EQ(resolveOpInt(a1, *grabCopy, dimAt(f, "d.entry")).value_or(-1),
            static_cast<std::int64_t>(OpcodeKind::Nop));

  // -- Session 2 on the resumed context: learn_where, checkpoint again -
  auto binds1 = mintMetaBindSet(a1, cycle->context->core, f.programRoot,
                                cycle->agentCell);
  ASSERT_TRUE(binds1.has_value()) << (binds1 ? "" : binds1.error());
  binds1->ownModule = f.sysModule;
  binds1->lineage   = MetaLineage{
      .programRef  = f.programRoot,
      .imageExtent = f.imageExtent,
  };
  MetaRuntime meta1(cycle->context->core, cycle->context->vm, *binds1);
  const auto replaced = meta1.learnWhere(clauseOnRank, f.progHeadB);
  ASSERT_TRUE(replaced.has_value()) << (replaced ? "" : replaced.error());

  // The second ceremony's inventory is the resumed session's: +d.code
  // heads plus genesis's own install set.
  std::optional<RegenesisResult> cycle2;
  auto c2 = makeP16Ceremony(
      a1, cycle->context->core, *binds1,
      [&](void) -> std::expected<void, std::string> {
        auto c = regenesisCycle(f.store, a1, cycle->agentCell, f.imageExtent);
        if (!c) return std::unexpected(c.error());
        cycle2 = std::move(*c);
        return {};
      },
      out1->installedCopies);
  ASSERT_TRUE(c2->onFlagged());
  ASSERT_TRUE(c2->onFlaggedTurnEnd().has_value());
  ASSERT_TRUE(cycle2.has_value());

  auto &a2 = cycle2->context->arena;
  const GenesisRoots roots2{
      .programRoot = f.programRoot,
      .agentCell   = cycle2->agentCell,
      .ownModule   = f.sysModule,
      .imageRecord = f.imageRecord,
      .imageExtent = f.imageExtent,
  };
  auto out2 = runGenesis(f.store, f.store, a2, cycle2->context->core,
                         cycle2->context->vm, roots2);
  ASSERT_TRUE(out2.has_value()) << (out2 ? "" : out2.error());
  EXPECT_EQ(out2->patchesReplayed, 2U)
      << "both patch records must replay across the second checkpoint";
  EXPECT_EQ(out2->patchesDropped, 0U);
  EXPECT_GT(out2->refRecordsRebound, 0U);

  // patch:N's clause re-installed and patch:M's learn_where re-pointed
  // the *same* clause's +d.grab — the where-copy resolves to frag-b's
  // recorded op kind.
  const auto &dims2   = cycle2->context->core.dims();
  const auto clauses2 = patchClauses(a2, f.hostSym, dims2.clause, dims2.name);
  ASSERT_EQ(clauses2.size(), 1U)
      << "learn_where re-points a clause; it never installs a second";
  const auto grab2 = a2.linked(clauses2.front(), dims2.grab, Posward);
  ASSERT_TRUE(grab2.has_value());
  EXPECT_EQ(resolveOpInt(a2, *grab2, dimAt(f, "d.entry")).value_or(-1),
            static_cast<std::int64_t>(OpcodeKind::Return))
      << "the where-copy's entry resolves to frag-b's op kind";

  // A permuted lineage still refuses at (a) — the checkpoint's band is
  // the comparand.
  Store foreign;
  auto at = foreign.sliceGenesis({});
  for (std::size_t i = 0; i < f.imageExtent; ++i)
    at = foreign.makeCell(at, "foreign");
  const auto refused = runGenesis(f.store, foreign, a2, cycle2->context->core,
                                  cycle2->context->vm, roots2);
  EXPECT_FALSE(refused.has_value())
      << "a foreign band of the same extent must refuse the bind";
}

} // namespace
