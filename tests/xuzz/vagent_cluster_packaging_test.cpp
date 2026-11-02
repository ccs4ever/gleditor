/**
 * @file vagent_cluster_packaging_test.cpp
 * @brief Unit and integration tests for Track 1: Cluster Packaging Subsystem
 * (P21.1–P21.8).
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <elf.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "common/xanadu/merkle_ledger.hpp"
#include "common/xanadu/vagent/cluster/package_cas.hpp"
#include "common/xanadu/vagent/cluster/package_inspector.hpp"
#include "common/xanadu/vagent/cluster/pending_fetch.hpp"
#include "common/xanadu/vortex/mesh_actor.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

namespace fs = std::filesystem;
using namespace xanadu::vagent::cluster;
using namespace zigzag::vortex::mesh;
using xanadu::identity::MessageType;
using zigzag::vortex::WorkerContext;

fs::path createTempDir(std::string_view prefix) {
  const auto p =
      fs::temp_directory_path() /
      (std::string(prefix) + "_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(p);
  return p;
}

xanadu::identity::Fingerprint makeFingerprint(char c) {
  const std::string s(40, c);
  return *xanadu::identity::Fingerprint::fromString(s);
}

std::vector<std::uint8_t> createMockElf(
    std::string_view soname, const std::vector<std::string> &needed = {},
    const std::vector<std::string> &runpaths = {},
    const std::vector<std::string> &rpaths = {}, bool withInterp = false) {

  // Build dynamic string table
  std::vector<char> strtab;
  strtab.push_back('\0'); // offset 0 is empty string

  const auto addString = [&](std::string_view s) -> std::size_t {
    if (s.empty()) return 0;
    const std::size_t off = strtab.size();
    strtab.insert(strtab.end(), s.begin(), s.end());
    strtab.push_back('\0');
    return off;
  };

  const std::size_t sonameOff = addString(soname);
  std::vector<std::size_t> neededOffs;
  for (const auto &n : needed) {
    neededOffs.push_back(addString(n));
  }
  std::vector<std::size_t> runpathOffs;
  for (const auto &rp : runpaths) {
    runpathOffs.push_back(addString(rp));
  }
  std::vector<std::size_t> rpathOffs;
  for (const auto &rp : rpaths) {
    rpathOffs.push_back(addString(rp));
  }

  // Build dynamic table entries
  std::vector<Elf64_Dyn> dyns;
  if (!soname.empty()) {
    Elf64_Dyn d{};
    d.d_tag      = DT_SONAME;
    d.d_un.d_val = sonameOff;
    dyns.push_back(d);
  }
  for (const auto off : neededOffs) {
    Elf64_Dyn d{};
    d.d_tag      = DT_NEEDED;
    d.d_un.d_val = off;
    dyns.push_back(d);
  }
  for (const auto off : runpathOffs) {
    Elf64_Dyn d{};
    d.d_tag      = DT_RUNPATH;
    d.d_un.d_val = off;
    dyns.push_back(d);
  }
  for (const auto off : rpathOffs) {
    Elf64_Dyn d{};
    d.d_tag      = DT_RPATH;
    d.d_un.d_val = off;
    dyns.push_back(d);
  }

  // DT_STRSZ
  {
    Elf64_Dyn d{};
    d.d_tag      = DT_STRSZ;
    d.d_un.d_val = strtab.size();
    dyns.push_back(d);
  }
  // DT_STRTAB (will be updated with offset)
  const std::size_t strtabDynIdx = dyns.size();
  {
    Elf64_Dyn d{};
    d.d_tag      = DT_STRTAB;
    d.d_un.d_ptr = 0;
    dyns.push_back(d);
  }
  // DT_NULL
  {
    Elf64_Dyn d{};
    d.d_tag      = DT_NULL;
    d.d_un.d_val = 0;
    dyns.push_back(d);
  }

  // Calculate layout offsets
  const std::size_t ehdrSize = sizeof(Elf64_Ehdr);
  const std::size_t numPhdrs =
      withInterp ? 3 : 2; // PT_LOAD, PT_DYNAMIC, [PT_INTERP]
  const std::size_t phdrSize   = sizeof(Elf64_Phdr) * numPhdrs;
  const std::size_t phdrOffset = ehdrSize;

  const std::size_t interpOffset = phdrOffset + phdrSize;
  const std::string interpStr    = "/lib64/ld-linux-x86-64.so.2";
  const std::size_t interpSize   = withInterp ? (interpStr.size() + 1) : 0;

  const std::size_t dynOffset = interpOffset + interpSize;
  const std::size_t dynSize   = dyns.size() * sizeof(Elf64_Dyn);

  const std::size_t strtabOffset = dynOffset + dynSize;
  const std::size_t strtabSize   = strtab.size();

  // Update DT_STRTAB pointer
  dyns[strtabDynIdx].d_un.d_ptr = strtabOffset;

  // Build section header string table
  std::vector<char> shstrtab;
  shstrtab.push_back('\0');
  const auto addShStr = [&](std::string_view s) -> std::size_t {
    const std::size_t off = shstrtab.size();
    shstrtab.insert(shstrtab.end(), s.begin(), s.end());
    shstrtab.push_back('\0');
    return off;
  };
  const std::size_t dynNameOff    = addShStr(".dynamic");
  const std::size_t dynstrNameOff = addShStr(".dynstr");
  const std::size_t shstrNameOff  = addShStr(".shstrtab");

  const std::size_t shstrOffset = strtabOffset + strtabSize;
  const std::size_t shstrSize   = shstrtab.size();

  const std::size_t shdrOffset = shstrOffset + shstrSize;
  const std::size_t numShdrs   = 4; // Null, .dynamic, .dynstr, .shstrtab
  const std::size_t totalSize  = shdrOffset + (numShdrs * sizeof(Elf64_Shdr));

  std::vector<std::uint8_t> binary(totalSize, 0);

  // 1. ELF Header
  auto *ehdr                = reinterpret_cast<Elf64_Ehdr *>(binary.data());
  ehdr->e_ident[EI_MAG0]    = 0x7f;
  ehdr->e_ident[EI_MAG1]    = 'E';
  ehdr->e_ident[EI_MAG2]    = 'L';
  ehdr->e_ident[EI_MAG3]    = 'F';
  ehdr->e_ident[EI_CLASS]   = ELFCLASS64;
  ehdr->e_ident[EI_DATA]    = ELFDATA2LSB;
  ehdr->e_ident[EI_VERSION] = EV_CURRENT;
  ehdr->e_ident[EI_OSABI]   = ELFOSABI_NONE;
  ehdr->e_type              = ET_DYN;
  ehdr->e_machine           = EM_X86_64;
  ehdr->e_version           = EV_CURRENT;
  ehdr->e_phoff             = phdrOffset;
  ehdr->e_shoff             = shdrOffset;
  ehdr->e_ehsize            = sizeof(Elf64_Ehdr);
  ehdr->e_phentsize         = sizeof(Elf64_Phdr);
  ehdr->e_phnum             = numPhdrs;
  ehdr->e_shentsize         = sizeof(Elf64_Shdr);
  ehdr->e_shnum             = numShdrs;
  ehdr->e_shstrndx          = 3; // index of .shstrtab

  // 2. Program Headers
  auto *phdrs = reinterpret_cast<Elf64_Phdr *>(binary.data() + phdrOffset);
  // PT_LOAD
  phdrs[0].p_type   = PT_LOAD;
  phdrs[0].p_offset = 0;
  phdrs[0].p_vaddr  = 0;
  phdrs[0].p_paddr  = 0;
  phdrs[0].p_filesz = totalSize;
  phdrs[0].p_memsz  = totalSize;
  phdrs[0].p_flags  = PF_R | PF_X;
  phdrs[0].p_align  = 0x1000;

  // PT_DYNAMIC
  phdrs[1].p_type   = PT_DYNAMIC;
  phdrs[1].p_offset = dynOffset;
  phdrs[1].p_vaddr  = dynOffset;
  phdrs[1].p_paddr  = dynOffset;
  phdrs[1].p_filesz = dynSize;
  phdrs[1].p_memsz  = dynSize;
  phdrs[1].p_flags  = PF_R | PF_W;
  phdrs[1].p_align  = 8;

  if (withInterp) {
    phdrs[2].p_type   = PT_INTERP;
    phdrs[2].p_offset = interpOffset;
    phdrs[2].p_vaddr  = interpOffset;
    phdrs[2].p_paddr  = interpOffset;
    phdrs[2].p_filesz = interpSize;
    phdrs[2].p_memsz  = interpSize;
    phdrs[2].p_flags  = PF_R;
    phdrs[2].p_align  = 1;
    std::memcpy(binary.data() + interpOffset, interpStr.c_str(), interpSize);
  }

  // 3. Dynamic Entries
  std::memcpy(binary.data() + dynOffset, dyns.data(), dynSize);

  // 4. Dynstr
  std::memcpy(binary.data() + strtabOffset, strtab.data(), strtabSize);

  // 5. Shstrtab
  std::memcpy(binary.data() + shstrOffset, shstrtab.data(), shstrSize);

  // 6. Section Headers
  auto *shdrs = reinterpret_cast<Elf64_Shdr *>(binary.data() + shdrOffset);
  // Shdr 0: Null
  shdrs[0] = {};

  // Shdr 1: .dynamic
  shdrs[1].sh_name    = dynNameOff;
  shdrs[1].sh_type    = SHT_DYNAMIC;
  shdrs[1].sh_flags   = SHF_ALLOC | SHF_WRITE;
  shdrs[1].sh_addr    = dynOffset;
  shdrs[1].sh_offset  = dynOffset;
  shdrs[1].sh_size    = dynSize;
  shdrs[1].sh_link    = 2; // linked to .dynstr
  shdrs[1].sh_entsize = sizeof(Elf64_Dyn);

  // Shdr 2: .dynstr
  shdrs[2].sh_name   = dynstrNameOff;
  shdrs[2].sh_type   = SHT_STRTAB;
  shdrs[2].sh_flags  = SHF_ALLOC;
  shdrs[2].sh_addr   = strtabOffset;
  shdrs[2].sh_offset = strtabOffset;
  shdrs[2].sh_size   = strtabSize;

  // Shdr 3: .shstrtab
  shdrs[3].sh_name   = shstrNameOff;
  shdrs[3].sh_type   = SHT_STRTAB;
  shdrs[3].sh_offset = shstrOffset;
  shdrs[3].sh_size   = shstrSize;

  return binary;
}

} // namespace

// ============================================================================
// Track 1.1: PackageCas Tests (P21 K8, §6)
// ============================================================================

TEST(VagentClusterPackagingCas, StoreBlobAndReadBack) {
  const fs::path tempDir = createTempDir("cas_store_test");
  PackageCas cas(CasOptions{.casDir = tempDir, .maxBytes = 1024 * 1024});

  const std::string content = "Hello, Xanadu cluster CAS package system!";
  const std::string hash    = xanadu::toHex32(xanadu::sha256Digest(content));

  auto storedPath = cas.storeBlob(hash, content);
  ASSERT_TRUE(storedPath.has_value());
  EXPECT_TRUE(fs::exists(*storedPath));
  EXPECT_TRUE(cas.hasBlob(hash));

  auto readResult = cas.readBlob(hash);
  ASSERT_TRUE(readResult.has_value());
  EXPECT_EQ(*readResult, content);

  fs::remove_all(tempDir);
}

TEST(VagentClusterPackagingCas, RejectHashMismatchAndCleanupTemp) {
  const fs::path tempDir = createTempDir("cas_mismatch_test");
  PackageCas cas(CasOptions{.casDir = tempDir, .maxBytes = 1024 * 1024});

  const std::string content = "Actual payload content";
  const std::string fakeHash =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

  auto storedPath = cas.storeBlob(fakeHash, content);
  ASSERT_FALSE(storedPath.has_value());
  EXPECT_EQ(storedPath.error(), CasError::HashMismatch);
  EXPECT_FALSE(cas.hasBlob(fakeHash));

  // Verify no orphaned .part files left in tmp/
  EXPECT_EQ(cas.sweepTmp(), 0u);

  fs::remove_all(tempDir);
}

TEST(VagentClusterPackagingCas, StartupSweepUnlinksOrphanedPartFiles) {
  const fs::path tempDir = createTempDir("cas_sweep_test");
  const fs::path tmpDir  = tempDir / "tmp";
  fs::create_directories(tmpDir);

  // Write orphaned part files
  const fs::path part1 = tmpDir / "orphan1.part";
  const fs::path part2 = tmpDir / "orphan2.part";
  {
    std::ofstream(part1) << "abandoned data 1";
    std::ofstream(part2) << "abandoned data 2";
  }
  ASSERT_TRUE(fs::exists(part1));
  ASSERT_TRUE(fs::exists(part2));

  // Constructing PackageCas executes startup sweep
  PackageCas cas(CasOptions{.casDir = tempDir, .maxBytes = 1024 * 1024});

  EXPECT_FALSE(fs::exists(part1));
  EXPECT_FALSE(fs::exists(part2));

  fs::remove_all(tempDir);
}

TEST(VagentClusterPackagingCas, AtomicCommitFileModeIs0400) {
  const fs::path tempDir = createTempDir("cas_mode_test");
  PackageCas cas(CasOptions{.casDir = tempDir, .maxBytes = 1024 * 1024});

  const std::string content = "Read-only payload";
  const std::string hash    = xanadu::toHex32(xanadu::sha256Digest(content));

  auto storedPath = cas.storeBlob(hash, content);
  ASSERT_TRUE(storedPath.has_value());

  struct stat st{};
  ASSERT_EQ(::stat(storedPath->c_str(), &st), 0);
  const mode_t permissions = st.st_mode & 0777;
  EXPECT_EQ(permissions, 0400u);

  fs::remove_all(tempDir);
}

TEST(VagentClusterPackagingCas, PinAndUnpinRefCounting) {
  const fs::path tempDir = createTempDir("cas_pin_test");
  PackageCas cas(CasOptions{.casDir = tempDir, .maxBytes = 1024 * 1024});

  const std::string content = "Pinned blob content";
  const std::string hash    = xanadu::toHex32(xanadu::sha256Digest(content));

  auto storedPath = cas.storeBlob(hash, content);
  ASSERT_TRUE(storedPath.has_value());

  EXPECT_FALSE(cas.isPinned(hash));
  EXPECT_EQ(cas.pinCount(hash), 0u);

  EXPECT_TRUE(cas.pinBlob(hash).has_value());
  EXPECT_TRUE(cas.isPinned(hash));
  EXPECT_EQ(cas.pinCount(hash), 1u);

  // Pin a second time
  EXPECT_TRUE(cas.pinBlob(hash).has_value());
  EXPECT_EQ(cas.pinCount(hash), 2u);

  // Unpin once
  EXPECT_TRUE(cas.unpinBlob(hash).has_value());
  EXPECT_TRUE(cas.isPinned(hash));
  EXPECT_EQ(cas.pinCount(hash), 1u);

  // Unpin twice
  EXPECT_TRUE(cas.unpinBlob(hash).has_value());
  EXPECT_FALSE(cas.isPinned(hash));
  EXPECT_EQ(cas.pinCount(hash), 0u);

  // Unpin third time fails with NotPinned
  auto unpinError = cas.unpinBlob(hash);
  ASSERT_FALSE(unpinError.has_value());
  EXPECT_EQ(unpinError.error(), CasError::NotPinned);

  fs::remove_all(tempDir);
}

TEST(VagentClusterPackagingCas, LruEvictionRespects80PercentWatermark) {
  const fs::path tempDir = createTempDir("cas_lru_test");
  // Set max capacity to 500 bytes; 80% watermark is 400 bytes
  PackageCas cas(CasOptions{
      .casDir               = tempDir,
      .maxBytes             = 500,
      .lowWatermarkFraction = 0.8,
  });

  // Store 5 distinct blobs of 110 bytes each = 550 bytes total
  std::vector<std::string> hashes;
  for (int i = 0; i < 5; ++i) {
    std::string payload = "BlobPayloadNumber_" + std::to_string(i) + "_";
    payload.append(110 - payload.size(), 'X'); // pad to 110 bytes
    const std::string hash = xanadu::toHex32(xanadu::sha256Digest(payload));
    hashes.push_back(hash);
    auto res = cas.storeBlob(hash, payload);
    ASSERT_TRUE(res.has_value());
  }

  // After 5th store, total bytes (550) exceeded 500 -> eviction swept to <= 400
  // bytes
  EXPECT_LE(cas.totalBytes(), 400u);

  // Oldest blobs should have been evicted first
  EXPECT_FALSE(cas.hasBlob(hashes[0]));
  // Newest blobs should be preserved
  EXPECT_TRUE(cas.hasBlob(hashes[4]));

  fs::remove_all(tempDir);
}

TEST(VagentClusterPackagingCas, PinnedBlobsSurviveLruEviction) {
  const fs::path tempDir = createTempDir("cas_pinned_survival_test");
  PackageCas cas(CasOptions{
      .casDir               = tempDir,
      .maxBytes             = 300,
      .lowWatermarkFraction = 0.8, // 240 bytes
  });

  // Blob 0: 100 bytes (will be pinned)
  std::string b0 = "Blob_0_";
  b0.append(100 - b0.size(), 'A');
  const std::string h0 = xanadu::toHex32(xanadu::sha256Digest(b0));
  ASSERT_TRUE(cas.storeBlob(h0, b0).has_value());
  ASSERT_TRUE(cas.pinBlob(h0).has_value());

  // Blob 1: 100 bytes
  std::string b1 = "Blob_1_";
  b1.append(100 - b1.size(), 'B');
  const std::string h1 = xanadu::toHex32(xanadu::sha256Digest(b1));
  ASSERT_TRUE(cas.storeBlob(h1, b1).has_value());

  // Blob 2: 100 bytes
  std::string b2 = "Blob_2_";
  b2.append(100 - b2.size(), 'C');
  const std::string h2 = xanadu::toHex32(xanadu::sha256Digest(b2));
  ASSERT_TRUE(cas.storeBlob(h2, b2).has_value());

  // Blob 3: 100 bytes -> triggers eviction (total = 400 > 300)
  std::string b3 = "Blob_3_";
  b3.append(100 - b3.size(), 'D');
  const std::string h3 = xanadu::toHex32(xanadu::sha256Digest(b3));
  ASSERT_TRUE(cas.storeBlob(h3, b3).has_value());

  // Pinned blob 0 MUST survive even though it was the oldest!
  EXPECT_TRUE(cas.hasBlob(h0));
  EXPECT_TRUE(cas.isPinned(h0));

  // Blob 1 (oldest unpinned) must be evicted
  EXPECT_FALSE(cas.hasBlob(h1));

  fs::remove_all(tempDir);
}

TEST(VagentClusterPackagingCas, StageBlobReflinkAndMode0500) {
  const fs::path tempDir = createTempDir("cas_stage_test");
  PackageCas cas(CasOptions{.casDir = tempDir, .maxBytes = 1024 * 1024});

  const std::string payload = "Native binary executable code payload";
  const std::string hash    = xanadu::toHex32(xanadu::sha256Digest(payload));
  auto storedPath           = cas.storeBlob(hash, payload);
  ASSERT_TRUE(storedPath.has_value());

  const fs::path stageDst = tempDir / "staging" / "lib" / "libwidget.so";
  auto stageResult        = cas.stageBlob(hash, stageDst);
  ASSERT_TRUE(stageResult.has_value());
  EXPECT_TRUE(fs::exists(stageDst));

  // Verify file mode is strictly 0500 (read and execute only)
  struct stat st{};
  ASSERT_EQ(::stat(stageDst.c_str(), &st), 0);
  const mode_t mode = st.st_mode & 0777;
  EXPECT_EQ(mode, 0500u);

  // Verify content
  std::ifstream is(stageDst, std::ios::binary);
  std::string readContent((std::istreambuf_iterator<char>(is)),
                          std::istreambuf_iterator<char>());
  EXPECT_EQ(readContent, payload);

  fs::remove_all(tempDir);
}

// ============================================================================
// Track 1.2: PackageInspector Tests (P21 K3, K4, §7)
// ============================================================================

TEST(VagentClusterPackagingInspector, ElfDtSonameMatchesTarget) {
  const auto elfData =
      createMockElf("libengine.so.1", {"libc.so.6"}, {"$ORIGIN"});
  auto inspected = inspectElfBytes(elfData);
  ASSERT_TRUE(inspected.has_value());
  EXPECT_EQ(inspected->soname, "libengine.so.1");
  EXPECT_EQ(inspected->needed.size(), 1u);
  EXPECT_EQ(inspected->needed[0], "libc.so.6");
  EXPECT_FALSE(inspected->hasInterp);

  auto validated = validateBundledLibraryBytes(elfData, "libengine.so.1");
  EXPECT_TRUE(validated.has_value());
}

TEST(VagentClusterPackagingInspector, ElfDtSonameMismatchFailsClosed) {
  const auto elfData = createMockElf("libother.so.1");
  auto validated     = validateBundledLibraryBytes(elfData, "libexpected.so.1");
  ASSERT_FALSE(validated.has_value());
  EXPECT_EQ(validated.error(), InspectorError::MissingSonameTag);
}

TEST(VagentClusterPackagingInspector, ElfMissingDtSonameFailsClosed) {
  const auto elfData = createMockElf(""); // empty soname
  auto validated     = validateBundledLibraryBytes(elfData, "libengine.so.1");
  ASSERT_FALSE(validated.has_value());
  EXPECT_EQ(validated.error(), InspectorError::MissingSonameTag);
}

TEST(VagentClusterPackagingInspector, ElfPtInterpRefusal) {
  const auto elfData =
      createMockElf("libengine.so.1", {}, {"$ORIGIN"}, {}, /*withInterp=*/true);
  auto validated = validateBundledLibraryBytes(elfData, "libengine.so.1");
  ASSERT_FALSE(validated.has_value());
  EXPECT_EQ(validated.error(), InspectorError::InterpForbidden);
}

