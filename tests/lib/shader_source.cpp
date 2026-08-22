#include <gtest/gtest.h>

#include <gleditor/render/shader_source.hpp>
#include <gleditor/render/types.hpp>

#include <gmock/gmock.h>
#include <string>

using render::Backend;
using render::ShaderStage;
using testing::HasSubstr;
using testing::Not;

namespace {

std::string vertexPreamble(const Backend backend) {
  return render::assembleShaderSource(backend, ShaderStage::Vertex, "");
}
std::string fragmentPreamble(const Backend backend) {
  return render::assembleShaderSource(backend, ShaderStage::Fragment, "");
}

} // namespace

TEST(ShaderSource, backendNamesRoundTrip) {
  for (const auto backend :
       {Backend::OpenGL, Backend::OpenGLES, Backend::Vulkan}) {
    EXPECT_EQ(render::backendFromName(render::backendName(backend)), backend);
  }
}

TEST(ShaderSource, rejectsUnknownBackendName) {
  EXPECT_THROW(render::backendFromName("metal"), std::invalid_argument);
}

TEST(ShaderSource, eachBackendGetsItsOwnVersionDirective) {
  EXPECT_THAT(vertexPreamble(Backend::OpenGL), HasSubstr("#version 330 core"));
  EXPECT_THAT(vertexPreamble(Backend::OpenGLES), HasSubstr("#version 300 es"));
  EXPECT_THAT(vertexPreamble(Backend::Vulkan), HasSubstr("#version 450 core"));
}

// Desktop GLSL supplies default precision; GLSL ES does not, and a fragment
// shader without one fails to compile.
TEST(ShaderSource, onlyGlesDeclaresDefaultPrecision) {
  EXPECT_THAT(fragmentPreamble(Backend::OpenGLES),
              HasSubstr("precision highp float"));
  EXPECT_THAT(fragmentPreamble(Backend::OpenGLES),
              HasSubstr("precision highp sampler2DArray"));
  EXPECT_THAT(fragmentPreamble(Backend::OpenGL),
              Not(HasSubstr("precision highp")));
  EXPECT_THAT(fragmentPreamble(Backend::Vulkan),
              Not(HasSubstr("precision highp")));
}

// Vulkan requires a location on every interface variable; GLSL 3.30 and
// GLSL ES 3.00 reject them on varyings and match by name instead.
TEST(ShaderSource, onlyVulkanLocatesVaryings) {
  EXPECT_THAT(
      vertexPreamble(Backend::Vulkan),
      HasSubstr("#define GLEDITOR_OUT(loc) layout(location = loc) out"));
  EXPECT_THAT(vertexPreamble(Backend::OpenGL),
              HasSubstr("#define GLEDITOR_OUT(loc) out\n"));
  EXPECT_THAT(vertexPreamble(Backend::OpenGLES),
              HasSubstr("#define GLEDITOR_OUT(loc) out\n"));
}

TEST(ShaderSource, vertexAttributesAreLocatedOnEveryBackend) {
  for (const auto backend :
       {Backend::OpenGL, Backend::OpenGLES, Backend::Vulkan}) {
    EXPECT_THAT(vertexPreamble(backend),
                HasSubstr("#define GLEDITOR_IN(loc) layout(location = loc) in"))
        << render::backendName(backend);
  }
}

TEST(ShaderSource, vertexIndexBuiltinDiffersOnVulkan) {
  EXPECT_THAT(vertexPreamble(Backend::Vulkan),
              HasSubstr("GLEDITOR_VERTEX_INDEX gl_VertexIndex"));
  EXPECT_THAT(vertexPreamble(Backend::OpenGL),
              HasSubstr("GLEDITOR_VERTEX_INDEX gl_VertexID"));
}

