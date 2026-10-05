/**
 * @file platform_web.cpp
 * @brief Delivering the shared accessibility tree to the browser's native DOM.
 */
#include <gleditor/a11y/platform.hpp> // IWYU pragma: associated

#include <charconv>
#include <sstream>
#include <string_view>
#include <utility>

#include <emscripten.h>

EM_JS_DEPS(gleditor_web_a11y, "$UTF8ToString,$lengthBytesUTF8,$stringToUTF8");

namespace gleditor::a11y {
namespace {

// IDs carry an owner in their high bits, beyond JavaScript's exact integer
// range. Strings preserve their identity on both sides of the bridge.
void string(std::ostream &out, const std::string_view value) {
  constexpr std::string_view hex = "0123456789abcdef";
  out << '"';
  for (const unsigned char character : value) {
    if (character == '"' || character == '\\') {
      out << '\\' << character;
    } else if (character < 0x20) {
      out << "\\u00" << hex[character >> 4U] << hex[character & 0xfU];
    } else {
      out << character;
    }
  }
  out << '"';
}

std::string_view roleOf(const Role role) {
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
    return "textRun";
  case Role::TextInput:
    return "textInput";
  case Role::MultilineTextInput:
    return "multilineTextInput";
  case Role::PasswordInput:
    return "passwordInput";
  case Role::ComboBox:
    return "comboBox";
  case Role::ListItem:
    return "listItem";
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
  return "group";
}

std::string serialize(const Tree &tree) {
  std::ostringstream out;
  out << "{\"focus\":\"" << tree.focus << "\",\"nodes\":[";
  bool first = true;
  for (const auto &node : tree.nodes) {
    if (!first) {
      out << ',';
    }
    first = false;
    out << "{\"id\":\"" << node.id << "\",\"role\":";
    string(out, roleOf(node.role));
    for (const auto &[name, value] :
         {std::pair{"label", std::string_view(node.label)},
          std::pair{"value", std::string_view(node.value)},
          std::pair{"description", std::string_view(node.description)},
          std::pair{"placeholder", std::string_view(node.placeholder)}}) {
      out << ",\"" << name << "\":";
      string(out, value);
    }
    out << ",\"actions\":" << node.actions
        << ",\"focusable\":" << (node.focusable ? "true" : "false")
        << ",\"readOnly\":" << (node.readOnly ? "true" : "false")
        << ",\"modal\":" << (node.modal ? "true" : "false")
        << ",\"live\":" << static_cast<unsigned>(node.live);
    if (node.toggled) {
      out << ",\"toggled\":" << (*node.toggled ? "true" : "false");
    }
    if (node.selection) {
      const auto &selection = *node.selection;
      out << ",\"selection\":{\"anchor\":{\"node\":\"" << selection.anchor.node
          << "\",\"character\":" << selection.anchor.character
          << "},\"focus\":{\"node\":\"" << selection.focus.node
          << "\",\"character\":" << selection.focus.character << "}}";
    }
    out << ",\"children\":[";
    for (std::size_t index = 0; index < node.children.size(); ++index) {
      if (index != 0) {
        out << ',';
      }
      out << '"' << node.children[index] << '"';
    }
    out << "]}";
  }
  out << "]}";
  return out.str();
}

class WebPlatform final : public Platform {
public:
  ~WebPlatform() override {
    MAIN_THREAD_EM_ASM({ Module.gleditorAccessibility.close(); });
  }

  void update(const Tree &tree) override {
    const auto encoded = serialize(tree);
    MAIN_THREAD_EM_ASM(
        { Module.gleditorAccessibility.update(JSON.parse(UTF8ToString($0))); },
        encoded.c_str());
  }

  void setWindowFocused(const bool focused) override {
    MAIN_THREAD_EM_ASM(
        { Module.gleditorAccessibility.setFocused(Boolean($0)); }, focused);
  }

  void setWindowBounds(const Rect &, const Rect &) override {
    // Browser geometry belongs to the canvas element, not desktop coordinates.
  }

  std::optional<ActionRequest> nextAction() override {
    const int length = MAIN_THREAD_EM_ASM_INT({
      const action = Module.gleditorAccessibility.nextAction();
      Module.gleditorAccessibilityAction =
          action ? action.node + '\n' + action.action + '\n' + action.value
                 : "";
      return action ? lengthBytesUTF8(Module.gleditorAccessibilityAction) + 1
                    : 0;
    });
    if (length <= 0) {
      return {};
    }
    std::string request(static_cast<std::size_t>(length), '\0');
    MAIN_THREAD_EM_ASM(
        {
          stringToUTF8(Module.gleditorAccessibilityAction, $0, $1);
          delete Module.gleditorAccessibilityAction;
        },
        request.data(), length);
    request.pop_back();
    const auto split = request.find('\n');
    if (split == std::string::npos || split + 2 >= request.size()) {
      return {};
    }
    ActionRequest result;
    const auto parsed =
        std::from_chars(request.data(), request.data() + split, result.node);
    if (parsed.ec != std::errc{} || parsed.ptr != request.data() + split ||
        request[split + 2] != '\n' || request[split + 1] < '0' ||
        request[split + 1] > '3') {
      return {};
    }
    result.action = static_cast<Action>(request[split + 1] - '0');
    result.value  = request.substr(split + 3);
    return result;
  }
};

} // namespace

std::unique_ptr<Platform> openPlatform(void *, const std::string &,
                                       const std::string &,
                                       const std::string &) {
  const bool available = MAIN_THREAD_EM_ASM_INT({
    return typeof document != 'undefined' && !!Module.gleditorAccessibility;
  });
  return available ? std::make_unique<WebPlatform>() : nullptr;
}

bool platformAvailable() { return true; }

} // namespace gleditor::a11y
