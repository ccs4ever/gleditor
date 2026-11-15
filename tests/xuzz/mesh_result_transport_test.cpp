#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/mesh_evaluator.hpp"
#include "common/xanadu/vortex/mesh_result.hpp"
#include "common/xanadu/vortex/mesh_value.hpp"

namespace zigzag::vortex::mesh {
namespace {

constexpr EvaluationBudget budget{1'000'000, 100'000, 100, 1'000'000};
constexpr std::string_view repeatedValues = R"(
  let $big := 9007199254740993;
  let $f := \(...$rest) $rest;
  return $f($big, $big, $big, 4, 4.0, 1.2345678901234567)
)";

class TemporaryDirectory {
public:
  TemporaryDirectory()
      : path_(std::filesystem::temp_directory_path() /
              ("mesh_result_transport_" +
               std::to_string(std::chrono::steady_clock::now()
                                  .time_since_epoch()
                                  .count()))) {
    std::filesystem::create_directories(path_);
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

std::vector<CellRef> occurrenceWrappers(const Manifold &manifold, CellRef root,
                                        const xanadu::SpanReader &reader) {
  std::optional<DimRef> items;
  for (const auto &edge : manifold.dimensionsOf(root))
    if (manifold.textOf(edge.dim, reader) == "d.mesh.items") items = edge.dim;
  EXPECT_TRUE(items);
  if (!items) return {};

  std::vector<CellRef> wrappers;
  auto record = manifold.linked(root, *items, DimVector::POS);
  while (record) {
    std::optional<DimRef> value;
    for (const auto &edge : manifold.dimensionsOf(*record))
      if (manifold.textOf(edge.dim, reader) == "d.mesh.value") value = edge.dim;
    EXPECT_TRUE(value);
    if (!value) break;
    const auto wrapper = manifold.linked(*record, *value, DimVector::POS);
    EXPECT_TRUE(wrapper);
    if (!wrapper) break;
    wrappers.push_back(*wrapper);
    record = manifold.linked(*record, *items, DimVector::POS);
  }
  return wrappers;
}

TEST(MeshResultTransportTest,
     PublishedRowsSurviveContextDestructionReceiveImportAndStoreReload) {
  TemporaryDirectory temporary;
  auto keys = xanadu::createMutableKeys();
  NodeId identity{"mesh-result-author", keys.publicKey.hex(), 1};
  const xanadu::SignedProvenance provenance{"fixture authorship",
                                            "fixture signature"};
  auto authorScroll =
      std::make_shared<xanadu::UserPermascroll>(xanadu::UserPermascroll::Config{
          .storageDir = temporary.path() / "author-scroll"});
  xanadu::Store evaluatorStore(authorScroll);
  const auto sourceVersion  = evaluatorStore.sliceGenesis({});
  const auto sourceSnapshot = evaluatorStore.rebuildManifold(sourceVersion);
  const auto initialOps     = evaluatorStore.opRecords().size();

  std::optional<ValueSeed> outgoing;
  {
    auto evaluated = evaluate(Frontend::Vquery, repeatedValues, budget,
                              &sourceSnapshot, &evaluatorStore);
    ASSERT_TRUE(evaluated) << evaluated.error();
    auto rows = readResultCells(*evaluated);
    ASSERT_TRUE(rows) << rows.error();
    ASSERT_EQ(rows->size(), 6U);
    EXPECT_EQ((*rows)[0], (*rows)[2]);

    xanadu::Store publisher(authorScroll);
    auto published =
        publishValue(publisher, {}, evaluated->context->arena, evaluated->root,
                     keys, identity, provenance, temporary.path() / "seed");
    ASSERT_TRUE(published) << published.error();
    outgoing = std::move(*published);
    EXPECT_EQ(evaluatorStore.opRecords().size(), initialOps);
  }
  ASSERT_TRUE(outgoing);

  xanadu::DirectoryContentSource content;
  for (const auto &payload : outgoing->payloads)
    content.add(payload.torrent.file, payload.path.string());
  content.add(
      outgoing->history.torrentFile,
      (temporary.path() / "seed" / outgoing->history.hash.hex()).string());
  content.add(outgoing->carrier.file, outgoing->carrierPath.string());

  auto receivedScroll =
      std::make_shared<xanadu::UserPermascroll>(xanadu::UserPermascroll::Config{
          .storageDir = temporary.path() / "receiver-scroll"});
  auto received = fetchValue(outgoing->carrier.hash, content, receivedScroll);
  ASSERT_TRUE(received) << received.error();
  auto rows =
      readResultCells(*received->manifold, received->root, *received->store);
  ASSERT_TRUE(rows) << rows.error();
  ASSERT_EQ(rows->size(), 6U);
  ResultReadLimits tight;
  tight.maxMetadataRelations = 1;
  const auto relationRefusal = readResultCells(
      *received->manifold, received->root, *received->store, tight);
  ASSERT_FALSE(relationRefusal);
  EXPECT_NE(relationRefusal.error().find("budget"), std::string::npos);
  tight                      = {};
  tight.maxMetadataTextBytes = 1;
  const auto textRefusal = readResultCells(*received->manifold, received->root,
                                           *received->store, tight);
  ASSERT_FALSE(textRefusal);
  EXPECT_NE(textRefusal.error().find("budget"), std::string::npos);
  const auto retried =
      readResultCells(*received->manifold, received->root, *received->store);
  ASSERT_TRUE(retried) << retried.error();
  EXPECT_EQ(*retried, *rows);
  EXPECT_EQ((*rows)[0], (*rows)[1]);
  EXPECT_EQ((*rows)[1], (*rows)[2]);
  for (std::size_t i = 0; i < 3; ++i) {
    const CellRef cell = (*rows)[i];
    EXPECT_EQ(received->manifold->valueKindOf(cell), xanadu::ValueKind::Int64);
    EXPECT_EQ(received->manifold->asInt64(cell), 9007199254740993LL);
  }
  EXPECT_EQ(received->manifold->valueKindOf((*rows)[3]),
            xanadu::ValueKind::Int64);
  EXPECT_EQ(received->manifold->asInt64((*rows)[3]), 4);
  EXPECT_EQ(received->manifold->valueKindOf((*rows)[4]),
            xanadu::ValueKind::Double);
  EXPECT_EQ(received->manifold->asDouble((*rows)[4]), 4.0);
  EXPECT_EQ(received->manifold->valueKindOf((*rows)[5]),
            xanadu::ValueKind::Double);
  ASSERT_TRUE(received->manifold->slot((*rows)[5]));
  EXPECT_EQ(received->manifold->slot((*rows)[5])->valueBits,
            std::bit_cast<std::uint64_t>(1.2345678901234567));

  const auto wrappers =
      occurrenceWrappers(*received->manifold, received->root, *received->store);
  ASSERT_EQ(wrappers.size(), 6U);
  EXPECT_NE(wrappers[0], wrappers[1]);
  EXPECT_NE(wrappers[1], wrappers[2]);
  for (std::size_t i = 0; i < wrappers.size(); ++i) {
    auto target = readOccurrenceTarget(*received->manifold, wrappers[i],
                                       *received->store);
    ASSERT_TRUE(target) << target.error();
    EXPECT_EQ(*target, (*rows)[i]);
  }

  auto destinationScroll =
      std::make_shared<xanadu::UserPermascroll>(xanadu::UserPermascroll::Config{
          .storageDir = temporary.path() / "destination-scroll"});
  xanadu::Store destination(destinationScroll);
  destination.setContentSource(&content);
  auto imported = importValue(*received, destination);
  ASSERT_TRUE(imported) << imported.error();
  auto importedRows = readResultCells(*imported->arena, imported->root);
  ASSERT_TRUE(importedRows) << importedRows.error();
  ASSERT_EQ(importedRows->size(), 6U);
  EXPECT_EQ((*importedRows)[0], (*importedRows)[1]);
  EXPECT_EQ((*importedRows)[1], (*importedRows)[2]);
  for (std::size_t i = 0; i < 3; ++i) {
    const CellRef cell = (*importedRows)[i];
    EXPECT_EQ(imported->arena->asInt64(cell), 9007199254740993LL);
  }
  ResultReadLimits importedLimits;
  importedLimits.maxItems = 5;
  EXPECT_FALSE(
      readResultCells(*imported->arena, imported->root, importedLimits));
  std::optional<DimRef> importedItems;
  for (const auto &edge : imported->arena->dimensionsOf(imported->root))
    if (imported->arena->textOf(edge.dim) == "d.mesh.items")
      importedItems = edge.dim;
  ASSERT_TRUE(importedItems);
  const auto firstRecord =
      imported->arena->linked(imported->root, *importedItems, DimVector::POS);
  ASSERT_TRUE(firstRecord);
  std::optional<DimRef> importedValue;
  for (const auto &edge : imported->arena->dimensionsOf(*firstRecord))
    if (imported->arena->textOf(edge.dim) == "d.mesh.value")
      importedValue = edge.dim;
  ASSERT_TRUE(importedValue);
  const auto firstWrapper =
      imported->arena->linked(*firstRecord, *importedValue, DimVector::POS);
  ASSERT_TRUE(firstWrapper);
  const auto firstTarget =
      readOccurrenceTarget(*imported->arena, *firstWrapper);
  ASSERT_TRUE(firstTarget) << firstTarget.error();
  EXPECT_EQ(*firstTarget, (*importedRows)[0]);
  EXPECT_TRUE(zigzag::isEphemeral(*firstTarget));

  const auto importedVersion =
      zigzag::promote(destination, {}, *imported->arena, imported->root);
  ASSERT_TRUE(importedVersion);
  const auto inputRoot =
      std::ranges::find(importedVersion->arena, imported->root);
  ASSERT_NE(inputRoot, importedVersion->arena.end());
  const auto persistentRoot = importedVersion->cells[static_cast<std::size_t>(
      inputRoot - importedVersion->arena.begin())];
  const auto storePath      = temporary.path() / "reloaded-store";
  destination.save(storePath.string());
  xanadu::Store reloaded(destinationScroll);
  reloaded.setContentSource(&content);
  reloaded.load(storePath.string());
  const auto reloadedManifold =
      reloaded.rebuildManifold(importedVersion->version);
  ASSERT_TRUE(reloadedManifold.slot(persistentRoot));
  const auto reloadedRows =
      readResultCells(reloadedManifold, persistentRoot, reloaded);
  ASSERT_TRUE(reloadedRows) << reloadedRows.error();
  ASSERT_EQ(reloadedRows->size(), 6U);
  EXPECT_EQ((*reloadedRows)[0], (*reloadedRows)[1]);
  EXPECT_EQ((*reloadedRows)[1], (*reloadedRows)[2]);
  for (std::size_t i = 0; i < 3; ++i) {
    const CellRef cell = (*reloadedRows)[i];
    EXPECT_EQ(reloadedManifold.asInt64(cell), 9007199254740993LL);
  }
  EXPECT_EQ(reloadedManifold.asInt64((*reloadedRows)[3]), 4);
  EXPECT_EQ(reloadedManifold.asDouble((*reloadedRows)[4]), 4.0);
  ASSERT_TRUE(reloadedManifold.slot((*reloadedRows)[5]));
  EXPECT_EQ(reloadedManifold.slot((*reloadedRows)[5])->valueBits,
            std::bit_cast<std::uint64_t>(1.2345678901234567));

  EXPECT_EQ(evaluatorStore.opRecords().size(), initialOps);
}

} // namespace
} // namespace zigzag::vortex::mesh
