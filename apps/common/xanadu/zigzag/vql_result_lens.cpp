/**
 * @file vql_result_lens.cpp
 * @brief VqlResultLens: a VQL query's answer set as a slice
 *        (design/lens-candidates.md section 3.5).
 */

#include "common/xanadu/zigzag/vql_result_lens.hpp"

#include "common/xanadu/vql/vql_engine.hpp"

namespace zigzag {

VqlResultLens::VqlResultLens(xanadu::vql::VQLEngine &engine)
    : engine_(&engine), connKey_("vq:0") {}

std::vector<std::pair<std::string, std::optional<GenericDim>>>
VqlResultLens::dimensions() const {
  return {{"d.contains", GenericDim::Containment},
          {"d.seq", GenericDim::Sequence},
          {"d.role", std::nullopt},
          {"d.vql", std::nullopt},
          {"d.index", std::nullopt},
          {"d.keystab", std::nullopt}};
}

std::expected<void, std::string>
VqlResultLens::run(const std::string_view vql) {
  answers_ = engine_->execute(vql);
  vql_     = vql;
  connKey_ = "vq:" + std::to_string(runs_++);
  return {};
}

void VqlResultLens::forEachNode(
    gleditor::cpp26::function_ref<void(const SourceNode &)> emit) const {
  if (vql_.empty()) {
    return; // nothing evaluated yet -- a run() is the materialization
  }
  emit({connKey_,
        std::string{vql_},
        {{"d.role", "vr.query"}, {"d.vql", vql_}},
        0});
  std::size_t i = 0;
  for (const auto answer : answers_) {
    emit({connKey_ + ":a:" + std::to_string(i), {}, {}, 0, answer});
    ++i;
  }
}

void VqlResultLens::forEachEdge(
    gleditor::cpp26::function_ref<void(const SourceEdge &)> emit) const {
  if (answers_.empty()) {
    return;
  }
  emit({connKey_, connKey_ + ":a:0", "d.contains", Posward});
  for (std::size_t i = 1; i < answers_.size(); ++i) {
    emit({connKey_ + ":a:" + std::to_string(i - 1),
          connKey_ + ":a:" + std::to_string(i), "d.seq", Posward});
  }
}

bool VqlResultLens::forEachRoot(
    gleditor::cpp26::function_ref<void(const ExternalKey &)> emit) const {
  if (!vql_.empty()) {
    emit(connKey_);
  }
  return true;
}

} // namespace zigzag
