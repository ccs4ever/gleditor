#include <cctype>
#include <chrono>
#include <expected>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/vagent/adapter.hpp"
#include "common/xanadu/vagent/decls.hpp"
#include "common/xanadu/vagent/frontend.hpp"
#include "common/xanadu/vagent/lineage.hpp"
#include "common/xanadu/vagent/token.hpp"
#include "common/xanadu/vortex/mesh_actor.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace {

using xanadu::vagent::VagentAdapter;
using zigzag::vortex::WorkerContext;
using zigzag::vortex::mesh::ActorError;
using zigzag::vortex::mesh::ActorExecution;
using zigzag::vortex::mesh::ActorGenesis;
using zigzag::vortex::mesh::ActorGrant;
using zigzag::vortex::mesh::ActorLineage;
using zigzag::vortex::mesh::ActorMessage;
using zigzag::vortex::mesh::ActorPid;
using zigzag::vortex::mesh::MeshActorRuntime;
using xanadu::identity::Fingerprint;
using xanadu::identity::MessageType;

void usage(std::ostream &out) {
  out << "Usage: vagent check FILE...\n"
         "       vagent run FILE [--send 'EVENT{f: v, ...}']...\n"
         "       vagent --help\n"
         "       vagent --version\n\n"
         "check validates each source unit without running it. run seeds a\n"
         "local lineage, spawns the program's agent on a single-node\n"
         "MeshActorRuntime, wakes it once so the Initialize handler runs,\n"
         "and pumps turns until the actor goes quiescent — no transport,\n"
         "so remote spawn/stop requests stay undispatched. Each --send\n"
         "delivers one fielded percept (ints, floats, bools, \"strings\")\n"
         "after genesis; a --send attaches to the FILE it follows.\n";
}

bool readSource(std::string_view path, std::string &source) {
  if (path == "-") {
    source.assign(std::istreambuf_iterator<char>(std::cin), {});
    if (std::cin.bad()) {
      std::cerr << "vagent: cannot read standard input\n";
      return false;
    }
    return true;
  }
  std::ifstream input{std::string(path)};
  if (!input) {
    std::cerr << path << ": cannot open source file\n";
    return false;
  }
  source.assign(std::istreambuf_iterator<char>(input), {});
  if (input.bad()) {
    std::cerr << path << ": cannot read source file\n";
    return false;
  }
  return true;
}

bool checkFile(std::string_view path) {
  std::string source;
  if (!readSource(path, source)) return false;

  // The staged frontend pipeline — parse → resolve → expand → check — owns
  // invocation order; expand is the seam later phases fill.
  auto program = xanadu::vagent::compile(source);
  if (!program) {
    for (const auto &diagnostic : program.error()) {
      std::cerr << path << ':' << xanadu::vagent::formatDiagnostic(diagnostic)
                << '\n';
    }
    return false;
  }
  std::cout << path << ": checked\n";
  return true;
}

// One `Name{f: v, ...}` percept the host stages after genesis — the
// fielded send the bare ActorSend `value` string cannot carry.
struct SendSpec {
  std::string name;
  std::vector<std::pair<std::string, zigzag::vortex::CellValue>> fields;
};

// `EVENT{f: v, ...}` or a bare `EVENT`: whitespace-tolerant, literals
// int / float / "string" / true / false. Field names and the event
// name are bare identifiers — the run-side counterpart of the
// record-literal syntax emit marshals.
std::optional<SendSpec> parseSendSpec(std::string_view text) {
  std::size_t pos   = 0;
  const auto ws     = [&] {
    while (pos < text.size() && std::isspace(text[pos])) ++pos;
  };
  const auto ident  = [&]() -> std::optional<std::string> {
    ws();
    const auto start = pos;
    if (pos >= text.size() ||
        !(std::isalpha(text[pos]) || text[pos] == '_'))
      return std::nullopt;
    while (pos < text.size() &&
           (std::isalnum(text[pos]) || text[pos] == '_'))
      ++pos;
    return std::string{text.substr(start, pos - start)};
  };
  const auto literal = [&]() -> std::optional<zigzag::vortex::CellValue> {
    ws();
    if (pos >= text.size()) return std::nullopt;
    if (text[pos] == '"') {
      const auto close = text.find('"', pos + 1);
      if (close == std::string_view::npos) return std::nullopt;
      const auto out = std::string{text.substr(pos + 1, close - pos - 1)};
      pos            = close + 1;
      return zigzag::vortex::CellValue{out};
    }
    if (text.substr(pos, 4) == "true") {
      pos += 4;
      return zigzag::vortex::CellValue{true};
    }
    if (text.substr(pos, 5) == "false") {
      pos += 5;
      return zigzag::vortex::CellValue{false};
    }
    const auto start = pos;
    if (text[pos] == '-') ++pos;
    bool dot = false;
    while (pos < text.size() &&
           (std::isdigit(text[pos]) || (text[pos] == '.' && !dot))) {
      dot |= text[pos] == '.';
      ++pos;
    }
    if (pos == start) return std::nullopt;
    const auto number = std::string{text.substr(start, pos - start)};
    try {
      if (dot) return zigzag::vortex::CellValue{std::stod(number)};
      return zigzag::vortex::CellValue{
          static_cast<std::int64_t>(std::stoll(number))};
    } catch (...) {
      return std::nullopt;
    }
  };
  SendSpec spec;
  const auto name = ident();
  if (!name) return std::nullopt;
  spec.name = *name;
  ws();
  if (pos == text.size()) return spec;
  if (text[pos] != '{') return std::nullopt;
  ++pos;
  ws();
  if (pos < text.size() && text[pos] == '}') {
    ++pos;
    ws();
    return pos == text.size() ? std::optional{spec} : std::nullopt;
  }
  for (;;) {
    const auto field = ident();
    if (!field) return std::nullopt;
    ws();
    if (pos >= text.size() || text[pos] != ':') return std::nullopt;
    ++pos;
    const auto value = literal();
    if (!value) return std::nullopt;
    spec.fields.emplace_back(*field, *value);
    ws();
    if (pos >= text.size()) return std::nullopt;
    if (text[pos] == '}') {
      ++pos;
      ws();
      return pos == text.size() ? std::optional{spec} : std::nullopt;
    }
    if (text[pos] != ',') return std::nullopt;
    ++pos;
  }
}

// The deadline sweep keys contexts by pid; MeshActorRuntime::pidKey is
// private, so the runner renders its own (the four fields are the
// identity, never a member index).
std::string pidKey(const ActorPid &pid) {
  return pid.node + ':' + pid.cursor + ':' + std::to_string(pid.epoch) + ':' +
         std::to_string(pid.spawnSequence);
}

bool runFile(std::string_view path, const std::vector<SendSpec> &sends) {
  std::string source;
  if (!readSource(path, source)) return false;

  auto program = xanadu::vagent::compile(source);
  if (!program) {
    for (const auto &diagnostic : program.error()) {
      std::cerr << path << ':' << xanadu::vagent::formatDiagnostic(diagnostic)
                << '\n';
    }
    return false;
  }

  // mint/file → fold → pin/inject → first-resolution: the program lowers
  // into a context over the filed vocabulary so dimension names resolve to
  // filed cells, then the whole image promotes into the lineage.
  xanadu::Store lineage;
  zigzag::ArenaManifold stage;
  auto filed =
      xanadu::vagent::fileDimensionVocabulary(lineage, stage, {});
  if (!filed) {
    std::cerr << path << ": " << filed.error() << '\n';
    return false;
  }
  WorkerContext compile{&filed->fold, &lineage};
  auto image = xanadu::vagent::lowerProgram(*program, compile.core, compile.vm);
  if (!image) {
    for (const auto &diagnostic : image.error()) {
      std::cerr << path << ':' << xanadu::vagent::formatDiagnostic(diagnostic)
                << '\n';
    }
    return false;
  }
  auto seeded = xanadu::vagent::seedProgramImage(lineage, compile.arena,
                                               image->programRoot);
  if (!seeded) {
    std::cerr << path << ": " << seeded.error() << '\n';
    return false;
  }
  auto fold = std::make_unique<zigzag::Manifold>(
      lineage.rebuildManifold(seeded->version));

  // A single-node runtime with a deterministic all-zero owner: everything
  // the runner signs (spawn, loopback deliveries) carries the same
  // fingerprint, which is what receive()'s owner check demands.
  const std::string node{"vagent-local"};
  MeshActorRuntime runtime{node, 1};
  VagentAdapter adapter;
  const Fingerprint identity{};
  const ActorPid client{node, "runner", 1, 1};

  // The runner is the bridge between the runtime's hooks and the adapter:
  // contexts seen in executor turns feed the deadline sweep, spawn-request
  // outcomes queue until the requester's next turn can drain them.
  std::map<std::string, std::pair<ActorPid, WorkerContext *>> actors;
  std::map<std::string,
           std::vector<std::pair<ActorMessage,
                                 std::expected<ActorPid, ActorError>>>>
      outcomes;

  runtime.setAuthorizer(
      [](const Fingerprint &, const ActorMessage &request)
          -> std::expected<ActorGrant, ActorError> {
        ActorGrant grant;
        grant.programRef       = request.programRef;
        grant.baseRef          = "local-lineage";
        grant.maxInstructions  = 1'000'000;
        grant.maxCells         = 1'000'000;
        grant.maxOutputBytes   = 32'768;
        grant.maxSolutions     = 64;
        grant.maxMailboxBytes  = 1u << 20;
        grant.expiresAt =
            std::chrono::system_clock::now() + std::chrono::hours{1};
        grant.immutable = true;
        return grant;
      });
  runtime.setExecutor([&](const ActorExecution &exec, WorkerContext &ctx)
                          -> std::expected<std::string, ActorError> {
    actors[pidKey(exec.pid)] = {exec.pid, &ctx};
    if (auto pending = outcomes.find(pidKey(exec.pid));
        pending != outcomes.end()) {
      for (const auto &[request, result] : pending->second)
        adapter.spawnOutcome(ctx, request, result);
      outcomes.erase(pending);
    }
    auto result = adapter(exec, ctx);
    if (!result)
      std::cerr << path << ": turn failed (actor down)\n";
    return result;
  });
  runtime.setGenesisHook([&](const ActorGenesis &genesis,
                             WorkerContext &ctx)
                             -> std::expected<void, ActorError> {
    if (const auto outcome = adapter.genesis(genesis, ctx); !outcome)
      return outcome;
    // The host's sends land once the member exists: each stages a
    // fielded percept on +d.pending, which the pump's pendingWork
    // sweep turns into a dispatch on a later turn.
    if (const auto member = adapter.memberForPid(ctx, genesis.pid))
      for (std::size_t i = 0; i < sends.size(); ++i)
        adapter.sendPercept(ctx, *member, sends[i].name,
                            sends[i].fields,
                            std::format("runner-send-{}", i));
    return {};
  });
  runtime.setSpawnResultHook(
      [&outcomes](const ActorMessage &request,
                  const std::expected<ActorPid, ActorError> &result) {
        if (request.source)
          outcomes[pidKey(*request.source)].emplace_back(request, result);
      });
  runtime.setSender([&](std::string_view destination,
                        const ActorMessage &message) -> bool {
    if (destination != node) return false;
    return runtime
        .receive(identity, message, std::chrono::steady_clock::now(),
                 std::chrono::system_clock::now())
        .has_value();
  });
  runtime.trustNode(node, identity);

  const auto wallNow = std::chrono::system_clock::now();
  ActorMessage spawnRequest;
  spawnRequest.type          = MessageType::ActorSpawn;
  spawnRequest.requestId     = "spawn-1";
  spawnRequest.source        = client;
  spawnRequest.programRef    = "vagent:" + std::string{path};
  spawnRequest.hashcashStamp = "local";
  spawnRequest.capability    = "local";
  auto pid = runtime.spawn(identity, spawnRequest, wallNow,
                           ActorLineage{fold.get(), &lineage});
  if (!pid) {
    std::cerr << path << ": spawn failed\n";
    return false;
  }

  // `on Initialize` runs on the first exec turn whatever it carries, so a
  // P4 wake is the honest boot — no percept needs to be fabricated.
  if (!runtime.wake(*pid)) {
    std::cerr << path << ": could not queue the actor's first turn\n";
    return false;
  }

  // Quiescence is a mailbox scan that finds nothing — except the two
  // backlogs only the adapter can see: parked deadlines, whose clock
  // advances through P4 wakes, and drained percepts waiting in the
  // member's own +d.pending/+d.inbox ranks.
  constexpr std::size_t maxTurns = 100'000;
  std::size_t turns              = 0;
  for (;;) {
    auto pumped = runtime.pumpOne(wallNow);
    if (!pumped) {
      std::cerr << path << ": turn failed\n";
      return false;
    }
    if (*pumped) {
      if (++turns >= maxTurns) {
        std::cerr << path << ": did not quiesce within " << maxTurns
                  << " turns\n";
        return false;
      }
      continue;
    }
    bool woke = false;
    for (const auto &[key, actor] : actors) {
      auto member = adapter.memberForPid(*actor.second, actor.first);
      if (!member) continue;
      if (adapter.nextDeadline(*actor.second, *member) ||
          adapter.pendingWork(*actor.second, *member)) {
        if (auto woken = runtime.wake(actor.first); !woken) continue;
        woke = true;
      }
    }
    if (!woke) break;
  }

  if (auto it = actors.find(pidKey(*pid)); it != actors.end())
    if (const auto member =
            adapter.memberForPid(*it->second.second, *pid)) {
      auto &arena = it->second.second->arena;
      auto &core  = it->second.second->core;
      const auto seen = core.findOrMintDimension("d.seen");
      for (const auto event : zigzag::rankAfter(arena, *member, seen))
        std::cout << path << ": saw " << arena.textOf(event) << '\n';
      // The member's +d.vars is a shared rank — a record belongs to
      // this agent only while its +d.owner scalar names it. `$` keys
      // are executor internals ($turn/$stats), not program state.
      const auto of = core.findOrMintDimension("d.owner");
      for (const auto rec : zigzag::rankAfter(arena, *member,
                                              core.dims().vars)) {
        const auto key = arena.textOf(rec);
        if (key.empty() || key[0] == '$') continue;
        if (const auto scope = arena.linked(rec, of))
          if (arena.asInt64(*scope).value_or(-1) !=
              static_cast<std::int64_t>(*member))
            continue;
        const auto value = arena.linked(rec, core.dims().values);
        std::cout << path << ": var " << key << " = ";
        if (!value) {
          std::cout << "(unset)\n";
        } else if (const auto b = arena.asBool(*value)) {
          std::cout << (*b ? "true" : "false") << '\n';
        } else if (const auto n = arena.asInt64(*value)) {
          std::cout << *n << '\n';
        } else if (const auto d = arena.asDouble(*value)) {
          std::cout << *d << '\n';
        } else {
          std::cout << '"' << arena.textOf(*value) << "\"\n";
        }
      }
    }
  std::cout << path << ": ran " << turns << " turns\n";
  return true;
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char *argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    usage(std::cout);
    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "--version") {
    std::cout << "vagent 0.2.0 (local runner)\n";
    return 0;
  }
  if (argc < 3 ||
      (std::string_view(argv[1]) != "check" &&
       std::string_view(argv[1]) != "run")) {
    usage(std::cerr);
    return 2;
  }
  const bool run = std::string_view(argv[1]) == "run";
  // A --send binds to the FILE before it: the arg walk groups each
  // file with the sends that trail it, then runs the pairs in order.
  std::vector<std::pair<std::string, std::vector<SendSpec>>> units;
  try {
    for (int index = 2; index < argc; ++index) {
      const std::string_view arg{argv[index]};
      if (arg == "--send") {
        if (!run || units.empty() || index + 1 >= argc) {
          usage(std::cerr);
          return 2;
        }
        const auto spec = parseSendSpec(argv[++index]);
        if (!spec) {
          std::cerr << "vagent: malformed --send '" << argv[index]
                    << "' — expected EVENT{f: v, ...}\n";
          return 2;
        }
        units.back().second.push_back(*spec);
        continue;
      }
      units.emplace_back(std::string{arg}, std::vector<SendSpec>{});
    }
    bool valid = true;
    for (const auto &[file, sends] : units)
      valid = (run ? runFile(file, sends) : checkFile(file)) && valid;
    return valid ? 0 : 1;
  } catch (const std::exception &error) {
    std::cerr << "vagent: " << error.what() << '\n';
    return 1;
  }
}
