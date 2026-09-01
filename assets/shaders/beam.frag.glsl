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
  // Soften the outer long edges over roughly one pixel with fwidth
  float pixelAcross = min(fwidth(vAcross), 0.9);
  float edge        = 1.0 - smoothstep(1.0 - pixelAcross, 1.0, abs(vAcross));

  // 1. Polished Glass Optical Core:
  // Concentrated bright transmission channel down the centerline
  float core = exp(-pow(abs(vAcross) * 3.2, 2.0));

  // 2. Beveled Glass Fresnel Rim:
  // Specular sheen catching the refractive edge of the glass ribbon
  float rim = smoothstep(0.65, 0.95, abs(vAcross)) *
              (1.0 - smoothstep(0.95, 1.0, abs(vAcross)));

  // 3. Active Selection Pulse Wave:
  // Traveling photonic pulse packets running along the transclusion conduit
  float wave   = 0.5 + 0.5 * sin(vAlong * 32.0);
  float packet = exp(-pow(fract(vAlong * 3.0) - 0.5, 2.0) * 36.0);
  float pulse  = mix(0.85, 1.45, wave * 0.35 + packet * 0.65);

  // Directional transmission along the path
  float along = mix(1.0, 0.60, vAlong);

  // Synthesize refractive glass colors
  vec3 glassBase = vColour.rgb * (0.6 + 0.4 * core);
  vec3 glassGlow =
      mix(glassBase, vec3(1.0, 1.0, 1.0), core * 0.55 + rim * 0.45);
  vec3 finalRgb = glassGlow * pulse;

  float alpha =
      vColour.a * edge * (0.45 + 0.55 * core + 0.35 * rim) * along * vOpacity;

  outColour = vec4(finalRgb, clamp(alpha, 0.0, 1.0));
  outTag    = uvec4(vTag, 0u, 0u);
}
