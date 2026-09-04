// Image fragment stage. See image.vert.glsl for the geometry.
//
// Unlike the glyph atlas, which is a single-channel coverage mask blended
// between a foreground and background colour, the image atlas holds full
// RGBA8 colour: this stage samples it directly and multiplies by the
// instance's tint, which is what lets a caller fade or recolour an image
// without a second draw.
//
// uGlyphAtlas is declared by the shared preamble under that name regardless
// of which pipeline this shader belongs to (see uniformBlock() in
// render/shader_source.cpp); it names whichever texture bindAtlasTexture()
// last bound, which for this pipeline is the image cache's atlas, not the
// glyph one.

GLEDITOR_IN(0) vec4 vTint;
GLEDITOR_IN(1) vec2 vTexCoord;
GLEDITOR_IN(2) float vLayer;
GLEDITOR_IN_FLAT(3) uvec2 vTag;
GLEDITOR_IN(4) float vOpacity;

GLEDITOR_FRAG_OUT(0) vec4 outColor;
// identity word, picking index, unused, unused -- the same shape
// glyph.frag.glsl and beam.frag.glsl write.
GLEDITOR_FRAG_OUT(1) uvec4 outTag;

void main() {
  vec4 sampled = texture(uGlyphAtlas, vec3(vTexCoord, floor(vLayer + 0.5)));
  outColor     = vec4(sampled.rgb * vTint.rgb, sampled.a * vTint.a * vOpacity);
  outTag       = uvec4(vTag, 0u, 0u);
}
