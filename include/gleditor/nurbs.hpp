#ifndef GLEDITOR_NURBS_HPP
#define GLEDITOR_NURBS_HPP

#include <array>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <span>

namespace gleditor {
/// A borrowed, clamped NURBS path. Degree 1..3, positive weights; no heap
/// storage or application policy. Control data must outlive the path.
class NurbsPath {
public:
  NurbsPath(std::span<const glm::vec3> points, std::span<const float> weights,
            std::span<const float> knots, unsigned degree);
  [[nodiscard]] glm::vec3 position(float parameter) const;
  [[nodiscard]] glm::vec3 tangent(float parameter) const;
  [[nodiscard]] float controlLength() const;

private:
  [[nodiscard]] glm::vec4 homogeneous(float parameter, bool derivative) const;
  std::span<const glm::vec3> points_;
  std::span<const float> weights_, knots_;
  unsigned degree_;
};

struct NurbsSamples {
  static constexpr unsigned maxSegments = 256;
  struct Point {
    glm::vec3 position{}, tangent{};
    float along{};
  };
  std::array<Point, maxSegments + 1> points;
  unsigned count{};
  float length{};
};
/// Bounded sampling with analytic tangents and cumulative chord distance.
[[nodiscard]] NurbsSamples sampleNurbs(const NurbsPath &, unsigned segments);
} // namespace gleditor
#endif