// Vulkan has no default uniform block, so the transform and the sampler have to
// be declared through a push constant and descriptors instead.
TEST(ShaderSource, vulkanDeclaresUniformsThroughDescriptors) {
  const auto vert = vertexPreamble(Backend::Vulkan);
  EXPECT_THAT(vert, HasSubstr("layout(push_constant) uniform Push"));
  const auto frag = fragmentPreamble(Backend::Vulkan);
  // The highlight table is read per fragment -- selection depends on where
  // inside a quad a fragment sits -- so it is declared in that stage, not the
  // vertex one.
  EXPECT_THAT(
      frag,
      HasSubstr("layout(set = 0, binding = 0, std140) uniform Highlights"));
  EXPECT_THAT(frag,
              HasSubstr("layout(set = 0, binding = 1) uniform sampler2DArray"));
  EXPECT_THAT(vert, testing::Not(HasSubstr("Highlights")))
      << "a vertex-stage decision could only select whole clusters";
}

TEST(ShaderSource, glFamilyDeclaresPlainUniforms) {
  for (const auto backend : {Backend::OpenGL, Backend::OpenGLES}) {
    EXPECT_THAT(vertexPreamble(backend), HasSubstr("uniform mat4 uMVP;"))
        << render::backendName(backend);
    EXPECT_THAT(fragmentPreamble(backend),
                HasSubstr("uniform sampler2DArray uGlyphAtlas;"))
        << render::backendName(backend);
  }
}

// The transform is one matrix per draw rather than a per-frame camera plus a
// per-draw model, which is what lets a screen-space overlay be drawn in the
// same frame as a perspective document. A camera block reappearing here would
// mean per-frame storage had come back with it.
TEST(ShaderSource, noBackendDeclaresASeparateCameraBlock) {
  for (const auto backend :
       {Backend::OpenGL, Backend::OpenGLES, Backend::Vulkan}) {
    const auto vert = vertexPreamble(backend);
    EXPECT_THAT(vert, testing::Not(HasSubstr("uProjection")))
        << render::backendName(backend);
    EXPECT_THAT(vert, testing::Not(HasSubstr("uView")))
        << render::backendName(backend);
    EXPECT_THAT(vert, HasSubstr("uMVP")) << render::backendName(backend);
  }
}

// The C++ side sizes the highlight buffer from maxHighlightRanges; if the
// shader's array were a different length the two would disagree silently.
TEST(ShaderSource, highlightArrayMatchesTheHostSideBound) {
  const auto expected =
      "uRanges[" + std::to_string(render::maxHighlightRanges) + "]";
  for (const auto backend :
       {Backend::OpenGL, Backend::OpenGLES, Backend::Vulkan}) {
    EXPECT_THAT(fragmentPreamble(backend), HasSubstr(expected))
        << render::backendName(backend);
    EXPECT_THAT(vertexPreamble(backend),
                HasSubstr("#define GLEDITOR_MAX_HIGHLIGHTS " +
                          std::to_string(render::maxHighlightRanges)))
        << render::backendName(backend);
  }
}

TEST(ShaderSource, bodyIsAppendedAfterThePreamble) {
  const auto out = render::assembleShaderSource(
      Backend::OpenGL, ShaderStage::Vertex, "void main() { }\n");
  EXPECT_THAT(out, HasSubstr("void main() { }"));
  EXPECT_LT(out.find("#version"), out.find("void main"));
}

TEST(ShaderSource, missingShaderFileReportsThePath) {
  EXPECT_THROW(render::readShaderBody("/nonexistent/glyph.vert.glsl"),
               std::runtime_error);
}

TEST(ShaderSource, glyphShadersAssembleWithVerticalMultiBanding) {
  const auto vertBody =
      render::readShaderBody("assets/shaders/glyph.vert.glsl");
  const auto fragBody =
      render::readShaderBody("assets/shaders/glyph.frag.glsl");

  for (const auto backend :
       {Backend::OpenGL, Backend::OpenGLES, Backend::Vulkan}) {
    const auto vertSource =
        render::assembleShaderSource(backend, ShaderStage::Vertex, vertBody);
    const auto fragSource =
        render::assembleShaderSource(backend, ShaderStage::Fragment, fragBody);

    EXPECT_THAT(vertSource, HasSubstr("vQuadV"));
    EXPECT_THAT(fragSource, HasSubstr("vQuadV"));
    EXPECT_THAT(fragSource, HasSubstr("GLEDITOR_MAX_OVERLAPPING_HIGHLIGHTS"));
    EXPECT_THAT(fragSource, HasSubstr("selectedBackground"));
  }
}
