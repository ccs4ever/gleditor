// A compiled Platform smoke fixture: no SDL or rendering dependencies can hide
// whether the C++ description actually reaches the browser, and actions return.
#include <gleditor/a11y/platform.hpp>

#include <cstdint>
#include <limits>
#include <memory>

#include <emscripten.h>

namespace {
std::unique_ptr<gleditor::a11y::Platform> platform;
constexpr auto fieldId = std::numeric_limits<std::uint64_t>::max();
} // namespace

extern "C" {
EMSCRIPTEN_KEEPALIVE int a11y_take_action() {
  const auto request = platform->nextAction();
  if (!request) {
    return 0;
  }
  const auto id = std::to_string(request->node);
  MAIN_THREAD_EM_ASM(
      {
        Module.lastAction        = {};
        Module.lastAction.node   = UTF8ToString($0);
        Module.lastAction.action = $1;
        Module.lastAction.value  = UTF8ToString($2);
      },
      id.c_str(), static_cast<int>(request->action), request->value.c_str());
  return 1;
}
}

int main() {
  using namespace gleditor::a11y;
  platform = openPlatform(nullptr, "Browser fixture", "gleditor", "test");
  if (!platform || !platformAvailable()) {
    return 1;
  }
  Tree tree;
  Node root;
  root.role     = Role::Window;
  root.label    = "Browser fixture";
  root.children = {fieldId};
  tree.nodes.push_back(root);
  Node input;
  input.id          = fieldId;
  input.role        = Role::MultilineTextInput;
  input.label       = "Compiled text";
  input.value       = "A😀B\nsecond";
  input.description = "C++ tree delivered through the browser adapter";
  input.actions     = bit(Action::Focus) | bit(Action::SetValue);
  input.focusable   = true;
  tree.nodes.push_back(input);
  tree.focus = fieldId;
  platform->update(tree);
  return 0;
}
