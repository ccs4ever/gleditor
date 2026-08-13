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
    // Soften the long edges. A beam is thin and usually seen at an angle,
    // which is where a hard edge crawls; fading the outer fifth of the ribbon
    // costs nothing and settles it.
    float edge = 1.0 - smoothstep(0.8, 1.0, abs(vAcross));

    // And fade along its length, brightest at the end it starts from. Which
    // way a link points is a fact about the link, and this says it without an
    // arrowhead to draw or a direction to explain.
    float along = mix(1.0, 0.35, vAlong);

    outColour = vec4(vColour.rgb, vColour.a * edge * along * vOpacity);
    outTag = uvec4(vTag, 0u, 0u);
}
// vi: set sw=4 sts=4 ts=4 et:
