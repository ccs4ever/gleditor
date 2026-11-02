#include "common/xanadu/evidence/capture.hpp"

#include <curl/curl.h>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <mutex>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <type_traits>
#include <utility>

namespace xanadu::evidence {
namespace {

using Clock = std::chrono::steady_clock;

CURLcode ensureCurlInitialized() {
  static std::once_flag curlInit;
  static CURLcode curlInitResult = CURLE_FAILED_INIT;
  std::call_once(
      curlInit, [] { curlInitResult = curl_global_init(CURL_GLOBAL_DEFAULT); });
  return curlInitResult;
}

std::string timestamp() {
  const auto now  = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return output.str();
}

bool cancelledOrExpired(const std::atomic_bool &cancel,
                        const Clock::time_point deadline) {
  return cancel.load(std::memory_order_relaxed) || Clock::now() >= deadline;
}

struct SqliteCloser {
  void operator()(sqlite3 *db) const { sqlite3_close_v2(db); }
};
struct StatementCloser {
  void operator()(sqlite3_stmt *statement) const {
    sqlite3_finalize(statement);
  }
};
using Database  = std::unique_ptr<sqlite3, SqliteCloser>;
using Statement = std::unique_ptr<sqlite3_stmt, StatementCloser>;

struct Progress {
  const std::atomic_bool *cancel{};
  Clock::time_point deadline;
};

int sqliteProgress(void *context) {
  const auto &progress = *static_cast<Progress *>(context);
  return cancelledOrExpired(*progress.cancel, progress.deadline) ? 1 : 0;
}

int sqliteAuthorizer(void *, int action, const char *arg1, const char *arg2,
                     const char *, const char *) {
  switch (action) {
  case SQLITE_SELECT:
  case SQLITE_READ:
  case SQLITE_RECURSIVE:
    return SQLITE_OK;
  case SQLITE_FUNCTION: {
    const char *name = arg2 ? arg2 : arg1;
    if (name && (sqlite3_stricmp(name, "load_extension") == 0 ||
                 sqlite3_stricmp(name, "writefile") == 0 ||
                 sqlite3_stricmp(name, "readfile") == 0)) {
      return SQLITE_DENY;
    }
    return SQLITE_OK;
  }
  default:
    return SQLITE_DENY;
  }
}

bool bindSqlite(sqlite3_stmt *statement, int index, const Value &value) {
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
          return sqlite3_bind_blob64(statement, index, item.bytes.data(),
                                     item.bytes.size(),
                                     SQLITE_TRANSIENT) == SQLITE_OK;
        }
      },
      value);
}

std::optional<std::string> sqliteFileWitness(const std::string &path) {
  struct stat info{};
  if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
    return {};
  }
  std::error_code error;
  const auto canonical = std::filesystem::canonical(path, error);
  if (error) return {};
  std::ostringstream witness;
  witness << canonical.string() << ':' << info.st_dev << ':' << info.st_ino;
  return witness.str();
}

std::size_t valueBytes(const CapturedValue &value) {
  if (const auto *text = std::get_if<std::string>(&value.value)) {
    return text->size();
  }
  if (const auto *blob = std::get_if<Binary>(&value.value)) {
    return blob->bytes.size();
  }
  return sizeof(value);
}

CaptureResult captureSqlite(const Source &source,
                            const std::atomic_bool &cancel) {
  CaptureResult result;
  result.source    = source;
  result.startedAt = timestamp();
  const auto deadline =
      Clock::now() + std::chrono::milliseconds(source.limits.timeoutMs);
  Database db;
  sqlite3 *rawDb = nullptr;
  const int openResult =
      sqlite3_open_v2(source.locator.c_str(), &rawDb,
                      SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
  db.reset(rawDb);
  if (openResult != SQLITE_OK || !db) {
    result.error =
        db ? sqlite3_errmsg(db.get()) : "unable to open SQLite source";
    result.state      = CaptureState::Failed;
    result.finishedAt = timestamp();
    return result;
  }
  sqlite3_enable_load_extension(db.get(), 0);
  const auto fileWitnessBefore = sqliteFileWitness(source.locator);

  if (source.query.find('\0') != std::string::npos ||
      source.query.size() > static_cast<std::size_t>(INT_MAX)) {
    result.error      = "SQLite query contains NUL or exceeds the parser limit";
    result.state      = CaptureState::Failed;
    result.finishedAt = timestamp();
    return result;
  }

  Progress progress{&cancel, deadline};
  sqlite3_busy_timeout(db.get(), static_cast<int>(std::clamp(
                                     source.limits.timeoutMs, 0L, 2147483L)));
  sqlite3_progress_handler(db.get(), 1000, sqliteProgress, &progress);
  sqlite3_set_authorizer(db.get(), sqliteAuthorizer, nullptr);

  sqlite3_stmt *rawStatement = nullptr;
  const char *tail           = nullptr;
  const int prepareResult    = sqlite3_prepare_v3(
      db.get(), source.query.c_str(), static_cast<int>(source.query.size()), 0,
      &rawStatement, &tail);
  Statement statement(rawStatement);
  if (prepareResult != SQLITE_OK || !statement) {
    result.error      = sqlite3_errmsg(db.get());
    result.state      = CaptureState::Failed;
    result.finishedAt = timestamp();
    return result;
  }
  sqlite3_stmt *extraRaw = nullptr;
  const int extraResult =
      sqlite3_prepare_v3(db.get(), tail, -1, 0, &extraRaw, nullptr);
  Statement extra(extraRaw);
  if (extraResult != SQLITE_OK || extra ||
      !sqlite3_stmt_readonly(statement.get())) {
    result.error      = "SQLite source requires one read-only statement";
    result.state      = CaptureState::Failed;
    result.finishedAt = timestamp();
    return result;
  }
  for (std::size_t index = 0; index < source.parameters.size(); ++index) {
    if (index >= static_cast<std::size_t>(
                     sqlite3_bind_parameter_count(statement.get())) ||
        !bindSqlite(statement.get(), static_cast<int>(index + 1),
                    source.parameters[index])) {
      result.error      = "SQLite parameter binding failed";
      result.state      = CaptureState::Failed;
      result.finishedAt = timestamp();
      return result;
    }
  }
  if (source.parameters.size() !=
      static_cast<std::size_t>(sqlite3_bind_parameter_count(statement.get()))) {
    result.error      = "SQLite parameter count does not match the query";
    result.state      = CaptureState::Failed;
    result.finishedAt = timestamp();
    return result;
  }

  const int columnCount     = sqlite3_column_count(statement.get());
  const int retainedColumns = static_cast<int>(std::min(
      source.limits.maxColumns, static_cast<std::size_t>(columnCount)));
  std::size_t retainedBytes = 0;
  for (int column = 0; column < retainedColumns; ++column) {
    const char *name = sqlite3_column_name(statement.get(), column);
    const char *type = sqlite3_column_decltype(statement.get(), column);
    const std::string_view columnName   = name ? name : "";
    const std::string_view declaredType = type ? type : "";
    const std::size_t remaining =
        source.limits.maxBytes -
        std::min(retainedBytes, source.limits.maxBytes);
    if (columnName.size() > remaining ||
        declaredType.size() > remaining - columnName.size()) {
      result.state    = CaptureState::Partial;
      result.coverage = "column metadata exceeded byte limit";
      break;
    }
    result.columns.push_back(
        {std::string(columnName), std::string(declaredType)});
    retainedBytes += columnName.size() + declaredType.size();
  }
  if (retainedColumns != columnCount ||
      result.columns.size() != static_cast<std::size_t>(retainedColumns) ||
      retainedBytes > source.limits.maxBytes) {
    result.state    = CaptureState::Partial;
    result.coverage = "column or byte limit reached";
    result.columns.resize(
        std::min(result.columns.size(), source.limits.maxColumns));
    if (retainedBytes > source.limits.maxBytes) {
      result.columns.clear();
    }
  }
  if (result.columns.size() != static_cast<std::size_t>(retainedColumns)) {
    result.finishedAt = timestamp();
    return result;
  }

  bool partial = result.state == CaptureState::Partial;
  if (columnCount == 0) {
    result.error = "SQLite source query returned no columns";
    result.state = CaptureState::Failed;
  } else {
    for (;;) {
      if (cancelledOrExpired(cancel, deadline)) {
        result.state =
            cancel.load() ? CaptureState::Cancelled : CaptureState::Partial;
        result.error =
            cancel.load() ? "capture cancelled" : "capture time limit reached";
        break;
      }
      const int step = sqlite3_step(statement.get());
      if (step == SQLITE_DONE) {
        result.state = partial ? CaptureState::Partial : CaptureState::Complete;
        break;
      }
      if (step != SQLITE_ROW) {
        result.state = sqlite3_errcode(db.get()) == SQLITE_INTERRUPT
                           ? (cancel.load() ? CaptureState::Cancelled
                                            : CaptureState::Partial)
                           : (result.rows.empty() ? CaptureState::Failed
                                                  : CaptureState::Partial);
        result.error = sqlite3_errmsg(db.get());
        break;
      }
      if (result.rows.size() >= source.limits.maxRows) {
        result.state    = CaptureState::Partial;
        result.coverage = "row limit reached; source may contain more rows";
        break;
      }
      CapturedRow row;
      std::size_t rowBytes         = sizeof(CapturedRow);
      bool rowTooLarge             = false;
      const auto availableRowBytes = [&] {
        const auto available = source.limits.maxBytes -
                               std::min(retainedBytes, source.limits.maxBytes);
        return rowBytes >= available ? std::size_t{0} : available - rowBytes;
      };
      for (int column = 0; column < retainedColumns; ++column) {
        CapturedValue value;
        switch (sqlite3_column_type(statement.get(), column)) {
        case SQLITE_INTEGER:
          value.storage = StorageClass::Integer;
          value.value   = static_cast<std::int64_t>(
              sqlite3_column_int64(statement.get(), column));
          break;
        case SQLITE_FLOAT:
          value.storage = StorageClass::Real;
          value.value   = sqlite3_column_double(statement.get(), column);
          break;
        case SQLITE_TEXT: {
          value.storage     = StorageClass::Text;
          const auto length = sqlite3_column_bytes(statement.get(), column);
          if (length < 0 ||
              static_cast<std::size_t>(length) > availableRowBytes()) {
            rowTooLarge = true;
            break;
          }
          const auto *data = reinterpret_cast<const char *>(
              sqlite3_column_text(statement.get(), column));
          value.value =
              std::string(data ? data : "", static_cast<std::size_t>(length));
          break;
        }
        case SQLITE_BLOB: {
          value.storage     = StorageClass::Blob;
          const auto length = sqlite3_column_bytes(statement.get(), column);
          if (length < 0 ||
              static_cast<std::size_t>(length) > availableRowBytes()) {
            rowTooLarge = true;
            break;
          }
          const auto *data = static_cast<const char *>(
              sqlite3_column_blob(statement.get(), column));
          value.value = Binary{
              std::string(data ? data : "", static_cast<std::size_t>(length))};
          break;
        }
        default:
          value.storage = StorageClass::Null;
          value.value   = std::monostate{};
          break;
        }
        if (rowTooLarge) break;
        rowBytes += valueBytes(value);
        row.values.push_back(std::move(value));
      }
      if (rowTooLarge ||
          rowBytes > source.limits.maxBytes -
                         std::min(retainedBytes, source.limits.maxBytes)) {
        result.state    = CaptureState::Partial;
        result.coverage = "byte limit reached; final row omitted";
        break;
      }
      retainedBytes += rowBytes;
      result.rows.push_back(std::move(row));
    }
  }
  const auto fileWitnessAfter = sqliteFileWitness(source.locator);
  if (fileWitnessBefore && fileWitnessAfter &&
      fileWitnessBefore == fileWitnessAfter) {
    result.incarnation = fileWitnessAfter;
  }
  if (result.coverage.empty()) {
    result.coverage =
        result.state == CaptureState::Complete
            ? "query exhausted; point-in-time consistency is source-defined"
            : "capture did not establish complete coverage";
  }
  result.finishedAt = timestamp();
  return result;
}

struct UrlParts {
  std::string scheme;
  std::string host;
  std::string port;
  bool hasCredentials{};
};

std::optional<UrlParts> urlParts(std::string_view text) {
  if (text.find('\0') != std::string_view::npos ||
      ensureCurlInitialized() != CURLE_OK) {
    return {};
  }
  CURLU *url = curl_url();
  if (!url) {
    return {};
  }
  const std::string owned(text);
  if (curl_url_set(url, CURLUPART_URL, owned.c_str(), 0) != CURLUE_OK) {
    curl_url_cleanup(url);
    return {};
  }
  char *scheme   = nullptr;
  char *host     = nullptr;
  char *port     = nullptr;
  char *user     = nullptr;
  char *password = nullptr;
  const bool ok =
      curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
      curl_url_get(url, CURLUPART_HOST, &host, 0) == CURLUE_OK;
  curl_url_get(url, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT);
  const bool hasCredentials =
      curl_url_get(url, CURLUPART_USER, &user, 0) == CURLUE_OK ||
      curl_url_get(url, CURLUPART_PASSWORD, &password, 0) == CURLUE_OK;
  UrlParts parts;
  if (ok) {
    parts.scheme         = scheme ? scheme : "";
    parts.host           = host ? host : "";
    parts.port           = port ? port : "";
    parts.hasCredentials = hasCredentials;
  }
  curl_free(scheme);
  curl_free(host);
  curl_free(port);
  curl_free(user);
  curl_free(password);
  curl_url_cleanup(url);
  return ok ? std::optional<UrlParts>(std::move(parts)) : std::nullopt;
}

struct ResolvedLink {
  std::optional<std::string> target;
  bool hasCredentials{};
};

ResolvedLink resolvedSameOrigin(std::string_view base,
                                std::string_view reference) {
  const auto origin = urlParts(base);
  if (!origin || reference.find('\0') != std::string_view::npos) {
    return {};
  }
  CURLU *url = curl_url();
  if (!url) {
    return {};
  }
  const std::string baseOwned(base);
  const std::string refOwned(reference);
  const bool ok =
      curl_url_set(url, CURLUPART_URL, baseOwned.c_str(), 0) == CURLUE_OK &&
      curl_url_set(url, CURLUPART_URL, refOwned.c_str(), 0) == CURLUE_OK;
  char *finalUrl = nullptr;
  const bool got =
      ok && curl_url_get(url, CURLUPART_URL, &finalUrl, 0) == CURLUE_OK;
  std::string target = got && finalUrl ? finalUrl : "";
  curl_free(finalUrl);
  curl_url_cleanup(url);
  const auto targetOrigin = urlParts(target);
  if (!targetOrigin) return {};
  if (targetOrigin->hasCredentials) return {.hasCredentials = true};
  if (targetOrigin->scheme != origin->scheme ||
      targetOrigin->host != origin->host ||
      targetOrigin->port != origin->port) {
    return {};
  }
  return {.target = std::move(target)};
}

std::optional<std::string> validateSource(const Source &source) {
  if (source.limits.timeoutMs < 1) {
    return "capture timeout must be positive";
  }
  const auto maxTimeout = std::chrono::duration_cast<std::chrono::milliseconds>(
                              Clock::time_point::max() - Clock::now())
                              .count();
  if (static_cast<long double>(source.limits.timeoutMs) >
      static_cast<long double>(maxTimeout)) {
    return "capture timeout exceeds the timer range";
  }
  if (source.limits.maxRows > std::vector<CapturedRow>{}.max_size() ||
      source.limits.maxColumns > std::vector<Column>{}.max_size() ||
      source.limits.maxPages > std::vector<Page>{}.max_size() ||
      source.limits.maxBytes > std::string{}.max_size()) {
    return "capture limit exceeds the supported container range";
  }
  if (source.kind == SourceKind::SQLite) {
    if (source.locator.empty() ||
        source.locator.find('\0') != std::string::npos) {
      return "SQLite locator is empty or contains NUL";
    }
    if (source.query.find('\0') != std::string::npos ||
        source.query.size() > static_cast<std::size_t>(INT_MAX)) {
      return "SQLite query contains NUL or exceeds the parser limit";
    }
    if (source.parameters.size() > static_cast<std::size_t>(INT_MAX)) {
      return "SQLite parameter count exceeds the supported range";
    }
    std::size_t parameterBytes = 0;
    for (const auto &parameter : source.parameters) {
      std::size_t size = 0;
      if (const auto *text = std::get_if<std::string>(&parameter)) {
        size = text->size();
      } else if (const auto *blob = std::get_if<Binary>(&parameter)) {
        size = blob->bytes.size();
      } else if (const auto *real = std::get_if<double>(&parameter);
                 real && !std::isfinite(*real)) {
        return "SQLite parameters must contain finite real values";
      }
      if (size > source.limits.maxBytes -
                     std::min(parameterBytes, source.limits.maxBytes)) {
        return "SQLite parameter bytes exceed the capture byte limit";
      }
      parameterBytes += size;
    }
    return {};
  }
  if (source.kind != SourceKind::Rest) {
    return "unknown source kind";
  }
  if (source.locator.find('\0') != std::string::npos) {
    return "REST URL contains NUL";
  }
  if (ensureCurlInitialized() != CURLE_OK) {
    return "libcurl initialization failed";
  }
  const auto parts = urlParts(source.locator);
  if (!parts || (parts->scheme != "http" && parts->scheme != "https")) {
    return "REST source requires an absolute HTTP(S) URL";
  }
  if (parts->hasCredentials) {
    return "REST URL credentials are refused";
  }
  return {};
}

struct HttpResponse {
  std::string body;
  std::string contentType;
  std::string etag;
  std::string lastModified;
  std::string next;
  bool malformedNext{};
  std::size_t *totalBytes{};
  std::size_t byteLimit{};
  const std::atomic_bool *cancel{};
  Clock::time_point deadline;
  bool tooLarge{};
  bool callbackError{};
};

std::string trim(std::string value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  const auto first = std::ranges::find_if_not(
      value, [](const unsigned char ch) { return std::isspace(ch); });
  value.erase(value.begin(), first);
  return value;
}

struct LinkItem {
  std::optional<std::string> next;
  bool malformedNext{};
};

bool relationContainsNext(std::string_view value) {
  std::size_t offset = 0;
  while (offset < value.size()) {
    while (offset < value.size() &&
           std::isspace(static_cast<unsigned char>(value[offset]))) {
      ++offset;
    }
    const auto end = value.find_first_of(" \t\r\n", offset);
    auto token     = value.substr(offset, end == std::string_view::npos
                                              ? value.size() - offset
                                              : end - offset);
    if (token.size() == 4 &&
        std::ranges::equal(token, std::string_view("next"),
                           [](const char left, const char right) {
                             return std::tolower(static_cast<unsigned char>(
                                        left)) == right;
                           })) {
      return true;
    }
    if (end == std::string_view::npos) break;
    offset = end + 1;
  }
  return false;
}

LinkItem parseLinkItem(std::string_view item) {
  LinkItem parsed;
  const auto targetOpen  = item.find('<');
  const auto targetClose = targetOpen == std::string_view::npos
                               ? std::string_view::npos
                               : item.find('>', targetOpen + 1);
  const bool hasTarget   = targetOpen != std::string_view::npos &&
                         targetClose != std::string_view::npos &&
                         targetClose > targetOpen + 1;
  const auto parameterStart = item.find(';');
  std::size_t offset = targetClose != std::string_view::npos ? targetClose + 1
                       : parameterStart == std::string_view::npos
                           ? 0
                           : parameterStart + 1;
  while (offset < item.size()) {
    while (offset < item.size() &&
           (std::isspace(static_cast<unsigned char>(item[offset])) ||
            item[offset] == ';')) {
      ++offset;
    }
    if (offset == item.size()) break;
    const auto keyStart = offset;
    while (offset < item.size() && item[offset] != '=' && item[offset] != ';' &&
           !std::isspace(static_cast<unsigned char>(item[offset]))) {
      ++offset;
    }
    std::string key(item.substr(keyStart, offset - keyStart));
    std::ranges::transform(key, key.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    while (offset < item.size() &&
           std::isspace(static_cast<unsigned char>(item[offset]))) {
      ++offset;
    }
    if (offset == item.size() || item[offset] != '=') {
      if (key == "rel") parsed.malformedNext = true;
      while (offset < item.size() && item[offset] != ';') ++offset;
      continue;
    }
    ++offset;
    while (offset < item.size() &&
           std::isspace(static_cast<unsigned char>(item[offset]))) {
      ++offset;
    }
    std::string relation;
    bool valueValid = true;
    if (offset < item.size() && item[offset] == '"') {
      ++offset;
      bool closed = false;
      while (offset < item.size()) {
        const char ch = item[offset++];
        if (ch == '"') {
          closed = true;
          break;
        }
        if (ch == '\\' && offset < item.size()) {
          relation.push_back(item[offset++]);
        } else {
          relation.push_back(ch);
        }
      }
      valueValid = closed;
    } else {
      const auto valueStart = offset;
      while (offset < item.size() && item[offset] != ';' &&
             !std::isspace(static_cast<unsigned char>(item[offset]))) {
        ++offset;
      }
      relation = std::string(item.substr(valueStart, offset - valueStart));
    }
    if (key == "rel") {
      const bool isNext = relationContainsNext(relation);
      if (!valueValid && isNext) {
        parsed.malformedNext = true;
      } else if (isNext) {
        if (hasTarget) {
          parsed.next = std::string(
              item.substr(targetOpen + 1, targetClose - targetOpen - 1));
        } else {
          parsed.malformedNext = true;
        }
      }
    }
    while (offset < item.size() && item[offset] != ';') ++offset;
  }
  return parsed;
}

void parseLinkHeader(std::string_view value, HttpResponse &response) {
  std::size_t start = 0;
  bool quoted       = false;
  bool angle        = false;
  bool escaped      = false;
  for (std::size_t index = 0; index <= value.size(); ++index) {
    if (index < value.size()) {
      const char ch = value[index];
      if (quoted && ch == '\\' && !escaped) {
        escaped = true;
        continue;
      }
      if (ch == '"' && !escaped) quoted = !quoted;
      if (!quoted && ch == '<') angle = true;
      if (!quoted && ch == '>') angle = false;
      if (escaped) {
        escaped = false;
        continue;
      }
      if (index != value.size() && (quoted || angle || ch != ',')) continue;
    }
    const auto parsed = parseLinkItem(value.substr(start, index - start));
    if (parsed.next) {
      response.next = *parsed.next;
      return;
    }
    if (parsed.malformedNext) response.malformedNext = true;
    start = index + 1;
  }
}

size_t receiveBody(char *data, size_t size, size_t count, void *context) {
  auto &response = *static_cast<HttpResponse *>(context);
  try {
    const size_t bytes = size * count;
    if (cancelledOrExpired(*response.cancel, response.deadline) ||
        bytes > response.byteLimit -
                    std::min(*response.totalBytes, response.byteLimit)) {
      response.tooLarge =
          !cancelledOrExpired(*response.cancel, response.deadline);
      return 0;
    }
    response.body.append(data, bytes);
    *response.totalBytes += bytes;
    return bytes;
  } catch (...) {
    response.callbackError = true;
    return 0;
  }
}

size_t receiveHeader(char *data, size_t size, size_t count, void *context) {
  auto &response = *static_cast<HttpResponse *>(context);
  try {
    const size_t bytes = size * count;
    if (cancelledOrExpired(*response.cancel, response.deadline) ||
        bytes > response.byteLimit -
                    std::min(*response.totalBytes, response.byteLimit)) {
      response.tooLarge =
          !cancelledOrExpired(*response.cancel, response.deadline);
      return 0;
    }
    *response.totalBytes += bytes;
    std::string line(data, bytes);
    const auto colon = line.find(':');
    if (colon == std::string::npos) return bytes;
    std::string name = line.substr(0, colon);
    std::ranges::transform(name, name.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    const auto value = trim(line.substr(colon + 1));
    if (name == "content-type") response.contentType = value;
    if (name == "etag") response.etag = value;
    if (name == "last-modified") response.lastModified = value;
    if (name == "link") parseLinkHeader(value, response);
    return bytes;
  } catch (...) {
    response.callbackError = true;
    return 0;
  }
}

int curlProgress(void *context, curl_off_t, curl_off_t, curl_off_t,
                 curl_off_t) {
  const auto &response = *static_cast<HttpResponse *>(context);
  return cancelledOrExpired(*response.cancel, response.deadline) ? 1 : 0;
}

CaptureResult captureRest(const Source &source,
                          const std::atomic_bool &cancel) {
  CaptureResult result;
  result.source    = source;
  result.startedAt = timestamp();
  const auto deadline =
      Clock::now() + std::chrono::milliseconds(source.limits.timeoutMs);
  std::size_t totalBytes = 0;
  std::string locator    = source.locator;
  bool partial           = false;
  for (std::size_t pageIndex = 0; pageIndex < source.limits.maxPages;
       ++pageIndex) {
    if (cancelledOrExpired(cancel, deadline)) {
      result.state =
          cancel.load() ? CaptureState::Cancelled : CaptureState::Partial;
      result.error =
          cancel.load() ? "capture cancelled" : "capture time limit reached";
      break;
    }
    CURL *handle = curl_easy_init();
    if (!handle) {
      result.error = "libcurl request allocation failed";
      result.state =
          result.pages.empty() ? CaptureState::Failed : CaptureState::Partial;
      break;
    }
    HttpResponse response{.totalBytes = &totalBytes,
                          .byteLimit  = source.limits.maxBytes,
                          .cancel     = &cancel,
                          .deadline   = deadline};
    curl_slist *headers = nullptr;
    headers =
        curl_slist_append(headers, "Accept: application/json, text/plain, */*");
    curl_easy_setopt(handle, CURLOPT_URL, locator.c_str());
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS,
                     source.limits.timeoutMs);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, source.limits.timeoutMs);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, receiveBody);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, receiveHeader);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &response);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, curlProgress);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &response);
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    const CURLcode transfer = curl_easy_perform(handle);
    long status             = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(handle);
    Page page{.locator      = locator,
              .status       = status,
              .body         = std::move(response.body),
              .contentType  = std::move(response.contentType),
              .etag         = std::move(response.etag),
              .lastModified = std::move(response.lastModified)};
    if (transfer != CURLE_OK || status < 200 || status >= 300) {
      page.state = cancel.load()       ? CaptureState::Cancelled
                   : response.tooLarge ? CaptureState::Partial
                   : transfer == CURLE_OPERATION_TIMEDOUT
                       ? CaptureState::Partial
                   : result.pages.empty() ? CaptureState::Failed
                                          : CaptureState::Partial;
      page.error =
          response.tooLarge        ? "response byte limit reached"
          : response.callbackError ? "response callback could not retain data"
          : transfer != CURLE_OK   ? curl_easy_strerror(transfer)
                                   : "HTTP status " + std::to_string(status);
      result.pages.push_back(std::move(page));
      result.state = cancel.load()       ? CaptureState::Cancelled
                     : response.tooLarge ? CaptureState::Partial
                     : transfer == CURLE_OPERATION_TIMEDOUT
                         ? CaptureState::Partial
                     : result.pages.size() == 1 ? CaptureState::Failed
                                                : CaptureState::Partial;
      result.error = result.pages.back().error;
      partial      = true;
      break;
    }
    if (response.malformedNext) {
      page.next  = "[withheld: refused pagination link]";
      page.state = CaptureState::Partial;
      page.error = "malformed rel=next pagination link";
      result.pages.push_back(std::move(page));
      result.state = CaptureState::Partial;
      result.error = result.pages.back().error;
      break;
    }
    if (response.next.empty()) {
      result.pages.push_back(std::move(page));
      result.state = partial ? CaptureState::Partial : CaptureState::Complete;
      break;
    }
    const auto next = resolvedSameOrigin(locator, response.next);
    if (!next.target) {
      page.next  = "[withheld: refused pagination link]";
      page.state = CaptureState::Partial;
      page.error = next.hasCredentials
                       ? "pagination URL credentials are refused"
                       : "cross-origin or invalid pagination link refused";
      result.pages.push_back(std::move(page));
      result.state = CaptureState::Partial;
      result.error = result.pages.back().error;
      partial      = true;
      break;
    }
    page.next = response.next;
    result.pages.push_back(std::move(page));
    locator = *next.target;
    if (pageIndex + 1 == source.limits.maxPages) {
      result.state    = CaptureState::Partial;
      result.coverage = "page limit reached; more source pages may exist";
      partial         = true;
    }
  }
  if (source.limits.maxPages == 0) {
    result.state    = CaptureState::Partial;
    result.coverage = "page limit is zero";
  }
  if (result.coverage.empty()) {
    result.coverage =
        result.state == CaptureState::Complete
            ? "pagination exhausted; no snapshot consistency claim"
            : "capture did not establish complete coverage";
  }
  result.finishedAt = timestamp();
  return result;
}

CaptureResult capture(const Source &source, const std::atomic_bool &cancel) {
  if (const auto problem = sourceProblem(source)) {
    CaptureResult result;
    result.source = source;
    result.state  = CaptureState::Failed;
    if (source.kind == SourceKind::Rest) {
      result.source.locator = "[withheld: invalid REST source]";
    }
    result.error     = *problem;
    result.startedAt = result.finishedAt = timestamp();
    return result;
  }
  return source.kind == SourceKind::SQLite ? captureSqlite(source, cancel)
                                           : captureRest(source, cancel);
}

} // namespace

std::optional<std::string> sourceProblem(const Source &source) {
  return validateSource(source);
}

struct CaptureJob::Impl {
  mutable std::mutex mutex;
  std::thread worker;
  std::atomic_bool cancelRequested{};
  bool running{};
  std::optional<CaptureResult> completed;
};

CaptureJob::CaptureJob() : impl_(std::make_unique<Impl>()) {}

CaptureJob::~CaptureJob() {
  cancel();
  if (impl_->worker.joinable()) impl_->worker.join();
}

bool CaptureJob::start(Source source) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->running || impl_->completed) return false;
  if (impl_->worker.joinable()) impl_->worker.join();
  impl_->completed.reset();
  impl_->cancelRequested.store(false, std::memory_order_relaxed);
  impl_->running = true;
  try {
    impl_->worker =
        std::thread([state = impl_.get(), source = std::move(source)] {
          CaptureResult result;
          try {
            result = capture(source, state->cancelRequested);
          } catch (const std::exception &error) {
            result.source    = source;
            result.state     = state->cancelRequested.load()
                                   ? CaptureState::Cancelled
                                   : CaptureState::Failed;
            result.error     = error.what();
            result.startedAt = result.finishedAt = timestamp();
          } catch (...) {
            result.source    = source;
            result.state     = CaptureState::Failed;
            result.error     = "unknown capture failure";
            result.startedAt = result.finishedAt = timestamp();
          }
          std::lock_guard completionLock(state->mutex);
          state->completed = std::move(result);
          state->running   = false;
        });
  } catch (...) {
    impl_->running = false;
    return false;
  }
  return true;
}

