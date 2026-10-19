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
GLEDITOR_IN_FLAT(7) uint vSolid;
GLEDITOR_IN(8) float vQuadV;
GLEDITOR_IN_FLAT(9) uint vPaletteInk;

GLEDITOR_FRAG_OUT(0) vec4 outColor;
// identity word, cluster index, fractional position across the quad, unused.
GLEDITOR_FRAG_OUT(1) uvec4 outTag;

vec4 unpackColor(uint bits) {
  return vec4(float((bits >> 24) & 255u), float((bits >> 16) & 255u),
              float((bits >> 8) & 255u), float(bits & 255u)) /
         255.0;
}

// WCAG relative luminance uses linear sRGB, not the brightness of encoded RGB.
float relativeLuminance(vec3 colour) {
  vec3 linearColour = mix(pow((colour + 0.055) / 1.055, vec3(2.4)),
                          colour / 12.92, lessThanEqual(colour, vec3(0.04045)));
  return dot(linearColour, vec3(0.2126, 0.7152, 0.0722));
}

vec3 readableInk(vec3 ink, vec3 background) {
  const float minimumTextContrast = 4.5;
  float inkLuminance              = relativeLuminance(ink);
  float backgroundLuminance       = relativeLuminance(background);
  float contrast = (max(inkLuminance, backgroundLuminance) + 0.05) /
                   (min(inkLuminance, backgroundLuminance) + 0.05);
  if (contrast >= minimumTextContrast) return ink;
  float blackContrast = (backgroundLuminance + 0.05) / 0.05;
  float whiteContrast = 1.05 / (backgroundLuminance + 0.05);
  return blackContrast >= whiteContrast ? vec3(0.0) : vec3(1.0);
}

// Background for this fragment, which is the selection colour when the
// fragment falls inside a highlighted span.
//
// A span names clusters, because that is what a glyph instance carries, plus a
// fraction at each end for when the boundary falls inside one. vQuadU says
// where across the quad this fragment sits, so a span ending between the "f"
// and the "i" of an "fi" ligature highlights the "f" alone -- the two share a
// single quad and have no geometry to tell them apart.
//
// When multiple highlight ranges overlap across the same glyph or passage (for
// example, a transclusion span and a link annotation), vertical multi-banding
// partitions the quad into stacked horizontal stripes. This preserves the
// distinct color and identity of each overlapping layer.
#define GLEDITOR_MAX_OVERLAPPING_HIGHLIGHTS 4

