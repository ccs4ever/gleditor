#include "common/xanadu/evidence/effect_adapter.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <type_traits>
#include <utility>

namespace xanadu::evidence {
namespace {

using Clock = std::chrono::steady_clock;

struct DatabaseCloser {
  void operator()(sqlite3 *db) const { sqlite3_close_v2(db); }
};
struct StatementCloser {
  void operator()(sqlite3_stmt *statement) const {
    sqlite3_finalize(statement);
  }
};
using Database  = std::unique_ptr<sqlite3, DatabaseCloser>;
using Statement = std::unique_ptr<sqlite3_stmt, StatementCloser>;

struct ProgressContext {
  const std::atomic_bool *cancel{};
  const EffectAuthority *authority{};
  Clock::time_point deadline;
};

bool stopped(const ProgressContext &context) {
  return context.cancel->load(std::memory_order_relaxed) ||
         context.authority->revoked.load(std::memory_order_relaxed) ||
         Clock::now() >= context.deadline;
}

int progress(void *opaque) {
  return stopped(*static_cast<const ProgressContext *>(opaque)) ? 1 : 0;
}

int busy(void *opaque, int priorAttempts) {
  const auto &context = *static_cast<const ProgressContext *>(opaque);
  if (stopped(context) || priorAttempts >= 1000) return 0;
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  return stopped(context) ? 0 : 1;
}

int authorizePatch(void *, int action, const char *arg1, const char *arg2,
                   const char *, const char *source) {
  if (source) return SQLITE_DENY;
  switch (action) {
  case SQLITE_SELECT:
  case SQLITE_READ:
  case SQLITE_RECURSIVE:
  case SQLITE_INSERT:
  case SQLITE_UPDATE:
  case SQLITE_DELETE:
  case SQLITE_TRANSACTION:
  case SQLITE_SAVEPOINT:
    return SQLITE_OK;
  case SQLITE_FUNCTION: {
    const char *name = arg2 ? arg2 : arg1;
    if (name && (sqlite3_stricmp(name, "load_extension") == 0 ||
                 sqlite3_stricmp(name, "readfile") == 0 ||
                 sqlite3_stricmp(name, "writefile") == 0))
      return SQLITE_DENY;
    return SQLITE_OK;
  }
  default:
    return SQLITE_DENY;
  }
}

bool bindValue(sqlite3_stmt *statement, const int index, const Value &value) {
  return std::visit(
      [statement, index](const auto &item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return sqlite3_bind_null(statement, index) == SQLITE_OK;
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          return sqlite3_bind_int64(statement, index, item) == SQLITE_OK;
        } else if constexpr (std::is_same_v<T, double>) {
          return std::isfinite(item) &&
                 sqlite3_bind_double(statement, index, item) == SQLITE_OK;
        } else if constexpr (std::is_same_v<T, std::string>) {
          return sqlite3_bind_text64(statement, index, item.data(), item.size(),
                                     SQLITE_TRANSIENT,
                                     SQLITE_UTF8) == SQLITE_OK;
        } else {
          static constexpr char emptyBlob = '\0';
          const void *bytes =
              item.bytes.empty() ? &emptyBlob : item.bytes.data();
          return sqlite3_bind_blob64(statement, index, bytes, item.bytes.size(),
                                     SQLITE_TRANSIENT) == SQLITE_OK;
        }
      },
      value);
}

bool valueEquals(sqlite3_stmt *statement, const int column,
                 const Value &expected) {
  return std::visit(
      [statement, column](const auto &item) {
        using T        = std::decay_t<decltype(item)>;
        const int type = sqlite3_column_type(statement, column);
        if constexpr (std::is_same_v<T, std::monostate>) {
          return type == SQLITE_NULL;
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          return type == SQLITE_INTEGER &&
                 sqlite3_column_int64(statement, column) == item;
        } else if constexpr (std::is_same_v<T, double>) {
          return std::isfinite(item) && type == SQLITE_FLOAT &&
                 sqlite3_column_double(statement, column) == item;
        } else if constexpr (std::is_same_v<T, std::string>) {
          if (type != SQLITE_TEXT) return false;
          const auto *bytes = reinterpret_cast<const char *>(
              sqlite3_column_text(statement, column));
          const auto size = sqlite3_column_bytes(statement, column);
          return size >= 0 && static_cast<std::size_t>(size) == item.size() &&
                 (size == 0 || std::string_view(bytes, static_cast<std::size_t>(
                                                           size)) == item);
        } else {
          if (type != SQLITE_BLOB) return false;
          const auto *bytes =
              static_cast<const char *>(sqlite3_column_blob(statement, column));
          const auto size = sqlite3_column_bytes(statement, column);
          return size >= 0 &&
                 static_cast<std::size_t>(size) == item.bytes.size() &&
                 (size == 0 ||
                  std::string_view(bytes, static_cast<std::size_t>(size)) ==
                      item.bytes);
        }
      },
      expected);
}

bool hasOnlyOneStatement(sqlite3 *db, const std::string &sql,
                         sqlite3_stmt *&statement) {
  if (sql.empty() || sql.find('\0') != std::string::npos ||
      sql.size() > static_cast<std::size_t>(INT_MAX)) {
    return false;
  }
  const char *tail = nullptr;
  const auto rc    = sqlite3_prepare_v3(
      db, sql.c_str(), static_cast<int>(sql.size()), 0, &statement, &tail);
  if (rc != SQLITE_OK || !statement) return false;
  sqlite3_stmt *extraRaw = nullptr;
  const int extraRc = sqlite3_prepare_v3(db, tail, -1, 0, &extraRaw, nullptr);
  Statement extra(extraRaw);
  return extraRc == SQLITE_OK && !extra;
}

std::size_t valueBytes(const Value &value) {
  return std::visit(
      [](const auto &item) -> std::size_t {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::string>) return item.size();
        if constexpr (std::is_same_v<T, Binary>) return item.bytes.size();
        return sizeof(item);
      },
      value);
}

bool bindAll(sqlite3_stmt *statement, const std::vector<Value> &values) {
  if (sqlite3_bind_parameter_count(statement) < 0 ||
      static_cast<std::size_t>(sqlite3_bind_parameter_count(statement)) !=
          values.size()) {
    return false;
  }
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (!bindValue(statement, static_cast<int>(i + 1), values[i])) return false;
  }
  return true;
}

} // namespace

EffectResult executeSqlPatch(const EffectRequest &request,
                             const EffectAuthority &authority,
                             const std::atomic_bool &cancel) {
  EffectResult result;
  auto reject = [&](std::string detail) {
    result.outcome = EffectOutcome::Rejected;
    result.detail  = std::move(detail);
    return result;
  };
  auto cancelled = [&] {
    result.outcome = EffectOutcome::Cancelled;
    result.detail  = "SQL patch cancelled or revoked before commit";
    return result;
  };

  if (request.kind != EffectKind::SqlPatch ||
      authority.operation != EffectKind::SqlPatch) {
    return reject("operation authority does not permit an SQL patch");
  }
  if (const auto problem = effectAuthorityProblem(request, authority, cancel)) {
    if (cancel.load(std::memory_order_relaxed) ||
        authority.revoked.load(std::memory_order_relaxed))
      return cancelled();
    return reject(*problem);
  }
  if (authority.targetFingerprint.empty()) {
    return reject("SQL patch authority has no pinned target identity");
  }
  if (request.limits.timeoutMs == 0 || request.limits.maxInputBytes == 0) {
    return reject("SQL patch limits must be positive");
  }
  std::size_t inputBytes = request.sql.size();
  for (const auto &value : request.parameters) {
    if (valueBytes(value) >
        request.limits.maxInputBytes -
            std::min(inputBytes,
                     static_cast<std::size_t>(request.limits.maxInputBytes))) {
      return reject("SQL patch input exceeds its byte limit");
    }
    inputBytes += valueBytes(value);
  }
  if (request.guard) {
    if (request.guard->query.size() >
        request.limits.maxInputBytes -
            std::min(inputBytes,
                     static_cast<std::size_t>(request.limits.maxInputBytes))) {
      return reject("SQL guard exceeds its byte limit");
    }
    inputBytes += request.guard->query.size();
    for (const auto &value : request.guard->parameters) {
      if (valueBytes(value) >
          request.limits.maxInputBytes -
              std::min(inputBytes, static_cast<std::size_t>(
                                       request.limits.maxInputBytes))) {
        return reject("SQL guard parameters exceed the byte limit");
      }
      inputBytes += valueBytes(value);
    }
  }
  if (inputBytes > request.limits.maxInputBytes) {
    return reject("SQL patch input exceeds its byte limit");
  }

  std::string canonical;
  try {
    canonical = canonicalEffectTarget(request);
  } catch (...) {
    return reject("SQL patch target cannot be resolved");
  }
  result.resolvedTarget = canonical;
  if (canonical.empty() || canonical != authority.canonicalTarget) {
    return reject("SQL patch target is outside its captured authority");
  }
  struct stat info{};
  if (stat(canonical.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
    return reject("SQL patch target must be an existing regular database file");
  }
  if (const auto problem = effectAuthorityProblem(request, authority, cancel)) {
    if (cancel.load(std::memory_order_relaxed) ||
        authority.revoked.load(std::memory_order_relaxed))
      return cancelled();
    return reject(*problem);
  }

  if (cancel.load(std::memory_order_relaxed) ||
      authority.revoked.load(std::memory_order_relaxed)) {
    return cancelled();
  }

  const auto deadline =
      Clock::now() + std::chrono::milliseconds(request.limits.timeoutMs);
  ProgressContext progressContext{&cancel, &authority, deadline};
  sqlite3 *rawDb = nullptr;
  const int openRc =
      sqlite3_open_v2(canonical.c_str(), &rawDb,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr);
  Database db(rawDb);
  if (openRc != SQLITE_OK || !db) {
    result.outcome = EffectOutcome::Failed;
    result.detail  = "unable to open authorized SQLite database read/write";
    return result;
  }
  sqlite3_enable_load_extension(db.get(), 0);
  sqlite3_extended_result_codes(db.get(), 1);
  sqlite3_busy_handler(db.get(), busy, &progressContext);
  sqlite3_progress_handler(db.get(), 500, progress, &progressContext);
  sqlite3_set_authorizer(db.get(), authorizePatch, nullptr);

  sqlite3_stmt *patchRaw = nullptr;
  if (!hasOnlyOneStatement(db.get(), request.sql, patchRaw)) {
    Statement patch(patchRaw);
    result.outcome = EffectOutcome::Rejected;
    result.detail  = "SQL patch must be exactly one permitted statement";
    return result;
  }
  Statement patch(patchRaw);
  if (sqlite3_stmt_readonly(patch.get()) ||
      sqlite3_column_count(patch.get()) != 0 ||
      !bindAll(patch.get(), request.parameters)) {
    result.outcome = EffectOutcome::Rejected;
    result.detail  = "SQL patch must be a bound write with no result columns";
    return result;
  }

  Statement guard;
  if (request.guard) {
    sqlite3_stmt *guardRaw = nullptr;
    if (!hasOnlyOneStatement(db.get(), request.guard->query, guardRaw)) {
      Statement invalid(guardRaw);
      result.outcome = EffectOutcome::Rejected;
      result.detail  = "SQL guard must be exactly one permitted statement";
      return result;
    }
    guard.reset(guardRaw);
    if (!sqlite3_stmt_readonly(guard.get()) ||
        sqlite3_column_count(guard.get()) != 1 ||
        !bindAll(guard.get(), request.guard->parameters)) {
      result.outcome = EffectOutcome::Rejected;
      result.detail  = "SQL guard must be a bound read-only scalar query";
      return result;
    }
  }

  // Authorization is checked immediately before opening a write transaction.
  if (const auto problem = effectAuthorityProblem(request, authority, cancel)) {
    return stopped(progressContext) ? cancelled() : reject(*problem);
  }
  const int beginRc =
      sqlite3_exec(db.get(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
  if (beginRc != SQLITE_OK) {
    if (sqlite3_get_autocommit(db.get()) == 0) {
      sqlite3_progress_handler(db.get(), 0, nullptr, nullptr);
      sqlite3_busy_handler(db.get(), nullptr, nullptr);
      const int rollbackRc =
          sqlite3_exec(db.get(), "ROLLBACK", nullptr, nullptr, nullptr);
      if (rollbackRc != SQLITE_OK || sqlite3_get_autocommit(db.get()) == 0) {
        result.outcome         = EffectOutcome::Unknown;
        result.detail          = "SQL begin failed and rollback is uncertain";
        result.mayHaveExecuted = true;
        return result;
      }
    }
    result.outcome = stopped(progressContext) ? EffectOutcome::Cancelled
                                              : EffectOutcome::Failed;
    result.detail  = "unable to begin SQL patch transaction";
    return result;
  }
  bool transactionOpen = true;
  auto rollback        = [&] {
    if (!transactionOpen) return true;
    sqlite3_progress_handler(db.get(), 0, nullptr, nullptr);
    sqlite3_busy_handler(db.get(), nullptr, nullptr);
    const int rc =
        sqlite3_exec(db.get(), "ROLLBACK", nullptr, nullptr, nullptr);
    const bool ended = sqlite3_get_autocommit(db.get()) != 0;
    transactionOpen  = !ended;
    if (rc != SQLITE_OK && !ended) return false;
    return ended;
  };
  auto uncertainRollback = [&] {
    result.outcome         = EffectOutcome::Unknown;
    result.detail          = "SQL rollback could not be confirmed; reconcile";
    result.mayHaveExecuted = true;
    return result;
  };
  if (guard) {
    if (stopped(progressContext)) {
      if (!rollback()) return uncertainRollback();
      return cancelled();
    }
    int stepRc         = sqlite3_step(guard.get());
    const bool matches = stepRc == SQLITE_ROW &&
                         valueEquals(guard.get(), 0, request.guard->expected);
    if (!matches || sqlite3_step(guard.get()) != SQLITE_DONE) {
      if (!rollback()) return uncertainRollback();
      result.outcome = stopped(progressContext) ? EffectOutcome::Cancelled
                                                : EffectOutcome::Rejected;
      result.detail  = "SQL guard did not match its exact typed expected value";
      return result;
    }
  }
  if (const auto problem = effectAuthorityProblem(request, authority, cancel)) {
    if (!rollback()) return uncertainRollback();
    return stopped(progressContext) ? cancelled() : reject(*problem);
  }
  if (stopped(progressContext)) {
    if (!rollback()) return uncertainRollback();
    return cancelled();
  }

  const int patchRc = sqlite3_step(patch.get());
  if (patchRc != SQLITE_DONE) {
    const bool wasStopped = stopped(progressContext);
    const bool constraint =
        (sqlite3_extended_errcode(db.get()) & 0xff) == SQLITE_CONSTRAINT;
    if (!rollback()) return uncertainRollback();
    result.outcome = wasStopped   ? EffectOutcome::Cancelled
                     : constraint ? EffectOutcome::Rejected
                                  : EffectOutcome::Failed;
    result.detail  = result.outcome == EffectOutcome::Rejected
                         ? "SQL patch rejected by a database constraint"
                         : "SQL patch did not complete";
    return result;
  }
  const auto affected = sqlite3_changes64(db.get());
  result.affectedRows =
      static_cast<std::uint64_t>(std::max<sqlite3_int64>(0, affected));
  if (*result.affectedRows != request.expectedAffectedRows) {
    if (!rollback()) return uncertainRollback();
    result.outcome = EffectOutcome::Rejected;
    result.detail  = "SQL patch affected-row count did not match expectation";
    return result;
  }
  if (const auto problem = effectAuthorityProblem(request, authority, cancel)) {
    if (!rollback()) return uncertainRollback();
    return stopped(progressContext) ? cancelled() : reject(*problem);
  }
  if (stopped(progressContext)) {
    if (!rollback()) return uncertainRollback();
    return cancelled();
  }

  const int commitRc =
      sqlite3_exec(db.get(), "COMMIT", nullptr, nullptr, nullptr);
  if (commitRc != SQLITE_OK) {
    rollback();
    result.outcome         = EffectOutcome::Unknown;
    result.detail          = "SQL commit outcome is uncertain";
    result.mayHaveExecuted = true;
    return result;
  }
  transactionOpen        = false;
  result.outcome         = EffectOutcome::Completed;
  result.detail          = "SQL patch committed";
  result.mayHaveExecuted = true;
  return result;
}

} // namespace xanadu::evidence
