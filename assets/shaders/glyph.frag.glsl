// Glyph fragment stage, shared by the OpenGL, OpenGL ES and Vulkan backends.
// See glyph.vert.glsl for how the per-backend preamble is applied.
//
// The glyph atlas is a single-channel coverage texture: the rasterised glyph
// contributes the blend factor between the background and foreground colour.
// The second output carries the picking identity of the glyph.

GLEDITOR_IN(0) vec3 vFgColor;
GLEDITOR_IN(1) vec3 vBgColor;
GLEDITOR_IN(2) vec2 vTexCoord;
GLEDITOR_IN(3) float vLayer;
GLEDITOR_IN_FLAT(4) uvec2 vTag;

GLEDITOR_FRAG_OUT(0) vec4 outColor;
GLEDITOR_FRAG_OUT(1) uvec2 outTag;

void main() {
    float coverage =
        texture(uGlyphAtlas, vec3(vTexCoord, floor(vLayer + 0.5))).r;
    outColor = vec4(mix(vBgColor, vFgColor, coverage), 1.0);
    outTag = vTag;
}
// vi: set sw=4 sts=4 ts=4 et:
