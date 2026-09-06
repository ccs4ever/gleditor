/**
 * @file apps/xudu/core/kinetic_tether.hpp
 * @brief Dynamic Hookean spring tether state machine and detachment physics.
 */
#ifndef XUDU_CORE_KINETIC_TETHER_HPP
#define XUDU_CORE_KINETIC_TETHER_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include <glm/vec2.hpp>

#include "microversion.hpp"
#include "ops.hpp"

namespace xudu {

/**
 * @enum class TetherState
 * @brief State machine governing kinetic detachment and spring snap-back.
 */
enum class TetherState : std::uint8_t {
  Idle,
  Dragging,
  SnappingBack,
  Spawning,
};

/**
 * @struct TetherPayload
 * @brief Spatio-temporal descriptor of the span being detached into the void.
 */
struct TetherPayload {
  PrimediaSpan span{};
  std::string previewText;
  MicroversionId originVersion{};
  std::uint32_t originDocIndex{0};
  std::uint32_t originCharStart{0};
  std::uint32_t originCharEnd{0};
  glm::vec2 originScreenPos{0.0F, 0.0F};
};

/**
 * @class KineticTetherEngine
 * @brief Governs Hookean spring physics, detachment radius threshold,
 *        and snap-back vs void materialization.
 */
class KineticTetherEngine {
public:
  static constexpr float kMinDetachmentDistance =
      120.0F;                               // Fitts detachment threshold
  static constexpr float kSpringK  = 0.22F; // Hooke's spring constant
  static constexpr float kDampingC = 0.75F; // Velocity damping coefficient

  using VoidSpawnHandler = std::function<void(const TetherPayload &payload,
                                              float screenX, float screenY)>;

  KineticTetherEngine() = default;

  void startDrag(TetherPayload payload, float startX, float startY) noexcept;
  void updateDrag(float currentX, float currentY) noexcept;
  bool endDrag(float endX,
               float endY) noexcept; // returns true if void spawn triggered
  void cancelDrag() noexcept;
  void stepPhysics() noexcept;

  void setVoidSpawnHandler(VoidSpawnHandler handler) {
    voidSpawnHandler_ = std::move(handler);
  }

  [[nodiscard]] TetherState state() const noexcept { return state_; }
  [[nodiscard]] bool isDragging() const noexcept {
    return state_ == TetherState::Dragging;
  }
  [[nodiscard]] bool busy() const noexcept {
    return state_ != TetherState::Idle;
  }
  [[nodiscard]] bool isDetached() const noexcept {
    return distance() >= kMinDetachmentDistance;
  }
  [[nodiscard]] float distance() const noexcept;
  [[nodiscard]] const TetherPayload &payload() const noexcept {
    return payload_;
  }
  [[nodiscard]] glm::vec2 currentPos() const noexcept { return currentPos_; }
  [[nodiscard]] glm::vec2 targetPos() const noexcept { return targetPos_; }
  [[nodiscard]] glm::vec2 originPos() const noexcept {
    return payload_.originScreenPos;
  }

private:
  TetherState state_{TetherState::Idle};
  TetherPayload payload_{};

  glm::vec2 currentPos_{0.0F, 0.0F};
  glm::vec2 targetPos_{0.0F, 0.0F};
  glm::vec2 velocity_{0.0F, 0.0F};

  VoidSpawnHandler voidSpawnHandler_{nullptr};
};

} // namespace xudu

#endif // XUDU_CORE_KINETIC_TETHER_HPP
