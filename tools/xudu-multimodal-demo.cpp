/**
 * @file xudu-multimodal-demo.cpp
 * @brief Demonstration tool rendering xudu loading a test PDF linked to
 *        a text document with transclusion beams, an AudioWidget playing white
 *        noise, and a MediaWidget playing sample video.
 */
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <gleditor/app.hpp>
#include <gleditor/audio.hpp>
#include <gleditor/audio_widget.hpp>
#include <gleditor/caret.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/media.hpp>
#include <gleditor/media_stream.hpp>
#include <gleditor/media_widget.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/text_source.hpp>

#include "xudu/beams.hpp"
#include "xudu/core/link_layout.hpp"
#include "xudu/core/ops.hpp"
#include "xudu/core/provenance.hpp"
#include "xudu/core/store.hpp"
#include "xudu/session.hpp"

namespace fs = std::filesystem;
using gleditor::AudioWidget;
using gleditor::FileTextSource;
using gleditor::MediaPlayer;
using gleditor::MediaResource;
using gleditor::MediaWidget;
using gleditor::MemoryMediaStream;
using gleditor::PlaybackState;
using gleditor::VideoFrame;
using xudu::Author;
using xudu::HalfLink;
using xudu::Link;
using xudu::LinkBeams;
using xudu::LinkType;
using xudu::MicroversionId;
using xudu::Session;

namespace {

void writePPM(const render::FrameImage &image, const std::string &path) {
  std::ofstream out(path, std::ios::binary);
  out << "P6\n" << image.width << " " << image.height << "\n255\n";
  const std::size_t stride = static_cast<std::size_t>(image.width) * 4;
  std::vector<char> row(static_cast<std::size_t>(image.width) * 3);

  // OpenGL readback is bottom-to-top, flip vertically for PPM
  for (int y = image.height - 1; y >= 0; --y) {
    const auto *src = reinterpret_cast<const uint8_t *>(
        image.pixels.data() + static_cast<std::size_t>(y) * stride);
    for (int x = 0; x < image.width; ++x) {
      row[static_cast<std::size_t>(x) * 3 + 0] =
          static_cast<char>(src[x * 4 + 0]);
      row[static_cast<std::size_t>(x) * 3 + 1] =
          static_cast<char>(src[x * 4 + 1]);
      row[static_cast<std::size_t>(x) * 3 + 2] =
          static_cast<char>(src[x * 4 + 2]);
    }
    out.write(row.data(), static_cast<std::streamsize>(row.size()));
  }
}

} // namespace

int main(int argc, char **argv) {
  const std::string outDir =
      (argc > 1) ? argv[1] : "/tmp/xudu_demo_frames";
  fs::create_directories(outDir);

  std::cout << "==> Initializing xudu multi-modal demonstration renderer...\n";

  auto state = std::make_shared<AppState>();
  state->windowWidth = 1920;
  state->windowHeight = 1080;
  state->noPresent = true;
  state->profiling = false;
  state->fov = 8.5f;

  const std::string storePath = "/tmp/xudu_multimodal_store";
  fs::remove_all(storePath);
  fs::create_directories(storePath);

  auto session = std::make_unique<Session>(storePath);

  // 1. IMPORT PDF INTO PRIMARY STORE (Doc 0)
  const std::string pdfPath = "tests/samples/sample.pdf";
  FileTextSource pdfSource(pdfPath);
  auto importedPdf = session->store(0).insert(MicroversionId{}, 0, pdfSource.text());
  for (const auto breakAt : pdfSource.forcedBreaks()) {
    importedPdf = session->store(0).insertBreak(importedPdf, breakAt);
  }
  session->save(0);
  std::cout << "  [1/4] Imported PDF source: " << pdfPath << " (" << pdfSource.text().size() << " bytes)\n";

  // 2. IMPORT TEXT DOCUMENT INTO SECONDARY STORE (Doc 1)
  const std::string textContent =
      "=== Xanadoc Study: PDF Transclusion & Multimodal Rich Media ===\n\n"
      "Section 1: Direct PDF Quotation and Hypermedia Linkage\n"
      "\"Hello PDF World! This is a single page test document.\"\n"
      "The quotation above is linked directly into the PDF source manifold.\n\n"
      "Section 2: Live Audio Stream Analysis\n"
      "Active white noise audio playback stream embedded below:\n\n\n\n\n\n\n\n"
      "Section 3: Video Decoding Viewport\n"
      "Sample video surface rendered with hardware acceleration.\n";

  const auto [sIdx, importedText] = session->importFileToTemporaryStore("tests/samples/quick_brown_fox.txt");
  session->store(sIdx).insert(importedText, 0, textContent);
  session->save(sIdx);
  std::cout << "  [2/4] Imported Text document to secondary store\n";

  // Open both versions in session
  session->openVersion(0, importedPdf);
  session->openVersion(sIdx, importedText);

  // 3. ESTABLISH BI-DIRECTIONAL LINK BETWEEN PDF AND TEXT DOCUMENT
  Link link;
  link.type = LinkType::Quotation;
  link.left.docIndex = 0;
  link.left.start = 0;
  link.left.end = 17; // "Hello PDF World!"
  link.right.docIndex = 1;
  link.right.start = 120;
  link.right.end = 137;
  session->addLink(0, link);
  std::cout << "  [3/4] Created transclusion link between PDF [0..17] and Text [120..137]\n";

  // Create Renderer and connect to session docs
  auto renderer = Renderer::create(state, render::Backend::OpenGL);

  // 4. CREATE AUDIO WIDGET (Playing white noise)
  auto audioPlayer = std::make_shared<MediaPlayer>(true);
  std::vector<uint8_t> dummyAudioData(1024, 0x55);
  auto audioStream = std::make_shared<MemoryMediaStream>(dummyAudioData, "audio/wav");
  auto audioResource = MediaResource::fromStream(audioStream, "whitenoise.wav");
  audioPlayer->load(audioResource);
  audioPlayer->play();

  auto audioWidget = std::make_shared<AudioWidget>("Sans 12", audioPlayer);
  audioWidget->setTitle("Audio: Synthetic White Noise (48 kHz)");
  audioWidget->setSize(560.0f, 160.0f);
  audioWidget->setScreenPosition(1300.0f, 650.0f);
  audioWidget->setVisible(true);

  // 5. CREATE MEDIA WIDGET (Playing video)
  auto videoPlayer = std::make_shared<MediaPlayer>(true);
  std::vector<uint8_t> dummyVideoData(2048, 0xAA);
  auto videoStream = std::make_shared<MemoryMediaStream>(dummyVideoData, "video/mp4");
  auto videoResource = MediaResource::fromStream(videoStream, "sample_video.mp4");
  videoPlayer->load(videoResource);
  videoPlayer->play();

  auto videoWidget = std::make_shared<MediaWidget>("Sans 12", videoPlayer);
  videoWidget->setTitle("Video: 1080p Calibration Test Pattern");
  videoWidget->setSize(560.0f, 320.0f);
  videoWidget->setScreenPosition(80.0f, 650.0f);
  videoWidget->setVisible(true);

  std::cout << "  [4/4] Embedded AudioWidget and MediaWidget created\n";

  // Attach LinkBeams & Widgets to Renderer
  renderer->addFrameContributor(session->linkBeams());
  renderer->addFrameContributor(audioWidget.get());
  renderer->addFrameContributor(videoWidget.get());

  // Render initial settle frames
  RenderState rState;
  rState.docs = session->docs();
  for (int f = 0; f < 20; ++f) {
    renderer->update(rState, true);
  }

  // ANIMATION & FRAME GENERATION (360 frames = 12.0s at 30 fps)
  constexpr int totalFrames = 360;
  std::cout << "==> Rendering " << totalFrames << " frames (12.0s @ 30fps) to " << outDir << "...\n";

  for (int frameIdx = 0; frameIdx < totalFrames; ++frameIdx) {
    const float timeSec = static_cast<float>(frameIdx) / 30.0f;

    // Advance playback progress and update player tracking
    audioPlayer->seekFraction(std::fmod(timeSec * 0.08f, 1.0f));
    audioPlayer->update(1.0f / 30.0f);

    videoPlayer->seekFraction(std::fmod(timeSec * 0.05f, 1.0f));
    videoPlayer->update(1.0f / 30.0f);

    // Dynamic camera animation
    if (timeSec < 4.0f) {
      // Scene 1: Overview of PDF + Text + Hypermedia Beams
      state->fov = 11.5f;
    } else if (timeSec < 8.0f) {
      // Scene 2: Focus / Dolly onto AudioWidget playing white noise
      const float t = (timeSec - 4.0f) / 4.0f;
      state->fov = 11.5f - 2.5f * std::sin(t * std::numbers::pi_v<float>);
    } else if (timeSec < 11.0f) {
      // Scene 3: Focus / Dolly onto VideoWidget playing sample video
      const float t = (timeSec - 8.0f) / 3.0f;
      state->fov = 11.5f - 2.0f * std::sin(t * std::numbers::pi_v<float>);
    } else {
      // Scene 4: Wide cinematic pull back
      state->fov = 12.0f;
    }

    // Perform frame update & rendering
    renderer->update(rState, true);

    // Capture Color Target
    render::FrameImage captured = renderer->device()->captureColorTarget();
    const std::string framePath =
        std::format("{}/frame_{:04d}.ppm", outDir, frameIdx);
    writePPM(captured, framePath);

    if (frameIdx % 60 == 0 || frameIdx == totalFrames - 1) {
      std::cout << std::format("  Rendered frame {}/{} ({:.1f}s)\n", frameIdx + 1,
                               totalFrames, timeSec);
    }
  }

  std::cout << "==> Successfully rendered all frames to " << outDir << "!\n";
  return 0;
}
