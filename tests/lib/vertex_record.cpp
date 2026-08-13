/**
 * @file vertex_record.cpp
 * @brief The packing every quad the renderer draws is written in.
 *
 * Doc::VBORow is twenty-four bytes with nine fields in it, and the shader
 * unpacks those fields by shifting and masking a copy of the same layout
 * written in GLSL. Nothing links the two: a field moved here and not there
 * produces glyphs of the wrong size in the wrong place, and no compiler
 * anywhere will say so.
 *
 * So the unpacking is written out again below, as the shader does it, and the
 * packing is checked against it. That is a test of the thing that can actually
 * break -- the two descriptions agreeing -- rather than of the arithmetic.
 */
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <gleditor/doc.hpp>
#include <gleditor/render/types.hpp>

namespace {

using Row = Doc::VBORow;

/// unpackQuad() from assets/shaders/glyph.vert.glsl.
struct Quad {
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t layer{};
  std::uint32_t kind{};
};
Quad unpackQuad(const std::uint32_t bits) {
  return {bits >> 20, (bits >> 8) & 4095U, (bits >> 2) & 63U, bits & 3U};
}

/// unpackInk() from the same file, in eight-bit channels rather than floats.
struct Ink {
  std::uint32_t red{};
  std::uint32_t green{};
  std::uint32_t blue{};
  std::uint32_t flags{};
};
Ink unpackInk(const std::uint32_t bits) {
  return {(bits >> 24) & 255U, (bits >> 16) & 255U, (bits >> 8) & 255U,
          bits & 255U};
}

/// unpackPaper(), scaled back to eight bits so it can be compared with what
/// went in.
struct Paper {
  std::uint32_t red{};
  std::uint32_t green{};
  std::uint32_t blue{};
};
Paper unpackPaper(const std::uint32_t bits) {
  const auto scale = [](const std::uint32_t value, const std::uint32_t most) {
    return (value * 255U) / most;
  };
  return {scale((bits >> 11) & 31U, 31U), scale((bits >> 5) & 63U, 63U),
          scale(bits & 31U, 31U)};
}

TEST(VertexRecord, isTwentyFourBytesAndDescribedAsSuch) {
  // The size is the point of the packing: a megabyte of text is a little over
  // a million of these, so every byte here is a megabyte across a document.
  EXPECT_EQ(sizeof(Row), 24U);

  const auto layout = Doc::vertexLayout();
  EXPECT_EQ(layout.stride, sizeof(Row));
  ASSERT_EQ(layout.attributes.size(), 5U);
  // Locations must be the ones the shader declares, and the offsets must be
  // the fields they name.
  EXPECT_EQ(layout.attributes[0].offset, offsetof(Row, pos));
  EXPECT_EQ(layout.attributes[1].offset, offsetof(Row, foreground));
  EXPECT_EQ(layout.attributes[2].offset, offsetof(Row, atlas));
  EXPECT_EQ(layout.attributes[3].offset, offsetof(Row, quad));
  EXPECT_EQ(layout.attributes[4].offset, offsetof(Row, paper));
  for (std::uint32_t i = 0; i < layout.attributes.size(); i++) {
    EXPECT_EQ(layout.attributes[i].location, i);
  }
}

TEST(VertexRecord, aQuadSaysItsSizeItsLayerAndItsKind) {
  const auto packed = Row::box(37, 1234, 4095, render::tagKindGlyph);
  const auto quad   = unpackQuad(packed);
  EXPECT_EQ(quad.width, 1234U);
  EXPECT_EQ(quad.height, 4095U);
  EXPECT_EQ(quad.layer, 37U);
  EXPECT_EQ(quad.kind, render::tagKindGlyph);

  // The extremes of every field at once, which is where a field that has crept
  // into its neighbour shows up.
  const auto full =
      unpackQuad(Row::box(Row::maxAtlasLayers - 1, Row::maxQuadExtent,
                          Row::maxQuadExtent, render::tagKindGlyph));
  EXPECT_EQ(full.width, Row::maxQuadExtent);
  EXPECT_EQ(full.height, Row::maxQuadExtent);
  EXPECT_EQ(full.layer, Row::maxAtlasLayers - 1);
  EXPECT_EQ(full.kind, render::tagKindGlyph);
}

TEST(VertexRecord, inkKeepsAllEightBitsOfEveryChannel) {
  // Ink is what a glyph and a greeked line are drawn in, and the bars that
  // stand in for lines are shaded by how much ink each line carries. Rounding
  // them would band a page seen from a distance, so this field is exact.
  const auto ink =
      unpackInk(Row::ink(Row::color3(17, 200, 3), Row::onText, false));
  EXPECT_EQ(ink.red, 17U);
  EXPECT_EQ(ink.green, 200U);
  EXPECT_EQ(ink.blue, 3U);

  for (unsigned int shade = 0; shade < 256; shade++) {
    const auto grey = unpackInk(
        Row::fill(Row::color(static_cast<unsigned char>(shade)), Row::onPaper));
    EXPECT_EQ(grey.red, shade);
    EXPECT_EQ(grey.green, shade);
    EXPECT_EQ(grey.blue, shade);
  }
}

TEST(VertexRecord, flagsRideInTheByteTheAlphaUsedToOccupy) {
  const auto glyph = unpackInk(Row::ink(Row::color(0), Row::onText, false));
  EXPECT_EQ(glyph.flags & Row::depthMask, Row::onText);
  EXPECT_EQ(glyph.flags & Row::solidFlag, 0U);

  const auto caret = unpackInk(Row::fill(Row::color(255), Row::onTop));
  EXPECT_EQ(caret.flags & Row::depthMask, Row::onTop);
  EXPECT_NE(caret.flags & Row::solidFlag, 0U);
  // Setting the flags must not have disturbed the colour above them.
  EXPECT_EQ(caret.red, 255U);
  EXPECT_EQ(caret.blue, 255U);
}

TEST(VertexRecord, paperRoundsAndWhiteStaysWhite) {
  // Paper is a flat fill behind text, so five, six and five bits of it is
  // enough -- but only if white comes back as white. A page is white, and
  // paper a shade under it would be visible as a grey page.
  const auto white = unpackPaper(Row::paperAt(Row::color(255), 0) >> 16);
  EXPECT_EQ(white.red, 255U);
  EXPECT_EQ(white.green, 255U);
  EXPECT_EQ(white.blue, 255U);

  const auto black = unpackPaper(Row::paperAt(Row::color(0), 0) >> 16);
  EXPECT_EQ(black.red, 0U);
  EXPECT_EQ(black.green, 0U);
  EXPECT_EQ(black.blue, 0U);

  // Everything else is within half a step of the field it was narrowed to --
  // five bits for red and blue, six for green -- which is what says the error
  // is quantisation and not a field written over its neighbour.
  for (unsigned int value = 0; value < 256; value++) {
    const auto shade = static_cast<unsigned char>(value);
    const auto back =
        unpackPaper(Row::paperAt(Row::color3(shade, shade, shade), 0) >> 16);
    EXPECT_LE(std::abs(static_cast<int>(back.red) - static_cast<int>(value)),
              5);
    EXPECT_LE(std::abs(static_cast<int>(back.green) - static_cast<int>(value)),
              2);
    EXPECT_LE(std::abs(static_cast<int>(back.blue) - static_cast<int>(value)),
              5);
  }
}

TEST(VertexRecord, theClusterSharesAWordWithPaperWithoutTouchingIt) {
  const auto packed = Row::paperAt(Row::color3(24, 26, 34), 40000);
  EXPECT_EQ(packed & 65535U, 40000U);
  const auto colour = unpackPaper(packed >> 16);
  EXPECT_LE(std::abs(static_cast<int>(colour.red) - 24), 5);
  EXPECT_LE(std::abs(static_cast<int>(colour.green) - 26), 2);
  EXPECT_LE(std::abs(static_cast<int>(colour.blue) - 34), 5);

  // The largest cluster a page may hold must still be its own value, since a
  // page is cut short rather than allowed to run past it.
  EXPECT_EQ(Row::paperAt(Row::color(255), Row::maxClustersPerPage) & 65535U,
            Row::maxClustersPerPage);
}

TEST(VertexRecord, atlasCoordinatesSurviveALargeAtlas) {
  const auto packed = Row::atlasAt(60000, 3);
  EXPECT_EQ(packed >> 16, 60000U);
  EXPECT_EQ(packed & 65535U, 3U);
}

// What BufferPool::eraseRows relies on. A row deleted from the middle of a
// page is zeroed rather than removed, so that the page stays a single range
// the draw can be aimed at; that is only safe if a row of zeroes draws
// nothing. The vertex stage tests exactly this -- "0.0 == whlk.x || 0.0 ==
// whlk.y" -- and collapses the quad to a point outside clip space, so it
// rasterises no fragment and writes no picking tag.
TEST(VertexRecord, aZeroedRowIsAQuadWithNothingInIt) {
  const Row erased{};
  const auto quad = unpackQuad(erased.quad);
  EXPECT_EQ(quad.width, 0U);
  EXPECT_EQ(quad.height, 0U);
  // And it claims to be nothing rather than claiming to be a glyph, so even a
  // stage that did draw it could not answer a click with it.
  EXPECT_EQ(quad.kind, render::tagKindNone);

  // Every byte of it, since that is what the pool writes.
  const std::array<std::byte, sizeof(Row)> raw{};
  Row fromZeroes{};
  std::memcpy(&fromZeroes, raw.data(), raw.size());
  const auto same = unpackQuad(fromZeroes.quad);
  EXPECT_EQ(same.width, 0U);
  EXPECT_EQ(same.height, 0U);
}

// The reason the identity is split at all: three quarters of it is the same
// for everything one draw covers, so it is said once in the draw's uniforms
// instead of in every one of a million instances.
TEST(VertexRecord, theDrawsIdentityAndTheQuadsKindRebuildTheWholeTag) {
  constexpr std::uint32_t kindShift = render::tagDocBits + render::tagPageBits;

  for (const auto kind :
       {render::tagKindOverlay, render::tagKindPage, render::tagKindGlyph}) {
    for (const std::uint32_t doc : {0U, 1U, 4095U}) {
      for (const std::uint32_t page : {0U, 7U, 16383U}) {
        // What Page and Canvas hand the draw, with no kind in it.
        const auto base = render::packTagIdentity(0, doc, page);
        // What the vertex stage assembles from that and the quad's two bits.
        const auto assembled = base | (kind << kindShift);
        EXPECT_EQ(assembled, render::packTagIdentity(kind, doc, page));

        const auto tag = render::unpackPickingTag(assembled, 0, 0);
        EXPECT_EQ(tag.kind, kind);
        EXPECT_EQ(tag.docIndex, doc);
        EXPECT_EQ(tag.pageIndex, page);
      }
    }
  }
}

} // namespace

// vi: set sw=2 sts=2 ts=2 et:
