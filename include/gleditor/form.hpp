/**
 * @file form.hpp
 * @brief A panel of labelled fields, filled in with the keyboard.
 *
 * There is no widget toolkit here and this is not the beginning of one. It is
 * the smallest thing that lets a program ask a question with more than one
 * answer in it -- which is what anything irreversible needs before it happens,
 * and publishing is irreversible in the way that matters: a document goes out
 * under somebody's name, signed, and cannot be recalled from whoever has it.
 *
 * Everything is drawn with a Canvas, which is to say with rectangles and text,
 * and driven with the keys a form has always been driven with: tab between
 * fields, type into one, enter to accept, escape to abandon. No mouse, because
 * a pointer would need hit testing and focus and a caret that follows a click,
 * and none of that would make the question any clearer.
 *
 * A form takes the keyboard while it is up -- see ModalInput -- so the text
 * being typed into it cannot land in the document behind it.
 */
#ifndef GLEDITOR_FORM_H
#define GLEDITOR_FORM_H

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <gleditor/frame_contributor.hpp>
#include <gleditor/modal_input.hpp>

namespace gleditor {

class Canvas;

/**
 * @class Form
 * @brief A modal panel of text fields.
 *
 * Registered as a frame contributor, so it draws itself, and as the
 * application's modal, so it receives the keyboard while it is open. Both
 * happen on different threads -- drawing on the render thread, keys on the
 * event thread -- so everything it holds is behind one mutex, held briefly and
 * never across a call back into the program.
 */
class Form : public FrameContributor, public ModalInput {
public:
  /// What kind of thing is being asked for.
  enum class Kind : std::uint8_t {
    /// Text, typed in.
    Text,
    /// Text shown as asterisks unless something reveals it. For a passphrase,
    /// which is the one thing here that somebody may be typing with a person
    /// behind them.
    Secret,
    /// One of a list. Opened with space, moved through with up and down,
    /// settled with enter -- which is a drop-down, driven the way a keyboard
    /// drives one.
    Choice,
    /// On or off, drawn as a button and flipped with space.
    Toggle,
  };

  /// One thing being asked for.
  struct Field {
    std::string label;
    std::string value;
    /// Shown greyed when the value is empty: what this would be if left alone,
    /// or what shape of answer is wanted.
    std::string hint;
    /// Whether the form will refuse to be accepted while this is empty. Never
    /// true of a Toggle, which is always one thing or the other.
    bool required{};
    Kind kind{Kind::Text};

    /// Choice: what there is to pick from, as somebody reads it.
    std::vector<std::string> options;
    /**
     * @brief Choice: what each option means, when that differs from its label.
     *
     * A key is shown as "Ada Lovelace <ada@example.org> (8844BE88)" and meant
     * as a fingerprint. Same length as @ref options, or empty when the two are
     * the same.
     */
    std::vector<std::string> optionValues;
    /// Choice: which one is picked.
    std::size_t chosen{};

    /// Toggle: whether it is on.
    bool on{};
    /**
     * @brief Toggle: whether flipping this shows the Secret fields.
     *
     * The button beside a passphrase. Kept as a property of the toggle rather
     * than wired up by the caller, so that a form with a passphrase in it
     * cannot be given a reveal button that reveals nothing.
     */
    bool revealsSecrets{};

    /// What this field is worth as an answer: the chosen option's value for a
    /// Choice, "on" or nothing for a Toggle, the text for anything else.
    [[nodiscard]] std::string answer() const;
  };

  /// What to do with the answers. Called on the event thread, with the form
  /// already closed, so an implementation may open another.
  using Accepted = std::function<void(const std::vector<Field> &fields)>;

  /**
   * @param aFontName Pango description the panel is drawn in. Deliberately not
   *        the document's font: this is chrome, and has to stay legible
   *        whatever the document is being read at.
   */
  explicit Form(std::string aFontName);
  ~Form() override;

  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &documentPipeline) override;
  void drawFrame(FrameContext &ctx) override;

  // -- gleditor::ModalInput ---------------------------------------------------
  [[nodiscard]] bool grabbing() const override;
  bool keyPressed(Key key, KeyMods mods) override;
  void textTyped(const std::string &utf8) override;

  /**
   * @brief Put the form up.
   *
   * @param aTitle A line at the top saying what is being asked.
   * @param aNote A line under it, for whatever the person needs to know before
   *        answering -- where this will be written, what cannot be undone.
   * @param aFields The questions, in the order they are asked.
   * @param onAccept Called with the answers when the form is accepted. Not
   *        called at all if it is abandoned.
   */
  void open(std::string aTitle, std::string aNote, std::vector<Field> aFields,
            Accepted onAccept);

  /// Take it down without accepting it.
  void close();

  [[nodiscard]] bool isOpen() const { return grabbing(); }

  /// What the fields hold now, for a caller that wants to look without
  /// waiting to be called back. Mostly for tests.
  [[nodiscard]] std::vector<Field> current() const;

  /// Which field the keyboard is in, and where in it the caret sits.
  [[nodiscard]] std::size_t focused() const;

  /// A message shown under the fields, in place of the note: what stopped the
  /// last attempt to accept.
  [[nodiscard]] std::string complaint() const;

  /// Whether a drop-down is open, which is what makes escape close the list
  /// rather than the form.
  [[nodiscard]] bool listOpen() const;

  /// Whether the Secret fields are currently readable.
  [[nodiscard]] bool secretsShown() const;

private:
  /// Where the caret may be put within the focused field's value: byte
  /// offsets that fall on character boundaries, since a UTF-8 sequence is not
  /// separately deletable.
  void moveCaret(int by);
  /// The field the caret is in, under the lock.
  [[nodiscard]] Field &field();
  /// Whether every required field has something in it, and which does not.
  [[nodiscard]] bool complete(std::string &missing) const;
  /// Move the highlight within an open drop-down, or the focus between fields.
  void step(int by);

  mutable std::mutex guard;
  std::string fontName;
  std::string title;
  std::string note;
  std::string trouble;
  std::vector<Field> fields;
  Accepted accepted;
  std::size_t focus{};
  /// Caret position within the focused field, as a byte offset.
  std::size_t caret{};
  bool open_{};
  /// Whether the focused Choice has its list down, and which option the
  /// highlight is on while it is.
  bool expanded{};
  std::size_t highlight{};
  /// Bumped whenever anything visible changes, so drawing knows to rebuild.
  std::uint64_t revision{1};

  std::unique_ptr<Canvas> canvas;
  std::uint64_t builtFor{};
  int builtWidth{};
  int builtHeight{};
};

} // namespace gleditor

#endif // GLEDITOR_FORM_H
// vi: set sw=2 sts=2 ts=2 et:
