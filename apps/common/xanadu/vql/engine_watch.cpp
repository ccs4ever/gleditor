// Q4: engine-side watch, perspective-block, mount and unmount entry points.
#include "common/xanadu/vql/vql_engine.hpp"

namespace xanadu::vql {

std::vector<zigzag::CellRef>
VQLEngine::executePerspectiveBlock(const PerspectiveBlock &block) {
  (void)block;
  throw QueryRefusal(QueryErrorCategory::Unsupported,
                     "perspective blocks require Vortex graph lowering");
}

std::vector<zigzag::CellRef>
VQLEngine::executeWatch(const WatchStatement &watchStmt) {
  (void)watchStmt;
  throw QueryRefusal(QueryErrorCategory::Unsupported,
                     "watch blocks require Vortex graph lowering");
}

VQLEngine::WatchId VQLEngine::subscribeWatch(PathExpression path) {
  (void)path;
  throw QueryRefusal(QueryErrorCategory::Unsupported,
                     "watch subscriptions require Vortex graph lowering");
}

void VQLEngine::executeMount(const MountDeclaration &mount) {
  (void)mount;
  throw QueryRefusal(QueryErrorCategory::Unsupported,
                     "mount declarations require Vortex graph lowering");
}

void VQLEngine::executeUnmount(const UnmountDeclaration &unmount) {
  (void)unmount;
  throw QueryRefusal(QueryErrorCategory::Unsupported,
                     "unmount declarations require Vortex graph lowering");
}

} // namespace xanadu::vql
