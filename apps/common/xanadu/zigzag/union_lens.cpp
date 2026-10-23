/**
 * @file union_lens.cpp
 * @brief UnionLens: several lenses hosted in one session (U-L1).
 */

#include "common/xanadu/zigzag/union_lens.hpp"

#include <unordered_set>

namespace zigzag {

std::vector<std::pair<std::string, std::optional<GenericDim>>>
UnionLens::dimensions() const {
  // Union with first-wins on name: two children naming the same rank
  // name share the dimension cell; their genericDim hints should
  // agree, and where they don't the earlier child is the authority.
  std::vector<std::pair<std::string, std::optional<GenericDim>>> out;
  std::unordered_set<std::string> seen;
  for (const auto &child : children_) {
    for (const auto &dim : child->dimensions()) {
      if (seen.insert(dim.first).second) {
        out.push_back(dim);
      }
    }
  }
  return out;
}

void UnionLens::forEachNode(
    const gleditor::cpp26::function_ref<void(const SourceNode &)> emit) const {
  for (const auto &child : children_) {
    child->forEachNode(emit);
  }
}

void UnionLens::forEachEdge(
    const gleditor::cpp26::function_ref<void(const SourceEdge &)> emit) const {
  for (const auto &child : children_) {
    child->forEachEdge(emit);
  }
}

bool UnionLens::forEachRoot(
    const gleditor::cpp26::function_ref<void(const ExternalKey &)> emit) const {
  // Every child's declared roots land in the shared session's roots.
  // A child that doesn't declare leaves its share to the structural
  // election.
  bool any = false;
  for (const auto &child : children_) {
    any = child->forEachRoot(emit) || any;
  }
  return any;
}

std::vector<std::pair<std::string, std::string>>
UnionLens::staleAttributes(const ExternalKey &key) const {
  // The key's own lens marks it; first child that returns anything is
  // the owner (children that don't know the key return {}).
  for (const auto &child : children_) {
    if (auto attrs = child->staleAttributes(key); !attrs.empty()) {
      return attrs;
    }
  }
  return {};
}

bool UnionLens::pollChanged() const {
  for (const auto &child : children_) {
    if (child->pollChanged()) {
      return true;
    }
  }
  return false;
}

std::expected<ExternalKey, std::string>
UnionLens::applyContentChange(const ExternalKey &node,
                              const std::string_view newContent) {
  std::string lastError = "no lens owns " + node;
  for (const auto &child : children_) {
    if (auto r = child->applyContentChange(node, newContent); r) {
      return r;
    } else {
      lastError = r.error();
    }
  }
  return std::unexpected(lastError);
}

} // namespace zigzag
