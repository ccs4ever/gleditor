// Beam vertex stage: a flat ribbon between two points in the world.
//
// The glyph pipeline draws quads that are axis-aligned by construction -- its
// corners are the centre plus or minus half the width and height -- which a
// line between two arbitrary points is not. This is the same instanced idea
// with the corners taken from the two ends instead:
//
//   vertex 0 -> from, left    vertex 1 -> from, right
//   vertex 2 -> to, left      vertex 3 -> to, right
//
// drawn as a triangle strip, one instance per beam, no per-vertex data.
//
// The ribbon turns about its own run to face the camera rather than lying in
// the plane of the pages. A ribbon in that plane has no width when the beam
// runs along Z, and is edge-on, and so invisible, whenever the camera looks
// along the plane; views put pages and beams at every depth
// (design/view-system-implementation-plan.md, F3).

GLEDITOR_IN(0) vec3 beamFrom; // one end, in world space
GLEDITOR_IN(1) float beamWidth;
GLEDITOR_IN(2) vec3 beamTo;     // the other
GLEDITOR_IN(3) uint beamColour; // packed RGBA8
GLEDITOR_IN(4) uint beamTag;    // which beam this is, for picking
// Where this segment falls along the whole route, 0 at the route's start and
// 1 at its end. A beam drawn on its own is (0, 1); a segment of a route that
// bends carries its own share, so the fade below runs once end to end instead
// of restarting at every joint.
GLEDITOR_IN(5) vec2 beamAlong;

GLEDITOR_OUT(0) vec4 vColour;
// Distance across the ribbon, -1 at one edge and 1 at the other, so the
// fragment stage can soften the edges rather than leaving them stepped: a
// beam is a thin thing seen at an angle, which is the worst case for
// aliasing.
GLEDITOR_OUT(1) float vAcross;
// How far along the beam, 0 at the near end and 1 at the far one. The far end
// is the one being pointed at, and a beam that fades along its length says
// which way it goes without an arrowhead.
GLEDITOR_OUT(2) float vAlong;
GLEDITOR_OUT_FLAT(3) uvec2 vTag;
GLEDITOR_OUT(4) float vOpacity;

// The direction the camera looks along through @p point, in world space: the
// line of points that land on the same pixel. Taken from the matrix alone, so
// the beam pipeline needs no uniform the others lack. A direction d keeps
// clip x/w and y/w fixed when it is orthogonal to w*rowX - x*rowW and to
// w*rowY - y*rowW. That holds for a perspective and an orthographic camera
// alike, and for Vulkan's rewrite of the matrix, which negates y (flipping
// only the sign here) and reshapes z (not read).
vec3 viewRayThrough(vec3 point) {
  vec4 clip = uMVP * vec4(point, 1.0);
  vec3 rowX = vec3(uMVP[0][0], uMVP[1][0], uMVP[2][0]);
  vec3 rowY = vec3(uMVP[0][1], uMVP[1][1], uMVP[2][1]);
  vec3 rowW = vec3(uMVP[0][3], uMVP[1][3], uMVP[2][3]);
  return cross(clip.w * rowX - clip.x * rowW, clip.w * rowY - clip.y * rowW);
}

vec4 unpackColour(uint bits) {
  return vec4(float((bits >> 24) & 255u), float((bits >> 16) & 255u),
              float((bits >> 8) & 255u), float(bits & 255u)) /
         255.0;
}

void main() {
  int corner   = GLEDITOR_VERTEX_INDEX;
  float along  = (0 != (corner & 2)) ? 1.0 : 0.0;
  float across = (0 != (corner & 1)) ? 1.0 : -1.0;

  vec3 run = beamTo - beamFrom;
  // A beam of no length has no direction to be perpendicular to; collapse it
  // outside clip space rather than dividing by zero.
  if (dot(run, run) <= 0.0) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    vColour     = vec4(0.0);
    vAcross     = 0.0;
    vAlong      = 0.0;
    vTag        = uvec2(0u);
    vOpacity    = 0.0;
    return;
  }

  // Perpendicular to the run and to the ray to the camera. Under perspective
  // every ray to the beam lies in the one plane through the run and the eye,
  // so the ray through the midpoint serves both ends and the ribbon stays
  // flat.
  vec3 along3 = normalize(run);
  vec3 sideways =
      cross(along3, normalize(viewRayThrough(mix(beamFrom, beamTo, 0.5))));
  // Seen end on, the beam is a point and every side is as good as another:
  // any perpendicular keeps it a dot of its own width rather than nothing.
  if (dot(sideways, sideways) < 1e-6) {
    vec3 other =
        abs(along3.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    sideways = cross(along3, other);
  }

  vec3 offset = normalize(sideways) * (beamWidth * 0.5) * across;
  vec3 point  = mix(beamFrom, beamTo, along) + offset;
  gl_Position = uMVP * vec4(point, 1.0);

  vColour  = unpackColour(beamColour);
  vAcross  = across;
  vAlong   = mix(beamAlong.x, beamAlong.y, along);
  vTag     = uvec2(uIdentity |
                       (uint(GLEDITOR_TAG_KIND_BEAM) << GLEDITOR_TAG_KIND_SHIFT),
                   beamTag);
  vOpacity = uOpacity;
}
