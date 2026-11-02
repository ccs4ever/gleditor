#ifndef XANADU_EVIDENCE_QUERY_JOB_HPP
#define XANADU_EVIDENCE_QUERY_JOB_HPP

#include <memory>
#include <optional>

#include "common/xanadu/evidence/query_composition.hpp"

namespace xanadu::evidence {

struct SqliteJoinCompletion {
  bool cancelled{};
  std::expected<vql::RelationJoinResult, QueryCompositionError> result;
};

// Only owned keys and policies cross into the worker. The owner retains the
// prepared lineage separately and fences completion against its case session.
class SqliteJoinJob {
public:
  SqliteJoinJob();
  ~SqliteJoinJob();
  SqliteJoinJob(const SqliteJoinJob &)            = delete;
  SqliteJoinJob &operator=(const SqliteJoinJob &) = delete;
  bool start(SqliteJoinEvaluation evaluation);
  void cancel();
  [[nodiscard]] bool busy() const;
  [[nodiscard]] std::optional<SqliteJoinCompletion> poll();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace xanadu::evidence
#endif
