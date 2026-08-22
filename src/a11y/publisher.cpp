/**
 * @file publisher.cpp
 * @brief Implementation of the collect-and-send half of accessibility.
 */
#include <gleditor/a11y/publisher.hpp> // IWYU pragma: associated

#include <algorithm>
#include <sstream>
#include <utility>

#include <gleditor/a11y/platform.hpp>

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
    // Truncated: a document's value is the whole document, and this is meant
    // to be read.
    constexpr std::size_t most = 60;
    out << " = \""
        << (node.value.size() > most ? node.value.substr(0, most) + "..."
                                     : node.value)
        << "\"";
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
  const auto *const node = tree.find(id);
  if (nullptr == node) {
    return;
  }
  describeNode(out, *node, depth);
  for (const auto child : node->children) {
    describeFrom(out, tree, child, depth + 1);
  }
}

} // namespace

Publisher::Publisher(std::string aName, std::string aToolkit,
                     std::string aVersion)
    : name(std::move(aName)), toolkit(std::move(aToolkit)),
      version(std::move(aVersion)), windowTitle(name) {}

Publisher::~Publisher() = default;

void Publisher::addSource(Source *const source,
                          const std::optional<std::uint16_t> owner) {
  if (nullptr == source) {
    return;
  }
  const std::scoped_lock locker(sourcesGuard);
  sources.push_back(Registered{source, owner.value_or(nextOwner)});
  if (!owner) {
    nextOwner++;
  }
  // Kept in owner order rather than registration order, because the two differ
  // and the tree should follow the first. A program registers its elements
  // before the render thread has started, and the library registers the
  // documents and the notification overlay once there is a device to build
  // them on -- which is afterwards. Describing the chrome before the thing it
  // is chrome for would be an odd way round for anybody moving through the
  // window.
  std::ranges::stable_sort(sources, {}, &Registered::owner);
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
  const std::scoped_lock registered(sourcesGuard);
  std::uint64_t revision = 0;
  for (const auto &[source, owner] : sources) {
    revision += source->accessibilityRevision();
  }

  {
    const std::scoped_lock locker(guard);
    if (everBuilt && revision == builtFrom) {
      return;
    }
  }

  Tree tree;
  // The root is made first so that it is the first node, and filled in last:
  // its children are whatever the sources turn out to contribute. Held by
  // index rather than by reference, because adding nodes moves the vector.
  Builder rootBuilder(tree, Ids::window);
  static_cast<void>(rootBuilder.add(0, Role::Window));
  const auto rootIndex = tree.nodes.size() - 1;
  tree.focus           = tree.nodes[rootIndex].id;

  std::vector<std::uint64_t> topLevel;
  for (const auto &[source, owner] : sources) {
    Builder builder(tree, owner);
    source->describe(builder);
    topLevel.insert(topLevel.end(), builder.roots().begin(),
                    builder.roots().end());
  }

  {
    const std::scoped_lock locker(guard);
    auto &root    = tree.nodes[rootIndex];
    root.label    = windowTitle;
    root.children = std::move(topLevel);
    root.bounds =
        Rect{0.0, 0.0, static_cast<double>(width), static_cast<double>(height)};
    built     = std::move(tree);
    builtFrom = revision;
    everBuilt = true;
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
  const std::scoped_lock registered(sourcesGuard);
  std::size_t done = 0;
  while (const auto asked = platform->nextAction()) {
    const auto owner = Ids::ownerOf(asked->node);
    const auto found = std::ranges::find(sources, owner, &Registered::owner);
    if (sources.end() == found) {
      // A node whose owner has since been unregistered, or one the platform
      // remembered across a rebuild. Not an error: the request is simply about
      // something that is no longer there.
      continue;
    }
    if (found->source->performAction(asked->node, asked->action,
                                     asked->value)) {
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
  const auto *const focused = tree.find(tree.focus);
  out << "focus: "
      << (nullptr == focused ? "(none)"
                             : (focused->label.empty() ? roleName(focused->role)
                                                       : focused->label))
      << "\n";
  return out.str();
}

} // namespace gleditor::a11y
