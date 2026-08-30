// Beam fragment stage. See beam.vert.glsl for the geometry.
//
// Both attachments are written, as the glyph stage does: the second holds the
// picking identity, and a stage that wrote only the first would leave whatever
// was there before under every beam -- so clicking a beam would report the
// page behind it, or nothing.

GLEDITOR_IN(0) vec4 vColour;
GLEDITOR_IN(1) float vAcross;
GLEDITOR_IN(2) float vAlong;
GLEDITOR_IN_FLAT(3) uvec2 vTag;
GLEDITOR_IN(4) float vOpacity;

GLEDITOR_FRAG_OUT(0) vec4 outColour;
GLEDITOR_FRAG_OUT(1) uvec4 outTag;

void main() {
  // Soften the long edges over roughly one pixel rather than over a fixed
  // fraction of the width. A beam is thin and usually seen at an angle, which
  // is where a hard edge crawls -- and a fixed fraction gets that wrong from
  // both directions: it blurs a beam drawn close and near head-on into a
  // gradient with no edge at all, and leaves one drawn small or steeply
  // foreshortened as stepped as it ever was. fwidth is how wide a pixel is in
  // this ribbon's own across-coordinate, which is the width the fade actually
  // wants. Capped short of the full half-width so a beam thinner than a pixel
  // keeps a core rather than fading away entirely.
  float pixelAcross = min(fwidth(vAcross), 0.9);
  float edge        = 1.0 - smoothstep(1.0 - pixelAcross, 1.0, abs(vAcross));

  // And fade along its length, brightest at the end it starts from. Which
  // way a link points is a fact about the link, and this says it without an
  // arrowhead to draw or a direction to explain. vAlong is the fraction of
  // the whole route rather than of this segment (see beam.vert.glsl), so a
  // route that bends fades once from end to end instead of once per segment.
  float along = mix(1.0, 0.55, vAlong);

  outColour = vec4(vColour.rgb, vColour.a * edge * along * vOpacity);
  outTag    = uvec4(vTag, 0u, 0u);
}
