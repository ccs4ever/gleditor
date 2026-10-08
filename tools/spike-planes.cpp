/**
 * @file spike-planes.cpp
 * @brief THROWAWAY spikes R1, R2 and R5 of the view project
 *        (design/view-system-implementation-plan.md section 3). Not for merge.
 *
 *   spike-planes r1 --backend B [--counts 100,300] [--frames N]
 *     Draw cost of one draw per plane against one draw per coplanar group.
 *   spike-planes r2 --backend B --out PREFIX
 *     A 1-D gradient baked into an R8 array texture sampled by the unchanged
 *     glyph pipeline: does it fade?
 *   spike-planes r5 --backend B --out PREFIX
 *     A translucent page, a translucent plane and a beam crossing both, from
 *     in front and from behind, under three orderings.
 */
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <gleditor/beams.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/paths.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render/shader_source.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/sdl_wrap.hpp>

namespace {

using Clock = std::chrono::steady_clock;
using Row   = Doc::VBORow;

struct Options {
  std::string mode;
  render::Backend backend{render::Backend::OpenGL};
  std::vector<int> counts{100, 300, 1000, 1300, 5000, 10000, 50000};
  int frames{60};
  int warmup{10};
  std::string out{"spike"};
};

render::PipelineDesc glyphPipeline(const bool depthWrite = true) {
  const auto shaders = gleditor::assetPath("shaders");
  render::PipelineDesc desc;
  desc.name           = depthWrite ? "spike-glyph" : "spike-glyph-nowrite";
  desc.vertexSource   = render::readShaderBody(shaders + "/glyph.vert.glsl");
  desc.fragmentSource = render::readShaderBody(shaders + "/glyph.frag.glsl");
  desc.spirvDir       = shaders + "/vulkan";
  desc.layout         = Doc::vertexLayout();
  desc.depthWrite     = depthWrite;
  return desc;
}

render::DrawUniforms uniforms(const glm::mat4 &mvp, const float opacity) {
  render::DrawUniforms result;
  std::copy_n(glm::value_ptr(mvp), 16, result.mvp.begin());
  result.opacity = opacity;
  return result;
}

Row solid(const float cx, const float cy, const unsigned width,
          const unsigned height, const unsigned rgb, const unsigned depth) {
  return Row{.pos        = {cx, cy},
             .foreground = Row::fill(rgb, depth),
             .atlas      = 0,
             .quad       = Row::box(0, width, height, 0),
             .paper      = Row::paperAt(rgb, 0)};
}

Row textured(const float cx, const float cy, const unsigned width,
             const unsigned height, const unsigned ink, const unsigned paper,
             const unsigned texX, const unsigned texY, const unsigned depth) {
  return Row{.pos        = {cx, cy},
             .foreground = Row::ink(ink, depth, false),
             .atlas      = Row::atlasAt(texX, texY),
             .quad       = Row::box(0, width, height, 0),
             .paper      = Row::paperAt(paper, 0)};
}

render::BufferHandle upload(render::RenderDevice &device,
                            const std::vector<Row> &rows) {
  const auto bytes  = rows.size() * sizeof(Row);
  const auto buffer = device.createBuffer(render::BufferKind::Vertex, bytes);
  device.updateBuffer(
      buffer, 0, std::as_bytes(std::span<const Row>(rows.data(), rows.size())));
  return buffer;
}

void writePpm(const render::FrameImage &image, const std::string &path) {
  std::ofstream output(path, std::ios::binary);
  output << "P6\n" << image.width << ' ' << image.height << "\n255\n";
  for (std::size_t offset = 0; offset < image.rgba.size(); offset += 4) {
    output.put(static_cast<char>(image.rgba[offset]));
    output.put(static_cast<char>(image.rgba[offset + 1]));
    output.put(static_cast<char>(image.rgba[offset + 2]));
  }
  if (!output) throw std::runtime_error("cannot write " + path);
}

/// Pixel at (x, y) with y measured from the bottom, as GL window space is.
std::array<int, 3> pixel(const render::FrameImage &image, const int x,
                         const int yUp) {
  const int row = image.height - 1 - yUp;
  const auto at =
      (static_cast<std::size_t>(row) * static_cast<std::size_t>(image.width) +
       static_cast<std::size_t>(x)) *
      4;
  return {image.rgba.at(at), image.rgba.at(at + 1), image.rgba.at(at + 2)};
}

std::string str(const std::array<int, 3> &rgb) {
  std::ostringstream text;
  text << '(' << rgb[0] << ',' << rgb[1] << ',' << rgb[2] << ')';
  return text.str();
}

double median(std::vector<double> values) {
  std::ranges::sort(values);
  return values[values.size() / 2];
}

struct Session {
  std::unique_ptr<AutoSDL> sdl;
  std::unique_ptr<AutoSDLWindow> window;
  std::unique_ptr<render::RenderDevice> device;
};

Session open(const render::Backend backend, const int width, const int height) {
  Session session;
  session.sdl = std::make_unique<AutoSDL>(SDL_INIT_VIDEO);
  render::configureBackendWindowAttributes(backend);
  session.window = std::make_unique<AutoSDLWindow>(
      "spike", width, height,
      render::backendWindowFlags(backend) | SDL_WINDOW_HIDDEN);
  session.device = render::createDevice(backend);
  session.device->initialize(*session.window);
  session.device->setStrictDiagnostics(true);
  if (backend != render::Backend::Vulkan) {
    session.device->setPresentEnabled(false);
  }
  return session;
}

/// A small R8 array texture standing in for the glyph atlas: a glyph-like
/// pattern for R1 and a horizontal ramp for R2.
render::TextureHandle makeTexture(render::RenderDevice &device) {
  constexpr int size = 256;
  const auto texture =
      device.createTextureArray(size, 1, render::TextureFormat::R8, 1);
  std::vector<std::byte> texels(static_cast<std::size_t>(size * size));
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      std::uint8_t value{};
      if (y < 32) {
        value = static_cast<std::uint8_t>(x); // the gradient band
      } else {
        value = ((x / 3 + y / 4) % 3 == 0) ? 255 : 40; // glyph-ish texels
      }
      texels[static_cast<std::size_t>(y * size + x)] = std::byte{value};
    }
  }
  device.updateTextureLayer(texture, 0, 0, 0, size, size, texels);
  return texture;
}

// ---------------------------------------------------------------------------
// R1

int r1Width = 1280, r1Height = 800; // --size WxH
constexpr int instancesPerPlane = 8;
constexpr int groupSize         = 10;

void runR1(const Options &options) {
  auto session = open(options.backend, r1Width, r1Height);
  auto &device = *session.device;
  {
    const auto pipeline = device.createPipeline(glyphPipeline());
    const auto texture  = makeTexture(device);
    const auto proj     = glm::perspective(glm::radians(45.0F),
                                           static_cast<float>(r1Width) /
                                               static_cast<float>(r1Height),
                                           0.1F, 100.0F);
    const auto view     = glm::lookAt(glm::vec3{0, 0, 3.2F}, glm::vec3{0, 0, 0},
                                      glm::vec3{0, 1, 0});
    const glm::mat4 worldToClip = proj * view;

    std::cout << "backend\tplanes\tvariant\tdraws\tinstances\tbuild_ms\t"
                 "issue_ms\tcpu_frame_ms\tcomplete_ms\n";
    for (const int planes : options.counts) {
      // Plane-local content: a 60x36 panel and seven textured "glyphs".
      std::vector<Row> local;
      local.push_back(solid(0, 0, 60, 36, 0x283344FFU, Row::onPaper));
      for (int g = 0; g < instancesPerPlane - 1; ++g) {
        local.push_back(textured(
            -24.0F + static_cast<float>(g) * 8.0F, 0, 7, 12, 0xE6EDF3FFU,
            0x283344FFU, static_cast<unsigned>(g * 9), 40, Row::onText));
      }
      // Grid of planes covering the view; groups of ten coplanar neighbours
      // share one rotation, so a group is drawable with one matrix.
      const int columns = static_cast<int>(std::ceil(
          std::sqrt(static_cast<double>(planes) * r1Width / r1Height)));
      const int rowsN   = (planes + columns - 1) / columns;
      const float cellW =
          4.0F * r1Width / r1Height / static_cast<float>(columns);
      const float cellH = 4.0F / static_cast<float>(rowsN);
      const float scale = std::min(cellW / 70.0F, cellH / 44.0F);

      std::vector<Row> perPlaneRows; // planes * local, plane-local positions
      std::vector<Row> groupedRows;  // same, positioned in group space
      std::vector<glm::mat4> planeModel;
      std::vector<glm::mat4> groupModel;
      for (int p = 0; p < planes; ++p) {
        const int group  = p / groupSize;
        const int first  = group * groupSize;
        const auto place = [&](int index) {
          return glm::vec3{
              -2.0F * r1Width / r1Height +
                  (static_cast<float>(index % columns) + 0.5F) * cellW,
              -2.0F + (static_cast<float>(index / columns) + 0.5F) * cellH,
              0.0F};
        };
        if (p == first) {
          auto model = glm::translate(glm::mat4{1}, place(first));
          model      = glm::rotate(
              model, glm::radians(static_cast<float>(group % 7) * 5.0F - 15.0F),
              glm::vec3{0, 1, 0});
          model = glm::scale(model, glm::vec3{scale});
          groupModel.push_back(model);
        }
        // Offset of this plane within its group, in group-local px, along
        // the group's own x axis so all ten stay coplanar.
        const glm::vec3 delta = (place(p) - place(first)) / scale;
        planeModel.push_back(groupModel.back() *
                             glm::translate(glm::mat4{1}, delta));
        for (const auto &row : local) {
          perPlaneRows.push_back(row);
          auto moved   = row;
          moved.pos[0] = row.pos[0] + delta.x;
          moved.pos[1] = row.pos[1] + delta.y;
          groupedRows.push_back(moved);
        }
      }
      const auto planeBuffer = upload(device, perPlaneRows);
      const auto groupBuffer = upload(device, groupedRows);
      const auto groups      = static_cast<int>(groupModel.size());

      struct Variant {
        std::string_view name;
        int draws;
      };
      const std::array variants{Variant{"per-plane", planes},
                                Variant{"per-group10", groups},
                                Variant{"single", 1}};
      std::vector<render::GlyphBatch> batches;
      batches.reserve(static_cast<std::size_t>(planes));
      std::array<render::FrameImage, 2> images;
      for (std::size_t v = 0; v < variants.size(); ++v) {
        const auto &variant = variants[v];
        const auto build    = [&] {
          batches.clear();
          if (variant.name == "per-plane") {
            for (int p = 0; p < planes; ++p) {
              batches.push_back(render::GlyphBatch{
                     .uniforms = uniforms(worldToClip * planeModel[p], 1.0F),
                     .vertices = planeBuffer,
                     .vertexByteOffset = static_cast<std::size_t>(p) *
                                      instancesPerPlane * sizeof(Row),
                     .instanceCount = instancesPerPlane});
            }
          } else if (variant.name == "per-group10") {
            for (int g = 0; g < groups; ++g) {
              const int first = g * groupSize;
              const int count = std::min(groupSize, planes - first);
              batches.push_back(render::GlyphBatch{
                     .uniforms = uniforms(worldToClip * groupModel[g], 1.0F),
                     .vertices = groupBuffer,
                     .vertexByteOffset = static_cast<std::size_t>(first) *
                                      instancesPerPlane * sizeof(Row),
                     .instanceCount =
                      static_cast<std::uint32_t>(count * instancesPerPlane)});
            }
          } else {
            batches.push_back(render::GlyphBatch{
                   .uniforms         = uniforms(worldToClip * groupModel[0], 1.0F),
                   .vertices         = groupBuffer,
                   .vertexByteOffset = 0,
                   .instanceCount =
                    static_cast<std::uint32_t>(planes * instancesPerPlane)});
          }
        };
        int frames = options.frames;
        if (planes >= 50000) frames = std::max(15, frames / 3);
        std::vector<double> buildMs, issueMs, cpuMs, completeMs;
        for (int frame = 0; frame < options.warmup + frames; ++frame) {
          const auto t0 = Clock::now();
          if (!device.beginFrame()) throw std::runtime_error("frame skipped");
          device.bindPipeline(pipeline);
          device.bindAtlasTexture(texture);
          device.setHighlights({});
          const auto t1 = Clock::now();
          build();
          const auto t2 = Clock::now();
          device.drawGlyphBatches(batches);
          const auto t3 = Clock::now();
          device.endFrame();
          const auto t4 = Clock::now();
          device.waitIdle();
          const auto t5 = Clock::now();
          if (frame < options.warmup) continue;
          const auto ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
          };
          buildMs.push_back(ms(t1, t2));
          issueMs.push_back(ms(t2, t3));
          cpuMs.push_back(ms(t0, t4));
          completeMs.push_back(ms(t0, t5));
        }
        if (v < 2) images[v] = device.captureColorTarget();
        std::cout << render::backendName(options.backend) << '\t' << planes
                  << '\t' << variant.name << '\t' << batches.size() << '\t'
                  << planes * instancesPerPlane << '\t' << std::fixed
                  << std::setprecision(3) << median(buildMs) << '\t'
                  << median(issueMs) << '\t' << median(cpuMs) << '\t'
                  << median(completeMs) << std::endl;
      }
      // Batching must not change the picture: per-plane and per-group draw
      // the same quads with the same final transforms.
      std::size_t differing{};
      for (std::size_t i = 0; i < images[0].rgba.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
          if (std::abs(images[0].rgba[i + c] - images[1].rgba[i + c]) > 1) {
            ++differing;
            break;
          }
        }
      }
      std::cout << "# " << planes << " planes: per-plane vs per-group10 "
                << differing << " pixels differ (>1)\n";
      device.waitIdle();
      device.destroyBuffer(planeBuffer);
      device.destroyBuffer(groupBuffer);
    }
    device.waitIdle();
    device.destroyTexture(texture);
  }
  device.shutdown();
}

// ---------------------------------------------------------------------------
// R2

constexpr int r2Width = 640, r2Height = 360;

void runR2(const Options &options) {
  auto session = open(options.backend, r2Width, r2Height);
  auto &device = *session.device;
  {
    const auto pipeline = device.createPipeline(glyphPipeline());
    const auto texture  = makeTexture(device);
    const auto mvp      = glm::ortho(0.0F, static_cast<float>(r2Width), 0.0F,
                                     static_cast<float>(r2Height), -1.0F, 1.0F);
    constexpr unsigned white  = 0xFFFFFF00U;
    constexpr unsigned accent = 0x1F4FBF00U;
    constexpr unsigned red    = 0xD0303000U;
    constexpr unsigned green  = 0x30A03000U;
    constexpr float bandX     = 320.0F; // band centre; spans 192..448
    const std::array<float, 3> bandY{290.0F, 180.0F, 70.0F};

    std::vector<Row> backdrop;
    // A: flat paper behind the band, the case the plan has in mind.
    backdrop.push_back(solid(bandX, bandY[0], 320, 60, white, Row::onPaper));
    // B and C: stripes behind the band, which a true alpha fade would reveal.
    for (int panel = 1; panel < 3; ++panel) {
      for (int s = 0; s < 40; ++s) {
        backdrop.push_back(solid(160.0F + static_cast<float>(s) * 8.0F + 4.0F,
                                 bandY[panel], 8, 60, (s % 2) ? red : green,
                                 Row::onPaper));
      }
    }
    std::vector<Row> band;
    for (int panel = 0; panel < 3; ++panel) {
      // 256x32 quad sampling texels (0..256, 0..32): coverage == x/255.
      band.push_back(textured(bandX, bandY[panel], 256, 32, accent, white, 0, 0,
                              Row::onText));
    }
    const auto backBuffer = upload(device, backdrop);
    const auto bandBuffer = upload(device, band);

    if (!device.beginFrame()) throw std::runtime_error("frame skipped");
    device.bindPipeline(pipeline);
    device.bindAtlasTexture(texture);
    device.setHighlights({});
    device.drawGlyphs(uniforms(mvp, 1.0F), backBuffer, 0,
                      static_cast<std::uint32_t>(backdrop.size()));
    device.drawGlyphs(uniforms(mvp, 1.0F), bandBuffer, 0, 2);
    // C: the same band at draw opacity 0.5.
    device.drawGlyphs(uniforms(mvp, 0.5F), bandBuffer, 2 * sizeof(Row), 1);
    device.endFrame();
    device.waitIdle();
    const auto image = device.captureColorTarget();
    writePpm(image, options.out + ".ppm");

    const std::array<std::string_view, 3> names{"A flat-paper", "B stripes",
                                                "C stripes@0.5"};
    for (int panel = 0; panel < 3; ++panel) {
      std::cout << render::backendName(options.backend) << '\t' << names[panel];
      for (const int u : {0, 4, 64, 128, 192, 251}) {
        const int x = static_cast<int>(bandX) - 128 + u;
        std::cout << "\tu" << u << '='
                  << str(pixel(image, x, static_cast<int>(bandY[panel])))
                  << "/under="
                  << str(pixel(image, x, static_cast<int>(bandY[panel]) + 24));
      }
      std::cout << '\n';
    }
    device.waitIdle();
    device.destroyBuffer(backBuffer);
    device.destroyBuffer(bandBuffer);
    device.destroyTexture(texture);
  }
  device.shutdown();
}

// ---------------------------------------------------------------------------
// R5

constexpr int r5Width = 800, r5Height = 600;

struct Sheet {
  glm::mat4 model;
  std::size_t first;
  std::uint32_t count;
  glm::vec3 centre;
};

void runR5(const Options &options) {
  auto session = open(options.backend, r5Width, r5Height);
  auto &device = *session.device;
  {
    RenderState state(&device);
    const auto writing   = device.createPipeline(glyphPipeline(true));
    const auto noWriting = device.createPipeline(glyphPipeline(false));
    const auto shaders   = gleditor::assetPath("shaders");
    const auto spirv     = shaders + "/vulkan";

    // Page: paper with text bars, at z = 0. Plane: amber card at z = -1.5.
    std::vector<Row> rows;
    constexpr float perPx = 1.0F / 50.0F;
    rows.push_back(solid(0, 0, 220, 280, 0xEEEEF4FFU, Row::onPaper));
    for (int line = 0; line < 12; ++line) {
      rows.push_back(solid(-10.0F, 120.0F - static_cast<float>(line) * 20.0F,
                           180, 8, 0x303040FFU, Row::onText));
    }
    const auto pageCount = static_cast<std::uint32_t>(rows.size());
    rows.push_back(solid(0, 0, 220, 160, 0xE0A030FFU, Row::onPaper));
    for (int line = 0; line < 4; ++line) {
      rows.push_back(solid(0, 50.0F - static_cast<float>(line) * 30.0F, 160, 14,
                           0x402000FFU, Row::onText));
    }
    const auto planeCount = static_cast<std::uint32_t>(rows.size()) - pageCount;
    const auto buffer     = upload(device, rows);

    const glm::vec3 pageCentre{-0.8F, 0.0F, 0.0F};
    const glm::vec3 planeCentre{0.8F, 0.0F, -1.5F};
    const auto sheetModel = [&](const glm::vec3 &centre) {
      return glm::scale(glm::translate(glm::mat4{1}, centre), glm::vec3{perPx});
    };
    const Sheet page{sheetModel(pageCentre), 0, pageCount, pageCentre};
    const Sheet plane{sheetModel(planeCentre), pageCount, planeCount,
                      planeCentre};
    constexpr float sheetOpacity = 0.55F;

    // Beam from in front of the page to behind the plane; crosses z = 0 at
    // x = -1 (inside the page) and z = -1.5 at x = 1 (inside the plane).
    const glm::vec3 from{-3.0F, 0.0F, 1.5F};
    const glm::vec3 to{3.0F, 0.0F, -3.0F};
    const auto at = [&](float t) { return from + (to - from) * t; };
    constexpr std::uint32_t beamColour = 0x20F0F0FFU;
    constexpr float beamWidth          = 0.18F;

    gleditor::Beams beamWrite(&device);
    beamWrite.createPipeline(shaders, spirv, true, true);
    beamWrite.add(from, to, beamWidth, beamColour, 1);
    beamWrite.commit();
    gleditor::Beams beamNoWrite(&device);
    beamNoWrite.createPipeline(shaders, spirv, true, false);
    beamNoWrite.add(from, to, beamWidth, beamColour, 1);
    beamNoWrite.commit();
    // The reference splits the beam where it crosses each sheet.
    const std::array<float, 4> cuts{0.0F, 1.0F / 3.0F, 2.0F / 3.0F, 1.0F};
    std::vector<std::unique_ptr<gleditor::Beams>> segments;
    for (int s = 0; s < 3; ++s) {
      auto segment = std::make_unique<gleditor::Beams>(&device);
      segment->createPipeline(shaders, spirv, true, false);
      segment->add(at(cuts[s]), at(cuts[s + 1]), beamWidth, beamColour, 1,
                   cuts[s], cuts[s + 1]);
      segment->commit();
      segments.push_back(std::move(segment));
    }

    const auto proj = glm::perspective(glm::radians(45.0F),
                                       static_cast<float>(r5Width) /
                                           static_cast<float>(r5Height),
                                       0.1F, 100.0F);
    const glm::vec3 target{0.0F, 0.0F, -0.75F};
    struct Camera {
      std::string_view name;
      glm::vec3 eye;
    };
    const std::array cameras{Camera{"front", {1.2F, 1.0F, 8.0F}},
                             Camera{"back", {1.2F, 1.0F, -9.5F}}};

    for (const auto &camera : cameras) {
      const auto view =
          glm::lookAt(camera.eye, target, glm::vec3{0.0F, 1.0F, 0.0F});
      const glm::mat4 worldToClip = proj * view;
      const auto depthOf          = [&](const glm::vec3 &p) {
        return (view * glm::vec4(p, 1.0F)).z; // more negative is farther
      };
      const auto drawSheet = [&](const Sheet &sheet,
                                 render::PipelineHandle pipe) {
        device.bindPipeline(pipe);
        device.bindAtlasTexture(state.glyphCache.textureHandle());
        device.drawGlyphs(uniforms(worldToClip * sheet.model, sheetOpacity),
                          buffer, sheet.first * sizeof(Row), sheet.count);
      };
      const auto toPixel = [&](const glm::vec3 &p) {
        const auto clip = worldToClip * glm::vec4(p, 1.0F);
        const glm::vec2 ndc{clip.x / clip.w, clip.y / clip.w};
        return std::array<int, 2>{
            static_cast<int>((ndc.x * 0.5F + 0.5F) * r5Width),
            static_cast<int>((ndc.y * 0.5F + 0.5F) * r5Height)};
      };

      std::array<render::FrameImage, 3> images;
      const std::array<std::string_view, 3> variants{"A-today", "B-plan43",
                                                     "C-reference"};
      for (std::size_t v = 0; v < 3; ++v) {
        if (!device.beginFrame()) throw std::runtime_error("frame skipped");
        device.setHighlights({});
        if (v == 0) {
          // Today: documents (renderer.cpp sorts them by world z, which
          // puts the page last-but-one either way), then what the program
          // draws, then beams; every pipeline writes depth.
          drawSheet(page, writing);
          drawSheet(plane, writing);
          beamWrite.draw(state, worldToClip, 1.0F, 0);
        } else if (v == 1) {
          // Section 4.3: translucent sheets in one list sorted back to
          // front, no depth write; beams after, tested, not written.
          std::array<const Sheet *, 2> order{&page, &plane};
          std::ranges::sort(order, [&](auto *a, auto *b) {
            return depthOf(a->centre) < depthOf(b->centre);
          });
          for (const auto *sheet : order) drawSheet(*sheet, noWriting);
          beamNoWrite.draw(state, worldToClip, 1.0F, 0);
        } else {
          // Reference: beams split at each crossing and sorted with the
          // sheets in one list.
          struct Item {
            float depth;
            const Sheet *sheet;
            const gleditor::Beams *beam;
          };
          std::vector<Item> items{{depthOf(page.centre), &page, nullptr},
                                  {depthOf(plane.centre), &plane, nullptr}};
          for (int s = 0; s < 3; ++s) {
            items.push_back({depthOf(at((cuts[s] + cuts[s + 1]) * 0.5F)),
                             nullptr,
                             segments[static_cast<std::size_t>(s)].get()});
          }
          std::ranges::sort(items, [](const Item &a, const Item &b) {
            return a.depth < b.depth;
          });
          for (const auto &item : items) {
            if (item.sheet) {
              drawSheet(*item.sheet, noWriting);
            } else {
              item.beam->draw(state, worldToClip, 1.0F, 0);
            }
          }
        }
        device.endFrame();
        device.waitIdle();
        images[v] = device.captureColorTarget();
        writePpm(images[v], options.out + "." + std::string(camera.name) + "." +
                                std::string(variants[v]) + ".ppm");
      }

      // Probes: the overlap of the two sheets, and the beam just behind
      // each sheet.
      struct Probe {
        std::string_view name;
        glm::vec3 where;
      };
      const std::array probes{Probe{"sheet-overlap", {0.0F, -1.2F, -0.75F}},
                              Probe{"beam@t=0.40(between)", at(0.40F)},
                              Probe{"beam@t=0.25(page front side)", at(0.25F)},
                              Probe{"beam@t=0.80(behind plane)", at(0.80F)}};
      for (const auto &probe : probes) {
        auto px = toPixel(probe.where);
        // The overlap probe is a point between the sheets; sample along the
        // ray through it instead of on a sheet.
        std::cout << render::backendName(options.backend) << '\t' << camera.name
                  << '\t' << probe.name << "\tpx=" << px[0] << ',' << px[1];
        for (std::size_t v = 0; v < 3; ++v) {
          std::cout << '\t' << variants[v] << '='
                    << str(pixel(images[v], px[0], px[1]));
        }
        std::cout << '\n';
      }
      for (std::size_t v = 0; v < 2; ++v) {
        std::size_t differing{};
        for (std::size_t i = 0; i < images[v].rgba.size(); i += 4) {
          for (int c = 0; c < 3; ++c) {
            if (std::abs(images[v].rgba[i + c] - images[2].rgba[i + c]) > 2) {
              ++differing;
              break;
            }
          }
        }
        std::cout << render::backendName(options.backend) << '\t' << camera.name
                  << '\t' << variants[v] << " vs reference: " << differing
                  << " pixels differ (>2)\n";
      }
    }
    device.waitIdle();
    device.destroyBuffer(buffer);
  }
  device.shutdown();
}

Options parse(const int argc, char **argv) {
  if (argc < 2) throw std::invalid_argument("usage: spike-planes r1|r2|r5 ...");
  Options options;
  options.mode = argv[1];
  for (int i = 2; i + 1 < argc; i += 2) {
    const std::string_view key{argv[i]};
    const std::string value{argv[i + 1]};
    if (key == "--backend") {
      options.backend = render::backendFromName(value);
    } else if (key == "--frames") {
      options.frames = std::stoi(value);
    } else if (key == "--size") {
      r1Width  = std::stoi(value.substr(0, value.find('x')));
      r1Height = std::stoi(value.substr(value.find('x') + 1));
    } else if (key == "--out") {
      options.out = value;
    } else if (key == "--counts") {
      options.counts.clear();
      std::stringstream list(value);
      std::string item;
      while (std::getline(list, item, ','))
        options.counts.push_back(std::stoi(item));
    } else {
      throw std::invalid_argument("unknown " + std::string(key));
    }
  }
  return options;
}

} // namespace

int main(const int argc, char **argv) {
  try {
    const auto options = parse(argc, argv);
    if (!render::backendCompiledIn(options.backend)) return 77;
    if (options.mode == "r1")
      runR1(options);
    else if (options.mode == "r2")
      runR2(options);
    else if (options.mode == "r5")
      runR5(options);
    else
      throw std::invalid_argument("unknown mode");
    return 0;
  } catch (const std::exception &failure) {
    std::cerr << "spike-planes failed: " << failure.what() << '\n';
    return 1;
  }
}
