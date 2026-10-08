#include <algorithm>
#include <cmath>
#include <gleditor/nurbs.hpp>
#include <glm/geometric.hpp>
#include <stdexcept>

namespace gleditor {
NurbsPath::NurbsPath(std::span<const glm::vec3> points,
                     std::span<const float> weights,
                     std::span<const float> knots, unsigned degree)
    : points_(points), weights_(weights), knots_(knots), degree_(degree) {
  if (degree < 1 || degree > 3 || points.size() <= degree ||
      weights.size() != points.size() ||
      knots.size() != points.size() + degree + 1)
    throw std::invalid_argument("invalid NURBS dimensions");
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (!std::isfinite(weights[i]) || weights[i] <= 0)
      throw std::invalid_argument("invalid NURBS weight");
    for (unsigned axis = 0; axis < 3; ++axis)
      if (!std::isfinite(points[i][axis]) ||
          !std::isfinite(points[i][axis] * weights[i]))
        throw std::invalid_argument("non-finite NURBS control point");
  }
  for (std::size_t i = 0; i < knots.size(); ++i) {
    if (!std::isfinite(knots[i]) || (i && knots[i] < knots[i - 1]))
      throw std::invalid_argument("invalid NURBS knots");
    if (i <= degree && knots[i] != 0)
      throw std::invalid_argument("NURBS start must be clamped at zero");
    if (i >= points.size() && knots[i] != 1)
      throw std::invalid_argument("NURBS end must be clamped at one");
  }
  // Interior multiplicity above degree introduces a disconnected path.
  unsigned repeats = 0;
  for (std::size_t i = degree + 1; i < points.size(); ++i) {
    if (knots[i] <= 0 || knots[i] >= 1)
      throw std::invalid_argument("invalid NURBS interior knot");
    repeats = i > degree + 1 && knots[i] == knots[i - 1] ? repeats + 1 : 1;
    if (repeats > degree)
      throw std::invalid_argument("disconnected NURBS path");
  }
}

glm::vec4 NurbsPath::homogeneous(float t, bool derivative) const {
  if (!std::isfinite(t) || t < 0 || t > 1)
    throw std::invalid_argument("NURBS parameter outside zero to one");
  const auto knots = derivative ? knots_.subspan(1, knots_.size() - 2) : knots_;
  const unsigned degree = degree_ - (derivative ? 1 : 0);
  const auto count      = points_.size() - (derivative ? 1 : 0);
  const auto h          = [this](std::size_t i) {
    return glm::vec4(points_[i] * weights_[i], weights_[i]);
  };
  const auto pole = [&](std::size_t i) {
    if (!derivative) return h(i);
    const float interval = knots_[i + degree_ + 1] - knots_[i + 1];
    return interval > 0 ? (h(i + 1) - h(i)) * (degree_ / interval)
                        : glm::vec4(0);
  };
  const auto span = t == 1
                        ? count - 1
                        : static_cast<std::size_t>(
                              std::upper_bound(knots.begin(), knots.end(), t) -
                              knots.begin() - 1);
  std::array<glm::vec4, 4> work;
  for (unsigned j = 0; j <= degree; ++j) work[j] = pole(span - degree + j);
  for (unsigned r = 1; r <= degree; ++r)
    for (unsigned j = degree; j >= r; --j) {
      const auto i         = span - degree + j;
      const float interval = knots[i + degree - r + 1] - knots[i];
      const float alpha    = interval > 0 ? (t - knots[i]) / interval : 0;
      work[j]              = glm::mix(work[j - 1], work[j], alpha);
    }
  return work[degree];
}
glm::vec3 NurbsPath::position(float t) const {
  const auto h = homogeneous(t, false);
  return glm::vec3(h) / h.w;
}
glm::vec3 NurbsPath::tangent(float t) const {
  const auto h = homogeneous(t, false), d = homogeneous(t, true);
  return (glm::vec3(d) - glm::vec3(h) / h.w * d.w) / h.w;
}
float NurbsPath::controlLength() const {
  float length = 0;
  for (std::size_t i = 1; i < points_.size(); ++i)
    length += glm::distance(points_[i - 1], points_[i]);
  if (!std::isfinite(length))
    throw std::invalid_argument("NURBS length overflow");
  return length;
}
NurbsSamples sampleNurbs(const NurbsPath &path, unsigned segments) {
  segments = std::clamp(segments, 1U, NurbsSamples::maxSegments);
  NurbsSamples result;
  result.count = segments + 1;
  for (unsigned i = 0; i <= segments; ++i) {
    const float t  = static_cast<float>(i) / segments;
    auto &point    = result.points[i];
    point.position = path.position(t);
    point.tangent  = path.tangent(t);
    for (unsigned axis = 0; axis < 3; ++axis)
      if (!std::isfinite(point.position[axis]) ||
          !std::isfinite(point.tangent[axis]))
        throw std::invalid_argument("NURBS evaluation overflow");
    if (i)
      result.length +=
          glm::distance(result.points[i - 1].position, point.position);
    point.along = result.length;
  }
  if (!std::isfinite(result.length))
    throw std::invalid_argument("NURBS length overflow");
  if (result.length > 0)
    for (unsigned i = 0; i < result.count; ++i)
      result.points[i].along /= result.length;
  return result;
}
} // namespace gleditor
