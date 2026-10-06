/**
 * @file ui-text-baseline.cpp
 * @brief Synchronized headless Canvas frame and text-overflow baseline.
 *
 * The immediate cases rebuild the same 36-label overlay every frame, as the
 * existing overlays do. The retained case draws exactly the same geometry
 * without rebuilding, exposing the zero-shaping target for later UI work.
 * Frame times include device completion, rather than only CPU submission.
 */
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>

#include <gleditor/canvas.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/logging.hpp>
#include <gleditor/paths.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render/shader_source.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/sdl_wrap.hpp>
#include <gleditor/text/diagnostics.hpp>
#include <gleditor/text/font.hpp>

namespace {

using Clock                        = std::chrono::steady_clock;
constexpr int screenWidth          = 1280;
constexpr int screenHeight         = 800;
constexpr int columns              = 3;
constexpr int rows                 = 12;
constexpr int labelCount           = columns * rows;
constexpr float margin             = 24.0F;
constexpr float gap                = 12.0F;
constexpr float padding            = 6.0F;
constexpr std::uint32_t rowColour  = 0x283344FFU;
constexpr std::uint32_t textColour = 0xE6EDF3FFU;

struct Options {
  render::Backend backend{render::Backend::OpenGL};
  std::string fixture{"tests/samples/ui/long-labels.tsv"};
  std::string font{"Sans 10"};
  std::string screenshotPrefix;
  int frames{100};
  int warmup{10};
};

struct OverflowCounts {
  int width{};
  int height{};
};

struct Samples {
  std::vector<double> frameMs;
  std::vector<std::uint64_t> layouts;
  std::vector<std::uint64_t> harfbuzz;
  std::vector<std::uint64_t> fallbacks;
  std::vector<std::uint64_t> bytes;
};

int positiveInt(const std::string_view value) {
  int result{};
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
      result <= 0) {
    throw std::invalid_argument(
        "frame and warmup counts must be positive integers");
  }
  return result;
}

std::optional<Options> parseOptions(const int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument == "--help") {
      std::cout
          << "Usage: ui-text-baseline [--backend opengl|opengles|vulkan] "
             "[--frames N] [--warmup N] [--font 'Sans 10'] "
             "[--fixture tests/samples/ui/long-labels.tsv] "
             "[--screenshot-prefix PATH]\n"
             "Run with SDL_VIDEODRIVER=offscreen or under Xvfb with "
             "SDL_VIDEODRIVER=x11. Windows remain hidden; GL/GLES presentation "
             "is disabled. Vulkan returns swapchain images via present. "
             "Times include GPU completion; captures are "
             "outside measured frames. An uncompiled backend exits 77.\n";
      return std::nullopt;
    }
    if (index + 1 >= argc) {
      throw std::invalid_argument("missing value for " + std::string(argument));
    }
    const std::string value{argv[++index]};
    if (argument == "--backend") {
      options.backend = render::backendFromName(value);
    } else if (argument == "--frames") {
      options.frames = positiveInt(value);
    } else if (argument == "--warmup") {
      options.warmup = positiveInt(value);
    } else if (argument == "--font") {
      options.font = value;
    } else if (argument == "--fixture") {
      options.fixture = value;
    } else if (argument == "--screenshot-prefix") {
      options.screenshotPrefix = value;
    } else {
      throw std::invalid_argument("unknown argument " + std::string(argument));
    }
  }
  return options;
}

std::vector<std::string> readLabels(const std::string &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot open label fixture " + path);
  }
  std::vector<std::string> labels;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const auto separator = line.find('\t');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 == line.size()) {
      throw std::runtime_error("expected identity<TAB>text in label fixture");
    }
    labels.push_back(line.substr(separator + 1));
  }
  if (input.bad() || labels.empty()) {
    throw std::runtime_error("label fixture unreadable or empty");
  }
  return labels;
}

render::PipelineDesc glyphPipeline() {
  const auto shaders = gleditor::assetPath("shaders");
  render::PipelineDesc descriptor;
  descriptor.name = "ui-text-baseline";
  descriptor.vertexSource =
      render::readShaderBody(shaders + "/glyph.vert.glsl");
  descriptor.fragmentSource =
      render::readShaderBody(shaders + "/glyph.frag.glsl");
  descriptor.spirvDir = shaders + "/vulkan";
  descriptor.layout   = Doc::vertexLayout();
  return descriptor;
}

OverflowCounts
buildScene(gleditor::Canvas &canvas, RenderState &state,
           const std::vector<std::string> &labels, const float lineHeight,
           const bool widthLimited, const bool withText = true,
           const std::optional<gleditor::text::Overflow> boxed = std::nullopt) {
  canvas.clear();
  constexpr float columnWidth =
      (static_cast<float>(screenWidth) - 2.0F * margin -
       static_cast<float>(columns - 1) * gap) /
      static_cast<float>(columns);
  constexpr float textWidth = columnWidth - 2.0F * padding;
  const float rowHeight     = lineHeight + 2.0F * padding;
  canvas.setTextWidthLimit(widthLimited ? static_cast<int>(textWidth) : 0);
  OverflowCounts overflows;
  for (int index = 0; index < labelCount; ++index) {
    const auto column = index / rows;
    const auto row    = index % rows;
    const float left =
        margin + static_cast<float>(column) * (columnWidth + gap);
    const float top = static_cast<float>(screenHeight) - margin -
                      static_cast<float>(row) * (rowHeight + gap);
    canvas.setTag(render::tagKindOverlay, static_cast<std::uint32_t>(index));
    canvas.addRect(left, top - rowHeight, columnWidth, rowHeight, rowColour);
    if (!withText) {
      continue;
    }
    canvas.setTextBounds(
        gleditor::ui::TextBounds{.left   = left + padding,
                                 .bottom = top - padding - lineHeight,
                                 .width  = textWidth,
                                 .height = lineHeight});
    const auto &label = labels[static_cast<std::size_t>(index) % labels.size()];
    gleditor::TextMetrics metrics;
    if (boxed) {
      const auto result = canvas.addText(
          state,
          gleditor::ui::Rect{left + padding, top - padding - lineHeight,
                             textWidth, lineHeight},
          label, textColour, rowColour, {.overflow = *boxed});
      metrics = {result.fitted.widthPx, result.fitted.heightPx};
    } else {
      metrics = canvas.addText(state, left + padding, top - padding, label,
                               textColour, rowColour);
    }
    overflows.width += metrics.width > textWidth ? 1 : 0;
    overflows.height += metrics.height > lineHeight ? 1 : 0;
  }
  state.glyphCache.flush();
  canvas.commit();
  return overflows;
}

void drawFrame(render::RenderDevice &device, RenderState &state,
               gleditor::Canvas &canvas, const glm::mat4 &projection) {
  if (!device.beginFrame()) {
    throw std::runtime_error("benchmark frame skipped by device");
  }
  canvas.draw(state, projection);
  device.endFrame();
  // Serializing completion gives both backends the same frame boundary and
  // prevents queued GPU work from being attributed to a later scenario.
  device.waitIdle();
}

template <typename Value>
double percentile(std::vector<Value> values, const double fraction) {
  std::ranges::sort(values);
  const auto index = static_cast<std::size_t>(
      std::ceil(fraction * static_cast<double>(values.size())) - 1.0);
  return static_cast<double>(values.at(index));
}

void writeScreenshot(const render::FrameImage &image, const std::string &path) {
  std::ofstream output(path, std::ios::binary);
  output << "P6\n" << image.width << ' ' << image.height << "\n255\n";
  for (std::size_t offset = 0; offset < image.rgba.size(); offset += 4) {
    const std::array<char, 3> rgb{static_cast<char>(image.rgba.at(offset)),
                                  static_cast<char>(image.rgba.at(offset + 1)),
                                  static_cast<char>(image.rgba.at(offset + 2))};
    output.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
  }
  if (!output) {
    throw std::runtime_error("cannot write screenshot " + path);
  }
}

std::size_t
changedPixels(const render::FrameImage &text,
              const render::FrameImage &rectangles,
              const std::optional<float> boxedLineHeight = std::nullopt) {
  if (text.width != screenWidth || text.height != screenHeight ||
      rectangles.width != text.width || rectangles.height != text.height ||
      text.rgba.size() != static_cast<std::size_t>(screenWidth) *
                              static_cast<std::size_t>(screenHeight) * 4 ||
      text.rgba.size() != rectangles.rgba.size()) {
    throw std::runtime_error("backend returned an invalid color capture");
  }
  std::size_t changed{};
  for (std::size_t offset = 0; offset < text.rgba.size(); offset += 4) {
    if (text.rgba[offset] != rectangles.rgba[offset] ||
        text.rgba[offset + 1] != rectangles.rgba[offset + 1] ||
        text.rgba[offset + 2] != rectangles.rgba[offset + 2]) {
      ++changed;
      if (boxedLineHeight) {
        constexpr float columnWidth =
            (static_cast<float>(screenWidth) - 2.0F * margin -
             static_cast<float>(columns - 1) * gap) /
            static_cast<float>(columns);
        const float x = static_cast<float>((offset / 4) % screenWidth) + 0.5F;
        const float y = static_cast<float>(screenHeight) -
                        static_cast<float>((offset / 4) / screenWidth) - 0.5F;
        bool inside = false;
        for (int index = 0; index < labelCount; ++index) {
          const float left =
              margin + static_cast<float>(index / rows) * (columnWidth + gap) +
              padding;
          const float top = static_cast<float>(screenHeight) - margin -
                            static_cast<float>(index % rows) *
                                (*boxedLineHeight + 2.0F * padding + gap) -
                            padding;
          inside = inside ||
                   (x >= left && x <= left + columnWidth - 2.0F * padding &&
                    y <= top && y >= top - *boxedLineHeight);
        }
        if (!inside) {
          throw std::runtime_error(
              "boxed text changed a pixel outside its clip");
        }
      }
    }
  }
  if (changed == 0) {
    throw std::runtime_error("labels changed no pixels: backend drew no text");
  }
  return changed;
}

void runScenario(
    const Options &options, const std::string_view mode,
    const bool widthLimited, const bool retained,
    const std::vector<std::string> &labels, render::RenderDevice &device,
    RenderState &state, gleditor::Canvas &canvas, const glm::mat4 &projection,
    const float lineHeight,
    const std::optional<gleditor::text::Overflow> boxed = std::nullopt) {
  auto overflows =
      buildScene(canvas, state, labels, lineHeight, widthLimited, true, boxed);
  Samples samples;
  samples.frameMs.reserve(static_cast<std::size_t>(options.frames));
  samples.layouts.reserve(static_cast<std::size_t>(options.frames));
  samples.harfbuzz.reserve(static_cast<std::size_t>(options.frames));
  samples.fallbacks.reserve(static_cast<std::size_t>(options.frames));
  samples.bytes.reserve(static_cast<std::size_t>(options.frames));
  const auto frameCount =
      static_cast<std::int64_t>(options.warmup) + options.frames;
  for (std::int64_t frame = 0; frame < frameCount; ++frame) {
    gleditor::text::ShapingStatsScope capture;
    const auto start = Clock::now();
    if (!retained) {
      overflows = buildScene(canvas, state, labels, lineHeight, widthLimited,
                             true, boxed);
    }
    drawFrame(device, state, canvas, projection);
    const auto elapsed =
        std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    if (frame < options.warmup) {
      continue;
    }
    const auto stats = capture.stats();
    samples.frameMs.push_back(elapsed);
    samples.layouts.push_back(stats.layoutCalls);
    samples.harfbuzz.push_back(stats.harfbuzzCalls);
    samples.fallbacks.push_back(stats.fallbackCalls);
    samples.bytes.push_back(stats.inputBytes);
  }

  const auto textImage = device.captureColorTarget();
  if (!options.screenshotPrefix.empty()) {
    writeScreenshot(textImage, options.screenshotPrefix + "." +
                                   std::string(mode) + ".ppm");
  }
  buildScene(canvas, state, labels, lineHeight, widthLimited, false, boxed);
  drawFrame(device, state, canvas, projection);
  const auto inkPixels =
      changedPixels(textImage, device.captureColorTarget(),
                    boxed ? std::optional<float>{lineHeight} : std::nullopt);

  std::cout << render::backendName(options.backend) << '\t' << mode << '\t'
            << options.font << '\t' << labels.size() << '\t' << labelCount
            << '\t' << options.frames << '\t' << std::fixed
            << std::setprecision(3) << percentile(samples.frameMs, 0.50) << '\t'
            << percentile(samples.frameMs, 0.95) << '\t'
            << percentile(samples.layouts, 0.50) << '\t'
            << percentile(samples.layouts, 0.95) << '\t'
            << percentile(samples.harfbuzz, 0.50) << '\t'
            << percentile(samples.harfbuzz, 0.95) << '\t'
            << percentile(samples.fallbacks, 0.50) << '\t'
            << percentile(samples.fallbacks, 0.95) << '\t'
            << percentile(samples.bytes, 0.50) << '\t' << overflows.width
            << '\t' << overflows.height << '\t' << inkPixels << '\n';
}

int run(const Options &options) {
  if (!render::backendCompiledIn(options.backend)) {
    std::cout << "unavailable backend=" << render::backendName(options.backend)
              << " reason=not_compiled\n";
    return 77;
  }
  if (std::getenv("SDL_VIDEODRIVER") == nullptr) {
    throw std::runtime_error(
        "set SDL_VIDEODRIVER=offscreen, or x11 under Xvfb, before running");
  }
  const auto labels = readLabels(options.fixture);
  const auto font =
      gleditor::text::FontManager::instance().getFont(options.font);
  const float lineHeight = font->metrics().lineHeight;
  AutoSDL sdl(SDL_INIT_VIDEO);
  render::configureBackendWindowAttributes(options.backend);
  AutoSDLWindow window("UI text baseline", screenWidth, screenHeight,
                       render::backendWindowFlags(options.backend) |
                           SDL_WINDOW_HIDDEN);
  auto device = render::createDevice(options.backend);
  device->initialize(window);
  device->setStrictDiagnostics(true);
  // Vulkan must present to return swapchain images even for a hidden window.
  if (options.backend != render::Backend::Vulkan) {
    device->setPresentEnabled(false);
  }
  {
    RenderState state(device.get());
    gleditor::Canvas canvas(device.get(), options.font);
    canvas.createPipeline(glyphPipeline());
    const auto projection =
        glm::ortho(0.0F, static_cast<float>(screenWidth), 0.0F,
                   static_cast<float>(screenHeight), -1.0F, 1.0F);
    std::cout << "backend\tmode\tfont\tfixtures\tlabels_per_frame\tframes\t"
                 "frame_ms_p50\tframe_ms_p95\tlayout_calls_p50\t"
                 "layout_calls_p95\tharfbuzz_calls_p50\tharfbuzz_calls_p95\t"
                 "fallback_calls_p50\tfallback_calls_p95\tinput_bytes_p50\t"
                 "width_overflows\theight_overflows\ttext_pixels\n";
    runScenario(options, "unbounded-rebuild", false, false, labels, *device,
                state, canvas, projection, lineHeight);
    runScenario(options, "width-limit-rebuild", true, false, labels, *device,
                state, canvas, projection, lineHeight);
    runScenario(options, "width-limit-retained", true, true, labels, *device,
                state, canvas, projection, lineHeight);
    runScenario(options, "boxed-rebuild", true, false, labels, *device, state,
                canvas, projection, lineHeight,
                gleditor::text::Overflow::Ellipsis);
    runScenario(options, "boxed-retained", true, true, labels, *device, state,
                canvas, projection, lineHeight,
                gleditor::text::Overflow::Ellipsis);
    runScenario(options, "boxed-clip-rebuild", true, false, labels, *device,
                state, canvas, projection, lineHeight,
                gleditor::text::Overflow::Clip);
    device->waitIdle();
  }
  device->shutdown();
  return 0;
}

} // namespace

int main(const int argc, char **argv) {
  try {
    const auto options = parseOptions(argc, argv);
    return options ? run(*options) : 0;
  } catch (const std::exception &failure) {
    GLEDITOR_LOG_ERROR("ui.layout", "UI text baseline failed: {}",
                       failure.what());
    return 1;
  }
}
