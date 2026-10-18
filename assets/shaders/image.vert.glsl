// Image vertex stage: a colour quad sampling an explicit UV rect.
//
// The glyph pipeline derives how far a quad reaches into the atlas from the
// quad's own size in layout pixels -- true for a glyph, which is drawn at
// exactly its rasterised size, but not for an image, which is routinely
// scaled and cropped. This carries the UV rect explicitly instead, so the
// quad's size on the page and the region of the atlas it samples are
// independent.
//
//   vertex 0 -> bottom left    vertex 1 -> bottom right
//   vertex 2 -> top left       vertex 3 -> top right
//
// drawn as a triangle strip, one instance per image, no per-vertex data --
// the same convention glyph.vert.glsl and beam.vert.glsl use.

GLEDITOR_IN(0) vec2 imgPos;  // centre of the quad in model space
GLEDITOR_IN(1) vec2 imgSize; // width, height in layout pixels
GLEDITOR_IN(2) vec4 imgUv;   // u0, v0 (bottom left), u1, v1 (top right)
GLEDITOR_IN(3) uint imgLayer;
GLEDITOR_IN(4) uint imgTint; // packed RGBA8, multiplies the sampled colour
// Picking index, e.g. which cell or span this image stands for. The kind
// half of the tag is not carried per instance: every quad this pipeline
// draws is an image, so it is hardcoded below, the same way beam.vert.glsl
// hardcodes GLEDITOR_TAG_KIND_BEAM instead of reading a kind field.
GLEDITOR_IN(5) uint imgIndex;

GLEDITOR_OUT(0) vec4 vTint;
GLEDITOR_OUT(1) vec2 vTexCoord;
GLEDITOR_OUT(2) float vLayer;
GLEDITOR_OUT_FLAT(3) uvec2 vTag;
GLEDITOR_OUT(4) float vOpacity;

vec4 unpackColour(uint bits) {
  return vec4(float((bits >> 24) & 255u), float((bits >> 16) & 255u),
              float((bits >> 8) & 255u), float(bits & 255u)) /
         255.0;
}

void main() {
  // A degenerate quad rasterises nothing useful; collapse it outside clip
  // space so the fragment stage never runs for it, as glyph.vert.glsl does
  // for a zero-sized glyph box.
  if (0.0 == imgSize.x || 0.0 == imgSize.y) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    vTint       = vec4(0.0);
    vTexCoord   = vec2(0.0);
    vLayer      = 0.0;
    vTag        = uvec2(0u);
    vOpacity    = 0.0;
    return;
  }

  int corner  = GLEDITOR_VERTEX_INDEX;
  float xSign = (0 != (corner & 1)) ? 1.0 : -1.0;
  float ySign = (0 != (corner & 2)) ? 1.0 : -1.0;

  // The corner offset is a direction, not a point: its w is zero, so adding it
  // to the centre in model space and transforming once is the same as
  // transforming both and adding, for one matrix multiply instead of two --
  // the same trick glyph.vert.glsl uses.
  vec4 offset =
      vec4(xSign * imgSize.x * 0.5, ySign * imgSize.y * 0.5, 0.0, 0.0);
  gl_Position = uMVP * (vec4(imgPos, 0.0, 1.0) + offset);

  vTint = unpackColour(imgTint);
  // u0 is the region's left edge in both conventions, so corner&1 (right)
  // selects u1 exactly as glyph.vert.glsl's width offset does. v0/v1 are the
  // opposite: ImageCache uploads a decoded image's row 0 -- its *top* row,
  // the same convention every decoder here produces -- at the texel offset
  // v0 is computed from, so the quad's top (corner&2) must sample v0 and its
  // bottom v1, not the other way around. Getting this backwards renders
  // every image upside down without otherwise looking wrong -- there is
  // nothing about a flipped rectangle that fails to rasterise -- which is
  // exactly what shipped from Phase 1 until a byte-for-byte pixel check
  // (rather than eyeballing a screenshot) caught it in Phase 4.
  vTexCoord = vec2((0 != (corner & 1)) ? imgUv.z : imgUv.x,
                   (0 != (corner & 2)) ? imgUv.y : imgUv.w);
  vLayer    = float(imgLayer);
  vTag = uvec2(uIdentity |
                   (uint(GLEDITOR_TAG_KIND_IMAGE) << GLEDITOR_TAG_KIND_SHIFT),
               imgIndex);
  vOpacity = uOpacity;
}
