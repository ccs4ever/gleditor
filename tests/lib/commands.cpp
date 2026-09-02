/**
 * @file commands.cpp
 * @brief The key table a program fills in and the event loop dispatches
 *        against.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <gleditor/app.hpp>
#include <gleditor/sdl_compat.hpp>

namespace {

using gleditor::CommandTable;
using gleditor::Mod;

/// Stand-ins for scancodes. The table never interprets them, so any distinct
/// values will do and using real ones would tie the test to an SDL version.
constexpr int keyA = 4;
constexpr int keyB = 5;

TEST(CommandTableTest, anUnboundKeyDoesNothing) {
  const CommandTable table;
  EXPECT_FALSE(table.dispatch(keyA, Mod::None));
}

TEST(CommandTableTest, aBoundKeyRuns) {
  CommandTable table;
  int ran = 0;
  table.bind(keyA, "a", "does a", [&ran] { ran++; });

  EXPECT_TRUE(table.dispatch(keyA, Mod::None));
  EXPECT_EQ(ran, 1);
}

TEST(CommandTableTest, otherKeysAreLeftAlone) {
  CommandTable table;
  int ran = 0;
  table.bind(keyA, "a", "does a", [&ran] { ran++; });

  EXPECT_FALSE(table.dispatch(keyB, Mod::None));
  EXPECT_EQ(ran, 0);
}

TEST(CommandTableTest, modifiersMustMatchExactly) {
  CommandTable table;
  std::vector<std::string> ran;
  table.bind(keyA, "plain", "", [&ran] { ran.emplace_back("plain"); });
  table.bind(keyA, Mod::Shift, "shifted", "",
             [&ran] { ran.emplace_back("shifted"); });

  EXPECT_TRUE(table.dispatch(keyA, Mod::None));
  EXPECT_TRUE(table.dispatch(keyA, Mod::Shift));
  // The whole point of matching exactly: a binding on the bare key must not
  // fire when a modifier it did not ask for is held, or it would be
  // impossible to bind the two to different things.
  EXPECT_FALSE(table.dispatch(keyA, Mod::Ctrl));

  EXPECT_THAT(ran, testing::ElementsAre("plain", "shifted"));
}

TEST(CommandTableTest, combinedModifiersAreDistinct) {
  CommandTable table;
  int ran = 0;
  table.bind(keyA, Mod::Ctrl | Mod::Shift, "both", "", [&ran] { ran++; });

  EXPECT_FALSE(table.dispatch(keyA, Mod::Ctrl));
  EXPECT_FALSE(table.dispatch(keyA, Mod::Shift));
  EXPECT_TRUE(table.dispatch(keyA, Mod::Ctrl | Mod::Shift));
  EXPECT_EQ(ran, 1);
}

TEST(CommandTableTest, theFirstMatchingBindingWins) {
  CommandTable table;
  std::string ran;
  table.bind(keyA, "first", "", [&ran] { ran = "first"; });
  table.bind(keyA, "second", "", [&ran] { ran = "second"; });

  EXPECT_TRUE(table.dispatch(keyA, Mod::None));
  EXPECT_EQ(ran, "first");
}

TEST(CommandTableTest, aBindingWithNothingToRunIsNotDispatched) {
  CommandTable table;
  table.bind(keyA, "empty", "", nullptr);
  EXPECT_FALSE(table.dispatch(keyA, Mod::None));
}

TEST(CommandTableTest, helpNamesEveryBinding) {
  CommandTable table;
  table.bind(keyA, "quit", "close the program", [] {});
  table.bind(keyB, "open", "open a document", [] {});

  const auto help = table.helpText();
  EXPECT_THAT(help, testing::HasSubstr("quit"));
  EXPECT_THAT(help, testing::HasSubstr("close the program"));
  EXPECT_THAT(help, testing::HasSubstr("open"));
  EXPECT_THAT(help, testing::HasSubstr("open a document"));
}

TEST(CommandTableTest, everyBindingIsListed) {
  CommandTable table;
  table.bind(keyA, "one", "", [] {});
  table.bind(keyB, Mod::Alt, "two", "", [] {});
  EXPECT_EQ(table.all().size(), 2U);
  EXPECT_EQ(table.all()[1].mods, Mod::Alt);
}

TEST(MouseWheelTest, WheelHelpersExtractCoordinates) {
  SDL_Event evt{};
  evt.type = SDL_EVENT_MOUSE_WHEEL;
#if GLEDITOR_SDL_MAJOR == 3
  evt.wheel.x = 2.0F;
  evt.wheel.y = -3.0F;
#else
  evt.wheel.x = 2;
  evt.wheel.y = -3;
#endif
  evt.wheel.direction = SDL_MOUSEWHEEL_NORMAL;

  EXPECT_FLOAT_EQ(sdl::wheelX(evt), 2.0F);
  EXPECT_FLOAT_EQ(sdl::wheelY(evt), -3.0F);
  EXPECT_FALSE(sdl::wheelFlipped(evt));

  evt.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
  EXPECT_TRUE(sdl::wheelFlipped(evt));
}

} // namespace