TEST(VagentClusterPackagingInspector, ElfDtRunpathAllowedTokens) {
  // $ORIGIN and $ORIGIN/lib are permitted
  const auto goodElf =
      createMockElf("libengine.so.1", {}, {"$ORIGIN:$ORIGIN/lib"});
  EXPECT_TRUE(
      validateBundledLibraryBytes(goodElf, "libengine.so.1").has_value());

  // Absolute or relative navigation path is refused with InsecureRpath
  const auto badElf = createMockElf("libengine.so.1", {}, {"$ORIGIN:/usr/lib"});
  auto res          = validateBundledLibraryBytes(badElf, "libengine.so.1");
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), InspectorError::InsecureRpath);

  const auto dotDotElf =
      createMockElf("libengine.so.1", {}, {"$ORIGIN/../lib"});
  auto resDotDot = validateBundledLibraryBytes(dotDotElf, "libengine.so.1");
  ASSERT_FALSE(resDotDot.has_value());
  EXPECT_EQ(resDotDot.error(), InspectorError::InsecureRpath);
}

TEST(VagentClusterPackagingInspector, SystemLibraryShadowingDenylist) {
  const std::vector<std::string> forbidden = {
      "libc.so.6",    "libm.so.6",     "libdl.so.2",        "libpthread.so.0",
      "librt.so.1",   "ld-linux.so.2", "libnss_files.so.2", "libSystem.B.dylib",
      "kernel32.dll", "ntdll.dll",
  };

  for (const auto &soname : forbidden) {
    const auto elfData = createMockElf(soname);
    auto validated     = validateBundledLibraryBytes(elfData, soname);
    ASSERT_FALSE(validated.has_value()) << "Expected failure for " << soname;
    EXPECT_EQ(validated.error(),
              InspectorError::SystemLibraryShadowingForbidden);
  }
}

TEST(VagentClusterPackagingInspector, InvalidSonamePathRejected) {
  const std::vector<std::string> invalidNames = {
      "../libbad.so",     "dir/libbad.so", "-libbad.so",
      "libbad.so;rm -rf", "libbad.exe",
  };

  for (const auto &soname : invalidNames) {
    const auto elfData = createMockElf(soname);
    auto validated     = validateBundledLibraryBytes(elfData, soname);
    ASSERT_FALSE(validated.has_value());
    EXPECT_EQ(validated.error(), InspectorError::InvalidSonamePath);
  }
}

TEST(VagentClusterPackagingInspector, TopologicalSortOrdering) {
  // Graph:
  // module.so depends on libengine.so.1 and libgui.so.1
  // libengine.so.1 depends on libcommon.so.1
  // libgui.so.1 depends on libcommon.so.1
  // libcommon.so.1 depends on libc.so.6 (system library)
  const std::vector<LibraryDependencyNode> slice = {
      {"module.so", {"libengine.so.1", "libgui.so.1", "libc.so.6"}},
      {"libgui.so.1", {"libcommon.so.1"}},
      {"libengine.so.1", {"libcommon.so.1"}},
      {"libcommon.so.1", {"libc.so.6"}},
  };

  auto sorted = topologicalSortLibraries(slice, {"libc.so.6"}, "module.so");
  ASSERT_TRUE(sorted.has_value());
  ASSERT_EQ(sorted->size(), 4u);

  // Deepest dependency must come first
  EXPECT_EQ((*sorted)[0], "libcommon.so.1");

  // Root module must come last
  EXPECT_EQ((*sorted)[3], "module.so");

  // Dependents must be ordered after libcommon and before module.so
  const auto posCommon =
      std::find(sorted->begin(), sorted->end(), "libcommon.so.1");
  const auto posEngine =
      std::find(sorted->begin(), sorted->end(), "libengine.so.1");
  const auto posGui = std::find(sorted->begin(), sorted->end(), "libgui.so.1");
  const auto posModule = std::find(sorted->begin(), sorted->end(), "module.so");

  EXPECT_LT(posCommon, posEngine);
  EXPECT_LT(posCommon, posGui);
  EXPECT_LT(posEngine, posModule);
  EXPECT_LT(posGui, posModule);
}

TEST(VagentClusterPackagingInspector, TopologicalSortDetectsCycles) {
  const std::vector<LibraryDependencyNode> cyclic = {
      {"libA.so.1", {"libB.so.1"}},
      {"libB.so.1", {"libA.so.1"}},
  };

  auto res = topologicalSortLibraries(cyclic);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), InspectorError::CyclicLibraryDependency);
}

TEST(VagentClusterPackagingInspector, TopologicalSortUnresolvedDependency) {
  const std::vector<LibraryDependencyNode> unresolved = {
      {"libengine.so.1", {"libmissing_custom.so.1"}},
  };

  auto res = topologicalSortLibraries(unresolved, {"libc.so.6"});
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), InspectorError::UnresolvedLibraryDependency);
}

// ============================================================================
// Track 1.3: PendingProgramFetch Tests (P21 K5, §8.3)
// ============================================================================

TEST(VagentClusterPackagingPendingFetch, QueueBoundedAt32Globally) {
  PendingFetchQueue queue(PendingFetchConfig{
      .maxPendingFetches        = 32,
      .maxPendingFetchesPerPeer = 64, // allow peer to reach 32
  });

  const auto origin = makeFingerprint('0');

  for (int i = 0; i < 32; ++i) {
    const std::string ref = "program_ref_" + std::to_string(i);
    EXPECT_TRUE(queue.enqueue(ref, origin).has_value());
  }

  // 33rd distinct fetch must be rejected with ResourceLimit
  auto overflow = queue.enqueue("program_ref_overflow", origin);
  ASSERT_FALSE(overflow.has_value());
  EXPECT_EQ(overflow.error(), ActorError::ResourceLimit);
}

TEST(VagentClusterPackagingPendingFetch, QueueBoundedAt4PerPeer) {
  PendingFetchQueue queue(PendingFetchConfig{
      .maxPendingFetches        = 32,
      .maxPendingFetchesPerPeer = 4,
  });

  const auto originA = makeFingerprint('A');
  const auto originB = makeFingerprint('B');

  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(
        queue.enqueue("ref_a_" + std::to_string(i), originA).has_value());
  }

  // 5th fetch from originA fails with ResourceLimit
  auto resA = queue.enqueue("ref_a_5", originA);
  ASSERT_FALSE(resA.has_value());
  EXPECT_EQ(resA.error(), ActorError::ResourceLimit);

  // originB can still enqueue up to 4 fetches
  EXPECT_TRUE(queue.enqueue("ref_b_1", originB).has_value());
}

TEST(VagentClusterPackagingPendingFetch,
     DeduplicationCoalescesConcurrentFetches) {
  PendingFetchQueue queue;
  const auto origin = makeFingerprint('1');

  const ActorPid pid1{"node1", "content1", 1, 100};
  const ActorPid pid2{"node1", "content2", 1, 101};

  EXPECT_TRUE(queue.enqueue("shared_ref", origin, pid1).has_value());
  EXPECT_EQ(queue.size(), 1u);

  // Second enqueue for same programRef coalesces into the existing entry
  EXPECT_TRUE(queue.enqueue("shared_ref", origin, pid2).has_value());
  EXPECT_EQ(queue.size(), 1u);

  const auto *fetch = queue.getFetch("shared_ref");
  ASSERT_NE(fetch, nullptr);
  EXPECT_EQ(fetch->pendingPids.size(), 2u);
  EXPECT_EQ(fetch->pendingPids[0], pid1);
  EXPECT_EQ(fetch->pendingPids[1], pid2);
}

TEST(VagentClusterPackagingPendingFetch,
     PreAdmissionMailboxBufferingAndOverflow) {
  PendingFetchQueue queue;
  const auto origin = makeFingerprint('2');
  const ActorPid pid{"node1", "content", 1, 200};

  ASSERT_TRUE(queue.enqueue("ref1", origin, pid).has_value());

  // Buffer messages up to 100 bytes limit
  const std::size_t maxMailboxBytes = 100;

  AuthenticatedActorEvent evt1;
  evt1.message.target = pid;
  evt1.message.value  = std::string(40, 'a');

  EXPECT_TRUE(queue.bufferMessage(pid, evt1, maxMailboxBytes).has_value());

  AuthenticatedActorEvent evt2;
  evt2.message.target = pid;
  evt2.message.value  = std::string(40, 'b');

  EXPECT_TRUE(queue.bufferMessage(pid, evt2, maxMailboxBytes).has_value());

  // evt3 (30 bytes) causes total to be 110 bytes > 100 -> rejected with
  // MailboxFull
  AuthenticatedActorEvent evt3;
  evt3.message.target = pid;
  evt3.message.value  = std::string(30, 'c');

  auto overflow = queue.bufferMessage(pid, evt3, maxMailboxBytes);
  ASSERT_FALSE(overflow.has_value());
  EXPECT_EQ(overflow.error(), ActorError::MailboxFull);
}

TEST(VagentClusterPackagingPendingFetch, FetchTimeoutTicks) {
  PendingFetchQueue queue(PendingFetchConfig{
      .fetchTimeout = std::chrono::milliseconds(50),
  });

  const auto origin = makeFingerprint('3');
  const auto now    = std::chrono::steady_clock::now();

  ASSERT_TRUE(
      queue.enqueue("timeout_ref", origin, std::nullopt, std::nullopt, now)
          .has_value());
  EXPECT_EQ(queue.size(), 1u);

  // Before timeout, tick returns nothing
  auto expired1 = queue.tick(now + std::chrono::milliseconds(20));
  EXPECT_TRUE(expired1.empty());
  EXPECT_EQ(queue.size(), 1u);

  // After timeout, tick removes and returns expired fetch
  auto expired2 = queue.tick(now + std::chrono::milliseconds(60));
  ASSERT_EQ(expired2.size(), 1u);
  EXPECT_EQ(expired2[0].programRef, "timeout_ref");
  EXPECT_EQ(queue.size(), 0u);
}

TEST(VagentClusterPackagingPendingFetch, ExceedingMaxPackageBytesAborts) {
  PendingFetchQueue queue(PendingFetchConfig{
      .maxPackageBytes = 1000,
  });

  const auto origin = makeFingerprint('4');
  ASSERT_TRUE(queue.enqueue("big_pkg", origin).has_value());

  EXPECT_TRUE(queue.recordBytesFetched("big_pkg", 500).has_value());

  // Additional 600 bytes exceeds 1000 limit -> aborts with TransportUnavailable
  auto res = queue.recordBytesFetched("big_pkg", 600);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), ActorError::TransportUnavailable);
  EXPECT_FALSE(queue.isPending("big_pkg"));
}

// ============================================================================
// Track 1.4: MeshActorRuntime Integration Tests (P21.1)
// ============================================================================

TEST(VagentClusterPackagingMeshActorRuntime,
     DeferredProgramAdmissionAndCompletion) {
  MeshActorRuntime runtime("nodeA", 1);
  const auto identity = makeFingerprint('A');
  runtime.trustNode("nodeA", identity);

  const std::string pendingProgRef = "pending_ref_12345";

  // Mark program as pending cluster fetch
  runtime.markProgramPending(pendingProgRef);
  EXPECT_TRUE(runtime.isProgramPending(pendingProgRef));

  runtime.setAuthorizer([&](const auto &, const auto &msg)
                            -> std::expected<ActorGrant, ActorError> {
    ActorGrant g;
    g.programRef      = msg.programRef;
    g.baseRef         = "base1";
    g.maxInstructions = 100000;
    g.maxCells        = 10000;
    g.maxOutputBytes  = 32768;
    g.maxSolutions    = 1;
    g.maxMailboxBytes = 65536;
    g.expiresAt = std::chrono::system_clock::now() + std::chrono::seconds(60);
    return g;
  });

  int turnsExecuted = 0;
  runtime.setExecutor(
      [&](const auto &, auto &) -> std::expected<std::string, ActorError> {
        ++turnsExecuted;
        return std::string("ok");
      });

  auto makeSpawnRequest = [](std::string reqId, std::string progRef) {
    ActorMessage req;
    req.type          = MessageType::ActorSpawn;
    req.requestId     = std::move(reqId);
    req.source        = ActorPid{"nodeA", "client", 1, 1};
    req.programRef    = std::move(progRef);
    req.hashcashStamp = "verified-stamp";
    req.capability    = "signed-grant";
    return req;
  };

  ActorMessage spawnReq = makeSpawnRequest("spawn1", pendingProgRef);

  const auto wallNow = std::chrono::system_clock::now();
  auto spawnResult   = runtime.spawn(identity, spawnReq, wallNow);
  ASSERT_TRUE(spawnResult.has_value());
  const ActorPid pid = *spawnResult;

  // 1. wake() on pending actor must return ProgramPending
  auto wakeResult = runtime.wake(pid, wallNow);
  ASSERT_FALSE(wakeResult.has_value());
  EXPECT_EQ(wakeResult.error(), ActorError::ProgramPending);

  // 2. Incoming messages targeting pending pid are buffered in mailbox
  ActorMessage msg;
  msg.type      = MessageType::ActorSend;
  msg.source    = ActorPid{"nodeA", "sender", 1, 99};
  msg.target    = pid;
  msg.requestId = "msg1";
  msg.value     = "hello pending actor";

  auto rxResult =
      runtime.receive(identity, msg, std::chrono::steady_clock::now(), wallNow);
  EXPECT_TRUE(rxResult.has_value());
  EXPECT_EQ(runtime.pending(pid), 1u);

  // 3. pumpOne() must skip the pending actor (no execution)
  auto pump1 = runtime.pumpOne(wallNow);
  ASSERT_TRUE(pump1.has_value());
  EXPECT_FALSE(*pump1); // no turn executed
  EXPECT_EQ(turnsExecuted, 0);

  // 4. completePendingProgram with success: unblocks actor and credits wait
  // time
  runtime.completePendingProgram(pendingProgRef, {});
  EXPECT_FALSE(runtime.isProgramPending(pendingProgRef));

  // 5. Now pumpOne() picks up the actor and executes the buffered turn!
  auto pump2 = runtime.pumpOne(wallNow);
  ASSERT_TRUE(pump2.has_value());
  EXPECT_TRUE(*pump2);
  EXPECT_EQ(turnsExecuted, 1);
  EXPECT_EQ(runtime.pending(pid), 0u);
}

TEST(VagentClusterPackagingMeshActorRuntime,
     PendingProgramFetchFailureStopsActor) {
  MeshActorRuntime runtime("nodeA", 1);
  const auto identity = makeFingerprint('B');
  runtime.trustNode("nodeA", identity);

  const std::string pendingProgRef = "failed_fetch_ref";
  runtime.markProgramPending(pendingProgRef);

  runtime.setAuthorizer([&](const auto &, const auto &msg)
                            -> std::expected<ActorGrant, ActorError> {
    ActorGrant g;
    g.programRef      = msg.programRef;
    g.baseRef         = "base1";
    g.maxInstructions = 100000;
    g.maxCells        = 10000;
    g.maxOutputBytes  = 32768;
    g.maxSolutions    = 1;
    g.maxMailboxBytes = 65536;
    g.expiresAt = std::chrono::system_clock::now() + std::chrono::seconds(60);
    return g;
  });

  runtime.setExecutor(
      [&](const auto &, auto &) -> std::expected<std::string, ActorError> {
        return std::string("ok");
      });

  auto makeSpawnRequest = [](std::string reqId, std::string progRef) {
    ActorMessage req;
    req.type          = MessageType::ActorSpawn;
    req.requestId     = std::move(reqId);
    req.source        = ActorPid{"nodeA", "client", 1, 1};
    req.programRef    = std::move(progRef);
    req.hashcashStamp = "verified-stamp";
    req.capability    = "signed-grant";
    return req;
  };

  ActorMessage spawnReq = makeSpawnRequest("spawn_fail", pendingProgRef);

  const auto wallNow = std::chrono::system_clock::now();
  auto spawnResult   = runtime.spawn(identity, spawnReq, wallNow);
  ASSERT_TRUE(spawnResult.has_value());
  const ActorPid pid = *spawnResult;

  // Complete with error (e.g. TransportUnavailable)
  runtime.completePendingProgram(
      pendingProgRef, std::unexpected(ActorError::TransportUnavailable));

  // Actor should now be stopped
  auto wakeRes = runtime.wake(pid, wallNow);
  ASSERT_FALSE(wakeRes.has_value());
  EXPECT_EQ(wakeRes.error(), ActorError::UnknownProcess);
}

TEST(VagentClusterPackagingMeshActorRuntime, UpgradeSuspendsOnPendingProgram) {
  MeshActorRuntime runtime("nodeA", 1);
  const auto identity = makeFingerprint('C');
  runtime.trustNode("nodeA", identity);

  runtime.setAuthorizer([&](const auto &, const auto &msg)
                            -> std::expected<ActorGrant, ActorError> {
    ActorGrant g;
    g.programRef      = msg.programRef;
    g.baseRef         = "base1";
    g.maxInstructions = 100000;
    g.maxCells        = 10000;
    g.maxOutputBytes  = 32768;
    g.maxSolutions    = 1;
    g.maxMailboxBytes = 65536;
    g.expiresAt = std::chrono::system_clock::now() + std::chrono::seconds(60);
    return g;
  });

  runtime.setExecutor(
      [&](const auto &, auto &) -> std::expected<std::string, ActorError> {
        return std::string("ok");
      });

  auto makeSpawnRequest = [](std::string reqId, std::string progRef) {
    ActorMessage req;
    req.type          = MessageType::ActorSpawn;
    req.requestId     = std::move(reqId);
    req.source        = ActorPid{"nodeA", "client", 1, 1};
    req.programRef    = std::move(progRef);
    req.hashcashStamp = "verified-stamp";
    req.capability    = "signed-grant";
    return req;
  };

  ActorMessage spawnReq =
      makeSpawnRequest("spawn_upgrade_test", "installed_prog_1");

  const auto wallNow = std::chrono::system_clock::now();
  auto spawnResult   = runtime.spawn(identity, spawnReq, wallNow);
  ASSERT_TRUE(spawnResult.has_value());
  const ActorPid pid = *spawnResult;

  // Target upgrade program is pending
  const std::string newProgRef = "upgrade_prog_pending";
  runtime.markProgramPending(newProgRef);

  ActorGrant upgradeGrant;
  upgradeGrant.programRef      = newProgRef;
  upgradeGrant.baseRef         = "base2";
  upgradeGrant.maxInstructions = 100000;
  upgradeGrant.maxCells        = 10000;
  upgradeGrant.maxOutputBytes  = 32768;
  upgradeGrant.maxSolutions    = 1;
  upgradeGrant.maxMailboxBytes = 65536;
  upgradeGrant.expiresAt       = wallNow + std::chrono::seconds(60);

  auto upgradeRes = runtime.upgrade(
      pid, upgradeGrant, wallNow,
      [](const ActorPid &, const zigzag::ArenaManifold &,
         std::optional<zigzag::CellRef>,
         WorkerContext &) -> std::expected<zigzag::CellRef, ActorError> {
        return zigzag::CellRef{1};
      });

  ASSERT_FALSE(upgradeRes.has_value());
  EXPECT_EQ(upgradeRes.error(), ActorError::ProgramPending);
}
