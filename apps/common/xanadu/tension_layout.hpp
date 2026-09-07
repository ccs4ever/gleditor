/**
 * @file tension_layout.hpp
 * @brief Real-time 3-Way Tension Layout Engine for Xudu documents and sources.
 *
 * Implements Ted Nelson's continuous physical optimization model balancing:
 * 1. Priority 1 (F_read): Text readability, comfortable viewing angle,
 *    and soft-body Coulomb repulsion preventing document overlapping.
 * 2. Priority 2 (F_align): Link collinear alignment pulling destination
 *    documents and transcluded passages into side-by-side reading alignment.
 * 3. Priority 3 (F_aest): Harmonic depth tier layering (foreground Z = 0 vs
 *    background Z = -40) and global link aesthetic spacing.
 */
#ifndef XUDU_TENSION_LAYOUT_HPP
#define XUDU_TENSION_LAYOUT_HPP

#include <cstddef>
#include <vector>

#include <glm/ext/vector_float3.hpp>

namespace xanadu {

/**
 * @struct TensionBody
 * @brief Dynamic physical state of a document in 3D coordinate space.
 */
struct TensionBody {
  std::size_t docIndex{0};

  glm::vec3 position{0.0F};
  glm::vec3 velocity{0.0F};
  glm::vec3 force{0.0F};

  /// Preferred or origin resting coordinate (e.g. background plane Z = -40).
  glm::vec3 restingPosition{0.0F};

  /// World-space dimensions of the document's page stack.
  float width{50.0F};
  float height{70.0F};

  /// True if currently participating in primary reading (Z ~ 0).
  bool isForeground{true};

  /// True if brought forward from a background plane into active collinear
  /// focus.
  bool isFlying{false};

  /// Physical mass for inertial momentum.
  float mass{1.0F};

  /// Fixed or pinned in space (immune to simulation forces).
  bool pinned{false};
};

/**
 * @struct TensionConstraint
 * @brief Relational spring constraint connecting two documents (link or
 * transclusion).
 */
struct TensionConstraint {
  std::size_t fromDoc{0};
  std::size_t toDoc{0};

  /// World Y coordinate offsets of the connection anchors.
  float nearAnchorY{0.0F};
  float farAnchorY{0.0F};

  /// Desired horizontal gap between documents.
  float targetGap{8.0F};

  /// Strength multiplier based on link tier / prominence.
  float prominence{1.0F};

  /// True if the link is active, hovered, or selected.
  bool active{false};
};

/**
 * @struct TensionParams
 * @brief Physical coefficients tuning the 3-way balance.
 */
struct TensionParams {
  /// Coulomb repulsion constant preventing box collisions (F_read).
  float kRepel{4500.0F};

  /// Spring constant pulling active documents to reading plane Z = 0 (F_read).
  float kPlane{14.0F};

  /// Collinear alignment spring constant pulling linked pages side-by-side
  /// (F_align).
  float kAlign{28.0F};

  /// Tier depth holding spring constant for background corpora (F_aest).
  float kTier{12.0F};

  /// Linear velocity damping coefficient ensuring asymptotic settling.
  float kDamping{7.5F};

  /// Resting background depth along Z axis.
  float backgroundDepthZ{-40.0F};

  /// Minimum comfortable horizontal reading gap.
  float defaultGap{8.0F};

  /// Velocity threshold below which simulation is considered settled.
  float settleVelocityThreshold{0.02F};

  /// Maximum instantaneous force magnitude to prevent numerical explosion.
  float maxForce{10000.0F};

  /// Maximum linear velocity magnitude for physical stability.
  float maxVelocity{1000.0F};

  /// Default simulation time step in seconds.
  float timeStep{0.016F};
};

/**
 * @class TensionLayoutEngine
 * @brief Real-time continuous 3-way tension solver using 4th-order Runge-Kutta
 * (RK4).
 */
class TensionLayoutEngine {
public:
  explicit TensionLayoutEngine(TensionParams params = TensionParams{});
  ~TensionLayoutEngine() = default;

  /// Update parameters.
  void setParams(const TensionParams &params) { params_ = params; }
  [[nodiscard]] const TensionParams &params() const noexcept { return params_; }

  /// Add or update a physical document body.
  void setBody(TensionBody body);

  /// Retrieve all current bodies.
  [[nodiscard]] const std::vector<TensionBody> &bodies() const noexcept {
    return bodies_;
  }
  [[nodiscard]] std::vector<TensionBody> &bodies() noexcept { return bodies_; }

  /// Retrieve specific body by doc index.
  [[nodiscard]] const TensionBody *findBody(std::size_t docIndex) const;
  [[nodiscard]] TensionBody *findBody(std::size_t docIndex);

  /// Clear all bodies and constraints.
  void clear();

  /// Add a relational link constraint.
  void addConstraint(TensionConstraint constraint);

  /// Clear all link constraints.
  void clearConstraints();

  /// Retrieve constraints.
  [[nodiscard]] const std::vector<TensionConstraint> &
  constraints() const noexcept {
    return constraints_;
  }

  /// Advance simulation by dt seconds using 4th-order Runge-Kutta (RK4)
  /// numerical integration.
  void step(float dt);

  /// Compute instantaneous forces acting on all bodies.
  void computeForces(const std::vector<TensionBody> &state,
                     std::vector<glm::vec3> &forces) const;

  /// Check if the physical simulation has settled below the velocity threshold.
  [[nodiscard]] bool isSettled() const;

  /// Analytical equilibrium solver: computes non-overlapping layout positions
  /// directly.
  void solveEquilibrium();

  /// Enable or disable continuous physics stepping.
  void setRunning(bool running) noexcept { running_ = running; }
  [[nodiscard]] bool isRunning() const noexcept { return running_; }
  void toggleRunning() noexcept { running_ = !running_; }

private:
  TensionParams params_;
  std::vector<TensionBody> bodies_;
  std::vector<TensionConstraint> constraints_;
  bool running_{false};
};

} // namespace xanadu

#endif // XUDU_TENSION_LAYOUT_HPP
