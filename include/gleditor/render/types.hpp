/**
 * @file types.hpp
 * @brief Backend-neutral types shared by every RenderDevice implementation.
 *
 * Nothing in this header may name an OpenGL, GLES or Vulkan type. Application
 * code (Doc, Page, GlyphCache, ...) speaks only in terms of what lives here,
 * which is what lets the same document rendering run on any backend.
 */
#ifndef GLEDITOR_RENDER_TYPES_H
#define GLEDITOR_RENDER_TYPES_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace render {

/**
 * @brief Which rendering API a device talks to.
 */
enum class Backend : std::uint8_t {
  OpenGL,   ///< Desktop OpenGL 3.3 core or newer.
  OpenGLES, ///< OpenGL ES 3.0 or newer.
  Vulkan,   ///< Vulkan 1.0 or newer.
};

/// Parse a backend name as accepted on the command line. Throws
/// std::invalid_argument for anything unrecognised.
Backend backendFromName(const std::string &name);
/// Canonical lowercase name of @p backend, the inverse of backendFromName().
std::string backendName(Backend backend);

/**
 * @brief A typed, opaque reference to a device-owned resource.
 *
 * Backends map the id onto whatever they actually use -- a GL object name, an
 * index into a table of Vulkan handles -- so callers can copy these around
 * freely without knowing the backing representation. Zero is the null handle.
 */
template <typename Tag> struct Handle {
  std::uint32_t id{};

  [[nodiscard]] bool valid() const { return 0 != id; }
  explicit operator bool() const { return valid(); }
  bool operator==(const Handle &oth) const = default;
};

struct BufferTag;
struct TextureTag;
struct PipelineTag;

using BufferHandle   = Handle<BufferTag>;
using TextureHandle  = Handle<TextureTag>;
using PipelineHandle = Handle<PipelineTag>;

/**
 * @brief What a buffer is used for, which decides its binding point and, on
 *        Vulkan, its usage flags and memory properties.
 */
enum class BufferKind : std::uint8_t {
  Vertex,   ///< Per-instance glyph attributes.
  Index,    ///< Element indices.
  Uniform,  ///< Shader-visible uniform block storage.
  Readback, ///< Transfer destination the host reads, such as a picking result.
};

/**
 * @brief Pixel format of a texture.
 *
 * Only the single-channel coverage format the glyph cache needs is defined;
 * every backend can represent it identically, which keeps the upload path free
 * of per-backend swizzling.
 */
enum class TextureFormat : std::uint8_t {
  R8, ///< One unsigned normalised byte per texel.
};

/**
 * @brief Device limits the glyph cache needs in order to size its atlas.
 */
struct TextureLimits {
  int maxSize{};   ///< Largest supported width/height of an array texture.
  int maxLayers{}; ///< Largest supported number of array layers.
};

/**
 * @brief Scalar type of a vertex attribute component.
 */
enum class AttributeType : std::uint8_t {
  Float,       ///< 32-bit float, consumed as float in the shader.
  UnsignedInt, ///< 32-bit unsigned integer, consumed as uint in the shader.
};

/**
 * @brief One vertex attribute within the per-instance vertex layout.
 */
struct VertexAttribute {
  std::string name;    ///< Name used to look the attribute up when the backend
                       ///< binds by name rather than by location.
  std::uint32_t location{}; ///< Shader location.
  AttributeType type{AttributeType::Float};
  int components{};         ///< 1..4.
  std::uint32_t offset{};   ///< Byte offset within the vertex structure.
};

/**
 * @brief Description of the per-instance vertex buffer layout.
 *
 * Every attribute advances once per instance: the pipeline draws a unit quad
 * whose corners come from the vertex index, so there is no per-vertex data at
 * all.
 */
struct VertexLayout {
  std::uint32_t stride{};
  std::vector<VertexAttribute> attributes;
};

/**
 * @brief Everything needed to build the glyph rendering pipeline.
 */
struct PipelineDesc {
  std::string name;           ///< Diagnostic name.
  std::string vertexSource;   ///< Portable GLSL body for the vertex stage.
  std::string fragmentSource; ///< Portable GLSL body for the fragment stage.
  /// Directory holding precompiled SPIR-V for backends that cannot consume
  /// GLSL directly. Ignored by the GL and GLES backends.
  std::string spirvDir;
  VertexLayout layout;
  /**
   * @brief Whether fragments are depth tested and depth written.
   *
   * Off for overlays: a screen-space notification has no meaningful depth to
   * compare against a perspective document, so it relies on being submitted
   * last instead. Leaving it on would need the overlay's clip-space Z to be
   * closer than everything else on both the OpenGL [-1,1] and Vulkan [0,1]
   * depth conventions, which no single value satisfies.
   */
  bool depthTest{true};
};

/**
 * @brief Uniform values that change per draw call.
 *
 * The whole transform is carried here rather than split into camera and model
 * halves. Splitting it would put the camera in per-frame storage that every
 * draw of the frame shares -- a descriptor-backed uniform block on Vulkan --
 * and a draw that wants a different transform, such as a screen-space overlay
 * drawn over a perspective document, could then not have one. Combining them
 * costs one matrix multiply on the host per draw and removes a uniform buffer,
 * a descriptor binding and a device entry point.
 */
struct DrawUniforms {
  /// projection * view * model, in the column-major order GLSL expects.
  std::array<float, 16> mvp{};
};

/**
 * @brief A colour target read back to host memory.
 *
 * Rows run top to bottom and pixels are tightly packed RGBA8, so the contents
 * are directly comparable between backends regardless of how each one stores
 * its framebuffer.
 */
struct FrameImage {
  int width{};
  int height{};
  std::vector<std::uint8_t> rgba;
};

/**
 * @brief Identity of whatever was drawn at a queried pixel.
 *
 * The glyph pipeline writes this to its second colour attachment, so every
 * fragment carries the identity of the thing that produced it. Values come from
 * Doc::VBORow::tag: kind 2 is a page background and kind 3 a glyph, whose index
 * is the byte offset of the glyph's cluster within the page text.
 */
struct PickingTag {
  /// 0 when nothing was drawn, 2 for a page background, 3 for a glyph.
  std::uint32_t kind{};
  /// Which open document the fragment belongs to.
  std::uint32_t docIndex{};
  /// Which page of that document.
  std::uint32_t pageIndex{};
  /// Index into the page's cluster table. One quad is one cluster, which may
  /// cover several characters, so this names the cluster and not a character.
  std::uint32_t clusterIndex{};
  /**
   * @brief Where across the picked quad the fragment sat, 0 at its left edge
   *        and 1 at its right.
   *
   * This is what makes a click inside a ligature resolvable. A quad may stand
   * for "ffi" or for a letter carrying combining marks, and the character
   * boundaries inside it have no fragment of their own to pick; the fraction
   * says how far along the caller landed, and the cluster's character count
   * says how many boundaries to divide that among.
   */
  float fraction{};

  /// True when nothing was drawn at the queried pixel.
  [[nodiscard]] bool empty() const { return 0 == kind; }
  /// Position only: two results at the same place with different fractions are
  /// the same object, which is what "has the cursor moved onto something new"
  /// asks.
  [[nodiscard]] bool sameObject(const PickingTag &oth) const {
    return kind == oth.kind && docIndex == oth.docIndex &&
           pageIndex == oth.pageIndex && clusterIndex == oth.clusterIndex;
  }
  bool operator==(const PickingTag &oth) const = default;
};

/// Tag kinds. Zero means nothing was drawn, so it is not usable as an
/// identity: an overlay tagged zero would report "no object" and be
/// indistinguishable from empty space.
inline constexpr std::uint32_t tagKindNone    = 0;
inline constexpr std::uint32_t tagKindOverlay = 1;
inline constexpr std::uint32_t tagKindPage    = 2;
inline constexpr std::uint32_t tagKindGlyph   = 3;

/// Bit widths of the identity word a glyph instance carries. Packed rather
/// than given a word each because the attachment is four words wide and the
/// cluster index and the fraction need one each.
inline constexpr std::uint32_t tagKindBits = 4;
inline constexpr std::uint32_t tagDocBits  = 14;
inline constexpr std::uint32_t tagPageBits = 14;

/// Pack the identity word written to Doc::VBORow::tag[0].
inline constexpr std::uint32_t packTagIdentity(const std::uint32_t kind,
                                               const std::uint32_t docIndex,
                                               const std::uint32_t pageIndex) {
  return ((kind & ((1U << tagKindBits) - 1U)) << (tagDocBits + tagPageBits)) |
         ((docIndex & ((1U << tagDocBits) - 1U)) << tagPageBits) |
         (pageIndex & ((1U << tagPageBits) - 1U));
}

/// Scale the fragment stage encodes the quad fraction with. Fixed point rather
/// than a float because the attachment holds unsigned integers.
inline constexpr std::uint32_t tagFractionScale = 65535;

/**
 * @brief How many character boundaries into a cluster a fraction falls.
 *
 * A cluster drawn as one quad may cover several characters -- a ligature, or a
 * letter with combining marks -- and the boundaries between them have no
 * geometry of their own to pick. Dividing the quad evenly is what Pango's own
 * x_to_index does, so a click resolves the way Pango would resolve it.
 *
 * Rounding rather than truncating puts the caret on the nearer boundary, so the
 * left half of a character lands before it and the right half after it. For a
 * three-character cluster that puts the steps at a sixth, a half and five
 * sixths of the quad.
 */
inline std::uint32_t clusterCharStep(const std::uint32_t charCount,
                                     const float fraction) {
  if (0 == charCount) {
    return 0;
  }
  const auto clamped = fraction < 0.0F ? 0.0F : (fraction > 1.0F ? 1.0F : fraction);
  const auto scaled  = static_cast<double>(clamped) * charCount;
  const auto steps   = static_cast<std::uint32_t>(scaled + 0.5);
  return steps > charCount ? charCount : steps;
}

/// Decode the four words read back from the picking attachment.
inline PickingTag unpackPickingTag(const std::uint32_t identity,
                                   const std::uint32_t clusterIndex,
                                   const std::uint32_t fraction) {
  PickingTag tag;
  tag.kind      = identity >> (tagDocBits + tagPageBits);
  tag.docIndex  = (identity >> tagPageBits) & ((1U << tagDocBits) - 1U);
  tag.pageIndex = identity & ((1U << tagPageBits) - 1U);
  tag.clusterIndex = clusterIndex;
  tag.fraction     = static_cast<float>(fraction) /
                 static_cast<float>(tagFractionScale);
  return tag;
}

/**
 * @brief A completed picking query.
 *
 * Readback is asynchronous, so a result names the pixel it came from: by the
 * time it arrives the cursor has usually moved on, and a caller that assumed
 * otherwise would attribute the tag to the wrong position.
 */
struct PickingResult {
  int x{};
  int y{};
  PickingTag tag;
};

/**
 * @brief A highlight range, matching the shader's std140 block element.
 *
 * Array members of a std140 uniform block are padded to 16 bytes, so the
 * trailing member is present to make the C++ and GLSL layouts agree.
 */
struct HighlightRange {
  std::uint32_t start{};
  std::uint32_t end{};
  std::uint32_t colour{};
  std::uint32_t reserved{};
};

/// Number of highlight ranges the uniform block holds. 1024 entries at 16
/// bytes each is exactly 16 KiB, the smallest maximum uniform block size any
/// Vulkan implementation is permitted to advertise, so the block fits
/// everywhere without a capability check.
inline constexpr int maxHighlightRanges = 1024;

} // namespace render

#endif // GLEDITOR_RENDER_TYPES_H
// vi: set sw=2 sts=2 ts=2 et:
