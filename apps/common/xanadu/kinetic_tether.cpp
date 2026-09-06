/**
 * @file apps/common/xanadu/kinetic_tether.cpp
 * @brief Dynamic Hookean spring tether state machine and detachment physics
 * implementation.
 */
#include "kinetic_tether.hpp"

#include <cmath>
#include <utility>

#include <glm/geometric.hpp>

namespace xanadu {

void KineticTetherEngine::startDrag(TetherPayload payload, const float startX,
                                    const float startY) noexcept {
  payload_    = std::move(payload);
  currentPos_ = {startX, startY};
  targetPos_  = {startX, startY};
  velocity_   = {0.0F, 0.0F};
  state_      = TetherState::Dragging;
}

void KineticTetherEngine::updateDrag(const float currentX,
                                     const float currentY) noexcept {
  if (state_ == TetherState::Dragging) {
    targetPos_ = {currentX, currentY};
  }
}

float KineticTetherEngine::distance() const noexcept {
  return glm::length(targetPos_ - payload_.originScreenPos);
}

bool KineticTetherEngine::endDrag(const float endX, const float endY) noexcept {
  targetPos_       = {endX, endY};
  const float dist = distance();

  if (dist >= kMinDetachmentDistance) {
    state_ = TetherState::Spawning;
    if (voidSpawnHandler_) {
      voidSpawnHandler_(payload_, endX, endY);
    }
    state_ = TetherState::Idle;
    return true;
  }

  // Under threshold: trigger Hookean snap-back bounce
  state_     = TetherState::SnappingBack;
  targetPos_ = payload_.originScreenPos;
  return false;
}

void KineticTetherEngine::cancelDrag() noexcept {
  state_     = TetherState::SnappingBack;
  targetPos_ = payload_.originScreenPos;
}

void KineticTetherEngine::stepPhysics() noexcept {
  // Hookean spring: F = -k * x - c * v
  const glm::vec2 delta = targetPos_ - currentPos_;
  const glm::vec2 force = delta * kSpringK;
  velocity_             = (velocity_ + force) * kDampingC;
  currentPos_ += velocity_;

  if (state_ == TetherState::SnappingBack) {
    const float distToAnchor =
        glm::length(currentPos_ - payload_.originScreenPos);
    const float speed = glm::length(velocity_);
    if (distToAnchor < 2.0F && speed < 0.5F) {
      currentPos_ = payload_.originScreenPos;
      velocity_   = {0.0F, 0.0F};
      state_      = TetherState::Idle;
    }
  }
}

} // namespace xanadu
