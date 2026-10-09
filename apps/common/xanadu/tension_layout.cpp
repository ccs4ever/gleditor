/**
 * @file tension_layout.cpp
 * @brief Real-time 3-Way Tension Layout Engine implementation.
 */
#include "common/xanadu/tension_layout.hpp"

#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>

#include <gleditor/ranges.hpp>

namespace xanadu {

TensionLayoutEngine::TensionLayoutEngine(TensionParams params)
    : params_(params) {}

void TensionLayoutEngine::setBody(TensionBody body) {
  for (auto &b : bodies_) {
    if (b.targetKind == body.targetKind && b.targetId == body.targetId) {
      b = body;
      return;
    }
  }
  bodies_.push_back(body);
}

namespace {
constexpr float sideSign(const AlignSide side) noexcept {
  return AlignSide::Left == side ? -1.0F : 1.0F;
}

auto isBody(const std::size_t targetId, const LinkTargetKind kind) {
  return [=](const TensionBody &b) {
    return b.targetKind == kind && b.targetId == targetId;
  };
}
} // namespace

gleditor::cpp26::optional<const TensionBody &>
TensionLayoutEngine::findBody(const std::size_t targetId,
                              const LinkTargetKind kind) const {
  return gleditor::findRef(bodies_, isBody(targetId, kind));
}

gleditor::cpp26::optional<TensionBody &>
TensionLayoutEngine::findBody(const std::size_t targetId,
                              const LinkTargetKind kind) {
  return gleditor::findRef(bodies_, isBody(targetId, kind));
}

void TensionLayoutEngine::clear() {
  bodies_.clear();
  constraints_.clear();
}

void TensionLayoutEngine::addConstraint(TensionConstraint constraint) {
  constraints_.push_back(constraint);
}

void TensionLayoutEngine::clearConstraints() { constraints_.clear(); }

void TensionLayoutEngine::computeForces(const std::vector<TensionBody> &state,
                                        std::vector<glm::vec3> &forces) const {
  const std::size_t n = state.size();
  forces.assign(n, glm::vec3(0.0F));

  // 1. Damping and depth plane / tether restoring forces (F_aest, F_read,
  // E_tether)
  for (std::size_t i = 0; i < n; ++i) {
    if (state[i].pinned) {
      continue;
    }

    // Velocity damping: F_damp = -c * v
    forces[i] -= params_.kDamping * state[i].velocity;

    if (state[i].isCell()) {
      bool hasActiveConstraint = false;
      for (const auto &c : constraints_) {
        if (c.active && ((c.fromTarget == state[i].targetId &&
                          c.fromKind == state[i].targetKind) ||
                         (c.toTarget == state[i].targetId &&
                          c.toKind == state[i].targetKind))) {
          hasActiveConstraint = true;
          break;
        }
      }

      if (!hasActiveConstraint && !state[i].isFlying) {
        // Tether restoring force pulling toward native coordinate in
        // background: F_tether = -k_tether * (P_c - P_native(c))
        const glm::vec3 tetherDelta =
            state[i].position - state[i].restingPosition;
        forces[i] -= params_.kTether * tetherDelta;

        const float deltaZ = state[i].position.z - params_.backgroundDepthZ;
        forces[i].z += -params_.kTier * deltaZ;
      }
    } else {
      // Depth tier spring for documents
      if (state[i].isForeground) {
        // Pull to Z = 0 reading plane
        forces[i].z += -params_.kPlane * state[i].position.z;
      } else {
        // Pull to backgroundDepthZ
        const float deltaZ = state[i].position.z - params_.backgroundDepthZ;
        forces[i].z += -params_.kTier * deltaZ;
      }
    }
  }

  // 2. Soft-body Coulomb repulsion between non-constrained bodies (F_read)
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = i + 1; j < n; ++j) {
      // Only repel if in similar depth tiers
      if (std::abs(state[i].position.z - state[j].position.z) > 15.0F) {
        continue;
      }

      // Do not repel if the pair is connected by an active constraint
      bool areConstrained = false;
      for (const auto &c : constraints_) {
        if (!c.active) {
          continue;
        }
        if ((c.fromTarget == state[i].targetId &&
             c.fromKind == state[i].targetKind &&
             c.toTarget == state[j].targetId &&
             c.toKind == state[j].targetKind) ||
            (c.fromTarget == state[j].targetId &&
             c.fromKind == state[j].targetKind &&
             c.toTarget == state[i].targetId &&
             c.toKind == state[i].targetKind)) {
          areConstrained = true;
          break;
        }
      }
      if (areConstrained) {
        continue;
      }

      const float dx = state[i].position.x - state[j].position.x;
      const float minXDist =
          0.5F * (state[i].width + state[j].width) + params_.defaultGap;

      if (std::abs(dx) < minXDist) {
        const float penetration = minXDist - std::abs(dx);
        const float sign        = (dx >= 0.0F) ? 1.0F : -1.0F;
        const float forceX      = params_.kRepel * (penetration / minXDist);

        if (!state[i].pinned) {
          forces[i].x += forceX * sign;
        }
        if (!state[j].pinned) {
          forces[j].x -= forceX * sign;
        }
      }
    }
  }

  // 3. Collinear link alignment attraction springs (F_align & E_satelloid)
  for (const auto &c : constraints_) {
    if (!c.active) {
      continue;
    }
    std::size_t fromIdx = n;
    std::size_t toIdx   = n;

    for (std::size_t i = 0; i < n; ++i) {
      if (state[i].targetId == c.fromTarget &&
          state[i].targetKind == c.fromKind) {
        fromIdx = i;
      }
      if (state[i].targetId == c.toTarget && state[i].targetKind == c.toKind) {
        toIdx = i;
      }
    }

    if (fromIdx >= n || toIdx >= n || fromIdx == toIdx) {
      continue;
    }

    const auto &near = state[fromIdx];
    const auto &far  = state[toIdx];

    const float kSpring =
        far.isCell() ? params_.kSatelloidAlign : params_.kAlign;
    float gap = c.targetGap;
    if (gap <= 0.0F) {
      gap = far.isCell() ? params_.satelloidGap : params_.defaultGap;
    }

    // Collinear target position for far body
    const float targetX =
        near.position.x +
        sideSign(c.side) * (0.5F * (near.width + far.width) + gap);
    const float deltaAnchorY = c.nearAnchorY - c.farAnchorY;
    const float targetY      = near.position.y + deltaAnchorY;
    const float targetZ      = near.position.z;

    const glm::vec3 targetPos(targetX, targetY, targetZ);
    const glm::vec3 error = targetPos - far.position;

    const glm::vec3 alignForce = kSpring * c.prominence * error;

    if (!far.pinned) {
      forces[toIdx] += alignForce;
    }
    if (!near.pinned) {
      const float reactionRatio = far.isCell() ? 0.02F : 0.15F;
      forces[fromIdx] -= reactionRatio * alignForce; // Reaction force
    }
  }

  // Clamp force magnitude if maxForce is configured
  if (params_.maxForce > 0.0F) {
    for (auto &f : forces) {
      const float mag = glm::length(f);
      if (mag > params_.maxForce) {
        f = (f / mag) * params_.maxForce;
      }
    }
  }
}

void TensionLayoutEngine::step(const float dt) {
  if (bodies_.empty() || dt <= 0.0F) {
    return;
  }

  const std::size_t n           = bodies_.size();
  auto &[forces, dv, dx, state] = scratch_;

  // Stage k evaluates the forces on @p at, and, when @p next is given, steps
  // a copy of the bodies by @p h along its slopes for the stage after.
  const auto stage = [&](const std::size_t k,
                         const std::vector<TensionBody> &at,
                         std::vector<TensionBody> *next, const float h) {
    computeForces(at, forces[k]);
    dv[k].resize(n);
    dx[k].resize(n);
    if (next != nullptr) {
      next->assign(bodies_.begin(), bodies_.end());
    }
    for (std::size_t i = 0; i < n; ++i) {
      if (bodies_[i].pinned) {
        dv[k][i] = glm::vec3(0.0F);
        dx[k][i] = glm::vec3(0.0F);
        continue;
      }
      const float invM =
          (bodies_[i].mass > 0.0F) ? (1.0F / bodies_[i].mass) : 1.0F;
      dv[k][i] = forces[k][i] * invM;
      dx[k][i] = at[i].velocity;
      if (next != nullptr) {
        (*next)[i].position += h * dx[k][i];
        (*next)[i].velocity += h * dv[k][i];
      }
    }
  };

  const float half = 0.5F * dt;
  stage(0, bodies_, &state[0], half);
  stage(1, state[0], &state[1], half);
  stage(2, state[1], &state[2], dt);
  stage(3, state[2], nullptr, dt);

  for (std::size_t i = 0; i < n; ++i) {
    if (bodies_[i].pinned) {
      continue;
    }
    // RK4 final weighted blend
    bodies_[i].position +=
        (dt / 6.0F) * (dx[0][i] + 2.0F * dx[1][i] + 2.0F * dx[2][i] + dx[3][i]);
    bodies_[i].velocity +=
        (dt / 6.0F) * (dv[0][i] + 2.0F * dv[1][i] + 2.0F * dv[2][i] + dv[3][i]);

    if (params_.maxVelocity > 0.0F) {
      const float speed = glm::length(bodies_[i].velocity);
      if (speed > params_.maxVelocity) {
        bodies_[i].velocity =
            (bodies_[i].velocity / speed) * params_.maxVelocity;
      }
    }

    bodies_[i].force = forces[3][i];
  }
}

