// Glyph vertex stage, shared by the OpenGL, OpenGL ES and Vulkan backends.
//
// The version directive, precision qualifiers and the uniform plumbing differ
// per backend and are prepended by the device implementation before
// compilation; see render/shader_source.cpp. Everything below is the common
// subset of GLSL 3.30, GLSL ES 3.00 and Vulkan GLSL.
//
// One glyph is one instance. There is no per-vertex data at all: the four
// corners of the quad are derived from the vertex index, which keeps the
// pipeline free of a geometry shader and therefore usable on OpenGL ES 3.0
// (which has no geometry stage) and cheap on Vulkan.
//
//   vertex 0 -> bottom left    vertex 1 -> bottom right
//   vertex 2 -> top left       vertex 3 -> top right
//
// drawn as a triangle strip, which is already counter-clockwise for both
// triangles and so survives face culling.

// Per-instance attributes. Locations must match VertexLayout in doc.hpp.
GLEDITOR_IN(0) vec3 position;   // centre of the glyph in model space
GLEDITOR_IN(1) uint fgcolor;    // packed RGBA8
GLEDITOR_IN(2) uint bgcolor;    // packed RGBA8
GLEDITOR_IN(3) vec2 texcoord;   // atlas origin of the glyph, normalised
GLEDITOR_IN(4) vec2 texBox;     // atlas extent of the glyph, normalised
GLEDITOR_IN(5) uint layerWH;    // layer:4 width:14 height:14
GLEDITOR_IN(6) uvec2 tag;       // picking identity

GLEDITOR_OUT(0) vec3 vFgColor;
GLEDITOR_OUT(1) vec3 vBgColor;
GLEDITOR_OUT(2) vec2 vTexCoord;
GLEDITOR_OUT(3) float vLayer;
GLEDITOR_OUT_FLAT(4) uvec2 vTag;

vec4 unpackColor(uint bits) {
    return vec4(float((bits >> 24) & 255u),
                float((bits >> 16) & 255u),
                float((bits >> 8) & 255u),
                float(bits & 255u)) / 255.0;
}

// Field widths must stay in step with Doc::VBORow::layerWidthHeight and with
// lwh() in doc.cpp.
vec3 unpackLayerWH(uint bits) {
    return vec3(float(bits >> 28),
                float((bits >> 14) & 16383u),
                float(bits & 16383u));
}

void main() {
    vec3 lwh = unpackLayerWH(layerWH);
    float halfWidth = lwh.y * 0.5;
    float halfHeight = lwh.z * 0.5;

    // A degenerate glyph box would rasterise nothing useful; collapse it to a
    // point outside clip space so the fragment stage never runs for it.
    if (0.0 == lwh.y || 0.0 == lwh.z) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        vFgColor = vec3(0.0);
        vBgColor = vec3(0.0);
        vTexCoord = vec2(0.0);
        vLayer = 0.0;
        vTag = uvec2(0u);
        return;
    }

    int corner = GLEDITOR_VERTEX_INDEX;
    // corner & 1 selects right, corner & 2 selects top
    float xSign = (0 != (corner & 1)) ? 1.0 : -1.0;
    float ySign = (0 != (corner & 2)) ? 1.0 : -1.0;

    vec4 offset = vec4(xSign * halfWidth, ySign * halfHeight, 0.0, 0.0);
    vec4 centre = uProjection * uView * uModel * vec4(position, 1.0);
    gl_Position = centre + uProjection * uView * uModel * offset;

    vec3 fg = unpackColor(fgcolor).rgb;
    vec3 bg = unpackColor(bgcolor).rgb;

    // Highlight lookup. Only glyph rows (tag.x == 3) carry a text offset in
    // tag.y; the loop is bounded by the block size so it cannot read past the
    // end of the array.
    if (3u == tag.x) {
        for (int i = 0; i < GLEDITOR_MAX_HIGHLIGHTS; i++) {
            if (0u == (uRanges[i].start | uRanges[i].end | uRanges[i].colour)) {
                break;
            }
            if (tag.y >= uRanges[i].start && tag.y <= uRanges[i].end) {
                bg = unpackColor(uRanges[i].colour).rgb;
            }
        }
    }

    vFgColor = fg;
    vBgColor = bg;
    // The atlas origin is the bottom-left of the glyph, so the texture
    // coordinate follows the same corner selection as the position.
    vTexCoord = texcoord + vec2((0 != (corner & 1)) ? texBox.x : 0.0,
                                (0 != (corner & 2)) ? texBox.y : 0.0);
    vLayer = lwh.x;
    vTag = tag;
}
// vi: set sw=4 sts=4 ts=4 et:
