#include <gleditor/logging.hpp>
#include <gtest/gtest.h>

#include <spdlog/sinks/ostream_sink.h>

#include <memory>
#include <sstream>

namespace {

std::shared_ptr<spdlog::sinks::ostream_sink_mt>
captureInto(const char *name, std::ostringstream &out) {
  auto logger = gleditor::logging::category(name);
  auto sink   = std::make_shared<spdlog::sinks::ostream_sink_mt>(out);
  sink->set_pattern("%n %v");
  logger->sinks().push_back(sink);
  logger->set_level(spdlog::level::debug);
  return sink;
}

} // namespace

// Two categories logging the same argument types share one instantiation of
// the logging template; each message must still reach its own category.
TEST(LoggingTest, EachCallReachesTheCategoryItNames) {
  std::ostringstream first;
  std::ostringstream second;
  const auto firstSink  = captureInto("test.logging.first", first);
  const auto secondSink = captureInto("test.logging.second", second);

  GLEDITOR_LOG_DEBUG("test.logging.first", "value {}", 1);
  GLEDITOR_LOG_DEBUG("test.logging.second", "value {}", 2);

  EXPECT_NE(first.str().find("test.logging.first value 1"), std::string::npos)
      << first.str();
  EXPECT_EQ(first.str().find("value 2"), std::string::npos) << first.str();
  EXPECT_NE(second.str().find("test.logging.second value 2"), std::string::npos)
      << second.str();
}
