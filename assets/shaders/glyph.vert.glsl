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
//
// The instance is twenty-four bytes and every field in it is packed, because a
// megabyte of text is a million instances and four bytes of waste is another
// four megabytes of vertex buffer. The unpacking below must stay in step with
// Doc::VBORow, which does the packing.

// Per-instance attributes. Locations must match VertexLayout in doc.hpp.
GLEDITOR_IN(0) vec2 position;   // centre of the quad in model space
GLEDITOR_IN(1) uint foreground; // r:8 g:8 b:8, then flags:8
GLEDITOR_IN(2) uint atlas;      // texel x:16, texel y:16
GLEDITOR_IN(3) uint quad;       // width:12 height:12 layer:6 kind:2
GLEDITOR_IN(4) uint paper;      // paper colour as RGB565:16, cluster:16

GLEDITOR_OUT(0) vec3 vFgColor;
GLEDITOR_OUT(1) vec3 vBgColor;
GLEDITOR_OUT(2) vec2 vTexCoord;
GLEDITOR_OUT(3) float vLayer;
GLEDITOR_OUT_FLAT(4) uvec2 vTag;
// Where across the quad this vertex sits, 0 at the left edge and 1 at the
// right. Interpolated, unlike the tag, because the whole point is to know
// where inside the quad a fragment landed: one quad can be a whole ligature,
// and the character boundaries within it have no geometry of their own.
GLEDITOR_OUT(5) float vQuadU;
// Alpha for the whole draw. Carried through the vertex stage rather than read
// straight from a fragment uniform so that the per-draw block stays visible to
// one stage only, which on Vulkan is what keeps it a vertex-stage push
// constant rather than a range shared with the fragment stage.
GLEDITOR_OUT(6) float vOpacity;
// Whether this quad is a solid fill rather than a glyph. Flat, and used by the
// fragment stage to skip the atlas entirely: a page background covers more
// fragments than all its text put together, and none of them need a sample.
GLEDITOR_OUT_FLAT(7) uint vSolid;

// Field widths must stay in step with Doc::VBORow.
const uint depthMask = 3u;
const uint solidFlag = 4u;
// Distance between depth steps, matching Doc::VBORow::depthStep. Paper sits on
// step zero, the text on it one step in front, the caret one further.
const float depthStep = 0.1;

vec3 unpackInk(uint bits) {
    return vec3(float((bits >> 24) & 255u),
                float((bits >> 16) & 255u),
                float((bits >> 8) & 255u)) / 255.0;
}

// Paper keeps five bits of red, six of green and five of blue: it is a flat
// fill behind text, so it has no gradient for the rounding to band. Scaled by
// the largest value of each field so that white stays exactly white.
vec3 unpackPaper(uint bits) {
    return vec3(float((bits >> 11) & 31u) / 31.0,
                float((bits >> 5) & 63u) / 63.0,
                float(bits & 31u) / 31.0);
}

// width, height, layer, kind.
vec4 unpackQuad(uint bits) {
    return vec4(float(bits >> 20),
                float((bits >> 8) & 4095u),
                float((bits >> 2) & 63u),
                float(bits & 3u));
}

void main() {
    vec4 whlk = unpackQuad(quad);
    float halfWidth = whlk.x * 0.5;
    float halfHeight = whlk.y * 0.5;
    uint flags = foreground & 255u;

    // A degenerate glyph box would rasterise nothing useful; collapse it to a
    // point outside clip space so the fragment stage never runs for it.
    if (0.0 == whlk.x || 0.0 == whlk.y) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        vFgColor = vec3(0.0);
        vBgColor = vec3(0.0);
        vTexCoord = vec2(0.0);
        vLayer = 0.0;
        vTag = uvec2(0u);
        vQuadU = 0.0;
        vSolid = 1u;
        return;
    }

    int corner = GLEDITOR_VERTEX_INDEX;
    // corner & 1 selects right, corner & 2 selects top
    float xSign = (0 != (corner & 1)) ? 1.0 : -1.0;
    float ySign = (0 != (corner & 2)) ? 1.0 : -1.0;

    // The corner offset is a direction, not a point: its w is zero, so adding
    // it to the centre in model space and transforming once is the same as
    // transforming both and adding, for one matrix multiply instead of two.
    vec4 offset = vec4(xSign * halfWidth, ySign * halfHeight, 0.0, 0.0);
    vec3 centre = vec3(position, float(flags & depthMask) * depthStep);
    gl_Position = uMVP * (vec4(centre, 1.0) + offset);

    // Selection is decided in the fragment stage, not here: whether a
    // fragment is selected depends on where inside its quad it sits, and a
    // vertex only knows its corner. Deciding per instance could only ever
    // select whole clusters, and one cluster can be a ligature covering
    // several characters.

    vFgColor = unpackInk(foreground);
    vBgColor = unpackPaper(paper >> 16);
    // The atlas origin is the bottom-left of the glyph, so the texture
    // coordinate follows the same corner selection as the position. How far it
    // reaches is not carried at all: the atlas holds a glyph at its own size,
    // so the box on the page is the box in texels. Texels rather than a
    // fraction, divided by the atlas size in the fragment stage, because the
    // atlas grows as glyphs arrive and a fraction baked in here would point
    // somewhere else the moment it did.
    vTexCoord = vec2(float(atlas >> 16), float(atlas & 65535u)) +
                vec2((0 != (corner & 1)) ? whlk.x : 0.0,
                     (0 != (corner & 2)) ? whlk.y : 0.0);
    vLayer = whlk.z;
    // The document and page are the draw's; the kind is the quad's. Assembled
    // here so the fragment stage sees one identity word, as it did when every
    // instance carried the whole thing.
    vTag = uvec2(uIdentity | (uint(whlk.w) << GLEDITOR_TAG_KIND_SHIFT),
                 paper & 65535u);
    vQuadU = (0 != (corner & 1)) ? 1.0 : 0.0;
    vOpacity = uOpacity;
    vSolid = (0u != (flags & solidFlag)) ? 1u : 0u;
}
// vi: set sw=4 sts=4 ts=4 et:
