#include "common/xanadu/shallow_alignment.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <vector>

namespace xanadu {
namespace {
constexpr std::size_t maximumCandidateBudget = 129;

auto keyOf(const SlopeFragment &fragment) {
  return std::tuple(fragment.link, fragment.right, fragment.occurrence,
                    fragment.fragment);
}

auto occurrenceOf(const SlopeFragment &fragment) {
  return std::tuple(fragment.link, fragment.right, fragment.occurrence);
}

struct Attachment {
  SlopeFragment fragment;
  double weight{};
};

struct Group {
  std::size_t begin{};
  std::size_t end{};
  double leftCount{};
  double rightCount{};
};
} // namespace

std::expected<ShallowAlignmentResult, ShallowAlignmentError>
preferShallowAlignment(const std::span<const SlopeFragment> fragments,
                       const ShallowAlignmentParams &params) {
  const auto finite = [](const double value) { return std::isfinite(value); };
  if (!finite(params.minimumDelta) || !finite(params.maximumDelta) ||
      params.minimumDelta > 0 || params.maximumDelta < 0 ||
      !finite(params.minimumRun) || params.minimumRun <= 0 ||
      !finite(params.slopeSaturation) || params.slopeSaturation <= 0 ||
      !finite(params.movementScale) || params.movementScale <= 0 ||
      !finite(params.movementWeight) || params.movementWeight < 0 ||
      !finite(params.worstSlopeWeight) || params.worstSlopeWeight < 0 ||
      !finite(params.minimumImprovement) || params.minimumImprovement < 0 ||
      !finite(params.gatheringShare) || params.gatheringShare <= 0 ||
      params.gatheringShare >= .5 || params.candidates < 3 ||
      params.candidates > maximumCandidateBudget ||
      params.maximumFragments == 0) {
    return std::unexpected(ShallowAlignmentError::InvalidParameters);
  }
  // Refuse before allocation or sorting; truncation would silently privilege
  // the first visible occurrences and give an incomplete objective.
  if (fragments.size() > params.maximumFragments) {
    return std::unexpected(ShallowAlignmentError::WorkLimit);
  }
  std::vector<Attachment> attachments;
  attachments.reserve(fragments.size());
  for (const auto &fragment : fragments) {
    if (!fragment.visible) continue;
    if (!finite(fragment.x) || !finite(fragment.y)) {
      return std::unexpected(ShallowAlignmentError::InvalidFragment);
    }
    attachments.push_back({fragment, 0});
  }
  std::ranges::sort(attachments, [](const auto &a, const auto &b) {
    return keyOf(a.fragment) < keyOf(b.fragment);
  });
  for (std::size_t i = 1; i < attachments.size(); ++i) {
    const auto &a = attachments[i - 1].fragment;
    const auto &b = attachments[i].fragment;
    if (keyOf(a) == keyOf(b) &&
        (a.x != b.x || a.y != b.y || a.translated != b.translated)) {
      return std::unexpected(ShallowAlignmentError::ConflictingDuplicate);
    }
  }
  const auto unique =
      std::ranges::unique(attachments, [](const auto &a, const auto &b) {
        return keyOf(a.fragment) == keyOf(b.fragment);
      });
  attachments.erase(unique.begin(), unique.end());
  std::size_t occurrences = 0;
  for (std::size_t begin = 0; begin < attachments.size();) {
    std::size_t end = begin + 1;
    while (end < attachments.size() &&
           occurrenceOf(attachments[begin].fragment) ==
               occurrenceOf(attachments[end].fragment)) {
      ++end;
    }
    const double weight = 1.0 / static_cast<double>(end - begin);
    for (auto i = begin; i < end; ++i) attachments[i].weight = weight;
    ++occurrences;
    begin = end;
  }
  std::vector<Group> groups;
  for (std::size_t begin = 0; begin < attachments.size();) {
    std::size_t end = begin;
    Group group{.begin = begin};
    while (end < attachments.size() &&
           attachments[begin].fragment.link == attachments[end].fragment.link) {
      const auto &attachment = attachments[end];
      (attachment.fragment.right ? group.rightCount : group.leftCount) +=
          attachment.weight;
      ++end;
    }
    group.end = end;
    if (group.leftCount > 0 && group.rightCount > 0) groups.push_back(group);
    begin = end;
  }
  const auto slope = [&params](const double dx, const double dy) {
    const double run = std::max(std::abs(dx), params.minimumRun);
    // atan bounds distant/extreme members without a flat cutoff that would
    // remove their incentive to improve altogether.
    const double angle = std::atan((dy / run) / params.slopeSaturation);
    return angle * angle;
  };
  const auto cost = [&](const double delta) {
    double sum   = 0;
    double votes = 0;
    double worst = 0;
    for (const auto &group : groups) {
      double leftX = 0, leftY = 0, rightX = 0, rightY = 0;
      for (auto i = group.begin; i < group.end; ++i) {
        const auto &attachment = attachments[i];
        const auto &fragment   = attachment.fragment;
        const double y         = fragment.y + (fragment.translated ? delta : 0);
        (fragment.right ? rightX : leftX) += fragment.x * attachment.weight;
        (fragment.right ? rightY : leftY) += y * attachment.weight;
      }
      leftX /= group.leftCount;
      leftY /= group.leftCount;
      rightX /= group.rightCount;
      rightY /= group.rightCount;
      const double runX = rightX - leftX, runY = rightY - leftY;
      const double leftHubX  = leftX + runX * params.gatheringShare;
      const double leftHubY  = leftY + runY * params.gatheringShare;
      const double rightHubX = rightX - runX * params.gatheringShare;
      const double rightHubY = rightY - runY * params.gatheringShare;
      const double count     = group.leftCount + group.rightCount;
      const double trunk = slope(rightHubX - leftHubX, rightHubY - leftHubY);
      double branches    = 0;
      for (auto i = group.begin; i < group.end; ++i) {
        const auto &attachment = attachments[i];
        const auto &fragment   = attachment.fragment;
        const double hubX      = fragment.right ? rightHubX : leftHubX;
        const double hubY      = fragment.right ? rightHubY : leftHubY;
        const double y         = fragment.y + (fragment.translated ? delta : 0);
        const double branch    = slope(hubX - fragment.x, hubY - y);
        branches += attachment.weight * branch;
        worst = std::max(worst, branch);
      }
      sum += .5 * (branches + count * trunk);
      worst = std::max(worst, trunk);
      votes += count;
    }
    const double movement = delta / params.movementScale;
    return (votes > 0 ? sum / votes : 0) + params.worstSlopeWeight * worst +
           params.movementWeight * movement * movement;
  };
  ShallowAlignmentResult result{.fragments   = attachments.size(),
                                .occurrences = occurrences,
                                .evaluations = 1};
  result.originalCost = result.cost = cost(0);
  if (!std::isfinite(result.cost)) {
    return std::unexpected(ShallowAlignmentError::NumericalOverflow);
  }
  if (groups.empty()) return result;
  for (std::size_t i = 0; i < params.candidates; ++i) {
    const double share =
        static_cast<double>(i) / static_cast<double>(params.candidates - 1);
    const double delta =
        std::lerp(params.minimumDelta, params.maximumDelta, share);
    const double candidate = cost(delta);
    if (!std::isfinite(candidate)) {
      return std::unexpected(ShallowAlignmentError::NumericalOverflow);
    }
    ++result.evaluations;
    if (candidate < result.cost || (candidate == result.cost &&
                                    std::abs(delta) < std::abs(result.delta))) {
      result.delta = delta;
      result.cost  = candidate;
    }
  }
  if (result.originalCost - result.cost <= params.minimumImprovement) {
    result.delta = 0;
    result.cost  = result.originalCost;
  }
  return result;
}
} // namespace xanadu
