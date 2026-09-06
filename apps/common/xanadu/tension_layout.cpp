/**
 * @file tension_layout.cpp
 * @brief Real-time 3-Way Tension Layout Engine implementation.
 */
#include "common/xanadu/tension_layout.hpp"

#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>

namespace xanadu {

TensionLayoutEngine::TensionLayoutEngine(TensionParams params)
    : params_(params) {}

void TensionLayoutEngine::setBody(TensionBody body) {
  for (auto &b : bodies_) {
    if (b.docIndex == body.docIndex) {
      b = body;
      return;
    }
  }
  bodies_.push_back(body);
}

const TensionBody *
TensionLayoutEngine::findBody(const std::size_t docIndex) const {
  for (const auto &b : bodies_) {
    if (b.docIndex == docIndex) {
      return &b;
    }
  }
  return nullptr;
}

TensionBody *TensionLayoutEngine::findBody(const std::size_t docIndex) {
  for (auto &b : bodies_) {
    if (b.docIndex == docIndex) {
      return &b;
    }
  }
  return nullptr;
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

  // 1. Damping and depth plane restoring forces (F_aest & F_read)
  for (std::size_t i = 0; i < n; ++i) {
    if (state[i].pinned) {
      continue;
    }

    // Velocity damping: F_damp = -c * v
    forces[i] -= params_.kDamping * state[i].velocity;

    // Depth tier spring
    if (state[i].isForeground) {
      // Pull to Z = 0 reading plane
      forces[i].z += -params_.kPlane * state[i].position.z;
    } else {
      // Pull to backgroundDepthZ
      const float deltaZ = state[i].position.z - params_.backgroundDepthZ;
      forces[i].z += -params_.kTier * deltaZ;
    }
  }

  // 2. Soft-body Coulomb repulsion between overlapping documents (F_read)
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = i + 1; j < n; ++j) {
      // Only repel if in similar depth tiers
      if (std::abs(state[i].position.z - state[j].position.z) > 15.0F) {
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

  // 3. Collinear link alignment attraction springs (F_align)
  for (const auto &c : constraints_) {
    std::size_t fromIdx = n;
    std::size_t toIdx   = n;

    for (std::size_t i = 0; i < n; ++i) {
      if (state[i].docIndex == c.fromDoc) {
        fromIdx = i;
      }
      if (state[i].docIndex == c.toDoc) {
        toIdx = i;
      }
    }

    if (fromIdx >= n || toIdx >= n || fromIdx == toIdx) {
      continue;
    }

    const auto &near = state[fromIdx];
    const auto &far  = state[toIdx];

    // Collinear target position for far document
    const float targetX =
        near.position.x + 0.5F * (near.width + far.width) + c.targetGap;
    const float deltaAnchorY = c.nearAnchorY - c.farAnchorY;
    const float targetY      = near.position.y + deltaAnchorY;
    const float targetZ      = near.position.z;

    const glm::vec3 targetPos(targetX, targetY, targetZ);
    const glm::vec3 error = targetPos - far.position;

    const glm::vec3 alignForce = params_.kAlign * c.prominence * error;

    if (!far.pinned) {
      forces[toIdx] += alignForce;
    }
    if (!near.pinned) {
      forces[fromIdx] -= 0.15F * alignForce; // Reaction force
    }
  }
}

void TensionLayoutEngine::step(const float dt) {
  if (bodies_.empty() || dt <= 0.0F) {
    return;
  }

  const std::size_t n = bodies_.size();

  // RK4 Stage 1
  std::vector<glm::vec3> f1;
  computeForces(bodies_, f1);

  std::vector<glm::vec3> k1_v(n);
  std::vector<glm::vec3> k1_x(n);
  std::vector<TensionBody> s1 = bodies_;

  for (std::size_t i = 0; i < n; ++i) {
    if (bodies_[i].pinned) {
      k1_v[i] = glm::vec3(0.0F);
      k1_x[i] = glm::vec3(0.0F);
      continue;
    }
    const float invM =
        (bodies_[i].mass > 0.0F) ? (1.0F / bodies_[i].mass) : 1.0F;
    k1_v[i] = f1[i] * invM;
    k1_x[i] = bodies_[i].velocity;

    s1[i].position += 0.5F * dt * k1_x[i];
    s1[i].velocity += 0.5F * dt * k1_v[i];
  }

  // RK4 Stage 2
  std::vector<glm::vec3> f2;
  computeForces(s1, f2);

  std::vector<glm::vec3> k2_v(n);
  std::vector<glm::vec3> k2_x(n);
  std::vector<TensionBody> s2 = bodies_;

  for (std::size_t i = 0; i < n; ++i) {
    if (bodies_[i].pinned) {
      k2_v[i] = glm::vec3(0.0F);
      k2_x[i] = glm::vec3(0.0F);
      continue;
    }
    const float invM =
        (bodies_[i].mass > 0.0F) ? (1.0F / bodies_[i].mass) : 1.0F;
    k2_v[i] = f2[i] * invM;
    k2_x[i] = s1[i].velocity;

    s2[i].position += 0.5F * dt * k2_x[i];
    s2[i].velocity += 0.5F * dt * k2_v[i];
  }

  // RK4 Stage 3
  std::vector<glm::vec3> f3;
  computeForces(s2, f3);

  std::vector<glm::vec3> k3_v(n);
  std::vector<glm::vec3> k3_x(n);
  std::vector<TensionBody> s3 = bodies_;

  for (std::size_t i = 0; i < n; ++i) {
    if (bodies_[i].pinned) {
      k3_v[i] = glm::vec3(0.0F);
      k3_x[i] = glm::vec3(0.0F);
      continue;
    }
    const float invM =
        (bodies_[i].mass > 0.0F) ? (1.0F / bodies_[i].mass) : 1.0F;
    k3_v[i] = f3[i] * invM;
    k3_x[i] = s2[i].velocity;

    s3[i].position += dt * k3_x[i];
    s3[i].velocity += dt * k3_v[i];
  }

  // RK4 Stage 4
  std::vector<glm::vec3> f4;
  computeForces(s3, f4);

  std::vector<glm::vec3> k4_v(n);
  std::vector<glm::vec3> k4_x(n);

  for (std::size_t i = 0; i < n; ++i) {
    if (bodies_[i].pinned) {
      continue;
    }
    const float invM =
        (bodies_[i].mass > 0.0F) ? (1.0F / bodies_[i].mass) : 1.0F;
    k4_v[i] = f4[i] * invM;
    k4_x[i] = s3[i].velocity;

    // RK4 final weighted blend
    bodies_[i].position +=
        (dt / 6.0F) * (k1_x[i] + 2.0F * k2_x[i] + 2.0F * k3_x[i] + k4_x[i]);
    bodies_[i].velocity +=
        (dt / 6.0F) * (k1_v[i] + 2.0F * k2_v[i] + 2.0F * k3_v[i] + k4_v[i]);
    bodies_[i].force = f4[i];
  }
}

bool TensionLayoutEngine::isSettled() const {
  for (const auto &b : bodies_) {
    if (!b.pinned &&
        glm::length(b.velocity) > params_.settleVelocityThreshold) {
      return false;
    }
  }
  return true;
}

void TensionLayoutEngine::solveEquilibrium() {
  if (bodies_.empty()) {
    return;
  }

  // 1. Separate foreground and background
  float currX  = 0.0F;
  bool firstFg = true;

  for (auto &b : bodies_) {
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

  // 2. Align constrained pairs vertically
  for (const auto &c : constraints_) {
    auto *const near = findBody(c.fromDoc);
    auto *const far  = findBody(c.toDoc);
    if (near != nullptr && far != nullptr) {
      const float deltaY = c.nearAnchorY - c.farAnchorY;
      far->position.y    = near->position.y + deltaY;
    }
  }
}

} // namespace xanadu
