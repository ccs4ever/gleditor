#include "adapter.hpp"

#include "genesis.hpp"
#include "meta.hpp"
#include "records.hpp"
#include "../vortex/worker_context.hpp"

#include <vector>

namespace xanadu::vagent {

namespace {

namespace mesh = zigzag::vortex::mesh;
using xanadu::identity::MessageType;
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::noCell;

/// The same key shape MeshActorRuntime::pidKey renders — a private
/// static there, so the adapter keeps its own copy; the two must agree
/// because the mesh's actors_ map and a member's `pid` record are the
/// same lookup from opposite sides.
[[nodiscard]] std::string
renderPid(const mesh::ActorPid &pid) {
  return std::to_string(pid.node.size()) + ":" + pid.node +
         std::to_string(pid.cursor.size()) + ":" + pid.cursor + ":" +
         std::to_string(pid.epoch) + ":" +
         std::to_string(pid.spawnSequence);
}

/// `home +d.dirs → directory +d.agents → member` — the walk
/// agentCellOf encodes in the executor, repeated here so the adapter
/// resolves through the same durable shape.
[[nodiscard]] std::vector<CellRef>
directoryMembers(zigzag::ArenaManifold &arena,
                 zigzag::vortex::VortexCore &core) {
  const auto dirs   = core.findOrMintDimension("d.dirs");
  const auto agents = core.findOrMintDimension("d.agents");
  std::vector<CellRef> members;
  for (const CellRef dir : zigzag::rankAfter(arena, core.home(), dirs))
    for (const CellRef member : zigzag::rankAfter(arena, dir, agents))
      members.push_back(member);
  return members;
}

/// The member's durable `pid` record — a text cell holding the
/// rendered mesh key, overwritten per incarnation.
[[nodiscard]] std::optional<std::string>
memberPidKey(zigzag::ArenaManifold &arena,
             zigzag::vortex::VortexCore &core, CellRef member) {
  const auto rec = varsRecord(arena, core, member, "pid", false);
  if (!rec) return std::nullopt;
  const auto v = arena.linked(*rec, core.dims().values, DimVector::POS);
  if (!v) return std::nullopt;
  return std::string{arena.textOf(*v)};
}

/// The +d.step/+d.name field envelope a fault payload carries —
/// `occurrence.<name>` on an `on EffectFailed` handler reads these
/// children back. The executor's own raise path mints the same shape.
void replyField(zigzag::ArenaManifold &arena,
                zigzag::vortex::VortexCore &core, CellRef payload,
                std::string_view name, std::string_view text) {
  const auto value = arena.makeCell(text);
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, payload, core.dims().step),
                 core.dims().step, DimVector::POS, value));
  zigzag::expectWritten(arena.link(value, core.dims().name,
                                 DimVector::POS, arena.makeCell(name)));
}

} // namespace

VagentAdapter::VagentAdapter(VagentExecutor executor)
    : executor_(std::move(executor)) {}

void VagentAdapter::bindNative(std::string_view capacity,
                               std::string_view method, NativeBody body) {
  natives_.emplace(std::string{capacity} + "." + std::string{method},
                   std::move(body));
}

VagentAdapter *VagentAdapter::setSpawnSender(SpawnSender sender) {
  spawnSender_ = std::move(sender);
  return this;
}

std::expected<std::string, mesh::ActorError>
VagentAdapter::operator()(const mesh::ActorExecution &exec,
                          zigzag::vortex::WorkerContext &ctx) {
  auto &arena = ctx.arena;
  auto &core  = ctx.core;

  // Mesh → language pid translation: a noted member's exec pid is
  // rewritten to its directory index — agentCellOf's namespace — so
  // the inner turn picks the same member the pid record names. The
  // language-pid fallback applies only while *no* member carries a
  // pid record (harness turns and pre-genesis contexts fabricate
  // pids in that namespace); once genesis notes any member, an
  // unresolvable exec pid must resolve to nobody — guessing a member
  // by index under the runtime's global spawnSequence would run a
  // turn on the wrong agent.
  mesh::ActorExecution turn = exec;
  std::optional<CellRef> self;
  if (const auto member = memberForPid(ctx, exec.pid)) {
    self = member;
    if (const auto index = memberIndex(ctx, *member))
      turn.pid.spawnSequence = *index + 1;
  } else {
    const auto members = directoryMembers(arena, core);
    const auto anyNoted = std::ranges::any_of(members, [&](CellRef m) {
      return memberPidKey(arena, core, m).has_value();
    });
    if (!anyNoted) {
      const auto index =
          exec.pid.spawnSequence < 2
              ? std::size_t{0}
              : static_cast<std::size_t>(exec.pid.spawnSequence - 1);
      if (index < members.size()) self = members[index];
    }
  }

  if (exec.message &&
      exec.message->type == MessageType::ActorDown) {
    // A monitored member's down arrives as a mailbox message; the
    // percept the language dispatches is ChildDown, so the raw
    // message is consumed here — journaling it as a generic
    // "ActorDown" event would double-report the same loss.
    if (self && exec.message->source)
      if (const auto dead = memberForPid(ctx, *exec.message->source))
        executor_.childDown(arena, core, *self, *dead);
    turn.message.reset();
  }

  auto result = executor_(turn, ctx);
  if (result && self) drain(ctx, *self);
  return result;
}

std::expected<void, mesh::ActorError>
VagentAdapter::genesis(const mesh::ActorGenesis &genesis,
                       zigzag::vortex::WorkerContext &ctx) {
  auto &arena = ctx.arena;
  auto &core  = ctx.core;
  // The actor is its own context's declared member — index 0 in the
  // home directory, the same cell an exec pid of sequence 1 picks.
  const auto members = directoryMembers(arena, core);
  if (members.empty()) return {};
  const CellRef self = members.front();

  // Phase-6's wiring: when the arena overlays a lineage fold —
  // spawn's pinned fold or resume's rebuild — genesis is the (a)–(e)
  // ceremony, not just pid bookkeeping. The lineage is the retained
  // store under the arena; the received band is the transported
  // comparand on resume and the same-store cycle on spawn. A fold
  // whose program never filed the image records binds no lineage —
  // the ceremony has no band to compare, so it stays skipped rather
  // than refused, matching the learn gate's closed-by-absence rule.
  if (const auto *base = arena.base(); base != nullptr) {
    if (const auto *lineage = base->store(); lineage != nullptr) {
      const CellRef programRoot = core.home();
      GenesisRoots roots{.programRoot = programRoot, .agentCell = self};
      // The agent's own module is the first +d.stdlib member — the
      // same default the turn's bind mint resolves bare names
      // against.
      for (const CellRef module : zigzag::rankAfter(
               arena, programRoot, core.dims().stdlib)) {
        roots.ownModule = module;
        break;
      }
      if (const auto rec = varsRecord(arena, core, programRoot,
                                    "imageRecord", false))
        if (const auto v =
                arena.linked(*rec, core.dims().values, DimVector::POS))
          roots.imageRecord = *v;
      if (const auto rec = varsRecord(arena, core, programRoot,
                                    "imageExtent", false))
        if (const auto v =
                arena.linked(*rec, core.dims().values, DimVector::POS))
          if (const auto extent = arena.asInt64(*v); extent && *extent > 0)
            roots.imageExtent = static_cast<std::uint32_t>(*extent);
      if (roots.imageRecord != noCell && roots.imageExtent.has_value()) {
        const auto &received = genesis.transported != nullptr
                                   ? *genesis.transported
                                   : *lineage;
        // A refusal here is the pairwise comparand failing closed —
        // the incarnation never notes a pid it cannot stand behind.
        if (const auto outcome = runGenesis(*lineage, received, arena,
                                            core, ctx.vm, roots);
            !outcome)
          return std::unexpected(mesh::ActorError::ExecutionFailed);
      }
    }
  }

  noteMemberPid(ctx, self, genesis.pid);
  if (genesis.transported != nullptr)
    executor_.resumed(arena, core, self);
  return {};
}

