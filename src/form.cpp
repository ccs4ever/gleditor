/**
 * @file form.cpp
 * @brief Implementation of the modal field panel.
 */
#include <gleditor/form.hpp> // IWYU pragma: associated

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <glm/ext/matrix_clip_space.hpp>

#include <array>
#include <cmath>
#include <gleditor/canvas.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/ui/widgets.hpp>
#include <gleditor/utf8.hpp>

namespace gleditor {

namespace {

/// Characters in @p text, counting lead bytes: what a masked field shows one
/// asterisk for. Bytes would show three for every accented letter, which says
/// something about the passphrase that is nobody's business.
std::size_t charactersIn(const std::string_view text) {
  return static_cast<std::size_t>(
      std::ranges::count_if(text, [](const char chr) {
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
  device_   = &device;
  pipeline_ = documentPipeline;
  canvas.reset();
  headingCanvas.reset();
  drawnFont_.clear();
  drawnHeadingFont_.clear();
  builtFor = 0;
}

bool Form::active() const {
  const std::scoped_lock locker(guard);
  return open_;
}

std::vector<Form::Field> Form::current() const {
  const std::scoped_lock locker(guard);
  return fields;
}

std::size_t Form::focused() const {
  const std::scoped_lock locker(guard);
  return focus;
}

std::string Form::complaint() const {
  const std::scoped_lock locker(guard);
  return trouble;
}

namespace {

/// What kind of thing a field is, in the platforms' vocabulary.
a11y::Role roleOf(const Form::Kind kind) {
  switch (kind) {
  case Form::Kind::Text:
    return a11y::Role::TextInput;
  case Form::Kind::Secret:
    // Its own role rather than a text field that happens to be masked: it is
    // what stops a screen reader reading a passphrase aloud, and what stops
    // it being kept in whatever history the platform keeps.
    return a11y::Role::PasswordInput;
  case Form::Kind::Choice:
    return a11y::Role::ComboBox;
  case Form::Kind::Toggle:
    return a11y::Role::Switch;
  }
  return a11y::Role::TextInput;
}

/// Node numbering within a form. Fixed offsets rather than a running counter,
/// so that a field keeps its id as the panel grows a complaint or loses one --
/// an id that moved would make a screen reader announce the whole form again
/// on every keystroke.
constexpr std::uint64_t panelId    = 0;
constexpr std::uint64_t titleId    = 1;
constexpr std::uint64_t noteId     = 2;
constexpr std::uint64_t firstField = 16;
/// Room for a Choice's options under each field.
constexpr std::uint64_t perField = 64;

constexpr std::uint64_t fieldId(const std::size_t which) {
  return firstField + (which * perField);
}
constexpr std::uint64_t optionId(const std::size_t which,
                                 const std::size_t option) {
  // Keep established IDs for short lists; long lists occupy a disjoint range.
  return option < perField - 1 ? fieldId(which) + 1 + option
                               : (1ULL << 31) + (which << 16) + option - 63;
}

constexpr std::size_t fieldNumber(std::uint64_t local) {
  return local >= (1ULL << 31) ? (local - (1ULL << 31)) >> 16
                               : (local - firstField) / perField;
}
constexpr std::size_t withinField(std::uint64_t local) {
  return local >= (1ULL << 31) ? ((local - (1ULL << 31)) & 65535) + 64
                               : (local - firstField) % perField;
}

} // namespace

void Form::describe(a11y::Builder &into) {
  const std::scoped_lock locker(guard);
  if (!open_) {
    return;
  }

  {
    auto &panel = into.add(panelId, a11y::Role::Dialog);
    panel.label = title;
    if (focusLayout_) {
      const auto rect = focusLayout_->bounds;
      panel.bounds    = a11y::Rect{
             .left  = rect.left,
             .top   = static_cast<double>(builtHeight) - rect.bottom - rect.height,
             .right = rect.left + rect.width,
             .bottom = static_cast<double>(builtHeight) - rect.bottom};
    }
    panel.children.push_back(into.id(titleId));
    if (!note.empty() || !trouble.empty()) {
      panel.children.push_back(into.id(noteId));
    }
    for (std::size_t which = 0; which < fields.size(); which++) {
      panel.children.push_back(into.id(fieldId(which)));
    }
    into.contribute(into.id(panelId));
  }

  {
    auto &heading = into.add(titleId, a11y::Role::Label);
    heading.value = title;
    if (focusLayout_) {
      if (const auto *box = focusLayout_->find(titleId)) {
        const auto area = ui::toInputArea(box->rect, builtHeight);
        heading.bounds  = {static_cast<double>(area.x),
                           static_cast<double>(area.y),
                           static_cast<double>(area.x + area.width),
                           static_cast<double>(area.y + area.height)};
      }
    }
  }
  if (!note.empty() || !trouble.empty()) {
    auto &line = into.add(noteId, a11y::Role::Label);
    // The complaint replaces the note on screen, and does the same here --
    // said at once rather than at the next pause, because it is the answer to
    // something the person just tried to do.
    line.value = trouble.empty() ? note : trouble;
    line.live  = trouble.empty() ? a11y::Live::Off : a11y::Live::Assertive;
    if (focusLayout_) {
      if (const auto *box = focusLayout_->find(noteId)) {
        const auto area = ui::toInputArea(box->rect, builtHeight);
        line.bounds = {static_cast<double>(area.x), static_cast<double>(area.y),
                       static_cast<double>(area.x + area.width),
                       static_cast<double>(area.y + area.height)};
      }
    }
  }

  const bool reveal = std::ranges::any_of(fields, [](const Field &one) {
    return Kind::Toggle == one.kind && one.revealsSecrets && one.on;
  });

  for (std::size_t which = 0; which < fields.size(); which++) {
    const auto &one = fields[which];
    auto &node      = into.add(fieldId(which), roleOf(one.kind));
    // A toggle beside a passphrase has no label of its own on screen -- the
    // button says what it does -- so it is named by what it does.
    node.label       = one.label.empty() && Kind::Toggle == one.kind
                           ? "show the passphrase"
                           : one.label;
    node.placeholder = one.hint;
    node.focusable   = true;
    node.actions     = a11y::bit(a11y::Action::Focus);
    if (focusLayout_) {
      if (const auto box =
              focusLayout_->find(static_cast<std::uint32_t>(fieldId(which)))) {
        const auto rect = box->rect;
        node.bounds     = a11y::Rect{
                .left = rect.left,
                .top = static_cast<double>(builtHeight) - rect.bottom - rect.height,
                .right  = rect.left + rect.width,
                .bottom = static_cast<double>(builtHeight) - rect.bottom};
      }
    }

    switch (one.kind) {
    case Kind::Text:
      node.actions |= a11y::bit(a11y::Action::SetValue);
      node.value = one.value;
      break;
    case Kind::Secret:
      node.actions |= a11y::bit(a11y::Action::SetValue);
      // Never the passphrase itself, revealed or not. What is on screen is a
      // person's choice about their own screen; what goes on the accessibility
      // bus is readable by anything on the session, and a screen reader will
      // say it out loud.
      node.value = std::string(charactersIn(one.value), '*');
      node.description =
          reveal ? "shown on screen" : "hidden; there is a button to show it";
      break;
    case Kind::Choice:
      if (!one.options.empty() &&
          one.optionDescriptions.size() == one.options.size())
        node.description = one.optionDescriptions[std::min(
            one.chosen, one.options.size() - 1)];
      node.value =
          one.options.empty()
              ? std::string{}
              : one.options[std::min(one.chosen, one.options.size() - 1)];
      node.actions |= a11y::bit(a11y::Action::Click);
      for (std::size_t option = 0; option < one.options.size(); option++) {
        node.children.push_back(into.id(optionId(which, option)));
      }
      break;
    case Kind::Toggle:
      node.toggled = one.on;
      node.actions |= a11y::bit(a11y::Action::Click);
      break;
    }

    if (Kind::Choice == one.kind) {
      for (std::size_t option = 0; option < one.options.size(); option++) {
        auto &entry = into.add(optionId(which, option), a11y::Role::ListItem);
        entry.label = one.options[option];
        if (one.optionDescriptions.size() == one.options.size())
          entry.description = one.optionDescriptions[option];
        entry.actions =
            a11y::bit(a11y::Action::Focus) | a11y::bit(a11y::Action::Click);
        entry.focusable = true;
        if (focusLayout_) {
          if (const auto *box = focusLayout_->find(
                  static_cast<std::uint32_t>(optionId(which, option)))) {
            const auto area = ui::toInputArea(box->rect, builtHeight);
            entry.bounds    = {static_cast<double>(area.x),
                               static_cast<double>(area.y),
                               static_cast<double>(area.x + area.width),
                               static_cast<double>(area.y + area.height)};
          }
        }
      }
    }
  }
}

std::uint64_t Form::accessibilityRevision() const {
  const std::scoped_lock locker(guard);
  // The same counter the drawing uses, which is bumped by every key the form
  // takes. Nothing else changes what a form has to say.
  return open_ ? revision : 0;
}

bool Form::performAction(const std::uint64_t nodeId, const a11y::Action action,
                         const std::string_view value) {
  const auto local = a11y::Ids::localOf(nodeId);
  if (local < firstField) {
    return false;
  }
  const auto which  = fieldNumber(local);
  const auto within = withinField(local);

  const std::scoped_lock locker(guard);
  if (!open_ || which >= fields.size()) {
    return false;
  }
  auto &one = fields[which];

  // An option under a Choice: picking it is what a click on it means.
  if (0 != within) {
    const auto option = within - 1;
    if (a11y::Action::Click != action || option >= one.options.size()) {
      return false;
    }
    setFocusedNode(static_cast<std::uint32_t>(fieldId(which)));
    requestFocus(static_cast<std::uint32_t>(fieldId(which)));
    one.chosen = option;
    expanded   = false;
    trouble.clear();
    revision++;
    return true;
  }

  switch (action) {
  case a11y::Action::Focus:
    setFocusedNode(static_cast<std::uint32_t>(fieldId(which)));
    requestFocus(static_cast<std::uint32_t>(fieldId(which)));
    caret = one.value.size();
    revision++;
    return true;
  case a11y::Action::Click:
    setFocusedNode(static_cast<std::uint32_t>(fieldId(which)));
    requestFocus(static_cast<std::uint32_t>(fieldId(which)));
    if (Kind::Toggle == one.kind) {
      one.on = !one.on;
      revision++;
      return true;
    }
    if (Kind::Choice == one.kind && !one.options.empty()) {
      expanded  = true;
      highlight = one.chosen;
      revision++;
      return true;
    }
    return false;
  case a11y::Action::SetValue:
    if (Kind::Text != one.kind && Kind::Secret != one.kind) {
      return false;
    }
    one.value = value;
    setFocusedNode(static_cast<std::uint32_t>(fieldId(which)));
    requestFocus(static_cast<std::uint32_t>(fieldId(which)));
    caret = one.value.size();
    trouble.clear();
    revision++;
    return true;
  case a11y::Action::ScrollIntoView:
    return false;
  }
  return false;
}

std::optional<InputArea> Form::textArea() const {
  const std::scoped_lock locker(guard);
  return typingAt;
}

bool Form::pointerEvent(const ui::PointerEvent &event) {
  std::optional<std::uint32_t> target;
  a11y::Action action = a11y::Action::Focus;
  {
    const std::scoped_lock locker(guard);
    if (!open_) return false;
    if (event.phase == ui::PointerPhase::Wheel && expanded && !fields.empty()) {
      if (event.deltaY == 0) return true;
      chooseOption(event.deltaY > 0 ? -1 : 1);
      ++revision;
      return true;
    }
    if (event.phase != ui::PointerPhase::Press || event.button != 1 ||
        !focusLayout_)
      return true;
    const auto *box = focusLayout_->hitTest(
        event.x, static_cast<float>(builtHeight) - event.y);
    if (!box || box->id < firstField) return true;
    target           = box->id;
    const auto which = fieldNumber(box->id);
    if (which >= fields.size()) return true;
    if (withinField(box->id) != 0 || fields[which].kind == Kind::Choice ||
        fields[which].kind == Kind::Toggle)
      action = a11y::Action::Click;
  }
  return target && performAction(*target, action, {});
}

void Form::open(std::string aTitle, std::string aNote,
                std::vector<Field> aFields, Accepted onAccept,
                Cancelled onCancel) {
  // IDs reserve 16 bits for each long list and 15 bits for its field.
  if (aFields.size() > 32768 ||
      std::ranges::any_of(
          aFields, [](const Field &f) { return f.options.size() > 65599; }))
    throw std::length_error("Form accessibility IDs exhausted");
  const std::scoped_lock locker(guard);
  title     = std::move(aTitle);
  note      = std::move(aNote);
  fields    = std::move(aFields);
  accepted  = std::move(onAccept);
  cancelled = std::move(onCancel);
  trouble.clear();
  focus    = 0;
  expanded = false;
  // At the end of the first field, which is where somebody correcting a
  // filled-in value wants to be.
  caret = fields.empty() ? 0 : fields.front().value.size();
  open_ = true;
  resetFocusLayout();
  activate();
  // Where the last form's focused field was is not where this one's is, and
  // until this one has been drawn nobody knows where that will be. Saying
  // nothing is right: the platform keeps whatever it was told last, which is
  // better than being pointed somewhere this form is not.
  typingAt.reset();
  revision++;
}

void Form::close() {
  Cancelled onCancel;
  {
    const std::scoped_lock locker(guard);
    open_ = false;
    deactivate();
    accepted  = nullptr;
    onCancel  = std::move(cancelled);
    cancelled = nullptr;
    // Nothing is being typed into any more, so there is nowhere for an input
    // method to put itself.
    typingAt.reset();
    revision++;
  }
  if (onCancel) {
    onCancel();
  }
}

Form::Field &Form::field() {
  focus = std::min(focus, fields.empty() ? 0 : fields.size() - 1);
  return fields[focus];
}

std::optional<std::string> Form::firstMissing() const {
  const auto unanswered = std::ranges::find_if(fields, [](const Field &one) {
    return one.required && one.answer().empty();
  });
  if (unanswered == fields.end()) {
    return std::nullopt;
  }
  return unanswered->label;
}

bool Form::listOpen() const {
  const std::scoped_lock locker(guard);
  return open_ && expanded;
}

bool Form::secretsShown() const {
  const std::scoped_lock locker(guard);
  return std::ranges::any_of(fields, [](const Field &one) {
    return Kind::Toggle == one.kind && one.revealsSecrets && one.on;
  });
}

std::shared_ptr<const ui::LayoutResult> Form::focusLayout() const {
  const std::scoped_lock locker(guard);
  return open_ ? focusLayout_ : nullptr;
}
void Form::resetFocusLayout() {
  auto layout = std::make_shared<ui::LayoutResult>();
  for (std::size_t which = 0; which < fields.size(); ++which) {
    const auto id = static_cast<std::uint32_t>(fieldId(which));
    const bool takesText =
        fields[which].kind == Kind::Text || fields[which].kind == Kind::Secret;
    layout->boxes.push_back(
        {.id = id, .focusable = true, .focusGroup = 1, .textInput = takesText});
    layout->focusOrder.push_back(id);
  }
  focusLayout_ = std::move(layout);
}
void Form::setFocusedNode(std::uint32_t id) {
  if (id < firstField || (id - firstField) % perField != 0) return;
  const auto which = static_cast<std::size_t>((id - firstField) / perField);
  if (which >= fields.size()) return;
  if (which != focus && expanded && focus < fields.size()) {
    fields[focus].chosen = highlight;
    expanded             = false;
  }
  focus = which;
  caret = fields[focus].value.size();
  typingAt.reset();
  if (focusLayout_) {
    if (const auto box = focusLayout_->find(id); box && box->textInput &&
                                                 box->rect.width > 0.0F &&
                                                 box->rect.height > 0.0F) {
      typingAt = ui::toInputArea(box->contentRect, builtHeight);
    }
  }
}
void Form::focusedNodeChanged(std::uint32_t id) {
  const std::scoped_lock locker(guard);
  if (!open_) return;
  setFocusedNode(id);
  ++revision;
}
bool Form::activateNode(std::uint32_t id) {
  {
    const std::scoped_lock locker(guard);
    if (!open_ || id < firstField || (id - firstField) % perField != 0 ||
        (id - firstField) / perField >= fields.size())
      return false;
    // Enter operates the field's control; selecting the same field must not
    // settle an expanded choice before its Return handler sees the list.
    if (static_cast<std::size_t>((id - firstField) / perField) != focus) {
      setFocusedNode(id);
    }
  }
  return keyPressed(Key::Return, KeyMods::None);
}
void Form::moveFocus(bool reverse) {
  if (!focusLayout_) resetFocusLayout();
  if (const auto id = ui::nextFocusNode(
          focusLayout_->focusOrder, static_cast<std::uint32_t>(fieldId(focus)),
          reverse)) {
    setFocusedNode(*id);
    requestFocus(*id);
  }
}
void Form::chooseOption(const int by) {
  if (fields.empty() || !expanded) return;
  const auto count = fields[focus].options.size();
  if (count == 0) return;
  highlight =
      by < 0 ? (highlight + count - 1) % count : (highlight + 1) % count;
}

void Form::moveCaret(const int by) {
  ui::TextField input{.value = fields[focus].value, .caret = caret};
  std::ignore = input.key(by < 0 ? Key::Left : Key::Right, KeyMods::None);
  caret       = input.caret;
}

void Form::beforeFocusTraversal() {
  const std::scoped_lock locker(guard);
  if (expanded && focus < fields.size()) {
    fields[focus].chosen = highlight;
    expanded             = false;
    ++revision;
  }
}
bool Form::keyPressed(const ui::KeyEvent &event) {
  if (event.key == Key::Up || event.key == Key::Down) {
    const std::scoped_lock locker(guard);
    if (!expanded) return false;
  }
  return keyPressed(event.key, event.mods);
}

bool Form::keyPressed(const Key key, const KeyMods mods) {
  Accepted toCall;
  Cancelled toCancel;
  std::vector<Field> answers;
  {
    const std::scoped_lock locker(guard);
    if (!open_) {
      return false;
    }
    revision++;

    if (fields.empty() && key != Key::Return && key != Key::Escape)
      return false;

    switch (key) {
    case Key::Escape: {
      if (expanded) {
        // The list closes and the form stays: escape undoes the smaller thing
        // first, which is what every list that opens like this does.
        expanded = false;
        return true;
      }
      open_ = false;
      deactivate();
      toCancel  = std::move(cancelled);
      accepted  = nullptr;
      cancelled = nullptr;
      typingAt.reset();
      break;
    }

    case Key::Return: {
      if (expanded) {
        fields[focus].chosen = highlight;
        expanded             = false;
        trouble.clear();
        return true;
      }
      if (!fields.empty()) {
        auto &here = field();
        // Enter on a closed drop-down opens it, so that a key can be picked
        // without knowing that space is what opens one. A list with nothing in
        // it is not opened: an empty panel would say less than the hint already
        // showing, and the field is answered as empty either way.
        if (Kind::Choice == here.kind && !here.options.empty() &&
            !here.submitOnEnter) {
          expanded  = true;
          highlight = here.chosen;
          return true;
        }
        if (Kind::Toggle == here.kind) {
          here.on = !here.on;
          return true;
        }
      }
      if (const auto missing = firstMissing()) {
        // Refused rather than published half-filled: the fields marked
        // required are the ones a reader would otherwise find empty in
        // something signed.
        trouble = *missing + " is needed before this can go out";
        return true;
      }
      // The form comes down first, and the callback runs outside the lock, so
      // that what it does -- which may be to open another form -- does not
      // deadlock against this one.
      open_ = false;
      deactivate();
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
      moveFocus(held(mods, KeyMods::Shift));
      return true;

    case Key::Up:
      if (expanded)
        chooseOption(-1);
      else
        moveFocus(true);
      return true;
    case Key::Down:
      if (expanded)
        chooseOption(1);
      else
        moveFocus(false);
      return true;

    case Key::Space: {
      auto &here = field();
      if (Kind::Toggle == here.kind) {
        here.on = !here.on;
        return true;
      }
      if (Kind::Choice == here.kind && !here.options.empty()) {
        expanded  = !expanded;
        highlight = here.chosen;
        return true;
      }
      return false;
    }
    case Key::Left:
    case Key::Right: {
      auto &here = field();
      if (Kind::Choice == here.kind && !here.options.empty()) {
        // Through the options without opening the list, for somebody who knows
        // what is in it.
        const auto count = here.options.size();
        if (Key::Left == key) {
          here.chosen = 0 == here.chosen ? count - 1 : here.chosen - 1;
        } else {
          here.chosen = (here.chosen + 1) % count;
        }
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
      if (value.kind != Kind::Text && value.kind != Kind::Secret) return false;
      ui::TextField input{.value = value.value, .caret = caret};
      std::ignore = input.key(Key::Backspace, mods);
      value.value = std::move(input.value);
      caret       = input.caret;
      trouble.clear();
      return true;
    }
    case Key::Delete: {
      auto &value = field();
      if (value.kind != Kind::Text && value.kind != Kind::Secret) return false;
      ui::TextField input{.value = value.value, .caret = caret};
      std::ignore = input.key(Key::Delete, mods);
      value.value = std::move(input.value);
      caret       = input.caret;
      trouble.clear();
      return true;
    }
    default:
      return false;
    }
  }

  if (toCancel) {
    toCancel();
  }
  if (toCall) {
    toCall(answers);
  }
  return true;
}

void Form::textTyped(std::string_view utf8) {
  const std::scoped_lock locker(guard);
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

  ui::TextField input{.value = here.value, .caret = caret};
  input.insert(utf8);
  here.value = std::move(input.value);
  caret      = input.caret;
  trouble.clear();
  revision++;
}

void Form::drawFrame(FrameContext &ctx) {
  if (!device_) return;
  auto metrics         = ctx.metrics;
  metrics.screenWidth  = ctx.screenWidth;
  metrics.screenHeight = ctx.screenHeight;
  metrics.chrome       = ctx.chrome;
  std::string heading, subheading;
  std::vector<Field> shown;
  std::size_t where{}, at{}, lit{};
  std::uint64_t seen{};
  bool listDown{}, reveal{}, complaining{}, rebuild{};
  {
    const std::scoped_lock locker(guard);
    if (!open_) return;
    seen    = revision;
    rebuild = seen != builtFor || metrics != builtMetrics_ ||
              ctx.theme != builtTheme_;
    // Steady frames submit the retained buffers without copying field values,
    // including secrets, or asking the text engine to shape them again.
    if (rebuild) {
      heading     = title;
      complaining = !trouble.empty();
      subheading  = complaining ? trouble : note;
      shown       = fields;
      where       = focus;
      at          = caret;
      listDown    = expanded;
      lit         = highlight;
      reveal      = std::ranges::any_of(fields, [](const Field &one) {
        return Kind::Toggle == one.kind && one.revealsSecrets && one.on;
      });
    }
  }
  if (rebuild) {
    const auto fontDescription = ui::scaledFontDescription(
        fontName, ui::FontRole::Label, metrics, ctx.theme);
    const auto headingDescription =
        metrics.fontDescription(ui::FontRole::Title, ctx.theme);
    if (!canvas || drawnFont_ != fontDescription) {
      canvas = std::make_unique<Canvas>(device_, fontDescription);
      canvas->createPipeline(pipeline_, false);
      drawnFont_ = fontDescription;
    }
    if (!headingCanvas || drawnHeadingFont_ != headingDescription) {
      headingCanvas = std::make_unique<Canvas>(device_, headingDescription);
      headingCanvas->createPipeline(pipeline_, false);
      drawnHeadingFont_ = headingDescription;
    }
    const auto font = text::FontManager::instance().getFont(fontDescription);
    const auto titleFont =
        text::FontManager::instance().getFont(headingDescription);
    const auto line = font->metrics().lineHeight;
    const auto safe = metrics.pixelSafeArea();
    const auto pad  = std::min(std::max(0.0F, line * ctx.theme.paddingEm),
                               safe.height * .04F);
    const auto gap =
        std::min(std::max(0.0F, line * ctx.theme.gapEm), safe.height * .015F);
    const auto controlHeight =
        std::max(metrics.px(ctx.theme.type.minTouchPx), line + pad * 2);
    // Intrinsic font-relative width scales with typography, then containment
    // takes precedence over the preferred size on a small window.
    const auto panelWidth = std::min(safe.width, line * 36);
    const auto innerWidth = std::max(0.0F, panelWidth - 2 * pad);
    const bool columns    = innerWidth >= controlHeight * 9;
    const auto rowHeight =
        columns ? controlHeight
                : std::ceil(line) + 2 + gap * .5F + controlHeight;
    const text::TextFit titleFit{.maxWidthPx = innerWidth,
                                 .maxHeightPx =
                                     titleFont->metrics().lineHeight * 2,
                                 .maxLines = 2,
                                 .overflow = text::Overflow::Wrap};
    const auto titleText = shaping_.fitted(heading, titleFont, titleFit);
    const text::TextFit noteFit{.maxWidthPx  = innerWidth,
                                .maxHeightPx = line * 3,
                                .maxLines    = 3,
                                .overflow    = text::Overflow::Wrap};
    const auto noteText = shaping_.fitted(subheading, font, noteFit);
    // A focused input gets first claim on the vertical budget. Supporting
    // headings clip on small windows rather than squeezing its content away.
    const auto supportingBudget =
        std::max(0.0F, safe.height - 2 * pad - 3 * gap -
                           std::min(rowHeight, safe.height * .7F));
    const auto footerHeight =
        std::min(std::ceil(line) + 2, supportingBudget * .2F);
    const auto titleHeight =
        std::min(std::max(titleText.heightPx, titleFont->metrics().lineHeight),
                 supportingBudget * .5F);
    const auto noteHeight =
        subheading.empty()
            ? 0.0F
            : std::min(noteText.heightPx,
                       supportingBudget - titleHeight - footerHeight);
    const auto extraRows = listDown && where < shown.size()
                               ? static_cast<float>(std::min<std::size_t>(
                                     shown[where].options.size(), 4)) *
                                     controlHeight
                               : 0;
    const auto fieldsHeight =
        static_cast<float>(shown.size()) * (rowHeight + gap) + extraRows;
    auto panel = ui::clampToSafeArea(
        metrics.rounded(
            {safe.left + (safe.width - panelWidth) * .5F,
             safe.bottom + (safe.height -
                            std::min(safe.height,
                                     titleHeight + noteHeight + fieldsHeight +
                                         footerHeight + pad * 2 + gap * 3)) *
                               .5F,
             panelWidth,
             std::min(safe.height, titleHeight + noteHeight + fieldsHeight +
                                       footerHeight + pad * 2 + gap * 3)}),
        safe);
    const auto actualPad =
        std::min({pad, panel.width * .5F, panel.height * .5F});
    const ui::Rect interior{panel.left + actualPad, panel.bottom + actualPad,
                            std::max(0.0F, panel.width - 2 * actualPad),
                            std::max(0.0F, panel.height - 2 * actualPad)};
    const std::array<ui::LayoutItem, 4> sections{{
        {.id        = 1,
         .intrinsic = {interior.width, titleHeight},
         .minimum   = {0, titleHeight}},
        {.id        = 2,
         .intrinsic = {interior.width, noteHeight},
         .minimum   = {0, noteHeight}},
        {.id        = 3,
         .intrinsic = {interior.width, fieldsHeight},
         .minimum   = {0, std::min(rowHeight, safe.height * .7F)},
         .grow      = 1},
        {.id        = 4,
         .intrinsic = {interior.width, footerHeight},
         .minimum   = {0, footerHeight}},
    }};
    const auto parts = ui::stack(interior, sections, {.gap = gap});
    auto layout      = std::make_shared<ui::LayoutResult>();
    layout->bounds   = panel;
    layout->boxes.push_back({.id = 0, .rect = panel, .contentRect = interior});
    layout->append(parts);
    // Hidden rows remain in traversal order. Focusing one reveals its viewport
    // on the next draw without losing the answers in other fields.
    for (std::size_t i = 0; i < shown.size(); ++i) {
      const auto id = static_cast<std::uint32_t>(fieldId(i));
      layout->boxes.push_back(
          {.id          = id,
           .parentId    = 0,
           .rect        = {interior.left, interior.bottom, 0, 0},
           .contentRect = {interior.left, interior.bottom, 0, 0},
           .focusable   = true,
           .focusGroup  = 1,
           .textInput =
               shown[i].kind == Kind::Text || shown[i].kind == Kind::Secret});
      layout->focusOrder.push_back(id);
    }
    canvas->clear();
    headingCanvas->clear();
    canvas->setTag(render::tagKindOverlay);
    headingCanvas->setTag(render::tagKindOverlay);
    const auto surface    = ui::rgba(ctx.theme.colours.surface);
    const auto foreground = ui::rgba(ctx.theme.colours.text);
    const auto muted      = ui::rgba(ctx.theme.colours.muted);
    const auto accent     = ui::rgba(ctx.theme.colours.accent);
    canvas->addRect(0, 0, static_cast<float>(ctx.screenWidth),
                    static_cast<float>(ctx.screenHeight), 0x00000090U);
    canvas->addRect(panel.left, panel.bottom, panel.width, panel.height,
                    surface);
    headingCanvas->addText(ctx.state, parts.find(1)->rect, titleText,
                           foreground, surface);
    canvas->addText(ctx.state, parts.find(2)->rect, noteText,
                    complaining ? accent : muted, surface);
    const auto viewport = parts.find(3)->rect;
    const auto capacity = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::floor(
               (viewport.height + gap) / std::max(1.0F, rowHeight + gap))));
    if (where < firstVisible_) firstVisible_ = where;
    if (where >= firstVisible_ + capacity) firstVisible_ = where - capacity + 1;
    firstVisible_ = std::min(
        firstVisible_, shown.size() > capacity ? shown.size() - capacity : 0);
    const auto first = listDown ? where : firstVisible_;
    const auto last = std::min(shown.size(), first + (listDown ? 1 : capacity));
    std::vector<ui::LayoutItem> rows;
    for (auto i = first; i < last; ++i)
      rows.push_back({.id        = static_cast<std::uint32_t>(fieldId(i)),
                      .intrinsic = {viewport.width, rowHeight}});
    auto rowBounds = viewport;
    if (listDown) {
      // Keep the highlighted option visible even when typography is taller
      // than the viewport; both control and option text can clip independently.
      const auto height =
          std::min(rowHeight, std::max(0.0F, viewport.height - gap) * .5F);
      rowBounds = {viewport.left, viewport.bottom + viewport.height - height,
                   viewport.width, height};
    }
    const auto placed    = ui::stack(rowBounds, rows, {.gap = gap});
    auto inputTheme      = ctx.theme;
    const auto lastSpace = fontDescription.rfind(' ');
    inputTheme.fonts[static_cast<std::size_t>(ui::FontRole::Label)] = {
        fontDescription.substr(0, lastSpace),
        static_cast<float>(font->pointSize())};
    const ui::UiMetrics inputMetrics{.screenWidth  = ctx.screenWidth,
                                     .screenHeight = ctx.screenHeight,
                                     .marginShare  = 0};
    canvas->pushClip(viewport);
    for (auto i = first; i < last; ++i) {
      const auto &one = shown[i];
      const auto id   = static_cast<std::uint32_t>(fieldId(i));
      const auto row  = placed.find(id)->rect;
      ui::Rect labelBox, fieldBox;
      if (columns) {
        const ui::LayoutItem labelItem{.id = 5};
        const ui::LayoutItem fieldItem{.id = id};
        const auto pair = ui::split(row, labelItem, fieldItem,
                                    {.firstShare = .32F, .gap = gap});
        labelBox        = pair.find(5)->rect;
        fieldBox        = pair.find(id)->rect;
      } else {
        const std::array<ui::LayoutItem, 2> items{{
            {.id = 5, .intrinsic = {row.width, std::ceil(line) + 2}},
            {.id = id, .intrinsic = {row.width, controlHeight}},
        }};
        const auto pair = ui::stack(row, items, {.gap = gap * .5F});
        labelBox        = pair.find(5)->rect;
        fieldBox        = pair.find(id)->rect;
      }
      std::ignore = canvas->addText(
          ctx.state, labelBox, one.required ? one.label + " *" : one.label,
          one.required ? accent : foreground, surface, {}, &shaping_);
      const bool takesText = one.kind == Kind::Text || one.kind == Kind::Secret;
      const auto masked    = one.kind == Kind::Secret && !reveal;
      const auto value =
          masked ? std::string(charactersIn(one.value), '*') : one.value;
      std::string controlLabel;
      if (one.kind == Kind::Choice) {
        controlLabel =
            one.options.empty()
                ? one.hint
                : one.options[std::min(one.chosen, one.options.size() - 1)];
        controlLabel += listDown && i == where ? "   \u25B4" : "   \u25BE";
      } else if (one.kind == Kind::Toggle)
        controlLabel = one.on ? "[ hide ]" : "[ show ]";
      ui::Widget widget{.id = id};
      if (takesText)
        widget.model = ui::TextField{
            .value       = value,
            .placeholder = one.hint,
            .caret =
                i == where
                    ? (masked ? charactersIn(std::string_view(one.value).substr(
                                    0, std::min(at, one.value.size())))
                              : at)
                    : 0};
      else
        widget.model = ui::Button{controlLabel, {}};
      inputTheme.paddingEm = std::min(
          ctx.theme.paddingEm, fieldBox.height / std::max(1.0F, line * 4));
      widget.preferred.height = fieldBox.height;
      const auto scene     = ui::layoutWidgets(widget, fieldBox, inputMetrics,
                                               inputTheme, shaping_);
      const auto *geometry = scene.layout.find(id);
      const auto *visual   = scene.find(id);
      auto box             = *geometry;
      box.parentId         = 0;
      box.focusGroup       = 1;
      box.focusable        = true;
      box.textInput        = takesText;
      *std::ranges::find(layout->boxes, id, &ui::LayoutBox::id) = box;
      const auto fill                                           = i == where
                                                                      ? ui::rgba(glm::mix(ctx.theme.colours.surface,
                                                                                          ctx.theme.colours.accent, .2F))
                                                                      : surface;
      canvas->addRect(box.rect.left, box.rect.bottom, box.rect.width,
                      box.rect.height, fill);
      canvas->pushClip(box.contentRect);
      auto textBox = box.contentRect;
      if (takesText) {
        textBox.left -= visual->textOffsetPx;
        textBox.width = std::max(textBox.width + visual->textOffsetPx,
                                 visual->fitted.widthPx);
      }
      canvas->addText(ctx.state, textBox, visual->fitted,
                      value.empty() && takesText ? muted : foreground, fill);
      if (takesText && i == where && visual->caretOffsetPx) {
        const auto thickness = std::min(metrics.px(1), box.contentRect.width);
        canvas->addRect(
            box.contentRect.left +
                std::min(*visual->caretOffsetPx,
                         std::max(0.0F, box.contentRect.width - thickness)),
            box.contentRect.bottom, thickness, box.contentRect.height,
            foreground);
      }
      canvas->popClip();
    }
    if (listDown && where < shown.size() && !shown[where].options.empty()) {
      const auto &options = shown[where].options;
      const auto used     = placed.boxes.empty() ? viewport.height
                                                 : placed.boxes.front().rect.height;
      const ui::Rect listBox{viewport.left, viewport.bottom, viewport.width,
                             std::max(0.0F, viewport.height - used - gap)};
      const auto visible = std::max(
          1.0F, std::floor(listBox.height / std::max(1.0F, controlHeight)));
      ui::List list{.scrollPx =
                        std::max(0.0F, static_cast<float>(lit) + 1 - visible) *
                        controlHeight,
                    .rowHeightPx = controlHeight,
                    .overscan    = 0};
      for (std::size_t option = 0; option < options.size(); ++option)
        list.rows.push_back(
            {static_cast<std::uint32_t>(optionId(where, option)),
             options[option],
             {}});
      ui::Widget widget{.id        = 3,
                        .model     = std::move(list),
                        .preferred = {listBox.width, listBox.height}};
      auto listTheme      = inputTheme;
      listTheme.paddingEm = 0;
      const auto choices =
          ui::layoutWidgets(widget, listBox, inputMetrics, listTheme, shaping_);
      for (const auto &visual : choices.visuals) {
        if (visual.id == 3) continue;
        const auto *box = choices.layout.find(visual.id);
        layout->boxes.push_back(*box);
        const auto selected = visual.itemIndex == lit;
        if (selected)
          canvas->addRect(box->rect.left, box->rect.bottom, box->rect.width,
                          box->rect.height, accent);
        canvas->addText(ctx.state, box->contentRect, visual.fitted, foreground,
                        selected ? accent : surface);
      }
    }
    canvas->popClip();
    std::ignore = canvas->addText(
        ctx.state, parts.find(4)->rect,
        listDown ? "up/down: choose   enter: take it   esc: close it"
                 : "tab: field   space: open/press   enter: go ahead   esc: "
                   "leave it",
        muted, surface, {}, &shaping_);
    canvas->commit();
    headingCanvas->commit();
    builtFor      = seen;
    builtMetrics_ = metrics;
    builtTheme_   = ctx.theme;
    {
      const std::scoped_lock locker(guard);
      builtWidth  = ctx.screenWidth;
      builtHeight = ctx.screenHeight;
      if (seen == revision && open_) {
        focusLayout_ = std::move(layout);
        typingAt.reset();
        if (const auto *box =
                focusLayout_->find(static_cast<std::uint32_t>(fieldId(focus)));
            box && box->textInput && box->contentRect.width > 0 &&
            box->contentRect.height > 0)
          typingAt = ui::toInputArea(box->contentRect, builtHeight);
        ++revision;
        builtFor = revision;
      }
    }
  }
  const auto projection =
      glm::ortho(0.0F, static_cast<float>(ctx.screenWidth), 0.0F,
                 static_cast<float>(ctx.screenHeight));
  canvas->draw(ctx.state, projection);
  headingCanvas->draw(ctx.state, projection);
}

} // namespace gleditor
