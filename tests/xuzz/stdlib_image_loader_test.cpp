#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/stdlib_image.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_set>

#include <gtest/gtest.h>

namespace {

using namespace zigzag::vortex;

class StdlibImageLoaderTest : public ::testing::Test {
protected:
  void SetUp() override {
    resetStdlibPolicyForTesting();
    setStdlibSource(StdlibMode::Cpp);
    setStdlibLocalImage(false);
    dir_ = std::filesystem::temp_directory_path() /
           ("stdlib_image_loader_" +
            std::to_string(nextId_.fetch_add(1, std::memory_order_relaxed)));
    std::filesystem::create_directories(dir_);
    ASSERT_NO_FATAL_FAILURE(generateImage());
  }

  void TearDown() override {
    resetStdlibPolicyForTesting();
    std::error_code ignored;
    std::filesystem::remove_all(dir_, ignored);
  }

  void generateImage() {
    permascroll_.reset();
    std::error_code ignored;
    std::filesystem::remove_all(dir_ / "permascroll", ignored);
    std::filesystem::remove_all(dir_ / "store", ignored);
    const auto permaDir = dir_ / "permascroll";
    xanadu::UserPermascroll::Config config;
    config.storageDir = permaDir;
    permascroll_ = std::make_shared<xanadu::UserPermascroll>(std::move(config));
    xanadu::Store store{permascroll_};
    zigzag::ArenaManifold arena;
    VortexCore core{arena};
    VortexVM vm{core};
    VortexStdLib stdlib{core, vm};
    const auto exported =
        stdlib.exportImageToStore(store, kStdlibContentVersion);
    ASSERT_TRUE(exported) << (exported ? "" : exported.error());
    store.save((dir_ / "store").string());
    ASSERT_TRUE(permascroll_->flush());
  }

  struct ManifestCells {
    zigzag::CellRef expectedBytes{};
    zigzag::CellRef generatorHead{};
    zigzag::CellRef imageEpoch{};
    zigzag::CellRef opcodeCount{};
  };

  static zigzag::CellRef namedMember(const zigzag::Manifold &manifold,
                                     const xanadu::SpanReader &reader,
                                     const zigzag::CellRef anchor,
                                     const zigzag::DimRef dim,
                                     const std::string_view name) {
    std::unordered_set<zigzag::CellRef> visited{anchor};
    auto current = anchor;
    while (const auto next =
               manifold.linked(current, dim, zigzag::DimVector::POS)) {
      if (!visited.insert(*next).second)
        throw std::runtime_error("cycle while locating test image record");
      if (manifold.textOf(*next, reader) == name) return *next;
      current = *next;
    }
    throw std::runtime_error("test image record not found: " +
                             std::string{name});
  }

  static std::string bytesAt(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file},
            std::istreambuf_iterator<char>{}};
  }

  ManifestCells manifestCells(xanadu::Store &store) {
    const auto physical =
        store.segmentedOps().idOf(static_cast<std::uint32_t>(store.opCount()));
    if (!physical) throw std::runtime_error("test image has no physical head");
    const auto manifold = store.rebuildManifold(*physical);
    const auto home     = *manifold.home();
    const auto vars     = *manifold.dimensionNamed("d.vars", store);
    const auto values   = *manifold.dimensionNamed("d.values", store);
    const auto manifestKey =
        namedMember(manifold, store, home, vars, "stdlib.image");
    const auto holder =
        *manifold.linked(manifestKey, values, zigzag::DimVector::POS);
    return ManifestCells{
        .expectedBytes = *manifold.linked(
            namedMember(manifold, store, holder, vars, "expectedScrollBytes"),
            values, zigzag::DimVector::POS),
        .generatorHead = *manifold.linked(
            namedMember(manifold, store, holder, vars, "generatorHead"), values,
            zigzag::DimVector::POS),
        .imageEpoch = *manifold.linked(
            namedMember(manifold, store, holder, vars, "imageEpoch"), values,
            zigzag::DimVector::POS),
        .opcodeCount = *manifold.linked(
            namedMember(manifold, store, holder, vars, "imageOpcodeCount"),
            values, zigzag::DimVector::POS)};
  }

  zigzag::CellRef firstOpsMember(xanadu::Store &store) {
    const auto physical =
        store.segmentedOps().idOf(static_cast<std::uint32_t>(store.opCount()));
    const auto manifold = store.rebuildManifold(*physical);
    const auto home     = *manifold.home();
    const auto vars     = *manifold.dimensionNamed("d.vars", store);
    const auto values   = *manifold.dimensionNamed("d.values", store);
    const auto step     = *manifold.dimensionNamed("d.step", store);
    const auto holder   = *manifold.linked(
        namedMember(manifold, store, home, vars, "stdlib.image"), values,
        zigzag::DimVector::POS);
    const auto ops     = namedMember(manifold, store, holder, vars, "ops");
    const auto opsHead = *manifold.linked(ops, values, zigzag::DimVector::POS);
    return *manifold.linked(opsHead, step, zigzag::DimVector::POS);
  }

  zigzag::CellRef firstMathInputParameter(xanadu::Store &store) {
    const auto physical =
        store.segmentedOps().idOf(static_cast<std::uint32_t>(store.opCount()));
    const auto manifold = store.rebuildManifold(*physical);
    const auto home     = *manifold.home();
    const auto vars     = *manifold.dimensionNamed("d.vars", store);
    const auto values   = *manifold.dimensionNamed("d.values", store);
    const auto params   = *manifold.dimensionNamed("d.params", store);
    const auto step     = *manifold.dimensionNamed("d.step", store);
    const auto modules  = *manifold.dimensionNamed("d.stdlib", store);
    const auto math  = namedMember(manifold, store, home, modules, "std:math");
    const auto clamp = namedMember(manifold, store, math, vars, "clamp");
    const auto signature =
        *manifold.linked(clamp, params, zigzag::DimVector::POS);
    const auto inRecord = namedMember(manifold, store, signature, vars, "in");
    const auto inHead =
        *manifold.linked(inRecord, values, zigzag::DimVector::POS);
    return *manifold.linked(inHead, step, zigzag::DimVector::POS);
  }

  xanadu::MicroversionId
  finishImageEdit(xanadu::Store &store, const ManifestCells &fields,
                  const xanadu::MicroversionId &editHead) {
    const auto finalHead    = editHead.next().next();
    const auto expectedSize = permascroll_->size() + finalHead.str().size();
    const auto afterScroll =
        store.setValue(editHead, fields.expectedBytes, xanadu::PrimediaSpan{},
                       xanadu::ValueKind::Int64, expectedSize);
    EXPECT_EQ(afterScroll.next(), finalHead);
    const auto headSpan =
        xanadu::requireSpoolWrite(permascroll_->append(finalHead.str()));
    return store.setValue(afterScroll, fields.generatorHead, headSpan,
                          xanadu::ValueKind::None, 0);
  }

  std::filesystem::path dir_;
  std::shared_ptr<xanadu::UserPermascroll> permascroll_;
  inline static std::atomic<unsigned> nextId_{0};
};

TEST_F(StdlibImageLoaderTest, OpensGeneratedImageReadOnly) {
  const auto result = openStdlibImage(dir_);
  ASSERT_TRUE(result) << (result ? "" : result.error().what());
  EXPECT_EQ((*result)->manifest.contentVersion, kStdlibContentVersion);
  const auto physicalHead = (*result)->store->segmentedOps().idOf(
      static_cast<std::uint32_t>((*result)->store->opCount()));
  ASSERT_TRUE(physicalHead);
  EXPECT_EQ((*result)->manifest.generatorHead, *physicalHead);
  EXPECT_FALSE((*result)->opcodeCells.empty());
  EXPECT_FALSE((*result)->routines.empty());
  EXPECT_TRUE((*result)->store->isReadOnly());
  EXPECT_TRUE((*result)->permascroll->config().readOnly);
}

TEST_F(StdlibImageLoaderTest, MissingPermascrollIsNamed) {
  const auto saved = dir_ / "permascroll.saved";
  std::filesystem::rename(dir_ / "permascroll", saved);
  const auto result = openStdlibImage(dir_);
  std::filesystem::rename(saved, dir_ / "permascroll");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().cause, StdlibRefusalCause::ImageMissing);
  EXPECT_NE(std::string{result.error().what()}.find("permascroll"),
            std::string::npos);
}

TEST_F(StdlibImageLoaderTest, ShortAndGrownScrollAreRefused) {
  const auto scroll       = dir_ / "permascroll" / "active.primedia";
  const auto originalSize = std::filesystem::file_size(scroll);
  ASSERT_GT(originalSize, 1U);
  const auto originalBytes = bytesAt(scroll);

  std::filesystem::resize_file(scroll, originalSize - 1U);
  auto shortResult = openStdlibImage(dir_);
  ASSERT_FALSE(shortResult);
  EXPECT_EQ(shortResult.error().cause, StdlibRefusalCause::ScrollTruncated);

  {
    // resize_file cannot un-truncate: restore the bytes, then grow by one.
    std::ofstream out{scroll, std::ios::binary | std::ios::trunc};
    out.write(originalBytes.data(),
              static_cast<std::streamsize>(originalBytes.size()));
    out.put('X');
    ASSERT_TRUE(out.good());
  }
  auto grownResult = openStdlibImage(dir_);
  ASSERT_FALSE(grownResult);
  EXPECT_EQ(grownResult.error().cause, StdlibRefusalCause::ScrollTruncated);
}

TEST_F(StdlibImageLoaderTest, ForeignOpsHeaderIsAnUnreadableRefusal) {
  const auto ops = dir_ / "store" / "ops.nodes";
  {
    std::fstream file{ops, std::ios::binary | std::ios::in | std::ios::out};
    ASSERT_TRUE(file);
    file.put('!');
  }
  const auto result = openStdlibImage(dir_);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().cause, StdlibRefusalCause::ImageUnreadable);
  EXPECT_NE(std::string{result.error().what()}.find("OpsSegmentUnreadable"),
            std::string::npos);
}

TEST_F(StdlibImageLoaderTest, EpochAndCountSkewAreRefused) {
  {
    xanadu::Store store{permascroll_};
    store.load((dir_ / "store").string());
    const auto fields = manifestCells(store);
    const auto parent =
        *store.segmentedOps().idOf(static_cast<std::uint32_t>(store.opCount()));
    const auto edited =
        store.setCellText(parent, fields.imageEpoch, "foreign-stdlib-epoch");
    (void)finishImageEdit(store, fields, edited);
    store.save((dir_ / "store").string());
  }
  {
    auto epochResult = openStdlibImage(dir_);
    ASSERT_FALSE(epochResult);
    EXPECT_EQ(epochResult.error().cause, StdlibRefusalCause::EpochMismatch);
    EXPECT_TRUE(epochResult.error().epochs);
  }

  ASSERT_NO_FATAL_FAILURE(generateImage());
  {
    xanadu::Store store{permascroll_};
    store.load((dir_ / "store").string());
    const auto fields = manifestCells(store);
    const auto parent =
        *store.segmentedOps().idOf(static_cast<std::uint32_t>(store.opCount()));
    const auto edited = store.setValue(
        parent, fields.opcodeCount, xanadu::PrimediaSpan{},
        xanadu::ValueKind::Int64,
        static_cast<std::uint64_t>(stdlibImageOpcodeCount() + 1U));
    (void)finishImageEdit(store, fields, edited);
    store.save((dir_ / "store").string());
  }
  const auto countResult = openStdlibImage(dir_);
  ASSERT_FALSE(countResult);
  EXPECT_EQ(countResult.error().cause, StdlibRefusalCause::FormatRefused);
  EXPECT_NE(std::string{countResult.error().detail}.find("count"),
            std::string::npos);
}

TEST_F(StdlibImageLoaderTest, MalformedSignatureAndOpcodeReferencesAreRefused) {
  {
    xanadu::Store store{permascroll_};
    store.load((dir_ / "store").string());
    const auto fields       = manifestCells(store);
    const auto badParameter = firstMathInputParameter(store);
    const auto parent =
        *store.segmentedOps().idOf(static_cast<std::uint32_t>(store.opCount()));
    const auto edited =
        store.setValue(parent, badParameter, xanadu::PrimediaSpan{},
                       xanadu::ValueKind::Int64, 999999);
    (void)finishImageEdit(store, fields, edited);
    store.save((dir_ / "store").string());
  }
  {
    auto signatureResult = openStdlibImage(dir_);
    ASSERT_FALSE(signatureResult);
    EXPECT_EQ(signatureResult.error().cause, StdlibRefusalCause::FormatRefused);
  }

  ASSERT_NO_FATAL_FAILURE(generateImage());
  {
    xanadu::Store store{permascroll_};
    store.load((dir_ / "store").string());
    const auto fields    = manifestCells(store);
    const auto badOpcode = firstOpsMember(store);
    const auto parent =
        *store.segmentedOps().idOf(static_cast<std::uint32_t>(store.opCount()));
    const auto edited =
        store.setValue(parent, badOpcode, xanadu::PrimediaSpan{},
                       xanadu::ValueKind::Int64, 999999);
    (void)finishImageEdit(store, fields, edited);
    store.save((dir_ / "store").string());
  }
  const auto opcodeResult = openStdlibImage(dir_);
  ASSERT_FALSE(opcodeResult);
  EXPECT_EQ(opcodeResult.error().cause, StdlibRefusalCause::FormatRefused);
}

TEST_F(StdlibImageLoaderTest, ReadOnlyOpenDoesNotChangeArtifactFiles) {
  const auto ops         = dir_ / "store" / "ops.nodes";
  const auto tables      = dir_ / "store" / "store.tables";
  const auto scroll      = dir_ / "permascroll" / "active.primedia";
  const auto opsSize     = std::filesystem::file_size(ops);
  const auto tablesSize  = std::filesystem::file_size(tables);
  const auto scrollSize  = std::filesystem::file_size(scroll);
  const auto opsBytes    = bytesAt(ops);
  const auto tablesBytes = bytesAt(tables);
  const auto scrollBytes = bytesAt(scroll);
  ASSERT_TRUE(openStdlibImage(dir_));
  EXPECT_EQ(std::filesystem::file_size(ops), opsSize);
  EXPECT_EQ(std::filesystem::file_size(tables), tablesSize);
  EXPECT_EQ(std::filesystem::file_size(scroll), scrollSize);
  EXPECT_EQ(bytesAt(ops), opsBytes);
  EXPECT_EQ(bytesAt(tables), tablesBytes);
  EXPECT_EQ(bytesAt(scroll), scrollBytes);
}

TEST_F(StdlibImageLoaderTest, GraftImageBuildsResolvableStdlib) {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  VortexStdLib stdlib{core, vm, VortexStdLib::Bootstrap::Manual};

  const auto image = openStdlibImage(dir_);
  ASSERT_TRUE(image) << (image ? "" : image.error().what());
  const auto grafted = stdlib.graftImage(**image);
  ASSERT_TRUE(grafted) << (grafted ? "" : grafted.error());

  EXPECT_FALSE(stdlib.modules().empty());
  EXPECT_TRUE(stdlib.has("std:math"));
  EXPECT_TRUE(stdlib.has("std:math/clamp"));
}

TEST_F(StdlibImageLoaderTest, EnsureStdlibGraftsUnderImageMode) {
  resetStdlibPolicyForTesting();
  setStdlibSource(StdlibMode::Image);
  setStdlibImageDir(dir_);
  setStdlibLocalImage(false);

  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  VortexStdLib stdlib{core, vm};

  EXPECT_TRUE(stdlib.has("std:math/clamp"));
  EXPECT_FALSE(stdlib.modules().empty());
}

TEST_F(StdlibImageLoaderTest, ImageThenCppFallsBackWhenImageMissing) {
  resetStdlibPolicyForTesting();
  setStdlibSource(StdlibMode::ImageThenCpp);
  setStdlibImageDir(dir_ / "nonexistent");
  setStdlibLocalImage(false);

  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  VortexStdLib stdlib{core, vm};

  EXPECT_TRUE(stdlib.has("std:math/clamp"));
}

TEST_F(StdlibImageLoaderTest, SecondInstanceRebindsWithoutRegrafting) {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  VortexStdLib first{core, vm, VortexStdLib::Bootstrap::Manual};

  const auto image = openStdlibImage(dir_);
  ASSERT_TRUE(image);
  ASSERT_TRUE(first.graftImage(**image));
  const auto moduleCount = first.modules().size();
  ASSERT_GT(moduleCount, 0u);

  resetStdlibPolicyForTesting();
  setStdlibSource(StdlibMode::Image);
  setStdlibImageDir(dir_);
  setStdlibLocalImage(false);
  VortexStdLib second{core, vm};

  EXPECT_EQ(second.modules().size(), moduleCount);
  EXPECT_TRUE(second.has("std:math/clamp"));
}

TEST_F(StdlibImageLoaderTest, LiveMarkRefusesGraftAndLeavesArenaUntouched) {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  VortexStdLib stdlib{core, vm, VortexStdLib::Bootstrap::Manual};

  const auto image = openStdlibImage(dir_);
  ASSERT_TRUE(image);
  const auto cellsBefore = arena.cells().size();

  const auto mark = arena.mark();
  const auto refused  = stdlib.graftImage(**image);
  EXPECT_FALSE(refused);
  EXPECT_NE(refused.error().find("live marks"), std::string::npos);
  arena.release(mark);
  EXPECT_EQ(arena.cells().size(), cellsBefore);

  const auto grafted = stdlib.graftImage(**image);
  ASSERT_TRUE(grafted) << (grafted ? "" : grafted.error());
}

TEST_F(StdlibImageLoaderTest, RegraftOnSameInstanceIsRefused) {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  VortexStdLib stdlib{core, vm, VortexStdLib::Bootstrap::Manual};

  const auto image = openStdlibImage(dir_);
  ASSERT_TRUE(image);
  ASSERT_TRUE(stdlib.graftImage(**image));

  const auto again = stdlib.graftImage(**image);
  EXPECT_FALSE(again);
}

TEST(StdlibImageNonImageTest, OrdinaryStoreWithoutManifestIsNotAnImage) {
  static std::atomic<unsigned> nonImageId{0};
  const auto dir =
      std::filesystem::temp_directory_path() /
      ("stdlib_nonimage_" +
       std::to_string(nonImageId.fetch_add(1, std::memory_order_relaxed)));
  std::filesystem::create_directories(dir / "permascroll");
  xanadu::UserPermascroll::Config config;
  config.storageDir = dir / "permascroll";
  auto permascroll =
      std::make_shared<xanadu::UserPermascroll>(std::move(config));
  xanadu::Store store{permascroll};
  const auto head = store.sliceGenesis(xanadu::MicroversionId{});
  (void)head;
  store.save((dir / "store").string());

  const auto result = openStdlibImage(dir);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().cause, StdlibRefusalCause::FormatRefused);
  std::error_code ignored;
  std::filesystem::remove_all(dir, ignored);
}

TEST_F(StdlibImageLoaderTest, RepeatedOpensShareOneCachedImage) {
  const auto first = openStdlibImage(dir_);
  ASSERT_TRUE(first);
  const auto second = openStdlibImage(dir_);
  ASSERT_TRUE(second);
  EXPECT_EQ(first->get(), second->get());

  // Regeneration rewrites all three artifacts; the stamp change must force a
  // reload rather than serve the cached image.
  ASSERT_NO_FATAL_FAILURE(generateImage());
  const auto third = openStdlibImage(dir_);
  ASSERT_TRUE(third);
  EXPECT_NE(first->get(), third->get());
}

TEST_F(StdlibImageLoaderTest, GraftedCallsMatchBootstrapCalls) {
  // SetUp leaves policy on Cpp, so this stdlib bootstraps from C++.
  zigzag::ArenaManifold cppArena;
  VortexCore cppCore{cppArena};
  VortexVM cppVm{cppCore};
  VortexStdLib cppStd{cppCore, cppVm};

  resetStdlibPolicyForTesting();
  setStdlibSource(StdlibMode::Image);
  setStdlibImageDir(dir_);
  setStdlibLocalImage(false);
  zigzag::ArenaManifold imageArena;
  VortexCore imageCore{imageArena};
  VortexVM imageVm{imageCore};
  VortexStdLib imageStd{imageCore, imageVm};

  const auto expectSameCall = [&](const std::string_view path,
                                  const std::vector<CellValue> &args) {
    EXPECT_EQ(cppStd.call(path, args), imageStd.call(path, args)) << path;
  };
  expectSameCall("std:math/abs", {CellValue(std::int64_t{-99})});
  expectSameCall("std:math/min",
                 {CellValue(std::int64_t{15}), CellValue(std::int64_t{42})});
  expectSameCall("std:math/max",
                 {CellValue(std::int64_t{15}), CellValue(std::int64_t{42})});
  expectSameCall("std:math/clamp",
                 {CellValue(std::int64_t{150}), CellValue(std::int64_t{0}),
                  CellValue(std::int64_t{100})});
  expectSameCall("std:math/add",
                 {CellValue(std::int64_t{25}), CellValue(std::int64_t{17})});
  expectSameCall("std:math/sub",
                 {CellValue(std::int64_t{50}), CellValue(std::int64_t{8})});
  expectSameCall("std:math/div",
                 {CellValue(std::int64_t{42}), CellValue(std::int64_t{0})});
  expectSameCall("std:math/mod",
                 {CellValue(std::int64_t{47}), CellValue(std::int64_t{5})});
  expectSameCall("std:math/neg", {CellValue(std::int64_t{42})});
  expectSameCall("std:string/to_upper",
                 {CellValue(std::string{"hello vortex"})});
  expectSameCall("std:string/to_lower",
                 {CellValue(std::string{"XANADU ZZSTRUCTURE"})});
  expectSameCall("std:string/trim",
                 {CellValue(std::string{"   padded text   "})});

  // Contract templates are exported without routine bindings: bootstrap
  // delivers call() arguments through the operand wings, and a grafted image
  // must reproduce that side effect on the shared operand cell.
  const auto cppOp = cppStd.resolve("std:contract/require_positive");
  const auto imgOp = imageStd.resolve("std:contract/require_positive");
  ASSERT_TRUE(cppOp.has_value());
  ASSERT_TRUE(imgOp.has_value());
  const std::vector<CellValue> five{CellValue(std::int64_t{5})};
  (void)cppStd.call(*cppOp, five);
  (void)imageStd.call(*imgOp, five);
  const auto cppIn = cppCore.inputsOf(*cppOp);
  const auto imgIn = imageCore.inputsOf(*imgOp);
  ASSERT_FALSE(cppIn.empty());
  ASSERT_FALSE(imgIn.empty());
  EXPECT_EQ(cppCore.render(cppIn.front()), imageCore.render(imgIn.front()));
  const auto imgOperand = imageCore.render(imgIn.front());
  ASSERT_TRUE(imgOperand.has_value());
  EXPECT_EQ(*imgOperand, CellValue(std::int64_t{5}));
}

} // namespace
