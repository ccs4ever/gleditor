/**
 * @file form.cpp
 * @brief Implementation of the modal field panel.
 */
#include <gleditor/form.hpp> // IWYU pragma: associated

#include <algorithm>
#include <utility>

#include <glm/ext/matrix_clip_space.hpp>

#include <gleditor/canvas.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/utf8.hpp>

namespace gleditor {

namespace {

/// Pixel geometry of the panel. One place, so nothing drifts out of line with
/// anything else.
constexpr float panelWidth   = 620.0F;
constexpr float padding      = 18.0F;
constexpr float rowHeight    = 30.0F;
constexpr float labelWidth   = 150.0F;
constexpr float boxHeight    = 24.0F;
constexpr float lineGap      = 8.0F;
constexpr float caretWidth   = 2.0F;

/// Colours. A panel over the documents has to be plainly in front of them
/// rather than blended into them, so it is opaque and dark and the sheet
/// behind it dims everything else.
constexpr auto dimming    = 0x00000090U;
constexpr auto panelInk   = 0xE8EAF0FFU;
constexpr auto panelBack  = 0x22252EFFU;
constexpr auto boxBack    = 0x171A21FFU;
constexpr auto boxFocused = 0x2C3446FFU;
constexpr auto hintInk    = 0x7C8494FFU;
constexpr auto titleInk   = 0xFFFFFFFFU;
constexpr auto requiredInk = 0xFFC46BFFU;
constexpr auto troubleInk = 0xFF9A8CFFU;

/// A colour as the canvas wants it, given RGBA8.
std::uint32_t ink(const std::uint32_t rgba) { return rgba; }

/// Characters in @p text, counting lead bytes: what a masked field shows one
/// asterisk for. Bytes would show three for every accented letter, which says
/// something about the passphrase that is nobody's business.
std::size_t charactersIn(const std::string_view text) {
  return static_cast<std::size_t>(std::ranges::count_if(text, [](const char chr) {
    return 0x80 != (static_cast<unsigned char>(chr) & 0xC0);
  }));
}

} // namespace

std::string Form::Field::answer() const {
  switch (kind) {
  case Kind::Choice: {
    if (options.empty()) {
      return value;
    }
    const auto which = std::min(chosen, options.size() - 1);
    // What the option means, when that differs from how it reads.
    return optionValues.size() == options.size() ? optionValues[which]
                                                 : options[which];
  }
  case Kind::Toggle:
    return on ? "on" : std::string{};
  case Kind::Text:
  case Kind::Secret:
    break;
  }
  return value;
}

Form::Form(std::string aFontName) : fontName(std::move(aFontName)) {}
Form::~Form() = default;

void Form::deviceReady(render::RenderDevice &device,
                       const render::PipelineDesc &documentPipeline) {
  canvas = std::make_unique<Canvas>(&device, fontName);
  // Depth testing off, like the notification overlay: this is on top because
  // it is drawn last, and a modal that a document could poke through would be
  // a modal in name only.
  canvas->createPipeline(documentPipeline, false);
}

bool Form::grabbing() const {
  const std::lock_guard locker(guard);
  return open_;
}

std::vector<Form::Field> Form::current() const {
  const std::lock_guard locker(guard);
  return fields;
}

std::size_t Form::focused() const {
  const std::lock_guard locker(guard);
  return focus;
}

std::string Form::complaint() const {
  const std::lock_guard locker(guard);
  return trouble;
}

std::optional<InputArea> Form::textArea() const {
  const std::lock_guard locker(guard);
  return typingAt;
}

void Form::open(std::string aTitle, std::string aNote,
                std::vector<Field> aFields, Accepted onAccept) {
  const std::lock_guard locker(guard);
  title    = std::move(aTitle);
  note     = std::move(aNote);
  fields   = std::move(aFields);
  accepted = std::move(onAccept);
  trouble.clear();
  focus    = 0;
  expanded = false;
  // At the end of the first field, which is where somebody correcting a
  // filled-in value wants to be.
  caret = fields.empty() ? 0 : fields.front().value.size();
  open_ = true;
  // Where the last form's focused field was is not where this one's is, and
  // until this one has been drawn nobody knows where that will be. Saying
  // nothing is right: the platform keeps whatever it was told last, which is
  // better than being pointed somewhere this form is not.
  typingAt.reset();
  revision++;
}

void Form::close() {
  const std::lock_guard locker(guard);
  open_    = false;
  accepted = nullptr;
  // Nothing is being typed into any more, so there is nowhere for an input
  // method to put itself.
  typingAt.reset();
  revision++;
}

Form::Field &Form::field() {
  focus = std::min(focus, fields.empty() ? 0 : fields.size() - 1);
  return fields[focus];
}

bool Form::complete(std::string &missing) const {
  for (const auto &one : fields) {
    if (one.required && one.answer().empty()) {
      missing = one.label;
      return false;
    }
  }
  return true;
}

bool Form::listOpen() const {
  const std::lock_guard locker(guard);
  return open_ && expanded;
}

bool Form::secretsShown() const {
  const std::lock_guard locker(guard);
  return std::ranges::any_of(fields, [](const Field &one) {
    return Kind::Toggle == one.kind && one.revealsSecrets && one.on;
  });
}

void Form::step(const int by) {
  if (fields.empty()) {
    return;
  }
  if (expanded) {
    // Inside the list rather than between the fields: what is being moved is
    // the highlight over the options.
    const auto count = fields[focus].options.size();
    if (0 == count) {
      return;
    }
    highlight = by < 0 ? (0 == highlight ? count - 1 : highlight - 1)
                       : (highlight + 1) % count;
    return;
  }
  focus = by < 0 ? (0 == focus ? fields.size() - 1 : focus - 1)
                 : (focus + 1) % fields.size();
  caret = fields[focus].value.size();
}

void Form::moveCaret(const int by) {
  const auto &value = fields[focus].value;
  if (by < 0) {
    caret = caret == 0 ? 0
                       : alignToCharacterStart(value,
                                               static_cast<std::uint32_t>(
                                                   caret - 1));
    return;
  }
  caret = caret >= value.size()
              ? value.size()
              : alignToCharacterEnd(value,
                                    static_cast<std::uint32_t>(caret + 1));
}

bool Form::keyPressed(const Key key, const KeyMods mods) {
  Accepted toCall;
  std::vector<Field> answers;
  {
    const std::lock_guard locker(guard);
    if (!open_) {
      return false;
    }
    revision++;

    switch (key) {
    case Key::Escape:
      if (expanded) {
        // The list closes and the form stays: escape undoes the smaller thing
        // first, which is what every list that opens like this does.
        expanded = false;
        return true;
      }
      open_    = false;
      accepted = nullptr;
      typingAt.reset();
      return true;

    case Key::Return: {
      if (expanded) {
        fields[focus].chosen = highlight;
        expanded             = false;
        trouble.clear();
        return true;
      }
      auto &here = field();
      // Enter on a closed drop-down opens it, so that a key can be picked
      // without knowing that space is what opens one. A list with nothing in
      // it is not opened: an empty panel would say less than the hint already
      // showing, and the field is answered as empty either way.
      if (Kind::Choice == here.kind && !here.options.empty()) {
        expanded  = true;
        highlight = here.chosen;
        return true;
      }
      if (Kind::Toggle == here.kind) {
        here.on = !here.on;
        return true;
      }
      std::string missing;
      if (!complete(missing)) {
        // Refused rather than published half-filled: the fields marked
        // required are the ones a reader would otherwise find empty in
        // something signed.
        trouble = missing + " is needed before this can go out";
        return true;
      }
      // The form comes down first, and the callback runs outside the lock, so
      // that what it does -- which may be to open another form -- does not
      // deadlock against this one.
      open_   = false;
      toCall  = std::exchange(accepted, nullptr);
      answers = fields;
      typingAt.reset();
      break;
    }

    case Key::Tab:
      // Tab leaves a field, so it settles an open list on the way out rather
      // than abandoning what the highlight was on.
      if (expanded) {
        fields[focus].chosen = highlight;
        expanded             = false;
      }
      step(held(mods, KeyMods::Shift) ? -1 : 1);
      return true;

    case Key::Up:
      step(-1);
      return true;
    case Key::Down:
      step(1);
      return true;

    case Key::Left:
    case Key::Right: {
      auto &here = field();
      if (Kind::Choice == here.kind && !here.options.empty()) {
        // Through the options without opening the list, for somebody who knows
        // what is in it.
        const auto count = here.options.size();
        here.chosen = Key::Left == key ? (0 == here.chosen ? count - 1
                                                           : here.chosen - 1)
                                       : (here.chosen + 1) % count;
        return true;
      }
      if (Kind::Toggle == here.kind) {
        here.on = !here.on;
        return true;
      }
      moveCaret(Key::Left == key ? -1 : 1);
      return true;
    }
    case Key::Home:
      caret = 0;
      return true;
    case Key::End:
      caret = field().value.size();
      return true;

    case Key::Backspace: {
      auto &value = field();
      if (0 == caret || value.value.empty()) {
        return true;
      }
      // A whole character, not a byte: half of a UTF-8 sequence is not a
      // shorter string, it is a broken one.
      const auto from =
          alignToCharacterStart(value.value, static_cast<std::uint32_t>(caret - 1));
      value.value.erase(from, caret - from);
      caret = from;
      trouble.clear();
      return true;
    }
    case Key::Delete: {
      auto &value = field();
      if (caret >= value.value.size()) {
        return true;
      }
      const auto to = alignToCharacterEnd(
          value.value, static_cast<std::uint32_t>(caret + 1));
      value.value.erase(caret, to - caret);
      trouble.clear();
      return true;
    }
    }
  }

  if (toCall) {
    toCall(answers);
  }
  return true;
}

void Form::textTyped(const std::string &utf8) {
  const std::lock_guard locker(guard);
  if (!open_ || fields.empty()) {
    return;
  }
  auto &here = field();

  // A field with nothing to type into takes space as "activate this", which is
  // what space does to a button and to a list everywhere else. A text field
  // takes it as a space, because that is what it is.
  if (Kind::Choice == here.kind || Kind::Toggle == here.kind) {
    if (" " == utf8) {
      if (Kind::Toggle == here.kind) {
        here.on = !here.on;
      } else if (!here.options.empty()) {
        expanded  = !expanded;
        highlight = here.chosen;
      }
      revision++;
    }
    return;
  }

  auto &value = here;
  caret       = std::min(caret, value.value.size());
  value.value.insert(caret, utf8);
  caret += utf8.size();
  trouble.clear();
  revision++;
}

void Form::drawFrame(FrameContext &ctx) {
  if (nullptr == canvas) {
    return;
  }

  // Copied under the lock and drawn outside it: laying text out rasterises
  // glyphs, which is not something to do while the event thread is waiting to
  // record a keystroke.
  std::string heading;
  std::string subheading;
  std::vector<Field> shown;
  std::size_t where     = 0;
  std::size_t at        = 0;
  std::uint64_t seen    = 0;
  bool listDown         = false;
  std::size_t lit       = 0;
  bool reveal           = false;
  {
    const std::lock_guard locker(guard);
    if (!open_) {
      return;
    }
    heading    = title;
    subheading = trouble.empty() ? note : trouble;
    shown      = fields;
    where      = focus;
    at         = caret;
    seen       = revision;
    listDown   = expanded;
    lit        = highlight;
    reveal     = std::ranges::any_of(fields, [](const Field &one) {
      return Kind::Toggle == one.kind && one.revealsSecrets && one.on;
    });
  }
  const bool complaining = subheading != note;

  if (seen != builtFor || ctx.screenWidth != builtWidth ||
      ctx.screenHeight != builtHeight) {
    builtFor    = seen;
    builtWidth  = ctx.screenWidth;
    builtHeight = ctx.screenHeight;

    canvas->clear();
    canvas->setTag(render::tagKindOverlay);

    const auto width  = static_cast<float>(ctx.screenWidth);
    const auto height = static_cast<float>(ctx.screenHeight);
    // Everything behind is dimmed rather than hidden: what is being published
    // is still worth seeing while deciding how to describe it.
    canvas->addRect(0.0F, 0.0F, width, height, ink(dimming));

    const auto rows = static_cast<float>(shown.size());
    // An open drop-down grows the panel rather than covering the rows under
    // it: a list that obscures the fields it is part of is a list somebody has
    // to close before they can see what they were filling in.
    const auto listRows =
        listDown && where < shown.size()
            ? static_cast<float>(shown[where].options.size())
            : 0.0F;
    const auto panelHeight = (2 * padding) + (rowHeight * 2) +
                             ((rows + listRows) * rowHeight) + rowHeight;
    const auto left   = std::max(0.0F, (width - panelWidth) / 2.0F);
    const auto bottom = std::max(0.0F, (height - panelHeight) / 2.0F);
    canvas->addRect(left, bottom, panelWidth, panelHeight, ink(panelBack));

    auto top = bottom + panelHeight - padding;
    // Ellipsised rather than allowed to run off the panel: a note saying where
    // a document will be written is often a long path, and text spilling past
    // the edge of a modal reads as a drawing mistake rather than as a value.
    canvas->setTextWidthLimit(static_cast<int>(panelWidth - (2 * padding)));
    canvas->addText(ctx.state, left + padding, top, heading, ink(titleInk),
                    ink(panelBack));
    top -= rowHeight;
    canvas->addText(ctx.state, left + padding, top, subheading,
                    ink(complaining ? troubleInk : hintInk), ink(panelBack));
    top -= rowHeight * 0.6F;

    for (std::size_t i = 0; i < shown.size(); i++) {
      const auto &one    = shown[i];
      const bool focused = i == where;
      top -= rowHeight;

      // A required field says so where it is asked, rather than only when it
      // is refused.
      canvas->addText(ctx.state, left + padding, top,
                      one.required ? one.label + " *" : one.label,
                      ink(one.required ? requiredInk : panelInk),
                      ink(panelBack));

      const auto boxLeft  = left + padding + labelWidth;
      const auto boxWidth = panelWidth - (2 * padding) - labelWidth;
      canvas->addRect(boxLeft, top - boxHeight + lineGap, boxWidth, boxHeight,
                      ink(focused ? boxFocused : boxBack));

      // What the box says, which for a secret is not what it holds.
      const auto held = Kind::Secret == one.kind && !reveal
                            ? std::string(charactersIn(one.value), '*')
                            : one.value;
      auto shownValue = held.empty() ? one.hint : held;
      auto valueInk   = held.empty() ? hintInk : panelInk;
      if (Kind::Choice == one.kind) {
        shownValue = one.options.empty()
                         ? one.hint
                         : one.options[std::min(one.chosen,
                                                one.options.size() - 1)];
        valueInk   = one.options.empty() ? hintInk : panelInk;
        // The mark every drop-down has, so it reads as one thing to open
        // rather than as text somebody forgot to make editable.
        shownValue += listDown && focused ? "   \u25B4" : "   \u25BE";
      } else if (Kind::Toggle == one.kind) {
        shownValue = one.on ? "[ hide ]" : "[ show ]";
        valueInk   = one.on ? titleInk : panelInk;
      }

      const auto textLeft = boxLeft + 6.0F;
      canvas->setTextWidthLimit(static_cast<int>(boxWidth - 12.0F));
      canvas->addText(ctx.state, textLeft, top, shownValue, ink(valueInk),
                      ink(focused ? boxFocused : boxBack));

      if (focused) {
        // Where the platform should put a candidate window or an on-screen
        // keyboard, in the pixels SDL counts in: from the top of the window
        // rather than the bottom, which is what the canvas draws in. Only for
        // fields that take text -- a button raises no keyboard.
        const std::lock_guard locker(guard);
        typingAt =
            Kind::Text == one.kind || Kind::Secret == one.kind
                ? std::optional<InputArea>{InputArea{
                      static_cast<int>(boxLeft),
                      static_cast<int>(static_cast<float>(ctx.screenHeight) -
                                       (top + lineGap)),
                      static_cast<int>(boxWidth), static_cast<int>(boxHeight)}}
                : std::nullopt;
      }

      if (focused && (Kind::Text == one.kind || Kind::Secret == one.kind)) {
        // Measured rather than counted: a caret placed by character count
        // would be in the wrong place in anything but a monospaced font. What
        // is measured is what is shown, so the caret in a masked field sits
        // among the asterisks.
        const auto upTo = Kind::Secret == one.kind && !reveal
                              ? std::string(charactersIn(one.value.substr(
                                                0, std::min(at, one.value.size()))),
                                            '*')
                              : one.value.substr(0, std::min(at, one.value.size()));
        const auto before = canvas->measureText(upTo);
        canvas->addRect(textLeft + before.width, top - boxHeight + lineGap,
                        caretWidth, boxHeight, ink(panelInk));
      }

      // The options, under the field they belong to.
      if (listDown && focused) {
        for (std::size_t option = 0; option < one.options.size(); option++) {
          top -= rowHeight;
          const bool under = option == lit;
          canvas->addRect(boxLeft, top - boxHeight + lineGap, boxWidth,
                          boxHeight, ink(under ? boxFocused : boxBack));
          canvas->addText(ctx.state, textLeft, top, one.options[option],
                          ink(under ? titleInk : panelInk),
                          ink(under ? boxFocused : boxBack));
        }
      }
    }

    top -= rowHeight;
    canvas->setTextWidthLimit(static_cast<int>(panelWidth - (2 * padding)));
    canvas->addText(ctx.state, left + padding, top,
                    listDown
                        ? "up/down: choose   enter: take it   esc: close it"
                        : "tab: field   space: open/press   enter: go ahead   "
                          "esc: leave it",
                    ink(hintInk), ink(panelBack));

    canvas->commit();
  }

  const glm::mat4 projection =
      glm::ortho(0.0F, static_cast<float>(ctx.screenWidth), 0.0F,
                 static_cast<float>(ctx.screenHeight));
  canvas->draw(ctx.state, projection);
}

} // namespace gleditor

// vi: set sw=2 sts=2 ts=2 et:
