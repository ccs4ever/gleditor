/**
 * @file vortex_stdlib_image_test.cpp
 * @brief Whole-space Vortex stdlib image export invariants.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "common/xanadu/document_id.hpp"
#include "common/xanadu/relocation_marks.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/stdlib_image_format.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::noCell;
using namespace zigzag::vortex;

struct SourceImage {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  VortexStdLib stdlib{core, vm};
};

xanadu::DocumentId fixedDocumentId() {
  xanadu::DocumentId id;
  std::string bytes(16, '\0');
  for (std::size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<char>(i + 1);
  if (!xanadu::DocumentId::fromBytes(bytes, id))
    throw std::logic_error("could not create fixed document id");
  return id;
}

std::shared_ptr<xanadu::UserPermascroll> freshPermascroll() {
  return std::make_shared<xanadu::UserPermascroll>();
}

std::optional<CellRef> rankCell(const zigzag::Manifold &manifold,
                                const xanadu::Store &store, CellRef anchor,
                                DimRef dim, std::string_view text) {
  for (const CellRef cell : zigzag::rankAfter(manifold, anchor, dim))
    if (manifold.textOf(cell, store) == text) return cell;
  return std::nullopt;
}

std::optional<CellRef> fieldValue(const zigzag::Manifold &manifold,
                                  const xanadu::Store &store, CellRef owner,
                                  DimRef vars, DimRef values,
                                  std::string_view name) {
  const auto key = rankCell(manifold, store, owner, vars, name);
  return key ? manifold.linked(*key, values, DimVector::POS) : std::nullopt;
}

TEST(VortexStdLibImageTest, ExportsGenesisRegistryAndRelocatesReferences) {
  SourceImage source;
  constexpr std::int64_t ordinaryNumber = 918273645;
  const CellRef ordinary = source.arena.makeScalarCell(ordinaryNumber);
  ASSERT_TRUE(source.arena.link(source.core.home(), source.core.dims().scan,
                                DimVector::POS, ordinary));

  auto permascroll = freshPermascroll();
  xanadu::Store store{permascroll, fixedDocumentId()};
  const auto exported = source.stdlib.exportImageToStore(store);
  ASSERT_TRUE(exported) << exported.error();
  EXPECT_FALSE(store.hasPendingMetadata());

  const auto folded = store.rebuildManifold(*exported);
  ASSERT_EQ(folded.refusedOps(), 0U);
  ASSERT_TRUE(folded.home());
  ASSERT_TRUE(folded.dimsDimension());
  ASSERT_GE(folded.cells().size(), 2U);
  EXPECT_EQ(folded.cells()[0].birthOp, *folded.home());
  EXPECT_EQ(folded.cells()[1].birthOp, *folded.dimsDimension());
  EXPECT_EQ(folded.textOf(*folded.dimsDimension(), store), "d.dims");

  std::vector<std::string> sourceDimensions;
  for (const CellRef dim : zigzag::rank(source.arena, source.core.dims().dims,
                                        source.core.dims().dims)) {
    const std::string name = source.arena.textOf(dim);
    sourceDimensions.push_back(name);
    EXPECT_TRUE(folded.dimensionNamed(name, store)) << name;
  }
  EXPECT_TRUE(std::ranges::find(sourceDimensions, "d.params") !=
              sourceDimensions.end());
  EXPECT_TRUE(std::ranges::find(sourceDimensions, "d.mesh.relocation") !=
              sourceDimensions.end());

  const auto relocationDim = folded.dimensionNamed("d.mesh.relocation", store);
  ASSERT_TRUE(relocationDim);
  std::size_t foldedInt64Marks  = 0;
  std::size_t foldedHandleMarks = 0;
  bool foundOrdinaryNumber      = false;
  for (const auto &cell : folded.cells()) {
    const CellRef ref = cell.birthOp;
    if (folded.asInt64(ref) == ordinaryNumber &&
        !folded.linked(ref, *relocationDim, DimVector::POS)) {
      foundOrdinaryNumber = true;
    }
    const auto marker = folded.linked(ref, *relocationDim, DimVector::POS);
    if (!marker || folded.textOf(*marker, store) != "cell") continue;
    if (folded.valueKindOf(ref) == xanadu::ValueKind::Int64) {
      const auto raw = folded.asInt64(ref);
      ASSERT_TRUE(raw);
      ASSERT_GT(*raw, 0);
      ASSERT_LE(static_cast<std::uint64_t>(*raw),
                std::numeric_limits<CellRef>::max());
      const CellRef target = static_cast<CellRef>(*raw);
      EXPECT_FALSE(zigzag::isEphemeral(target));
      EXPECT_TRUE(folded.contains(target));
      ++foldedInt64Marks;
    } else if (folded.valueKindOf(ref) == xanadu::ValueKind::OpHandle) {
      const auto target = folded.handleTarget(ref);
      ASSERT_TRUE(target);
      EXPECT_FALSE(zigzag::isEphemeral(*target));
      EXPECT_TRUE(folded.contains(*target));
      ++foldedHandleMarks;
    }
  }
  const auto sourceMarks = xanadu::markedRelocationSlots(source.arena);
  const auto sourceInt64Marks =
      std::ranges::count_if(sourceMarks, [&](const CellRef ref) {
        return source.arena.valueKindOf(ref) == xanadu::ValueKind::Int64;
      });
  const auto sourceHandleMarks =
      std::ranges::count_if(sourceMarks, [&](const CellRef ref) {
        return source.arena.valueKindOf(ref) == xanadu::ValueKind::OpHandle;
      });
  EXPECT_EQ(foldedInt64Marks, sourceInt64Marks);
  EXPECT_EQ(foldedHandleMarks, sourceHandleMarks);
  EXPECT_TRUE(foundOrdinaryNumber);

  const auto vars   = folded.dimensionNamed("d.vars", store);
  const auto values = folded.dimensionNamed("d.values", store);
  const auto step   = folded.dimensionNamed("d.step", store);
  ASSERT_TRUE(vars);
  ASSERT_TRUE(values);
  ASSERT_TRUE(step);
  const auto imageKey =
      rankCell(folded, store, *folded.home(), *vars, "stdlib.image");
  ASSERT_TRUE(imageKey);
  const auto holder = folded.linked(*imageKey, *values, DimVector::POS);
  ASSERT_TRUE(holder);
  const auto opsHead =
      fieldValue(folded, store, *holder, *vars, *values, "ops");
  ASSERT_TRUE(opsHead);
  std::size_t opMembers = 0;
  for (const CellRef member : zigzag::rankAfter(folded, *opsHead, *step)) {
    const auto raw = folded.asInt64(member);
    ASSERT_TRUE(raw);
    ASSERT_GT(*raw, 0);
    const CellRef target = static_cast<CellRef>(*raw);
    EXPECT_FALSE(zigzag::isEphemeral(target));
    EXPECT_TRUE(folded.contains(target));
    ++opMembers;
  }
  EXPECT_GT(opMembers, 0U);

  const auto params    = folded.dimensionNamed("d.params", store);
  const auto stdlibDim = folded.dimensionNamed("d.stdlib", store);
  ASSERT_TRUE(params);
  ASSERT_TRUE(stdlibDim);
  std::size_t signatureMembers = 0;
  for (const CellRef module :
       zigzag::rankAfter(folded, *folded.home(), *stdlibDim)) {
    for (const CellRef symbol : zigzag::rankAfter(folded, module, *vars)) {
      const auto signature = folded.linked(symbol, *params, DimVector::POS);
      ASSERT_TRUE(signature);
      for (const CellRef record :
           zigzag::rankAfter(folded, *signature, *vars)) {
        const auto memberHead = folded.linked(record, *values, DimVector::POS);
        ASSERT_TRUE(memberHead);
        for (const CellRef member :
             zigzag::rankAfter(folded, *memberHead, *step)) {
          const auto raw = folded.asInt64(member);
          ASSERT_TRUE(raw);
          ASSERT_GT(*raw, 0);
          ASSERT_LE(static_cast<std::uint64_t>(*raw),
                    std::numeric_limits<CellRef>::max());
          const CellRef target = static_cast<CellRef>(*raw);
          EXPECT_FALSE(zigzag::isEphemeral(target));
          EXPECT_TRUE(folded.contains(target));
          ++signatureMembers;
        }
      }
    }
  }
  EXPECT_GT(signatureMembers, 0U);

  const auto epoch =
      fieldValue(folded, store, *holder, *vars, *values, "imageEpoch");
  const auto version =
      fieldValue(folded, store, *holder, *vars, *values, "contentVersion");
  const auto head =
      fieldValue(folded, store, *holder, *vars, *values, "generatorHead");
  const auto scroll =
      fieldValue(folded, store, *holder, *vars, *values, "expectedScrollBytes");
  ASSERT_TRUE(epoch);
  ASSERT_TRUE(version);
  ASSERT_TRUE(head);
  ASSERT_TRUE(scroll);
  EXPECT_EQ(folded.textOf(*epoch, store), stdlibImageEpoch());
  EXPECT_EQ(folded.asInt64(*version), kStdlibContentVersion);
  EXPECT_EQ(folded.textOf(*head, store), exported->str());
  EXPECT_EQ(folded.asInt64(*scroll),
            static_cast<std::int64_t>(permascroll->size()));
  const auto opcodeCount =
      fieldValue(folded, store, *holder, *vars, *values, "imageOpcodeCount");
  const auto verbCount =
      fieldValue(folded, store, *holder, *vars, *values, "imageVerbCount");
  ASSERT_TRUE(opcodeCount);
  ASSERT_TRUE(verbCount);
  EXPECT_EQ(folded.asInt64(*opcodeCount),
            static_cast<std::int64_t>(stdlibImageOpcodeCount()));
  EXPECT_EQ(folded.asInt64(*verbCount),
            static_cast<std::int64_t>(stdlibImageVerbCount()));
  EXPECT_EQ(
      store.segmentedOps().idOf(static_cast<std::uint32_t>(store.opCount())),
      *exported);
}

TEST(VortexStdLibImageTest, FoldedImageRebindsClampAndGoalAliases) {
  SourceImage source;
  auto permascroll = freshPermascroll();
  xanadu::Store store{permascroll, fixedDocumentId()};
  const auto exported = source.stdlib.exportImageToStore(store);
  ASSERT_TRUE(exported) << exported.error();
  const auto folded = store.rebuildManifold(*exported);

  const auto invoke = [&](const std::string_view path) {
    ArenaManifold arena{&folded};
    VortexCore core{arena};
    VortexVM vm{core};
    VortexStdLib rebound{core, vm, VortexStdLib::Bootstrap::Manual};
    EXPECT_TRUE(rebound.rebindInstance());

    EXPECT_EQ(
        rebound.call("std:math/clamp",
                     {std::int64_t{150}, std::int64_t{0}, std::int64_t{100}}),
        (std::vector<CellValue>{std::int64_t{100}}));

    const auto entry = rebound.resolve(path);
    ASSERT_TRUE(entry) << path;
    const auto inputs  = rebound.routineInputs(*entry);
    const auto outputs = rebound.routineOutputs(*entry);
    ASSERT_EQ(inputs.size(), 4U);
    ASSERT_EQ(outputs.size(), 1U);
    ASSERT_TRUE(arena.setValueBits(inputs[0], xanadu::ValueKind::Int64, 7));
    ASSERT_TRUE(arena.setValueBits(inputs[1], xanadu::ValueKind::Int64, 1));
    EXPECT_EQ(
        rebound.goalBoundedFirst(*entry, outputs[0], rebound.goalHaltOp()),
        (std::optional<CellValue>{CellValue{std::int64_t{7}}}));
    EXPECT_EQ(arena.asInt64(outputs[0]), 7) << path;
    EXPECT_EQ(arena.liveMarks(), 0U);
  };
  invoke("std:goal/from");
  invoke("std:goal/seq");
}

TEST(VortexStdLibImageTest, InvalidPreconditionsLeaveDestinationUntouched) {
  SourceImage source;
  const auto tryExport = [&](const std::int64_t contentVersion) {
    auto permascroll = freshPermascroll();
    xanadu::Store store{permascroll, fixedDocumentId()};
    const auto before = store.opCount();
    EXPECT_FALSE(source.stdlib.exportImageToStore(store, contentVersion));
    EXPECT_EQ(store.opCount(), before);
    EXPECT_TRUE(store.allVersions().empty());
    EXPECT_EQ(permascroll->size(), 0U);
  };
  tryExport(0);
  tryExport(-1);

  {
    auto permascroll = freshPermascroll();
    xanadu::Store store{permascroll, fixedDocumentId()};
    const auto mark = source.arena.mark();
    EXPECT_FALSE(source.stdlib.exportImageToStore(store));
    EXPECT_TRUE(store.allVersions().empty());
    EXPECT_EQ(permascroll->size(), 0U);
    source.arena.release(mark);
  }
  {
    ArenaManifold arena;
    VortexCore core{arena};
    VortexVM vm{core};
    VortexStdLib manual{core, vm, VortexStdLib::Bootstrap::Manual};
    auto permascroll = freshPermascroll();
    xanadu::Store store{permascroll, fixedDocumentId()};
    EXPECT_FALSE(manual.exportImageToStore(store));
    EXPECT_TRUE(store.allVersions().empty());
    EXPECT_EQ(permascroll->size(), 0U);
  }
  {
    auto permascroll = freshPermascroll();
    xanadu::Store store{permascroll, fixedDocumentId()};
    const auto occupied = store.makeCell(xanadu::MicroversionId{}, "occupied");
    ASSERT_FALSE(occupied.isZero());
    const auto before = store.opCount();
    EXPECT_FALSE(source.stdlib.exportImageToStore(store));
    EXPECT_EQ(store.opCount(), before);
    EXPECT_GT(permascroll->size(), 0U);
  }
  {
    zigzag::Manifold base;
    ArenaManifold arena{&base};
    VortexCore core{arena};
    VortexVM vm{core};
    VortexStdLib based{core, vm};
    auto permascroll = freshPermascroll();
    xanadu::Store store{permascroll, fixedDocumentId()};
    EXPECT_FALSE(based.exportImageToStore(store));
    EXPECT_TRUE(store.allVersions().empty());
    EXPECT_EQ(permascroll->size(), 0U);
  }
  {
    SourceImage malformed;
    std::optional<CellRef> key;
    for (const CellRef cell :
         zigzag::rankAfter(malformed.arena, malformed.core.home(),
                           malformed.core.dims().vars)) {
      if (malformed.arena.textOf(cell) == "stdlib.image") key = cell;
    }
    ASSERT_TRUE(key);
    const auto holder = malformed.arena.linked(
        *key, malformed.core.dims().values, DimVector::POS);
    ASSERT_TRUE(holder);
    std::optional<CellRef> epoch;
    for (const CellRef cell : zigzag::rankAfter(malformed.arena, *holder,
                                                malformed.core.dims().vars)) {
      if (malformed.arena.textOf(cell) == "imageEpoch") epoch = cell;
    }
    ASSERT_TRUE(epoch);
    const auto value = malformed.arena.linked(
        *epoch, malformed.core.dims().values, DimVector::POS);
    ASSERT_TRUE(value);
    ASSERT_TRUE(malformed.arena.setText(*value, "stale-epoch"));

    auto permascroll = freshPermascroll();
    xanadu::Store store{permascroll, fixedDocumentId()};
    EXPECT_FALSE(malformed.stdlib.exportImageToStore(store));
    EXPECT_TRUE(store.allVersions().empty());
    EXPECT_EQ(permascroll->size(), 0U);
  }
}

TEST(VortexStdLibImageTest, FreshInMemoryGenerationIsDeterministic) {
  SourceImage firstSource;
  SourceImage secondSource;
  auto firstPermascroll  = freshPermascroll();
  auto secondPermascroll = freshPermascroll();
  xanadu::Store first{firstPermascroll, fixedDocumentId()};
  xanadu::Store second{secondPermascroll, fixedDocumentId()};

  const auto firstHead  = firstSource.stdlib.exportImageToStore(first);
  const auto secondHead = secondSource.stdlib.exportImageToStore(second);
  ASSERT_TRUE(firstHead) << firstHead.error();
  ASSERT_TRUE(secondHead) << secondHead.error();
  EXPECT_EQ(first.documentId(), second.documentId());
  EXPECT_EQ(*firstHead, *secondHead);
  EXPECT_EQ(first.exportBinaryOps(), second.exportBinaryOps());
  EXPECT_EQ(firstPermascroll->bytes(), secondPermascroll->bytes());
}

} // namespace
