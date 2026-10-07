/**
 * @file publisher.cpp
 * @brief Implementation of the collect-and-send half of accessibility.
 */
#include <gleditor/a11y/publisher.hpp> // IWYU pragma: associated

#include <algorithm>
#include <limits>
#include <sstream>
#include <tuple>
#include <unordered_set>
#include <utility>

#include <gleditor/a11y/platform.hpp>
#include <gleditor/ui/focus_manager.hpp>

namespace gleditor::a11y {

namespace {

const char *roleName(const Role role) {
  switch (role) {
  case Role::Window:
    return "window";
  case Role::Group:
    return "group";
  case Role::Label:
    return "label";
  case Role::Document:
    return "document";
  case Role::TextRun:
    return "text run";
  case Role::TextInput:
    return "text input";
  case Role::MultilineTextInput:
    return "multiline text input";
  case Role::PasswordInput:
    return "password input";
  case Role::ComboBox:
    return "combo box";
  case Role::ListItem:
    return "list item";
  case Role::Button:
    return "button";
  case Role::Switch:
    return "switch";
  case Role::Dialog:
    return "dialog";
  case Role::Log:
    return "log";
  case Role::Link:
    return "link";
  case Role::List:
    return "list";
  }
  return "?";
}

/// One line of describe(), without the children.
void describeNode(std::ostringstream &out, const Node &node,
                  const std::size_t depth) {
  out << std::string(depth * 2, ' ') << roleName(node.role);
  if (!node.label.empty()) {
    out << " \"" << node.label << "\"";
  }
  if (!node.value.empty()) {
    out << " = \"" << node.value << "\"";
  }
  if (!node.placeholder.empty()) {
    out << " (" << node.placeholder << ")";
  }
  if (node.toggled) {
    out << (*node.toggled ? " [on]" : " [off]");
  }
  if (node.modal) {
    out << " [modal]";
  }
  if (node.readOnly) {
    out << " [read only]";
  }
  if (Live::Off != node.live) {
    out << (Live::Assertive == node.live ? " [assertive]" : " [polite]");
  }
  if (node.selection) {
    out << " [caret " << node.selection->focus.character;
    if (node.selection->anchor != node.selection->focus) {
      out << " from " << node.selection->anchor.character;
    }
    out << "]";
  }
  out << "\n";
}

void describeFrom(std::ostringstream &out, const Tree &tree,
                  const std::uint64_t id, const std::size_t depth) {
  const auto node = tree.find(id);
  if (!node) {
    return;
  }
  describeNode(out, *node, depth);
  for (const auto child : node->children) {
    describeFrom(out, tree, child, depth + 1);
  }
}

std::unordered_set<std::uint64_t>
descendants(const Tree &tree, std::vector<std::uint64_t> pending) {
  std::unordered_set<std::uint64_t> result;
  while (!pending.empty()) {
    const auto id = pending.back();
    pending.pop_back();
    const auto node = tree.find(id);
    if (!node || !result.insert(id).second) continue;
    pending.insert(pending.end(), node->children.begin(), node->children.end());
  }
  return result;
}

Node *mutableNode(Tree &tree, std::uint64_t id) {
  const auto found = std::ranges::find(tree.nodes, id, &Node::id);
  return found == tree.nodes.end() ? nullptr : &*found;
}

} // namespace

Publisher::Publisher(std::string aName, std::string aToolkit,
                     std::string aVersion, std::unique_ptr<Platform> aPlatform)
    : name(std::move(aName)), toolkit(std::move(aToolkit)),
      version(std::move(aVersion)), platform(std::move(aPlatform)),
      windowTitle(name) {}

Publisher::~Publisher() = default;

void Publisher::addSource(Source *source,
                          const std::optional<std::uint16_t> owner) {
  if (source) registerSource(Registered{.source = source}, owner);
}

void Publisher::addSource(std::shared_ptr<Source> source,
                          const std::optional<std::uint16_t> owner) {
  if (!source) return;
  auto *pointer = source.get();
  registerSource(Registered{.source = pointer, .lifetime = std::move(source)},
                 owner);
}

void Publisher::registerSource(Registered registration,
                               const std::optional<std::uint16_t> owner) {
  const std::scoped_lock locker(sourcesGuard);
  registration.owner = owner.value_or(nextOwner);
  sources.push_back(std::move(registration));
  if (!owner) ++nextOwner;
  // Kept in owner order rather than registration order, because the two differ
  // and the tree should follow the first. A program registers its elements
  // before the render thread has started, and the library registers the
  // documents and the notification overlay once there is a device to build
  // them on -- which is afterwards. Describing the chrome before the thing it
  // is chrome for would be an odd way round for anybody moving through the
  // window.
  std::ranges::stable_sort(sources, {}, &Registered::owner);
  registeredSnapshot = std::make_shared<const std::vector<Registered>>(sources);
  ++registrationRevision;
}

void Publisher::removeSource(Source *const source) {
  // The last shared lease may destroy a source, whose destructor can itself
  // unregister children. Release the prior snapshot outside the registry lock.
  std::shared_ptr<const std::vector<Registered>> prior;
  {
    const std::scoped_lock locker(sourcesGuard);
    prior = std::move(registeredSnapshot);
    std::erase_if(sources, [source](const Registered &reg) {
      return reg.source == source;
    });
    registeredSnapshot =
        std::make_shared<const std::vector<Registered>>(sources);
    ++registrationRevision;
  }
}

void Publisher::setFocusManager(ui::FocusManager *manager) {
  const std::scoped_lock lock(guard);
  focusManager = manager;
  everBuilt    = false;
}

void Publisher::setToolkit(std::string aToolkit, std::string aVersion) {
  toolkit = std::move(aToolkit);
  version = std::move(aVersion);
}

bool Publisher::start(void *const nativeWindow) {
  if (nullptr != platform) {
    return true;
  }
  platform = openPlatform(nativeWindow, name, toolkit, version);
  return nullptr != platform;
}

void Publisher::setWindowTitle(std::string title) {
  const std::scoped_lock locker(guard);
  windowTitle = std::move(title);
  // Forces the next rebuild to happen even if no source moved.
  everBuilt = false;
}

void Publisher::rebuild(const int width, const int height) {
  ui::FocusManager *manager;
  {
    const std::scoped_lock lock(guard);
    manager = focusManager;
  }
  // Refresh can notify scopes, which can register sources or inspect this
  // publisher. Keep it outside both publisher locks.
  const auto focus = manager ? manager->focusSnapshot() : ui::FocusSnapshot{};
  auto *focusedSource =
      focus.scope ? dynamic_cast<Source *>(focus.scope) : nullptr;
  std::shared_ptr<const std::vector<Registered>> registered;
  std::uint64_t registrations;
  {
    const std::scoped_lock lock(sourcesGuard);
    registered    = registeredSnapshot;
    registrations = registrationRevision;
  }
  std::uint64_t revision = 0;
  for (const auto &registration : *registered) {
    revision += registration.source->accessibilityRevision();
  }

  {
    const std::scoped_lock locker(guard);
    if (everBuilt && revision == builtFrom &&
        registrations == builtRegistrations && focus.revision == builtFocus &&
        width == builtWidth && height == builtHeight) {
      return;
    }
  }

  Tree tree;
  // The root is made first so that it is the first node, and filled in last:
  // its children are whatever the sources turn out to contribute. Held by
  // index rather than by reference, because adding nodes moves the vector.
  Builder rootBuilder(tree, Ids::window);
  std::ignore          = rootBuilder.add(0, Role::Window);
  const auto rootIndex = tree.nodes.size() - 1;
  tree.focus           = tree.nodes[rootIndex].id;

  std::vector<std::uint64_t> topLevel;
  std::vector<std::uint64_t> focusedRoots;
  std::optional<std::uint16_t> focusedOwner;
  std::optional<std::uint64_t> sourceFocus;
  for (const auto &registration : *registered) {
    const auto source = registration.source;
    const auto owner  = registration.owner;
    Builder builder(tree, owner);
    source->describe(builder);
    topLevel.insert(topLevel.end(), builder.roots().begin(),
                    builder.roots().end());
    if (source == focusedSource) {
      focusedOwner = owner;
      focusedRoots.assign(builder.roots().begin(), builder.roots().end());
      if (Ids::ownerOf(tree.focus) == owner) sourceFocus = tree.focus;
    }
  }

  if (manager) {
    for (auto &node : tree.nodes) node.modal = false;
    std::optional<std::uint64_t> dialog;
    if (focus.modal) {
      if (focusedRoots.size() == 1 && tree.find(focusedRoots.front())) {
        dialog      = focusedRoots.front();
        auto *node  = mutableNode(tree, *dialog);
        node->role  = Role::Dialog;
        node->modal = true;
      } else {
        std::uint64_t local = 1;
        while (tree.find(Ids::of(Ids::window, local))) ++local;
        Builder wrapper(tree, Ids::window);
        auto &node     = wrapper.add(local, Role::Dialog);
        node.label     = "Dialog";
        node.modal     = true;
        node.focusable = true;
        node.children  = focusedRoots;
        dialog         = node.id;
        if (focus.scope) {
          auto area = focus.scope->pointerArea();
          if (!area) area = focus.scope->textArea();
          if (area)
            node.bounds =
                Rect{static_cast<double>(area->x), static_cast<double>(area->y),
                     static_cast<double>(area->x + area->width),
                     static_cast<double>(area->y + area->height)};
        }
        std::erase_if(topLevel, [&](auto id) {
          return std::ranges::find(focusedRoots, id) != focusedRoots.end();
        });
        topLevel.push_back(*dialog);
      }
    }
    const auto owned = descendants(
        tree, dialog ? std::vector<std::uint64_t>{*dialog} : focusedRoots);
    const auto layout = focus.scope ? focus.scope->focusLayout() : nullptr;
    if (layout) {
      for (auto &node : tree.nodes) {
        if (!owned.contains(node.id) || !node.focusable ||
            Ids::localOf(node.id) > std::numeric_limits<std::uint32_t>::max())
          continue;
        const auto *box =
            layout->find(static_cast<std::uint32_t>(Ids::localOf(node.id)));
        if (box && box->enabled && box->focusable)
          node.actions |= bit(Action::Focus);
      }
    }
    std::optional<std::uint64_t> selected;
    if (focusedOwner && focus.node) {
      const auto node = Ids::of(*focusedOwner, *focus.node);
      if (owned.contains(node)) selected = node;
    }
    if (!selected && sourceFocus && owned.contains(*sourceFocus))
      selected = sourceFocus;
    if (focus.modal) {
      for (auto &node : tree.nodes) {
        if (!owned.contains(node.id)) {
          node.focusable = false;
          node.actions   = 0;
        } else if (!selected && node.focusable && node.id != *dialog) {
          selected = node.id;
        }
      }
      tree.focus = selected.value_or(*dialog);
    } else if (selected) {
      tree.focus = *selected;
    }
  }

  {
    const std::scoped_lock locker(guard);
    auto &root         = tree.nodes[rootIndex];
    root.label         = windowTitle;
    root.children      = std::move(topLevel);
    root.bounds        = Rect{.left   = 0.0,
                              .top    = 0.0,
                              .right  = static_cast<double>(width),
                              .bottom = static_cast<double>(height)};
    built              = std::move(tree);
    builtFrom          = revision;
    builtRegistrations = registrations;
    builtFocus         = focus.revision;
    builtWidth         = width;
    builtHeight        = height;
    everBuilt          = true;
  }
}

void Publisher::publish() {
  Tree current;
  {
    const std::scoped_lock locker(guard);
    if (built.empty() || built == sent) {
      return;
    }
    current = built;
    sent    = built;
  }
  if (nullptr != platform) {
    platform->update(current);
  }
}

std::size_t Publisher::pumpActions() {
  if (nullptr == platform) {
    return 0;
  }
  std::size_t done = 0;
  while (const auto asked = platform->nextAction()) {
    ui::FocusManager *manager;
    {
      const std::scoped_lock lock(guard);
      manager = focusManager;
    }
    const auto focus = manager ? manager->focusSnapshot() : ui::FocusSnapshot{};
    const auto owner = Ids::ownerOf(asked->node);
    Source *source{};
    std::shared_ptr<Source> lifetime;
    {
      const std::scoped_lock lock(sourcesGuard);
      const auto found = std::ranges::find(sources, owner, &Registered::owner);
      if (found != sources.end()) {
        source   = found->source;
        lifetime = found->lifetime;
      }
    }
    if (!source) {
      // A node whose owner has since been unregistered, or one the platform
      // remembered across a rebuild. Not an error: the request is simply about
      // something that is no longer there.
      continue;
    }
    if (manager) {
      auto *focusedSource =
          focus.scope ? dynamic_cast<Source *>(focus.scope) : nullptr;
      if (focus.modal && source != focusedSource) continue;
      const auto tree = snapshot();
      const auto node = tree.find(asked->node);
      if (!node || (node->actions & bit(asked->action)) == 0) continue;
      if (focus.modal) {
        std::vector<std::uint64_t> roots;
        if (const auto root = tree.find(tree.root())) {
          for (auto id : root->children)
            if (Ids::ownerOf(id) == owner) roots.push_back(id);
        }
        // A manager wrapper can group multiple roots from this namespace.
        for (const auto &candidate : tree.nodes)
          if (candidate.modal && Ids::ownerOf(candidate.id) == Ids::window)
            for (auto id : candidate.children)
              if (Ids::ownerOf(id) == owner) roots.push_back(id);
        if (!descendants(tree, std::move(roots)).contains(asked->node))
          continue;
      }
    }
    const bool tookFocus = manager && asked->action == Action::Focus &&
                           source == dynamic_cast<Source *>(focus.scope) &&
                           Ids::localOf(asked->node) <=
                               std::numeric_limits<std::uint32_t>::max() &&
                           manager->focusNode(static_cast<std::uint32_t>(
                               Ids::localOf(asked->node)));
    if (source->performAction(asked->node, asked->action, asked->value) ||
        tookFocus) {
      done++;
    }
  }
  return done;
}

void Publisher::setWindowFocused(const bool focused) {
  if (nullptr != platform) {
    platform->setWindowFocused(focused);
  }
}

void Publisher::setWindowBounds(const Rect &outer, const Rect &inner) {
  if (nullptr != platform) {
    platform->setWindowBounds(outer, inner);
  }
}

Tree Publisher::snapshot() const {
  const std::scoped_lock locker(guard);
  return built;
}

std::string Publisher::describe(const Tree &tree) {
  std::ostringstream out;
  if (tree.empty()) {
    return "(nothing)\n";
  }
  describeFrom(out, tree, tree.root(), 0);
  const auto focused     = tree.find(tree.focus);
  std::string focusLabel = "(none)";
  if (focused.has_value()) {
    focusLabel =
        focused->label.empty() ? roleName(focused->role) : focused->label;
  }
  out << "focus: " << focusLabel << "\n";
  return out.str();
}

} // namespace gleditor::a11y
