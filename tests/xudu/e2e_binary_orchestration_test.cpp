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

std::string activeBackend() {
  const char *env = std::getenv("XUDU_BACKEND");
  return (env && *env) ? std::string(env) : "opengl";
}

fs::path getScreenshotDir() {
  const char *env = std::getenv("XUDU_SCREENSHOT_DIR");
  if (env && *env) {
    return fs::path(env);
  }
  return fs::current_path() / "build" / "integration_screenshots";
}

std::string makeMultiPageText(std::size_t pageCount, const std::string &topic) {
  // Calibrated against the app's actual page layout (139.7 DPI, 11in-tall
  // Letter page) rather than guessed: with a ~30-character topic name
  // embedded in every filler line, 22 filler lines is where a page's real
  // rendered capacity tips over into a second physical page (measured with
  // a throwaway single-page probe against the real xudu binary). 20 leaves
  // a small margin so a somewhat longer topic name still lands close to
  // one requested page per one rendered page, instead of the ~3x overshoot
  // the old fixed 82-line count produced (an 8-page request rendering as
  // 27 real pages).
  constexpr std::size_t linesPerLogicalPage = 20;
  std::string text;
  for (std::size_t p = 1; p <= pageCount; ++p) {
    text += "=== " + topic + " - Page " + std::to_string(p) + " of " +
            std::to_string(pageCount) + " ===\n";
    text += "The initial sentence establishes fundamental axioms of "
            "hyperstructure and coordinate manifolds.\n";
    for (std::size_t line = 3; line <= 2 + linesPerLogicalPage; ++line) {
      text += "Line " + std::to_string(line) +
              ": Content chunk validating manifold invariants in " + topic +
              " page " + std::to_string(p) + ".\n";
    }
    text += "Visual variables and Bertin semiology govern graphical density, "
            "color hue, and spatial separation.\n";
    text += "Concluding summary of section " + std::to_string(p) +
            " confirming robust mathematical invariants.\n\n";
  }
  return text;
}

std::string makeFullPageProse(const std::string &title,
                              const std::string &focus) {
  std::string text;
  text += "========================================================\n";
  text += "=== " + title + " ===\n";
  text += "========================================================\n\n";
  text += "Section 1: Fundamental Principles and Architectural Axioms\n";
  text += "In the design of universal hypermedia systems, all content "
          "addresses\n";
  text += "are immutable and content-derived. A span is a range of offsets "
          "into a\n";
  text += "permanent scroll, and its meaning never decays across distributed "
          "swarms.\n\n";
  text += "Section 2: Geometric Visual Variables and Dynamic Depth\n";
  text += "Following Bertin's semiology of graphics, link types are "
          "differentiated\n";
  text += "by tailored visual variables: chromatic hue for relation "
          "categories,\n";
  text += "value gradient for traversal direction, and spatial bundling for "
          "clarity.\n";
  text += "Focus area: " + focus + "\n\n";
  text += "Section 3: Centroid Alignment and Camera Frustum Dynamics\n";
  text += "When documents are brought alongside one another, centroid "
          "alignment\n";
  text += "levels the vertical midpoint of multi-span selections, eliminating "
          "jump.\n";
  text += "Dynamic camera auto-framing adjusts eye position to fit all "
          "connection\n";
  text += "anchors simultaneously within the screen viewport.\n\n";
  text += "Section 4: Concluding Synthesis on Hypertime Topology\n";
  text += "Hypertime branches preserve all previous versions without "
          "destructive\n";
  text += "overwrites. Cross-document ribbons pass through foreground and "
          "depth\n";
  text += "layers cleanly, providing unprecedented structural insight.\n";
  return text;
}

TEST(E2EBinaryOrchestrationTest,
     fullPageTransclusionAndAdoptionLifecycleOrchestration) {
  const auto xuduBin = findXuduBinary();
  ASSERT_TRUE(fs::exists(xuduBin)) << "xudu binary not found at " << xuduBin;

  const auto testRoot = fs::current_path() / "build" / "integration_workspace";
  const auto screenshotDir = getScreenshotDir();

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

  // STEP 1: Quoting 2 Source Media Torrents
  Store storeStep1Inst;
  const auto s1Hash   = InfoHash::fromHex(xudu_test::singleFileHash);
  const auto s1Scroll = Scroll::ofTorrentFile(s1Hash, 0, "fox.txt", 0, 62);
  const auto v1 =
      storeStep1Inst.transcludeExternal(MicroversionId{}, 0, s1Scroll, 0, 44);

  const auto s2Hash   = InfoHash::fromHex(xudu_test::multiFileHash);
  const auto s2Scroll = Scroll::ofTorrentFile(s2Hash, 0, "one.txt", 0, 27);
  const auto v2 =
      storeStep1Inst.transcludeExternal(MicroversionId{}, 0, s2Scroll, 0, 27);

  const auto storeStep1 = testRoot / "store_step1";
  storeStep1Inst.save(storeStep1.string());

  const auto step1Ppm = screenshotDir / "step1_source_torrents.ppm";
  const auto step1Png = screenshotDir / "step1_source_torrents.png";

  std::string cmd1 = xuduBin.string() + " --backend " + activeBackend() +
                     " --profile --fov 7.5 --coarse-below 0" + torrentArgs +
                     " --version-id " + v1.str() + " --alongside " + v2.str() +
                     " --screenshot " + step1Ppm.string() + " " +
                     storeStep1.string();

  const auto res1 = executeProcess(cmd1);
  EXPECT_EQ(res1.exitCode, 0) << "Step 1 process failed: " << res1.output;
  EXPECT_TRUE(fs::exists(step1Ppm)) << "Step 1 screenshot missing";

  const auto info1 = inspectPpm(step1Ppm);
  EXPECT_TRUE(info1.valid) << "Step 1 PPM invalid: " << info1.errorMessage;
  EXPECT_GT(info1.width, 0);
  EXPECT_GT(info1.height, 0);
  EXPECT_GE(info1.distinctColors, 20U);
  exportToPng(step1Ppm, step1Png);

  // STEP 2: Loading 2 XanaDoc Publications Side-by-Side
  const auto authorA = createMutableKeys();
  Store storeA;
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

  const auto authorB = createMutableKeys();
  Store storeB;
  const auto vB1 =
      storeB.transcludeExternal(MicroversionId{}, 0, s2Scroll, 0, 27);
  auto pubB =
      publish(storeB, vB1, authorB, "xanadoc_b",
              "Bob Observations on Multi-Source Data", 1, 1700000050, nullptr);
  pubB.signature = signMutableItem(publicationSigningBuffer(pubB), authorB);

  const auto pubBPath = testRoot / "xanadoc_b.manifest";
  {
    std::ofstream out(pubBPath, std::ios::binary);
    out << encodePublication(pubB);
  }

  const auto storeReader = testRoot / "store_reader";
  const auto step2Ppm    = screenshotDir / "step2_xanadocs_loaded.ppm";
  const auto step2Png    = screenshotDir / "step2_xanadocs_loaded.png";

  std::string cmd2 = xuduBin.string() + " --backend " + activeBackend() +
                     " --profile --fov 7.5 --coarse-below 0" + torrentArgs +
                     " --read " + pubAPath.string() + " --read " +
                     pubBPath.string() + " --screenshot " + step2Ppm.string() +
                     " " + storeReader.string();

  const auto res2 = executeProcess(cmd2);
  EXPECT_EQ(res2.exitCode, 0) << "Step 2 process failed: " << res2.output;
  EXPECT_TRUE(fs::exists(step2Ppm)) << "Step 2 screenshot missing";

  const auto info2 = inspectPpm(step2Ppm);
  EXPECT_TRUE(info2.valid) << "Step 2 PPM invalid: " << info2.errorMessage;
  EXPECT_GE(info2.distinctColors, 20U);
  exportToPng(step2Ppm, step2Png);

  // STEP 3: Bi-directional Linking Between the XanaDocs
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

  std::string cmd3 = xuduBin.string() + " --backend " + activeBackend() +
                     " --profile --fov 7.5 --coarse-below 0" + torrentArgs +
                     " --version-id " + verLinked.str() + " --alongside " +
                     verB.str() + " --screenshot " + step3Ppm.string() + " " +
                     storeReader.string();

  const auto res3 = executeProcess(cmd3);
  EXPECT_EQ(res3.exitCode, 0) << "Step 3 process failed: " << res3.output;
  EXPECT_TRUE(fs::exists(step3Ppm)) << "Step 3 screenshot missing";

  const auto info3 = inspectPpm(step3Ppm);
  EXPECT_TRUE(info3.valid) << "Step 3 PPM invalid: " << info3.errorMessage;
  EXPECT_GE(info3.distinctColors, 20U);
  exportToPng(step3Ppm, step3Png);

  // STEP 4: Transcluding Content Between the XanaDocs
  const auto verBTranscluded =
      activeReaderStore.transclude(verB, 0, verA, 0, 20);
  activeReaderStore.save(storeReader.string());

  const auto step4Ppm = screenshotDir / "step4_transclusion.ppm";
  const auto step4Png = screenshotDir / "step4_transclusion.png";

  std::string cmd4 = xuduBin.string() + " --backend " + activeBackend() +
                     " --profile --fov 7.5 --coarse-below 0" + torrentArgs +
                     " --version-id " + verLinked.str() + " --alongside " +
                     verBTranscluded.str() + " --screenshot " +
                     step4Ppm.string() + " " + storeReader.string();

  const auto res4 = executeProcess(cmd4);
  EXPECT_EQ(res4.exitCode, 0) << "Step 4 process failed: " << res4.output;
  EXPECT_TRUE(fs::exists(step4Ppm)) << "Step 4 screenshot missing";

  const auto info4 = inspectPpm(step4Ppm);
  EXPECT_TRUE(info4.valid) << "Step 4 PPM invalid: " << info4.errorMessage;
  EXPECT_GE(info4.distinctColors, 20U);
  exportToPng(step4Ppm, step4Png);

  // STEP 5: Creating & Applying LinkPackages (Materializing 3rd Source)
  const auto curatorKeys = createMutableKeys();
  const auto authorC     = createMutableKeys();
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

  adoptLinkPackage(activeReaderStore, linkPkg1, ProminenceTier::Curated);
  adoptLinkPackage(activeReaderStore, linkPkg2, ProminenceTier::Curated);
  activeReaderStore.transcludeExternal(MicroversionId{}, 0, s3Scroll, 0, 84);
  activeReaderStore.save(storeReader.string());

  const auto step5Ppm = screenshotDir / "full_page_transclusion_lifecycle.ppm";
  const auto step5Png = screenshotDir / "full_page_transclusion_lifecycle.png";

  std::string cmd5 = xuduBin.string() + " --backend " + activeBackend() +
                     " --profile --fov 15 --coarse-below 0" + torrentArgs +
                     " --read " + pubAPath.string() + " --read " +
                     pubBPath.string() + " --read " + pubCPath.string() +
                     " --screenshot " + step5Ppm.string() + " " +
                     storeReader.string();

  const auto res5 = executeProcess(cmd5);
  EXPECT_EQ(res5.exitCode, 0) << "Step 5 process failed: " << res5.output;
  EXPECT_TRUE(fs::exists(step5Ppm)) << "Step 5 screenshot missing";

  const auto info5 = inspectPpm(step5Ppm);
  EXPECT_TRUE(info5.valid) << "Step 5 PPM invalid: " << info5.errorMessage;
  EXPECT_GE(info5.distinctColors, 20U);
  exportToPng(step5Ppm, step5Png);
}

TEST(E2EBinaryOrchestrationTest, fullPageManyToManyHypermeshOrchestration) {
  const auto xuduBin = findXuduBinary();
  ASSERT_TRUE(fs::exists(xuduBin)) << "xudu binary not found at " << xuduBin;

  const auto testRoot =
      fs::current_path() / "build" / "integration_workspace_hypermesh";
  const auto screenshotDir = getScreenshotDir();

  fs::remove_all(testRoot);
  fs::create_directories(testRoot);
  fs::create_directories(screenshotDir);

  const auto textA = makeFullPageProse("Primary Hypertext Architecture",
                                       "Many-to-Many Hypermesh Topology");
  const auto textB = makeFullPageProse("Comparative Geometric Analysis",
                                       "Symmetric Multi-Span Ribbons");

  const auto storePath = testRoot / "store_mesh";
  Store readerStore;
  const auto vA = readerStore.insert(MicroversionId{}, 0, textA);
  const auto vB = readerStore.insert(MicroversionId{}, 0, textB);

  // Create 4-to-4 Many-to-Many link across 4 distinct lines down each document
  Link meshLink;
  meshLink.type  = LinkType::Quotation;
  meshLink.owner = "HypermeshCurator";

  const auto vAObj = readerStore.rebuild(vA);
  const auto vBObj = readerStore.rebuild(vB);

  meshLink.left.push_back(vAObj.spansFor(60, 50).front());
  meshLink.left.push_back(vAObj.spansFor(250, 60).front());
  meshLink.left.push_back(vAObj.spansFor(500, 70).front());
  meshLink.left.push_back(vAObj.spansFor(800, 60).front());

  meshLink.right.push_back(vBObj.spansFor(80, 50).front());
  meshLink.right.push_back(vBObj.spansFor(280, 60).front());
  meshLink.right.push_back(vBObj.spansFor(530, 70).front());
  meshLink.right.push_back(vBObj.spansFor(820, 60).front());

  const auto vLinked = readerStore.addLink(vA, meshLink);
  readerStore.save(storePath.string());

  const auto ppmPath = screenshotDir / "full_page_many_to_many_hypermesh.ppm";
  const auto pngPath = screenshotDir / "full_page_many_to_many_hypermesh.png";

  std::string cmd = xuduBin.string() + " --backend " + activeBackend() +
                    " --profile --fov 15 --coarse-below 0" + " --version-id " +
                    vLinked.str() + " --alongside " + vB.str() +
                    " --screenshot " + ppmPath.string() + " " +
                    storePath.string();

  const auto res = executeProcess(cmd);
  EXPECT_EQ(res.exitCode, 0) << "Hypermesh test failed: " << res.output;
  EXPECT_TRUE(fs::exists(ppmPath)) << "Hypermesh screenshot missing";

  const auto info = inspectPpm(ppmPath);
  EXPECT_TRUE(info.valid) << "Hypermesh PPM invalid: " << info.errorMessage;
  EXPECT_GE(info.distinctColors, 25U);
  exportToPng(ppmPath, pngPath);
}

TEST(E2EBinaryOrchestrationTest,
     fullPageOneToManyAndManyToOneTopologyOrchestration) {
  const auto xuduBin = findXuduBinary();
  ASSERT_TRUE(fs::exists(xuduBin)) << "xudu binary not found at " << xuduBin;

  const auto testRoot =
      fs::current_path() / "build" / "integration_workspace_fans";
  const auto screenshotDir = getScreenshotDir();

  fs::remove_all(testRoot);
  fs::create_directories(testRoot);
  fs::create_directories(screenshotDir);

  const auto textA = makeFullPageProse("Universal Hypertext Foundations",
                                       "One-to-Many Fan Topologies");
  const auto textB = makeFullPageProse("Distributed Graph Topologies",
                                       "Many-to-One Converging Funnels");

  const auto storePath = testRoot / "store_fans";
  Store readerStore;
  const auto vA = readerStore.insert(MicroversionId{}, 0, textA);
  const auto vB = readerStore.insert(MicroversionId{}, 0, textB);

  const auto vAObj = readerStore.rebuild(vA);
  const auto vBObj = readerStore.rebuild(vB);

  // Link 1: One-to-Many (1 anchor on Doc A fanning to 3 targets on Doc B)
  Link fanLink;
  fanLink.type  = LinkType::Illustration;
  fanLink.owner = "FanCurator";
  fanLink.left.push_back(vAObj.spansFor(100, 60).front());
  fanLink.right.push_back(vBObj.spansFor(80, 50).front());
  fanLink.right.push_back(vBObj.spansFor(400, 60).front());
  fanLink.right.push_back(vBObj.spansFor(750, 70).front());

  // Link 2: Many-to-One (3 anchors on Doc A converging to 1 target on Doc B)
  Link funnelLink;
  funnelLink.type  = LinkType::Authorship;
  funnelLink.owner = "FunnelCurator";
  funnelLink.left.push_back(vAObj.spansFor(200, 50).front());
  funnelLink.left.push_back(vAObj.spansFor(500, 50).front());
  funnelLink.left.push_back(vAObj.spansFor(850, 50).front());
  funnelLink.right.push_back(vBObj.spansFor(550, 80).front());

  auto vLinked = readerStore.addLink(vA, fanLink);
  vLinked      = readerStore.addLink(vLinked, funnelLink);
  readerStore.save(storePath.string());

  const auto ppmPath = screenshotDir / "full_page_one_to_many_fan.ppm";
  const auto pngPath = screenshotDir / "full_page_one_to_many_fan.png";

  std::string cmd = xuduBin.string() + " --backend " + activeBackend() +
                    " --profile --fov 15 --coarse-below 0" + " --version-id " +
                    vLinked.str() + " --alongside " + vB.str() +
                    " --screenshot " + ppmPath.string() + " " +
                    storePath.string();

  const auto res = executeProcess(cmd);
  EXPECT_EQ(res.exitCode, 0) << "Fan test failed: " << res.output;
  EXPECT_TRUE(fs::exists(ppmPath)) << "Fan screenshot missing";

  const auto info = inspectPpm(ppmPath);
  EXPECT_TRUE(info.valid) << "Fan PPM invalid: " << info.errorMessage;
  EXPECT_GE(info.distinctColors, 25U);
  exportToPng(ppmPath, pngPath);
}

TEST(E2EBinaryOrchestrationTest, fullPageMultiTypeLinksOrchestration) {
  const auto xuduBin = findXuduBinary();
  ASSERT_TRUE(fs::exists(xuduBin)) << "xudu binary not found at " << xuduBin;

  const auto testRoot =
      fs::current_path() / "build" / "integration_workspace_types";
  const auto screenshotDir = getScreenshotDir();

  fs::remove_all(testRoot);
  fs::create_directories(testRoot);
  fs::create_directories(screenshotDir);

  const auto textA = makeFullPageProse("Comprehensive Hypermedia Foundations",
                                       "Multi-Type Link Visual Suite");
  const auto textB = makeFullPageProse("Multi-Variable Semiology Analysis",
                                       "Distinct Chromatic Hues");

  const auto storePath = testRoot / "store_types";
  Store readerStore;
  const auto vA = readerStore.insert(MicroversionId{}, 0, textA);
  const auto vB = readerStore.insert(MicroversionId{}, 0, textB);

  const auto vAObj = readerStore.rebuild(vA);
  const auto vBObj = readerStore.rebuild(vB);

  const std::vector<std::pair<LinkType, std::uint32_t>> linkDefs = {
      {LinkType::Comment, 60},       {LinkType::Illustration, 220},
      {LinkType::Disagreement, 380}, {LinkType::Quotation, 540},
      {LinkType::Authorship, 700},   {LinkType::Other, 860}};

  auto vCur = vA;
  for (const auto &[type, offset] : linkDefs) {
    Link l;
    l.type  = type;
    l.owner = "MultiTypeCurator";
    l.left.push_back(vAObj.spansFor(offset, 40).front());
    l.right.push_back(vBObj.spansFor(offset + 20, 40).front());
    vCur = readerStore.addLink(vCur, l);
  }
  readerStore.save(storePath.string());

  const auto ppmPath = screenshotDir / "full_page_multi_type_links.ppm";
  const auto pngPath = screenshotDir / "full_page_multi_type_links.png";

  std::string cmd = xuduBin.string() + " --backend " + activeBackend() +
                    " --profile --fov 15 --coarse-below 0" + " --version-id " +
                    vCur.str() + " --alongside " + vB.str() + " --screenshot " +
                    ppmPath.string() + " " + storePath.string();

  const auto res = executeProcess(cmd);
  EXPECT_EQ(res.exitCode, 0) << "Multi-type test failed: " << res.output;
  EXPECT_TRUE(fs::exists(ppmPath)) << "Multi-type screenshot missing";

  const auto info = inspectPpm(ppmPath);
  EXPECT_TRUE(info.valid) << "Multi-type PPM invalid: " << info.errorMessage;
  EXPECT_GE(info.distinctColors, 30U);
  exportToPng(ppmPath, pngPath);
}

