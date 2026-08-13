/**
 * @file form.cpp
 * @brief Filling in a modal panel with the keyboard.
 *
 * The drawing needs a device and is checked by looking at it. What is checked
 * here is the half that has rules: which field the keyboard is in, where the
 * caret sits inside it, what a required field does to an attempt to accept,
 * and that a character is a character rather than a byte -- because a form is
 * where somebody types their own name, and a name is not ASCII.
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <gleditor/form.hpp>

namespace {

using gleditor::Form;
using gleditor::Key;
using gleditor::KeyMods;

/// The publish dialog's shape, near enough: two that must be filled in and one
/// that need not be.
std::vector<Form::Field> questions() {
  return {
      Form::Field{"Name", "document", "one word", true},
      Form::Field{"Author", {}, "who is publishing this", true},
      Form::Field{"Note", {}, "optional", false},
  };
}

/// A form with those fields up, and somewhere to put the answers.
struct FormTest : testing::Test {
  Form form{"Sans 11"};
  std::vector<Form::Field> answered;
  int accepted{};

  void openIt() {
    form.open("Publish 1", "signed, then sealed", questions(),
              [this](const std::vector<Form::Field> &fields) {
                answered = fields;
                accepted++;
              });
  }
};

} // namespace

TEST_F(FormTest, aClosedFormTakesNoKeys) {
  EXPECT_FALSE(form.grabbing());
  // False rather than swallowed: a form that is not up must not eat the keys
  // that would otherwise reach the document.
  EXPECT_FALSE(form.keyPressed(Key::Return, KeyMods::None));
  form.textTyped("x");
  EXPECT_TRUE(form.current().empty());
}

TEST_F(FormTest, typingGoesIntoTheFocusedField) {
  openIt();
  EXPECT_TRUE(form.grabbing());
  EXPECT_EQ(form.focused(), 0U);

  // The caret starts at the end of the filled-in value, which is where
  // somebody correcting one wants to be.
  form.textTyped("s");
  EXPECT_EQ(form.current()[0].value, "documents");

  form.keyPressed(Key::Tab, KeyMods::None);
  EXPECT_EQ(form.focused(), 1U);
  form.textTyped("Ada");
  EXPECT_EQ(form.current()[1].value, "Ada");
  EXPECT_EQ(form.current()[0].value, "documents") << "the first field moved";
}

TEST_F(FormTest, tabWrapsAndShiftTabGoesBack) {
  openIt();
  form.keyPressed(Key::Tab, KeyMods::None);
  form.keyPressed(Key::Tab, KeyMods::None);
  EXPECT_EQ(form.focused(), 2U);
  form.keyPressed(Key::Tab, KeyMods::None);
  EXPECT_EQ(form.focused(), 0U) << "past the last field is the first";
  form.keyPressed(Key::Tab, KeyMods::Shift);
  EXPECT_EQ(form.focused(), 2U) << "and back again the other way";
  // Up and down do the same thing, since a list of fields is a list.
  form.keyPressed(Key::Down, KeyMods::None);
  EXPECT_EQ(form.focused(), 0U);
  form.keyPressed(Key::Up, KeyMods::None);
  EXPECT_EQ(form.focused(), 2U);
}

TEST_F(FormTest, theCaretMovesAndTextGoesInWhereItIs) {
  openIt();
  form.keyPressed(Key::Home, KeyMods::None);
  form.textTyped("my ");
  EXPECT_EQ(form.current()[0].value, "my document");

  form.keyPressed(Key::End, KeyMods::None);
  form.textTyped("s");
  EXPECT_EQ(form.current()[0].value, "my documents");

  form.keyPressed(Key::Left, KeyMods::None);
  form.keyPressed(Key::Left, KeyMods::None);
  form.textTyped("X");
  EXPECT_EQ(form.current()[0].value, "my documenXts");
}

// A form is where somebody types their own name, and backspace has to remove a
// character rather than a byte: half of a UTF-8 sequence is not a shorter
// string, it is a broken one -- and this one ends up inside something signed.
TEST_F(FormTest, backspaceAndDeleteRemoveWholeCharacters) {
  openIt();
  form.keyPressed(Key::Tab, KeyMods::None);
  form.textTyped("Ada Lovelacé");
  EXPECT_EQ(form.current()[1].value, "Ada Lovelacé");

  form.keyPressed(Key::Backspace, KeyMods::None);
  EXPECT_EQ(form.current()[1].value, "Ada Lovelac")
      << "the two bytes of the accented letter went together";

  form.keyPressed(Key::Home, KeyMods::None);
  form.keyPressed(Key::Delete, KeyMods::None);
  EXPECT_EQ(form.current()[1].value, "da Lovelac");
}

TEST_F(FormTest, acceptingHandsBackTheAnswersAndClosesTheForm) {
  openIt();
  form.keyPressed(Key::Tab, KeyMods::None);
  form.textTyped("Ada Lovelace");
  form.keyPressed(Key::Return, KeyMods::None);

  EXPECT_EQ(accepted, 1);
  EXPECT_FALSE(form.grabbing()) << "the panel comes down when it is answered";
  ASSERT_EQ(answered.size(), 3U);
  EXPECT_EQ(answered[0].value, "document");
  EXPECT_EQ(answered[1].value, "Ada Lovelace");
  EXPECT_EQ(answered[2].value, "");
}

// The fields marked required are the ones a reader would otherwise find empty
// in something signed, so an attempt to accept without them says which.
TEST_F(FormTest, aRequiredFieldStopsItAndSaysWhichOne) {
  openIt();
  form.keyPressed(Key::Return, KeyMods::None);

  EXPECT_EQ(accepted, 0);
  EXPECT_TRUE(form.grabbing()) << "the panel stays up to be finished";
  EXPECT_TRUE(form.complaint().contains("Author")) << form.complaint();

  // And the complaint clears as soon as somebody does something about it,
  // rather than staying up as an accusation.
  form.keyPressed(Key::Tab, KeyMods::None);
  form.textTyped("Ada");
  EXPECT_TRUE(form.complaint().empty());
  form.keyPressed(Key::Return, KeyMods::None);
  EXPECT_EQ(accepted, 1);
}

TEST_F(FormTest, escapeAbandonsItWithoutAnswering) {
  openIt();
  form.textTyped("!");
  form.keyPressed(Key::Escape, KeyMods::None);

  EXPECT_EQ(accepted, 0) << "nothing was published";
  EXPECT_FALSE(form.grabbing());
  // And a second escape is not a second answer: there is nothing up to press.
  EXPECT_FALSE(form.keyPressed(Key::Escape, KeyMods::None));
  EXPECT_EQ(accepted, 0);
}

TEST_F(FormTest, closingItFromOutsideDoesNotAnswerEither) {
  openIt();
  form.close();
  EXPECT_FALSE(form.grabbing());
  EXPECT_EQ(accepted, 0);
}

// vi: set sw=2 sts=2 ts=2 et:
