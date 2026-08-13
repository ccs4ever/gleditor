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

#include <optional>
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

// Where typing lands is handed to SDL and by SDL to the platform: it is what
// puts an input method's candidate window beside the field being typed into,
// and what stops a phone's keyboard covering it. Which means it must be a
// place that exists. It is worked out while drawing -- that is where the
// panel's geometry is decided -- so there is nothing to report until a frame
// has been drawn, and nothing to report once the form is down.
TEST_F(FormTest, nowhereIsBeingTypedUntilThereIsSomewhereToType) {
  EXPECT_FALSE(form.textArea().has_value())
      << "a closed form is not typed into";
  openIt();
  EXPECT_FALSE(form.textArea().has_value())
      << "opened but not yet drawn: nobody knows where the field will be";
  form.keyPressed(Key::Escape, KeyMods::None);
  EXPECT_FALSE(form.textArea().has_value());
}

// The event loop tells the platform where typing lands only when it moves,
// which is a comparison rather than a flag -- so the comparison has to be one.
TEST(InputAreaTest, twoOfThemAreTheSameWhenTheyAreInTheSamePlace) {
  using gleditor::InputArea;
  const InputArea here{40, 120, 300, 24};
  EXPECT_EQ(here, (InputArea{40, 120, 300, 24}));
  EXPECT_NE(here, (InputArea{40, 152, 300, 24})) << "the next field down";
  // And an area at all differs from none, which is how a button that raises no
  // keyboard is told apart from a field that does.
  EXPECT_NE(std::optional<InputArea>{here}, std::optional<InputArea>{});
}

// -- the kinds beyond plain text ---------------------------------------------

namespace {

using Kind = Form::Kind;

/// A form with the three kinds in it: a drop-down of keys, a passphrase, and
/// the button that reveals it.
struct ChoiceFormTest : testing::Test {
  Form form{"Sans 11"};
  std::vector<Form::Field> answered;
  int accepted{};

  void openIt() {
    Form::Field keys{"Signing key", {}, "nothing to sign with", false,
                     Kind::Choice};
    keys.options      = {"Ada Lovelace  (8844BE88)", "Grace Hopper  (3E6A9DB7)"};
    keys.optionValues = {std::string(32, 'a') + "8844BE88",
                         std::string(32, 'b') + "3E6A9DB7"};

    Form::Field reveal{"", {}, {}, false, Kind::Toggle};
    reveal.revealsSecrets = true;

    form.open("Publish 1", "signed, then sealed",
              {Form::Field{"Title", "essay", "what it is called", true},
               std::move(keys),
               Form::Field{"Passphrase", {}, "only if needed", false,
                           Kind::Secret},
               std::move(reveal)},
              [this](const std::vector<Form::Field> &fields) {
                answered = fields;
                accepted++;
              });
  }
};

} // namespace

// A drop-down is worth having because a fingerprint is not something anybody
// remembers -- so what it hands back is the fingerprint, and what it shows is
// the name.
TEST_F(ChoiceFormTest, aChoiceAnswersWithWhatItMeansRatherThanWhatItShows) {
  openIt();
  const auto keys = form.current()[1];
  EXPECT_EQ(keys.answer(), keys.optionValues[0]);
  EXPECT_TRUE(keys.options[0].contains("Ada"));
}

TEST_F(ChoiceFormTest, theListOpensMovesAndSettles) {
  openIt();
  form.keyPressed(Key::Tab, KeyMods::None);
  EXPECT_EQ(form.focused(), 1U);
  EXPECT_FALSE(form.listOpen());

  // Space opens it, as it opens a list anywhere else.
  form.textTyped(" ");
  EXPECT_TRUE(form.listOpen());

  // While it is down, up and down move within it rather than between fields.
  form.keyPressed(Key::Down, KeyMods::None);
  EXPECT_EQ(form.focused(), 1U) << "still on the same field";
  form.keyPressed(Key::Return, KeyMods::None);
  EXPECT_FALSE(form.listOpen());
  EXPECT_EQ(form.current()[1].chosen, 1U);
  EXPECT_EQ(accepted, 0) << "enter settled the list, it did not publish";
}

TEST_F(ChoiceFormTest, escapeClosesTheListBeforeItClosesTheForm) {
  openIt();
  form.keyPressed(Key::Tab, KeyMods::None);
  form.textTyped(" ");
  form.keyPressed(Key::Down, KeyMods::None);

  form.keyPressed(Key::Escape, KeyMods::None);
  EXPECT_FALSE(form.listOpen());
  EXPECT_TRUE(form.grabbing()) << "the form is still up";
  EXPECT_EQ(form.current()[1].chosen, 0U) << "and nothing was picked";

  // The second escape is the one that abandons it.
  form.keyPressed(Key::Escape, KeyMods::None);
  EXPECT_FALSE(form.grabbing());
  EXPECT_EQ(accepted, 0);
}

TEST_F(ChoiceFormTest, leftAndRightGoThroughTheOptionsWithoutOpeningIt) {
  openIt();
  form.keyPressed(Key::Tab, KeyMods::None);
  form.keyPressed(Key::Right, KeyMods::None);
  EXPECT_FALSE(form.listOpen());
  EXPECT_EQ(form.current()[1].chosen, 1U);
  form.keyPressed(Key::Right, KeyMods::None);
  EXPECT_EQ(form.current()[1].chosen, 0U) << "past the last is the first";
}

// The one field somebody may be typing with a person behind them.
TEST_F(ChoiceFormTest, aSecretIsHeldButNotShownUntilTheButtonIsPressed) {
  openIt();
  form.keyPressed(Key::Tab, KeyMods::None);
  form.keyPressed(Key::Tab, KeyMods::None);
  EXPECT_EQ(form.focused(), 2U);
  form.textTyped("correct horse");

  // Held exactly as typed -- it has to be, since it is what gpg is given.
  EXPECT_EQ(form.current()[2].value, "correct horse");
  EXPECT_FALSE(form.secretsShown());

  form.keyPressed(Key::Tab, KeyMods::None);
  EXPECT_EQ(form.focused(), 3U) << "the button beside it";
  form.textTyped(" ");
  EXPECT_TRUE(form.secretsShown());
  EXPECT_TRUE(form.current()[3].on);

  form.textTyped(" ");
  EXPECT_FALSE(form.secretsShown()) << "and pressing it again hides it";
}

TEST_F(ChoiceFormTest, aButtonIsPressedRatherThanTypedInto) {
  openIt();
  form.keyPressed(Key::Tab, KeyMods::None);
  form.keyPressed(Key::Tab, KeyMods::None);
  form.keyPressed(Key::Tab, KeyMods::None);

  // Text goes nowhere: there is nothing to type into a button.
  form.textTyped("x");
  EXPECT_TRUE(form.current()[3].value.empty());
  EXPECT_FALSE(form.current()[3].on);

  // Enter presses it rather than accepting the form, which is what enter does
  // to a focused button everywhere else.
  form.keyPressed(Key::Return, KeyMods::None);
  EXPECT_TRUE(form.current()[3].on);
  EXPECT_EQ(accepted, 0);
}

TEST_F(ChoiceFormTest, everythingComesBackTogetherWhenItIsAccepted) {
  openIt();
  form.keyPressed(Key::Tab, KeyMods::None);
  form.keyPressed(Key::Right, KeyMods::None);
  form.keyPressed(Key::Tab, KeyMods::None);
  form.textTyped("correct horse");
  form.keyPressed(Key::Return, KeyMods::None);

  ASSERT_EQ(accepted, 1);
  ASSERT_EQ(answered.size(), 4U);
  EXPECT_EQ(answered[0].answer(), "essay");
  EXPECT_EQ(answered[1].answer(), answered[1].optionValues[1])
      << "the fingerprint of the key that was picked";
  EXPECT_EQ(answered[2].answer(), "correct horse");
  EXPECT_EQ(answered[3].answer(), "");
}

// A required drop-down with nothing in it is an empty answer, which is what
// stops a publication going out signed by nothing at all.
TEST_F(ChoiceFormTest, anEmptyChoiceAnswersWithNothing) {
  Form::Field keys{"Signing key", {}, "nothing to sign with", true,
                   Kind::Choice};
  form.open("Publish", "note", {keys}, [](const std::vector<Form::Field> &) {});
  EXPECT_EQ(form.current()[0].answer(), "");
  form.keyPressed(Key::Return, KeyMods::None);
  EXPECT_TRUE(form.grabbing());
  EXPECT_TRUE(form.complaint().contains("Signing key"));
}

// vi: set sw=2 sts=2 ts=2 et:
