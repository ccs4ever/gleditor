// Phase-5 genesis ceremony tests (design/vagent-metaprogramming-
// implementation.md :421-479; spec §6.5, §9): the (a)-(e) arms in order —
// the pairwise lineage bind, the dim sweep + P15 re-registration, the bind
// set, the ordinal d.patch replay, and the post-replay +d.refs rehydration.
//
// Two fixtures cover the exit criteria:
//
//  - `DurableProgram` authors the *durable* state directly on a Store —
//    persistent-position recipes, patch records, registered heads — the
//    same well-formed graph a session + promote + P16's checkpoint
//    re-kind would produce (phase-6 re-kinds Int64 slots into OpHandles;
//    the durable records this phase replays are name data either way).
//    This is the deep arm-by-arm fixture.
//
//  - `ProgramImage` is program-side cells only; the agent cell, its
//    d.code registrations and its d.patch records arrive through a real
//    session's learn_* + regenesisCycle promote — the end-to-end path
//    whose replayed install copies are isomorphs: a session-minted
//    recipe's int64 positions are arena-local and stay stale until
//    P16's re-kind, so the copy's +d.entry chain ends on its own twin
//    op — a fresh cell carrying the recorded kind verbatim.
#include <gtest/gtest.h>

#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/vagent/genesis.hpp"
#include "common/xanadu/vagent/lineage.hpp"
#include "common/xanadu/vagent/meta.hpp"
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
using zigzag::DimVector;
using zigzag::Negward;
using zigzag::noCell;
using zigzag::Posward;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::WorkerContext;

/// The fixed filing list: the sibling's three name arrays, verbatim.
std::vector<std::string_view> fixedDimNames() {
  std::vector<std::string_view> out;
  for (const auto name : kGenesisDimensionNames) out.push_back(name);
  for (const auto name : kMetaDimensionNames) out.push_back(name);
  for (const auto name : kAgentDimensionNames) out.push_back(name);
  return out;
}

/// Store-side authoring: threads the head version and hands back the
/// cell ref every make* produced. setLink's returned version indexes
/// the link op, never a cell — captures happen only after make calls.
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
  CellRef opHandle(std::uint32_t target, MicroversionId &at) {
    at             = store.makeOpHandle(at, target, "");
    const auto ref = store.cellRefOf(at);
    EXPECT_TRUE(ref.has_value()) << "makeOpHandle produced no cell";
    return ref.value_or(noCell);
  }
  void link(CellRef from, DimRef dim, CellRef to, MicroversionId &at) {
    at = store.setLink(at, from, dim, Posward, to);
  }
};

/// One `+d.vars → key-record → +d.values → value` field, tail-chained
/// the way rankTail appends.
void storeField(Author &a, CellRef &varsTail, std::string_view key,
                CellRef value, MicroversionId &at,
                const std::map<std::string, DimRef> &dim) {
  const auto record = a.cell(key, at);
  a.link(record, dim.at("d.values"), value, at);
  a.link(varsTail, dim.at("d.vars"), record, at);
  varsTail = record;
}

/// A `mintPatch`-shaped durable patch record (meta.cpp:1614-1660): the
/// patch's own fields on its `d.vars`, the verb/target/parent on the
/// `+d.values` descriptor's, the head names on `+d.patch-head`.
struct PatchSpec {
  std::string label;
  std::int64_t ordinal;
  std::string verb;
  std::string target;
  std::optional<std::string> parent;
  std::optional<std::string> alias;
  std::optional<std::int64_t> priority;
  std::vector<std::string> heads;
};

// ---------------------------------------------------------------------------
// The durable fixture
// ---------------------------------------------------------------------------

struct DurableProgram {
  Store store;
  MicroversionId at;
  std::map<std::string, DimRef> dim;

  // Roots the ceremony binds.
  CellRef programRoot{noCell}, agentCell{noCell}, ownModule{noCell},
      imageRecord{noCell};
  // Program side.
  CellRef sysModule{noCell}, libModule{noCell}, hostSym{noCell},
      methodSym{noCell}, declaredMethod{noCell}, declaredClause{noCell};
  CellRef progTestHead{noCell}, progBodyHead{noCell}, progMethodHead{noCell},
      declaredMethodHead{noCell};
  CellRef progTestOp{noCell}, progBodyOp{noCell}, progMethodOp{noCell};
  // Registered heads by label, and their private entry ops by label.
  std::map<std::string, CellRef> regHead, regOp;
  // The patch rank in append order.
  std::vector<CellRef> patches;
  // The dead patch (a replay refusal target) and its authored label.
  CellRef deadPatch{noCell};
  // (e) witness slots.
  CellRef runSlot{noCell}, callSlot{noCell}, callInstalledSlot{noCell},
      plainSlot{noCell}, handleSlot{noCell}, intSlot{noCell};
  CellRef anchorMember{noCell}, customAnchorMember{noCell};
  std::int64_t intSlotValue   = 0;
  std::int64_t authoredSerial = 0;
  std::uint32_t imageExtent   = 0;
};

DimRef dimAt(const std::map<std::string, DimRef> &dim, std::string_view name) {
  return dim.at(std::string{name});
}

template <typename F> DimRef dimAt(const F &f, std::string_view name) {
  return dimAt(f.dim, name);
}

/// Mint a registered head: `+d.entry → op`, `+d.name → label`, and a
/// self-describing `+d.recipe` in persistent coordinates — the
/// [literal(H), literal(O), tuple(H, dim, +, O)] shape fragment.cpp's
/// emission records and meta.cpp's reify parses. The dim-anchor member
/// deliberately carries a *stale* position — the (e) dim arm must
/// re-resolve it from the filed name.
CellRef mintRegisteredHead(DurableProgram &f, Author &a, std::string_view label,
                           CellRef op, CellRef &codeTail,
                           const std::optional<std::string_view> &extraDim,
                           CellRef *anchorOut      = nullptr,
                           CellRef *extraAnchorOut = nullptr) {
  const auto head = a.cell("", f.at);
  a.link(head, dimAt(f, "d.entry"), op, f.at);
  a.link(head, dimAt(f, "d.name"), a.cell(label, f.at), f.at);

  const auto mLitH  = a.scalar(static_cast<std::int64_t>(head), f.at);
  const auto mLitO  = a.scalar(static_cast<std::int64_t>(op), f.at);
  const auto mFrom  = a.scalar(static_cast<std::int64_t>(head), f.at);
  const auto anchor = a.scalar(static_cast<std::int64_t>(f.programRoot), f.at);
  a.link(anchor, dimAt(f, "d.name"), a.cell("d.entry", f.at), f.at);
  const auto mDir = a.scalar(1, f.at);
  const auto mTo  = a.scalar(static_cast<std::int64_t>(op), f.at);
  a.link(head, dimAt(f, "d.recipe"), mLitH, f.at);
  a.link(mLitH, dimAt(f, "d.step"), mLitO, f.at);
  a.link(mLitO, dimAt(f, "d.step"), mFrom, f.at);
  a.link(mFrom, dimAt(f, "d.step"), anchor, f.at);
  a.link(anchor, dimAt(f, "d.step"), mDir, f.at);
  a.link(mDir, dimAt(f, "d.step"), mTo, f.at);
  if (anchorOut) *anchorOut = anchor;

  const CellRef tail = mTo;
  if (extraDim) {
    const auto eFrom = a.scalar(static_cast<std::int64_t>(head), f.at);
    const auto eAnchor =
        a.scalar(static_cast<std::int64_t>(f.programRoot), f.at);
    a.link(eAnchor, dimAt(f, "d.name"), a.cell(*extraDim, f.at), f.at);
    const auto eDir = a.scalar(1, f.at);
    const auto eTo  = a.scalar(static_cast<std::int64_t>(op), f.at);
    a.link(tail, dimAt(f, "d.step"), eFrom, f.at);
    a.link(eFrom, dimAt(f, "d.step"), eAnchor, f.at);
    a.link(eAnchor, dimAt(f, "d.step"), eDir, f.at);
    a.link(eDir, dimAt(f, "d.step"), eTo, f.at);
    if (extraAnchorOut) *extraAnchorOut = eAnchor;
  }

  a.link(codeTail, dimAt(f, "d.code"), head, f.at);
  codeTail = head;
  return head;
}

/// One `+d.refs` record on a head's chain: `+d.ref → slot`, optional
/// `+d.name → text`, optional `+d.grab → role`.
CellRef mintRefRecord(DurableProgram &f, Author &a, CellRef slot,
                      const std::optional<std::string_view> &name,
                      const std::optional<std::string_view> &role) {
  const auto record = a.cell("", f.at);
  a.link(record, dimAt(f, "d.ref"), slot, f.at);
  if (name) a.link(record, dimAt(f, "d.name"), a.cell(*name, f.at), f.at);
  if (role) a.link(record, dimAt(f, "d.grab"), a.cell(*role, f.at), f.at);
  return record;
}

CellRef mintPatchRecord(DurableProgram &f, Author &a, const PatchSpec &spec,
                        CellRef &patchTail) {
  const auto patch = a.cell("", f.at);
  a.link(patch, dimAt(f, "d.name"), a.cell(spec.label, f.at), f.at);
  const auto descriptor = a.cell("", f.at);
  a.link(patch, dimAt(f, "d.values"), descriptor, f.at);
  CellRef varsTail = descriptor;
  storeField(a, varsTail, "target", a.cell(spec.target, f.at), f.at, f.dim);
  storeField(a, varsTail, "verb", a.cell(spec.verb, f.at), f.at, f.dim);
  if (spec.parent)
    storeField(a, varsTail, "parent", a.cell(*spec.parent, f.at), f.at, f.dim);
  CellRef ownTail = patch;
  storeField(a, ownTail, "ordinal", a.scalar(spec.ordinal, f.at), f.at, f.dim);
  if (spec.alias)
    storeField(a, ownTail, "alias", a.cell(*spec.alias, f.at), f.at, f.dim);
  if (spec.priority)
    storeField(a, ownTail, "priority", a.scalar(*spec.priority, f.at), f.at,
               f.dim);
  std::optional<CellRef> chainTail;
  for (const auto &name : spec.heads) {
    const auto record = a.cell("", f.at);
    a.link(record, dimAt(f, "d.values"), a.cell(name, f.at), f.at);
    if (!chainTail)
      a.link(patch, dimAt(f, "d.patch-head"), record, f.at);
    else
      a.link(*chainTail, dimAt(f, "d.step"), record, f.at);
    chainTail = record;
  }
  a.link(patchTail, dimAt(f, "d.patch"), patch, f.at);
  patchTail = patch;
  return patch;
}

/// The deep fixture: dims filed on the store's d.dims rank, then the
/// whole durable state — program image, registered heads with
/// persistent-coordinate recipes, patch records, the serial counter and
/// the image record — authored as persistent cells. `Store` is
/// non-movable — the caller owns the fixture, this fills it.
void buildDurableProgram(DurableProgram &f) {
  Author a{f.store};
  f.at        = a.store.sliceGenesis({});
  auto filing = fileDimensionNames(a.store, f.at, fixedDimNames());
  EXPECT_TRUE(filing.has_value()) << (filing ? "" : filing.error());
  for (const auto name : fixedDimNames())
    f.dim[std::string{name}] =
        filing->fold.dimensionNamed(name, a.store).value_or(noCell);
  f.at = filing->head;

  // ---- program image -------------------------------------------------
  f.programRoot = a.cell("program-root", f.at);
  f.sysModule   = a.cell("sys", f.at);
  f.libModule   = a.cell("lib", f.at);
  a.link(f.programRoot, dimAt(f, "d.stdlib"), f.sysModule, f.at);
  a.link(f.sysModule, dimAt(f, "d.stdlib"), f.libModule, f.at);
  f.ownModule = f.sysModule;

  f.progTestOp = a.scalar(static_cast<std::int64_t>(OpcodeKind::Nop), f.at);
  f.progBodyOp = a.scalar(static_cast<std::int64_t>(OpcodeKind::Return), f.at);
  f.progMethodOp = a.scalar(static_cast<std::int64_t>(OpcodeKind::Halt), f.at);

  const auto progHead = [&](std::string_view label, CellRef op) {
    const auto head = a.cell("", f.at);
    a.link(head, dimAt(f, "d.entry"), op, f.at);
    a.link(head, dimAt(f, "d.name"), a.cell(label, f.at), f.at);
    return head;
  };
  f.progTestHead   = progHead("prog-test", f.progTestOp);
  f.progBodyHead   = progHead("prog-body", f.progBodyOp);
  f.progMethodHead = progHead("prog-method", f.progMethodOp);

  // "boot" — the host symbol on the agent's own module, carrying the
  // declared "main" clause the learn_plan alias replaces. Its
  // +d.values is the symbol's head — the program-side anchor that
  // keeps "prog-test" reachable to code_named once the clause that
  // also names it leaves the rank.
  f.hostSym = a.cell("boot", f.at);
  a.link(f.sysModule, dimAt(f, "d.vars"), f.hostSym, f.at);
  a.link(f.hostSym, dimAt(f, "d.values"), f.progTestHead, f.at);
  f.declaredClause = a.cell("", f.at);
  a.link(f.declaredClause, dimAt(f, "d.name"), a.cell("main", f.at), f.at);
  a.link(f.declaredClause, dimAt(f, "d.grab"), f.progTestHead, f.at);
  a.link(f.declaredClause, dimAt(f, "d.spin"), f.progBodyHead, f.at);
  a.link(f.hostSym, dimAt(f, "d.clause"), f.declaredClause, f.at);

  // "Owner.op" on lib — the call-record's program-side target.
  f.methodSym = a.cell("Owner.op", f.at);
  a.link(f.libModule, dimAt(f, "d.vars"), f.methodSym, f.at);
  a.link(f.methodSym, dimAt(f, "d.values"), f.progMethodHead, f.at);

  // A declared "Agent.skill" already on lib's rank — graft_method's
  // replay unlink-first must evict it before appending the learned
  // one. Its +d.values twin is a *distinct* head: -d.values evicts
  // links, so two symbols naming the same head would leave one of
  // them headless.
  f.declaredMethod     = a.cell("Agent.skill", f.at);
  f.declaredMethodHead = progHead("decl-method", f.progBodyOp);
  a.link(f.methodSym, dimAt(f, "d.vars"), f.declaredMethod, f.at);
  a.link(f.declaredMethod, dimAt(f, "d.values"), f.declaredMethodHead, f.at);

  // ---- agent cell, registered heads, refs records --------------------
  f.agentCell      = a.cell("agent", f.at);
  CellRef codeTail = f.agentCell;

  const auto regHead = [&](std::string_view label, CellRef op) {
    return mintRegisteredHead(f, a, label, op, codeTail, std::nullopt);
  };
  f.regOp["reify:src:test:0"] =
      a.scalar(static_cast<std::int64_t>(OpcodeKind::Nop), f.at);
  f.regHead["reify:src:test:0"] = mintRegisteredHead(
      f, a, "reify:src:test:0", f.regOp["reify:src:test:0"], codeTail,
      std::string_view{"d.custom-ref"}, &f.anchorMember, &f.customAnchorMember);
  const std::vector<std::pair<std::string, OpcodeKind>> rest = {
      {"reify:src:body:1", OpcodeKind::Return},
      {"reify:src:where:2", OpcodeKind::Nop},
      {"reify:src:skill:3", OpcodeKind::Halt},
      {"reify:src:guard:4", OpcodeKind::Nop},
      {"reify:src:hbody:5", OpcodeKind::Return},
  };
  for (const auto &[label, kind] : rest) {
    f.regOp[label]   = a.scalar(static_cast<std::int64_t>(kind), f.at);
    f.regHead[label] = regHead(label, f.regOp[label]);
  }

  // The +d.refs inventory on the test head: one record per arm.
  f.runSlot           = a.scalar(-3, f.at);
  f.callSlot          = a.scalar(-5, f.at);
  f.callInstalledSlot = a.scalar(-7, f.at);
  f.plainSlot         = a.scalar(-9, f.at);
  const auto head0    = f.regHead.at("reify:src:test:0");
  std::optional<CellRef> refsTail;
  const auto pushRef = [&](CellRef record) {
    if (!refsTail)
      a.link(head0, dimAt(f, "d.refs"), record, f.at);
    else
      a.link(*refsTail, dimAt(f, "d.step"), record, f.at);
    refsTail = record;
  };
  pushRef(mintRefRecord(f, a, f.runSlot, std::string_view{"prog-test"},
                        std::string_view{"run"}));
  pushRef(mintRefRecord(f, a, f.callSlot, std::string_view{"lib:Owner.op"},
                        std::string_view{"call"}));
  pushRef(mintRefRecord(f, a, f.callInstalledSlot,
                        std::string_view{"lib:Agent.skill"},
                        std::string_view{"call"}));
  pushRef(mintRefRecord(f, a, f.plainSlot, std::string_view{"reify:src:body:1"},
                        std::nullopt));
  f.handleSlot = a.opHandle(static_cast<std::uint32_t>(f.progBodyOp), f.at);
  pushRef(mintRefRecord(f, a, f.handleSlot, std::nullopt, std::nullopt));
  f.intSlotValue = 4242;
  f.intSlot      = a.scalar(f.intSlotValue, f.at);
  pushRef(mintRefRecord(f, a, f.intSlot, std::nullopt, std::nullopt));

  // ---- the durable counter and the patch EDL -------------------------
  f.authoredSerial = 9;
  CellRef ownVars  = f.agentCell;
  storeField(a, ownVars, "meta.serial", a.scalar(f.authoredSerial, f.at), f.at,
             f.dim);

  CellRef patchTail = f.agentCell;
  f.patches.push_back(mintPatchRecord(f, a,
                                      PatchSpec{
                                          .label   = "patch:0",
                                          .ordinal = 0,
                                          .verb    = "learn_plan",
                                          .target  = "patch:0",
                                          .parent  = "boot",
                                          .alias   = "main",
                                          .heads =
                                              {
                                                  "reify:src:test:0",
                                                  "reify:src:body:1",
                                              },
                                      },
                                      patchTail));
  f.patches.push_back(mintPatchRecord(f, a,
                                      PatchSpec{
                                          .label   = "patch:1",
                                          .ordinal = 1,
                                          .verb    = "learn_where",
                                          .target  = "patch:0",
                                          .heads =
                                              {
                                                  "reify:src:where:2",
                                              },
                                      },
                                      patchTail));
  f.patches.push_back(mintPatchRecord(f, a,
                                      PatchSpec{
                                          .label   = "patch:2",
                                          .ordinal = 2,
                                          .verb    = "learn_method",
                                          .target  = "lib:Agent.skill",
                                          .parent  = "lib",
                                          .heads =
                                              {
                                                  "reify:src:skill:3",
                                              },
                                      },
                                      patchTail));
  f.patches.push_back(mintPatchRecord(f, a,
                                      PatchSpec{
                                          .label    = "patch:3",
                                          .ordinal  = 3,
                                          .verb     = "learn_handler",
                                          .target   = "patch:3",
                                          .parent   = "boot",
                                          .alias    = "tick",
                                          .priority = std::int64_t{7},
                                          .heads =
                                              {
                                                  "reify:src:guard:4",
                                                  "reify:src:hbody:5",
                                              },
                                      },
                                      patchTail));
  // A patch whose host never resolves — the dead-drop arm.
  f.deadPatch = mintPatchRecord(f, a,
                                PatchSpec{
                                    .label   = "patch:4",
                                    .ordinal = 4,
                                    .verb    = "learn_plan",
                                    .target  = "patch:4",
                                    .parent  = "ghost",
                                    .alias   = "ghost-alias",
                                    .heads =
                                        {
                                            "reify:src:test:0",
                                            "reify:src:body:1",
                                        },
                                },
                                patchTail);
  f.patches.push_back(f.deadPatch);

  // ---- the image record ----------------------------------------------
  f.imageRecord   = a.cell("image-record", f.at);
  CellRef imgVars = f.imageRecord;
  storeField(a, imgVars, "floor",
             a.scalar(static_cast<std::int64_t>(f.programRoot), f.at), f.at,
             f.dim);
  const auto opsRecord = a.cell("ops", f.at);
  const auto opsHead   = a.cell("", f.at);
  a.link(opsRecord, dimAt(f, "d.values"), opsHead, f.at);
  a.link(imgVars, dimAt(f, "d.vars"), opsRecord, f.at);
  const std::vector<CellRef> programOps = {
      f.progTestOp,
      f.progBodyOp,
      f.progMethodOp,
  };
  CellRef opsTail = opsHead;
  for (const auto op : programOps) {
    const auto member = a.scalar(static_cast<std::int64_t>(op), f.at);
    a.link(opsTail, dimAt(f, "d.step"), member, f.at);
    opsTail = member;
  }

  f.imageExtent = static_cast<std::uint32_t>(a.store.segmentedOps().size());
}

/// The fresh bundle over a fixture's fold, and the roots record the
/// ceremony binds.
struct GenesisContext {
  std::unique_ptr<zigzag::Manifold> fold;
  std::unique_ptr<WorkerContext> context;
  GenesisRoots roots;
};

GenesisContext freshContext(Store &store, const DurableProgram &f) {
  GenesisContext out;
  out.fold =
      std::make_unique<zigzag::Manifold>(store.rebuildManifold(store.latest()));
  out.context = std::make_unique<WorkerContext>(out.fold.get(), &store);
  out.roots   = GenesisRoots{
      .programRoot = f.programRoot,
      .agentCell   = f.agentCell,
      .ownModule   = f.ownModule,
      .imageRecord = f.imageRecord,
      .imageExtent = f.imageExtent,
  };
  return out;
}

CellRef clauseLabelled(zigzag::ArenaManifold &arena, CellRef host,
                       DimRef clause, DimRef name, std::string_view label) {
  for (const auto c : zigzag::rankAfter(arena, host, clause)) {
    const auto labelCell = arena.linked(c, name, Posward);
    if (labelCell && arena.textOf(*labelCell) == label) return c;
  }
  return noCell;
}

// ---------------------------------------------------------------------------
// (a) — the comparand alone
// ---------------------------------------------------------------------------

TEST(VagentGenesisTest, LineageBandSelfVerifiesAndForeignRefuses) {
  DurableProgram f;
  buildDurableProgram(f);
  ASSERT_GT(f.imageExtent, 0U);
  // Self: the kept and received spools are the same band.
  EXPECT_TRUE(verifyLineageBand(f.store, f.store, f.imageExtent).has_value());

  // A shorter received band is a missing-prefix refusal.
  Store shortStore;
  auto at = shortStore.sliceGenesis({});
  at      = shortStore.makeCell(at, "foreign-cell");
  const auto shortResult =
      verifyLineageBand(f.store, shortStore, f.imageExtent);
  ASSERT_FALSE(shortResult.has_value());

  // A foreign band of the same length refuses at the first differing
  // operation — a permutation/merge never verifies.
  Store foreign;
  at = foreign.sliceGenesis({});
  for (std::size_t i = 0; i < f.imageExtent; ++i)
    at = foreign.makeCell(at, "foreign");
  const auto foreignResult = verifyLineageBand(f.store, foreign, f.imageExtent);
  ASSERT_FALSE(foreignResult.has_value());
}

// ---------------------------------------------------------------------------
// The full ceremony over the durable fixture
// ---------------------------------------------------------------------------

TEST(VagentGenesisTest, FullCeremonyReplaysMutations) {
  DurableProgram f;
  buildDurableProgram(f);
  auto gc  = freshContext(f.store, f);
  auto &ar = gc.context->arena;

  auto outcome = runGenesis(f.store, f.store, ar, gc.context->core,
                            gc.context->vm, gc.roots);
  ASSERT_TRUE(outcome.has_value()) << (outcome ? "" : outcome.error());

  // (a): the whole band compared pairwise.
  EXPECT_EQ(outcome->opsCompared, f.imageExtent);
  // (b): the recorded enumeration registered — no scan.
  EXPECT_EQ(outcome->opsRegistered, 3U);
  EXPECT_TRUE(std::ranges::find(outcome->dimsMinted, "d.custom-ref") !=
              outcome->dimsMinted.end())
      << "the recipe's unfiled dim name must mint through the sweep";
  // (d): four live verbs replayed, the dead one dropped.
  EXPECT_EQ(outcome->patchesReplayed, 4U);
  EXPECT_EQ(outcome->patchesDropped, 1U);
  EXPECT_EQ(outcome->installedCopies.size(), 6U);
  // (e): all four name-records, the anchors, the lone OpHandle.
  EXPECT_GE(outcome->nameRecordsRebound, 4U);
  EXPECT_GE(outcome->dimRecordsRebound, 7U);
  EXPECT_EQ(outcome->refRecordsRebound, 1U);

  const auto &dims = gc.context->core.dims();
  const auto entry = dimAt(f, "d.entry");
  const auto grab  = dims.grab;
  const auto spin  = dims.spin;
  const auto name  = dims.name;

  // ---- (c): the bind set --------------------------------------------
  EXPECT_EQ(outcome->binds.programRoot, f.programRoot);
  EXPECT_EQ(outcome->binds.agentCell, f.agentCell);
  ASSERT_TRUE(outcome->binds.lineage.has_value());
  const auto lineage = outcome->binds.lineage.value_or(MetaLineage{});
  EXPECT_EQ(lineage.programRef, f.programRoot);
  EXPECT_EQ(lineage.imageExtent, f.imageExtent);

  // ---- (d): learn_plan — the clause and the alias replacement -------
  const auto patch0 =
      clauseLabelled(ar, f.hostSym, dims.clause, name, "patch:0");
  ASSERT_NE(patch0, noCell) << "patch:0's clause is not on the host rank";
  // The declared "main" clause yielded to the learned replacement —
  // verbatim alias dedupe on the install rank.
  EXPECT_EQ(clauseLabelled(ar, f.hostSym, dims.clause, name, "main"), noCell)
      << "the declared clause stayed on the rank through replay";
  // patch:1's learn_where re-pointed the same clause's grab.
  const auto grabCopy = ar.linked(patch0, grab, Posward);
  ASSERT_TRUE(grabCopy.has_value());
  const auto spinCopy = ar.linked(patch0, spin, Posward);
  ASSERT_TRUE(spinCopy.has_value());
  EXPECT_NE(grabCopy.value_or(noCell), spinCopy.value_or(noCell))
      << "grab and spin take distinct copies";
  // The where-copy's +d.entry chain ends on its *twin* op — reify's
  // isomorph copies the recorded entry edge onto a fresh op cell
  // carrying the recorded kind verbatim, so the copy is a runnable
  // fragment whose graph never leans on the registration's cells.
  auto current     = grabCopy.value_or(noCell);
  std::size_t hops = 0;
  for (; hops < 8; ++hops) {
    if (ar.asInt64(current)) break;
    const auto next = ar.linked(current, entry, Posward);
    if (!next) break;
    current = *next;
  }
  EXPECT_LT(hops, 8U) << "the where-copy's +d.entry chain never reached an op";
  if (hops < 8)
    EXPECT_EQ(ar.asInt64(current).value_or(-1),
              static_cast<std::int64_t>(OpcodeKind::Nop))
        << "reify:src:where:2's entry resolves to a twin of its recorded op";

  // ---- (d): learn_handler — clause plus the priority record ---------
  const auto patch3 =
      clauseLabelled(ar, f.hostSym, dims.clause, name, "patch:3");
  ASSERT_NE(patch3, noCell);
  bool foundPriority = false;
  for (const auto rec : zigzag::rankAfter(ar, patch3, dims.vars)) {
    if (ar.textOf(rec) == "priority") {
      const auto v = ar.linked(rec, dims.values, Posward);
      ASSERT_TRUE(v.has_value());
      EXPECT_EQ(ar.asInt64(v.value_or(noCell)).value_or(-1), 7);
      foundPriority = true;
    }
  }
  EXPECT_TRUE(foundPriority) << "learn_handler's priority record missing";

  // ---- (d): learn_method — graft_method's unlink-first --------------
  std::vector<CellRef> libSyms;
  for (const auto s : zigzag::rankAfter(ar, f.libModule, dims.vars))
    libSyms.push_back(s);
  ASSERT_EQ(libSyms.size(), 2U)
      << "the declared Agent.skill plus the learned one would be three";
  EXPECT_EQ(ar.textOf(libSyms[0]), "Owner.op");
  EXPECT_EQ(ar.textOf(libSyms[1]), "Agent.skill");
  EXPECT_NE(libSyms[1], f.declaredMethod)
      << "the replay must mint a fresh symbol, not resurrect the declared";
  const auto skillValue = ar.linked(libSyms[1], dims.values, Posward);
  ASSERT_TRUE(skillValue.has_value());
  EXPECT_TRUE(
      ar.linked(skillValue.value_or(noCell), entry, Posward).has_value())
      << "the method's installed head carries no +d.entry";

  // ---- (d): the dead patch's tombstone ------------------------------
  bool deadOnRank = false;
  for (const auto p :
       zigzag::rankAfter(ar, f.agentCell, outcome->binds.dims.patch))
    if (p == f.deadPatch) deadOnRank = true;
  EXPECT_FALSE(deadOnRank) << "the refused patch stayed on the d.patch rank";
  bool tombstone = false;
  for (const auto rec : zigzag::rankAfter(ar, f.agentCell, dims.vars))
    if (ar.textOf(rec) == "fault:patch:4") tombstone = true;
  EXPECT_TRUE(tombstone) << "no fault:patch:4 d.vars record was minted";

  // ---- (e): the name-records ----------------------------------------
  EXPECT_EQ(ar.asInt64(f.runSlot).value_or(-1),
            static_cast<std::int64_t>(f.progTestHead))
      << "run-role record must bind Int64(head)";
  EXPECT_EQ(ar.asInt64(f.callSlot).value_or(-1),
            static_cast<std::int64_t>(f.progMethodOp))
      << "call-role record must bind Int64(entryOp)";
  // The replay-installed method's entry op — resolvable only because
  // (e) runs after (d): the symbol was minted by the replay itself.
  // The rebind lands on the installed copy's graph root — the twin op
  // of reify:src:skill:3's recorded kind, never the registration's
  // own cell.
  const auto reboundCall = ar.asInt64(f.callInstalledSlot).value_or(-1);
  ASSERT_TRUE(ar.contains(static_cast<CellRef>(reboundCall)))
      << "the call slot bound a cell outside the arena";
  EXPECT_EQ(ar.asInt64(static_cast<CellRef>(reboundCall)).value_or(-1),
            static_cast<std::int64_t>(OpcodeKind::Halt))
      << "the installed method's entry twin carries the recorded kind";
  EXPECT_EQ(ar.asInt64(f.plainSlot).value_or(-1),
            static_cast<std::int64_t>(f.regHead["reify:src:body:1"]))
      << "a role-less name-record binds Int64(head) verbatim";

  // ---- (e): the ref-records ------------------------------------------
  EXPECT_EQ(ar.asInt64(f.handleSlot).value_or(-1),
            static_cast<std::int64_t>(f.progBodyOp))
      << "OpHandle slots restore as Int64(*handleTarget)";
  EXPECT_EQ(ar.asInt64(f.intSlot).value_or(-1), f.intSlotValue)
      << "an emission-fixed Int64 slot stays verbatim — P16's re-kind "
         "is phase 6";

  // ---- (e): the dim-field recipe members -----------------------------
  EXPECT_EQ(ar.asInt64(f.anchorMember).value_or(-1),
            static_cast<std::int64_t>(entry))
      << "the stale anchor must re-resolve to the filed d.entry cell";
  const auto customDim = ar.dimensionNamed("d.custom-ref");
  ASSERT_TRUE(customDim.has_value());
  EXPECT_EQ(ar.asInt64(f.customAnchorMember).value_or(-1),
            static_cast<std::int64_t>(customDim.value_or(noCell)))
      << "the unfiled-name anchor re-resolves to the minted dim";

  // ---- (b): P15's opcode map is the recorded enumeration's ----------
  EXPECT_EQ(
      gc.context->vm.getOpcodeKind(f.progTestOp).value_or(OpcodeKind::Add),
      OpcodeKind::Nop);
  EXPECT_EQ(
      gc.context->vm.getOpcodeKind(f.progBodyOp).value_or(OpcodeKind::Add),
      OpcodeKind::Return);
  // Unrecorded cells stay unregistered — registration is never a scan.
  // hasOpcodeKind tests the map itself (getOpcodeKind decodes any
  // int64-valued cell, which a head-private op is by construction).
  EXPECT_FALSE(gc.context->vm.hasOpcodeKind(f.regOp["reify:src:test:0"]))
      << "a head-private op cell is not in the image enumeration";
  // The guard: a value write on a registered op refuses OpcodeKind.
  zigzag::vortex::WriteFault fault = zigzag::vortex::WriteFault::None;
  const auto wrote = gc.context->core.writeValue(f.progTestOp, 0, std::nullopt,
                                                 std::string{"x"}, &fault);
  EXPECT_FALSE(wrote.has_value());
  EXPECT_EQ(fault, zigzag::vortex::WriteFault::OpcodeKind);

  // ---- serial identity: draws continue off the durable counter ------
  // authoredSerial=9 plus six reify draws leaves the arena-visible
  // counter at 15 — the durable counter never restarts.
  const auto serialRec = [&] {
    for (const auto rec : zigzag::rankAfter(ar, f.agentCell, dims.vars))
      if (ar.textOf(rec) == "meta.serial") return rec;
    return noCell;
  }();
  ASSERT_NE(serialRec, noCell);
  const auto serialVal = ar.linked(serialRec, dims.values, Posward);
  ASSERT_TRUE(serialVal.has_value());
  EXPECT_EQ(ar.asInt64(serialVal.value_or(noCell)).value_or(-1), 15);

  // ---- no-ABA: replay minted no patch records ------------------------
  std::size_t patchRankSize = 0;
  for (const auto p [[maybe_unused]] :
       zigzag::rankAfter(ar, f.agentCell, outcome->binds.dims.patch))
    ++patchRankSize;
  EXPECT_EQ(patchRankSize, 4U)
      << "replay unlinked only the dead patch — nothing was re-minted";
}

// ---------------------------------------------------------------------------
// Refusal arms — (a) gates everything
// ---------------------------------------------------------------------------

TEST(VagentGenesisTest, ForeignReceivedBandRefusesBeforeBinds) {
  DurableProgram f;
  buildDurableProgram(f);
  // A received lineage claiming the same extent but carrying a foreign
  // band: same-count, different content.
  Store foreign;
  auto at = foreign.sliceGenesis({});
  for (std::size_t i = 0; i < f.imageExtent; ++i)
    at = foreign.makeCell(at, "foreign");
  auto gc = freshContext(f.store, f); // the arena reads the *fixture* fold
  // Presenting the foreign store as the received lineage must refuse at
  // (a) — before dims, binds or replay run.
  auto outcome = runGenesis(f.store, foreign, gc.context->arena,
                            gc.context->core, gc.context->vm, gc.roots);
  ASSERT_FALSE(outcome.has_value());
}

// ---------------------------------------------------------------------------
// The second re-genesis — ≥2 folds preserve the EDL
// ---------------------------------------------------------------------------

TEST(VagentGenesisTest, SecondGenesisReplaysTheWholeEdl) {
  DurableProgram f;
  buildDurableProgram(f);
  auto gc = freshContext(f.store, f);
  auto o1 = runGenesis(f.store, f.store, gc.context->arena, gc.context->core,
                       gc.context->vm, gc.roots);
  ASSERT_TRUE(o1.has_value()) << (o1 ? "" : o1.error());

  // Fold the genesis session's promote-visible state — the fault
  // tombstone is the durable residue installs leave behind.
  auto cycle =
      regenesisCycle(f.store, gc.context->arena, f.agentCell, f.imageExtent);
  ASSERT_TRUE(cycle.has_value()) << (cycle ? "" : cycle.error());
  EXPECT_EQ(cycle->agentCell, f.agentCell)
      << "a base-cell root keeps its persistent name";

  auto &ar2 = cycle->context->arena;
  auto o2   = runGenesis(f.store, f.store, ar2, cycle->context->core,
                         cycle->context->vm, gc.roots);
  ASSERT_TRUE(o2.has_value()) << (o2 ? "" : o2.error());
  // The dead tail patch's unlink now crosses: regenesisCycle's Clears
  // policy commits its predecessor's dropped rank edge. The promoted
  // tombstone remains the retired marker regardless — genesis consults
  // it before replay.
  EXPECT_EQ(o2->patchesReplayed, 4U);
  EXPECT_EQ(o2->patchesDropped, 0U);

  const auto &dims = gc.context->core.dims();
  EXPECT_NE(clauseLabelled(ar2, f.hostSym, dims.clause, dims.name, "patch:0"),
            noCell);
  EXPECT_NE(clauseLabelled(ar2, f.hostSym, dims.clause, dims.name, "patch:3"),
            noCell);
  // The installed copies are per-context products: the second genesis's
  // reify minted fresh cells, never resurrected the first's.
  const auto c0 =
      clauseLabelled(ar2, f.hostSym, dims.clause, dims.name, "patch:0");
  const auto copy2 = ar2.linked(c0, dims.grab, Posward);
  ASSERT_TRUE(copy2.has_value());
  EXPECT_TRUE(std::ranges::find(o2->installedCopies, copy2.value_or(noCell)) !=
              o2->installedCopies.end())
      << "the second genesis's install is its own reify, not the "
         "first's cell";

  // The tombstone folded in: the dead patch skips without a new fault.
  bool tombstone = false;
  for (const auto rec : zigzag::rankAfter(ar2, f.agentCell, dims.vars))
    if (ar2.textOf(rec) == "fault:patch:4") tombstone = true;
  EXPECT_TRUE(tombstone);

  // One rank cell per filed name across two folds — no orphan twins.
  for (const auto dim : cycle->fold->dimensions()) {
    const auto name = cycle->fold->textOf(dim, f.store);
    if (name.empty()) continue;
    std::size_t count = 0;
    for (const auto other : cycle->fold->dimensions())
      if (cycle->fold->textOf(other, f.store) == name) ++count;
    EXPECT_EQ(count, 1U) << "dim '" << name << "' has " << count
                         << " rank cells";
  }
}

// ---------------------------------------------------------------------------
// The end-to-end session: real learn_* → promote → genesis → learn →
// promote → genesis — the ≥2 re-geneses cycle
// ---------------------------------------------------------------------------

/// Program-side cells only: the agent cell and its ranks arrive through
/// real session verbs and promote.
struct ProgramImage {
  Store store;
  MicroversionId at;
  std::map<std::string, DimRef> dim;
  CellRef programRoot{noCell}, sysModule{noCell}, libModule{noCell};
  CellRef hostSym{noCell};
  CellRef progHeadA{noCell}, progHeadB{noCell};
  CellRef opA{noCell}, opB{noCell}, imageRecord{noCell};
  std::uint32_t imageExtent{0};
};

void buildProgramImage(ProgramImage &f) {
  Author a{f.store};
  f.at        = a.store.sliceGenesis({});
  auto filing = fileDimensionNames(a.store, f.at, fixedDimNames());
  EXPECT_TRUE(filing.has_value()) << (filing ? "" : filing.error());
  for (const auto name : fixedDimNames())
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

  // The learn arguments: program-side heads with descriptors flagging
  // pure, entry ops, labels and persistent-coordinate recipes.
  const auto progHead = [&](std::string_view label, CellRef op) {
    const auto head  = a.cell("", f.at);
    const auto entry = dimAt(f, "d.entry");
    a.link(head, entry, op, f.at);
    a.link(head, dimAt(f, "d.name"), a.cell(label, f.at), f.at);
    // descriptor: head +d.values→ desc; desc d.vars "pure"→1, "result"→()
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
    // recipe [literal(H), literal(O), tuple(H, d.entry, +, O)]
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

  // The image record's "ops" enumeration.
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

TEST(VagentGenesisTest, SessionLearnRegenesisLearnCycles) {
  ProgramImage f;
  buildProgramImage(f);

  // -- Session 1 over the image fold: a real learn_plan. --------------
  auto fold0 = std::make_unique<zigzag::Manifold>(
      f.store.rebuildManifold(f.store.latest()));
  auto ctx0            = std::make_unique<WorkerContext>(fold0.get(), &f.store);
  auto &a0             = ctx0->arena;
  const auto agentCell = a0.makeCell("agent");
  auto binds0 = mintMetaBindSet(a0, ctx0->core, f.programRoot, agentCell);
  ASSERT_TRUE(binds0.has_value()) << (binds0 ? "" : binds0.error());
  binds0->ownModule = f.sysModule;
  binds0->lineage   = MetaLineage{
      .programRef  = f.programRoot,
      .imageExtent = f.imageExtent,
  };
  MetaRuntime meta0(ctx0->core, ctx0->vm, *binds0);
  const auto clause = meta0.learnPlan("boot", "main", f.progHeadA, f.progHeadB);
  ASSERT_TRUE(clause.has_value()) << (clause ? "" : clause.error());

  // Cycle 1: promote the agent root, then the ceremony over the fresh
  // context — the session's patch record replays.
  auto c1 = regenesisCycle(f.store, a0, agentCell, f.imageExtent);
  ASSERT_TRUE(c1.has_value()) << (c1 ? "" : c1.error());
  GenesisRoots roots{
      .programRoot = f.programRoot,
      .agentCell   = c1->agentCell,
      .ownModule   = f.sysModule,
      .imageRecord = f.imageRecord,
      .imageExtent = f.imageExtent,
  };
  auto &a1  = c1->context->arena;
  auto out1 = runGenesis(f.store, f.store, a1, c1->context->core,
                         c1->context->vm, roots);
  ASSERT_TRUE(out1.has_value()) << (out1 ? "" : out1.error());
  EXPECT_EQ(out1->patchesReplayed, 1U);
  EXPECT_EQ(out1->patchesDropped, 0U);
  // The session's registered heads folded in on the d.code rank.
  std::size_t codeRank = 0;
  for (const auto h [[maybe_unused]] :
       zigzag::rankAfter(a1, c1->agentCell, out1->binds.dims.code))
    ++codeRank;
  EXPECT_EQ(codeRank, 2U) << "learnCopies' two registered heads did not fold";
  // The replayed clause sits on the host rank with the session's
  // verbatim patch label.
  const auto &dims0 = c1->context->core.dims();
  const auto c0 =
      clauseLabelled(a1, f.hostSym, dims0.clause, dims0.name, "patch:4");
  ASSERT_NE(c0, noCell)
      << "session learn_plan's clause was not regenerated on the rank";
  // The install copy's +d.entry chain rides the fold and ends on the
  // isomorph's own twin op — a fresh cell carrying frag-a's recorded
  // kind, so the replayed fragment dispatches without leaning on the
  // image's cells.
  const auto grabCopy = a1.linked(c0, dims0.grab, Posward);
  ASSERT_TRUE(grabCopy.has_value());
  CellRef resolve  = grabCopy.value_or(noCell);
  std::size_t hops = 0;
  for (; hops < 8; ++hops) {
    if (a1.asInt64(resolve)) break;
    const auto next = a1.linked(resolve, dimAt(f, "d.entry"), Posward);
    if (!next) break;
    resolve = *next;
  }
  EXPECT_LT(hops, 8U);
  if (hops < 8)
    EXPECT_EQ(a1.asInt64(resolve).value_or(-1),
              static_cast<std::int64_t>(OpcodeKind::Nop))
        << "the folded copy's entry chain ends on frag-a's op kind";

  // -- Session 2 inside the post-genesis context: learn_where on the
  //    replayed clause — the learn → re-genesis → learn cycle. --------
  auto binds1 =
      mintMetaBindSet(a1, c1->context->core, f.programRoot, c1->agentCell);
  ASSERT_TRUE(binds1.has_value());
  binds1->ownModule = f.sysModule;
  binds1->lineage   = MetaLineage{
      .programRef  = f.programRoot,
      .imageExtent = f.imageExtent,
  };
  MetaRuntime meta1(c1->context->core, c1->context->vm, *binds1);
  const auto replaced = meta1.learnWhere(c0, f.progHeadB);
  ASSERT_TRUE(replaced.has_value()) << (replaced ? "" : replaced.error());

  // Cycle 2: another fold — both patch records ride it.
  auto c2 = regenesisCycle(f.store, a1, c1->agentCell, f.imageExtent);
  ASSERT_TRUE(c2.has_value()) << (c2 ? "" : c2.error());
  auto &a2  = c2->context->arena;
  auto out2 = runGenesis(f.store, f.store, a2, c2->context->core,
                         c2->context->vm, roots);
  ASSERT_TRUE(out2.has_value()) << (out2 ? "" : out2.error());
  EXPECT_EQ(out2->patchesReplayed, 2U);
  EXPECT_EQ(out2->patchesDropped, 0U);
  // patch:4's clause installs, then patch:7's learn_where re-points its
  // grab — ordinal-order replay, not rank-order.
  const auto &dims1 = c2->context->core.dims();
  const auto c1c =
      clauseLabelled(a2, f.hostSym, dims1.clause, dims1.name, "patch:4");
  ASSERT_NE(c1c, noCell);
  const auto whereCopy = a2.linked(c1c, dims1.grab, Posward);
  ASSERT_TRUE(whereCopy.has_value());
  // The where copy's entry chain ends on the twin op of frag-b's
  // recorded kind.
  resolve = whereCopy.value_or(noCell);
  hops    = 0;
  for (; hops < 8; ++hops) {
    if (a2.asInt64(resolve)) break;
    const auto next = a2.linked(resolve, dimAt(f, "d.entry"), Posward);
    if (!next) break;
    resolve = *next;
  }
  EXPECT_LT(hops, 8U);
  if (hops < 8)
    EXPECT_EQ(a2.asInt64(resolve).value_or(-1),
              static_cast<std::int64_t>(OpcodeKind::Return))
        << "the where copy's entry chain ends on frag-b's op kind";

  // Both patch records stayed durable — one rank walk, exactly two.
  std::size_t patchRank = 0;
  for (const auto p [[maybe_unused]] :
       zigzag::rankAfter(a2, c2->agentCell, out2->binds.dims.patch))
    ++patchRank;
  EXPECT_EQ(patchRank, 2U);
}

// The meta serial counter is durable state-side, and promote never commits an
// in-place value write on a shadowed scalar — only the additive re-point a
// minted replacement carries. An in-place increment rewinds at every fold and
// re-mints `patch:<n>` serials already handed out. learn → cycle → learn
// across three sessions: the clause serials must stay strictly increasing.
TEST(VagentGenesisTest, MetaSerialStaysMonotoneAcrossCycles) {
  ProgramImage f;
  buildProgramImage(f);
  const auto serialOf = [](std::string_view label) {
    const auto colon = label.rfind(':');
    EXPECT_NE(colon, std::string_view::npos);
    return colon == std::string_view::npos
               ? -1
               : std::stoll(std::string{label.substr(colon + 1)});
  };

  auto fold = std::make_unique<zigzag::Manifold>(
      f.store.rebuildManifold(f.store.latest()));
  auto ctx          = std::make_unique<WorkerContext>(fold.get(), &f.store);
  CellRef agent     = ctx->arena.makeCell("agent");
  std::int64_t last = -1;
  for (std::size_t round = 0; round < 3; ++round) {
    auto &arena = ctx->arena;
    auto binds  = mintMetaBindSet(arena, ctx->core, f.programRoot, agent);
    ASSERT_TRUE(binds.has_value()) << (binds ? "" : binds.error());
    binds->ownModule = f.sysModule;
    binds->lineage   = MetaLineage{
        .programRef  = f.programRoot,
        .imageExtent = f.imageExtent,
    };
    MetaRuntime meta(ctx->core, ctx->vm, *binds);
    const auto clause = meta.learnPlan("boot", std::format("m{}", round),
                                       f.progHeadA, f.progHeadB);
    ASSERT_TRUE(clause.has_value()) << (clause ? "" : clause.error());
    const std::string label = meta.labelOf(*clause).value_or("");
    ASSERT_TRUE(label.starts_with("patch:")) << "label: " << label;
    const auto serial = serialOf(label);
    EXPECT_GT(serial, last) << "serial rewound on round " << round;
    last = serial;

    auto cycle = regenesisCycle(f.store, arena, agent, f.imageExtent);
    ASSERT_TRUE(cycle.has_value()) << (cycle ? "" : cycle.error());
    GenesisRoots roots{
        .programRoot = f.programRoot,
        .agentCell   = cycle->agentCell,
        .ownModule   = f.sysModule,
        .imageRecord = f.imageRecord,
        .imageExtent = f.imageExtent,
    };
    auto out = runGenesis(f.store, f.store, cycle->context->arena,
                          cycle->context->core, cycle->context->vm, roots);
    ASSERT_TRUE(out.has_value()) << (out ? "" : out.error());
    // The fresh context's arena points raw into cycle->fold — both must
    // outlive the loop's RegenesisResult or the next round's base dangles.
    fold  = std::move(cycle->fold);
    ctx   = std::move(cycle->context);
    agent = cycle->agentCell;
  }
}

// A clean unlearn must stay retired across a fold. Under regenesisCycle's
// commitShadowed (Clears on state-side shadows) the d.patch/d.clause rank
// unlinks commit durably — the records no longer resurrect — while the
// `unlearn:<label>` tombstone remains the retired marker: it is still what
// retires lineages folded before clears existed, and it keeps genesis's
// replay and the name surface from re-running them. learn_plan → cycle →
// unlearn → cycle.
TEST(VagentGenesisTest, UnlearnedPatchStaysOutOfReplay) {
  ProgramImage f;
  buildProgramImage(f);

  auto fold0 = std::make_unique<zigzag::Manifold>(
      f.store.rebuildManifold(f.store.latest()));
  auto ctx0            = std::make_unique<WorkerContext>(fold0.get(), &f.store);
  auto &a0             = ctx0->arena;
  const auto agentCell = a0.makeCell("agent");
  auto binds0 = mintMetaBindSet(a0, ctx0->core, f.programRoot, agentCell);
  ASSERT_TRUE(binds0.has_value()) << (binds0 ? "" : binds0.error());
  binds0->ownModule = f.sysModule;
  binds0->lineage   = MetaLineage{
      .programRef  = f.programRoot,
      .imageExtent = f.imageExtent,
  };
  MetaRuntime meta0(ctx0->core, ctx0->vm, *binds0);
  const auto clause = meta0.learnPlan("boot", "main", f.progHeadA, f.progHeadB);
  ASSERT_TRUE(clause.has_value()) << (clause ? "" : clause.error());
  const std::string label = meta0.labelOf(*clause).value_or("");
  ASSERT_TRUE(label.starts_with("patch:"));

  auto c1 = regenesisCycle(f.store, a0, agentCell, f.imageExtent);
  ASSERT_TRUE(c1.has_value()) << (c1 ? "" : c1.error());
  GenesisRoots roots{
      .programRoot = f.programRoot,
      .agentCell   = c1->agentCell,
      .ownModule   = f.sysModule,
      .imageRecord = f.imageRecord,
      .imageExtent = f.imageExtent,
  };
  auto &a1  = c1->context->arena;
  auto out1 = runGenesis(f.store, f.store, a1, c1->context->core,
                         c1->context->vm, roots);
  ASSERT_TRUE(out1.has_value()) << (out1 ? "" : out1.error());
  EXPECT_EQ(out1->patchesReplayed, 1U);

  // Session 2: the replayed clause resolves, then unlearn retires it.
  auto binds1 =
      mintMetaBindSet(a1, c1->context->core, f.programRoot, c1->agentCell);
  ASSERT_TRUE(binds1.has_value());
  binds1->ownModule = f.sysModule;
  binds1->lineage   = MetaLineage{
      .programRef  = f.programRoot,
      .imageExtent = f.imageExtent,
  };
  MetaRuntime meta1(c1->context->core, c1->context->vm, *binds1);
  const auto live = meta1.planNamed(label);
  ASSERT_TRUE(live.has_value()) << "replayed clause did not resolve";
  const auto removed = meta1.unlearn(*live);
  ASSERT_TRUE(removed.has_value()) << (removed ? "" : removed.error());

  auto c2 = regenesisCycle(f.store, a1, c1->agentCell, f.imageExtent);
  ASSERT_TRUE(c2.has_value()) << (c2 ? "" : c2.error());
  auto &a2  = c2->context->arena;
  auto out2 = runGenesis(f.store, f.store, a2, c2->context->core,
                         c2->context->vm, roots);
  ASSERT_TRUE(out2.has_value()) << (out2 ? "" : out2.error());
  // The tombstoned patch stays out of replay; under clears its rank record
  // no longer even resurrects with the fold.
  EXPECT_EQ(out2->patchesReplayed, 0U)
      << "the unlearned patch re-ran at genesis";

  // The clause cell stays name-dead on the meta surface — the committed
  // unlink removes it, and the tombstone remains the retired marker for
  // lineages folded before clears.
  auto binds2 =
      mintMetaBindSet(a2, c2->context->core, f.programRoot, c2->agentCell);
  ASSERT_TRUE(binds2.has_value());
  binds2->ownModule = f.sysModule;
  binds2->lineage   = MetaLineage{
      .programRef  = f.programRoot,
      .imageExtent = f.imageExtent,
  };
  MetaRuntime meta2(c2->context->core, c2->context->vm, *binds2);
  EXPECT_FALSE(meta2.planNamed(label).has_value())
      << "the unlearned clause still resolves after genesis";
}

// ---------------------------------------------------------------------------
// Step (b½) — durable d.kind-registered rehydration and circuit breaker
// ---------------------------------------------------------------------------

CellRef mintKindRegisteredHead(DurableProgram &f, Author &a,
                               std::string_view label, CellRef op) {
  const auto head = a.cell("", f.at);
  a.link(head, dimAt(f, "d.entry"), op, f.at);
  a.link(head, dimAt(f, "d.name"), a.cell(label, f.at), f.at);

  const auto mLitH  = a.scalar(static_cast<std::int64_t>(head), f.at);
  const auto mLitO  = a.scalar(static_cast<std::int64_t>(op), f.at);
  const auto mFrom  = a.scalar(static_cast<std::int64_t>(head), f.at);
  const auto anchor = a.scalar(static_cast<std::int64_t>(f.programRoot), f.at);
  a.link(anchor, dimAt(f, "d.name"), a.cell("d.entry", f.at), f.at);
  const auto mDir = a.scalar(1, f.at);
  const auto mTo  = a.scalar(static_cast<std::int64_t>(op), f.at);
  a.link(head, dimAt(f, "d.recipe"), mLitH, f.at);
  a.link(mLitH, dimAt(f, "d.step"), mLitO, f.at);
  a.link(mLitO, dimAt(f, "d.step"), mFrom, f.at);
  a.link(mFrom, dimAt(f, "d.step"), anchor, f.at);
  a.link(anchor, dimAt(f, "d.step"), mDir, f.at);
  a.link(mDir, dimAt(f, "d.step"), mTo, f.at);
  return head;
}

TEST(VagentGenesisTest, KindRegisteredHeadRehydratedInGenesis) {
  DurableProgram f;
  buildDurableProgram(f);
  Author a{f.store};

  // Author a head with an op on d.kind-registered directly (not on d.code).
  const auto kindOp =
      a.scalar(static_cast<std::int64_t>(OpcodeKind::Return), f.at);
  const auto head = mintKindRegisteredHead(f, a, "kind-reg-frag", kindOp);
  a.link(f.agentCell, dimAt(f, "d.kind-registered"), head, f.at);

  auto gc      = freshContext(f.store, f);
  auto outcome = runGenesis(f.store, f.store, gc.context->arena,
                            gc.context->core, gc.context->vm, gc.roots);
  ASSERT_TRUE(outcome.has_value()) << (outcome ? "" : outcome.error());

  // (b½): 1 registered head rehydrated.
  EXPECT_EQ(outcome->registeredHeadsRehydrated, 1U);
  EXPECT_EQ(outcome->registeredHeadsDropped, 0U);
  // The head's op was registered in VM.
  EXPECT_TRUE(gc.context->vm.hasOpcodeKind(kindOp));
  EXPECT_EQ(gc.context->vm.getOpcodeKind(kindOp).value_or(OpcodeKind::Add),
            OpcodeKind::Return);
  // And the head was ensured on agentCell +d.code.
  bool onCode = false;
  for (const auto h : zigzag::rankAfter(gc.context->arena, f.agentCell,
                                        outcome->binds.dims.code)) {
    if (h == head) {
      onCode = true;
      break;
    }
  }
  EXPECT_TRUE(onCode) << "rehydrated head was not placed on d.code";
}

TEST(VagentGenesisTest, PatchReferencingKindRegisteredHeadSucceeds) {
  DurableProgram f;
  buildDurableProgram(f);
  Author a{f.store};

  // Author a head only on d.kind-registered.
  const auto kindOp =
      a.scalar(static_cast<std::int64_t>(OpcodeKind::Return), f.at);
  const auto head = mintKindRegisteredHead(f, a, "kind-patch-head", kindOp);
  a.link(f.agentCell, dimAt(f, "d.kind-registered"), head, f.at);

  // Author a patch record that references "kind-patch-head".
  CellRef patchTail = f.patches.back();
  f.patches.push_back(mintPatchRecord(f, a,
                                      PatchSpec{
                                          .label   = "patch:5",
                                          .ordinal = 5,
                                          .verb    = "learn_where",
                                          .target  = "patch:0",
                                          .heads   = {"kind-patch-head"},
                                      },
                                      patchTail));

  auto gc      = freshContext(f.store, f);
  auto outcome = runGenesis(f.store, f.store, gc.context->arena,
                            gc.context->core, gc.context->vm, gc.roots);
  ASSERT_TRUE(outcome.has_value()) << (outcome ? "" : outcome.error());

  EXPECT_EQ(outcome->registeredHeadsRehydrated, 1U);
  // 5 live patches replayed (0, 1, 2, 3, and our 5; patch 4 was dead).
  EXPECT_EQ(outcome->patchesReplayed, 5U);
  EXPECT_EQ(outcome->patchesDropped, 1U);
}

TEST(VagentGenesisTest, CorruptedKindRegisteredHeadDroppedAndCircuitBreaker) {
  DurableProgram f;
  buildDurableProgram(f);
  Author a{f.store};

  // Corrupted head 1: op ordinal is invalid / out of range for validate.
  const auto badOp1   = a.scalar(999'999, f.at);
  const auto badHead1 = mintKindRegisteredHead(f, a, "corrupt-head-1", badOp1);

  // Corrupted head 2: another head on the d.kind-registered rank.
  const auto badOp2   = a.scalar(999'998, f.at);
  const auto badHead2 = mintKindRegisteredHead(f, a, "corrupt-head-2", badOp2);

  // Link agentCell -> badHead1 -> badHead2 on d.kind-registered.
  a.link(f.agentCell, dimAt(f, "d.kind-registered"), badHead1, f.at);
  a.link(badHead1, dimAt(f, "d.kind-registered"), badHead2, f.at);

  auto gc      = freshContext(f.store, f);
  auto outcome = runGenesis(f.store, f.store, gc.context->arena,
                            gc.context->core, gc.context->vm, gc.roots);
  ASSERT_TRUE(outcome.has_value()) << (outcome ? "" : outcome.error());

  // Head 1 failed verification; maxReplayFailures (default 1) was reached;
  // Head 2 tripped the circuit breaker. Both are dropped.
  EXPECT_EQ(outcome->registeredHeadsRehydrated, 0U);
  EXPECT_EQ(outcome->registeredHeadsDropped, 2U);

  // Both heads should be unlinked from d.kind-registered.
  for (const auto h : zigzag::rankAfter(gc.context->arena, f.agentCell,
                                        outcome->binds.dims.kindRegistered)) {
    EXPECT_NE(h, badHead1);
    EXPECT_NE(h, badHead2);
  }

  // Fault tombstones written to agentCell's d.vars.
  bool foundFault1 = false;
  bool foundFault2 = false;
  for (const auto rec : zigzag::rankAfter(gc.context->arena, f.agentCell,
                                          gc.context->core.dims().vars)) {
    if (gc.context->arena.textOf(rec) == "fault:corrupt-head-1") {
      foundFault1 = true;
    }
    if (gc.context->arena.textOf(rec) == "fault:corrupt-head-2") {
      foundFault2    = true;
      const auto val = gc.context->arena.linked(
          rec, gc.context->core.dims().values, Posward);
      ASSERT_TRUE(val.has_value());
      EXPECT_EQ(gc.context->arena.textOf(*val), "circuit_breaker");
    }
  }
  EXPECT_TRUE(foundFault1) << "tombstone for corrupt-head-1 not found";
  EXPECT_TRUE(foundFault2)
      << "circuit breaker tombstone for corrupt-head-2 not found";
}

} // namespace
