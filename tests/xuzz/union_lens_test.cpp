/**
 * @file union_lens_test.cpp
 * @brief UnionLens: several lenses hosted in one session (U-L1) --
 *        one epoch, same-key unification, atomic refresh.
 */
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"
#include "common/xanadu/zigzag/union_lens.hpp"

namespace {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::ExternalKey;
using zigzag::GenericDim;
using zigzag::OptionalCell;
using zigzag::Posward;
using zigzag::ProjectionLens;
using zigzag::ProjectionSession;
using zigzag::SourceEdge;
using zigzag::SourceNode;
using zigzag::UnionLens;
using zigzag::vortex::VortexCore;

/// A minimal child: a fixed root -> one member.
class ToyLens final : public ProjectionLens {
public:
  explicit ToyLens(std::string name) : name_(std::move(name)) {}
  [[nodiscard]] std::vector<std::pair<std::string, std::optional<GenericDim>>>
  dimensions() const override {
    return {{"d.contains", GenericDim::Containment},
            {"d.seq", GenericDim::Sequence},
            {"d.role", std::nullopt}};
  }
  void forEachNode(gleditor::cpp26::function_ref<void(const SourceNode &)> emit)
      const override {
    emit({"toy:" + name_, name_, {{"d.role", "toy.root"}}, 0});
    emit({"toy:" + name_ + ":m", "member", {}, 0});
  }
  void forEachEdge(gleditor::cpp26::function_ref<void(const SourceEdge &)> emit)
      const override {
    emit({"toy:" + name_, "toy:" + name_ + ":m", "d.contains", Posward});
  }
  bool forEachRoot(gleditor::cpp26::function_ref<void(const ExternalKey &)>
                       emit) const override {
    emit("toy:" + name_);
    return true;
  }
  [[nodiscard]] bool pollChanged() const override { return dirty_; }
  void poke() { dirty_ = true; }

private:
  std::string name_;
  bool dirty_{false};
};

TEST(UnionLensTest, TwoChildrenShareOneEpochAndRoots) {
  ArenaManifold arena;
  VortexCore core{arena};
  auto union_ = std::make_unique<UnionLens>();
  union_->add(std::make_unique<ToyLens>("a"));
  union_->add(std::make_unique<ToyLens>("b"));
  ProjectionSession session(std::move(union_), core);
  EXPECT_TRUE(session.cellFor("toy:a").has_value());
  EXPECT_TRUE(session.cellFor("toy:b").has_value());
  EXPECT_EQ(session.roots().size(), 2u);
}

TEST(UnionLensTest, SameKeyAcrossChildrenIsOneCell) {
  ArenaManifold arena;
  VortexCore core{arena};
  auto union_ = std::make_unique<UnionLens>();
  // Two children emitting the same key: union identity makes it one
  // cell -- the deliberate unification mechanism (U-L1's union rule).
  union_->add(std::make_unique<ToyLens>("x"));
  union_->add(std::make_unique<ToyLens>("x"));
  ProjectionSession session(std::move(union_), core);
  EXPECT_TRUE(session.cellFor("toy:x").has_value());
  EXPECT_TRUE(session.cellFor("toy:x:m").has_value());
  // One cell each, minted once -- not duplicated by the second emitter.
  EXPECT_EQ(session.cellCount(), 2u);
}

TEST(UnionLensTest, PollChangedFoldsChildren) {
  ArenaManifold arena;
  VortexCore core{arena};
  auto union_ = std::make_unique<UnionLens>();
  auto a      = std::make_unique<ToyLens>("a");
  auto b      = std::make_unique<ToyLens>("b");
  ToyLens *la = a.get();
  union_->add(std::move(a));
  union_->add(std::move(b));
  ProjectionSession session(std::move(union_), core);
  EXPECT_FALSE(session.refreshIfChanged());
  la->poke();
  EXPECT_TRUE(session.refreshIfChanged());
}

} // namespace
