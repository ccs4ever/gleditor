/**
 * @file app.cpp
 * @brief Implementation of the window, event loop and shared command line.
 */
#include <gleditor/app.hpp> // IWYU pragma: associated

#include <algorithm>
#include <array>
#include <clocale>
#include <cstdint>
#include <functional>
#include <iostream>
#include <locale>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <argparse/argparse.hpp>
#include <glibmm/init.h>
#include <pangomm/wrap_init.h>
#include <glm/ext/vector_float3.hpp>
#include <glm/geometric.hpp>

#include <gleditor/paths.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render/diagnostics.hpp>
#include <gleditor/sdl_compat.hpp>
#include <gleditor/sdl_wrap.hpp>

namespace gleditor {

namespace {

/// The library's modifier mask for whatever SDL currently reports held.
Mod modsFromSdl(const std::uint16_t sdlMods) {
  auto mods = Mod::None;
  if (0 != (sdlMods & SDL_KMOD_SHIFT)) {
    mods = mods | Mod::Shift;
  }
  if (0 != (sdlMods & SDL_KMOD_CTRL)) {
    mods = mods | Mod::Ctrl;
  }
  if (0 != (sdlMods & SDL_KMOD_ALT)) {
    mods = mods | Mod::Alt;
  }
  return mods;
}

/// A key by the name a script writes it under, with whatever it is held with.
std::optional<std::pair<gleditor::Key, gleditor::KeyMods>>
keyNamed(std::string_view name) {
  using gleditor::Key;
  using gleditor::KeyMods;
  auto mods = KeyMods::None;
  if (name.starts_with("shift-")) {
    mods = KeyMods::Shift;
    name.remove_prefix(6);
  }
  static constexpr std::array<std::pair<std::string_view, Key>, 11> named = {{
      {"escape", Key::Escape},
      {"enter", Key::Return},
      {"tab", Key::Tab},
      {"backspace", Key::Backspace},
      {"delete", Key::Delete},
      {"left", Key::Left},
      {"right", Key::Right},
      {"up", Key::Up},
      {"down", Key::Down},
      {"home", Key::Home},
      {"end", Key::End},
  }};
  for (const auto &[spelling, key] : named) {
    if (spelling == name) {
      return std::pair{key, mods};
    }
  }
  return std::nullopt;
}

/// The modal keys, for whatever SDL reports. Everything else is either text,
/// which arrives composed as its own event, or nothing a form has a use for.
std::optional<gleditor::Key> modalKey(const int scancode) {
  switch (scancode) {
  case SDL_SCANCODE_ESCAPE:
    return gleditor::Key::Escape;
  case SDL_SCANCODE_RETURN:
  case SDL_SCANCODE_KP_ENTER:
    return gleditor::Key::Return;
  case SDL_SCANCODE_TAB:
    return gleditor::Key::Tab;
  case SDL_SCANCODE_BACKSPACE:
    return gleditor::Key::Backspace;
  case SDL_SCANCODE_DELETE:
    return gleditor::Key::Delete;
  case SDL_SCANCODE_LEFT:
    return gleditor::Key::Left;
  case SDL_SCANCODE_RIGHT:
    return gleditor::Key::Right;
  case SDL_SCANCODE_UP:
    return gleditor::Key::Up;
  case SDL_SCANCODE_DOWN:
    return gleditor::Key::Down;
  case SDL_SCANCODE_HOME:
    return gleditor::Key::Home;
  case SDL_SCANCODE_END:
    return gleditor::Key::End;
  default:
    return std::nullopt;
  }
}

/// The same modifier mask, in the spelling a modal is written against.
gleditor::KeyMods modalMods(const Mod mods) {
  return static_cast<gleditor::KeyMods>(static_cast<std::uint16_t>(mods));
}

/// Split a `--toast` argument into its severity and its text. An unprefixed
/// argument is a warning, which is what a message worth showing but not worth
/// stopping for amounts to.
std::pair<render::DiagnosticSeverity, std::string>
parseToast(const std::string &argument) {
  using render::DiagnosticSeverity;
  static const std::array<std::pair<std::string_view, DiagnosticSeverity>, 3>
      prefixes = {std::pair{"info:", DiagnosticSeverity::Info},
                  std::pair{"warning:", DiagnosticSeverity::Warning},
                  std::pair{"error:", DiagnosticSeverity::Error}};

  for (const auto &[prefix, severity] : prefixes) {
    if (argument.starts_with(prefix)) {
      return {severity, argument.substr(prefix.size())};
    }
  }
  return {DiagnosticSeverity::Warning, argument};
}

/// Parse an "X,Y" pair, naming the option in the complaint when it is not one.
std::pair<int, int> parsePair(const std::string &value,
                              const std::string_view option,
                              const std::string_view shape) {
  const auto comma = value.find(',');
  if (std::string::npos == comma) {
    throw std::runtime_error(std::string{option} + " expects " +
                             std::string{shape} + ", got: " + value);
  }
  return {std::stoi(value.substr(0, comma)),
          std::stoi(value.substr(comma + 1))};
}

} // namespace

/**
 * @brief Read the automation options out of the command line, in order.
 *
 * Not from the parser: argparse collects each option's values into its own
 * list, which is exactly the ordering this needs and does not keep. What a
 * test wants to say is "click here, type this, click there, type that", and
 * that is a sequence rather than two clicks and two strings.
 *
 * The parser has already accepted these arguments by the time this runs, so
 * this is reading a command line known to be well formed rather than parsing
 * one: what is left is to note which option each value belonged to and in what
 * order. Both spellings are accepted, since argparse accepts both.
 */
std::vector<AppState::AutomationStep>
readAutomationScript(const int argc, const char *const *const argv) {
  using Step = AppState::AutomationStep;
  std::vector<Step> script;
  if (nullptr == argv) {
    return script;
  }

  const auto add = [&script](const std::string_view option,
                             const std::string &value) {
    if ("--pick" == option || "--click" == option) {
      const auto [x, y] = parsePair(value, option, "X,Y");
      Step step{"--pick" == option ? Step::Kind::Pick : Step::Kind::Click};
      step.x = x;
      step.y = y;
      script.push_back(std::move(step));
    } else if ("--type" == option) {
      Step step{Step::Kind::Type};
      step.text = value;
      script.push_back(std::move(step));
    } else if ("--do" == option) {
      Step step{Step::Kind::Command};
      step.text = value;
      script.push_back(std::move(step));
    } else if ("--key" == option) {
      Step step{Step::Kind::Press};
      if (const auto named = keyNamed(value); named) {
        step.key  = named->first;
        step.mods = named->second;
        script.push_back(std::move(step));
      } else {
        std::cerr << "--key: no key called \"" << value << "\"\n";
      }
    } else if ("--select" == option) {
      const auto [from, to] = parsePair(value, option, "START,END");
      Step step{Step::Kind::Select};
      step.from = static_cast<std::uint32_t>(from);
      step.to   = static_cast<std::uint32_t>(to);
      script.push_back(std::move(step));
    }
  };

  static constexpr std::array scripted = {"--pick", "--click", "--type",
                                          "--select", "--do", "--key"};
  for (int i = 1; i < argc; i++) {
    if (nullptr == argv[i]) {
      continue;
    }
    const std::string_view arg{argv[i]};
    for (const auto *const option : scripted) {
      if (arg == option) {
        // "--click 3,4": the value is the argument after it.
        if (i + 1 < argc && nullptr != argv[i + 1]) {
          add(option, std::string{argv[i + 1]});
          i++;
        }
        break;
      }
      const std::string_view joined{option};
      if (arg.starts_with(joined) && arg.size() > joined.size() &&
          '=' == arg[joined.size()]) {
        // "--click=3,4": the value is the rest of this argument.
        add(option, std::string{arg.substr(joined.size() + 1)});
        break;
      }
    }
  }
  return script;
}


void CommandTable::bind(const int scancode, const Mod mods, std::string name,
                        std::string help, std::function<void()> run) {
  bindings.push_back(Command{scancode, mods, std::move(name), std::move(help),
                             std::move(run)});
}

bool CommandTable::run(const std::string_view name) const {
  const auto found = std::ranges::find_if(
      bindings, [name](const Command &one) { return one.name == name; });
  if (bindings.end() == found) {
    return false;
  }
  found->run();
  return true;
}

bool CommandTable::dispatch(const int scancode, const Mod mods) const {
  const auto found = std::ranges::find_if(bindings, [&](const Command &cmd) {
    return cmd.scancode == scancode && cmd.mods == mods;
  });
  if (found == bindings.end() || !found->run) {
    return false;
  }
  found->run();
  return true;
}

std::string CommandTable::helpText() const {
  std::ostringstream out;
  for (const auto &cmd : bindings) {
    out << "  " << cmd.name;
    if (!cmd.help.empty()) {
      out << " -- " << cmd.help;
    }
    out << "\n";
  }
  return out.str();
}

void initLocale() {
  // "" signals that LC_ALL should be set from the environment.
  std::setlocale(LC_ALL, ""); // for C and C++ where synced with stdio
  try {
    std::locale::global(std::locale("")); // for C++
  } catch (const std::runtime_error &) {
    // MinGW's libstdc++ has no named locales, so this throws whenever the
    // environment names one. Under cmd.exe nothing does and the program
    // started; under a shell that exports LANG it died before parsing an
    // argument, with a message about facets that named neither the program nor
    // the cause. The user's locale where it can be had and the classic one
    // where it cannot is a better answer than no program at all.
    std::locale::global(std::locale::classic());
  }
  std::cerr.imbue(std::locale());
  std::cin.imbue(std::locale());
  std::cout.imbue(std::locale());
}

void addCommonArguments(argparse::ArgumentParser &parser, const bool detailed) {
  // One definition serves both listings: the switches are always accepted, and
  // the flag decides only how they describe themselves and whether the
  // automation ones appear at all. Two separate lists would drift, and the one
  // that drifted would be the help.
  const auto everyday = [detailed](argparse::Argument &arg,
                                   const std::string_view brief,
                                   const std::string_view detail)
      -> argparse::Argument & {
    return arg.help(std::string(detailed ? detail : brief));
  };

  parser.add_argument("--help-all")
      .help("show every option, including the ones for driving this without a "
            "person, and describe them at length")
      .flag();

  everyday(parser.add_argument("--font").default_value("Monospace 16"),
           "default font to use for display",
           "Default font, as a Pango font description, for example "
           "\"Serif 16\". Anything Pango can resolve on this machine.");
  everyday(parser.add_argument("--fov").default_value(std::string{"5"}),
           "initial vertical field of view in degrees",
           "Initial vertical field of view in degrees. Widening it is how "
           "several pages are brought on screen at once, and how a headless "
           "run sees many small ones.");
  everyday(parser.add_argument("--backend").default_value(std::string{"opengl"}),
           "rendering backend: opengl, opengles or vulkan",
           "Rendering backend: opengl (the default), opengles or vulkan. The "
           "Vulkan backend is only present when the binary was built with it; "
           "naming one that is not compiled in reports which are.");
  everyday(parser.add_argument("--coarse-below").default_value(std::string{"0.15"}),
           "draw distant pages as one bar per line below this scale",
           "Draw a page as one solid bar per line once one layout pixel of it "
           "covers fewer than this many screen pixels. Zero draws every "
           "visible page in full detail, which is far slower on a document "
           "held at a distance.");

  // Everything below drives the program without a person at the keyboard.
  // Grouped only in the detailed listing: argparse prints a group's heading
  // whether or not anything in it is visible, so creating the group in the
  // everyday parser would leave an empty section behind.
  if (detailed) {
    parser.add_group("Options for automation and diagnosis");
  }

  // Registered, then hidden from the everyday listing. Hiding rather than
  // omitting is deliberate: a script that passes one of these to a build whose
  // help does not mention it still works.
  const auto automation = [detailed](argparse::Argument &arg,
                                     const std::string_view brief,
                                     const std::string_view detail)
      -> argparse::Argument & {
    arg.help(std::string(detailed ? detail : brief));
    if (!detailed) {
      arg.hidden();
    }
    return arg;
  };

  automation(parser.add_argument("--profile").flag(),
             "perform initial setup, then quit",
             "Perform initial setup, then quit once the document has settled. "
             "Pairs with --screenshot to capture a finished frame and stop.");
  automation(parser.add_argument("--no-cull").flag(),
             "draw every page, including those outside the view",
             "Draw every page of every document, including those entirely "
             "outside the view. Only useful for checking that culling changes "
             "nothing it should not: the frame must come out identical.");
  automation(parser.add_argument("--benchmark").default_value(std::string{"0"}),
             "draw N settled frames, report the timings and quit",
             "Draw N frames once the document has settled, then report frame, "
             "collect and record times and exit. The median is reported rather "
             "than the mean, because a software rasteriser produces occasional "
             "hundred-millisecond frames no average removes.");
  automation(parser.add_argument("--screenshot").default_value(std::string{}),
             "write the first settled frame to this path as a PPM",
             "Write the first fully drawn frame to this path as a binary PPM. "
             "The frame is the settled one -- every queued command carried "
             "out, every document loaded, and every animation finished -- so a "
             "capture shows the finished result rather than the middle of a "
             "fade.");
  automation(parser.add_argument("--no-present").flag(),
             "draw frames without showing them",
             "Draw frames without showing them, for capturing a frame on a "
             "machine that can give a rendering context but cannot put one on "
             "a screen. The capture is unaffected: drawing goes to an offscreen "
             "target either way. Accepted by the OpenGL and OpenGL ES backends; "
             "Vulkan refuses it, because every frame acquires a swapchain image "
             "and presenting is what hands it back.");
  automation(parser.add_argument("--pick").append(),
             "report the picking tag at X,Y; repeatable",
             "Report the picking tag at pixel X,Y once the document has "
             "settled. Repeatable. The read is asynchronous, so a run waits for "
             "every query to come back before it exits.");
  automation(parser.add_argument("--click").append(),
             "click at X,Y once settled, placing the caret; repeatable",
             "Click at pixel X,Y once the document has settled, placing the "
             "caret there, and print where it landed. Repeatable.");
  automation(parser.add_argument("--select").append(),
             "select the document byte range START,END; repeatable",
             "Select the document byte range START,END, as a click and drag "
             "would. Carried out in the order it was written among the other "
             "automation options.");
  automation(parser.add_argument("--type").append().default_value(
                 std::vector<std::string>{}),
             "insert text at the caret; repeatable",
             "Insert this text at the caret. Repeatable, and carried out in "
             "the order it was written among the other automation options, so "
             "that \"--click here --type this --click there --type that\" is "
             "the sequence it reads as. The document is spliced immediately "
             "and the layout that follows is scheduled off the render "
             "thread. Goes to whatever has the keyboard, so while a dialog is "
             "up this fills in the field it is on rather than the document.");
  automation(parser.add_argument("--do").append(),
             "run the command called NAME; repeatable",
             "Run the bound command called NAME -- the names are the ones "
             "`--help` lists -- in the order it was written among the other "
             "automation options. By name rather than by key, because a "
             "scancode and a modifier mask say which keys somebody would have "
             "pressed and not what they meant by it. This is also how a run "
             "with nobody at the keyboard reaches a dialog: open it with "
             "--do, fill it in with --type and --key.");
  automation(parser.add_argument("--key").append(),
             "press escape, enter, tab, up, down and so on; repeatable",
             "Press a key that is not text, for a dialog that is up: escape, "
             "enter, tab, backspace, delete, left, right, up, down, home, "
             "end, and any of them prefixed shift-. Carried out in order with "
             "the other automation options, and reported as a mistake when "
             "nothing is up to receive it.");
  automation(parser.add_argument("--toast").append(),
             "show a notification, as [info:|warning:|error:]TEXT; repeatable",
             "Show a notification once the first frame is drawn, written as "
             "[info:|warning:|error:]TEXT. An unprefixed message is a warning. "
             "Repeatable.");
  automation(parser.add_argument("--strict-diagnostics").flag(),
             "treat a driver error as fatal",
             "Treat a driver error as fatal instead of showing it as a "
             "notification, and set the exit status accordingly. Automated runs "
             "want this: a frame rendered by a driver that was reporting errors "
             "proves nothing, however plausible it looks.");
  automation(parser.add_argument("--print-asset-dir").flag(),
             "report where the shaders and icon were found, then quit",
             "Report the directory the shaders and the icon were found in, then "
             "quit without opening a window. Answers the one question a package "
             "can be asked on a machine whose driver cannot give it a context.");
}

render::Backend applyCommonArguments(argparse::ArgumentParser &parser,
                                     const AppStateRef &state, const int argc,
                                     const char *const *const argv) {
  const auto backend = render::backendFromName(parser.get<std::string>("--backend"));
  if (!render::backendCompiledIn(backend)) {
    throw std::runtime_error("The " + render::backendName(backend) +
                             " backend was not compiled into this binary. "
                             "Rebuild with GLEDITOR_ENABLE_VULKAN=1 to enable "
                             "Vulkan.");
  }

  state->defaultFontName   = parser.get("--font");
  state->profiling         = parser["--profile"] == true;
  state->printAssetDir     = parser["--print-asset-dir"] == true;
  state->benchmarkFrames   = std::stoul(parser.get<std::string>("--benchmark"));
  state->cullPages         = parser["--no-cull"] == false;
  state->coarseBelow       = std::stof(parser.get<std::string>("--coarse-below"));
  state->screenshotPath    = parser.get<std::string>("--screenshot");
  state->strictDiagnostics = parser["--strict-diagnostics"] == true;
  state->noPresent         = parser["--no-present"] == true;
  {
    const std::lock_guard locker(state->view);
    state->view.fov = std::stof(parser.get<std::string>("--fov"));
  }

  if (parser.present<std::vector<std::string>>("--toast")) {
    for (const auto &toast : parser.get<std::vector<std::string>>("--toast")) {
      state->requestedToasts.emplace_back(parseToast(toast));
    }
  }
  // Every option that acts on the document, in the order it was written. The
  // parser has already validated them; this is only about their order, which
  // is the one thing it does not keep.
  state->script = readAutomationScript(argc, argv);

  return backend;
}

Application::Application(AppStateRef aState, RendererRef aRenderer,
                         const render::Backend aBackend, std::string aTitle)
    : state(std::move(aState)), renderer(std::move(aRenderer)),
      backend(aBackend), title(std::move(aTitle)) {}

Application::~Application() = default;

void Application::bindDefaultViewCommands() {
  const auto move = [this](const std::function<void(AppState::ViewPerspective &)> &fun) {
    return [this, fun] {
      const std::lock_guard locker(state->view);
      fun(state->view);
    };
  };
  // One world unit per press. The view is a camera in the scene rather than a
  // scroll offset, so these are all in its own frame: forwards is where it
  // looks, sideways is the cross product with up.
  constexpr float step = 1.0F;

  commandTable.bind(SDL_SCANCODE_R, "reset view",
                    "put the camera back where it started",
                    move([](AppState::ViewPerspective &view) { view.resetPos(); }));
  commandTable.bind(SDL_SCANCODE_D, "back",
                    "move the camera away from the documents",
                    move([](AppState::ViewPerspective &view) {
                      view.pos -= step * view.front;
                    }));
  commandTable.bind(SDL_SCANCODE_D, Mod::Shift, "forward",
                    "move the camera towards the documents",
                    move([](AppState::ViewPerspective &view) {
                      view.pos += step * view.front;
                    }));
  commandTable.bind(SDL_SCANCODE_S, "left", "move the camera left",
                    move([](AppState::ViewPerspective &view) {
                      view.pos -= glm::normalize(
                                      glm::cross(view.front, view.upward)) *
                                  step;
                    }));
  commandTable.bind(SDL_SCANCODE_F, "right", "move the camera right",
                    move([](AppState::ViewPerspective &view) {
                      view.pos += glm::normalize(
                                      glm::cross(view.front, view.upward)) *
                                  step;
                    }));
  commandTable.bind(SDL_SCANCODE_E, "up", "move the camera up",
                    move([](AppState::ViewPerspective &view) {
                      view.pos -= glm::normalize(glm::cross(
                                      view.front, glm::vec3(1.0F, 0.0F, 0.0F))) *
                                  step;
                    }));
  commandTable.bind(SDL_SCANCODE_C, "down", "move the camera down",
                    move([](AppState::ViewPerspective &view) {
                      view.pos += glm::normalize(glm::cross(
                                      view.front, glm::vec3(1.0F, 0.0F, 0.0F))) *
                                  step;
                    }));
  commandTable.bind(SDL_SCANCODE_G, "widen", "widen the field of view",
                    move([](AppState::ViewPerspective &view) {
                      view.fov = std::min(360.0F, view.fov + 1.0F);
                    }));
  commandTable.bind(SDL_SCANCODE_G, Mod::Shift, "narrow",
                    "narrow the field of view",
                    move([](AppState::ViewPerspective &view) {
                      view.fov = std::max(1.0F, view.fov - 1.0F);
                    }));
}

int Application::run() {
  Glib::init();
  // What Pango::init() does, spelled out, because on MinGW it is not there to
  // call: the DLL exports what the headers mark for export, and pangomm's
  // init.cc is the one file that defines a function without including its own
  // header, so the marking never reaches the definition. wrap_init is generated
  // with its header included and is exported everywhere.
  Pango::wrap_init();

  AutoSDL sdlScoped(SDL_INIT_VIDEO);

  // Answered here rather than before SDL starts, because the search asks SDL
  // where the executable is and SDL only knows once it has been initialised.
  // Before any window is created, though: a package installed on a machine with
  // no usable GL driver can still be asked whether it found its files, and that
  // is the question packaging gets wrong.
  if (state->printAssetDir) {
    std::cout << assetDir() << "\n";
    return 0;
  }

  // Found next to the installed data files rather than in the working
  // directory, so that a packaged copy still has its icon.
  const AutoSDLSurface icon(assetPath("logo.png").c_str());

  // The window has to be created with the flags and, for the GL family, the
  // context attributes the chosen backend needs; both are decided before the
  // device itself is constructed on the render thread.
  render::configureBackendWindowAttributes(backend);

  AutoSDLWindow window(title.c_str(), state->view.screenWidth,
                       state->view.screenHeight,
                       render::backendWindowFlags(backend) |
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
                       icon.surface);

  if (textInput) {
    // Composed text rather than raw key events: this is what gives dead keys,
    // input methods and anything else the platform composes before it becomes a
    // character.
    sdl::startTextInput(window.window);
  }

  // What a scripted --do reaches. Set before the render thread starts, since
  // that is the thread that carries a script out.
  state->runCommand = [this](const std::string &name) {
    return commandTable.run(name);
  };

  std::jthread renderThread(std::ref(*renderer), std::ref(window));

  // Milliseconds to block in SDL_WaitEventTimeout. Waiting rather than spinning
  // on SDL_PollEvent keeps this thread off the CPU while idle; the timeout still
  // lets the loop notice `alive` being cleared by the renderer.
  constexpr int eventWaitMs = 100;

  while (state->alive) {
    SDL_Event evt;
    if (!SDL_WaitEventTimeout(&evt, eventWaitMs)) {
      continue;
    }
    do {
      switch (evt.type) {
      case SDL_EVENT_QUIT: {
        state->alive = false;
        break;
      }
      case SDL_EVENT_KEY_DOWN: {
        const auto scancode = static_cast<int>(sdl::keyScancode(evt));
        const auto mods     = modsFromSdl(sdl::keyModifiers(evt));
        // A modal has the keyboard while it is up, and gets first refusal on
        // every key: a question on screen is not answered by editing the
        // document behind it. A key it does not use falls through, so that
        // quitting still works while one is open.
        if (nullptr != state->modal && state->modal->grabbing()) {
          const auto key = modalKey(scancode);
          if (key && state->modal->keyPressed(*key, modalMods(mods))) {
            break;
          }
        }
        commandTable.dispatch(scancode, mods);
        break;
      }
      case SDL_EVENT_MOUSE_MOTION: {
        // Kept in window coordinates, top-down, which is what
        // RenderDevice::requestPickingTag takes. OpenGL's bottom-up framebuffer
        // convention is the GL backend's business, not the application's.
        state->mouseX = static_cast<int>(evt.motion.x);
        state->mouseY = static_cast<int>(evt.motion.y);
        // Motion with the left button held is a drag, which extends the
        // selection rather than moving the caret on its own.
        if (0 != (evt.motion.state & SDL_BUTTON_LMASK) &&
            (nullptr == state->modal || !state->modal->grabbing())) {
          state->dragX       = static_cast<int>(evt.motion.x);
          state->dragY       = static_cast<int>(evt.motion.y);
          state->dragPending = true;
        }
        break;
      }
      case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        // Held while a modal is up, along with the drag above: the caret is
        // not what is being moved when there is a question on screen.
        if (nullptr != state->modal && state->modal->grabbing()) {
          break;
        }
        // The render thread answers this: where a click lands in the text is a
        // question only the picking attachment can answer, and that read is
        // asynchronous.
        state->clickX       = static_cast<int>(evt.button.x);
        state->clickY       = static_cast<int>(evt.button.y);
        state->clickPending = true;
        break;
      }
      case SDL_EVENT_TEXT_INPUT: {
        if (nullptr != state->modal && state->modal->grabbing()) {
          state->modal->textTyped(evt.text.text);
          break;
        }
        if (textInput) {
          const std::lock_guard locker(state->typedMutex);
          state->typedText += evt.text.text;
        }
        break;
      }
      default: {
        // SDL2 reports every window change as one event type with a sub-type,
        // SDL3 as distinct types, so this is asked rather than matched on.
        int width  = 0;
        int height = 0;
        if (!sdl::windowSizeChanged(evt, width, height)) {
          break;
        }
        {
          // The render thread reads these under the same lock while building
          // its projection matrix.
          const std::lock_guard locker(state->view);
          state->view.screenWidth  = width;
          state->view.screenHeight = height;
        }
        renderer->push(RenderItemResize(width, height));
        break;
      }
      }
    } while (state->alive && SDL_PollEvent(&evt));
  }

  // The render thread reports its own failures and cannot return a status
  // through std::jthread, so the flag it sets decides the exit code.
  return state->renderFailed ? 1 : 0;
}

} // namespace gleditor

// vi: set sw=2 sts=2 ts=2 et:
