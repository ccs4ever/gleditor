// Q4: engine-side watch, perspective-block, mount and unmount entry points.
//
// Statements and blocks run through the same compile-and-run path as any
// other query, so their refusals and their scoping come from the compiler.
// A subscription keeps its parsed path only to recompile it: pollWatch runs
// the compiled program on the VM and never executes the AST.
#include <stdexcept>

#include "common/xanadu/vql/vql_engine.hpp"

namespace xanadu::vql {

std::vector<zigzag::CellRef>
VQLEngine::executePerspectiveBlock(const PerspectiveBlock &block) {
  return executeStatement(Statement{block});
}

std::vector<zigzag::CellRef>
VQLEngine::executeWatch(const WatchStatement &watchStmt) {
  return executeStatement(Statement{watchStmt});
}

VQLEngine::WatchId VQLEngine::subscribeWatch(PathExpression path) {
  if (path.anchor.kind == AnchorKind::StoreGenerator && path.anchor.generator) {
    const auto &name = path.anchor.generator->opName;
    if (name != "current" && name != "local" && name != "system")
      throw QueryRefusal(QueryErrorCategory::Unsupported,
                         "watch over remote discovery '" + name +
                             "' requires a connected ZZ9P session");
  }
  if (nextWatchId_ == 0)
    throw std::overflow_error("VQL watch subscription identifiers exhausted");
  const WatchId id = nextWatchId_++;
  watches_.emplace(id, WatchSubscription{.path = std::move(path)});
  return id;
}

void VQLEngine::executeMount(const MountDeclaration &mount) {
  QueryExpression query;
  query.namespaceDeclarations.push_back(mount);
  (void)execute(query);
}

void VQLEngine::executeUnmount(const UnmountDeclaration &unmount) {
  QueryExpression query;
  query.namespaceDeclarations.push_back(unmount);
  (void)execute(query);
}

} // namespace xanadu::vql
