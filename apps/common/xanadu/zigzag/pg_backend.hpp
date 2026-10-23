/**
 * @file pg_backend.hpp
 * @brief PostgreSQL backend for the sql lens (spec section 7.3).
 *
 * Built under GLEDITOR_HAVE_PG (the Makefile's GLEDITOR_SQL_PG probe);
 * without libpq the same entry point exists and refuses with an
 * actionable error, so `sql:pg:` URIs always parse.
 */
#ifndef COMMON_XANADU_ZIGZAG_PG_BACKEND_HPP
#define COMMON_XANADU_ZIGZAG_PG_BACKEND_HPP

#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include "common/xanadu/zigzag/sql_backend.hpp"

namespace zigzag {

/// Open a PostgreSQL connection string (`postgresql://...` or a libpq
/// keyword list). Read-only: the session sets
/// `default_transaction_read_only` and every unit of work opens inside
/// `BEGIN READ ONLY` -- the hard guarantee still needs a read-only
/// role server-side (spec section 7.3).
[[nodiscard]] std::expected<std::unique_ptr<SqlBackend>, std::string>
openPgBackend(std::string_view conninfo);

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_PG_BACKEND_HPP