void VagentAdapter::drain(zigzag::vortex::WorkerContext &ctx,
                          CellRef member) const {
  auto &arena = ctx.arena;
  auto &core  = ctx.core;
  for (const auto &entry : VagentExecutor::drainOutbox(arena, core, member)) {
    if (entry.kind == "effect" || entry.kind == "cancel") {
      // Host work, not routing: an effect invokes its binding and
      // replies on the request's correlation; a cancel only notifies
      // — the parked wait already faulted on its own deadline. The
      // `dispatched` mark is what makes "performed once" durable: the
      // outbox rank persists, so without it every drain would re-run
      // the body and a checkpoint would replay it.
      if (entry.cell == noCell ||
          varsRecord(arena, core, entry.cell, "dispatched", false))
        continue;
      if (const auto rec =
              varsRecord(arena, core, entry.cell, "dispatched", true))
        zigzag::expectWritten(arena.link(*rec, core.dims().values,
                                       DimVector::POS,
                                       arena.makeCell("1")));
      if (entry.kind != "effect") continue;

      zigzag::CellRef result = noCell;
      std::string fault;
      if (const auto fn = natives_.find(entry.capacity + "." +
                                        entry.payload);
          fn != natives_.end()) {
        if (auto out = fn->second(ctx, entry.sender, entry.payloadCell))
          result = *out;
        else
          fault = std::move(out.error());
      } else {
        fault = "unbound";
      }
      Delivery reply{.agent        = entry.sender,
                     .percept      = fault.empty() ? "EffectDone"
                                                  : "EffectFailed",
                     .id           = entry.id + ":effect",
                     .kind         = "effect",
                     .payloadCell  = noCell,
                     .correlation  = entry.id,
                     .sender       = entry.sender,
                     .delegator    = noCell,
                     .fault        = fault};
      if (fault.empty()) {
        reply.payloadCell = result;
      } else {
        // Handlers read occurrence.capacity/method/cause — the
        // fielded record — while the parked wait takes the same
        // cause off the journal's fault record into its else ladder.
        const auto payload = arena.makeCell("EffectFailed");
        replyField(arena, core, payload, "capacity", entry.capacity);
        replyField(arena, core, payload, "method", entry.payload);
        replyField(arena, core, payload, "cause", fault);
        reply.payloadCell = payload;
      }
      executor_.deliver(arena, core, reply);
      continue;
    }
    if (entry.kind == "spawn") {
      // A member the verb minted but the wire never asked for —
      // payloadCell is the member cell, entry.id the durable
      // requestId. At-least-once by ordering: the correlation is
      // durable before the send runs and `dispatched` marks only
      // after the attempt, so a crashed drain retries and send's
      // own requestId dedupe keeps the wire honest. No sender
      // installed leaves the entry pending — a member is a live
      // language-side child either way; the mesh spawn is the
      // integration layer's choice.
      if (entry.cell == noCell || entry.payloadCell == noCell ||
          varsRecord(arena, core, entry.cell, "dispatched", false) ||
          !spawnSender_)
        continue;
      noteSpawnRequest(ctx, entry.payloadCell, entry.id);
      const auto sent = spawnSender_(ctx, entry.payloadCell, entry.id);
      if (const auto rec =
              varsRecord(arena, core, entry.cell, "dispatched", true))
        zigzag::expectWritten(
            arena.link(*rec, core.dims().values, DimVector::POS,
                       arena.makeCell("1")));
      // A send that can't even leave is the refusal's synchronous
      // half — the asynchronous one arrives through spawnOutcome.
      if (!sent)
        executor_.spawnFailed(arena, core, entry.sender,
                              entry.payloadCell);
      continue;
    }
    const auto deliveries =
        VagentExecutor::route(arena, core, core.home(), entry);
    if (deliveries.empty()) {
      // "0"/empty is broadcast — silence there means an empty
      // directory, never a fault. A concrete target the name scan
      // cannot find is the unroutable case §"delivery failure"
      // raises on the sender.
      if (!entry.target.empty() && entry.target != "0" &&
          !memberNamed(ctx, entry.target))
        executor_.deliveryFailed(arena, core, entry, "unroutable");
      continue;
    }
    for (const auto &delivery : deliveries)
      executor_.deliver(arena, core, delivery);
  }
}

void VagentAdapter::noteMemberPid(zigzag::vortex::WorkerContext &ctx,
                                  CellRef member,
                                  const mesh::ActorPid &pid) const {
  const auto rec =
      varsRecord(ctx.arena, ctx.core, member, "pid", true);
  if (!rec) return;
  zigzag::expectWritten(
      ctx.arena.link(*rec, ctx.core.dims().values, DimVector::POS,
                     ctx.arena.makeCell(renderPid(pid))));
}

std::optional<CellRef>
VagentAdapter::memberForPid(zigzag::vortex::WorkerContext &ctx,
                            const mesh::ActorPid &pid) const {
  const auto key = renderPid(pid);
  for (const CellRef member : directoryMembers(ctx.arena, ctx.core))
    if (memberPidKey(ctx.arena, ctx.core, member) == key) return member;
  return std::nullopt;
}

std::optional<CellRef>
VagentAdapter::memberNamed(zigzag::vortex::WorkerContext &ctx,
                           std::string_view name) const {
  for (const CellRef member : directoryMembers(ctx.arena, ctx.core))
    if (const auto n = ctx.arena.linked(member, ctx.core.dims().name,
                                        DimVector::POS))
      if (ctx.arena.textOf(*n) == name) return member;
  return std::nullopt;
}

std::optional<std::size_t>
VagentAdapter::memberIndex(zigzag::vortex::WorkerContext &ctx,
                           CellRef member) const {
  const auto members = directoryMembers(ctx.arena, ctx.core);
  for (std::size_t i = 0; i < members.size(); ++i)
    if (members[i] == member) return i;
  return std::nullopt;
}

void VagentAdapter::memberRebound(zigzag::vortex::WorkerContext &ctx,
                                  CellRef holder, CellRef member) const {
  executor_.moved(ctx.arena, ctx.core, holder, member);
}

void VagentAdapter::spawnRefused(zigzag::vortex::WorkerContext &ctx,
                                 CellRef parent, CellRef child) const {
  executor_.spawnFailed(ctx.arena, ctx.core, parent, child);
}

void VagentAdapter::memberStopping(zigzag::vortex::WorkerContext &ctx,
                                   CellRef parent, CellRef member,
                                   std::string_view cause) const {
  executor_.childStopping(ctx.arena, ctx.core, parent, member, cause);
}

std::optional<std::int64_t>
VagentAdapter::nextDeadline(zigzag::vortex::WorkerContext &ctx,
                            CellRef member) const {
  auto &arena = ctx.arena;
  auto &core  = ctx.core;
  std::optional<std::int64_t> earliest;
  // Only a parked intention is a live wait — a fired one leaves its
  // awaitDeadline behind until the intention unlinks, and a running
  // intention's stale field is not a wake the host owes.
  for (const CellRef intention : zigzag::rankAfter(
           arena, member, core.findOrMintDimension("d.intention"))) {
    const auto parked = [&](std::string_view flag) {
      const auto rec = varsRecord(arena, core, intention, flag, false);
      if (!rec) return false;
      const auto v = arena.linked(*rec, core.dims().values,
                                  DimVector::POS);
      return v && arena.asInt64(*v).value_or(0) != 0;
    };
    if (!parked("waiting") && !parked("delegated")) continue;
    const auto rec =
        varsRecord(arena, core, intention, "awaitDeadline", false);
    if (!rec) continue;
    const auto v = arena.linked(*rec, core.dims().values,
                                DimVector::POS);
    if (!v) continue;
    const auto deadline = arena.asInt64(*v);
    if (!deadline) continue;
    if (!earliest || *deadline < *earliest) earliest = deadline;
  }
  return earliest;
}

void VagentAdapter::noteSpawnRequest(zigzag::vortex::WorkerContext &ctx,
                                     CellRef member,
                                     std::string_view requestId) const {
  const auto rec = varsRecord(ctx.arena, ctx.core, member,
                              "spawnRequest", true);
  if (!rec) return;
  zigzag::expectWritten(
      ctx.arena.link(*rec, ctx.core.dims().values, DimVector::POS,
                     ctx.arena.makeCell(requestId)));
}

void VagentAdapter::spawnOutcome(
    zigzag::vortex::WorkerContext &ctx,
    const mesh::ActorMessage &request,
    const std::expected<mesh::ActorPid, mesh::ActorError> &result)
    const {
  auto &arena = ctx.arena;
  auto &core  = ctx.core;
  const auto members = directoryMembers(arena, core);
  for (const CellRef member : members) {
    const auto rec =
        varsRecord(arena, core, member, "spawnRequest", false);
    if (!rec) continue;
    const auto v = arena.linked(*rec, core.dims().values,
                                DimVector::POS);
    if (!v || arena.textOf(*v) != request.requestId) continue;
    if (result) {
      // The grant's target is the child's real mesh identity —
      // the spawnRequest record's whole job is done once it lands.
      noteMemberPid(ctx, member, *result);
      zigzag::expectWritten(
          arena.unlink(*rec, core.dims().values, DimVector::POS));
    } else if (!members.empty()) {
      // §8.6: the requesting member is the directory's self —
      // spawn minted the member under its own agent.
      executor_.spawnFailed(arena, core, members.front(), member);
    }
    return;
  }
}

} // namespace xanadu::vagent
