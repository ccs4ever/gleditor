#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "common/xanadu/foreign_op_handle.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/mesh_result.hpp"
#include "common/xanadu/vortex/mesh_value.hpp"

namespace zigzag::vortex::mesh {
namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory()
      : path_(std::filesystem::temp_directory_path() /
              ("mesh_result_publication_guard_" +
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

CellRef oneRowResult(ArenaManifold &arena, CellRef target) {
  const CellRef root      = arena.makeCell("mesh:result/v2");
  const DimRef items      = arena.makeCell("d.mesh.items");
  const DimRef value      = arena.makeCell("d.mesh.value");
  const DimRef count      = arena.makeCell("d.mesh.count");
  const DimRef relocation = arena.makeCell(xanadu::cellRelocationDimension);
  const CellRef countCell = arena.makeScalarCell(std::int64_t{1});
  const CellRef row       = arena.makeCell();
  const CellRef wrapper   = arena.makeCell();
  const CellRef marker    = arena.makeCell(xanadu::cellRelocationMarker);

  EXPECT_TRUE(arena.setValueBits(wrapper, xanadu::ValueKind::OpHandle, target));
  EXPECT_TRUE(arena.link(wrapper, relocation, DimVector::POS, marker));
  EXPECT_TRUE(arena.link(root, count, DimVector::POS, countCell));
  EXPECT_TRUE(arena.link(root, items, DimVector::POS, row));
  EXPECT_TRUE(arena.link(row, value, DimVector::POS, wrapper));
  return root;
}

struct SourceCell {
  xanadu::MicroversionId version;
  CellRef ref;
  Manifold manifold;
};

SourceCell makeSourceCell(xanadu::Store &store, std::string_view text) {
  const auto genesis = store.sliceGenesis({});
  const auto version = store.makeCell(genesis, text);
  return {.version  = version,
          .ref      = store.cellRefOf(version).value(),
          .manifold = store.rebuildManifold(version)};
}

TEST(MeshResultPublicationGuardTest,
     CrossStoreCellRefCollisionRefusesBeforeEitherStoreChanges) {
  xanadu::Store source;
  const auto sourceCell = makeSourceCell(source, "source payload");
  xanadu::Store publisher;
  const auto publisherCell = makeSourceCell(publisher, "publisher payload");
  ASSERT_EQ(publisherCell.ref, sourceCell.ref);
  EXPECT_EQ(sourceCell.manifold.textOf(sourceCell.ref, source),
            "source payload");
  EXPECT_EQ(publisherCell.manifold.textOf(publisherCell.ref, publisher),
            "publisher payload");

  ArenaManifold arena(&sourceCell.manifold);
  const CellRef root = oneRowResult(arena, sourceCell.ref);
  const auto rows    = readResultCells(arena, root);
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), 1U);
  EXPECT_EQ(rows->front(), sourceCell.ref);
  EXPECT_EQ(arena.textOf(rows->front()), "source payload");

  const auto sourceOps     = source.opCount();
  const auto sourceBytes   = source.primedia().size();
  const auto publisherOps  = publisher.opCount();
  const auto publisherData = publisher.primedia().size();
  auto keys                = xanadu::createMutableKeys();
  NodeId identity{"mesh-result-guard", keys.publicKey.hex(), 1};
  const xanadu::SignedProvenance provenance{"fixture authorship",
                                            "fixture signature"};
  TemporaryDirectory temporary;

  const auto published =
      publishValue(publisher, publisherCell.version, arena, root, keys,
                   identity, provenance, temporary.path());
  ASSERT_FALSE(published);
  EXPECT_NE(published.error().find("cross-store"), std::string::npos);
  EXPECT_NE(published.error().find("persistent base-cell identity"),
            std::string::npos);
  EXPECT_EQ(source.opCount(), sourceOps);
  EXPECT_EQ(source.primedia().size(), sourceBytes);
  EXPECT_EQ(publisher.opCount(), publisherOps);
  EXPECT_EQ(publisher.primedia().size(), publisherData);
}

TEST(MeshResultPublicationGuardTest,
     CrossStoreMissingCellRefAlsoRefusesBeforeStoreChanges) {
  xanadu::Store source;
  const auto sourceCell = makeSourceCell(source, "source payload");
  xanadu::Store publisher;
  const auto publisherParent = publisher.sliceGenesis({});
  ASSERT_EQ(publisher.getCompactOp(sourceCell.ref), nullptr);

  ArenaManifold arena(&sourceCell.manifold);
  const CellRef root = oneRowResult(arena, sourceCell.ref);
  const auto rows    = readResultCells(arena, root);
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), 1U);
  EXPECT_EQ(arena.textOf(rows->front()), "source payload");

  const auto sourceOps     = source.opCount();
  const auto sourceBytes   = source.primedia().size();
  const auto publisherOps  = publisher.opCount();
  const auto publisherData = publisher.primedia().size();
  auto keys                = xanadu::createMutableKeys();
  NodeId identity{"mesh-result-guard", keys.publicKey.hex(), 1};
  const xanadu::SignedProvenance provenance{"fixture authorship",
                                            "fixture signature"};
  TemporaryDirectory temporary;

  const auto published =
      publishValue(publisher, publisherParent, arena, root, keys, identity,
                   provenance, temporary.path());
  ASSERT_FALSE(published);
  EXPECT_NE(published.error().find("cross-store"), std::string::npos);
  EXPECT_EQ(source.opCount(), sourceOps);
  EXPECT_EQ(source.primedia().size(), sourceBytes);
  EXPECT_EQ(publisher.opCount(), publisherOps);
  EXPECT_EQ(publisher.primedia().size(), publisherData);
}

TEST(MeshResultPublicationGuardTest,
     CrossStoreRawOperationHandleRefusesBeforeStoreChanges) {
  xanadu::Store source;
  const auto sourceVersion = source.insert({}, 0, "source operation");
  const CellRef sourceOperation =
      source.segmentedOps().indexOf(sourceVersion).value();
  const auto sourceManifold = source.rebuildManifold(sourceVersion);
  xanadu::Store publisher;
  const auto publisherVersion = publisher.insert({}, 0, "publisher operation");
  ASSERT_EQ(publisher.segmentedOps().indexOf(publisherVersion).value(),
            sourceOperation);

  ArenaManifold arena(&sourceManifold);
  const CellRef handle = arena.makeCell("operation reference");
  ASSERT_TRUE(
      arena.setValueBits(handle, xanadu::ValueKind::OpHandle, sourceOperation));
  const CellRef root = oneRowResult(arena, handle);
  const auto rows    = readResultCells(arena, root);
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), 1U);
  EXPECT_EQ(rows->front(), handle);

  const auto sourceOps     = source.opCount();
  const auto sourceBytes   = source.primedia().size();
  const auto publisherOps  = publisher.opCount();
  const auto publisherData = publisher.primedia().size();
  auto keys                = xanadu::createMutableKeys();
  NodeId identity{"mesh-result-guard", keys.publicKey.hex(), 1};
  const xanadu::SignedProvenance provenance{"fixture authorship",
                                            "fixture signature"};
  TemporaryDirectory temporary;

  const auto published =
      publishValue(publisher, publisherVersion, arena, root, keys, identity,
                   provenance, temporary.path());
  ASSERT_FALSE(published);
  EXPECT_NE(published.error().find("cross-store"), std::string::npos);
  EXPECT_EQ(source.opCount(), sourceOps);
  EXPECT_EQ(source.primedia().size(), sourceBytes);
  EXPECT_EQ(publisher.opCount(), publisherOps);
  EXPECT_EQ(publisher.primedia().size(), publisherData);
}

TEST(MeshResultPublicationGuardTest,
     SameStoreBaseCellResultRemainsPublishable) {
  xanadu::Store store;
  const auto base = makeSourceCell(store, "source payload");
  ArenaManifold arena(&base.manifold);
  const CellRef root = oneRowResult(arena, base.ref);
  const auto rows    = readResultCells(arena, root);
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), 1U);
  EXPECT_EQ(arena.textOf(rows->front()), "source payload");

  auto keys = xanadu::createMutableKeys();
  NodeId identity{"mesh-result-guard", keys.publicKey.hex(), 1};
  const xanadu::SignedProvenance provenance{"fixture authorship",
                                            "fixture signature"};
  TemporaryDirectory temporary;
  const auto published = publishValue(store, base.version, arena, root, keys,
                                      identity, provenance, temporary.path());
  ASSERT_TRUE(published) << published.error();
}

} // namespace
} // namespace zigzag::vortex::mesh
