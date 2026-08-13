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

} // namespace

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

void Form::open(std::string aTitle, std::string aNote,
                std::vector<Field> aFields, Accepted onAccept) {
  const std::lock_guard locker(guard);
  title    = std::move(aTitle);
  note     = std::move(aNote);
  fields   = std::move(aFields);
  accepted = std::move(onAccept);
  trouble.clear();
  focus = 0;
  // At the end of the first field, which is where somebody correcting a
  // filled-in value wants to be.
  caret = fields.empty() ? 0 : fields.front().value.size();
  open_ = true;
  revision++;
}

void Form::close() {
  const std::lock_guard locker(guard);
  open_ = false;
  accepted = nullptr;
  revision++;
}

Form::Field &Form::field() {
  focus = std::min(focus, fields.empty() ? 0 : fields.size() - 1);
  return fields[focus];
}

bool Form::complete(std::string &missing) const {
  for (const auto &one : fields) {
    if (one.required && one.value.empty()) {
      missing = one.label;
      return false;
    }
  }
  return true;
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
      open_    = false;
      accepted = nullptr;
      return true;

    case Key::Return: {
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
      break;
    }

    case Key::Tab:
      if (fields.empty()) {
        return true;
      }
      if (held(mods, KeyMods::Shift)) {
        focus = 0 == focus ? fields.size() - 1 : focus - 1;
      } else {
        focus = (focus + 1) % fields.size();
      }
      caret = fields[focus].value.size();
      return true;

    case Key::Up:
    case Key::Down:
      if (fields.empty()) {
        return true;
      }
      focus = Key::Up == key ? (0 == focus ? fields.size() - 1 : focus - 1)
                             : (focus + 1) % fields.size();
      caret = fields[focus].value.size();
      return true;

    case Key::Left:
      moveCaret(-1);
      return true;
    case Key::Right:
      moveCaret(1);
      return true;
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
  auto &value = field();
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
  std::size_t where  = 0;
  std::size_t at     = 0;
  std::uint64_t seen = 0;
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

    const auto rows       = static_cast<float>(shown.size());
    const auto panelHeight =
        (2 * padding) + (rowHeight * 2) + (rows * rowHeight) + rowHeight;
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

      const auto shownValue = one.value.empty() ? one.hint : one.value;
      const auto valueInk   = one.value.empty() ? hintInk : panelInk;
      const auto textLeft   = boxLeft + 6.0F;
      canvas->setTextWidthLimit(static_cast<int>(boxWidth - 12.0F));
      canvas->addText(ctx.state, textLeft, top, shownValue, ink(valueInk),
                      ink(focused ? boxFocused : boxBack));

      if (focused) {
        // Measured rather than counted: a caret placed by character count
        // would be in the wrong place in anything but a monospaced font.
        const auto before =
            canvas->measureText(one.value.substr(0, std::min(at, one.value.size())));
        canvas->addRect(textLeft + before.width, top - boxHeight + lineGap,
                        caretWidth, boxHeight, ink(panelInk));
      }
    }

    top -= rowHeight;
    canvas->setTextWidthLimit(static_cast<int>(panelWidth - (2 * padding)));
    canvas->addText(ctx.state, left + padding, top,
                    "tab: next field   enter: go ahead   esc: leave it",
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
