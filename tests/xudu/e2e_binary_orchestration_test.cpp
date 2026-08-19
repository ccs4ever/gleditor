/**
 * @file e2e_binary_orchestration_test.cpp
 * @brief End-to-end integration test suite orchestrating the actual xudu binary
 *        across 5 lifecycle scenarios with visual PPM and PNG screenshots:
 *
 *  Step 1: Loading 2 source media torrents and quoting ranges.
 *  Step 2: Creating and loading 2 independent XanaDoc publications.
 *  Step 3: Bi-directional linking between the XanaDocs.
 *  Step 4: Transcluding content across the XanaDocs.
 *  Step 5: Applying 2 LinkPackages (including 3rd source materialization).
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <xudu/core/link_package.hpp>
#include <xudu/core/ops.hpp>
#include <xudu/core/publication.hpp>
#include <xudu/core/scroll.hpp>
#include <xudu/core/store.hpp>
#include <xudu/core/torrent.hpp>
#include <xudu/core/version.hpp>

#include "torrent_data.hpp"

namespace {

namespace fs = std::filesystem;

using xudu::adopt;
using xudu::adoptLinkPackage;
using xudu::createMutableKeys;
using xudu::decodePublication;
using xudu::encodeLinkPackage;
using xudu::encodePublication;
using xudu::GlobalLink;
using xudu::GlobalSpan;
using xudu::InfoHash;
using xudu::Link;
using xudu::LinkPackage;
using xudu::LinkType;
using xudu::MicroversionId;
using xudu::MutableKeys;
using xudu::PrimediaSpan;
using xudu::ProminenceTier;
using xudu::Publication;
using xudu::publicationSigningBuffer;
using xudu::publish;
using xudu::publishLinkPackage;
using xudu::Scroll;
using xudu::scrollKey;
using xudu::ScrollSegment;
using xudu::signMutableItem;
using xudu::Store;

// Source 3 vector: 84 bytes "Epilogue..."
inline const std::string source3Torrent = xudu_test::fromHex(
    "64383a616e6e6f756e636533313a687474703a2f2f747261636b65722e696e76616c"
    "69642f616e6e6f756e6365343a696e666f64363a6c656e67746869383465343a6e61"
    "6d6531323a6570696c6f6775652e74787431323a7069656365206c656e6774686933"
    "3265363a70696563657336303a9f93aca018285eec7e84e17c31ad13d1c45bea4c11"
    "7a8da4cc40dbb9b07ba522cb1fc571c6b9c38e7ac262631c3c84c1b10421a8c61d46"
    "4c1202347c6565");
inline const std::string source3Text =
    "Epilogue: Xanadocs transclude, link, and materialize eternal primedia "
    "across swarms.";
inline constexpr const char *source3Hash =
    "69137d16acf934bc28e6e6745e03079c9f7c9947";

struct ExecutionResult {
  int exitCode{-1};
  std::string output;
};

ExecutionResult executeProcess(const std::string &cmd) {
  std::string fullCmd = cmd + " 2>&1";
  FILE *pipe          = popen(fullCmd.c_str(), "r");
  if (!pipe) {
    return {-1, "Failed to popen: " + cmd};
  }
  char buffer[512];
  std::string output;
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    output += buffer;
  }
  int status = pclose(pipe);
  int code   = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return {code, output};
}

struct PpmImageInfo {
  bool valid{false};
  int width{0};
  int height{0};
  std::size_t distinctColors{0};
  std::string errorMessage;
};

PpmImageInfo inspectPpm(const fs::path &path) {
  PpmImageInfo info;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    info.errorMessage = "Failed to open PPM: " + path.string();
    return info;
  }
  std::string magic;
  in >> magic;
  if (magic != "P6") {
    info.errorMessage = "Not P6 format: " + magic;
    return info;
  }
  in >> info.width >> info.height;
  int maxVal = 0;
  in >> maxVal;
  char ws;
  in.read(&ws, 1);

  std::vector<char> rawPixels(
      static_cast<std::size_t>(info.width * info.height * 3));
  in.read(rawPixels.data(), rawPixels.size());
  if (in.gcount() != static_cast<std::streamsize>(rawPixels.size())) {
    info.errorMessage = "Truncated pixel stream in PPM";
    return info;
  }

  std::set<std::tuple<uint8_t, uint8_t, uint8_t>> unique;
  for (std::size_t i = 0; i + 2 < rawPixels.size(); i += 3) {
    unique.emplace(static_cast<uint8_t>(rawPixels[i]),
                   static_cast<uint8_t>(rawPixels[i + 1]),
                   static_cast<uint8_t>(rawPixels[i + 2]));
  }
  info.distinctColors = unique.size();
  info.valid          = true;
  return info;
}

void exportToPng(const fs::path &ppmPath, const fs::path &pngPath) {
  std::string py = "python3 -c \"from PIL import Image; Image.open('" +
                   ppmPath.string() + "').save('" + pngPath.string() +
                   "')\" >/dev/null 2>&1";
  static_cast<void>(std::system(py.c_str()));
}

fs::path findXuduBinary() {
  std::vector<fs::path> candidates = {
      fs::current_path() / "build" / "xudu",
      fs::current_path() / "xudu",
      fs::current_path() / ".." / "build" / "xudu",
  };
  for (const auto &cand : candidates) {
    if (fs::exists(cand) && (fs::status(cand).permissions() &
                             fs::perms::owner_exec) != fs::perms::none) {
      return cand;
    }
  }
  return fs::current_path() / "build" / "xudu";
}

TEST(E2EBinaryOrchestrationTest,
     fullFiveStepOrchestrationWithVisualVerification) {
  const auto xuduBin = findXuduBinary();
  ASSERT_TRUE(fs::exists(xuduBin)) << "xudu binary not found at " << xuduBin;

  const auto testRoot = fs::current_path() / "build" / "integration_workspace";
  const auto screenshotDir =
      fs::current_path() / "build" / "integration_screenshots";

  fs::remove_all(testRoot);
  fs::create_directories(testRoot);
  fs::create_directories(screenshotDir);

  // Write source torrent files and payloads to separate directories
  const auto s1Dir = testRoot / "sources" / "source1";
  const auto s2Dir = testRoot / "sources" / "source2";
  const auto s3Dir = testRoot / "sources" / "source3";
  fs::create_directories(s1Dir);
  fs::create_directories(s2Dir / "sub");
  fs::create_directories(s3Dir);

  // Source 1 (Single file)
  const auto s1TorrentPath = s1Dir / "source1.torrent";
  const auto s1DataPath    = s1Dir / "fox.txt";
  {
    std::ofstream out(s1TorrentPath, std::ios::binary);
    out << xudu_test::singleFileTorrent;
  }
  {
    std::ofstream out(s1DataPath, std::ios::binary);
    out << xudu_test::singleFileText;
  }

  // Source 2 (Multi-file)
  const auto s2TorrentPath = s2Dir / "source2.torrent";
  {
    std::ofstream out(s2TorrentPath, std::ios::binary);
    out << xudu_test::multiFileTorrent;
  }
  {
    std::ofstream out(s2Dir / "one.txt", std::ios::binary);
    out << xudu_test::multiFileFirst;
  }
  {
    std::ofstream out(s2Dir / "sub" / "two.txt", std::ios::binary);
    out << xudu_test::multiFileSecond;
  }

  // Source 3 (Single file)
  const auto s3TorrentPath = s3Dir / "source3.torrent";
  const auto s3DataPath    = s3Dir / "epilogue.txt";
  {
    std::ofstream out(s3TorrentPath, std::ios::binary);
    out << source3Torrent;
  }
  {
    std::ofstream out(s3DataPath, std::ios::binary);
    out << source3Text;
  }

  const std::string torrentArgs = " --torrent " + s1TorrentPath.string() +
                                  " --torrent " + s2TorrentPath.string() +
                                  " --torrent " + s3TorrentPath.string();

  // =========================================================================
  // STEP 1: Loading 2 Source Torrents into Xudu Store
  // =========================================================================
  const auto storeStep1 = testRoot / "store_step1";
  const auto step1Ppm   = screenshotDir / "step1_source_torrents.ppm";
  const auto step1Png   = screenshotDir / "step1_source_torrents.png";

  MicroversionId v1;
  MicroversionId v2;
  {
    Store initialStore;
    const auto s1Hash   = InfoHash::fromHex(xudu_test::singleFileHash);
    const auto s1Scroll = Scroll::ofTorrentFile(s1Hash, 0, "fox.txt", 0, 62);
    v1 = initialStore.transcludeExternal(MicroversionId{}, 0, s1Scroll, 0, 62);

    const auto s2Hash   = InfoHash::fromHex(xudu_test::multiFileHash);
    const auto s2Scroll = Scroll::ofTorrentFile(s2Hash, 0, "one.txt", 0, 27);
    v2 = initialStore.transcludeExternal(MicroversionId{}, 0, s2Scroll, 0, 27);
    initialStore.save(storeStep1.string());
  }

  std::string cmd1 =
      xuduBin.string() +
      " --backend opengl --no-present --profile --fov 7.5 --coarse-below 0" +
      torrentArgs + " --version-id " + v1.str() + " --alongside " + v2.str() +
      " --screenshot " + step1Ppm.string() + " " + storeStep1.string();

  const auto res1 = executeProcess(cmd1);
  EXPECT_EQ(res1.exitCode, 0) << "Step 1 process failed: " << res1.output;
  EXPECT_TRUE(fs::exists(step1Ppm)) << "Step 1 screenshot missing";

  const auto info1 = inspectPpm(step1Ppm);
  EXPECT_TRUE(info1.valid) << "Step 1 PPM invalid: " << info1.errorMessage;
  EXPECT_EQ(info1.width, 800);
  EXPECT_EQ(info1.height, 600);
  EXPECT_GE(info1.distinctColors, 20U)
      << "Step 1 screenshot has insufficient color detail";
  exportToPng(step1Ppm, step1Png);

  // =========================================================================
  // STEP 2: Loading 2 XanaDoc Publications Side-by-Side
  // =========================================================================
  // Author XanaDoc A (Alice): Quotes from Source 1
  const auto authorA = createMutableKeys();
  Store storeA;
  const auto s1Hash   = InfoHash::fromHex(xudu_test::singleFileHash);
  const auto s1Scroll = Scroll::ofTorrentFile(s1Hash, 0, "fox.txt", 0, 62);
  const auto vA1 =
      storeA.transcludeExternal(MicroversionId{}, 0, s1Scroll, 0, 62);
  auto pubA = publish(storeA, vA1, authorA, "xanadoc_a",
                      "Alice Study on Fox Behavior", 1, 1700000000, nullptr);
  pubA.signature = signMutableItem(publicationSigningBuffer(pubA), authorA);

  const auto pubAPath = testRoot / "xanadoc_a.manifest";
  {
    std::ofstream out(pubAPath, std::ios::binary);
    out << encodePublication(pubA);
  }

  // Author XanaDoc B (Bob): Quotes from Source 2
  const auto authorB = createMutableKeys();
  Store storeB;
  const auto s2Hash   = InfoHash::fromHex(xudu_test::multiFileHash);
  const auto s2Scroll = Scroll::ofTorrentFile(s2Hash, 0, "one.txt", 0, 27);
  const auto vB1 =
      storeB.transcludeExternal(MicroversionId{}, 0, s2Scroll, 0, 27);
  auto pubB      = publish(storeB, vB1, authorB, "xanadoc_b",
                           "Bob Multi-file Analysis", 1, 1700000100, nullptr);
  pubB.signature = signMutableItem(publicationSigningBuffer(pubB), authorB);

  const auto pubBPath = testRoot / "xanadoc_b.manifest";
  {
    std::ofstream out(pubBPath, std::ios::binary);
    out << encodePublication(pubB);
  }

  const auto storeReader = testRoot / "store_reader";
  const auto step2Ppm    = screenshotDir / "step2_xanadocs_loaded.ppm";
  const auto step2Png    = screenshotDir / "step2_xanadocs_loaded.png";

  std::string cmd2 =
      xuduBin.string() +
      " --backend opengl --no-present --profile --fov 7.5 --coarse-below 0" +
      torrentArgs + " --read " + pubAPath.string() + " --read " +
      pubBPath.string() + " --screenshot " + step2Ppm.string() + " " +
      storeReader.string();

  const auto res2 = executeProcess(cmd2);
  EXPECT_EQ(res2.exitCode, 0) << "Step 2 process failed: " << res2.output;
  EXPECT_TRUE(fs::exists(step2Ppm)) << "Step 2 screenshot missing";

  const auto info2 = inspectPpm(step2Ppm);
  EXPECT_TRUE(info2.valid) << "Step 2 PPM invalid: " << info2.errorMessage;
  EXPECT_GE(info2.distinctColors, 20U);
  exportToPng(step2Ppm, step2Png);

  // =========================================================================
  // STEP 3: Bi-directional Linking Between the XanaDocs
  // =========================================================================
  Store activeReaderStore;
  activeReaderStore.load(storeReader.string());
  const auto allReaderVersions = activeReaderStore.allVersions();
  ASSERT_GE(allReaderVersions.size(), 2U);
  const auto verA = allReaderVersions[0];
  const auto verB = allReaderVersions[1];

  Link crossDocLink;
  crossDocLink.type  = LinkType::Comment;
  crossDocLink.owner = "reader_curator";
  crossDocLink.left.push_back(
      activeReaderStore.rebuild(verA).spansFor(0, 30).front());
  crossDocLink.right.push_back(
      activeReaderStore.rebuild(verB).spansFor(0, 20).front());
  const auto verLinked = activeReaderStore.addLink(verA, crossDocLink);
  activeReaderStore.save(storeReader.string());

  const auto step3Ppm = screenshotDir / "step3_cross_linking.ppm";
  const auto step3Png = screenshotDir / "step3_cross_linking.png";

  std::string cmd3 =
      xuduBin.string() +
      " --backend opengl --no-present --profile --fov 7.5 --coarse-below 0" +
      torrentArgs + " --version-id " + verLinked.str() + " --alongside " +
      verB.str() + " --screenshot " + step3Ppm.string() + " " +
      storeReader.string();

  const auto res3 = executeProcess(cmd3);
  EXPECT_EQ(res3.exitCode, 0) << "Step 3 process failed: " << res3.output;
  EXPECT_TRUE(fs::exists(step3Ppm)) << "Step 3 screenshot missing";

  const auto info3 = inspectPpm(step3Ppm);
  EXPECT_TRUE(info3.valid) << "Step 3 PPM invalid: " << info3.errorMessage;
  EXPECT_GE(info3.distinctColors, 20U);
  exportToPng(step3Ppm, step3Png);

  // =========================================================================
  // STEP 4: Transcluding Content Between the XanaDocs
  // =========================================================================
  // Transclude 20 bytes from XanaDoc A into XanaDoc B
  const auto verBTranscluded =
      activeReaderStore.transclude(verB, 0, verA, 0, 20);
  activeReaderStore.save(storeReader.string());

  const auto step4Ppm = screenshotDir / "step4_transclusion.ppm";
  const auto step4Png = screenshotDir / "step4_transclusion.png";

  std::string cmd4 =
      xuduBin.string() +
      " --backend opengl --no-present --profile --fov 7.5 --coarse-below 0" +
      torrentArgs + " --version-id " + verLinked.str() + " --alongside " +
      verBTranscluded.str() + " --screenshot " + step4Ppm.string() + " " +
      storeReader.string();

  const auto res4 = executeProcess(cmd4);
  EXPECT_EQ(res4.exitCode, 0) << "Step 4 process failed: " << res4.output;
  EXPECT_TRUE(fs::exists(step4Ppm)) << "Step 4 screenshot missing";

  const auto info4 = inspectPpm(step4Ppm);
  EXPECT_TRUE(info4.valid) << "Step 4 PPM invalid: " << info4.errorMessage;
  EXPECT_GE(info4.distinctColors, 20U);
  exportToPng(step4Ppm, step4Png);

  // =========================================================================
  // STEP 5: Creating & Applying 2 LinkPackages (Materializing 3rd Source)
  // =========================================================================
  const auto curatorKeys = createMutableKeys();

  // Author XanaDoc C (Epilogue) quoting Source 3
  const auto authorC = createMutableKeys();
  Store storeC;
  const auto s3Hash   = InfoHash::fromHex(source3Hash);
  const auto s3Scroll = Scroll::ofTorrentFile(s3Hash, 0, "epilogue.txt", 0, 84);
  const auto vC1 =
      storeC.transcludeExternal(MicroversionId{}, 0, s3Scroll, 0, 84);
  auto pubC =
      publish(storeC, vC1, authorC, "xanadoc_c",
              "Epilogue on Universal Xanadu Wisdom", 1, 1700000250, nullptr);
  pubC.signature = signMutableItem(publicationSigningBuffer(pubC), authorC);

  const auto pubCPath = testRoot / "xanadoc_c.manifest";
  {
    std::ofstream out(pubCPath, std::ios::binary);
    out << encodePublication(pubC);
  }

  // LinkPackage 1 (Curated): Commentary links between XanaDoc A and B
  std::vector<GlobalLink> pkg1Links;
  GlobalLink l1;
  l1.type  = LinkType::Comment;
  l1.owner = "Editorial_Committee";
  l1.left.push_back(GlobalSpan{scrollKey(s1Scroll), 0, 25});
  l1.right.push_back(GlobalSpan{scrollKey(s2Scroll), 0, 20});
  pkg1Links.push_back(l1);

  std::map<std::string, Scroll> pkg1Scrolls;
  pkg1Scrolls.insert_or_assign(l1.left.front().scroll, s1Scroll);
  pkg1Scrolls.insert_or_assign(l1.right.front().scroll, s2Scroll);

  const auto linkPkg1 = publishLinkPackage(
      curatorKeys, "pkg_curated", "Editorial Curated Commentary", 1, 1700000200,
      std::move(pkg1Links), std::move(pkg1Scrolls));

  // LinkPackage 2 (External Materializing): References 3rd Source Torrent
  std::vector<GlobalLink> pkg2Links;
  GlobalLink l2;
  l2.type  = LinkType::Quotation;
  l2.owner = "Scholarly_Annotator";
  l2.left.push_back(GlobalSpan{scrollKey(s1Scroll), 30, 20});
  l2.right.push_back(GlobalSpan{scrollKey(s3Scroll), 0, 40});
  pkg2Links.push_back(l2);

  std::map<std::string, Scroll> pkg2Scrolls;
  pkg2Scrolls.insert_or_assign(l2.left.front().scroll, s1Scroll);
  pkg2Scrolls.insert_or_assign(l2.right.front().scroll, s3Scroll);

  const auto linkPkg2 = publishLinkPackage(
      curatorKeys, "pkg_external", "Scholarly Epilogue Citations", 1,
      1700000300, std::move(pkg2Links), std::move(pkg2Scrolls));

  // Adopt both LinkPackages into activeReaderStore
  const auto adoptRes1 =
      adoptLinkPackage(activeReaderStore, linkPkg1, ProminenceTier::Curated);
  EXPECT_GT(adoptRes1.linksAdopted, 0U);

  const auto adoptRes2 =
      adoptLinkPackage(activeReaderStore, linkPkg2, ProminenceTier::Curated);
  EXPECT_GT(adoptRes2.linksAdopted, 0U);
  EXPECT_GT(adoptRes2.scrollsAdded, 0U)
      << "Source 3 scroll failed to materialize";
  activeReaderStore.save(storeReader.string());

  // Also read pubC into activeReaderStore to create its microversion in the
  // store
  const auto verC = activeReaderStore.transcludeExternal(MicroversionId{}, 0,
                                                         s3Scroll, 0, 84);
  activeReaderStore.save(storeReader.string());

  const auto step5Ppm = screenshotDir / "step5_link_packages_applied.ppm";
  const auto step5Png = screenshotDir / "step5_link_packages_applied.png";

  std::string cmd5 =
      xuduBin.string() +
      " --backend opengl --no-present --profile --fov 15 --coarse-below 0" +
      torrentArgs + " --read " + pubAPath.string() + " --read " +
      pubBPath.string() + " --read " + pubCPath.string() + " --screenshot " +
      step5Ppm.string() + " " + storeReader.string();

  const auto res5 = executeProcess(cmd5);
  EXPECT_EQ(res5.exitCode, 0) << "Step 5 process failed: " << res5.output;
  EXPECT_TRUE(fs::exists(step5Ppm)) << "Step 5 screenshot missing";

  const auto info5 = inspectPpm(step5Ppm);
  EXPECT_TRUE(info5.valid) << "Step 5 PPM invalid: " << info5.errorMessage;
  EXPECT_GE(info5.distinctColors, 20U);
  exportToPng(step5Ppm, step5Png);

  std::cout << "E2E Binary Orchestration completed successfully:\n"
            << "  - Step 1: " << step1Png << " (" << info1.distinctColors
            << " colors)\n"
            << "  - Step 2: " << step2Png << " (" << info2.distinctColors
            << " colors)\n"
            << "  - Step 3: " << step3Png << " (" << info3.distinctColors
            << " colors)\n"
            << "  - Step 4: " << step4Png << " (" << info4.distinctColors
            << " colors)\n"
            << "  - Step 5: " << step5Png << " (" << info5.distinctColors
            << " colors)\n";
}

} // namespace