bool TensionLayoutEngine::isSettled() const {
  return std::ranges::all_of(bodies_, [this](const auto &b) {
    return b.pinned ||
           glm::length(b.velocity) <= params_.settleVelocityThreshold;
  });
}

void TensionLayoutEngine::solveEquilibrium() {
  if (bodies_.empty()) {
    return;
  }

  // 1. Separate foreground and background documents
  float currX  = 0.0F;
  bool firstFg = true;

  for (auto &b : bodies_) {
    if (b.isCell()) {
      continue; // Handled via constraints below
    }
    if (!b.isForeground) {
      b.position.z = params_.backgroundDepthZ;
      b.velocity   = glm::vec3(0.0F);
      continue;
    }

    b.position.z = 0.0F;
    if (firstFg) {
      b.position.x = 0.0F;
      currX        = 0.5F * b.width;
      firstFg      = false;
    } else {
      currX += 0.5F * b.width + params_.defaultGap;
      b.position.x = currX;
      currX += 0.5F * b.width;
    }
    b.velocity = glm::vec3(0.0F);
  }

  // 2. Position satelloid cells and align constrained pairs
  for (const auto &c : constraints_) {
    auto near = findBody(c.fromTarget, c.fromKind);
    auto far  = findBody(c.toTarget, c.toKind);
    if (!near || !far) {
      continue;
    }
    const float deltaY = c.nearAnchorY - c.farAnchorY;
    far->position.y    = near->position.y + deltaY;

    if (far->isCell()) {
      const float gap = c.targetGap > 0.0F ? c.targetGap : params_.satelloidGap;
      far->position.x =
          near->position.x +
          sideSign(c.side) * (0.5F * (near->width + far->width) + gap);
      far->position.z = near->position.z;
      far->velocity   = glm::vec3(0.0F);
    }
  }

  // Inactive cells return to resting position
  for (auto &b : bodies_) {
    if (b.isCell() && !b.isFlying) {
      b.position = b.restingPosition;
      b.velocity = glm::vec3(0.0F);
    }
  }
}

} // namespace xanadu