void CaptureJob::cancel() {
  impl_->cancelRequested.store(true, std::memory_order_relaxed);
}

bool CaptureJob::busy() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->running;
}

std::optional<CaptureResult> CaptureJob::poll() {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->completed) return {};
  auto result = std::move(impl_->completed);
  impl_->completed.reset();
  if (impl_->worker.joinable()) impl_->worker.join();
  return result;
}

std::string stateName(CaptureState state) {
  switch (state) {
  case CaptureState::Complete:
    return "complete";
  case CaptureState::Partial:
    return "partial";
  case CaptureState::Failed:
    return "failed";
  case CaptureState::Cancelled:
    return "cancelled";
  }
  return "failed";
}

std::string storageName(StorageClass storage) {
  switch (storage) {
  case StorageClass::Null:
    return "NULL";
  case StorageClass::Integer:
    return "integer";
  case StorageClass::Real:
    return "real";
  case StorageClass::Text:
    return "text";
  case StorageClass::Blob:
    return "blob";
  }
  return "unknown";
}

std::string displayValue(const CapturedValue &value) {
  if (std::holds_alternative<std::monostate>(value.value)) return "NULL";
  if (const auto *integer = std::get_if<std::int64_t>(&value.value)) {
    return std::to_string(*integer);
  }
  if (const auto *real = std::get_if<double>(&value.value)) {
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<double>::max_digits10)
           << *real;
    return output.str();
  }
  if (const auto *text = std::get_if<std::string>(&value.value)) return *text;
  if (const auto *blob = std::get_if<Binary>(&value.value)) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output             = "0x";
    output.reserve(2 + blob->bytes.size() * 2);
    for (const unsigned char byte : blob->bytes) {
      output.push_back(digits[byte >> 4]);
      output.push_back(digits[byte & 0x0f]);
    }
    return output;
  }
  return {};
}

} // namespace xanadu::evidence
