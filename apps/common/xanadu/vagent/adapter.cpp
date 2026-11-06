#include "adapter.hpp"

#include "genesis.hpp"
#include "meta.hpp"
#include "percept_codec.hpp"
#include "records.hpp"
#include "../scalar.hpp"
#include "../vortex/worker_context.hpp"

#include <bit>
#include <format>
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

/// renderPid's inverse — a member's durable `pid` record text back
/// into the mesh identity a wire send needs. `<nlen>:<node><clen>:<cursor>:<epoch>:<seq>`.
[[nodiscard]] std::optional<mesh::ActorPid>
parsePid(std::string_view key) {
  const auto field = [&](std::size_t &pos) -> std::optional<std::string_view> {
    const auto colon = key.find(':', pos);
    if (colon == std::string_view::npos) return std::nullopt;
    const auto len = [&]() -> std::optional<std::size_t> {
      try {
        return static_cast<std::size_t>(
            std::stoull(std::string{key.substr(pos, colon - pos)}));
      } catch (...) {
        return std::nullopt;
      }
    }();
    if (!len) return std::nullopt;
    pos = colon + 1;
    if (pos + *len > key.size()) return std::nullopt;
    const auto out = key.substr(pos, *len);
    pos += *len;
    return out;
  };
  std::size_t pos = 0;
  mesh::ActorPid pid;
  const auto node   = field(pos);
  const auto cursor = field(pos);
  if (!node || !cursor || pos >= key.size() || key[pos] != ':')
    return std::nullopt;
  ++pos;
  const auto colon = key.find(':', pos);
  if (colon == std::string_view::npos) return std::nullopt;
  try {
    pid.node          = std::string{*node};
    pid.cursor        = std::string{*cursor};
    pid.epoch         = std::stoull(std::string{key.substr(pos, colon - pos)});
    pid.spawnSequence = std::stoull(std::string{key.substr(colon + 1)});
  } catch (...) {
    return std::nullopt;
  }
  return pid.isValid() ? std::optional{pid} : std::nullopt;
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

/// The member cell @p member's `$parent` record names — the scalar-ref
/// convention every vars-bound member rides. noCell for a top-level
/// member.
[[nodiscard]] CellRef
parentMember(zigzag::ArenaManifold &arena,
             zigzag::vortex::VortexCore &core, CellRef member) {
  const auto rec = varsRecord(arena, core, member, "$parent", false);
  if (!rec) return noCell;
  const auto v = arena.linked(*rec, core.dims().values, DimVector::POS);
  if (!v) return noCell;
  const auto r = arena.asInt64(*v);
  return r ? static_cast<CellRef>(*r) : noCell;
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

VagentAdapter *VagentAdapter::setSendSender(SendSender sender) {
  sendSender_ = std::move(sender);
  return this;
}

std::string VagentAdapter::stageSend(
    zigzag::vortex::WorkerContext &ctx, CellRef sender,
    CellRef targetMember, std::string_view percept,
    const std::vector<std::pair<std::string,
                                zigzag::vortex::CellValue>> &fields,
    std::string_view id) const {
  auto &arena = ctx.arena;
  auto &core  = ctx.core;
  const auto outbox = core.findOrMintDimension("d.outbox");
  const auto position = std::ranges::to<std::vector>(
                            zigzag::rankAfter(arena, sender, outbox))
                            .size();
  const auto entry = arena.makeCell();
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, sender, outbox), outbox,
                 DimVector::POS, entry));
  const auto outField = [&](std::string_view name, CellRef value) {
    if (const auto rec = varsRecord(arena, core, entry, name, true))
      zigzag::expectWritten(
          arena.link(*rec, core.dims().values, DimVector::POS, value));
  };
  outField("kind", arena.makeCell("send"));
  outField("percept", arena.makeCell(std::string{percept}));
  outField("targetMember", refCell(arena, core, targetMember));
  if (!fields.empty()) {
    const auto payload = arena.makeCell(percept);
    for (const auto &[field, value] : fields) {
      // marshalTermExpr's dual channels: typed bits for bound-value
      // reads, text for the wire render and term equality.
      const auto cell = std::visit(
          [&](const auto &v) -> CellRef {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
              return arena.makeCell(v);
            } else {
              const auto c = arena.makeCell(std::format("{}", v));
              if constexpr (std::is_same_v<T, bool>) {
                zigzag::expectWritten(arena.setValueBits(
                    c, xanadu::ValueKind::Bool,
                    static_cast<std::uint64_t>(v)));
              } else if constexpr (std::is_same_v<T, std::int64_t>) {
                zigzag::expectWritten(arena.setValueBits(
                    c, xanadu::ValueKind::Int64,
                    std::bit_cast<std::uint64_t>(v)));
              } else {
                zigzag::expectWritten(arena.setValueBits(
                    c, xanadu::ValueKind::Double,
                    xanadu::canonicalDoubleBits(v)));
              }
              return c;
            }
          },
          value);
      zigzag::expectWritten(
          arena.link(zigzag::rankTail(arena, payload, core.dims().step),
                     core.dims().step, DimVector::POS, cell));
      zigzag::expectWritten(arena.link(cell, core.dims().name,
                                     DimVector::POS,
                                     arena.makeCell(field)));
    }
    outField("payload", refCell(arena, core, payload));
  }
  const auto requestId = std::string{id};
  outField("id", arena.makeCell(requestId.empty()
                                    ? std::format("send:{}:{}",
                                                  targetMember, position)
                                    : requestId));
  return requestId.empty() ? std::format("send:{}:{}", targetMember,
                                         position)
                           : requestId;
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
      if (const auto dead = memberForPid(ctx, *exec.message->source)) {
        // §4.5's lease-vs-loss classification: the monitor's
        // "unreachable" (phi + lease + discovery evidence) and any
        // unknown reason are the loss path — ChildDown. An orderly
        // cause announces a stopping member, which reports
        // ChildStopping; the shared dedupe collapses whichever report
        // arrives second.
        const auto &reason = exec.message->reason;
        const auto cause =
            reason == "stopped" ? std::string_view{"stop"}
            : reason == "stop" || reason == "terminate" ||
                    reason == "lease" || reason == "init"
                ? std::string_view{reason}
                : std::string_view{};
        if (!cause.empty())
          executor_.childStopping(arena, core, *self, *dead, cause);
        else
          executor_.childDown(arena, core, *self, *dead);
      }
    turn.message.reset();
  }

  if (exec.message && self &&
      exec.message->type == MessageType::ActorSend &&
      exec.message->source) {
    // The wire's fielded-percept arm: `value` parses as
    // `Name{f: v, …}` — the codec the remote drain arm renders and
    // the runner's --send shares. Three control names get adapter
    // handling (Moved, stop, ChildStopping) because their semantics
    // live above a plain inject; a bare name stays on the executor's
    // message path, which dispatches it verbatim.
    if (const auto spec = parsePercept(exec.message->value)) {
      if (spec->name == "Moved") {
        // §5.4's "receivers update $peers": the envelope's source is
        // the peer's new incarnation — the fresh reference cell the
        // holder's same-name records rebind to. The peer's durable
        // name rides the payload's `name` field.
        std::string movedName;
        for (const auto &[field, value] : spec->fields)
          if (field == "name")
            if (const auto *text = std::get_if<std::string>(&value))
              movedName = *text;
        if (!movedName.empty()) {
          const auto dirs   = core.findOrMintDimension("d.dirs");
          const auto agents = core.findOrMintDimension("d.agents");
          const auto fresh  = arena.makeCell();
          for (const CellRef dir :
               zigzag::rankAfter(arena, core.home(), dirs)) {
            zigzag::expectWritten(
                arena.link(zigzag::rankTail(arena, dir, agents),
                           agents, DimVector::POS, fresh));
            break;
          }
          zigzag::expectWritten(
              arena.link(fresh, core.dims().name, DimVector::POS,
                         arena.makeCell(movedName)));
          noteMemberPid(ctx, fresh, *exec.message->source);
          executor_.moved(arena, core, *self, fresh);
          // A moved parent arrives as the same cursor under a new
          // node/epoch — the pid's content identity is what survives
          // an incarnation, and a name-less $parent member gives the
          // name rebind nothing to find. Rename $parentPid to the new
          // incarnation so the next genesis rebinds through it, and
          // swing the $parent scalar now.
          if (const auto pp = varsRecord(arena, core, *self, "$parentPid",
                                         false))
            if (const auto pv = arena.linked(*pp, core.dims().values,
                                             DimVector::POS))
              if (const auto old = parsePid(arena.textOf(*pv));
                  old && old->cursor == exec.message->source->cursor) {
                zigzag::expectWritten(arena.unlink(
                    *pp, core.dims().values, DimVector::POS));
                zigzag::expectWritten(arena.link(
                    *pp, core.dims().values, DimVector::POS,
                    arena.makeCell(renderPid(*exec.message->source))));
                if (const auto prec = varsRecord(arena, core, *self,
                                                 "$parent", true)) {
                  if (arena.linked(*prec, core.dims().values,
                                   DimVector::POS))
                    zigzag::expectWritten(arena.unlink(
                        *prec, core.dims().values, DimVector::POS));
                  zigzag::expectWritten(arena.link(
                      *prec, core.dims().values, DimVector::POS,
                      refCell(arena, core, fresh)));
                }
              }
        }
        turn.message.reset();
      } else if (spec->name == "stop") {
        // §7.1's "accepted from the parent only": the wire source's
        // pid must be the member $parent records. Anyone else's stop
        // is consumed and dropped — no refuse kind is defined for it.
        const auto parent = parentMember(arena, core, *self);
        if (parent != noCell &&
            memberPidKey(arena, core, parent) ==
                renderPid(*exec.message->source)) {
          executor_.stoppedByParent(arena, core, *self);
          // §4.5's announcement returns over the same arm — the
          // parent hears ChildStopping{cause:"stop"} from the
          // member's own drain, not the mesh.
          stageSend(ctx, *self, parent, "ChildStopping",
                    {{"cause", std::string{"stop"}}}, {});
        }
        turn.message.reset();
      } else if (spec->name == "ChildStopping") {
        // The child's orderly-stop report: `child` is the wire's
        // source (the member cell its pid records), `cause` the
        // payload field — executor-side dedupe keeps the trailing
        // ActorDown from doubling the raise.
        std::string cause;
        for (const auto &[field, value] : spec->fields)
          if (field == "cause")
            if (const auto *text = std::get_if<std::string>(&value))
              cause = *text;
        if (const auto child =
                memberForPid(ctx, *exec.message->source))
          executor_.childStopping(arena, core, *self, *child,
                                  cause.empty() ? "stop" : cause);
        turn.message.reset();
      } else if (!spec->fields.empty()) {
        // A fielded wire percept injects through the same deliver
        // path --send uses — the request id rides +d.delivered's
        // dedupe, the pending entry dispatches in this turn's settle.
        // `__`-prefixed fields are the sender's transport metadata:
        // __kind/__correlation restore the staged record's semantics,
        // and the wire source maps to the member delivery records as
        // the belief's +d.source.
        std::string kind, correlation;
        std::vector<
            std::pair<std::string, zigzag::vortex::CellValue>>
            fields;
        for (const auto &f : spec->fields) {
          const auto text = std::get_if<std::string>(&f.second);
          if (f.first == "__kind") {
            if (text) kind = *text;
            continue;
          }
          if (f.first == "__correlation") {
            if (text) correlation = *text;
            continue;
          }
          fields.push_back(f);
        }
        const auto sender =
            memberForPid(ctx, *exec.message->source).value_or(noCell);
        executor_.injectPercept(arena, core, *self, spec->name,
                                fields, exec.message->requestId,
                                kind, correlation, sender);
        turn.message.reset();
      }
    }
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

  // The spawn request's source is the pid that asked — the member
  // $parent records. Only a first spawn reads it: a resume's request
  // source is the migrator, not the parent, so the durable
  // $parentPid text is the persistent name — minted here, then every
  // genesis rebinds the $parent scalar through it. The scalar itself
  // does not survive promote; the text and the member's directory
  // edges do.
  if (genesis.transported == nullptr && genesis.request != nullptr &&
      genesis.request->source && *genesis.request->source != genesis.pid) {
    CellRef parent =
        memberForPid(ctx, *genesis.request->source).value_or(noCell);
    if (parent == noCell) {
      parent            = arena.makeCell();
      const auto dirs   = core.findOrMintDimension("d.dirs");
      const auto agents = core.findOrMintDimension("d.agents");
      for (const CellRef dir :
           zigzag::rankAfter(arena, core.home(), dirs)) {
        zigzag::expectWritten(
            arena.link(zigzag::rankTail(arena, dir, agents), agents,
                       DimVector::POS, parent));
        break;
      }
      noteMemberPid(ctx, parent, *genesis.request->source);
    }
    if (const auto rec =
            varsRecord(arena, core, self, "$parentPid", true))
      zigzag::expectWritten(
          arena.link(*rec, core.dims().values, DimVector::POS,
                     arena.makeCell(renderPid(*genesis.request->source))));
  }
  // Rebind the scalar through the durable pid text — first-spawn and
  // resume both land here, so a publish-stale position renames the
  // member the text survived. A moved parent keeps this pointing at
  // the old incarnation until Moved{name}'s peer rebind reaches it —
  // member naming on the wire is the deferred half.
  if (const auto rec =
          varsRecord(arena, core, self, "$parentPid", false))
    if (const auto v =
            arena.linked(*rec, core.dims().values, DimVector::POS))
      if (const auto parentPid =
              parsePid(arena.textOf(*v)))
        if (const auto parent = memberForPid(ctx, *parentPid))
          if (const auto prec =
                  varsRecord(arena, core, self, "$parent", true)) {
            if (arena.linked(*prec, core.dims().values, DimVector::POS))
              zigzag::expectWritten(arena.unlink(
                  *prec, core.dims().values, DimVector::POS));
            zigzag::expectWritten(
                arena.link(*prec, core.dims().values, DimVector::POS,
                           refCell(arena, core, *parent)));
          }

  if (genesis.transported != nullptr) {
    executor_.resumed(arena, core, self);
    // §5.4's outbound half: the migrated agent announces Moved{name}
    // to its parent and its children — member-addressed sends the
    // next committed turn's drain wires to each member's recorded
    // pid. Space hosts are not materialized as members yet; they join
    // the set when the space registry exists.
    const auto selfKey = memberPidKey(arena, core, self);
    std::string selfName;
    if (const auto n = arena.linked(self, core.dims().name))
      selfName = std::string{arena.textOf(*n)};
    std::vector<CellRef> targets;
    if (const auto parent = parentMember(arena, core, self);
        parent != noCell)
      targets.push_back(parent);
    for (const CellRef peer : directoryMembers(arena, core))
      if (parentMember(arena, core, peer) == self)
        targets.push_back(peer);
    for (const CellRef target : targets)
      if (const auto key = memberPidKey(arena, core, target);
          key && key != selfKey)
        stageSend(ctx, self, target, "Moved",
                  {{"name", selfName}},
                  std::format("moved:{}", target));
  }
  return {};
}

void VagentAdapter::drain(zigzag::vortex::WorkerContext &ctx,
                          CellRef member) const {
  auto &arena = ctx.arena;
  auto &core  = ctx.core;
  const auto selfKey = memberPidKey(arena, core, member);
  // §3.7's remote arm: a member whose `pid` record names another
  // incarnation is wire-reachable — the send rides the same
  // `dispatched` mark the spawn arm uses (at-least-once by ordering,
  // the wire's own requestId dedupe keeps retries honest), and the
  // requestId carries the target's key so one broadcast can't
  // collide with itself across members.
  const auto remoteSend = [&](const OutboxEntry &entry,
                             const std::string &targetKey) {
    if (entry.cell == noCell ||
        varsRecord(arena, core, entry.cell, "dispatched", false))
      return;
    if (!sendSender_) {
      executor_.deliveryFailed(arena, core, entry, "transport");
      return;
    }
    const auto target = parsePid(targetKey);
    const auto source = selfKey ? parsePid(*selfKey) : std::nullopt;
    if (!target || !source) {
      executor_.deliveryFailed(arena, core, entry, "transport");
      return;
    }
    // The wire `value` is the percept's own term syntax — bare name
    // for field-less sends, `Name{f: v}` when the payload marshaled
    // fields the receiver's adapter parses back through
    // injectPercept. `__`-prefixed fields are transport metadata the
    // receiver strips before the percept lands: `__kind` keeps
    // tell/untell's belief effect and `__correlation` keeps an ask's
    // request id so the reply wakes the parked wait.
    const auto percept =
        entry.percept.empty() ? entry.payload : entry.percept;
    PerceptSpec spec{.name = percept};
    if (entry.payloadCell != noCell) {
      // The flat Name{f: v} codec admits only named scalar fields.
      // An unnamed child would silently drop off the arg list, a
      // +d.marshal-marked compound head leaks its own args into the
      // rank as apparent siblings — +d.step is one dimension, so the
      // marshal's mark is the only surviving signal — and a
      // relocation-marked Int64 is a live arena position that means
      // nothing off-node. Any of them renders a corrupt percept —
      // refuse the send honestly instead, the same deliveryFailed
      // arm transport faults take.
      const auto relocation =
          core.findOrMintDimension("d.mesh.relocation");
      const auto marshal = core.findOrMintDimension("d.marshal");
      for (const CellRef child :
           zigzag::rankAfter(arena, entry.payloadCell,
                             core.dims().step)) {
        const auto refMarked = [&] {
          const auto r =
              arena.linked(child, relocation, DimVector::POS);
          return r && arena.textOf(*r) == "cell";
        };
        if (!arena.linked(child, core.dims().name) ||
            arena.linked(child, marshal, DimVector::POS) ||
            refMarked()) {
          executor_.deliveryFailed(arena, core, entry,
                                   "unmarshalable");
          return;
        }
      }
      for (const CellRef child :
           zigzag::rankAfter(arena, entry.payloadCell,
                             core.dims().step)) {
        const auto n = arena.linked(child, core.dims().name);
        if (!n) continue;
        zigzag::vortex::CellValue v;
        if (const auto i = arena.asInt64(child))
          v = *i;
        else if (const auto d = arena.asDouble(child))
          v = *d;
        else if (const auto b = arena.asBool(child))
          v = *b;
        else
          v = std::string{arena.textOf(child)};
        spec.fields.emplace_back(std::string{arena.textOf(*n)}, v);
      }
    }
    // Only the kinds `deliver` routes into +d.inbox change what the
    // receiver does — send/emit/stop all arrive as percepts either
    // way, so `__kind` there would be wire noise.
    if (entry.kind == "tell" || entry.kind == "untell" ||
        entry.kind == "ask" || entry.kind == "achieve" ||
        entry.kind == "refuse" || entry.kind == "outcome" ||
        entry.kind == "cancel")
      spec.fields.emplace_back("__kind", entry.kind);
    if (!entry.correlation.empty())
      spec.fields.emplace_back("__correlation", entry.correlation);
    mesh::ActorMessage message;
    message.type      = MessageType::ActorSend;
    message.requestId = entry.id + ":" + targetKey;
    message.source    = *source;
    message.target    = *target;
    message.value     = renderPercept(spec.name, spec.fields);
    const auto sent   = sendSender_(ctx, message);
    if (!sent) {
      executor_.deliveryFailed(arena, core, entry, "transport");
      return;
    }
    if (const auto rec =
            varsRecord(arena, core, entry.cell, "dispatched", true))
      zigzag::expectWritten(
          arena.link(*rec, core.dims().values, DimVector::POS,
                     arena.makeCell("1")));
  };
  for (const auto &entry : VagentExecutor::drainOutbox(arena, core, member)) {
    if (entry.targetMember != noCell) {
      // Member-addressed entries bypass name routing entirely — the
      // target's pid record decides wire-vs-local.
      if (const auto targetKey =
              memberPidKey(arena, core, entry.targetMember);
          targetKey && targetKey != selfKey) {
        remoteSend(entry, *targetKey);
      } else {
        const auto percept =
            entry.percept.empty() ? entry.payload : entry.percept;
        executor_.deliver(
            arena, core,
            Delivery{.agent        = entry.targetMember,
                     .percept      = percept,
                     .id           = entry.id,
                     .kind         = entry.kind,
                     .payloadCell  = entry.payloadCell,
                     .correlation  = entry.correlation,
                     .sender       = entry.sender,
                     .delegator    = entry.delegator,
                     .fault        = entry.fault});
      }
      continue;
    }
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
    for (const auto &delivery : deliveries) {
      // A name can resolve to a remote peer — the member's `pid`
      // record naming another incarnation turns the delivery into a
      // wire send; the requestId's target key keeps a broadcast to
      // two remote members from colliding on the wire's dedupe.
      if (const auto targetKey =
              memberPidKey(arena, core, delivery.agent);
          targetKey && targetKey != selfKey) {
        remoteSend(entry, *targetKey);
      } else {
        executor_.deliver(arena, core, delivery);
      }
    }
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

void VagentAdapter::sendPercept(
    zigzag::vortex::WorkerContext &ctx, CellRef member,
    std::string_view name,
    const std::vector<std::pair<std::string,
                                zigzag::vortex::CellValue>> &fields,
    std::string_view id) const {
  executor_.injectPercept(ctx.arena, ctx.core, member, name, fields, id);
}

bool VagentAdapter::pendingWork(zigzag::vortex::WorkerContext &ctx,
                                CellRef member) const {
  auto &arena = ctx.arena;
  auto &core  = ctx.core;
  for (const char *name : {"d.pending", "d.inbox"})
    if (!zigzag::rankAfter(arena, member,
                           core.findOrMintDimension(name))
             .empty())
      return true;
  return false;
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
