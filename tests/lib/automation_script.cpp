/**
 * @file automation_script.cpp
 * @brief The automation options, read back in the order they were written.
 *
 * The order is the whole of what this produces. A run that did every click and
 * then all the typing would be a different test from the one the command line
 * asked for -- it would insert everything at the last caret -- so "the order
 * they were written" is the property, and it is worth pinning rather than
 * assuming.
 *
 * It is read from the command line rather than from the parser because a
 * parser collects each option's values into its own list, which is the one
 * thing this needs and the one thing that loses.
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <gleditor/app.hpp>

namespace {

using Kind = AppState::AutomationStep::Kind;

/// Read a command line given as words, as main() would see it.
std::vector<AppState::AutomationStep> scriptOf(std::vector<std::string> words) {
  words.insert(words.begin(), "gleditor");
  std::vector<const char *> argv;
  argv.reserve(words.size());
  for (const auto &word : words) {
    argv.push_back(word.c_str());
  }
  return gleditor::readAutomationScript(static_cast<int>(argv.size()),
                                        argv.data());
}

TEST(AutomationScript, keepsTheOrderTheOptionsWereWrittenIn) {
  const auto script = scriptOf({"--click", "1,2", "--type", "hello", "--click",
                                "3,4", "--type", "there", "--pick", "5,6"});
  ASSERT_EQ(script.size(), 5U);
  EXPECT_EQ(script[0].kind, Kind::Click);
  EXPECT_EQ(script[0].x, 1);
  EXPECT_EQ(script[0].y, 2);
  EXPECT_EQ(script[1].kind, Kind::Type);
  EXPECT_EQ(script[1].text, "hello");
  EXPECT_EQ(script[2].kind, Kind::Click);
  EXPECT_EQ(script[2].x, 3);
  EXPECT_EQ(script[3].kind, Kind::Type);
  EXPECT_EQ(script[3].text, "there");
  EXPECT_EQ(script[4].kind, Kind::Pick);
  EXPECT_EQ(script[4].x, 5);
  EXPECT_EQ(script[4].y, 6);
}

// The same options in the other order are a different script, which is the
// point: reading them by category would make these two the same run.
TEST(AutomationScript, theSameOptionsInAnotherOrderAreAnotherScript) {
  const auto typed = scriptOf({"--click", "1,2", "--type", "a"});
  const auto other = scriptOf({"--type", "a", "--click", "1,2"});
  ASSERT_EQ(typed.size(), 2U);
  ASSERT_EQ(other.size(), 2U);
  EXPECT_EQ(typed[0].kind, Kind::Click);
  EXPECT_EQ(other[0].kind, Kind::Type);
}

TEST(AutomationScript, takesAValueJoinedByAnEqualsSign) {
  // argparse accepts both spellings, so this has to as well, or a command line
  // it accepted would come back with steps missing.
  const auto script = scriptOf({"--click=7,8", "--type=hi", "--select=1,9"});
  ASSERT_EQ(script.size(), 3U);
  EXPECT_EQ(script[0].kind, Kind::Click);
  EXPECT_EQ(script[0].x, 7);
  EXPECT_EQ(script[0].y, 8);
  EXPECT_EQ(script[1].text, "hi");
  EXPECT_EQ(script[2].kind, Kind::Select);
  EXPECT_EQ(script[2].from, 1U);
  EXPECT_EQ(script[2].to, 9U);
}

TEST(AutomationScript, ignoresEverythingThatIsNotAnAutomationOption) {
  const auto script =
      scriptOf({"--font", "Serif 12", "file.txt", "--profile", "--click", "1,2",
                "--screenshot", "out.ppm", "--backend", "vulkan"});
  ASSERT_EQ(script.size(), 1U);
  EXPECT_EQ(script[0].kind, Kind::Click);
  EXPECT_EQ(script[0].x, 1);
}

// A value that looks like an option is still that option's value: "--type
// --profile" types the word, it does not turn profiling on. The parser has
// already decided that, and this must agree with it.
TEST(AutomationScript, takesTheNextWordEvenWhenItLooksLikeAnOption) {
  const auto script = scriptOf({"--type", "--profile", "--click", "1,2"});
  ASSERT_EQ(script.size(), 2U);
  EXPECT_EQ(script[0].kind, Kind::Type);
  EXPECT_EQ(script[0].text, "--profile");
  EXPECT_EQ(script[1].kind, Kind::Click);
}

TEST(AutomationScript, anEmptyCommandLineIsAnEmptyScript) {
  EXPECT_TRUE(scriptOf({}).empty());
  EXPECT_TRUE(gleditor::readAutomationScript(0, nullptr).empty());
  // A trailing option with nothing after it: the parser would have refused
  // this, and reading it must not run off the end of the arguments.
  EXPECT_TRUE(scriptOf({"--click"}).empty());
}

TEST(AutomationScript, typingCanBeEmptyAndIsStillAStep) {
  // Inserting nothing is a no-op the document handles; dropping the step would
  // be a different thing from carrying it out.
  const auto script = scriptOf({"--type", ""});
  ASSERT_EQ(script.size(), 1U);
  EXPECT_EQ(script[0].kind, Kind::Type);
  EXPECT_TRUE(script[0].text.empty());
}

} // namespace
