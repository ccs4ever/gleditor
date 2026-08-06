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
  std::uint32_t kind{};
  std::uint32_t index{};

  /// True when nothing was drawn at the queried pixel.
  [[nodiscard]] bool empty() const { return 0 == kind && 0 == index; }
  bool operator==(const PickingTag &oth) const = default;
};

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
