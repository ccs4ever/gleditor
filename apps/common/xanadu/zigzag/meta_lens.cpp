/**
 * @file meta_lens.cpp
 * @brief MetaLens: a frozen fold's cells and dimension census as
 *        project-by-reference nodes (design/lens-candidates.md 3.3).
 */

#include "common/xanadu/zigzag/meta_lens.hpp"

#include "common/xanadu/scroll.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace zigzag {

MetaLens::MetaLens(std::string label, const Manifold &source,
                   const xanadu::SpanReader &reader)
    : connKey_("meta:" + std::move(label)), source_(&source), reader_(&reader) {
}

std::vector<std::pair<std::string, std::optional<GenericDim>>>
MetaLens::dimensions() const {
  return {{"d.contains", GenericDim::Containment},
          {"d.seq", GenericDim::Sequence},
          {"d.ref", GenericDim::Reference},
          {"d.role", std::nullopt}};
}

void MetaLens::forEachNode(
    gleditor::cpp26::function_ref<void(const SourceNode &)> emit) const {
  emit({connKey_,
        std::string{"meta:"} + connKey_.substr(5),
        {{"d.role", "meta.store"}},
        0});
  const auto dimsKey  = connKey_ + ":dims";
  const auto cellsKey = connKey_ + ":cells";
  emit({dimsKey, "dims", {{"d.role", "meta.index"}}, 0});
  emit({cellsKey, "cells", {{"d.role", "meta.index"}}, 0});
  // The dimension census: one ref node per dim, named by its text.
  for (const auto d : source_->dimensions()) {
    emit({connKey_ + ":d:" + source_->textOf(d, *reader_), {}, {}, 0, d});
  }
  // The cell census: every cell the fold minted, keyed on its birth
  // op (the CellRef -- pk-strength identity).
  for (const auto &slot : source_->cells()) {
    emit({connKey_ + ":c:" + std::to_string(slot.birthOp),
          {},
          {},
          0,
          slot.birthOp});
  }
}

void MetaLens::forEachEdge(
    gleditor::cpp26::function_ref<void(const SourceEdge &)> emit) const {
  const auto dimsKey  = connKey_ + ":dims";
  const auto cellsKey = connKey_ + ":cells";
  emit({connKey_, dimsKey, "d.contains", Posward});
  emit({dimsKey, cellsKey, "d.seq", Posward});
  std::string prev;
  for (const auto d : source_->dimensions()) {
    const auto key = connKey_ + ":d:" + source_->textOf(d, *reader_);
    if (prev.empty()) {
      emit({dimsKey, key, "d.contains", Posward});
    } else {
      emit({prev, key, "d.seq", Posward});
    }
    prev = key;
  }
  prev.clear();
  for (const auto &slot : source_->cells()) {
    const auto key = connKey_ + ":c:" + std::to_string(slot.birthOp);
    if (prev.empty()) {
      emit({cellsKey, key, "d.contains", Posward});
    } else {
      emit({prev, key, "d.seq", Posward});
    }
    prev = key;
  }
}

bool MetaLens::forEachRoot(
    gleditor::cpp26::function_ref<void(const ExternalKey &)> emit) const {
  emit(connKey_);
  return true;
}

} // namespace zigzag
