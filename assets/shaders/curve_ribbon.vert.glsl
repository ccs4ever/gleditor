GLEDITOR_IN(0) vec2 curveFrom;
GLEDITOR_IN(1) vec2 curveControl;
GLEDITOR_IN(2) vec2 curveTo;
GLEDITOR_IN(3) vec2 curveWidths;
GLEDITOR_IN(4) vec2 curveInterval;
GLEDITOR_IN(5) uint curveColour;
GLEDITOR_IN(6) uint curveTag;
GLEDITOR_IN(7) vec4 curveSurface;
GLEDITOR_IN(8) vec3 curveHole;
GLEDITOR_OUT(0) vec4 vColour;
GLEDITOR_OUT(1) float vAcross;
GLEDITOR_OUT(2) float vAlong;
GLEDITOR_OUT(3) float vHalfWidth;
GLEDITOR_OUT(4) vec2 vPosition;
GLEDITOR_OUT_FLAT(5) vec4 vSurface;
GLEDITOR_OUT_FLAT(6) vec3 vHole;
GLEDITOR_OUT_FLAT(7) uvec2 vTag;
void main() {
  int corner   = GLEDITOR_VERTEX_INDEX;
  float t      = (corner & 2) != 0 ? curveInterval.y : curveInterval.x;
  float side   = (corner & 1) != 0 ? 1.0 : -1.0;
  vec2 tangent = mix(curveControl - curveFrom, curveTo - curveControl, t);
  if (dot(tangent, tangent) < 0.000001) tangent = curveTo - curveFrom;
  if (dot(tangent, tangent) < 0.000001) tangent = vec2(1.0, 0.0);
  vec2 normal = normalize(vec2(-tangent.y, tangent.x));
  vec2 centre =
      mix(mix(curveFrom, curveControl, t), mix(curveControl, curveTo, t), t);
  float halfWidth =
      mix(curveWidths.x, curveWidths.y, smoothstep(0.0, 1.0, t)) * 0.5;
  float across = side * (halfWidth + curveSurface.x);
  vPosition    = centre + normal * across;
  gl_Position  = uMVP * vec4(vPosition, 0.0, 1.0);
  vAcross      = across;
  vHalfWidth   = halfWidth;
  vAlong       = t;
  vSurface     = curveSurface;
  vHole        = curveHole;
  vColour =
      vec4(float((curveColour >> 24) & 255u), float((curveColour >> 16) & 255u),
           float((curveColour >> 8) & 255u), float(curveColour & 255u)) /
      255.0;
  vColour.a *= uOpacity;
  vTag = uvec2(uIdentity |
                   (uint(GLEDITOR_TAG_KIND_OVERLAY) << GLEDITOR_TAG_KIND_SHIFT),
               curveTag);
}