vec3 selectedBackground(vec3 base) {
  uint matches[GLEDITOR_MAX_OVERLAPPING_HIGHLIGHTS];
  uvec2 owners[GLEDITOR_MAX_OVERLAPPING_HIGHLIGHTS];
  for (int i = 0; i < GLEDITOR_MAX_OVERLAPPING_HIGHLIGHTS; ++i)
    owners[i] = uvec2(0u);
  int matchCount  = 0;
  vec4 edge       = vec4(0.0);
  vec4 legacyEdge = vec4(0.0);
  float edgeInset = 0.0;
  vec2 pixelStep  = max(fwidth(vec2(vQuadU, vQuadV)), vec2(0.00001));

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
    float lo =
        (vTag.y == uRanges[i].firstCluster) ? uRanges[i].startFraction : 0.0;
    float hi =
        (vTag.y == uRanges[i].lastCluster) ? uRanges[i].endFraction : 1.0;
    if (vQuadU >= lo && (vQuadU < hi || (hi >= 1.0 && vQuadU <= 1.0))) {
      if (uRanges[i].emphasis != 0u) {
        // Only terminal clusters receive vertical edges; internal glyphs share
        // a continuous top/bottom accent without a box around every letter.
        float distancePx = min(vQuadV, 1.0 - vQuadV) / pixelStep.y;
        if (vTag.y == uRanges[i].firstCluster)
          distancePx = min(distancePx, (vQuadU - lo) / pixelStep.x);
        if (vTag.y == uRanges[i].lastCluster)
          distancePx = min(distancePx, (hi - vQuadU) / pixelStep.x);
        vec4 colour   = unpackColor(uRanges[i].colour);
        float widthPx = max(uRanges[i].edgeWidthPx, 0.5);
        if (uRanges[i].emphasis == 2u)
          widthPx = min(widthPx, 0.125 / pixelStep.y);
        if (uRanges[i].emphasis == 2u)
          edgeInset = max(edgeInset, widthPx * pixelStep.y);
        float strength =
            colour.a * (1.0 - smoothstep(0.0, widthPx, distancePx));
        if (uRanges[i].emphasis == 2u) {
          if (strength > edge.a) edge = vec4(colour.rgb, strength);
        } else if (strength > legacyEdge.a)
          legacyEdge = vec4(colour.rgb, strength);
      } else {
        bool duplicate = false;
        if (any(notEqual(uRanges[i].layer, uvec2(0u))))
          for (int j = 0; j < matchCount; j++)
            if (all(equal(owners[j], uRanges[i].layer))) duplicate = true;
        if (!duplicate && matchCount < GLEDITOR_MAX_OVERLAPPING_HIGHLIGHTS) {
          matches[matchCount] = uRanges[i].colour;
          owners[matchCount]  = uRanges[i].layer;
          matchCount++;
        }
      }
    }
  }

  // A paper-colour separation keeps the selected contour's hue independent
  // of every semantic band, including another link at an outer quad edge.
  // Both authored terminals still meet the complete outer contour.
  if (edge.a > 0.0) return mix(base, edge.rgb, edge.a);
  if (0 == matchCount) return mix(base, legacyEdge.rgb, legacyEdge.a);
  if (1 == matchCount)
    return mix(unpackColor(matches[0]).rgb, legacyEdge.rgb, legacyEdge.a);

  // Overlapping owners retain discrete horizontal bands, even for equal RGB.
  float clampedV =
      clamp((vQuadV - edgeInset) / (1.0 - 2.0 * edgeInset), 0.0, 0.9999);
  int band =
      clamp(int((1.0 - clampedV) * float(matchCount)), 0, matchCount - 1);
  return mix(unpackColor(matches[band]).rgb, legacyEdge.rgb, legacyEdge.a);
}

void main() {
  // A solid quad -- a page, a bar standing in for a line of text, a panel,
  // the caret -- has no glyph on it, so it is its own colour and the atlas is
  // never touched. Page backgrounds cover more fragments than all the text
  // they sit behind, and this is the fetch each of them no longer does.
  if (0u != vSolid) {
    outColor = vec4(vFgColor, vOpacity);
  } else {
    // vTexCoord arrives in texels; the atlas is resized as glyphs are
    // added, so the divisor is read from the texture rather than baked into
    // the vertex data. textureSize reports level zero, which is the level
    // the coordinates were placed in.
    vec2 atlas = vec2(textureSize(uGlyphAtlas, 0).xy);
    float coverage =
        texture(uGlyphAtlas, vec3(vTexCoord / atlas, floor(vLayer + 0.5))).r;
    vec3 background = selectedBackground(vBgColor);
    vec3 ink        = vFgColor;
    // Highlight hues stay semantic. Adapt monochrome text instead of changing
    // a relation's colour to accommodate a dark-paper ink palette.
    if (vPaletteInk != 0u && any(notEqual(background, vBgColor)))
      ink = readableInk(ink, background);
    outColor = vec4(mix(background, ink, coverage), vOpacity);
  }
  // Fixed point: the attachment holds unsigned integers. The scale must
  // match render::tagFractionScale.
  // Page backgrounds have no cluster. Reuse the two 16-bit picking fields as
  // normalized x/y coordinates so blank-page picks can resolve to the nearest
  // caret instead of unconditionally snapping to the page start.
  uint kind = vTag.x >> GLEDITOR_TAG_KIND_SHIFT;
  if (0u != vSolid && kind == uint(GLEDITOR_TAG_KIND_PAGE)) {
    outTag = uvec4(vTag.x, uint(clamp(vQuadU, 0.0, 1.0) * 65535.0),
                   uint(clamp(vQuadV, 0.0, 1.0) * 65535.0), 0u);
  } else {
    outTag = uvec4(vTag, uint(clamp(vQuadU, 0.0, 1.0) * 65535.0), 0u);
  }
}
