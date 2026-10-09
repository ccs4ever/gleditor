/**
 * @file translucent_list.cpp
 * @brief Implementation of the frame's sorted translucent draws.
 */
#include <gleditor/translucent_list.hpp> // IWYU pragma: associated

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

#include <glm/ext/matrix_double4x4.hpp>
#include <glm/ext/vector_double4.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/matrix.hpp>

#include <gleditor/render/device.hpp>
#include <gleditor/render_state.hpp>

namespace gleditor {

namespace {

/// Copy a matrix into the flat array the device uniform structs carry.
std::array<float, 16> toArray(const glm::mat4 &mat) {
  std::array<float, 16> out{};
  std::copy_n(glm::value_ptr(mat), out.size(), out.begin());
  return out;
}

glm::dmat4 fromArray(const std::array<float, 16> &flat) {
  glm::dmat4 out{};
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      out[column][row] = flat[static_cast<std::size_t>((column * 4) + row)];
    }
  }
  return out;
}

glm::dvec4 point(const std::array<float, 3> &xyz) {
  return {xyz[0], xyz[1], xyz[2], 1.0};
}

/**
 * @brief Cuts closer together than this, as a fraction of a beam, are one.
 *
 * Every page of a document lies in the document's one plane, so a beam
 * crossing a document crosses each of its pages' planes at the same place; and
 * a cut at an end makes a piece of no length. Neither is worth a draw. Found
 * through single-precision matrices, one crossing comes back from different
 * pages a few millionths apart, so the margin is wider than that; a ten
 * thousandth of a beam is far below a pixel.
 */
constexpr double sameCut = 1e-4;

/**
 * @brief Below this a sheet's matrix is taken to have flattened it.
 *
 * A matrix that is not invertible has no plane to give back, and a sheet
 * squashed to a line covers nothing a beam could be behind.
 */
constexpr double singular = 1e-30;

} // namespace

TranslucentList::TranslucentList(render::RenderDevice *const aDevice,
                                 const std::uint32_t initialRows)
    : device(aDevice), pool(std::make_unique<BufferPool>(
                           aDevice, sizeof(Beams::Row), initialRows)) {}

TranslucentList::~TranslucentList() = default;

TranslucentList *TranslucentList::clear() {
  sheets.clear();
  beamSources.clear();
  return this;
}

TranslucentList *
TranslucentList::addSheet(const render::PipelineHandle pipeline,
                          const render::GlyphBatch &batch) {
  if (0 != batch.instanceCount) {
    sheets.push_back(Sheet{.pipeline = pipeline, .batch = batch});
  }
  return this;
}

TranslucentList *TranslucentList::addBeams(const Beams &beams,
                                           const glm::mat4 &transform,
                                           const float opacity,
                                           const std::uint32_t identity) {
  if (beams.ready() && !beams.committedBeams().empty()) {
    beamSources.push_back(BeamSource{.beams     = &beams,
                                     .transform = transform,
                                     .opacity   = opacity,
                                     .identity  = identity});
  }
  return this;
}

void TranslucentList::cutBeams() {
  sheetToLocal.clear();
  for (const auto &sheet : sheets) {
    const auto mvp = fromArray(sheet.batch.uniforms.mvp);
    if (std::abs(glm::determinant(mvp)) <= singular) {
      continue;
    }
    sheetToLocal.push_back(glm::inverse(mvp));
  }

  pieces.clear();
  for (std::uint32_t source = 0; source < beamSources.size(); ++source) {
    const auto &from     = beamSources[source];
    const auto transform = glm::dmat4(from.transform);
    for (const auto &row : from.beams->committedBeams()) {
      const auto start = point(row.from);
      const auto end   = point(row.to);
      // Clip coordinates are a linear function of the beam's own, so a
      // fraction along the beam is the same fraction along the segment
      // between its two ends' clip coordinates, and the same again in any
      // sheet's own space. Only the divide by w bends it, and nothing here
      // divides before finding the crossing.
      const auto clipStart = transform * start;
      const auto clipEnd   = transform * end;
      cuts.clear();
      cuts.push_back(0.0);
      for (const auto &toLocal : sheetToLocal) {
        const double before = (toLocal * clipStart).z;
        const double after  = (toLocal * clipEnd).z;
        if ((before < 0.0) == (after < 0.0) || before == after) {
          continue;
        }
        const double at = before / (before - after);
        if (at > sameCut && at < 1.0 - sameCut) {
          cuts.push_back(at);
        }
      }
      cuts.push_back(1.0);
      std::ranges::sort(cuts);
      const auto kept = std::ranges::unique(
          cuts, [](double a, double b) { return b - a < sameCut; });
      cuts.erase(kept.begin(), kept.end());

      const glm::dvec3 a{row.from[0], row.from[1], row.from[2]};
      const glm::dvec3 b{row.to[0], row.to[1], row.to[2]};
      for (std::size_t i = 1; i < cuts.size(); ++i) {
        const auto lerp = [&](const double t) { return a + ((b - a) * t); };
        const auto p0   = lerp(cuts[i - 1]);
        const auto p1   = lerp(cuts[i]);
        const auto fade = [&](const double t) {
          return static_cast<float>(row.along[0] +
                                    ((row.along[1] - row.along[0]) * t));
        };
        Beams::Row piece = row;
        piece.from       = {static_cast<float>(p0.x), static_cast<float>(p0.y),
                            static_cast<float>(p0.z)};
        piece.to         = {static_cast<float>(p1.x), static_cast<float>(p1.y),
                            static_cast<float>(p1.z)};
        piece.along      = {fade(cuts[i - 1]), fade(cuts[i])};
        pieces.push_back(Piece{.row = piece, .source = source});
      }
    }
  }
}

void TranslucentList::uploadPieces() {
  upload.clear();
  uploadedAt.assign(pieces.size(), 0);
  for (const auto &item : order) {
    if (item.beam) {
      uploadedAt[item.index] = static_cast<std::uint32_t>(upload.size());
      upload.push_back(pieces[item.index].row);
    }
  }
  if (!backing.empty()) {
    pool->release(backing);
    backing = {};
  }
  if (upload.empty()) {
    return;
  }
  backing = pool->reserve(static_cast<std::uint32_t>(upload.size()));
  pool->write(
      backing, 0,
      std::as_bytes(std::span<const Beams::Row>(upload.data(), upload.size())));
}

void TranslucentList::draw(RenderState &state) {
  stats = Stats{.sheets = sheets.size()};
  for (const auto &source : beamSources) {
    stats.beams += source.beams->committedBeams().size();
  }
  if (empty()) {
    return;
  }

  // With no sheet there is nothing for a beam to be on the wrong side of, so
  // each batch is drawn whole from its own storage, as Beams::draw() would.
  if (sheets.empty()) {
    for (const auto &source : beamSources) {
      source.beams->draw(state, source.transform, source.opacity,
                         source.identity);
      stats.beamPieces += source.beams->committedBeams().size();
      ++stats.draws;
    }
    return;
  }

  cutBeams();
  stats.beamPieces = pieces.size();

  order.clear();
  for (std::uint32_t i = 0; i < sheets.size(); ++i) {
    const auto clip = fromArray(sheets[i].batch.uniforms.mvp) *
                      glm::dvec4(0.0, 0.0, 0.0, 1.0);
    order.push_back(Item{.w = clip.w, .z = clip.z, .index = i, .beam = false});
  }
  for (std::uint32_t i = 0; i < pieces.size(); ++i) {
    const auto &row = pieces[i].row;
    const glm::dvec4 middle{(row.from[0] + row.to[0]) / 2.0,
                            (row.from[1] + row.to[1]) / 2.0,
                            (row.from[2] + row.to[2]) / 2.0, 1.0};
    const auto clip =
        glm::dmat4(beamSources[pieces[i].source].transform) * middle;
    order.push_back(Item{.w = clip.w, .z = clip.z, .index = i, .beam = true});
  }
  // Farthest first. Stable, so draws at one depth keep the order they were
  // added in, which is the order their owner built them in.
  std::ranges::stable_sort(order, [](const Item &a, const Item &b) {
    return a.w != b.w ? a.w > b.w : a.z > b.z;
  });

  uploadPieces();

  auto *const target = state.device;
  const auto atlas   = state.glyphCache.textureHandle();
  std::optional<render::PipelineHandle> bound;
  const auto bind = [&](const render::PipelineHandle pipeline) {
    if (bound && bound->id == pipeline.id) {
      return;
    }
    target->bindPipeline(pipeline);
    // A beam samples nothing, but every pipeline's descriptor set has the
    // atlas in it and Vulkan wants it filled whether or not it is read.
    target->bindAtlasTexture(atlas);
    bound = pipeline;
  };

  std::size_t at = 0;
  while (at < order.size()) {
    if (!order[at].beam) {
      // A run of sheets sharing a pipeline is one call, so a device that
      // records on several threads still can.
      const auto pipeline = sheets[order[at].index].pipeline;
      runBatches.clear();
      while (at < order.size() && !order[at].beam &&
             sheets[order[at].index].pipeline.id == pipeline.id) {
        runBatches.push_back(sheets[order[at].index].batch);
        ++at;
      }
      bind(pipeline);
      target->drawGlyphBatches(runBatches);
      stats.draws += runBatches.size();
      continue;
    }
    // A run of pieces of one batch is contiguous in the upload, because the
    // upload was written in this order.
    const auto source   = pieces[order[at].index].source;
    const auto first    = uploadedAt[order[at].index];
    std::uint32_t count = 0;
    while (at < order.size() && order[at].beam &&
           pieces[order[at].index].source == source) {
      ++count;
      ++at;
    }
    const auto &from = beamSources[source];
    bind(from.beams->pipelineHandle());
    target->drawGlyphs(render::DrawUniforms{.mvp      = toArray(from.transform),
                                            .opacity  = from.opacity,
                                            .identity = from.identity},
                       pool->buffer(),
                       pool->byteOffset(backing) +
                           (std::size_t{first} * sizeof(Beams::Row)),
                       count);
    ++stats.draws;
  }
}

} // namespace gleditor
