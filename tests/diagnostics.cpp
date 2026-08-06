#include <gtest/gtest.h>

#include <gleditor/render/diagnostics.hpp>

#include <gmock/gmock.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using render::DiagnosticSeverity;
using render::DiagnosticSink;
using testing::HasSubstr;

TEST(Diagnostics, startsWithNothingRecorded) {
  DiagnosticSink sink;
  EXPECT_FALSE(sink.hasError());
  EXPECT_EQ(sink.distinctCount(), 0U);
  // Nothing recorded means nothing to raise.
  EXPECT_NO_THROW(sink.raiseIfError("test"));
}

TEST(Diagnostics, nonErrorSeveritiesAreNotFatal) {
  DiagnosticSink sink;
  sink.record(DiagnosticSeverity::Info, "just so you know");
  sink.record(DiagnosticSeverity::Warning, "that looks odd");
  EXPECT_FALSE(sink.hasError());
  EXPECT_NO_THROW(sink.raiseIfError("test"));
}

TEST(Diagnostics, anErrorIsRaisedWithItsContextAndText) {
  DiagnosticSink sink;
  sink.record(DiagnosticSeverity::Error, "GL_INVALID_ENUM in glEnable");

  ASSERT_TRUE(sink.hasError());
  try {
    sink.raiseIfError("opengl driver reported an error");
    FAIL() << "expected raiseIfError to throw";
  } catch (const std::runtime_error &err) {
    const std::string what = err.what();
    EXPECT_THAT(what, HasSubstr("opengl driver reported an error"));
    EXPECT_THAT(what, HasSubstr("GL_INVALID_ENUM in glEnable"));
  }
}

// Raising consumes the error, so one bad call does not keep killing every
// subsequent frame.
TEST(Diagnostics, raisingClearsTheError) {
  DiagnosticSink sink;
  sink.record(DiagnosticSeverity::Error, "boom");
  EXPECT_THROW(sink.raiseIfError("test"), std::runtime_error);
  EXPECT_FALSE(sink.hasError());
  EXPECT_NO_THROW(sink.raiseIfError("test"));
}

// The first error is the one that explains the others, so it is the one kept.
TEST(Diagnostics, theFirstErrorIsTheOneReported) {
  DiagnosticSink sink;
  sink.record(DiagnosticSeverity::Error, "first failure");
  sink.record(DiagnosticSeverity::Error, "second failure");
  EXPECT_THAT([&sink] { sink.raiseIfError("test"); },
              testing::ThrowsMessage<std::runtime_error>(
                  HasSubstr("first failure")));
}

// Drivers repeat the same complaint every draw call; without deduplication the
// log would be unreadable and the sink would grow without bound.
TEST(Diagnostics, repeatedMessagesAreRecordedOnce) {
  DiagnosticSink sink;
  for (int i = 0; i < 100; i++) {
    sink.record(DiagnosticSeverity::Warning, "same complaint");
  }
  EXPECT_EQ(sink.distinctCount(), 1U);
}

TEST(Diagnostics, distinctMessagesAreEachKept) {
  DiagnosticSink sink;
  sink.record(DiagnosticSeverity::Warning, "one");
  sink.record(DiagnosticSeverity::Warning, "two");
  EXPECT_EQ(sink.distinctCount(), 2U);
}

// A driver emitting endlessly varying text must not grow the sink forever.
TEST(Diagnostics, rememberedMessagesAreBounded) {
  DiagnosticSink sink;
  for (int i = 0; i < 2000; i++) {
    sink.record(DiagnosticSeverity::Warning, "message " + std::to_string(i));
  }
  EXPECT_LE(sink.distinctCount(), 256U);
}

// Past the dedupe cap, an error must still be recorded: dropping it would mean
// a noisy driver could hide a real fault.
TEST(Diagnostics, errorsAreStillCaughtAfterTheDedupeCapIsReached) {
  DiagnosticSink sink;
  for (int i = 0; i < 2000; i++) {
    sink.record(DiagnosticSeverity::Warning, "noise " + std::to_string(i));
  }
  sink.record(DiagnosticSeverity::Error, "the real problem");
  EXPECT_TRUE(sink.hasError());
  EXPECT_THAT([&sink] { sink.raiseIfError("test"); },
              testing::ThrowsMessage<std::runtime_error>(
                  HasSubstr("the real problem")));
}

TEST(Diagnostics, clearForgetsEverything) {
  DiagnosticSink sink;
  sink.record(DiagnosticSeverity::Error, "boom");
  sink.clear();
  EXPECT_FALSE(sink.hasError());
  EXPECT_EQ(sink.distinctCount(), 0U);
}

// Vulkan may call its messenger from any thread the driver uses, so recording
// has to be safe under concurrency.
TEST(Diagnostics, concurrentRecordingIsSafe) {
  DiagnosticSink sink;
  std::vector<std::jthread> threads;
  threads.reserve(4);
  for (int t = 0; t < 4; t++) {
    threads.emplace_back([&sink, t] {
      for (int i = 0; i < 200; i++) {
        sink.record(DiagnosticSeverity::Warning,
                    std::to_string(t) + ":" + std::to_string(i));
      }
      sink.record(DiagnosticSeverity::Error, "thread " + std::to_string(t));
    });
  }
  threads.clear(); // joins

  EXPECT_TRUE(sink.hasError());
  EXPECT_LE(sink.distinctCount(), 256U);
}

// vi: set sw=2 sts=2 ts=2 et:
