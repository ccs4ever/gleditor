#include <gtest/gtest.h>

#include <filesystem>
#include <sstream>

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/binary_ops.hpp"
#include "common/xanadu/foreign_op_handle.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/mesh_value.hpp"

namespace {
using namespace zigzag::vortex::mesh;
using zigzag::DimVector;

class MeshValueTest : public testing::Test {
protected:
  xanadu::MutableKeys keys = xanadu::createMutableKeys();
  NodeId identity{"author", keys.publicKey.hex(), 1};
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("xanadu-mesh-value-" + keys.publicKey.hex());
  xanadu::DirectoryContentSource source;
  xanadu::SignedProvenance provenance{"fixture authorship",
                                      "fixture signature"};

  ~MeshValueTest() override { std::filesystem::remove_all(directory); }

  void seed(const ValueSeed &value) {
    for (const auto &payload : value.payloads)
      source.add(payload.torrent.file, payload.path.string());
    source.add(value.history.torrentFile,
               (directory / value.history.hash.hex()).string());
    source.add(value.carrier.file, value.carrierPath.string());
  }

  std::expected<ValueSeed, std::string> make(std::string_view text) {
    zigzag::ArenaManifold arena;
    const auto root      = arena.makeCell(text);
    const auto dimension = arena.makeCell("d.answer");
    const auto number    = arena.makeScalarCell(std::int64_t{42});
    zigzag::expectWritten(arena.link(root, dimension, DimVector::POS, number));
    xanadu::Store output;
    return publishValue(output, {}, arena, root, keys, identity, provenance,
                        directory);
  }
};

TEST_F(MeshValueTest, PublishedSpanRoundTripPromotesAnswerBack) {
  auto outgoing = make("a published question");
  ASSERT_TRUE(outgoing) << outgoing.error();
  seed(*outgoing);
  auto reader   = std::make_shared<xanadu::UserPermascroll>();
  auto received = fetchValue(outgoing->carrier.hash, source, reader);
  ASSERT_TRUE(received) << received.error();
  EXPECT_EQ(received->manifold->textOf(received->root, *received->store),
            "a published question");
  EXPECT_EQ(reader->spool().size(), 0U);
  const auto original = received->manifold->contentOf(received->root).front();
  const auto global   = xanadu::globalise(*received->store, original);
  ASSERT_TRUE(global);

  xanadu::Store responder;
  responder.setContentSource(&source);
  auto imported = importValue(*received, responder);
  ASSERT_TRUE(imported) << imported.error();
  const auto localSpan = imported->arena->contentOf(imported->root).front();
  EXPECT_EQ(xanadu::globalise(responder, localSpan), global);
  EXPECT_EQ(responder.primedia().size(), 0U);
  std::optional<zigzag::CellRef> dimension;
  for (const auto &edge : imported->arena->dimensionsOf(imported->root))
    if (imported->arena->textOf(edge.dim, responder) == "d.answer")
      dimension = edge.dim;
  ASSERT_TRUE(dimension);
  const auto number =
      imported->arena->linked(imported->root, *dimension, DimVector::POS);
  ASSERT_NE(number, zigzag::noCell);
  EXPECT_EQ(imported->arena->asInt64(number), 42);
  const auto computed = imported->arena->makeScalarCell(
      imported->arena->asInt64(number).value() + 1);
  const auto computedDimension = imported->arena->makeCell("d.computed");
  zigzag::expectWritten(imported->arena->link(imported->root, computedDimension,
                                              DimVector::POS, computed));

  const auto responseKeys = xanadu::createMutableKeys();
  NodeId responderId{"responding-author", responseKeys.publicKey.hex(), 9};
  auto reply = publishValue(responder, {}, *imported->arena, imported->root,
                            responseKeys, responderId, provenance, directory);
  ASSERT_TRUE(reply) << reply.error();
  seed(*reply);
  auto returned = fetchValue(reply->carrier.hash, source,
                             std::make_shared<xanadu::UserPermascroll>());
  ASSERT_TRUE(returned) << returned.error();
  EXPECT_EQ(returned->manifold->textOf(returned->root, *returned->store),
            "a published question");
  EXPECT_EQ(
      xanadu::globalise(*returned->store,
                        returned->manifold->contentOf(returned->root).front()),
      global);
  std::optional<zigzag::CellRef> returnedAnswer;
  for (const auto &edge : returned->manifold->dimensionsOf(returned->root))
    if (returned->manifold->textOf(edge.dim, *returned->store) == "d.computed")
      returnedAnswer = edge.pos;
  ASSERT_TRUE(returnedAnswer);
  EXPECT_EQ(returned->manifold->asInt64(*returnedAnswer), 43);
  for (const auto &edge : received->manifold->dimensionsOf(received->root))
    if (received->manifold->textOf(edge.dim, *received->store) == "d.answer")
      EXPECT_EQ(received->manifold->asInt64(edge.pos), 42);
}

TEST_F(MeshValueTest, SignatureBindsRootParentJobAndIdentity) {
  auto outgoing = make("signed result");
  ASSERT_TRUE(outgoing) << outgoing.error();
  const auto manifest = encodeValue(outgoing->value);
  ASSERT_TRUE(decodeValue(manifest));
  for (auto field : {"root", "parent", "job", "author"}) {
    auto body   = xanadu::bencode::decode(manifest).asDict();
    body[field] = xanadu::bencode::Value::string(
        field == std::string_view{"job"} || field == std::string_view{"author"}
            ? "changed"
            : "999");
    EXPECT_FALSE(
        decodeValue(xanadu::bencode::Value::dict(std::move(body)).encode()))
        << field;
  }
  EXPECT_FALSE(decodeValue(manifest + "junk"));
  auto limits          = ValueLimits{};
  limits.manifestBytes = manifest.size() - 1;
  EXPECT_FALSE(decodeValue(manifest, limits));
}

TEST_F(MeshValueTest, MissingOrCorruptDependenciesCannotBecomeEmptyValues) {
  auto outgoing = make("required bytes");
  ASSERT_TRUE(outgoing) << outgoing.error();
  xanadu::DirectoryContentSource empty;
  EXPECT_FALSE(receiveValue(encodeValue(outgoing->value), empty,
                            std::make_shared<xanadu::UserPermascroll>()));
  seed(*outgoing);
  auto limits         = ValueLimits{};
  limits.payloadBytes = 1;
  EXPECT_FALSE(fetchValue(outgoing->carrier.hash, source,
                          std::make_shared<xanadu::UserPermascroll>(), limits));
}

TEST_F(MeshValueTest,
       SharedPrimediaAndIndependentOperationHistoriesKeepDistinctNames) {
  auto author = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store first(author), second(author);
  zigzag::ArenaManifold left, right;
  const auto leftRoot  = left.makeCell("first document");
  const auto rightRoot = right.makeCell("second document");
  auto a = publishValue(first, {}, left, leftRoot, keys, identity, provenance,
                        directory);
  auto b = publishValue(second, {}, right, rightRoot, keys, identity,
                        provenance, directory);
  ASSERT_TRUE(a) << a.error();
  ASSERT_TRUE(b) << b.error();
  EXPECT_EQ(a->value.root.produces, b->value.root.produces);
  EXPECT_NE(a->value.root.scroll, b->value.root.scroll);
  EXPECT_EQ(a->value.publication.historyScroll,
            b->value.publication.historyScroll);
  EXPECT_EQ(a->value.publication.historyScroll, author->globalScrollKey());
  EXPECT_NE(a->value.root.scroll, a->value.publication.historyScroll);
  EXPECT_LE(a->history.scroll.salt.size(), 64U);
  seed(*a);
  seed(*b);
  auto restored = fetchValue(a->carrier.hash, source,
                             std::make_shared<xanadu::UserPermascroll>());
  ASSERT_TRUE(restored) << restored.error();
  EXPECT_EQ(restored->store->documentId(), first.documentId());
  zigzag::ArenaManifold update;
  const auto updateRoot = update.makeCell("next checkpoint");
  auto repeated =
      publishValue(*restored->store, a->value.publication.version, update,
                   updateRoot, keys, identity, provenance, directory);
  ASSERT_TRUE(repeated) << repeated.error();
  EXPECT_EQ(repeated->value.root.scroll, a->value.root.scroll);
}

TEST_F(MeshValueTest, PrimediaSignerMustCoverItsOwnIncrementalBytes) {
  const ValueProvenanceSigner history = [](xanadu::Store &)
      -> std::expected<xanadu::SignedProvenance, std::string> {
    return xanadu::SignedProvenance{"history fixture", "signature"};
  };
  const auto signPrimedia = [](bool wrongDigest) {
    return [wrongDigest](xanadu::Store &, std::string_view bytes,
                         std::uint64_t start, std::string_view scroll)
               -> std::expected<xanadu::SignedProvenance, std::string> {
      xanadu::Provenance record;
      record.author        = {"Fixture author", "author@example.invalid", {}};
      record.permascroll   = scroll;
      record.contentLength = bytes.size();
      record.contentDigest =
          xanadu::sha256Hex(wrongDigest ? std::string_view{} : bytes);
      record.extra.emplace_back("content_start", std::to_string(start));
      return xanadu::SignedProvenance{record.toTsv(), "fixture signature"};
    };
  };
  zigzag::ArenaManifold arena;
  const auto root = arena.makeCell("the separately signed primedia");
  xanadu::Store missingSigner;
  EXPECT_FALSE(publishValue(missingSigner, {}, arena, root, keys, identity,
                            history, directory));
  xanadu::Store wrongCoverage;
  EXPECT_FALSE(publishValue(wrongCoverage, {}, arena, root, keys, identity,
                            history, directory, {}, {}, {},
                            signPrimedia(true)));
  xanadu::Store correct;
  auto result = publishValue(correct, {}, arena, root, keys, identity, history,
                             directory, {}, {}, {}, signPrimedia(false));
  ASSERT_TRUE(result) << result.error();
  ASSERT_FALSE(result->payloads.empty());
  seed(*result);
  EXPECT_TRUE(fetchValue(result->carrier.hash, source,
                         std::make_shared<xanadu::UserPermascroll>()));
}

TEST_F(MeshValueTest,
       ArbitraryOperationHandlesRetainGlobalIdentityAndLabelProvenance) {
  xanadu::Store publisher;
  const auto edit      = publisher.insert({}, 0, "historical edit");
  const auto editIndex = publisher.segmentedOps().indexOf(edit);
  zigzag::ArenaManifold outgoing;
  const auto handle = outgoing.makeCell("comment on the edit");
  ASSERT_TRUE(
      outgoing.setValueBits(handle, xanadu::ValueKind::OpHandle, editIndex));
  auto sealed = publishValue(publisher, edit, outgoing, handle, keys, identity,
                             provenance, directory);
  ASSERT_TRUE(sealed) << sealed.error();
  seed(*sealed);
  auto received = fetchValue(sealed->carrier.hash, source,
                             std::make_shared<xanadu::UserPermascroll>());
  ASSERT_TRUE(received) << received.error();
  const auto originalTarget = received->manifold->handleTarget(received->root);
  ASSERT_TRUE(originalTarget);
  const auto &history =
      received->value.publication.scrolls.at(received->value.root.scroll);
  const auto global =
      xanadu::opRefOf(*received->store, *originalTarget, history);
  xanadu::Store destination;
  destination.setContentSource(&source);
  const auto unrelated =
      destination.insert({}, 0, "unrelated receiver history");
  const auto before = destination.primedia().size();
  auto imported     = importValue(*received, destination);
  ASSERT_TRUE(imported) << imported.error();
  EXPECT_EQ(destination.primedia().size(), before);
  EXPECT_EQ(imported->arena->valueKindOf(imported->root),
            xanadu::ValueKind::ForeignOpHandle);
  EXPECT_FALSE(imported->arena->handleTarget(imported->root));
  const auto target = xanadu::foreignOpTarget(*imported->arena, imported->root);
  ASSERT_TRUE(target);
  EXPECT_EQ(*target, global);
  const auto resolved =
      xanadu::resolveForeignOperation(*target, *received->store, history);
  ASSERT_TRUE(resolved);
  EXPECT_EQ(&resolved->store, received->store.get());
  EXPECT_EQ(resolved->store.getCompactOp(resolved->index)->kind,
            xanadu::OpKind::Insert);
  const auto labelDimension =
      imported->arena->dimensionNamed("d.op-label", destination);
  ASSERT_TRUE(labelDimension);
  const auto label =
      imported->arena->linked(imported->root, *labelDimension, DimVector::POS);
  ASSERT_NE(label, zigzag::noCell);
  EXPECT_EQ(imported->arena->textOf(label), "comment on the edit");
  EXPECT_EQ(
      xanadu::globalise(destination, imported->arena->contentOf(label).front()),
      xanadu::globalise(*received->store,
                        received->manifold->contentOf(received->root).front()));
  const auto promoted =
      zigzag::promote(destination, unrelated, *imported->arena, imported->root);
  ASSERT_TRUE(promoted);
  auto fold = destination.rebuildManifold(promoted->version);
  ASSERT_FALSE(promoted->cells.empty());
  EXPECT_EQ(xanadu::foreignOpTarget(fold, promoted->cells.front(), destination),
            global);
  std::ostringstream wire;
  xanadu::writeBinaryOpsSpool(wire, destination.opRecords());
  std::istringstream input(wire.str());
  std::vector<xanadu::OpRecord> records;
  xanadu::readOpsSpool(input, records);
  EXPECT_TRUE(std::ranges::any_of(records, [](const auto &record) {
    return xanadu::valueKindOf(record.op.flags) ==
               xanadu::ValueKind::ForeignOpHandle &&
           record.op.value == 0;
  }));
}

TEST_F(MeshValueTest,
       ExplicitCellRelocationsPromoteAndImportWithoutForeignIndices) {
  zigzag::ArenaManifold arena;
  const auto root      = arena.makeCell("relocation operand");
  const auto target    = arena.makeCell("target instruction");
  const auto dimension = arena.makeCell(xanadu::cellRelocationDimension);
  const auto marker    = arena.makeCell(xanadu::cellRelocationMarker);
  ASSERT_TRUE(arena.setValueBits(root, xanadu::ValueKind::OpHandle, target));
  ASSERT_TRUE(arena.link(root, dimension, DimVector::POS, marker));
  xanadu::Store publisher;
  auto sealed = publishValue(publisher, {}, arena, root, keys, identity,
                             provenance, directory);
  ASSERT_TRUE(sealed) << sealed.error();
  seed(*sealed);
  auto received = fetchValue(sealed->carrier.hash, source,
                             std::make_shared<xanadu::UserPermascroll>());
  ASSERT_TRUE(received) << received.error();
  EXPECT_TRUE(xanadu::isCellRelocationHandle(*received->manifold,
                                             received->root, *received->store));
  xanadu::Store destination;
  destination.setContentSource(&source);
  auto imported = importValue(*received, destination);
  ASSERT_TRUE(imported) << imported.error();
  const auto localTarget = imported->arena->handleTarget(imported->root);
  ASSERT_TRUE(localTarget);
  EXPECT_TRUE(zigzag::isEphemeral(*localTarget));
  EXPECT_TRUE(imported->arena->contains(*localTarget));
  EXPECT_EQ(imported->arena->textOf(*localTarget), "target instruction");
  auto promoted =
      zigzag::promote(destination, {}, *imported->arena, imported->root);
  ASSERT_TRUE(promoted);
  const auto fold        = destination.rebuildManifold(promoted->version);
  const auto finalTarget = fold.handleTarget(promoted->cells.front());
  ASSERT_TRUE(finalTarget);
  EXPECT_FALSE(zigzag::isEphemeral(*finalTarget));
  EXPECT_EQ(fold.textOf(*finalTarget, destination), "target instruction");
}

TEST_F(MeshValueTest, CyclicCellRelocationsUseImmutableBirthsAndReplayLocally) {
  zigzag::ArenaManifold arena;
  const auto left        = arena.makeCell("left label");
  const auto right       = arena.makeCell("right label");
  const auto dimension   = arena.makeCell(xanadu::cellRelocationDimension);
  const auto leftMarker  = arena.makeCell(xanadu::cellRelocationMarker);
  const auto rightMarker = arena.makeCell(xanadu::cellRelocationMarker);
  ASSERT_TRUE(arena.link(left, dimension, DimVector::POS, leftMarker));
  ASSERT_TRUE(arena.link(right, dimension, DimVector::POS, rightMarker));
  ASSERT_TRUE(arena.setValueBits(left, xanadu::ValueKind::OpHandle, right));
  ASSERT_TRUE(arena.setValueBits(right, xanadu::ValueKind::OpHandle, left));
  xanadu::Store publisher;
  const auto parent = publisher.insert({}, 0, "unchanged parent");
  auto outgoing = publishValue(publisher, parent, arena, left, keys, identity,
                               provenance, directory);
  ASSERT_TRUE(outgoing) << outgoing.error();
  EXPECT_EQ(publisher.textOf(parent), "unchanged parent");
  seed(*outgoing);
  auto received = fetchValue(outgoing->carrier.hash, source,
                             std::make_shared<xanadu::UserPermascroll>());
  ASSERT_TRUE(received) << received.error();
  const auto receivedRight = received->manifold->handleTarget(received->root);
  ASSERT_TRUE(receivedRight);
  EXPECT_EQ(received->manifold->handleTarget(*receivedRight), received->root);
  xanadu::Store destination;
  destination.setContentSource(&source);
  auto imported = importValue(*received, destination);
  ASSERT_TRUE(imported) << imported.error();
  auto answer =
      zigzag::promote(destination, {}, *imported->arena, imported->root);
  ASSERT_TRUE(answer);
  ASSERT_FALSE(answer->cells.empty());
  const auto root  = answer->cells.front();
  const auto fold  = destination.rebuildManifold(answer->version);
  const auto other = fold.handleTarget(root);
  ASSERT_TRUE(other);
  EXPECT_EQ(fold.handleTarget(*other), root);
  EXPECT_EQ(fold.textOf(root, destination), "left label");
  EXPECT_EQ(fold.textOf(*other, destination), "right label");
  EXPECT_EQ(fold.refusedOps(), 0U);
  EXPECT_EQ(destination.getCompactOp(root)->kind, xanadu::OpKind::Structure);
  EXPECT_EQ(xanadu::structureVerbOf(destination.getCompactOp(root)->flags),
            xanadu::StructureVerb::MakeCell);
}

TEST(MeshForeignHandleTest,
     DescriptorAndUnmarkedEphemeralTargetsRefuseMalformedAddresses) {
  const xanadu::GlobalOpRef ref{"btpk:history",
                                xanadu::MicroversionId::parse("1")};
  const auto encoded = xanadu::writeForeignOpHandle(ref);
  EXPECT_EQ(xanadu::readForeignOpHandle(encoded), ref);
  EXPECT_FALSE(xanadu::readForeignOpHandle(encoded + "trailing"));
  EXPECT_FALSE(xanadu::readForeignOpHandle(std::string(10000, 'x')));
  auto wrongVersion    = encoded;
  wrongVersion.front() = '\x02';
  EXPECT_EQ(xanadu::readForeignOpHandle(wrongVersion).error(),
            xanadu::ForeignOpHandleError::UnsupportedVersion);
  zigzag::ArenaManifold arena;
  const auto handle = arena.makeCell();
  const auto target = arena.makeCell("no birth operation yet");
  ASSERT_TRUE(arena.setValueBits(handle, xanadu::ValueKind::OpHandle, target));
  xanadu::Store destination;
  EXPECT_FALSE(zigzag::promote(destination, {}, arena, handle));
  EXPECT_EQ(destination.opCount(), 0U);
}

TEST_F(MeshValueTest, ArrivalOrderAndRedeliveryDoNotChangeMergedOperations) {
  auto a = make("answer A"), b = make("answer B");
  ASSERT_TRUE(a) << a.error();
  ASSERT_TRUE(b) << b.error();
  seed(*a);
  seed(*b);
  auto ar = fetchValue(a->carrier.hash, source,
                       std::make_shared<xanadu::UserPermascroll>());
  auto br = fetchValue(b->carrier.hash, source,
                       std::make_shared<xanadu::UserPermascroll>());
  ASSERT_TRUE(ar) << ar.error();
  ASSERT_TRUE(br) << br.error();
  xanadu::Store first, second;
  first.setContentSource(&source);
  second.setContentSource(&source);
  const std::vector<const ReceivedValue *> forward{&*ar, &*br, &*ar};
  const std::vector<const ReceivedValue *> reverse{&*br, &*ar};
  auto one = mergeValues(first, {}, forward),
       two = mergeValues(second, {}, reverse);
  ASSERT_TRUE(one) << one.error();
  ASSERT_TRUE(two) << two.error();
  ASSERT_EQ(one->size(), 2U);
  ASSERT_EQ(two->size(), 2U);
  EXPECT_NE((*one)[0].promoted.version, (*one)[1].promoted.version);
  std::ostringstream before, after;
  xanadu::writeBinaryOpsSpool(before, first.opRecords());
  xanadu::writeBinaryOpsSpool(after, second.opRecords());
  EXPECT_EQ(before.str(), after.str());
  EXPECT_EQ(first.primedia().size(), second.primedia().size());
}
} // namespace
