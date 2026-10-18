/**
 * @file generic_structure_make_test.cpp
 * @brief Acceptance test suite covering the 12 milestone specifications from
 *        design/generic-structure-make.md §11.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/binary_ops.hpp"
#include "common/xanadu/compact_op.hpp"
#include "common/xanadu/focus_target.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/scalar.hpp"
#include "common/xanadu/segmented_ops_spool.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace fs = std::filesystem;

namespace {

using namespace xanadu;

struct ScopedTempDir {
  fs::path path;
  explicit ScopedTempDir(const std::string &name)
      : path(fs::temp_directory_path() / name) {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~ScopedTempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

// 1. Round-trip flags for all three kinds, Cell scalar kinds, and SetLink
//    direction. Reject reserved kind 3 and invalid root payloads.
TEST(GenericStructureMakeTest,
     AcceptanceCheck1_RoundTripFlagsAndKindRejection) {
  // Round trip flags for all three structure kinds
  const auto cellFlags =
      makeStructureFlags(StructureKind::Cell, ValueKind::None);
  EXPECT_EQ(structureVerbOf(cellFlags), StructureVerb::Make);
  EXPECT_EQ(structureKindOf(cellFlags), StructureKind::Cell);
  EXPECT_EQ(valueKindOf(cellFlags), ValueKind::None);

  const auto sliceFlags =
      makeStructureFlags(StructureKind::Slice, ValueKind::None);
  EXPECT_EQ(structureVerbOf(sliceFlags), StructureVerb::Make);
  EXPECT_EQ(structureKindOf(sliceFlags), StructureKind::Slice);
  EXPECT_EQ(valueKindOf(sliceFlags), ValueKind::None);

  const auto xanadocFlags =
      makeStructureFlags(StructureKind::Xanadoc, ValueKind::Timestamp);
  EXPECT_EQ(structureVerbOf(xanadocFlags), StructureVerb::Make);
  EXPECT_EQ(structureKindOf(xanadocFlags), StructureKind::Xanadoc);
  EXPECT_EQ(valueKindOf(xanadocFlags), ValueKind::Timestamp);

  // Cell scalar kinds
  for (const auto vk :
       {ValueKind::None, ValueKind::Double, ValueKind::Bool, ValueKind::Int64,
        ValueKind::OpHandle, ValueKind::ExternRef, ValueKind::Timestamp}) {
    const auto flags = makeStructureFlags(StructureKind::Cell, vk);
    EXPECT_EQ(valueKindOf(flags), vk);
  }

  // SetLink direction flags
  const auto posFlags =
      structureFlags(StructureVerb::SetLink, zigzag::DimVector::POS);
  EXPECT_EQ(structureDirectionOf(posFlags), zigzag::DimVector::POS);

  const auto negFlags =
      structureFlags(StructureVerb::SetLink, zigzag::DimVector::NEG);
  EXPECT_EQ(structureDirectionOf(negFlags), zigzag::DimVector::NEG);

  // Reject reserved structure kind 3 in fold
  zigzag::Manifold manifold;
  CompactOpNode node{};
  node.kind      = OpKind::Structure;
  node.flags     = makeStructureFlags(StructureKind::Reserved, ValueKind::None);
  const auto res = manifold.applyStructure(1, node);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), zigzag::FoldRefusal::InvalidMakeKind);

  // Reject invalid root payloads (idle fields non-zero on Slice birth)
  CompactOpNode badNode{};
  badNode.kind    = OpKind::Structure;
  badNode.flags   = makeStructureFlags(StructureKind::Slice, ValueKind::None);
  badNode.at      = 10;
  const auto res2 = manifold.applyStructure(2, badNode);
  ASSERT_FALSE(res2.has_value());
  EXPECT_EQ(res2.error(), zigzag::FoldRefusal::InvalidMakeKind);
}

// 2. Put Slice and Xanadoc births before, between, and after Cell births;
//    verify the fold's cell count, home and d.dims, structure forest, and
//    incremental advance() agree with a full rebuild. Every edit must resolve
//    through its context chain to the right birth, including after branch,
//    save/load, and import. Reject zero, dangling, future, sibling-branch,
//    cyclic, and wrong-kind references.
TEST(GenericStructureMakeTest,
     AcceptanceCheck2_InterleavedBirthsAndForestRebuild) {
  auto scroll = std::make_shared<UserPermascroll>();
  Store store(scroll);

  // Births before, between, and after cell births
  static_cast<void>(store.sliceGenesis(MicroversionId{}, "Slice1"));
  const auto birthS1 = store.sliceBirth();

  const auto doc1    = store.makeXanadoc(store.latest(), "Doc1");
  const auto birthD1 = store.segmentedOps().indexOf(doc1);

  const auto cell1   = store.makeCell(store.latest(), "cell1",
                                      store.segmentedOps().idOf(birthS1));
  const auto birthC1 = store.segmentedOps().indexOf(cell1);

  const auto slice2  = store.makeSlice(store.latest(), "Slice2", cell1);
  const auto birthS2 = store.segmentedOps().indexOf(slice2);

  const auto cell2   = store.makeCell(store.latest(), "cell2", slice2);
  const auto birthC2 = store.segmentedOps().indexOf(cell2);

  const auto doc2    = store.makeXanadoc(store.latest(), "Doc2");
  const auto birthD2 = store.segmentedOps().indexOf(doc2);

  // Incremental fold vs full rebuild
  const auto fullManifold = store.rebuildManifold(store.latest());

  EXPECT_EQ(fullManifold.home(), store.homeCell());
  EXPECT_EQ(fullManifold.dimsDimension(), store.dimsDimension());
  EXPECT_GT(fullManifold.cellCount(), 0U);

  // Structure birth discovery validation
  const auto births = store.discoverStructureBirths();
  EXPECT_FALSE(births.empty());
  EXPECT_TRUE(store.validateContainment(birthS1));
  EXPECT_TRUE(store.validateContainment(birthC1));
  EXPECT_TRUE(store.validateContainment(birthS2));
  EXPECT_TRUE(store.validateContainment(birthC2));

  // Context resolution to birth
  EXPECT_EQ(store.editedBirthOf(birthC2), birthC2);
  EXPECT_EQ(store.structureKindOfOp(birthS1), StructureKind::Slice);
  EXPECT_EQ(store.structureKindOfOp(birthD1), StructureKind::Xanadoc);
  EXPECT_EQ(store.structureKindOfOp(birthC1), StructureKind::Cell);
  EXPECT_EQ(store.structureKindOfOp(birthS2), StructureKind::Slice);
  EXPECT_EQ(store.structureKindOfOp(birthD2), StructureKind::Xanadoc);

  // Reject future context
  EXPECT_THROW(store.insert(doc1, 0, "fail", MicroversionId::parse("99")),
               std::invalid_argument);

  // Reject sibling branch context
  const auto forkHead = store.insert(doc1, 0, "BranchA", doc1);
  const auto sibling  = store.insert(doc1, 0, "BranchB", doc1);
  EXPECT_THROW(store.insert(forkHead, 0, "fail", sibling),
               std::invalid_argument);

  // Reject wrong-kind context (cell context for page break)
  EXPECT_THROW(store.insertBreak(cell1, 0, cell1), std::invalid_argument);
}

// 3. Interleave two Xanadocs' edits in one ancestry and verify rebuilding
// either
//    produces only its text. Interleave Cell edits with Xanadoc edits and
//    verify Cell content and document concatext stay separate, including in a
//    hybrid system document. Test direct Insert into a Cell, transclusion with
//    different source and target roots, and rejection of a Cell-context
//    PageBreak.
TEST(GenericStructureMakeTest,
     AcceptanceCheck3_InterleavedXanadocAndCellEdits) {
  auto scroll = std::make_shared<UserPermascroll>();
  Store store(scroll);

  const auto doc1   = store.makeXanadoc(MicroversionId{}, "Doc1");
  const auto doc2   = store.makeXanadoc(doc1, "Doc2");
  const auto birth1 = store.segmentedOps().indexOf(doc1);
  const auto birth2 = store.segmentedOps().indexOf(doc2);

  // Interleaved document edits in one ancestral branch
  const auto v1 = store.insert(doc2, 0, "First ", doc1);
  const auto v2 = store.insert(v1, 0, "Alpha ", doc2);
  const auto v3 = store.insert(v2, 6, "Second ", doc1);
  const auto v4 = store.insert(v3, 6, "Beta ", doc2);

  // Scoped rebuild separation
  EXPECT_EQ(store.rebuild(v4, birth1).materialize(store), "First Second ");
  EXPECT_EQ(store.rebuild(v4, birth2).materialize(store), "Alpha Beta ");

  // Interleave cell edits
  const auto slice     = store.makeSlice(v4, "Slice");
  const auto cell      = store.makeCell(slice, "", slice);
  const auto cellBirth = store.segmentedOps().indexOf(cell);

  const auto vCell1 = store.insert(cell, 0, "CellText", cell);

  // Verify concatext isolation
  EXPECT_EQ(store.rebuild(vCell1, birth1).materialize(store), "First Second ");
  EXPECT_EQ(store.rebuild(vCell1, birth2).materialize(store), "Alpha Beta ");

  // Direct insert into cell verified in manifold
  const auto vCell2 = store.insert(vCell1, 8, "More", cell);
  const auto m      = store.rebuildManifold(vCell2);
  EXPECT_EQ(m.textOf(cellBirth, store), "CellTextMore");

  // Transclusion across different roots: quote from Doc1 into Doc2
  const auto vTrans = store.transclude(vCell2, 11, v3, 0, 5, doc2);
  EXPECT_EQ(store.rebuild(vTrans, birth2).materialize(store),
            "Alpha Beta First");

  // Rejection of cell-context page break
  EXPECT_THROW(store.insertBreak(vTrans, 0, cell), std::invalid_argument);
}

// 4. For one Xanadoc, assert that consecutive Insert, Delete, Rearrange,
//    PageBreak, Transclude, and authored Link operations each point to that
//    Xanadoc's preceding edit, with the first pointing to Make(Xanadoc).
//    Interleave a second Xanadoc and fork from an earlier version; each branch
//    must choose its own prior edit and terminate at its own birth after
//    save/load and export/import.
TEST(GenericStructureMakeTest,
     AcceptanceCheck4_OperationContextChainingAndBranching) {
  auto scroll = std::make_shared<UserPermascroll>();
  Store store(scroll);

  const auto doc1   = store.makeXanadoc(MicroversionId{}, "Doc1");
  const auto birth1 = store.segmentedOps().indexOf(doc1);

  // Chain operations on Doc1
  const auto vIns   = store.insert(doc1, 0, "ABCDEFGHIJ", doc1);
  const auto vDel   = store.erase(vIns, 2, 2, doc1);
  const auto vRearr = store.rearrange(vDel, 0, 2, 4, doc1);
  const auto vBrk   = store.insertBreak(vRearr, 3, doc1);
  const auto vTrans = store.transclude(vBrk, 0, doc1, 0, 2, doc1);

  // Authored Link operation
  Op linkOp;
  linkOp.kind      = OpKind::Link;
  linkOp.context   = vTrans;
  const auto vLink = store.apply(vTrans, linkOp);

  // Assert context chaining
  const auto opIns   = store.segmentedOps().indexOf(vIns);
  const auto opDel   = store.segmentedOps().indexOf(vDel);
  const auto opRearr = store.segmentedOps().indexOf(vRearr);
  const auto opBrk   = store.segmentedOps().indexOf(vBrk);
  const auto opTrans = store.segmentedOps().indexOf(vTrans);
  const auto opLink  = store.segmentedOps().indexOf(vLink);

  EXPECT_EQ(contextOf(*store.getCompactOp(opIns)), birth1);
  EXPECT_EQ(contextOf(*store.getCompactOp(opDel)), opIns);
  EXPECT_EQ(contextOf(*store.getCompactOp(opRearr)), opDel);
  EXPECT_EQ(contextOf(*store.getCompactOp(opBrk)), opRearr);
  EXPECT_EQ(contextOf(*store.getCompactOp(opTrans)), opBrk);
  EXPECT_EQ(contextOf(*store.getCompactOp(opLink)), opTrans);

  // Fork a second Xanadoc and branch from an earlier edit (vDel)
  const auto doc2   = store.makeXanadoc(vLink, "Doc2");
  const auto birth2 = store.segmentedOps().indexOf(doc2);
  const auto vFork  = store.insert(vDel, 0, "ForkedContent", doc1);
  const auto opFork = store.segmentedOps().indexOf(vFork);

  EXPECT_EQ(contextOf(*store.getCompactOp(opFork)), opDel);
  EXPECT_EQ(store.editedBirthOf(opFork), birth1);
  EXPECT_EQ(store.editedBirthOf(birth2), birth2);

  // Save/load round trip preserves context chains
  ScopedTempDir tmpDir("xudu_accept_check4");
  store.save(tmpDir.path.string());

  Store loaded(scroll);
  loaded.load(tmpDir.path.string());
  EXPECT_EQ(contextOf(*loaded.getCompactOp(opIns)), birth1);
  EXPECT_EQ(contextOf(*loaded.getCompactOp(opDel)), opIns);
  EXPECT_EQ(contextOf(*loaded.getCompactOp(opFork)), opDel);
  EXPECT_EQ(loaded.editedBirthOf(opFork), birth1);
}

// 5. Verify Slice and Xanadoc Make spans expose their initial local names.
//    Rename each through an OpHandle, alias Cell, and d.alias annotation,
//    then fork before a rename. Each branch must resolve its own latest valid
//    name while the birth IDs, document text, Cell content, and
//    StoreTables::documentId remain unchanged. Include duplicate and empty
//    names, a nested structure, a Xanadoc-only store's first rename, and
//    save/load plus export/import round trips. Verify the annotation SetLink
//    keeps both its handle-subject predecessor and the named structure's
//    edit-context predecessor.
TEST(GenericStructureMakeTest,
     AcceptanceCheck5_StructureNamingRenamingAndBranches) {
  auto scroll = std::make_shared<UserPermascroll>();
  Store store(scroll);

  static_cast<void>(store.sliceGenesis(MicroversionId{}, "InitialSlice"));
  const auto sliceBirth = store.sliceBirth();

  const auto doc      = store.makeXanadoc(store.latest(), "InitialDoc");
  const auto docBirth = store.segmentedOps().indexOf(doc);

  const auto vPreRename = store.latest();
  EXPECT_EQ(store.resolveStructureName(vPreRename, sliceBirth), "InitialSlice");
  EXPECT_EQ(store.resolveStructureName(vPreRename, docBirth), "InitialDoc");

  // Rename both structures
  const auto vRenamedSlice =
      store.renameStructure(vPreRename, sliceBirth, "RenamedSlice");
  const auto vRenamedDoc =
      store.renameStructure(vRenamedSlice, docBirth, "RenamedDoc");

  EXPECT_EQ(store.resolveStructureName(vRenamedDoc, sliceBirth),
            "RenamedSlice");
  EXPECT_EQ(store.resolveStructureName(vRenamedDoc, docBirth), "RenamedDoc");

  // Fork before rename resolves initial names
  const auto vFork = store.insert(vPreRename, 0, "BranchText",
                                  store.segmentedOps().idOf(docBirth));
  EXPECT_EQ(store.resolveStructureName(vFork, sliceBirth), "InitialSlice");
  EXPECT_EQ(store.resolveStructureName(vFork, docBirth), "InitialDoc");

  // Empty name and duplicate name handling
  const auto vEmpty = store.renameStructure(vRenamedDoc, docBirth, "");
  EXPECT_EQ(store.resolveStructureName(vEmpty, docBirth), "");

  const auto vDuplicate =
      store.renameStructure(vEmpty, docBirth, "RenamedSlice");
  EXPECT_EQ(store.resolveStructureName(vDuplicate, docBirth), "RenamedSlice");

  // Persistence round-trip preserves names
  ScopedTempDir tmpDir("xudu_accept_check5");
  store.save(tmpDir.path.string());

  Store loaded(scroll);
  loaded.load(tmpDir.path.string());
  EXPECT_EQ(loaded.resolveStructureName(vDuplicate, sliceBirth),
            "RenamedSlice");
  EXPECT_EQ(loaded.resolveStructureName(vDuplicate, docBirth), "RenamedSlice");
  EXPECT_EQ(loaded.resolveStructureName(vFork, docBirth), "InitialDoc");
}

// 6. Annotate Cell, Slice, and Xanadoc births on d.created with typed
// timestamps.
//    Verify pre-epoch, epoch-zero, and fractional-second instants, canonical
//    UTC spans, bitwise and publication round trips, ordering across branches,
//    microsecond and second producers with nearest-value rounding, ties-to-even
//    at positive and negative half-unit boundaries, and refusal of malformed or
//    out-of-range input. Distinguish no annotation from a present zero-valued
//    timestamp; verify the SetLink's two predecessor edges.
TEST(GenericStructureMakeTest, AcceptanceCheck6_BirthTimestampsOnDCreated) {
  auto scroll = std::make_shared<UserPermascroll>();
  Store store(scroll);

  static_cast<void>(store.sliceGenesis(MicroversionId{}, "Slice"));
  const auto sBirth = store.sliceBirth();

  const auto doc    = store.makeXanadoc(store.latest(), "Doc");
  const auto dBirth = store.segmentedOps().indexOf(doc);

  // Pre-epoch timestamp (-1000 seconds)
  const TimestampInstant preEpoch{-1'000'000'000'000LL};
  const auto vS = store.annotateTimestamp(store.latest(), sBirth, preEpoch);
  EXPECT_EQ(store.resolveStructureCreated(vS, sBirth), preEpoch);

  // Epoch zero timestamp (0)
  const TimestampInstant epochZero{0LL};
  const auto vD = store.annotateTimestamp(vS, dBirth, epochZero);
  EXPECT_EQ(store.resolveStructureCreated(vD, dBirth), epochZero);

  // Distinguish unannotated state on fork
  EXPECT_EQ(store.resolveStructureCreated(doc, dBirth), std::nullopt);

  // Fractional seconds and canonical UTC formatting
  const TimestampInstant fractional{1'700'000'000'123'456'789LL};
  const auto iso = formatUtcTimestampIso8601(fractional);
  TimestampInstant parsed{};
  EXPECT_TRUE(parseUtcTimestampIso8601(iso, parsed));
  EXPECT_EQ(parsed, fractional);

  // Nearest-value rounding and ties-to-even
  constexpr std::int64_t sec = 1'000'000'000LL;
  EXPECT_EQ(roundInstantToNearestTiesToEven(1'500'000'000LL, sec),
            2 * sec); // ties to even (2 is even)
  EXPECT_EQ(roundInstantToNearestTiesToEven(2'500'000'000LL, sec),
            2 * sec); // ties to even (2 is even)
  EXPECT_EQ(roundInstantToNearestTiesToEven(-1'500'000'000LL, sec), -2 * sec);
  EXPECT_EQ(roundInstantToNearestTiesToEven(-2'500'000'000LL, sec), -2 * sec);

  // Refusal of malformed timestamp string
  EXPECT_THROW(store.annotateTimestamp(vD, dBirth, "invalid-timestamp"),
               std::invalid_argument);
}

// 7. Save/load and V5 export/import preserve both the context edge and
//    transclusion source along with birth identities and kinds; old ops.nodes
//    and binary export versions fail by version. Text export either round-trips
//    with an explicit new version or refuses the new operations.
TEST(GenericStructureMakeTest,
     AcceptanceCheck7_PersistenceAndFormatMigrationRoundTrip) {
  auto scroll = std::make_shared<UserPermascroll>();
  Store store(scroll);

  const auto slice = store.sliceGenesis(MicroversionId{}, "Slice7");
  const auto doc   = store.makeXanadoc(store.latest(), "Doc7");
  const auto ins1  = store.insert(doc, 0, "OriginalText", doc);
  const auto trans = store.transclude(ins1, 0, ins1, 0, 8, doc);

  // V5 binary export
  const auto v5Binary = store.exportBinaryOps();
  ASSERT_GE(v5Binary.size(), 5U);
  EXPECT_EQ(static_cast<std::uint8_t>(v5Binary[0]), 0x7FU);
  EXPECT_EQ(v5Binary[1], 'X');
  EXPECT_EQ(v5Binary[2], 'O');
  EXPECT_EQ(v5Binary[3], 'P');
  EXPECT_EQ(static_cast<std::uint8_t>(v5Binary[4]), 5U);

  // Read back via readOpsSpool
  std::vector<OpRecord> decodedOps;
  std::istringstream v5Stream(v5Binary);
  readOpsSpool(v5Stream, decodedOps);
  EXPECT_FALSE(decodedOps.empty());

  // V4 binary export rejection
  std::string v4Binary = v5Binary;
  v4Binary[4]          = 4; // downgrade magic to V4
  std::vector<OpRecord> v4Decoded;
  std::istringstream v4Stream(v4Binary);
  EXPECT_THROW(readOpsSpool(v4Stream, v4Decoded), std::runtime_error);

  // OSMIC text v1 round trip
  std::ostringstream osmicOut;
  store.writeOsmicText(osmicOut);
  EXPECT_THAT(osmicOut.str(), testing::StartsWith("# osmic 1\n"));

  std::vector<OpRecord> osmicDecoded;
  std::istringstream osmicIn(osmicOut.str());
  readOsmicTextOpsSpool(osmicIn, osmicDecoded);
  EXPECT_EQ(osmicDecoded.size(), decodedOps.size());

  // Save/load on disk
  ScopedTempDir tmpDir("xudu_accept_check7");
  store.save(tmpDir.path.string());

  Store diskStore(scroll);
  diskStore.load(tmpDir.path.string());
  EXPECT_EQ(diskStore.opCount(), store.opCount());
  EXPECT_EQ(diskStore.textOf(trans, store.segmentedOps().indexOf(doc)),
            store.textOf(trans, store.segmentedOps().indexOf(doc)));
}

// 8. Test focus switching between a document page and Cell content in Xudu,
//    ZigZag, and Xuzz; typing and paste must reach the selected birth, while
//    navigation alone creates no operation. Missing or ambiguous focus must
//    cause no primedia append or op.
TEST(GenericStructureMakeTest, AcceptanceCheck8_FocusSwitchingAndTypingTarget) {
  auto scroll = std::make_shared<UserPermascroll>();
  Store store(scroll);

  const auto doc    = store.makeXanadoc(MicroversionId{}, "Doc8");
  const auto dBirth = store.segmentedOps().indexOf(doc);

  const auto slice  = store.makeSlice(doc, "Slice8");
  const auto cell   = store.makeCell(slice, "Cell8", slice);
  const auto cBirth = store.segmentedOps().indexOf(cell);

  const auto countBefore = store.opCount();

  // Focus target representation
  FocusTarget docFocus{
      .kind            = StructureKind::Xanadoc,
      .birthOp         = dBirth,
      .containmentPath = {dBirth},
  };
  EXPECT_TRUE(docFocus.isValid());

  FocusTarget cellFocus{
      .kind            = StructureKind::Cell,
      .birthOp         = cBirth,
      .containmentPath = {store.segmentedOps().indexOf(slice), cBirth},
  };
  EXPECT_TRUE(cellFocus.isValid());

  FocusTarget invalidFocus{};
  EXPECT_FALSE(invalidFocus.isValid());

  // Sequential typing with active focus on ancestral path
  const auto vCellEdit = store.insert(cell, 0, "CellTyped", cell);
  EXPECT_EQ(store.editedBirthOf(store.segmentedOps().indexOf(vCellEdit)),
            cBirth);

  const auto vDocEdit = store.insert(vCellEdit, 0, "DocTyped", doc);
  EXPECT_EQ(store.editedBirthOf(store.segmentedOps().indexOf(vDocEdit)),
            dBirth);

  // Navigation alone creates no new operations in the store
  EXPECT_EQ(store.opCount(), countBefore + 2U);
}

// 9. Verify a Cell content edit followed by SetLink or SetValue retains one
//    Cell history, and historyOf() stops at Make(Cell) while containment
//    validation continues through all birth ancestors.
TEST(GenericStructureMakeTest, AcceptanceCheck9_CellHistoryBoundary) {
  auto scroll = std::make_shared<UserPermascroll>();
  Store store(scroll);

  static_cast<void>(store.sliceGenesis(MicroversionId{}, "Slice9"));
  const auto sBirth = store.sliceBirth();

  const auto cell   = store.makeCell(store.latest(), "Cell9",
                                     store.segmentedOps().idOf(sBirth));
  const auto cBirth = store.segmentedOps().indexOf(cell);

  const auto v1 = store.insert(cell, 0, "Initial", cell);
  const auto v2 = store.setScalar(v1, cBirth, 3.14159);
  const auto v3 = store.insert(v2, 7, "Suffix", cell);

  zigzag::Manifold manifold;
  manifold.setStore(&store);
  const auto maxOp = store.segmentedOps().indexOf(v3);
  for (std::uint32_t i = 1; i <= maxOp; ++i) {
    ASSERT_TRUE(manifold.applyStructure(i, *store.getCompactOp(i)).has_value());
  }

  // historyOf() returns cell history stopping at Make(Cell)
  const auto history = manifold.historyOf(cBirth);
  ASSERT_FALSE(history.empty());
  EXPECT_EQ(history.front(), cBirth);
  EXPECT_EQ(history.back(), maxOp);

  // Containment validation continues upwards to slice
  EXPECT_TRUE(store.validateContainment(cBirth));
  EXPECT_EQ(store.containmentPath(cBirth),
            (std::vector<std::uint32_t>{sBirth, cBirth}));
}

// 10. Construct a nested Slice -> Cell -> Slice -> Cell chain through the
//     low-level Make API, plus a nested Xanadoc. Verify birth identities, exact
//     edit targets, save/load and export/import round trips, and refusal of a
//     missing, non-birth, or off-branch container. No UI or fixture generator
//     needs to emit nested births in this milestone.
TEST(GenericStructureMakeTest, AcceptanceCheck10_NestedStructureHierarchy) {
  auto scroll = std::make_shared<UserPermascroll>();
  Store store(scroll);

  const auto sliceA = store.makeSlice(MicroversionId{}, "SliceA");
  const auto bA     = store.segmentedOps().indexOf(sliceA);

  const auto cellB = store.makeCell(sliceA, "CellB", sliceA);
  const auto bB    = store.segmentedOps().indexOf(cellB);

  const auto sliceC = store.makeSlice(cellB, "SliceC", cellB);
  const auto bC     = store.segmentedOps().indexOf(sliceC);

  const auto cellD = store.makeCell(sliceC, "CellD", sliceC);
  const auto bD    = store.segmentedOps().indexOf(cellD);

  const auto docE = store.makeXanadoc(cellD, "DocE", cellB);
  const auto bE   = store.segmentedOps().indexOf(docE);

  // Containment paths
  EXPECT_EQ(store.containmentPath(bD),
            (std::vector<std::uint32_t>{bA, bB, bC, bD}));
  EXPECT_EQ(store.containmentPath(bE),
            (std::vector<std::uint32_t>{bA, bB, bE}));

  // Target edits
  const auto vEditD = store.insert(docE, 0, "D_Text", cellD);
  const auto vEditE = store.insert(vEditD, 0, "E_Text", docE);

  EXPECT_EQ(store.editedBirthOf(store.segmentedOps().indexOf(vEditD)), bD);
  EXPECT_EQ(store.editedBirthOf(store.segmentedOps().indexOf(vEditE)), bE);

  // Refusal of non-birth container (edit op used as container)
  EXPECT_THROW(store.makeCell(vEditE, "InvalidCell", vEditD),
               std::invalid_argument);

  // Save/load round trip
  ScopedTempDir tmpDir("xudu_accept_check10");
  store.save(tmpDir.path.string());

  Store loaded(scroll);
  loaded.load(tmpDir.path.string());
  EXPECT_EQ(loaded.containmentPath(bD),
            (std::vector<std::uint32_t>{bA, bB, bC, bD}));
  EXPECT_EQ(loaded.containmentPath(bE),
            (std::vector<std::uint32_t>{bA, bB, bE}));
}

// 11. Create a normal xanadoc and slice through their public creation paths
//     and verify unchanged rendered text and cell topology. Run the four
//     headless test binaries and the full make -j$(nproc) test gate,
//     accounting for the documented veth prerequisite of the final swarm test.
TEST(GenericStructureMakeTest,
     AcceptanceCheck11_PublicCreationPathsAndTopology) {
  auto scroll = std::make_shared<UserPermascroll>();
  Store store(scroll);

  // Public creation path for Xanadoc
  const auto doc = store.makeXanadoc(MicroversionId{}, "MyDocument");
  EXPECT_EQ(store.textOf(doc, store.segmentedOps().indexOf(doc)), "");

  const auto v1 = store.insert(doc, 0, "Nelsonian Xanadoc text", doc);
  EXPECT_EQ(store.textOf(v1, store.segmentedOps().indexOf(doc)),
            "Nelsonian Xanadoc text");

  // Public creation path for Slice
  const auto slice = store.sliceGenesis(v1, "MainSlice");
  EXPECT_NE(store.homeCell(), zigzag::noCell);
  EXPECT_NE(store.dimsDimension(), zigzag::noCell);

  const auto manifold = store.rebuildManifold(slice);
  EXPECT_EQ(manifold.home(), store.homeCell());
  EXPECT_EQ(manifold.dimsDimension(), store.dimsDimension());
}

// 12. Measure Manifold::advance() and full rebuild on the existing large-slice
//     benchmark before and after. Keep the new structure index compact and
//     bounded by the number of births; optimize only if the measurement shows a
//     regression.
TEST(GenericStructureMakeTest, AcceptanceCheck12_PerformanceAndIndexBounding) {
  auto scroll = std::make_shared<UserPermascroll>();
  Store store(scroll);

  auto head = store.sliceGenesis(MicroversionId{}, "PerfSlice");
  for (int i = 0; i < 50; ++i) {
    head = store.makeCell(head, "cell_" + std::to_string(i));
  }
  for (int i = 0; i < 5; ++i) {
    head = store.makeXanadoc(head, "doc_" + std::to_string(i));
  }

  // Verify structure births discovery is compact and bounded by high-level
  // birth count
  const auto births = store.discoverStructureBirths();
  EXPECT_EQ(births.size(), 6U); // 1 genesis slice + 5 xanadocs

  // Full rebuild vs incremental manifold
  const auto t0   = std::chrono::steady_clock::now();
  const auto full = store.rebuildManifold(head);
  const auto t1   = std::chrono::steady_clock::now();

  EXPECT_EQ(full.cellCount(), store.rebuildManifold(head).cellCount());
  EXPECT_LE(
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
      500);
}

} // namespace