TEST(E2EBinaryOrchestrationTest,
     fullPageThreeDocumentBypassRoutingOrchestration) {
  const auto xuduBin = findXuduBinary();
  ASSERT_TRUE(fs::exists(xuduBin)) << "xudu binary not found at " << xuduBin;

  const auto testRoot =
      fs::current_path() / "build" / "integration_workspace_3doc";
  const auto screenshotDir = getScreenshotDir();

  fs::remove_all(testRoot);
  fs::create_directories(testRoot);
  fs::create_directories(screenshotDir);

  const auto storePath = testRoot / "store_3doc";
  fs::create_directories(storePath);

  const auto s1Dir = testRoot / "sources" / "source1";
  const auto s2Dir = testRoot / "sources" / "source2";
  const auto s3Dir = testRoot / "sources" / "source3";
  fs::create_directories(s1Dir);
  fs::create_directories(s2Dir / "sub");
  fs::create_directories(s3Dir);

  const auto s1TorrentPath = s1Dir / "source1.torrent";
  {
    std::ofstream out(s1TorrentPath, std::ios::binary);
    out << xudu_test::singleFileTorrent;
  }
  {
    std::ofstream out(s1Dir / "fox.txt", std::ios::binary);
    out << xudu_test::singleFileText;
  }

  const auto s2TorrentPath = s2Dir / "source2.torrent";
  {
    std::ofstream out(s2TorrentPath, std::ios::binary);
    out << xudu_test::multiFileTorrent;
  }
  {
    std::ofstream out(s2Dir / "one.txt", std::ios::binary);
    out << xudu_test::multiFileFirst;
  }

  const auto s3TorrentPath = s3Dir / "source3.torrent";
  {
    std::ofstream out(s3TorrentPath, std::ios::binary);
    out << source3Torrent;
  }
  {
    std::ofstream out(s3Dir / "epilogue.txt", std::ios::binary);
    out << source3Text;
  }

  const std::string torrentArgs = " --torrent " + s1TorrentPath.string() +
                                  " --torrent " + s2TorrentPath.string() +
                                  " --torrent " + s3TorrentPath.string();

  const auto s1Hash   = InfoHash::fromHex(xudu_test::singleFileHash);
  const auto s1Scroll = Scroll::ofTorrentFile(s1Hash, 0, "fox.txt", 0, 62);

  const auto s2Hash   = InfoHash::fromHex(xudu_test::multiFileHash);
  const auto s2Scroll = Scroll::ofTorrentFile(s2Hash, 0, "one.txt", 0, 27);

  const auto s3Hash   = InfoHash::fromHex(source3Hash);
  const auto s3Scroll = Scroll::ofTorrentFile(s3Hash, 0, "epilogue.txt", 0, 84);

  const auto author1 = createMutableKeys();
  const auto author2 = createMutableKeys();
  const auto author3 = createMutableKeys();

  Store s1, s2, s3;
  auto v1 = s1.transcludeExternal(MicroversionId{}, 0, s1Scroll, 0, 62);
  v1      = s1.transcludeExternal(v1, 62, s3Scroll, 0, 84);

  auto v2 = s2.transcludeExternal(MicroversionId{}, 0, s2Scroll, 0, 27);
  v2      = s2.transcludeExternal(v2, 27, s1Scroll, 0, 62);

  auto v3 = s3.transcludeExternal(MicroversionId{}, 0, s3Scroll, 0, 84);
  v3      = s3.transcludeExternal(v3, 84, s2Scroll, 0, 27);

  auto pub1 = publish(s1, v1, author1, "p1", "Doc 1", 1, 1700000001, nullptr);
  pub1.signature = signMutableItem(publicationSigningBuffer(pub1), author1);
  auto pub2 = publish(s2, v2, author2, "p2", "Doc 2", 1, 1700000002, nullptr);
  pub2.signature = signMutableItem(publicationSigningBuffer(pub2), author2);
  auto pub3 = publish(s3, v3, author3, "p3", "Doc 3", 1, 1700000003, nullptr);
  pub3.signature = signMutableItem(publicationSigningBuffer(pub3), author3);

  const auto pub1Path = testRoot / "p1.manifest";
  const auto pub2Path = testRoot / "p2.manifest";
  const auto pub3Path = testRoot / "p3.manifest";
  {
    std::ofstream out(pub1Path, std::ios::binary);
    out << encodePublication(pub1);
  }
  {
    std::ofstream out(pub2Path, std::ios::binary);
    out << encodePublication(pub2);
  }
  {
    std::ofstream out(pub3Path, std::ios::binary);
    out << encodePublication(pub3);
  }

  const auto curator   = createMutableKeys();
  const std::string k1 = scrollKey(s1Scroll);
  const std::string k2 = scrollKey(s2Scroll);
  const std::string k3 = scrollKey(s3Scroll);

  std::vector<GlobalLink> links;

  // Link 1: Doc 1 <-> Doc 2 (Adjacent, Foreground Z = 0)
  GlobalLink l12;
  l12.type  = LinkType::Comment;
  l12.owner = "LinkDoc1Doc2";
  l12.left.push_back(GlobalSpan{k1, 0, 25});
  l12.right.push_back(GlobalSpan{k2, 0, 20});
  links.push_back(l12);

  // Link 2: Doc 2 <-> Doc 3 (Adjacent, Foreground Z = 0)
  GlobalLink l23;
  l23.type  = LinkType::Illustration;
  l23.owner = "LinkDoc2Doc3";
  l23.left.push_back(GlobalSpan{k2, 10, 15});
  l23.right.push_back(GlobalSpan{k3, 0, 30});
  links.push_back(l23);

  // Link 3: Doc 1 <-> Doc 3 (Non-adjacent, Background Bypass Z = -20)
  GlobalLink l13;
  l13.type  = LinkType::Disagreement;
  l13.owner = "BypassLinkDoc1Doc3";
  l13.left.push_back(GlobalSpan{k1, 30, 25});
  l13.right.push_back(GlobalSpan{k3, 35, 35});
  links.push_back(l13);

  std::map<std::string, Scroll> pkgScrolls;
  pkgScrolls.insert_or_assign(k1, s1Scroll);
  pkgScrolls.insert_or_assign(k2, s2Scroll);
  pkgScrolls.insert_or_assign(k3, s3Scroll);

  const auto pkg =
      publishLinkPackage(curator, "pkg_3doc", "3DocRoutingNetwork", 1,
                         1700000010, std::move(links), std::move(pkgScrolls));

  Store readerStore;
  adopt(readerStore, pub1);
  adopt(readerStore, pub2);
  adopt(readerStore, pub3);
  adoptLinkPackage(readerStore, pkg, ProminenceTier::Curated);
  readerStore.save(storePath.string());

  const auto ppmPath = screenshotDir / "full_page_three_doc_depth_routing.ppm";
  const auto pngPath = screenshotDir / "full_page_three_doc_depth_routing.png";

  std::string cmd = xuduBin.string() + " --backend " + activeBackend() +
                    " --profile --fov 18 --coarse-below 0" + torrentArgs +
                    " --read " + pub1Path.string() + " --read " +
                    pub2Path.string() + " --read " + pub3Path.string() +
                    " --screenshot " + ppmPath.string() + " " +
                    storePath.string();

  const auto res = executeProcess(cmd);
  EXPECT_EQ(res.exitCode, 0) << "3-doc test failed: " << res.output;
  EXPECT_TRUE(fs::exists(ppmPath)) << "3-doc screenshot missing";

  const auto info = inspectPpm(ppmPath);
  EXPECT_TRUE(info.valid) << "3-doc PPM invalid: " << info.errorMessage;
  EXPECT_GE(info.distinctColors, 25U);
  exportToPng(ppmPath, pngPath);
}

TEST(E2EBinaryOrchestrationTest,
     extremeFramingSymmetricMultiPageOrchestration) {
  const auto xuduBin = findXuduBinary();
  ASSERT_TRUE(fs::exists(xuduBin)) << "xudu binary not found at " << xuduBin;

  const auto testRoot =
      fs::current_path() / "build" / "integration_workspace_symm_multipage";
  const auto screenshotDir = getScreenshotDir();

  fs::remove_all(testRoot);
  fs::create_directories(testRoot);
  fs::create_directories(screenshotDir);

  const std::vector<std::size_t> pageCounts = {3, 5, 8, 10};

  for (const auto pages : pageCounts) {
    const auto textA =
        makeMultiPageText(pages, "Doc_A_" + std::to_string(pages) + "P");
    const auto textB =
        makeMultiPageText(pages, "Doc_B_" + std::to_string(pages) + "P");

    const auto storePath = testRoot / ("store_symm_" + std::to_string(pages));
    Store readerStore;
    const auto vA = readerStore.insert(MicroversionId{}, 0, textA);
    const auto vB = readerStore.insert(MicroversionId{}, 0, textB);

    const auto vAObj = readerStore.rebuild(vA);
    const auto vBObj = readerStore.rebuild(vB);

    // Many-to-Many link from Page 1 Sentence 1 to Page N Last Sentence
    Link extremeLink;
    if (pages == 3) {
      extremeLink.type = LinkType::Illustration;
    } else if (pages == 5) {
      extremeLink.type = LinkType::Comment;
    } else if (pages == 8) {
      extremeLink.type = LinkType::Disagreement;
    } else {
      extremeLink.type = LinkType::Authorship;
    }
    extremeLink.owner = "ExtremeSymmetricCurator";

    // Doc A: Page 1 sentence 1 and Page N last sentence
    extremeLink.left.push_back(vAObj.spansFor(35, 70).front());
    extremeLink.left.push_back(
        vAObj.spansFor(static_cast<std::uint32_t>(textA.size() - 80), 70)
            .front());

    // Doc B: Page 1 sentence 1 and Page N last sentence
    extremeLink.right.push_back(vBObj.spansFor(35, 70).front());
    extremeLink.right.push_back(
        vBObj.spansFor(static_cast<std::uint32_t>(textB.size() - 80), 70)
            .front());

    const auto vLinked = readerStore.addLink(vA, extremeLink);
    readerStore.save(storePath.string());

    const std::string filename = "extreme_framing_" + std::to_string(pages) +
                                 "x" + std::to_string(pages) + "_pages";
    const auto ppmPath         = screenshotDir / (filename + ".ppm");
    const auto pngPath         = screenshotDir / (filename + ".png");

    std::string cmd = xuduBin.string() + " --backend " + activeBackend() +
                      " --profile --fov 15 --coarse-below 0" +
                      " --version-id " + vLinked.str() + " --alongside " +
                      vB.str() + " --screenshot " + ppmPath.string() + " " +
                      storePath.string();

    const auto res = executeProcess(cmd);
    EXPECT_EQ(res.exitCode, 0)
        << "Symmetric " << pages << "-page test failed: " << res.output;
    EXPECT_TRUE(fs::exists(ppmPath))
        << "Symmetric " << pages << "-page screenshot missing";

    const auto info = inspectPpm(ppmPath);
    EXPECT_TRUE(info.valid)
        << "Symmetric " << pages << "-page PPM invalid: " << info.errorMessage;
    EXPECT_GE(info.distinctColors, 20U);
    exportToPng(ppmPath, pngPath);
  }
}

TEST(E2EBinaryOrchestrationTest,
     extremeFramingAsymmetricMultiPageOrchestration) {
  const auto xuduBin = findXuduBinary();
  ASSERT_TRUE(fs::exists(xuduBin)) << "xudu binary not found at " << xuduBin;

  const auto testRoot =
      fs::current_path() / "build" / "integration_workspace_asymm_multipage";
  const auto screenshotDir = getScreenshotDir();

  fs::remove_all(testRoot);
  fs::create_directories(testRoot);
  fs::create_directories(screenshotDir);

  const std::vector<std::pair<std::size_t, std::size_t>> pairs = {{3, 8},
                                                                  {5, 10}};

  for (const auto &[pagesA, pagesB] : pairs) {
    const auto textA =
        makeMultiPageText(pagesA, "Doc_A_" + std::to_string(pagesA) + "P");
    const auto textB =
        makeMultiPageText(pagesB, "Doc_B_" + std::to_string(pagesB) + "P");

    const auto storePath = testRoot / ("store_asymm_" + std::to_string(pagesA) +
                                       "x" + std::to_string(pagesB));
    Store readerStore;
    const auto vA = readerStore.insert(MicroversionId{}, 0, textA);
    const auto vB = readerStore.insert(MicroversionId{}, 0, textB);

    const auto vAObj = readerStore.rebuild(vA);
    const auto vBObj = readerStore.rebuild(vB);

    // Many-to-Many link from Page 1 Sentence 1 to Last Page Last Sentence
    // across asymmetric heights
    Link asymmLink;
    asymmLink.type  = LinkType::Illustration;
    asymmLink.owner = "AsymmetricCurator";

    asymmLink.left.push_back(vAObj.spansFor(35, 70).front());
    asymmLink.left.push_back(
        vAObj.spansFor(static_cast<std::uint32_t>(textA.size() - 80), 70)
            .front());

    asymmLink.right.push_back(vBObj.spansFor(35, 70).front());
    asymmLink.right.push_back(
        vBObj.spansFor(static_cast<std::uint32_t>(textB.size() - 80), 70)
            .front());

    const auto vLinked = readerStore.addLink(vA, asymmLink);
    readerStore.save(storePath.string());

    const std::string filename = "extreme_framing_" + std::to_string(pagesA) +
                                 "x" + std::to_string(pagesB) + "_asymmetric";
    const auto ppmPath         = screenshotDir / (filename + ".ppm");
    const auto pngPath         = screenshotDir / (filename + ".png");

    std::string cmd = xuduBin.string() + " --backend " + activeBackend() +
                      " --profile --fov 15 --coarse-below 0" +
                      " --version-id " + vLinked.str() + " --alongside " +
                      vB.str() + " --screenshot " + ppmPath.string() + " " +
                      storePath.string();

    const auto res = executeProcess(cmd);
    std::cout << "ASYMM (" << pagesA << "x" << pagesB << ") OUTPUT:\n"
              << res.output << "\n";
    EXPECT_EQ(res.exitCode, 0) << "Asymmetric " << pagesA << "x" << pagesB
                               << "-page test failed: " << res.output;
    EXPECT_TRUE(fs::exists(ppmPath)) << "Asymmetric " << pagesA << "x" << pagesB
                                     << "-page screenshot missing";

    const auto info = inspectPpm(ppmPath);
    EXPECT_TRUE(info.valid) << "Asymmetric " << pagesA << "x" << pagesB
                            << "-page PPM invalid: " << info.errorMessage;
    EXPECT_GE(info.distinctColors, 20U);
    exportToPng(ppmPath, pngPath);
  }
}

TEST(E2EBinaryOrchestrationTest,
     largeMultipageBackgroundCorpusWithForegroundFlyInOrchestration) {
  const auto xuduBin = findXuduBinary();
  ASSERT_TRUE(fs::exists(xuduBin)) << "xudu binary not found at " << xuduBin;

  const auto testRoot =
      fs::current_path() / "build" / "integration_workspace_flyin";
  const auto screenshotDir = getScreenshotDir();

  fs::remove_all(testRoot);
  fs::create_directories(testRoot);
  fs::create_directories(screenshotDir);

  // The exact sentences the corpus opens and closes on, reused verbatim as
  // the text of two small standalone "page" documents below. Reused rather
  // than transcluded (a virtual copy sharing the corpus's own primedia
  // address) on purpose: a link built on a shared address would match
  // wherever that address appears, including the corpus itself once it is
  // open, and sworph would be just as willing to bring the whole corpus
  // forward as the small excerpt -- exactly what putting the corpus in the
  // background is meant to avoid. Independent text keeps the link aimed at
  // only the small page, while still reading, word for word, as the same
  // page a reader would find inside the corpus.
  const std::string openingSentence =
      "This exact opening sentence also begins the corpus shown behind it, "
      "verbatim.";
  const std::string closingSentence =
      "This exact closing sentence also ends the corpus shown behind it, "
      "verbatim.";

  // Foreground: a short thesis whose opening and closing claims each cite
  // one of those two corpus pages.
  const auto text1 = makeFullPageProse("Primary Thesis Investigation",
                                       "Corpus Cross-Reference Study");

  // Background: the corpus itself, large, opening and closing on the same
  // two sentences -- shown in full for spatial context (where do these two
  // excerpts actually sit in it), but never itself the target of a link,
  // so it never becomes the far end an alignment brings forward.
  const auto corpusBody = makeMultiPageText(8, "Universal_Encyclopedic_Corpus");
  const auto corpusText =
      openingSentence + "\n\n" + corpusBody + "\n\n" + closingSentence;

  const auto storePath = testRoot / "store_flyin";
  Store readerStore;
  const auto vThesis = readerStore.insert(MicroversionId{}, 0, text1);
  const auto vCorpus = readerStore.insert(MicroversionId{}, 0, corpusText);
  const auto vPageTop =
      readerStore.insert(MicroversionId{}, 0, openingSentence);
  const auto vPageBottom =
      readerStore.insert(MicroversionId{}, 0, closingSentence);

  const auto thesisObj     = readerStore.rebuild(vThesis);
  const auto pageTopObj    = readerStore.rebuild(vPageTop);
  const auto pageBottomObj = readerStore.rebuild(vPageBottom);

  // The thesis's own opening claim and closing claim -- the same
  // "top sentence / bottom sentence" shape the asymmetric extreme-framing
  // suite links between two whole documents, here linking instead to the
  // two small pages pulled out of the corpus.
  const std::string thesisTopPhrase = "In the design of universal hypermedia";
  const std::string thesisBottomPhrase = "Hypertime branches preserve";
  const auto thesisTopAt               = text1.find(thesisTopPhrase);
  ASSERT_NE(thesisTopAt, std::string::npos);
  const auto thesisBottomAt = text1.find(thesisBottomPhrase);
  ASSERT_NE(thesisBottomAt, std::string::npos);

  Link topLink;
  topLink.type  = LinkType::Quotation;
  topLink.owner = "CorpusFlyInCurator";
  topLink.left.push_back(
      thesisObj
          .spansFor(static_cast<std::uint32_t>(thesisTopAt),
                    static_cast<std::uint32_t>(thesisTopPhrase.size()))
          .front());
  topLink.right.push_back(
      pageTopObj.spansFor(0, static_cast<std::uint32_t>(openingSentence.size()))
          .front());
  auto vLinked = readerStore.addLink(vThesis, topLink);

  Link bottomLink;
  bottomLink.type  = LinkType::Quotation;
  bottomLink.owner = "CorpusFlyInCurator";
  bottomLink.left.push_back(
      thesisObj
          .spansFor(static_cast<std::uint32_t>(thesisBottomAt),
                    static_cast<std::uint32_t>(thesisBottomPhrase.size()))
          .front());
  bottomLink.right.push_back(
      pageBottomObj
          .spansFor(0, static_cast<std::uint32_t>(closingSentence.size()))
          .front());
  vLinked = readerStore.addLink(vLinked, bottomLink);

  readerStore.save(storePath.string());

  const auto ppmPath = screenshotDir / "large_multipage_background_flyin.ppm";
  const auto pngPath = screenshotDir / "large_multipage_background_flyin.png";

  // Foreground: the thesis. Background: the corpus, and the two pages --
  // opened alongside it at the same depth, so each starts out part of the
  // unread background and sworphs forward into the foreground row only
  // once its link to the thesis comes into view.
  std::string cmd = xuduBin.string() + " --backend " + activeBackend() +
                    " --profile --fov 15 --coarse-below 0" + " --version-id " +
                    vLinked.str() + " --background " + vCorpus.str() +
                    " --background " + vPageTop.str() + " --background " +
                    vPageBottom.str() + " --screenshot " + ppmPath.string() +
                    " " + storePath.string();

  const auto res = executeProcess(cmd);
  EXPECT_EQ(res.exitCode, 0) << "Fly-in test failed: " << res.output;
  EXPECT_TRUE(fs::exists(ppmPath)) << "Fly-in screenshot missing";
  // LinkBeams::align()'s trace: proof the two pages actually flew forward
  // rather than merely being linked.
  EXPECT_NE(res.output.find("aligns centroid"), std::string::npos)
      << "background flyin never brought a linked page forward:\n"
      << res.output;

  const auto info = inspectPpm(ppmPath);
  EXPECT_TRUE(info.valid) << "Fly-in PPM invalid: " << info.errorMessage;
  EXPECT_GE(info.distinctColors, 20U);
  exportToPng(ppmPath, pngPath);
}

TEST(E2EBinaryOrchestrationTest,
     forcedPageBreakSplitsAPageThatWouldOtherwiseFit) {
  const auto xuduBin = findXuduBinary();
  ASSERT_TRUE(fs::exists(xuduBin)) << "xudu binary not found at " << xuduBin;

  const auto testRoot =
      fs::current_path() / "build" / "integration_workspace_pagebreak";
  const auto screenshotDir = getScreenshotDir();
  fs::remove_all(testRoot);
  fs::create_directories(testRoot);
  fs::create_directories(screenshotDir);

  const std::string firstParagraph = "First paragraph, short and plain.\n\n";
  const std::string secondParagraph =
      "Second paragraph, also short and plain.\n";
  const std::string text = firstParagraph + secondParagraph;

  Store store;
  const auto whole = store.insert(MicroversionId{}, 0, text);
  // A break exactly between the two paragraphs: nothing about laying out
  // twenty-odd words should ever need a second physical page on its own,
  // which is the point -- the only reason one exists here is the break.
  const auto broken = store.insertBreak(
      whole, static_cast<std::uint32_t>(firstParagraph.size()));
  const auto storePath = testRoot / "store";
  store.save(storePath.string());

  const auto runAndCountPages = [&](const MicroversionId &version,
                                    const std::string &label) {
    const auto ppmPath    = screenshotDir / (label + ".ppm");
    const std::string cmd = xuduBin.string() + " --backend " + activeBackend() +
                            " --profile --version-id " + version.str() +
                            " --screenshot " + ppmPath.string() + " " +
                            storePath.string();
    const auto res        = executeProcess(cmd);
    EXPECT_EQ(res.exitCode, 0) << label << " failed: " << res.output;
    const auto marker = std::string("total pages: ");
    const auto at     = res.output.find(marker);
    EXPECT_NE(at, std::string::npos) << label << ": no page count in output:\n"
                                     << res.output;
    return at == std::string::npos
               ? -1
               : std::atoi(res.output.c_str() + at + marker.size());
  };

  EXPECT_EQ(runAndCountPages(whole, "pagebreak_unbroken"), 1)
      << "the unbroken text was expected to fit on one physical page";
  EXPECT_EQ(runAndCountPages(broken, "pagebreak_forced"), 2)
      << "a forced break between the two paragraphs should split them onto "
         "separate physical pages";
}

TEST(E2EBinaryOrchestrationTest, typeWithDecorationsRecordsAFormatLink) {
  const auto xuduBin = findXuduBinary();
  ASSERT_TRUE(fs::exists(xuduBin)) << "xudu binary not found at " << xuduBin;

  const auto testRoot =
      fs::current_path() / "build" / "integration_workspace_type_decorated";
  const auto screenshotDir = getScreenshotDir();
  fs::remove_all(testRoot);
  fs::create_directories(testRoot);
  fs::create_directories(screenshotDir);

  Store store;
  const auto whole     = store.insert(MicroversionId{}, 0, "hello world");
  const auto storePath = testRoot / "store";
  store.save(storePath.string());

  const auto ppmPath = screenshotDir / "type_decorated.ppm";
  // The click lands at the middle of the screenshot, which for a single
  // short line centred in frame resolves to a caret offset inside the text
  // -- exactly where is read back from "caret ...: doc 0 offset N" below
  // rather than assumed, so this does not silently start asserting against
  // the wrong byte range if rendering ever centres the line differently.
  std::string cmd = xuduBin.string() + " --backend " + activeBackend() +
                    " --profile --fov 15 --version-id " + whole.str() +
                    " --click 400,300 --type '[bold,italic]MARKERWORD' "
                    "--do save --screenshot " +
                    ppmPath.string() + " " + storePath.string();

  const auto res = executeProcess(cmd);
  EXPECT_EQ(res.exitCode, 0) << "type-decorated test failed: " << res.output;

  const auto caretMarker = std::string("offset ");
  const auto caretAt     = res.output.find(caretMarker);
  ASSERT_NE(caretAt, std::string::npos)
      << "--click never resolved to a caret offset:\n"
      << res.output;
  const auto insertedAt = static_cast<std::uint32_t>(
      std::atoi(res.output.c_str() + caretAt + caretMarker.size()));

  const std::string marker = "MARKERWORD";

  Store reloaded;
  reloaded.load(storePath.string());
  const auto finalVersion = reloaded.latest();
  const auto finalText    = reloaded.textOf(finalVersion);
  ASSERT_NE(finalText.find(marker), std::string::npos)
      << "typed text never made it into the saved store: " << finalText;

  const auto typedSpans =
      reloaded.rebuild(finalVersion).spansFor(insertedAt, marker.size());
  ASSERT_FALSE(typedSpans.empty());

  std::set<xudu::FormatAttribute> found;
  for (const auto &span : typedSpans) {
    for (const auto *const link : reloaded.linksTouching(span)) {
      if (const auto attribute = reloaded.formatAttributeOf(*link)) {
        found.insert(*attribute);
      }
    }
  }
  EXPECT_THAT(found,
              testing::UnorderedElementsAre(xudu::FormatAttribute::Bold,
                                            xudu::FormatAttribute::Italic))
      << "--type '[bold,italic]...' should have recorded both as Format "
         "links over the typed text";
}

} // namespace
