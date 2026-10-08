GLEDITOR_IN(0) vec4 vColour;
GLEDITOR_IN(1) float vAcross;
GLEDITOR_IN(2) float vAlong;
GLEDITOR_IN(3) float vHalfWidth;
GLEDITOR_IN(4) vec2 vPosition;
GLEDITOR_IN_FLAT(5) vec4 vSurface;
GLEDITOR_IN_FLAT(6) vec3 vHole;
GLEDITOR_IN_FLAT(7) uvec2 vTag;
GLEDITOR_FRAG_OUT(0) vec4 outColour;
GLEDITOR_FRAG_OUT(1) uvec4 outTag;
void main() {
  if (vHole.z > 0.0 && distance(vPosition, vHole.xy) <= vHole.z) discard;
  float soft = max(fwidth(vAcross), vSurface.x);
  float edge = 1.0 - smoothstep(vHalfWidth - soft * 0.5,
                                vHalfWidth + soft * 0.5, abs(vAcross));
  if (edge <= 0.0) discard;
  float across = vAcross / max(vHalfWidth, 0.001);
  // Two quiet fibres share the optical core vocabulary of a ribbon without
  // its broad refractive rim. Texture fades before the hairline tip.
  float fibres  = exp(-pow((abs(across) - 0.36) * 8.0, 2.0));
  float band    = 0.5 + 0.5 * cos(vAlong * vSurface.w / vSurface.y * 6.2831853);
  float texture = vSurface.z * (1.0 - smoothstep(0.55, 0.85, vAlong));
  vec3 colour =
      vColour.rgb * (1.0 - texture * (1.0 - fibres) * (0.5 + 0.5 * band));
  outColour = vec4(colour, vColour.a * edge);
  outTag    = uvec4(vTag, 0u, 0u);
}
