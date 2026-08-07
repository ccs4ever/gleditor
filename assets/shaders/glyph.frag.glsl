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
GLEDITOR_IN(5) float vQuadU;
GLEDITOR_IN(6) float vOpacity;

GLEDITOR_FRAG_OUT(0) vec4 outColor;
// identity word, cluster index, fractional position across the quad, unused.
GLEDITOR_FRAG_OUT(1) uvec4 outTag;

vec4 unpackColor(uint bits) {
    return vec4(float((bits >> 24) & 255u),
                float((bits >> 16) & 255u),
                float((bits >> 8) & 255u),
                float(bits & 255u)) / 255.0;
}

// Background for this fragment, which is the selection colour when the
// fragment falls inside a highlighted span.
//
// A span names clusters, because that is what a glyph instance carries, plus a
// fraction at each end for when the boundary falls inside one. vQuadU says
// where across the quad this fragment sits, so a span ending between the "f"
// and the "i" of an "fi" ligature highlights the "f" alone -- the two share a
// single quad and have no geometry to tell them apart.
vec3 selectedBackground(vec3 base) {
    for (int i = 0; i < GLEDITOR_MAX_HIGHLIGHTS; i++) {
        // Identity zero is not a glyph, so it terminates the list.
        if (0u == uRanges[i].identity) {
            break;
        }
        if (uRanges[i].identity != vTag.x) {
            continue;
        }
        if (vTag.y < uRanges[i].firstCluster || vTag.y > uRanges[i].lastCluster) {
            continue;
        }
        float lo = (vTag.y == uRanges[i].firstCluster) ? uRanges[i].startFraction : 0.0;
        float hi = (vTag.y == uRanges[i].lastCluster) ? uRanges[i].endFraction : 1.0;
        if (vQuadU >= lo && vQuadU < hi) {
            return unpackColor(uRanges[i].colour).rgb;
        }
    }
    return base;
}

void main() {
    // vTexCoord arrives in texels; the atlas is resized as glyphs are added, so
    // the divisor is read from the texture rather than baked into the vertex
    // data. textureSize reports level zero, which is the level the coordinates
    // were placed in.
    vec2 atlas = vec2(textureSize(uGlyphAtlas, 0).xy);
    float coverage =
        texture(uGlyphAtlas, vec3(vTexCoord / atlas, floor(vLayer + 0.5))).r;
    outColor =
        vec4(mix(selectedBackground(vBgColor), vFgColor, coverage), vOpacity);
    // Fixed point: the attachment holds unsigned integers. The scale must
    // match render::tagFractionScale.
    outTag = uvec4(vTag, uint(clamp(vQuadU, 0.0, 1.0) * 65535.0), 0u);
}
// vi: set sw=4 sts=4 ts=4 et:
