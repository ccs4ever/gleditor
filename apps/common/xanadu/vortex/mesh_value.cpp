#include "common/xanadu/vortex/mesh_value.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <tuple>

#include <blake3.h>

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/foreign_op_handle.hpp"
#include "common/xanadu/resolver.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace zigzag::vortex::mesh {
namespace {
using V                                 = xanadu::bencode::Value;
constexpr std::string_view manifestName = "vortex-value.bencode";

std::string historySalt(const xanadu::DocumentId &document) {
  return "vortex-history-" + xanadu::sha256Hex(document.str()).substr(0, 48);
}

V body(const PublishedValue &value) {
  return V::dict({
      {"author", V::string(value.origin.authorKey)},
      {"continuation", V::string(value.continuation)},
      {"device", V::string(value.origin.deviceKey)},
      {"job", V::string(value.job)},
      {"epoch", V::string(std::to_string(value.origin.arenaEpoch))},
      {"parent", V::string(value.parent.str())},
      {"publication", V::string(xanadu::encodePublication(value.publication))},
      {"root", V::string(value.root.produces.str())},
      {"scroll", V::string(value.root.scroll)},
      {"version", V::integer(1)},
  });
}

std::expected<void, std::string> check(const PublishedValue &value,
                                       const ValueLimits limits) {
  if (value.origin.authorKey.empty() ||
      value.origin.deviceKey != value.publication.publisher.hex())
    return std::unexpected(
        "mesh value: publisher does not match origin device");
  const auto history = value.publication.scrolls.find(value.root.scroll);
  if (history == value.publication.scrolls.end() ||
      history->second.publisher != value.publication.publisher ||
      history->second.salt != historySalt(value.publication.storeId) ||
      value.root.produces.isZero())
    return std::unexpected(
        "mesh value: root has no published operation identity");
  if (!xanadu::verifyPublication(value.publication) ||
      !xanadu::verifyMutableItem(valueSigningBuffer(value), value.signature,
                                 value.publication.publisher))
    return std::unexpected("mesh value: signature verification failed");
  std::uint64_t operations = 0;
  std::uint64_t payload    = 0;
  for (const auto &segment : value.publication.opsSegments) {
    if (segment.length > limits.operations - std::min<std::uint64_t>(
                                                 operations, limits.operations))
      return std::unexpected("mesh value: operation budget exceeded");
    operations += segment.length;
  }
  if (operations == 0)
    return std::unexpected("mesh value: missing operation history");
  std::set<std::tuple<xanadu::InfoHash, std::uint64_t, std::uint64_t>> counted;
  for (const auto &[key, scroll] : value.publication.scrolls) {
    (void)key;
    for (const auto &segment : scroll.segments) {
      if (!counted
               .emplace(segment.torrent, segment.streamOffset, segment.length)
               .second)
        continue;
      if (segment.length >
          limits.payloadBytes - std::min(payload, limits.payloadBytes))
        return std::unexpected("mesh value: payload budget exceeded");
      payload += segment.length;
    }
  }
  return {};
}

} // namespace

std::string valueSigningBuffer(const PublishedValue &value) {
  return "xanadu:vortex-value:1:" + body(value).encode();
}

std::string encodeValue(const PublishedValue &value) {
  auto fields = body(value).asDict();
  fields.emplace("signature",
                 V::string(std::string{reinterpret_cast<const char *>(
                                           value.signature.bytes.data()),
                                       value.signature.bytes.size()}));
  return V::dict(std::move(fields)).encode();
}

std::expected<PublishedValue, std::string>
decodeValue(const std::string_view bytes, const ValueLimits limits) {
  if (bytes.size() > limits.manifestBytes)
    return std::unexpected("mesh value: manifest budget exceeded");
  try {
    const auto decoded = xanadu::bencode::decode(bytes);
    if (!decoded.isDict() || decoded.asDict().size() != 11 ||
        decoded.encode() != bytes)
      return std::unexpected("mesh value: invalid canonical envelope");
    const auto field = [&](std::string_view key) -> const std::string & {
      const auto found = decoded.find(key);
      if (!found || !found->isString())
        throw std::runtime_error("mesh value: missing string field " +
                                 std::string(key));
      return found->asString();
    };
    const auto version = decoded.find("version");
    if (!version || !version->isInteger() || version->asInteger() != 1)
      return std::unexpected("mesh value: unsupported manifest version");
    PublishedValue value;
    value.job               = field("job");
    value.continuation      = field("continuation");
    value.origin.authorKey  = field("author");
    value.origin.deviceKey  = field("device");
    const auto &epoch       = field("epoch");
    std::size_t used        = 0;
    value.origin.arenaEpoch = std::stoull(epoch, &used);
    if (used != epoch.size() ||
        std::to_string(value.origin.arenaEpoch) != epoch)
      return std::unexpected("mesh value: invalid arena epoch");
    value.parent           = xanadu::MicroversionId::parse(field("parent"));
    value.root             = {.scroll   = field("scroll"),
                              .produces = xanadu::MicroversionId::parse(field("root"))};
    const auto publication = xanadu::decodePublication(field("publication"));
    if (!publication)
      return std::unexpected(
          "mesh value: invalid publication signature or shape");
    value.publication     = *publication;
    const auto &signature = field("signature");
    if (signature.size() != value.signature.bytes.size())
      return std::unexpected("mesh value: invalid signature length");
    std::memcpy(value.signature.bytes.data(), signature.data(),
                signature.size());
    if (auto valid = check(value, limits); !valid)
      return std::unexpected(valid.error());
    return value;
  } catch (const std::exception &error) {
    return std::unexpected(std::string{"mesh value: "} + error.what());
  }
}

static std::expected<ValueSeed, std::string> publishValueImpl(
    xanadu::Store &store, const xanadu::MicroversionId &parent,
    const ArenaManifold &arena, const CellRef root,
    const xanadu::MutableKeys &keys, NodeId origin,
    const ValueProvenanceSigner &signer, const std::filesystem::path &seedRoot,
    const ValueLimits limits, std::string job, std::string continuation,
    const ValuePrimediaProvenanceSigner &primediaSigner,
    bool allowPresignedFixture) {
  if (origin.authorKey.empty() || origin.deviceKey != keys.publicKey.hex())
    return std::unexpected("mesh value: signing device does not match origin");
  if (!signer || seedRoot.empty())
    return std::unexpected(
        "mesh value: signed provenance and seed directory required");
  try {
    const auto promoted =
        promote(store, parent, arena, root, {limits.operations});
    if (!promoted || promoted->cells.empty())
      return std::unexpected("mesh value: promotion refused");
    store.sealMetadata();
    const auto provenance = signer(store);
    if (!provenance || provenance->tsv.empty() || provenance->signature.empty())
      return std::unexpected(provenance ? "mesh value: empty signed provenance"
                                        : provenance.error());
    ValueSeed seed;
    const auto priorPrimedia    = store.userPermascroll().currentScroll();
    const auto priorPrimediaKey = xanadu::scrollKey(priorPrimedia);
    if (!priorPrimediaKey)
      return std::unexpected(
          "mesh value: author's permascroll has no identity");
    const auto allBytes = store.primedia().bytes();
    if (priorPrimedia.length() > allBytes.size())
      return std::unexpected(
          "mesh value: invalid author's primedia seal range");
    const auto unsealed = allBytes.substr(priorPrimedia.length());
    if (!unsealed.empty()) {
      if (!primediaSigner)
        return std::unexpected(
            "mesh value: separate primedia provenance signer required");
      const auto signedPrimedia = primediaSigner(
          store, unsealed, priorPrimedia.length(), *priorPrimediaKey);
      if (!signedPrimedia || signedPrimedia->tsv.empty() ||
          signedPrimedia->signature.empty())
        return std::unexpected(signedPrimedia
                                   ? "mesh value: empty primedia provenance"
                                   : signedPrimedia.error());
      const auto record = xanadu::parseProvenance(signedPrimedia->tsv);
      if ((!record && !allowPresignedFixture) ||
          (record && (record->contentLength != unsealed.size() ||
                      record->contentDigest != xanadu::sha256Hex(unsealed) ||
                      record->permascroll != *priorPrimediaKey))) {
        return std::unexpected(
            "mesh value: primedia provenance does not cover sealed bytes");
      }
      store.userPermascroll().sealIncremental(seedRoot, *signedPrimedia, {},
                                              allBytes.size());
    }
    const auto primedia = store.userPermascroll().currentScroll();
    std::set<xanadu::InfoHash> payloads;
    for (const auto &segment : primedia.segments) {
      if (!payloads.insert(segment.torrent).second) continue;
      const auto path = seedRoot / segment.torrent.hex();
      std::ifstream input(path / "metainfo.torrent", std::ios::binary);
      if (!input)
        return std::unexpected(
            "mesh value: author's prior primedia seed unavailable");
      std::string file{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
      if (xanadu::Metainfo::parse(file).hash() != segment.torrent)
        return std::unexpected(
            "mesh value: author's primedia metainfo mismatch");
      seed.payloads.push_back({{std::move(file), segment.torrent}, path});
    }
    seed.history =
        xanadu::sealLocalSpool(store, keys, historySalt(store.documentId()),
                               seedRoot.string(), *provenance, primedia);
    seed.value.origin       = std::move(origin);
    seed.value.job          = std::move(job);
    seed.value.continuation = std::move(continuation);
    seed.value.parent       = parent;
    const auto root =
        xanadu::opRefOf(store, promoted->cells.front(), seed.history.scroll);
    if (!root)
      return std::unexpected(
          "mesh value: promoted root has no global identity");
    seed.value.root        = *root;
    seed.value.publication = xanadu::publish(
        store, promoted->version, keys, "vortex-value", "Vortex value", 1, 0,
        &primedia, {*seed.history.opsSegment});
    const auto historyKey = xanadu::scrollKey(seed.history.scroll);
    if (!historyKey)
      return std::unexpected(
          "mesh value: sealed history has no global identity");
    seed.value.publication.scrolls.insert_or_assign(*historyKey,
                                                    seed.history.scroll);
    // A foreign operation may have no text spans quoting its source history.
    // Its deployment descriptor must still survive an independently seeded
    // result, even when the optional label is empty.
    for (std::uint32_t index = 1; index <= store.opCount(); ++index) {
      const auto *op = store.getCompactOp(index);
      if (!op || op->kind != xanadu::OpKind::Structure ||
          xanadu::valueKindOf(op->flags) != xanadu::ValueKind::ForeignOpHandle)
        continue;
      const auto target = xanadu::readForeignOpHandle(store.read(op->span()));
      if (!target)
        return std::unexpected("mesh value: malformed foreign handle history");
      const auto descriptor =
          std::ranges::find_if(store.scrolls(), [&](const auto &scroll) {
            const auto identity = xanadu::scrollKey(scroll);
            return identity && *identity == target->scroll;
          });
      if (descriptor == store.scrolls().end())
        return std::unexpected(
            "mesh value: foreign operation deployment missing");
      seed.value.publication.scrolls.insert_or_assign(target->scroll,
                                                      *descriptor);
    }
    seed.value.publication.signature = xanadu::signMutableItem(
        xanadu::publicationSigningBuffer(seed.value.publication), keys);
    seed.value.signature =
        xanadu::signMutableItem(valueSigningBuffer(seed.value), keys);
    if (auto valid = check(seed.value, limits); !valid)
      return std::unexpected(valid.error());
    const auto manifest = encodeValue(seed.value);
    if (manifest.size() > limits.manifestBytes)
      return std::unexpected("mesh value: manifest budget exceeded");
    const std::vector<xanadu::TorrentContent> files{
        {.path = std::string(manifestName), .data = manifest}};
    seed.carrier     = xanadu::makeTorrent(files, "vortex-value");
    seed.carrierPath = xanadu::writeTorrentSeed(seedRoot, seed.carrier, files);
    return seed;
  } catch (const std::exception &error) {
    return std::unexpected(std::string{"mesh value: "} + error.what());
  }
}

std::expected<ValueSeed, std::string> publishValue(
    xanadu::Store &store, const xanadu::MicroversionId &parent,
    const ArenaManifold &arena, CellRef root, const xanadu::MutableKeys &keys,
    NodeId origin, const ValueProvenanceSigner &signer,
    const std::filesystem::path &seedRoot, ValueLimits limits, std::string job,
    std::string continuation, ValuePrimediaProvenanceSigner primediaSigner) {
  return publishValueImpl(store, parent, arena, root, keys, std::move(origin),
                          signer, seedRoot, limits, std::move(job),
                          std::move(continuation), primediaSigner, false);
}

std::expected<ValueSeed, std::string>
publishValue(xanadu::Store &store, const xanadu::MicroversionId &parent,
             const ArenaManifold &arena, CellRef root,
             const xanadu::MutableKeys &keys, NodeId origin,
             const xanadu::SignedProvenance &provenance,
             const std::filesystem::path &seedRoot, ValueLimits limits,
             std::string job, std::string continuation) {
  return publishValueImpl(
      store, parent, arena, root, keys, std::move(origin),
      [&provenance](xanadu::Store &)
          -> std::expected<xanadu::SignedProvenance, std::string> {
        return provenance;
      },
      seedRoot, limits, std::move(job), std::move(continuation),
      [&provenance](xanadu::Store &, std::string_view, std::uint64_t,
                    std::string_view)
          -> std::expected<xanadu::SignedProvenance, std::string> {
        return provenance;
      },
      true);
}

std::expected<ReceivedValue, std::string>
receiveValue(std::string_view manifest, const xanadu::ContentSource &source,
             std::shared_ptr<xanadu::UserPermascroll> reader,
             ValueLimits limits) {
  auto decoded = decodeValue(manifest, limits);
  if (!decoded) return std::unexpected(decoded.error());
  try {
    std::set<xanadu::InfoHash> dependencies;
    for (const auto &segment : decoded->publication.opsSegments)
      dependencies.insert(segment.torrent);
    for (const auto &[key, scroll] : decoded->publication.scrolls) {
      (void)key;
      for (const auto &segment : scroll.segments)
        dependencies.insert(segment.torrent);
    }
    std::uint64_t totalBytes = 0;
    for (const auto &hash : dependencies) {
      const auto meta = source.metainfo(hash);
      if (!meta || meta->totalLength() > limits.payloadBytes - totalBytes)
        return std::unexpected(
            "mesh value: dependency metadata missing or over budget");
      totalBytes += meta->totalLength();
    }
    auto store        = xanadu::restorePublication(decoded->publication, source,
                                                   std::move(reader));
    const auto scroll = decoded->publication.scrolls.find(decoded->root.scroll);
    const auto root =
        xanadu::localiseOpRef(*store, decoded->root, scroll->second);
    auto manifold = std::make_unique<Manifold>(
        store->rebuildManifold(decoded->publication.version));
    if (!root || !manifold->contains(*root))
      return std::unexpected(
          "mesh value: root does not name a cell in the snapshot");
    return ReceivedValue{.value       = std::move(*decoded),
                         .contentHash = xanadu::sha256Hex(manifest),
                         .store       = std::move(store),
                         .manifold    = std::move(manifold),
                         .root        = *root};
  } catch (const std::exception &error) {
    return std::unexpected(std::string{"mesh value: "} + error.what());
  }
}

std::expected<ReceivedValue, std::string>
fetchValue(const xanadu::InfoHash &hash, const xanadu::ContentSource &source,
           std::shared_ptr<xanadu::UserPermascroll> reader,
           ValueLimits limits) {
  const auto meta = source.metainfo(hash);
  if (!meta || meta->hash() != hash || meta->files().size() != 1 ||
      meta->files().front().path != manifestName ||
      meta->totalLength() > limits.manifestBytes)
    return std::unexpected(
        "mesh value: missing or over-budget manifest metadata");
  const auto &file = meta->files().front();
  xanadu::Resolver resolver(&source);
  const auto resolved =
      resolver.resolve(xanadu::Scroll::ofTorrentFile(hash, 0, file.path,
                                                     file.offset, file.length),
                       {.start = 0, .length = file.length});
  if (!resolved.isVerified() || resolved.text.size() != file.length)
    return std::unexpected("mesh value: manifest pieces missing or corrupt");
  return receiveValue(resolved.text, source, std::move(reader), limits);
}

std::expected<ImportedValue, std::string>
importValue(const ReceivedValue &value, xanadu::Store &destination,
            ValueLimits limits) {
  if (!value.store || !value.manifold || !value.manifold->contains(value.root))
    return std::unexpected("mesh value: missing restored graph");
  const auto &source = *value.manifold;
  std::vector<CellRef> order{value.root};
  std::vector<CellRef> frontier{value.root};
  std::set<CellRef> seen{value.root}, expanded{value.root};
  for (std::size_t i = 0; i < frontier.size(); ++i) {
    if (xanadu::isCellRelocationHandle(source, frontier[i], *value.store)) {
      const auto target = source.handleTarget(frontier[i]);
      if (!target || !source.contains(*target) ||
          source.slot(*target)->birthOp != *target)
        return std::unexpected("mesh value: relocation target is not a cell");
      if (seen.insert(*target).second) order.push_back(*target);
      if (expanded.insert(*target).second) frontier.push_back(*target);
    }
    for (const auto &edge : source.dimensionsOf(frontier[i])) {
      if (source.textOf(edge.dim, *value.store) == "d.dims") continue;
      if (seen.insert(edge.dim).second) order.push_back(edge.dim);
      for (auto ref : {edge.neg, edge.pos}) {
        if (ref == noCell || !source.contains(ref)) continue;
        if (seen.insert(ref).second) order.push_back(ref);
        if (expanded.insert(ref).second) frontier.push_back(ref);
      }
    }
    if (order.size() > limits.operations)
      return std::unexpected("mesh value: graph budget exceeded");
  }
  auto arena = std::make_unique<ArenaManifold>();
  arena->setDefaultSpanReader(&destination);
  std::map<CellRef, CellRef> local;
  std::map<CellRef, xanadu::GlobalOpRef> foreignHandles;
  for (const auto ref : order) {
    if (source.valueKindOf(ref) == xanadu::ValueKind::OpHandle) {
      if (xanadu::isCellRelocationHandle(source, ref, *value.store)) continue;
      const auto target = source.handleTarget(ref);
      const auto history =
          value.value.publication.scrolls.find(value.value.root.scroll);
      if (!target || history == value.value.publication.scrolls.end())
        return std::unexpected(
            "mesh value: operation handle history unavailable");
      const auto global =
          xanadu::opRefOf(*value.store, *target, history->second);
      if (!global || !xanadu::resolveForeignOperation(*global, *value.store,
                                                      history->second))
        return std::unexpected("mesh value: operation handle target absent");
      foreignHandles.emplace(ref, *global);
    } else if (source.valueKindOf(ref) == xanadu::ValueKind::ForeignOpHandle) {
      const auto target = xanadu::foreignOpTarget(source, ref, *value.store);
      if (!target)
        return std::unexpected(
            "mesh value: malformed foreign operation handle");
      foreignHandles.emplace(ref, *target);
    }
  }
  std::size_t labelCells = 0;
  for (const auto &[ref, target] : foreignHandles)
    if (source.valueKindOf(ref) == xanadu::ValueKind::OpHandle &&
        !source.contentOf(ref).empty())
      labelCells += 2;
  if (labelCells > limits.operations ||
      order.size() > limits.operations - labelCells)
    return std::unexpected("mesh value: handle label graph budget exceeded");
  for (const auto &[ref, target] : foreignHandles) {
    const auto descriptor = value.value.publication.scrolls.find(target.scroll);
    if (descriptor == value.value.publication.scrolls.end())
      return std::unexpected(
          "mesh value: operation history descriptor missing");
    (void)destination.addScroll(descriptor->second);
  }
  for (const auto ref : order) {
    auto spans =
        xanadu::carrySpans(*value.store, destination, source.contentOf(ref));
    if (!spans) return std::unexpected(spans.error());
    if (const auto handle = foreignHandles.find(ref);
        handle != foreignHandles.end()) {
      const auto cell =
          arena->makeCell(xanadu::writeForeignOpHandle(handle->second));
      if (!arena->setValueBits(cell, xanadu::ValueKind::ForeignOpHandle, 0))
        return std::unexpected(
            "mesh value: foreign handle construction refused");
      local.emplace(ref, cell);
      if (source.valueKindOf(ref) == xanadu::ValueKind::OpHandle &&
          !spans->empty()) {
        const auto label          = arena->makeCell();
        const auto labelDimension = arena->ensureDimension("d.op-label");
        if (!arena->setContent(label, *spans) ||
            !arena->link(cell, labelDimension, DimVector::POS, label))
          return std::unexpected(
              "mesh value: operation label construction refused");
      }
      continue;
    }
    const auto cell = arena->makeCell();
    if (!arena->setContent(cell, *spans) ||
        !arena->setValueBits(
            cell,
            source.valueKindOf(ref) == xanadu::ValueKind::OpHandle
                ? xanadu::ValueKind::None
                : source.valueKindOf(ref),
            source.valueKindOf(ref) == xanadu::ValueKind::OpHandle
                ? 0
                : source.slot(ref)->valueBits))
      return std::unexpected("mesh value: local graph construction refused");
    local.emplace(ref, cell);
  }
  for (const auto ref : order) {
    if (xanadu::isCellRelocationHandle(source, ref, *value.store)) {
      const auto target = source.handleTarget(ref);
      if (!target || !local.contains(*target) ||
          !arena->setValueBits(local.at(ref), xanadu::ValueKind::OpHandle,
                               local.at(*target)))
        return std::unexpected("mesh value: relocation construction refused");
    }
  }
  for (const auto ref : order)
    for (const auto &edge : source.dimensionsOf(ref))
      if (edge.pos != noCell && local.contains(edge.pos) &&
          !arena->link(local.at(ref), local.at(edge.dim), DimVector::POS,
                       local.at(edge.pos)))
        return std::unexpected("mesh value: local graph edge refused");
  return ImportedValue{.arena = std::move(arena), .root = local.at(value.root)};
}

std::expected<std::vector<MergedValue>, std::string>
mergeValues(xanadu::Store &destination, const xanadu::MicroversionId &parent,
            std::span<const ReceivedValue *const> values, ValueLimits limits) {
  std::vector<const ReceivedValue *> ordered(values.begin(), values.end());
  if (std::ranges::any_of(ordered,
                          [](const auto *value) { return value == nullptr; }))
    return std::unexpected("mesh value: missing result");
  std::ranges::sort(ordered, [](const auto *a, const auto *b) {
    return std::tie(a->contentHash, a->value.origin.authorKey,
                    a->value.parent) <
           std::tie(b->contentHash, b->value.origin.authorKey, b->value.parent);
  });
  std::vector<ImportedValue> imports;
  std::vector<const ReceivedValue *> unique;
  for (const auto *value : ordered) {
    if (value->value.parent != parent)
      return std::unexpected("mesh value: unresolved causal parent");
    if (!unique.empty() && unique.back()->contentHash == value->contentHash)
      continue;
    auto imported = importValue(*value, destination, limits);
    if (!imported) return std::unexpected(imported.error());
    imports.push_back(std::move(*imported));
    unique.push_back(value);
  }
  std::vector<MergedValue> out;
  for (std::size_t i = 0; i < imports.size(); ++i) {
    auto promoted = promote(destination, parent, *imports[i].arena,
                            imports[i].root, {limits.operations});
    if (!promoted)
      return std::unexpected("mesh value: result promotion refused");
    out.push_back({unique[i]->contentHash, std::move(*promoted)});
  }
  return out;
}

namespace {
std::string blake3Hex(const std::string_view data) {
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  if (!data.empty()) {
    blake3_hasher_update(&hasher, data.data(), data.size());
  }
  std::array<std::uint8_t, BLAKE3_OUT_LEN> digest{};
  blake3_hasher_finalize(&hasher, digest.data(), digest.size());
  return xanadu::toHex(std::string_view(
      reinterpret_cast<const char *>(digest.data()), digest.size()));
}
} // namespace

std::vector<std::string>
computeSpoolMerkleLeaves(const std::string_view streamBytes) {
  std::vector<std::string> leaves;
  if (streamBytes.empty()) {
    leaves.push_back(blake3Hex(""));
    return leaves;
  }
  leaves.reserve((streamBytes.size() + kSpoolMerkleLeafBytes - 1) /
                 kSpoolMerkleLeafBytes);
  for (std::size_t offset = 0; offset < streamBytes.size();
       offset += kSpoolMerkleLeafBytes) {
    const auto chunk = streamBytes.substr(
        offset, std::min(kSpoolMerkleLeafBytes, streamBytes.size() - offset));
    leaves.push_back(blake3Hex(chunk));
  }
  return leaves;
}

bool verifySpoolMerkleLeaves(const std::string_view streamBytes,
                             const std::span<const std::string> leafHashes) {
  const auto expected = computeSpoolMerkleLeaves(streamBytes);
  if (expected.size() != leafHashes.size()) return false;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (expected[i] != leafHashes[i]) return false;
  }
  return true;
}

std::expected<VerifiedTorrentSlice, std::string>
packTorrentSlice(const std::string_view torrentFile,
                 const std::filesystem::path &seedDir,
                 const std::uint64_t maxBytes) {
  try {
    auto meta = xanadu::Metainfo::parse(torrentFile);
    if (meta.totalLength() > maxBytes)
      return std::unexpected("spool slice: torrent stream exceeds byte budget");
    xanadu::DirectoryContentSource dirSource;
    const auto hash   = dirSource.add(torrentFile, seedDir.string());
    const auto stream = dirSource.readStream(hash, 0, meta.totalLength());
    if (stream.size() != meta.totalLength())
      return std::unexpected(
          "spool slice: unable to read complete stream from seed directory");
    VerifiedTorrentSlice slice{
        .hash             = hash,
        .metainfoBytes    = std::string(torrentFile),
        .streamBytes      = stream,
        .merkleLeafHashes = computeSpoolMerkleLeaves(stream),
    };
    auto verified = verifyTorrentSlice(slice, maxBytes);
    if (!verified) return std::unexpected(verified.error());
    return slice;
  } catch (const std::exception &e) {
    return std::unexpected(std::string{"spool slice: "} + e.what());
  }
}

std::expected<xanadu::Metainfo, std::string>
verifyTorrentSlice(const VerifiedTorrentSlice &slice,
                   const std::uint64_t maxBytes) {
  try {
    if (slice.hash.isZero() || slice.metainfoBytes.empty())
      return std::unexpected("spool slice: missing hash or metainfo");
    auto meta = xanadu::Metainfo::parse(slice.metainfoBytes);
    if (meta.hash() != slice.hash)
      return std::unexpected("spool slice: metainfo hash mismatch");
    if (meta.totalLength() > maxBytes ||
        slice.streamBytes.size() != meta.totalLength())
      return std::unexpected(
          "spool slice: stream length mismatch or over budget");
    if (!verifySpoolMerkleLeaves(slice.streamBytes, slice.merkleLeafHashes))
      return std::unexpected(
          "spool slice: 1 KiB Merkle leaf verification failed");
    const std::string_view view(slice.streamBytes);
    for (std::size_t p = 0; p < meta.pieceCount(); ++p) {
      const auto offset = p * meta.pieceLength();
      const auto length = meta.lengthOfPiece(p);
      if (offset + length > view.size() ||
          !meta.verifyPiece(p, view.substr(offset, length)))
        return std::unexpected("spool slice: piece SHA-1 verification failed");
    }
    return meta;
  } catch (const std::exception &e) {
    return std::unexpected(std::string{"spool slice: "} + e.what());
  }
}

std::expected<SpoolSliceBundle, std::string>
packSpoolSliceBundle(const ValueSeed &seed, ValueLimits limits) {
  const auto manifest = encodeValue(seed.value);
  if (manifest.size() > limits.manifestBytes)
    return std::unexpected("spool slice: manifest exceeds budget");
  auto decoded = decodeValue(manifest, limits);
  if (!decoded) return std::unexpected(decoded.error());

  auto carrier = packTorrentSlice(seed.carrier.file, seed.carrierPath,
                                  limits.manifestBytes);
  if (!carrier) return std::unexpected(carrier.error());

  const auto historyRoot =
      seed.carrierPath.parent_path() / seed.history.hash.hex();
  auto history = packTorrentSlice(seed.history.torrentFile, historyRoot,
                                  limits.payloadBytes);
  if (!history) return std::unexpected(history.error());

  std::uint64_t usedBytes = history->streamBytes.size();
  std::vector<VerifiedTorrentSlice> payloads;
  payloads.reserve(seed.payloads.size());
  for (const auto &payload : seed.payloads) {
    if (usedBytes > limits.payloadBytes)
      return std::unexpected("spool slice: aggregate payload budget exceeded");
    auto packed = packTorrentSlice(payload.torrent.file, payload.path,
                                   limits.payloadBytes - usedBytes);
    if (!packed) return std::unexpected(packed.error());
    usedBytes += packed->streamBytes.size();
    payloads.push_back(std::move(*packed));
  }
  return SpoolSliceBundle{
      .value         = std::move(*decoded),
      .manifestBytes = manifest,
      .carrier       = std::move(*carrier),
      .history       = std::move(*history),
      .payloads      = std::move(payloads),
  };
}

namespace {
V encodeSliceValue(const VerifiedTorrentSlice &slice) {
  std::vector<V> leaves;
  leaves.reserve(slice.merkleLeafHashes.size());
  for (const auto &leaf : slice.merkleLeafHashes)
    leaves.push_back(V::string(leaf));
  return V::dict({
      {"hash", V::string(slice.hash.hex())},
      {"leaves", V::list(std::move(leaves))},
      {"meta", V::string(slice.metainfoBytes)},
      {"stream", V::string(slice.streamBytes)},
  });
}

std::expected<VerifiedTorrentSlice, std::string>
decodeSliceValue(const V &node, const std::uint64_t maxBytes) {
  if (!node.isDict() || node.asDict().size() != 4)
    return std::unexpected("spool slice: invalid slice dictionary");
  const auto hashNode   = node.find("hash");
  const auto leavesNode = node.find("leaves");
  const auto metaNode   = node.find("meta");
  const auto streamNode = node.find("stream");
  if (!hashNode || !hashNode->isString() || !leavesNode ||
      !leavesNode->isList() || !metaNode || !metaNode->isString() ||
      !streamNode || !streamNode->isString())
    return std::unexpected("spool slice: malformed slice fields");
  const auto parsedHash = xanadu::InfoHash::parseHex(hashNode->asString());
  if (!parsedHash) return std::unexpected("spool slice: invalid slice hash");
  VerifiedTorrentSlice slice;
  slice.hash          = *parsedHash;
  slice.metainfoBytes = metaNode->asString();
  slice.streamBytes   = streamNode->asString();
  slice.merkleLeafHashes.reserve(leavesNode->asList().size());
  for (const auto &leaf : leavesNode->asList()) {
    if (!leaf.isString())
      return std::unexpected("spool slice: non-string Merkle leaf hash");
    slice.merkleLeafHashes.push_back(leaf.asString());
  }
  if (auto verified = verifyTorrentSlice(slice, maxBytes); !verified)
    return std::unexpected(verified.error());
  return slice;
}
} // namespace

std::string encodeSpoolSliceBundle(const SpoolSliceBundle &bundle) {
  std::vector<V> payloads;
  payloads.reserve(bundle.payloads.size());
  for (const auto &payload : bundle.payloads)
    payloads.push_back(encodeSliceValue(payload));
  return V::dict({
                     {"carrier", encodeSliceValue(bundle.carrier)},
                     {"history", encodeSliceValue(bundle.history)},
                     {"manifest", V::string(bundle.manifestBytes)},
                     {"payloads", V::list(std::move(payloads))},
                     {"version", V::integer(1)},
                 })
      .encode();
}

std::expected<SpoolSliceBundle, std::string>
decodeSpoolSliceBundle(const std::string_view bytes, const ValueLimits limits) {
  if (bytes.empty() ||
      bytes.size() > limits.manifestBytes + limits.payloadBytes * 2)
    return std::unexpected("spool slice: bundle exceeds byte budget");
  try {
    const auto decoded = xanadu::bencode::decode(bytes);
    if (!decoded.isDict() || decoded.asDict().size() != 5 ||
        decoded.encode() != bytes)
      return std::unexpected("spool slice: invalid canonical bundle envelope");
    const auto version  = decoded.find("version");
    const auto manifest = decoded.find("manifest");
    const auto carrier  = decoded.find("carrier");
    const auto history  = decoded.find("history");
    const auto payloads = decoded.find("payloads");
    if (!version || !version->isInteger() || version->asInteger() != 1 ||
        !manifest || !manifest->isString() || !carrier || !history ||
        !payloads || !payloads->isList())
      return std::unexpected("spool slice: missing bundle fields");

    auto parsedValue = decodeValue(manifest->asString(), limits);
    if (!parsedValue) return std::unexpected(parsedValue.error());

    auto parsedCarrier = decodeSliceValue(*carrier, limits.manifestBytes);
    if (!parsedCarrier) return std::unexpected(parsedCarrier.error());

    auto parsedHistory = decodeSliceValue(*history, limits.payloadBytes);
    if (!parsedHistory) return std::unexpected(parsedHistory.error());

    std::uint64_t usedBytes = parsedHistory->streamBytes.size();
    std::vector<VerifiedTorrentSlice> parsedPayloads;
    parsedPayloads.reserve(payloads->asList().size());
    for (const auto &item : payloads->asList()) {
      if (usedBytes > limits.payloadBytes)
        return std::unexpected(
            "spool slice: aggregate payload budget exceeded");
      auto slice = decodeSliceValue(item, limits.payloadBytes - usedBytes);
      if (!slice) return std::unexpected(slice.error());
      usedBytes += slice->streamBytes.size();
      parsedPayloads.push_back(std::move(*slice));
    }

    return SpoolSliceBundle{
        .value         = std::move(*parsedValue),
        .manifestBytes = manifest->asString(),
        .carrier       = std::move(*parsedCarrier),
        .history       = std::move(*parsedHistory),
        .payloads      = std::move(parsedPayloads),
    };
  } catch (const std::exception &e) {
    return std::unexpected(std::string{"spool slice: "} + e.what());
  }
}

std::expected<void, std::string>
VerifiedSliceContentSource::ingestSlice(const VerifiedTorrentSlice &slice,
                                        const std::uint64_t maxBytes) {
  auto meta = verifyTorrentSlice(slice, maxBytes);
  if (!meta) return std::unexpected(meta.error());
  std::scoped_lock lock(mutex_);
  entries_.insert_or_assign(slice.hash,
                            Entry{
                                .meta             = std::move(*meta),
                                .metainfoBytes    = slice.metainfoBytes,
                                .streamBytes      = slice.streamBytes,
                                .merkleLeafHashes = slice.merkleLeafHashes,
                            });
  return {};
}

std::expected<void, std::string>
VerifiedSliceContentSource::ingestBundle(const SpoolSliceBundle &bundle,
                                         const ValueLimits limits) {
  if (auto ok = ingestSlice(bundle.carrier, limits.manifestBytes); !ok)
    return ok;
  if (auto ok = ingestSlice(bundle.history, limits.payloadBytes); !ok)
    return ok;
  for (const auto &payload : bundle.payloads) {
    if (auto ok = ingestSlice(payload, limits.payloadBytes); !ok) return ok;
  }
  return {};
}

std::expected<void, std::string>
VerifiedSliceContentSource::ingestSeed(const ValueSeed &seed,
                                       const ValueLimits limits) {
  auto bundle = packSpoolSliceBundle(seed, limits);
  if (!bundle) return std::unexpected(bundle.error());
  return ingestBundle(*bundle, limits);
}

bool VerifiedSliceContentSource::contains(const xanadu::InfoHash &hash) const {
  std::scoped_lock lock(mutex_);
  return entries_.contains(hash);
}

std::optional<VerifiedTorrentSlice>
VerifiedSliceContentSource::exportSlice(const xanadu::InfoHash &hash) const {
  std::scoped_lock lock(mutex_);
  const auto found = entries_.find(hash);
  if (found == entries_.end()) return std::nullopt;
  return VerifiedTorrentSlice{
      .hash             = hash,
      .metainfoBytes    = found->second.metainfoBytes,
      .streamBytes      = found->second.streamBytes,
      .merkleLeafHashes = found->second.merkleLeafHashes,
  };
}

gleditor::cpp26::optional<const xanadu::Metainfo &>
VerifiedSliceContentSource::metainfo(const xanadu::InfoHash &hash) const {
  const xanadu::ContentSource *fb = nullptr;
  {
    std::scoped_lock lock(mutex_);
    const auto found = entries_.find(hash);
    if (found != entries_.end()) return found->second.meta;
    fb = fallback_;
  }
  return fb ? fb->metainfo(hash) : gleditor::cpp26::nullopt;
}

std::string
VerifiedSliceContentSource::readStream(const xanadu::InfoHash &hash,
                                       const std::uint64_t offset,
                                       const std::uint64_t length) const {
  const xanadu::ContentSource *fb = nullptr;
  {
    std::scoped_lock lock(mutex_);
    const auto found = entries_.find(hash);
    if (found != entries_.end()) {
      const auto &stream = found->second.streamBytes;
      if (offset >= stream.size()) return {};
      const auto count =
          std::min<std::uint64_t>(length, stream.size() - offset);
      return stream.substr(static_cast<std::size_t>(offset),
                           static_cast<std::size_t>(count));
    }
    fb = fallback_;
  }
  return fb ? fb->readStream(hash, offset, length) : std::string{};
}

std::expected<ReceivedValue, std::string>
receiveSpoolSliceBundle(const std::string_view bundleBytes,
                        const xanadu::ContentSource *fallbackSource,
                        std::shared_ptr<xanadu::UserPermascroll> reader,
                        const ValueLimits limits) {
  auto bundle = decodeSpoolSliceBundle(bundleBytes, limits);
  if (!bundle) return std::unexpected(bundle.error());
  auto source = std::make_shared<VerifiedSliceContentSource>(fallbackSource);
  if (auto ok = source->ingestBundle(*bundle, limits); !ok)
    return std::unexpected(ok.error());
  auto received =
      receiveValue(bundle->manifestBytes, *source, std::move(reader), limits);
  if (!received) return std::unexpected(received.error());
  received->sliceSource = std::move(source);
  return received;
}

} // namespace zigzag::vortex::mesh
