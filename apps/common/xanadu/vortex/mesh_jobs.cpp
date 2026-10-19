#include "common/xanadu/vortex/mesh_jobs.hpp"

#include <charconv>
#include <limits>
#include <stdexcept>

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/resolver.hpp"
#include "common/xanadu/store.hpp"

namespace zigzag::vortex::mesh {
namespace {
using V                                = xanadu::bencode::Value;
constexpr std::string_view programFile = "vortex-program";
constexpr std::size_t maxEnvelope      = 16 * 1024;

V number(std::uint64_t n) { return V::string(std::to_string(n)); }
const V &field(const V &v, std::string_view key) {
  if (!v.isDict()) throw std::runtime_error("mesh job: dictionary expected");
  const auto found = v.asDict().find(std::string(key));
  if (found == v.asDict().end())
    throw std::runtime_error("mesh job: missing field");
  return found->second;
}
const std::string &string(const V &v, std::string_view key) {
  const auto &result = field(v, key);
  if (!result.isString()) throw std::runtime_error("mesh job: string expected");
  return result.asString();
}
std::uint64_t integer(const V &v, std::string_view key) {
  const auto &s = string(v, key);
  std::uint64_t n{};
  const auto parsed = std::from_chars(s.data(), s.data() + s.size(), n);
  if (parsed.ec != std::errc{} || parsed.ptr != s.data() + s.size() ||
      std::to_string(n) != s)
    throw std::runtime_error("mesh job: invalid unsigned integer");
  return n;
}
void shape(const V &v, std::size_t count) {
  if (!v.isDict() || v.asDict().size() != count)
    throw std::runtime_error("mesh job: invalid dictionary shape");
}
V node(const NodeId &n) {
  return V::dict({{"author", V::string(n.authorKey)},
                  {"device", V::string(n.deviceKey)},
                  {"epoch", number(n.arenaEpoch)}});
}
NodeId readNode(const V &v) {
  shape(v, 3);
  NodeId n{string(v, "author"), string(v, "device"), integer(v, "epoch")};
  const auto author = xanadu::identity::Fingerprint::fromString(n.authorKey);
  const auto device = xanadu::PublicKey::parseHex(n.deviceKey);
  if (!author || author->toString() != n.authorKey || !device ||
      device->isZero() || device->hex() != n.deviceKey)
    throw std::runtime_error("mesh job: invalid node identity");
  return n;
}
V budget(const EvaluationBudget &b) {
  return V::dict({{"cycles", number(b.maxCycles)},
                  {"cells", number(b.maxCells)},
                  {"solutions", number(b.maxSolutions)},
                  {"output", number(b.maxOutputBytes)}});
}
EvaluationBudget readBudget(const V &v) {
  shape(v, 4);
  const auto size = [&](std::string_view key) {
    const auto n = integer(v, key);
    if (n == 0 || n > std::numeric_limits<std::size_t>::max())
      throw std::runtime_error("mesh job: invalid budget");
    return static_cast<std::size_t>(n);
  };
  return {size("cycles"), size("cells"), size("solutions"), size("output")};
}
bool within(const EvaluationBudget &a, const EvaluationBudget &b) {
  return a.maxCycles > 0 && a.maxCycles <= b.maxCycles && a.maxCells > 0 &&
         a.maxCells <= b.maxCells && a.maxSolutions > 0 &&
         a.maxSolutions <= b.maxSolutions && a.maxOutputBytes > 0 &&
         a.maxOutputBytes <= b.maxOutputBytes;
}
V grantBody(const CapabilityGrant &g) {
  return V::dict(
      {{"version", V::integer(1)},
       {"job", V::string(g.job)},
       {"requester", node(g.requester)},
       {"worker", node(g.worker)},
       {"expires", number(g.expires)},
       {"budget", budget(g.budget)},
       {"issuer", V::string(g.issuer.hex())},
       {"proof", V::dict({{"resource", V::string(g.proof.resource)},
                          {"timestamp", number(g.proof.timestamp)},
                          {"nonce", number(g.proof.nonce)},
                          {"difficulty", number(g.proof.difficultyBits)}})}});
}
V envelope(std::string_view bytes, std::size_t count) {
  if (bytes.size() > maxEnvelope)
    throw std::runtime_error("mesh job: envelope budget exceeded");
  auto v = xanadu::bencode::decode(bytes);
  shape(v, count);
  if (v.encode() != bytes || !field(v, "version").isInteger() ||
      field(v, "version").asInteger() != 1)
    throw std::runtime_error("mesh job: invalid canonical envelope/version");
  return v;
}
} // namespace

std::string encodeJob(const ImmutableJob &job) {
  return V::dict(
             {{"version", V::integer(1)},
              {"program", V::string(job.program.hex())},
              {"frontend", number(static_cast<std::uint64_t>(job.frontend))},
              {"base", V::string(job.base ? job.base->hex() : "")},
              {"parent", V::string(job.parent.str())},
              {"budget", budget(job.budget)},
              {"requester", node(job.requester)}})
      .encode();
}
std::string jobHash(const ImmutableJob &job) {
  return xanadu::sha256Hex("xanadu:vortex-job:1:" + encodeJob(job));
}
std::expected<ImmutableJob, std::string> decodeJob(std::string_view bytes) {
  try {
    const auto v = envelope(bytes, 7);
    ImmutableJob job;
    job.program         = xanadu::InfoHash::fromHex(string(v, "program"));
    const auto frontend = integer(v, "frontend");
    if (frontend > static_cast<std::uint64_t>(Frontend::Vlace) ||
        job.program.isZero())
      throw std::runtime_error("mesh job: invalid program/frontend");
    job.frontend = static_cast<Frontend>(frontend);
    if (!string(v, "base").empty()) {
      job.base = xanadu::InfoHash::fromHex(string(v, "base"));
      if (job.base->isZero())
        throw std::runtime_error("mesh job: zero base hash");
    }
    job.parent    = xanadu::MicroversionId::parse(string(v, "parent"));
    job.budget    = readBudget(field(v, "budget"));
    job.requester = readNode(field(v, "requester"));
    if ((!job.base && !job.parent.isZero()) || encodeJob(job) != bytes)
      throw std::runtime_error(
          "mesh job: noncanonical job or parent without base");
    return job;
  } catch (const std::exception &e) {
    return std::unexpected(e.what());
  }
}
std::expected<ProgramSeed, std::string>
publishProgram(std::string_view source, const std::filesystem::path &seedRoot) {
  try {
    if (seedRoot.empty() || source.empty())
      return std::unexpected("mesh job: missing source or seed directory");
    const std::vector<xanadu::TorrentContent> files{
        {.path = std::string(programFile), .data = std::string(source)}};
    auto torrent    = xanadu::makeTorrent(files, "vortex-program");
    const auto path = xanadu::writeTorrentSeed(seedRoot, torrent, files);
    return ProgramSeed{std::move(torrent), path};
  } catch (const std::exception &e) {
    return std::unexpected(e.what());
  }
}
std::string grantSigningBuffer(const CapabilityGrant &grant) {
  return "xanadu:vortex-grant:1:" + grantBody(grant).encode();
}
std::string encodeGrant(const CapabilityGrant &grant) {
  auto fields = grantBody(grant).asDict();
  fields.emplace("signature", V::string(grant.signature.hex()));
  return V::dict(std::move(fields)).encode();
}
std::expected<CapabilityGrant, std::string>
decodeGrant(std::string_view bytes) {
  try {
    const auto v = envelope(bytes, 9);
    CapabilityGrant g;
    g.job         = string(v, "job");
    g.requester   = readNode(field(v, "requester"));
    g.worker      = readNode(field(v, "worker"));
    g.expires     = integer(v, "expires");
    g.budget      = readBudget(field(v, "budget"));
    g.issuer      = xanadu::PublicKey::fromHex(string(v, "issuer"));
    g.signature   = xanadu::Signature::fromHex(string(v, "signature"));
    const auto &p = field(v, "proof");
    shape(p, 4);
    g.proof.resource      = string(p, "resource");
    g.proof.timestamp     = integer(p, "timestamp");
    g.proof.nonce         = integer(p, "nonce");
    const auto difficulty = integer(p, "difficulty");
    if (difficulty > 255)
      throw std::runtime_error("mesh job: invalid proof difficulty");
    g.proof.difficultyBits = static_cast<std::uint8_t>(difficulty);
    if (encodeGrant(g) != bytes)
      throw std::runtime_error("mesh job: noncanonical grant");
    return g;
  } catch (const std::exception &e) {
    return std::unexpected(e.what());
  }
}
std::expected<void, std::string>
verifyGrant(const ImmutableJob &job, const CapabilityGrant &grant,
            const NodeId &worker, const xanadu::identity::Fingerprint &peer,
            const DelegatedKeyVerifier &delegation,
            const JobAuthorizer &authorize, std::uint64_t now) {
  try {
    if (!decodeJob(encodeJob(job)))
      return std::unexpected("mesh job: invalid immutable job");
    if (grant.job != jobHash(job) || grant.requester != job.requester ||
        grant.worker != worker || now >= grant.expires ||
        !within(job.budget, grant.budget))
      return std::unexpected("mesh job: grant scope, expiry or budget refused");
    if (!delegation || peer.toString() != job.requester.authorKey)
      return std::unexpected("mesh job: unauthenticated requester");
    const auto key = delegation(peer);
    if (!key || key->hex() != job.requester.deviceKey)
      return std::unexpected("mesh job: device delegation refused");
    if (grant.issuer.isZero() ||
        !xanadu::verifyMutableItem(grantSigningBuffer(grant), grant.signature,
                                   grant.issuer))
      return std::unexpected("mesh job: invalid capability signature");
    if (!authorize || !authorize(job, grant))
      return std::unexpected(
          "mesh job: issuer trust/payment authorization refused");
    return {};
  } catch (const std::exception &e) {
    return std::unexpected(e.what());
  }
}

FutureScheduler::FutureScheduler(JobWorkerOptions options)
    : options_(std::move(options)),
      thread_([this](std::stop_token stop) { work(stop); }) {}
FutureScheduler::~FutureScheduler() {
  thread_.request_stop();
  ready_.notify_all();
}
std::expected<std::string, std::string>
FutureScheduler::submit(const ImmutableJob &job, const CapabilityGrant &grant,
                        const xanadu::identity::Fingerprint &peer,
                        std::uint64_t now) {
  if (auto verified = verifyGrant(job, grant, options_.node, peer,
                                  options_.delegation, options_.authorize, now);
      !verified)
    return std::unexpected(verified.error());
  if (!within(job.budget, options_.budget))
    return std::unexpected("mesh job: worker budget exceeded");
  const auto hash = jobHash(job);
  std::lock_guard lock(mutex_);
  if (retained_.contains(hash)) return hash;
  if (!options_.source || !options_.permascroll || !options_.provenance ||
      !options_.seed || !options_.monotonicNow ||
      options_.keys.publicKey.hex() != options_.node.deviceKey)
    return std::unexpected(
        "mesh job: worker publication/seed configuration refused");
  if (queue_.size() >= options_.maxQueued ||
      retained_.size() >= options_.maxRetained)
    return std::unexpected("mesh job: queue or retained budget exceeded");
  if (grant.proof.resource != hash ||
      !hashcash_.verify(hash, grant.proof.timestamp, grant.proof.nonce,
                        grant.proof.difficultyBits, options_.hashcashDifficulty,
                        now))
    return std::unexpected("mesh job: Hashcash refused");
  retained_.emplace(hash, Entry{.job          = job,
                                .grant        = grant,
                                .peer         = peer,
                                .admittedWall = now,
                                .admittedAt   = options_.monotonicNow(),
                                .snapshot     = {.job = hash},
                                .seed         = {}});
  queue_.push_back(hash);
  ready_.notify_one();
  return hash;
}
std::optional<FutureSnapshot>
FutureScheduler::poll(std::string_view hash) const {
  std::lock_guard lock(mutex_);
  const auto found = retained_.find(std::string(hash));
  if (found == retained_.end()) return std::nullopt;
  return found->second.snapshot;
}
bool FutureScheduler::forget(std::string_view hash) {
  std::lock_guard lock(mutex_);
  const auto found = retained_.find(std::string(hash));
  if (found == retained_.end() ||
      (found->second.snapshot.state != FutureState::Succeeded &&
       found->second.snapshot.state != FutureState::Failed))
    return false;
  retained_.erase(found);
  return true;
}
std::expected<ValueSeed, std::string>
FutureScheduler::execute(const ImmutableJob &job) {
  try {
    const auto meta = options_.source->metainfo(job.program);
    if (!meta || meta->hash() != job.program || meta->files().size() != 1 ||
        meta->files().front().path != programFile ||
        meta->totalLength() > options_.maxProgramBytes)
      return std::unexpected(
          "mesh job: program metadata missing or over budget");
    const auto &file = meta->files().front();
    xanadu::Resolver resolver(options_.source.get());
    const auto program = resolver.resolve(
        xanadu::Scroll::ofTorrentFile(job.program, 0, file.path, file.offset,
                                      file.length),
        {.start = 0, .length = file.length});
    if (!program.isVerified() || program.text.size() != file.length)
      return std::unexpected("mesh job: missing/corrupt program pieces");
    std::optional<ReceivedValue> base;
    std::unique_ptr<xanadu::Store> empty;
    xanadu::Store *store = nullptr;
    if (job.base) {
      auto received = fetchValue(*job.base, *options_.source,
                                 options_.permascroll, options_.values);
      if (!received) return std::unexpected(received.error());
      if (received->value.publication.version != job.parent)
        return std::unexpected("mesh job: base parent mismatch");
      base.emplace(std::move(*received));
      store = base->store.get();
    } else {
      empty = std::make_unique<xanadu::Store>(options_.permascroll);
      store = empty.get();
    }
    auto answer = evaluate(job.frontend, program.text, job.budget,
                           base ? base->manifold.get() : nullptr, store);
    if (!answer) return std::unexpected(answer.error());
    auto result = publishValue(
        *store, job.parent, answer->context->arena, answer->root, options_.keys,
        options_.node, options_.provenance, options_.seedRoot, options_.values,
        jobHash(job), {}, options_.primediaProvenance);
    if (!result) return std::unexpected(result.error());
    if (auto seeded = options_.seed(*result); !seeded)
      return std::unexpected(seeded.error());
    return result;
  } catch (const std::exception &e) {
    return std::unexpected(e.what());
  }
}
void FutureScheduler::work(std::stop_token stop) {
  while (!stop.stop_requested()) {
    std::string hash;
    std::optional<Entry> admitted;
    {
      std::unique_lock lock(mutex_);
      if (!ready_.wait(lock, stop, [&] { return !queue_.empty(); })) return;
      hash = std::move(queue_.front());
      queue_.pop_front();
      auto &entry          = retained_.at(hash);
      entry.snapshot.state = FutureState::Running;
      admitted             = entry;
    }
    auto result = [&]() -> std::expected<ValueSeed, std::string> {
      try {
        const auto &admission = *admitted;
        std::optional<std::string> refusal;
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                 options_.monotonicNow() - admission.admittedAt)
                                 .count();
        if (elapsed < 0 || static_cast<std::uint64_t>(elapsed) >
                               std::numeric_limits<std::uint64_t>::max() -
                                   admission.admittedWall) {
          refusal = "mesh job: admission clock invalid";
        } else if (auto verified = verifyGrant(
                       admission.job, admission.grant, options_.node,
                       admission.peer, options_.delegation, options_.authorize,
                       admission.admittedWall +
                           static_cast<std::uint64_t>(elapsed));
                   !verified) {
          refusal = verified.error();
        }
        return refusal ? std::expected<ValueSeed, std::string>(
                             std::unexpected(*refusal))
                       : execute(admission.job);
      } catch (const std::exception &error) {
        return std::unexpected(error.what());
      }
    }();
    std::lock_guard lock(mutex_);
    auto &entry = retained_.at(hash);
    if (result) {
      entry.seed            = std::make_shared<ValueSeed>(std::move(*result));
      entry.snapshot.result = entry.seed->carrier.hash;
      entry.snapshot.value  = entry.seed->value;
      if (auto bundle = packSpoolSliceBundle(*entry.seed, options_.values)) {
        entry.snapshot.spoolBundle = encodeSpoolSliceBundle(*bundle);
      }
      entry.snapshot.state = FutureState::Succeeded;
    } else {
      entry.snapshot.error = result.error();
      entry.snapshot.state = FutureState::Failed;
    }
  }
}
} // namespace zigzag::vortex::mesh
