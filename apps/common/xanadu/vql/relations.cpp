#include "common/xanadu/vql/relations.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace xanadu::vql {
namespace {

using IndexList = std::vector<std::size_t>;
using KeyIndex  = std::unordered_map<std::string, IndexList>;

RelationJoinError error(const RelationJoinErrorCode code, std::string message,
                        const std::optional<std::size_t> left  = {},
                        const std::optional<std::size_t> right = {}) {
  return {.code            = code,
          .message         = std::move(message),
          .leftOccurrence  = left,
          .rightOccurrence = right};
}

bool incompleteCoverage(const RelationInputCoverage coverage) {
  return coverage != RelationInputCoverage::Complete;
}

std::optional<RelationJoinError>
validateKey(const RelationKey &key, const std::size_t index, const bool left,
            std::size_t &keyBytes, const std::size_t maxKeyBytes) {
  if (key.state != RelationKeyState::Missing &&
      key.state != RelationKeyState::Null &&
      key.state != RelationKeyState::Present)
    return error(RelationJoinErrorCode::InvalidKey,
                 "relation key has an unknown state",
                 left ? std::optional(index) : std::nullopt,
                 left ? std::nullopt : std::optional(index));
  if (key.state != RelationKeyState::Present && !key.canonicalBytes.empty()) {
    return error(RelationJoinErrorCode::InvalidKey,
                 "missing and null keys must have empty canonical bytes",
                 left ? std::optional(index) : std::nullopt,
                 left ? std::nullopt : std::optional(index));
  }
  if (key.canonicalBytes.size() >
      maxKeyBytes - std::min(keyBytes, maxKeyBytes)) {
    return error(RelationJoinErrorCode::KeyByteLimitExceeded,
                 "relation key byte limit exceeded",
                 left ? std::optional(index) : std::nullopt,
                 left ? std::nullopt : std::optional(index));
  }
  keyBytes += key.canonicalBytes.size();
  return {};
}

bool takeWork(RelationJoinResult &result, const std::size_t limit) {
  if (result.work == limit) return false;
  ++result.work;
  return true;
}

void markPartial(RelationJoinResult &result, std::string reason) {
  result.truncated = true;
  if (!result.gap) result.gap = std::move(reason);
}

} // namespace

std::expected<RelationJoinResult, RelationJoinError>
joinOccurrences(const RelationJoinRequest &request, const std::size_t leftSize,
                const RelationKeyAt leftKey, const std::size_t rightSize,
                const RelationKeyAt rightKey) {
  const auto &limits = request.limits;
  if (limits.maxInputRows == 0 || limits.maxKeyBytes == 0 ||
      limits.maxOutputPairs == 0 || limits.maxWork == 0 ||
      limits.maxPolicyBytes == 0)
    return std::unexpected(error(RelationJoinErrorCode::InvalidLimits,
                                 "relation limits must be positive"));
  if (request.keyPolicy.empty())
    return std::unexpected(error(RelationJoinErrorCode::MissingKeyPolicy,
                                 "relation key policy must be named"));
  if (request.keyPolicy.size() > limits.maxPolicyBytes)
    return std::unexpected(error(RelationJoinErrorCode::PolicyByteLimitExceeded,
                                 "relation key policy byte limit exceeded"));
  if ((request.kind != RelationJoinKind::Inner &&
       request.kind != RelationJoinKind::Left) ||
      (request.cardinality != RelationCardinality::Many &&
       request.cardinality != RelationCardinality::AtMostOne) ||
      (request.nullMatch != RelationNullMatchPolicy::Never &&
       request.nullMatch != RelationNullMatchPolicy::Equal) ||
      (request.leftCoverage != RelationInputCoverage::Complete &&
       request.leftCoverage != RelationInputCoverage::Partial &&
       request.leftCoverage != RelationInputCoverage::Gapped) ||
      (request.rightCoverage != RelationInputCoverage::Complete &&
       request.rightCoverage != RelationInputCoverage::Partial &&
       request.rightCoverage != RelationInputCoverage::Gapped))
    return std::unexpected(error(RelationJoinErrorCode::InvalidPolicy,
                                 "relation policy contains an unknown value"));
  if (request.cardinality == RelationCardinality::AtMostOne &&
      request.rightCoverage != RelationInputCoverage::Complete)
    return std::unexpected(
        error(RelationJoinErrorCode::InsufficientCoverage,
              "atMostOne requires complete right input coverage"));
  if (leftSize > limits.maxInputRows || rightSize > limits.maxInputRows ||
      rightSize > limits.maxInputRows - std::min(leftSize, limits.maxInputRows))
    return std::unexpected(error(RelationJoinErrorCode::InputLimitExceeded,
                                 "relation input row limit exceeded"));

  RelationJoinResult result{
      .leftCoverage  = request.leftCoverage,
      .rightCoverage = request.rightCoverage,
      .inputCoverage =
          request.leftCoverage == RelationInputCoverage::Gapped ||
                  request.rightCoverage == RelationInputCoverage::Gapped
              ? RelationInputCoverage::Gapped
          : incompleteCoverage(request.leftCoverage) ||
                  incompleteCoverage(request.rightCoverage)
              ? RelationInputCoverage::Partial
              : RelationInputCoverage::Complete,
      .keyPolicy   = request.keyPolicy,
      .kind        = request.kind,
      .cardinality = request.cardinality,
      .nullMatch   = request.nullMatch};

  KeyIndex rightIndex;
  IndexList matchingNulls;
  std::vector<RelationKey> leftKeys;
  leftKeys.reserve(leftSize);
  std::size_t keyBytes{};
  for (std::size_t index = 0; index < rightSize; ++index) {
    if (!takeWork(result, limits.maxWork)) {
      markPartial(result, "work limit reached while indexing right input");
      return result;
    }
    auto key = rightKey(index);
    if (const auto invalid =
            validateKey(key, index, false, keyBytes, limits.maxKeyBytes))
      return std::unexpected(*invalid);
    if (key.state == RelationKeyState::Present)
      rightIndex[key.canonicalBytes].push_back(index);
    else if (key.state == RelationKeyState::Null &&
             request.nullMatch == RelationNullMatchPolicy::Equal)
      matchingNulls.push_back(index);
  }

  for (std::size_t index = 0; index < leftSize; ++index) {
    if (!takeWork(result, limits.maxWork)) {
      markPartial(result, "work limit reached while reading left input");
      return result;
    }
    auto key = leftKey(index);
    if (const auto invalid =
            validateKey(key, index, true, keyBytes, limits.maxKeyBytes))
      return std::unexpected(*invalid);
    leftKeys.push_back(std::move(key));
  }

  const auto matchesFor = [&](const RelationKey &key) -> const IndexList * {
    if (key.state == RelationKeyState::Present) {
      const auto found = rightIndex.find(key.canonicalBytes);
      return found == rightIndex.end() ? nullptr : &found->second;
    }
    if (key.state == RelationKeyState::Null &&
        request.nullMatch == RelationNullMatchPolicy::Equal)
      return &matchingNulls;
    return nullptr;
  };

  if (request.cardinality == RelationCardinality::AtMostOne) {
    for (std::size_t left = 0; left < leftKeys.size(); ++left) {
      const auto *matches = matchesFor(leftKeys[left]);
      if (!matches) continue;
      for (const auto right : *matches) {
        if (!takeWork(result, limits.maxWork)) {
          markPartial(result,
                      "work limit reached while checking key cardinality");
          result.pairs.clear();
          return result;
        }
        if (matches->size() > 1)
          return std::unexpected(
              error(RelationJoinErrorCode::CardinalityViolation,
                    "atMostOne relation has multiple right occurrences for a "
                    "left key",
                    left, right));
      }
    }
  }

  for (std::size_t left = 0; left < leftKeys.size(); ++left) {
    const auto *matches = matchesFor(leftKeys[left]);
    if (matches && !matches->empty()) {
      for (const auto right : *matches) {
        if (!takeWork(result, limits.maxWork)) {
          markPartial(result, "work limit reached while emitting join pairs");
          return result;
        }
        if (result.pairs.size() == limits.maxOutputPairs) {
          markPartial(result, "output pair limit reached");
          return result;
        }
        result.pairs.push_back({left, right, RelationPairState::Matched});
      }
    } else if (request.kind == RelationJoinKind::Left) {
      if (!takeWork(result, limits.maxWork)) {
        markPartial(result, "work limit reached while emitting left rows");
        return result;
      }
      if (result.pairs.size() == limits.maxOutputPairs) {
        markPartial(result, "output pair limit reached");
        return result;
      }
      const auto pairState = incompleteCoverage(request.rightCoverage)
                                 ? RelationPairState::UnknownCoverage
                                 : RelationPairState::Unmatched;
      result.pairs.push_back({left, {}, pairState});
    }
  }
  return result;
}

std::string_view
relationCoverageName(const RelationInputCoverage coverage) noexcept {
  switch (coverage) {
  case RelationInputCoverage::Complete:
    return "complete";
  case RelationInputCoverage::Partial:
    return "partial";
  case RelationInputCoverage::Gapped:
    return "gapped";
  }
  return "partial";
}

std::string_view relationPairStateName(const RelationPairState state) noexcept {
  switch (state) {
  case RelationPairState::Matched:
    return "matched";
  case RelationPairState::Unmatched:
    return "unmatched";
  case RelationPairState::UnknownCoverage:
    return "unknown-within-coverage";
  }
  return "unknown-within-coverage";
}

} // namespace xanadu::vql
