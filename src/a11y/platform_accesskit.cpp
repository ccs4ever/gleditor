/**
 * @file platform_accesskit.cpp
 * @brief Handing the described interface to AccessKit.
 *
 * AccessKit is the library that turns a description of a user interface into
 * whatever the platform's assistive technologies speak: UI Automation on
 * Windows, AT-SPI on X11 and Wayland, NSAccessibility on macOS. This is the
 * only file in the project that includes its header, and the only one that
 * knows the platforms apart.
 *
 * Four things are worth knowing before reading it.
 *
 * **One adapter serves X11 and Wayland.** AT-SPI is a D-Bus protocol. An
 * assistive technology on Linux talks to the accessibility bus and never to
 * the display server, so there is nothing to choose between the two here. The
 * one place the display server shows through is where the window is on screen,
 * which a Wayland client is not told; see setWindowBounds().
 *
 * **macOS is a third, wholly separate API.** NSAccessibility is neither UI
 * Automation nor a bus: an assistive technology asks the window's content
 * view directly, so AccessKit's macOS adapter subclasses that view the same
 * way the Windows one subclasses a window procedure -- dynamically, at run
 * time, because this program does not own the class SDL created. SDL also
 * places the keyboard focus on the *window* rather than the content view, so
 * open() additionally patches the window's class to forward
 * accessibilityFocusedUIElement down to the view; see
 * accesskit_macos_add_focus_forwarder_to_window_class_with_length() there.
 *
 * **AccessKit calls back from its own threads.** The AT-SPI adapter runs an
 * executor on one of its own, the Windows adapter is called from inside a
 * window procedure, and the macOS one from whatever thread NSAccessibility
 * queries on. The program on the other side of this file is an event loop
 * with a render thread, and handing it a call from an unknown thread would
 * mean locking everything it owns. So nothing is passed on: the tree is kept
 * here and rebuilt on demand, and actions are queued here and polled for.
 *
 * **The whole tree is kept, not the changes.** The platform adapters activate
 * lazily -- nothing is built until an assistive technology asks, which may be
 * an hour after the program started -- and what they ask for is a complete
 * tree. Keeping the last one is what makes that answerable without reaching
 * back across a thread boundary for it.
 */
#include <gleditor/a11y/platform.hpp> // IWYU pragma: associated

#include <deque>
#include <mutex>
#include <utility>

#include <accesskit.h>

namespace gleditor::a11y {

namespace {

/// AccessKit's role for one of ours. A switch rather than a cast, so that
/// reordering either enumeration is a compile error and not a user interface
/// described as something it is not.
accesskit_role roleOf(const Role role) {
  switch (role) {
  case Role::Window:
    return ACCESSKIT_ROLE_WINDOW;
  case Role::Group:
    return ACCESSKIT_ROLE_GROUP;
  case Role::Label:
    return ACCESSKIT_ROLE_LABEL;
  case Role::Document:
    return ACCESSKIT_ROLE_DOCUMENT;
  case Role::TextRun:
    return ACCESSKIT_ROLE_TEXT_RUN;
  case Role::TextInput:
    return ACCESSKIT_ROLE_TEXT_INPUT;
  case Role::MultilineTextInput:
    return ACCESSKIT_ROLE_MULTILINE_TEXT_INPUT;
  case Role::PasswordInput:
    return ACCESSKIT_ROLE_PASSWORD_INPUT;
  case Role::ComboBox:
    return ACCESSKIT_ROLE_COMBO_BOX;
  case Role::ListItem:
    return ACCESSKIT_ROLE_LIST_ITEM;
  case Role::Button:
    return ACCESSKIT_ROLE_BUTTON;
  case Role::Switch:
    return ACCESSKIT_ROLE_SWITCH;
  case Role::Dialog:
    return ACCESSKIT_ROLE_DIALOG;
  case Role::Log:
    return ACCESSKIT_ROLE_LOG;
  case Role::Link:
    return ACCESSKIT_ROLE_LINK;
  case Role::List:
    return ACCESSKIT_ROLE_LIST;
  }
  return ACCESSKIT_ROLE_UNKNOWN;
}

accesskit_action actionOf(const Action action) {
  switch (action) {
  case Action::Focus:
    return ACCESSKIT_ACTION_FOCUS;
  case Action::Click:
    return ACCESSKIT_ACTION_CLICK;
  case Action::SetValue:
    return ACCESSKIT_ACTION_SET_VALUE;
  case Action::ScrollIntoView:
    return ACCESSKIT_ACTION_SCROLL_INTO_VIEW;
  }
  return ACCESSKIT_ACTION_CLICK;
}

/// Ours for one of AccessKit's, for a request coming back. Nothing for the
/// many it can ask that this program has nothing to do about -- scrolling by a
/// page, expanding a tree item -- which the platforms are entitled to try.
std::optional<Action> actionFrom(const accesskit_action action) {
  switch (action) {
  case ACCESSKIT_ACTION_FOCUS:
    return Action::Focus;
  case ACCESSKIT_ACTION_CLICK:
    return Action::Click;
  case ACCESSKIT_ACTION_SET_VALUE:
    return Action::SetValue;
  case ACCESSKIT_ACTION_SCROLL_INTO_VIEW:
    return Action::ScrollIntoView;
  default:
    return std::nullopt;
  }
}

accesskit_rect rectOf(const Rect &rect) {
  return accesskit_rect{rect.left, rect.top, rect.right, rect.bottom};
}

/// Build one AccessKit node. Ownership passes to the caller, which hands it to
/// a tree update.
accesskit_node *nodeOf(const Node &node) {
  auto *const built = accesskit_node_new(roleOf(node.role));

  // The _with_length forms throughout: the strings are std::string and may
  // hold a NUL that somebody typed, and truncating a document at one would be
  // a silent loss of the rest of it.
  if (!node.label.empty()) {
    accesskit_node_set_label_with_length(built, node.label.data(),
                                         node.label.size());
  }
  if (!node.value.empty()) {
    accesskit_node_set_value_with_length(built, node.value.data(),
                                         node.value.size());
  }
  if (!node.description.empty()) {
    accesskit_node_set_description_with_length(built, node.description.data(),
                                               node.description.size());
  }
  if (!node.placeholder.empty()) {
    accesskit_node_set_placeholder_with_length(built, node.placeholder.data(),
                                               node.placeholder.size());
  }
  if (node.bounds) {
    accesskit_node_set_bounds(built, rectOf(*node.bounds));
  }
  for (const auto child : node.children) {
    accesskit_node_push_child(built, child);
  }
  for (const auto action : {Action::Focus, Action::Click, Action::SetValue,
                            Action::ScrollIntoView}) {
    if (0 != (node.actions & bit(action))) {
      accesskit_node_add_action(built, actionOf(action));
    }
  }
  // AccessKit has no separate focusable property: a node is focusable exactly
  // when it will accept being focused, which is what supporting the action
  // says.
  if (node.focusable) {
    accesskit_node_add_action(built, ACCESSKIT_ACTION_FOCUS);
  }
  if (node.readOnly) {
    accesskit_node_set_read_only(built);
  }
  if (node.modal) {
    accesskit_node_set_modal(built);
  }
  if (node.toggled) {
    accesskit_node_set_toggled(built, *node.toggled ? ACCESSKIT_TOGGLED_TRUE
                                                    : ACCESSKIT_TOGGLED_FALSE);
  }
  switch (node.live) {
  case Live::Polite:
    accesskit_node_set_live(built, ACCESSKIT_LIVE_POLITE);
    break;
  case Live::Assertive:
    accesskit_node_set_live(built, ACCESSKIT_LIVE_ASSERTIVE);
    break;
  case Live::Off:
    break;
  }
  if (!node.characterLengths.empty()) {
    accesskit_node_set_character_lengths(built, node.characterLengths.size(),
                                         node.characterLengths.data());
  }
  if (!node.wordStarts.empty()) {
    accesskit_node_set_word_starts(built, node.wordStarts.size(),
                                   node.wordStarts.data());
  }
  if (node.selection) {
    const accesskit_text_selection selection{
        accesskit_text_position{node.selection->anchor.node,
                                node.selection->anchor.character},
        accesskit_text_position{node.selection->focus.node,
                                node.selection->focus.character}};
    accesskit_node_set_text_selection(built, selection);
  }
  return built;
}

/**
 * @class AccessKitPlatform
 * @brief One window's connection to the assistive technologies.
 */
class AccessKitPlatform : public Platform {
public:
  AccessKitPlatform(std::string aToolkit, std::string aVersion)
      : toolkit(std::move(aToolkit)), version(std::move(aVersion)) {}

  ~AccessKitPlatform() override {
    if (nullptr == adapter) {
      return;
    }
#if defined(_WIN32)
    accesskit_windows_subclassing_adapter_free(adapter);
#elif defined(__APPLE__)
    accesskit_macos_subclassing_adapter_free(adapter);
#else
    accesskit_unix_adapter_free(adapter);
#endif
  }

  AccessKitPlatform(const AccessKitPlatform &)            = delete;
  AccessKitPlatform &operator=(const AccessKitPlatform &) = delete;
  AccessKitPlatform(AccessKitPlatform &&)                 = delete;
  AccessKitPlatform &operator=(AccessKitPlatform &&)      = delete;

  /// Open the platform's adapter. Separate from the constructor so that a
  /// failure is a null Platform rather than an exception through a C callback
  /// boundary.
  bool open(void *const nativeWindow) {
#if defined(_WIN32)
    if (nullptr == nativeWindow) {
      return false;
    }
    // The subclassing adapter installs itself on the window procedure to
    // answer WM_GETOBJECT, and it must be created before the window is first
    // shown. See Application::run(), which creates the window hidden for
    // exactly this reason.
    adapter = accesskit_windows_subclassing_adapter_new(
        static_cast<HWND>(nativeWindow), supplyTree, this, takeAction, this);
#elif defined(__APPLE__)
    if (nullptr == nativeWindow) {
      return false;
    }
    // SDL's NSWindow answers accessibilityFocusedUIElement itself, rather
    // than deferring to its content view the way a plain NSWindow does, so
    // whichever element AccessKit puts the focus on is never reached. This
    // patches SDL's window class -- once for the process, and safe to repeat
    // since it only ever installs the same method -- to forward that one
    // query down to the content view. The class differs between the two SDL
    // major versions this project builds against; both are SDL's own name,
    // not configurable and not looked up, so they are spelled out here.
#if GLEDITOR_SDL_MAJOR == 3
    static constexpr char windowClass[] = "SDL3Window";
#else
    static constexpr char windowClass[] = "SDLWindow";
#endif
    accesskit_macos_add_focus_forwarder_to_window_class_with_length(
        windowClass, sizeof(windowClass) - 1);
    // for_window rather than the plain view-taking constructor: it finds the
    // content view itself, which is what NSAccessibility actually queries.
    // Unlike the Windows adapter this needs no particular ordering against
    // the window being shown -- NSAccessibility asks the view directly rather
    // than through a message the window might have missed.
    adapter = accesskit_macos_subclassing_adapter_for_window(
        nativeWindow, supplyTree, this, takeAction, this);
#else
    // The window handle means nothing here: AT-SPI is a bus, and what it wants
    // is a name on that bus rather than a window to hang off.
    static_cast<void>(nativeWindow);
    adapter = accesskit_unix_adapter_new(supplyTree, this, takeAction, this,
                                         forgetTree, this);
#endif
    return nullptr != adapter;
  }

  void update(const Tree &tree) override {
    {
      const std::lock_guard locker(guard);
      kept = tree;
    }
    if (nullptr == adapter) {
      return;
    }
    // A factory rather than a tree: AccessKit asks for one only when something
    // is listening, so an editor nobody is running a screen reader against
    // pays for the copy above and nothing else.
#if defined(_WIN32)
    if (auto *const events =
            accesskit_windows_subclassing_adapter_update_if_active(
                adapter, supplyTree, this);
        nullptr != events) {
      // This is what actually delivers the events to the assistive technology.
      accesskit_windows_queued_events_raise(events);
    }
#elif defined(__APPLE__)
    if (auto *const events =
            accesskit_macos_subclassing_adapter_update_if_active(
                adapter, supplyTree, this);
        nullptr != events) {
      accesskit_macos_queued_events_raise(events);
    }
#else
    accesskit_unix_adapter_update_if_active(adapter, supplyTree, this);
#endif
  }

  void setWindowFocused(const bool focused) override {
    if (nullptr == adapter) {
      return;
    }
#if defined(_WIN32)
    // The subclassing adapter sees the window messages that say so, so there
    // is nothing to tell it.
    static_cast<void>(focused);
#elif defined(__APPLE__)
    if (auto *const events =
            accesskit_macos_subclassing_adapter_update_view_focus_state(
                adapter, focused);
        nullptr != events) {
      accesskit_macos_queued_events_raise(events);
    }
#else
    accesskit_unix_adapter_update_window_focus_state(adapter, focused);
#endif
  }

  void setWindowBounds(const Rect &outer, const Rect &inner) override {
    if (nullptr == adapter) {
      return;
    }
#if defined(_WIN32) || defined(__APPLE__)
    // Both take the window's position from the window itself: NSAccessibility
    // reads a view's frame from AppKit directly, the same way UI Automation
    // reads it from the HWND.
    static_cast<void>(outer);
    static_cast<void>(inner);
#else
    accesskit_unix_adapter_set_root_window_bounds(adapter, rectOf(outer),
                                                  rectOf(inner));
#endif
  }

  std::optional<ActionRequest> nextAction() override {
    const std::lock_guard locker(actionGuard);
    if (actions.empty()) {
      return std::nullopt;
    }
    auto next = std::move(actions.front());
    actions.pop_front();
    return next;
  }

private:
  /**
   * @brief Build a tree update from whatever was last described.
   *
   * Called by AccessKit on its own thread, for two different reasons -- an
   * assistive technology has just arrived and wants the whole tree, or an
   * update is being applied and it wants the nodes. Both want the same thing,
   * which is why one function answers both.
   *
   * Null before anything has been drawn. AccessKit puts a placeholder up and
   * asks again; an empty tree would be a lie that sticks.
   */
  static accesskit_tree_update *supplyTree(void *const opaque) {
    auto *const self = static_cast<AccessKitPlatform *>(opaque);
    const std::lock_guard locker(self->guard);
    if (self->kept.empty()) {
      return nullptr;
    }

    auto *const update = accesskit_tree_update_with_capacity_and_focus(
        self->kept.nodes.size(), self->kept.focus);
    auto *const tree = accesskit_tree_new(self->kept.root());
    accesskit_tree_set_toolkit_name_with_length(tree, self->toolkit.data(),
                                                self->toolkit.size());
    accesskit_tree_set_toolkit_version_with_length(tree, self->version.data(),
                                                   self->version.size());
    accesskit_tree_update_set_tree(update, tree);
    for (const auto &node : self->kept.nodes) {
      accesskit_tree_update_push_node(update, node.id, nodeOf(node));
    }
    return update;
  }

  /// Something was asked for. Called on AccessKit's thread; the event loop
  /// takes it from the queue and does it.
  static void takeAction(accesskit_action_request *const request,
                         void *const opaque) {
    auto *const self = static_cast<AccessKitPlatform *>(opaque);
    if (const auto action = actionFrom(request->action); action) {
      ActionRequest asked;
      asked.node   = request->target_node;
      asked.action = *action;
      if (request->data.has_value &&
          ACCESSKIT_ACTION_DATA_VALUE == request->data.value.tag &&
          nullptr != request->data.value.value) {
        asked.value = request->data.value.value;
      }

      const std::lock_guard locker(self->actionGuard);
      // Bounded, because nothing guarantees the event loop is still draining
      // this -- it is blocked for as long as a native message box is up.
      // Dropping the oldest is right: an action nobody carried out while a
      // dialog was open is one the person has given up on.
      constexpr std::size_t most = 64;
      while (self->actions.size() >= most) {
        self->actions.pop_front();
      }
      self->actions.push_back(std::move(asked));
    }
    // Ours to free either way, including for the actions above that this
    // program has nothing to do about.
    accesskit_action_request_free(request);
  }

  /**
   * @brief Every assistive technology has gone away.
   *
   * The kept tree is what makes the next one free, and it is one tree.
   * Dropping it would mean the next screen reader to start gets a placeholder
   * until something on screen next changes -- which, in an editor sitting
   * still, is whenever the person next types.
   */
  static void forgetTree(void * /*opaque*/) {}

#if defined(_WIN32)
  accesskit_windows_subclassing_adapter *adapter{};
#elif defined(__APPLE__)
  accesskit_macos_subclassing_adapter *adapter{};
#else
  accesskit_unix_adapter *adapter{};
#endif

  std::string toolkit;
  std::string version;

  /// The last description, and the queue of what has been asked of it. Two
  /// locks rather than one: an action arriving must not wait behind a screen
  /// reader reading the whole tree.
  std::mutex guard;
  Tree kept;
  std::mutex actionGuard;
  std::deque<ActionRequest> actions;
};

} // namespace

std::unique_ptr<Platform> openPlatform(void *const nativeWindow,
                                       const std::string &name,
                                       const std::string &toolkit,
                                       const std::string &version) {
  // The name reaches the assistive technology as the application's, which
  // AccessKit takes from the process rather than from us on every platform it
  // has an adapter for. Kept in the signature because that is not true of all
  // of them and because a caller should not have to know which.
  static_cast<void>(name);

  auto platform = std::make_unique<AccessKitPlatform>(toolkit, version);
  if (!platform->open(nativeWindow)) {
    return nullptr;
  }
  return platform;
}

bool platformAvailable() { return true; }

} // namespace gleditor::a11y
