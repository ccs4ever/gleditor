#include "common/xanadu/vortex/mesh_cli.hpp"

#include <charconv>
#include <fstream>
#include <iostream>
#include <set>
#include <thread>

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/vortex/mesh_epoch.hpp"
#include "common/xanadu/vortex/mesh_host.hpp"

namespace zigzag::vortex::mesh {
namespace {
using V = xanadu::bencode::Value;
std::string read(const std::filesystem::path &path, std::size_t maximum) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("mesh command: cannot open " + path.string());
  std::string bytes;
  char buffer[4096];
  while (input.read(buffer, sizeof(buffer)) || input.gcount()) {
    if (static_cast<std::size_t>(input.gcount()) > maximum - bytes.size())
      throw std::runtime_error("mesh command: file exceeds byte budget");
    bytes.append(buffer, static_cast<std::size_t>(input.gcount()));
  }
  if (!input.eof()) throw std::runtime_error("mesh command: file read failed");
  return bytes;
}
const V &field(const V &value, std::string_view name) {
  if (!value.isDict())
    throw std::runtime_error("mesh command: configuration dictionary required");
  const auto found = value.asDict().find(std::string(name));
  if (found == value.asDict().end())
    throw std::runtime_error("mesh command: missing configuration " +
                             std::string(name));
  return found->second;
}
std::uint64_t positive(std::string_view text) {
  std::uint64_t out{};
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), out);
  if (error != std::errc{} || end != text.data() + text.size() || out == 0 ||
      std::to_string(out) != text)
    throw std::runtime_error(
        "mesh command: positive canonical counter required");
  return out;
}
NodeId node(std::string_view text) {
  const auto a = text.find(':');
  const auto b = a == text.npos ? text.npos : text.find(':', a + 1);
  if (b == text.npos)
    throw std::runtime_error("mesh command: expected author:device:epoch");
  const auto fingerprint =
      xanadu::identity::Fingerprint::fromString(text.substr(0, a));
  const auto key = xanadu::PublicKey::parseHex(text.substr(a + 1, b - a - 1));
  if (!fingerprint || !key || key->isZero())
    throw std::runtime_error("mesh command: invalid worker identity");
  return {fingerprint->toString(), key->hex(), positive(text.substr(b + 1))};
}
std::uint64_t wallNow() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}
struct Launch {
  MeshHostOptions host;
  MeshTransportOptions transport;
  std::shared_ptr<xanadu::identity::IdentityNetworkController> controller;
  std::filesystem::path deployment;
  std::chrono::seconds timeout;
};
Launch load(const std::filesystem::path &configuration) {
  const auto bytes = read(configuration, 65536);
  const auto value = xanadu::bencode::decode(bytes);
  if (field(value, "version").asInteger() != 1)
    throw std::runtime_error("mesh command: unsupported deployment version");
  const auto directory = std::filesystem::absolute(configuration).parent_path();
  auto path            = [&](std::string_view key) {
    auto p = std::filesystem::path(field(value, key).asString());
    if (p.empty())
      throw std::runtime_error("mesh command: empty deployment path");
    return p.is_absolute() ? p : directory / p;
  };
  const auto author = xanadu::identity::Fingerprint::fromString(
      field(value, "author").asString());
  if (!author)
    throw std::runtime_error("mesh command: invalid author fingerprint");
  xanadu::MutableKeys keys;
  keys.publicKey =
      xanadu::PublicKey::fromHex(field(value, "device").asString());
  auto secret = read(path("secret"), 129);
  if (!secret.empty() && secret.back() == '\n') secret.pop_back();
  keys.secretKey = xanadu::SecretKey::fromHex(secret);
  // Challenge verification also refuses mismatched keys; reject them at
  // startup.
  const auto signature = xanadu::signMutableItem("mesh-key-check", keys);
  if (!xanadu::verifyMutableItem("mesh-key-check", signature, keys.publicKey))
    throw std::runtime_error("mesh command: device keypair mismatch");
  Launch result;
  result.controller =
      std::make_shared<xanadu::identity::IdentityNetworkController>(
          xanadu::identity::IdentityNetworkController::Options{
              .localFingerprint = *author, .localDeviceKeys = keys});
  for (const auto &entry : field(value, "delegations").asList()) {
    const auto relative = [&](std::string_view key) {
      const auto p = std::filesystem::path(field(entry, key).asString());
      return p.is_absolute() ? p : directory / p;
    };
    const auto delegation = xanadu::DeviceDelegation::fromTsv(
        read(relative("certificate"), 1024 * 1024));
    if (!delegation || !result.controller->trustDelegation(
                           *delegation, read(relative("master"), 1024 * 1024)))
      throw std::runtime_error("mesh command: unverified device delegation");
  }
  const auto ownKey = result.controller->deviceKeyFor(*author);
  if (!ownKey || *ownKey != keys.publicKey.bytes)
    throw std::runtime_error(
        "mesh command: local device requires a verified delegation");
  std::set<std::string> allowed;
  for (const auto &entry : field(value, "allow_compute").asList()) {
    const auto fingerprint =
        xanadu::identity::Fingerprint::fromString(entry.asString());
    if (!fingerprint)
      throw std::runtime_error("mesh command: invalid compute principal");
    allowed.insert(fingerprint->toString());
  }
  result.host.worker.authorize =
      [allowed = std::move(allowed), controller = result.controller](
          const ImmutableJob &job, const CapabilityGrant &grant) {
        const auto requester =
            xanadu::identity::Fingerprint::fromString(job.requester.authorKey);
        const auto issuer =
            requester ? controller->deviceKeyFor(*requester) : std::nullopt;
        return allowed.contains(job.requester.authorKey) && issuer &&
               *issuer == grant.issuer.bytes;
      };
  auto &worker           = result.host.worker;
  worker.keys            = keys;
  worker.node            = {author->toString(), keys.publicKey.hex(), 0};
  result.deployment      = path("deployment");
  worker.seedRoot        = path("seeds");
  result.host.resultRoot = path("results");
  worker.permascroll     = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir     = path("permascroll"),
                                          .masterIdentity = *author,
                                          .deviceKeys     = keys});
  const auto &budget = field(value, "budget");
  auto count         = [&](std::string_view key) {
    const auto n = field(budget, key).asInteger();
    if (n <= 0)
      throw std::runtime_error(
          "mesh command: positive execution budget required");
    return static_cast<std::uint64_t>(n);
  };
  worker.budget = {count("cycles"), count("cells"), count("solutions"),
                   count("output")};
  xanadu::Author who{field(value, "name").asString(),
                     field(value, "email").asString(), author->toString()};
  if (const auto signingKey = value.find("gpg_key"))
    who.signingKey = signingKey->asString();
  if (!who.named())
    throw std::runtime_error("mesh command: named signing author required");
  xanadu::SigningOptions signing;
  if (const auto home = value.find("gpg_home")) {
    const auto p    = std::filesystem::path(home->asString());
    signing.gpgHome = (p.is_absolute() ? p : directory / p).string();
  }
  worker.provenance = [who, signing, keys](xanadu::Store &store)
      -> std::expected<xanadu::SignedProvenance, std::string> {
    try {
      xanadu::Provenance record;
      record.author       = who;
      record.title        = "Vortex distributed result";
      record.publisher    = keys.publicKey.hex();
      record.permascroll  = store.userPermascroll().globalScrollKey();
      record.published    = wallNow();
      const auto versions = store.currentVersions();
      if (!versions.empty()) record.version = versions.back().str();
      record.contentDigest = xanadu::sha256Hex({});
      const auto ops       = xanadu::sealableOps(store);
      record.opsLength     = ops.size();
      record.opsDigest     = xanadu::sha256Hex(ops);
      return xanadu::signProvenance(record, signing);
    } catch (const std::exception &error) {
      return std::unexpected(error.what());
    }
  };
  worker.primediaProvenance =
      [who, signing, keys](xanadu::Store &, std::string_view bytes,
                           std::uint64_t start, std::string_view scroll)
      -> std::expected<xanadu::SignedProvenance, std::string> {
    try {
      xanadu::Provenance record;
      record.author        = who;
      record.title         = "Vortex author primedia segment";
      record.publisher     = keys.publicKey.hex();
      record.permascroll   = std::string(scroll);
      record.published     = wallNow();
      record.contentLength = bytes.size();
      record.contentDigest = xanadu::sha256Hex(bytes);
      record.extra.emplace_back("primedia_start", std::to_string(start));
      return xanadu::signProvenance(record, signing);
    } catch (const std::exception &error) {
      return std::unexpected(error.what());
    }
  };
  result.transport.downloads              = path("downloads");
  result.transport.swarm.listenInterfaces = field(value, "listen").asString();
  result.transport.swarm.enableLocalDiscovery = false;
  result.transport.swarm.enableTrackers       = false;
  result.transport.swarm.enableDht = field(value, "dht").asInteger() != 0;
  result.transport.swarm.allowManyConnectionsPerAddress = true;
  for (const auto &peer : field(value, "peers").asList()) {
    const auto port = field(peer, "port").asInteger();
    if (port <= 0 || port > 65535)
      throw std::runtime_error("mesh command: invalid peer port");
    result.transport.peers.emplace_back(field(peer, "address").asString(),
                                        static_cast<std::uint16_t>(port));
  }
  const auto seconds = field(value, "timeout").asInteger();
  if (seconds <= 0 || seconds > 86400)
    throw std::runtime_error("mesh command: timeout outside 1..86400 seconds");
  result.timeout = std::chrono::seconds(seconds);
  return result;
}
} // namespace

std::optional<int> runMeshCommand(Frontend frontend, int argc, char **argv) {
  if (argc < 2) return {};
  const std::string_view command(argv[1]);
  if (!command.starts_with("--mesh-")) return {};
  try {
    if (command == "--mesh-help") {
      std::cout << "--mesh-worker CONFIG SECONDS\n--mesh-run CONFIG "
                   "AUTHOR:DEVICE:EPOCH SOURCE\n"
                   "CONFIG is a version-1 bencoded deployment; see "
                   "design/vortex-mesh-cli.md.\n";
      return 0;
    }
    const bool worker = command == "--mesh-worker";
    if ((!worker && command != "--mesh-run") || argc != (worker ? 4 : 5))
      throw std::runtime_error(
          "mesh command: use --mesh-help for command syntax");
    auto launch            = load(argv[2]);
    const auto fingerprint = *xanadu::identity::Fingerprint::fromString(
        launch.host.worker.node.authorKey);
    auto reserved = reserveNodeId(launch.deployment, fingerprint,
                                  launch.host.worker.keys.publicKey);
    if (!reserved) throw std::runtime_error(reserved.error());
    launch.host.worker.node = *reserved;
    launch.host.identity    = launch.controller;
    const auto keys         = launch.host.worker.keys;
    const auto budget       = launch.host.worker.budget;
    const auto seedRoot     = launch.host.worker.seedRoot;
    auto transport = std::make_shared<MeshSwarmTransport>(launch.controller,
                                                          launch.transport);
    std::optional<ProgramSeed> program;
    if (!worker) {
      auto sealed = publishProgram(
          read(argv[4], launch.host.worker.maxProgramBytes), seedRoot);
      if (!sealed) throw std::runtime_error(sealed.error());
      program.emplace(std::move(*sealed));
      transport->source().addTorrent(program->torrent.file,
                                     program->path.string(), true);
    }
    launch.host.transport = makeSwarmHostTransport(transport);
    MeshHost host(std::move(launch.host));
    std::cout << "mesh-node " << host.node().str() << std::endl;
    if (worker) {
      const auto duration = positive(argv[3]);
      if (duration > 86400)
        throw std::runtime_error(
            "mesh command: worker duration exceeds one day");
      std::this_thread::sleep_for(std::chrono::seconds(duration));
      return 0;
    }
    ImmutableJob job{.program   = program->torrent.hash,
                     .frontend  = frontend,
                     .budget    = budget,
                     .requester = host.node()};
    CapabilityGrant grant{.job       = jobHash(job),
                          .requester = host.node(),
                          .worker    = node(argv[3]),
                          .expires   = wallNow() + static_cast<std::uint64_t>(
                                                     launch.timeout.count()),
                          .budget = budget,
                          .issuer = keys.publicKey};
    const auto stampTime = wallNow();
    auto nonce           = xanadu::identity::HashcashEngine::mint(
        grant.job, stampTime, xanadu::identity::kDefaultHashcashDifficulty);
    if (!nonce) throw std::runtime_error("mesh command: Hashcash mint failed");
    grant.proof     = {.resource  = grant.job,
                       .timestamp = stampTime,
                       .nonce     = *nonce,
                       .difficultyBits =
                           xanadu::identity::kDefaultHashcashDifficulty};
    grant.signature = xanadu::signMutableItem(grantSigningBuffer(grant), keys);
    const auto dispatched = host.dispatch(job, grant, true);
    if (!dispatched) throw std::runtime_error(dispatched.error());
    const auto deadline = std::chrono::steady_clock::now() + launch.timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto state = host.poll(*dispatched);
      if (state && state->state == FutureState::Failed)
        throw std::runtime_error(state->error);
      if (state && state->state == FutureState::Succeeded) {
        std::cout << "mesh-store " << state->store.string() << '\n';
        for (const auto &hash : state->results)
          std::cout << "mesh-result " << hash.hex() << '\n';
        return 0;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("mesh command: result deadline expired");
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
} // namespace zigzag::vortex::mesh
