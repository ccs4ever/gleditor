#include "renderer.hpp"

#include "GL/glew.h"
#include "GLState.hpp"
#include "SDLwrap.hpp"
#include "doc.hpp"
#include "pangomm/attributes.h"
#include "pangomm/attrlist.h"
#include "pangomm/fontdescription.h"
#include "state.hpp"
#include <SDL_video.h>
#include <algorithm>
#include <cairomm/context.h>
#include <cairomm/surface.h>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <ostream>
#include <pango/pangocairo.h>
#include <pangomm.h>
#include <ranges>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_map>

void setupGL(AppState &state, const GLState &glState) {

  auto program = glState.programs.at("main");

  glUseProgram(program.id);

  glUniform1i(program["texUnit"], 0);

  {
    std::lock_guard locker(state.view);

    glm::mat4 projection = glm::perspective(glm::radians(state.view.fov),
                                            (float)state.view.screenWidth /
                                                (float)state.view.screenHeight,
                                            0.1F, 1000.0F);
    glm::mat4 view       = glm::lookAt(
        state.view.pos, state.view.pos + state.view.front, state.view.upward);

    glUniformMatrix4fv(program["projection"], 1, GL_FALSE,
                       glm::value_ptr(projection));
    glUniformMatrix4fv(program["view"], 1, GL_FALSE, glm::value_ptr(view));
  }
}

void resize(AppState &appState) {
  glViewport(0, 0, appState.view.screenWidth, appState.view.screenHeight);
}

void newDoc(AppState& appState, GLState &glState, AutoSDLWindow &window) {
#if 0
  auto tempSurf =
      Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, 0, 0);
  auto ctx    = Cairo::Context::create(tempSurf);
  auto layout = Pango::Layout::create(ctx);
  auto desc   = Pango::FontDescription("Monospace 16");
  layout->set_text("foo bar baz\nbaz boo\n");
  layout->set_font_description(desc);
  auto attr = Pango::Attribute::create_attr_font_desc(
      Pango::FontDescription("Courier 19"));
  attr.set_start_index(0);
  attr.set_end_index(6);
  auto attr2 = Pango::Attribute::create_attr_foreground(
      std::numeric_limits<unsigned short>::max(), 0, 0);
  // start_index inclusive, end_index exclusive
  attr2.set_start_index(6);
  attr2.set_end_index(9);
  Pango::AttrList attrList;
  attrList.insert(attr);
  attrList.insert(attr2);
  layout->set_attributes(attrList);
  auto iter = layout->get_iter();
  do {
    auto i = iter.get_run();
    if (nullptr != i.gobj()) {
      auto item = i.get_item();
      std::cout << "first run: offset: " << item.get_offset()
                << " length: " << item.get_length()
                << " num chars: " << item.get_num_chars()
                << " analysis(level): " << item.get_analysis().get_level()
                << "\n";
    }
  } while (iter.next_run());

  int width;
  int height;
  layout->get_pixel_size(width, height);
  auto format = Cairo::Surface::Format::ARGB32;
  int stride  = Cairo::ImageSurface::format_stride_for_width(format, width);
  auto *data  = new unsigned char[static_cast<long>(width * height) * stride];
  auto layoutSurf =
      Cairo::ImageSurface::create(data, format, width, height, stride);
  auto layCtx = Cairo::Context::create(layoutSurf);
  layCtx->set_source_rgb(0, 0, 0);
  layCtx->rectangle(0, 0, width, height);
  layCtx->fill();
  layCtx->set_source_rgb(1, 1, 1);
  layout->show_in_cairo_context(layCtx);
  layoutSurf->write_to_png("/tmp/page.png");
#endif
  if (glState.docs.empty()) {
    auto docPtr = Doc::create(glm::mat4(1.0));
    glState.docs.push_back(docPtr->getPtr());
  }
  std::shared_ptr<Doc> doc = glState.docs.back()->getPtr();

  std::cerr << "doc use count: " << doc.use_count() << "\n";

  doc->newPage(appState, glState);
}

void openDoc(AppState& appState, GLState &glState, AutoSDLWindow &window, std::string &fileName) {
  auto docPtr = Doc::create(glm::mat4(1.0), fileName);
  glState.docs.push_back(docPtr->getPtr());
  newDoc(appState, glState, window);
}

inline GLenum getShaderType(const std::string &stage) {

  static std::map<std::string, GLenum> types = {
      {"vert", GL_VERTEX_SHADER},       {"frag", GL_FRAGMENT_SHADER},
      {"tesc", GL_TESS_CONTROL_SHADER}, {"tese", GL_TESS_EVALUATION_SHADER},
      {"geom", GL_GEOMETRY_SHADER},     {"comp", GL_COMPUTE_SHADER},
  };
  if (types.contains(stage)) {
    return types[stage];
  }
  throw std::runtime_error(
      std::format("Error: Unknown shader type: {}", stage));
}

void setupShaders(GLState &state) {

  static std::regex uniformsReg(
      R"(^\s*(uniform|in)\s+([a-zA-Z]+)(\d+)?\S*?\s+(\w+)\s*;)",
      std::regex_constants::multiline);

  const std::filesystem::path glslDir{"assets/glsl"};

  std::vector<std::pair<unsigned int, unsigned int>> shadersToDelete;

  for (const auto &entry : std::filesystem::directory_iterator{glslDir}) {
    if (const auto &path = entry.path();
        path.extension() == ".glsl" && path.stem().has_extension()) {
      std::cerr << "processing glsl file: " << path << "\n";
      // main.vert.glsl => vert
      const auto shaderStage = path.stem().extension().string().erase(0, 1);
      // main.vert.glsl => main
      const auto progName = path.stem().stem().string();
      std::cerr << "processing glsl name/stage: " << progName << "/"
                << shaderStage << "\n";
      if (!state.programs.contains(progName)) {
        unsigned int pid = glCreateProgram();
        if (0 == pid) {
          throw std::runtime_error("Failed to create GL program object");
        }
        std::cerr << "creating program name/id: " << progName << "/" << pid
                  << "\n";
        state.programs.emplace(progName, GLState::Program{pid, {}});
      }
      auto &prog = state.programs[progName];
      std::ifstream stream(path, std::ios::ate);
      if (!stream.is_open()) {
        throw std::runtime_error("Failed to open file: " +
                                 entry.path().string());
      }
      std::cerr << "creating shader type: "
                << (getShaderType(shaderStage) == GL_FRAGMENT_SHADER
                        ? "fragment"
                        : (getShaderType(shaderStage) == GL_VERTEX_SHADER
                               ? "vertex"
                               : ""))
                << "\n";
      const auto shader = glCreateShader(getShaderType(shaderStage));
      std::cerr << "created shader: " << shader << "\n";
      // slurp entire file into string
      std::string glslSource;
      const auto size = (int)stream.tellg();
      std::cerr << "reading shader file size: " << size << "\n";
      glslSource.resize(size + 1);
      stream.seekg(0);
      stream.read(glslSource.data(), size);
      std::cerr << "read shader file size: " << size << "\n";
      const char *cstr = glslSource.c_str();
      glShaderSource(shader, 1, &cstr, nullptr);
      glCompileShader(shader);
      GLint shaderCompiled = GL_FALSE;
      glGetShaderiv(shader, GL_COMPILE_STATUS, &shaderCompiled);
      std::cerr << "shader compiled: " << bool(shaderCompiled == GL_TRUE)
                << "\n"
                << std::flush;
      if (GL_FALSE == shaderCompiled) {
        int logLen = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLen);
        if (logLen > 0) {
          char *buf = new char[logLen];
          glGetShaderInfoLog(shader, logLen, &logLen, buf);
          std::cerr << "shader log: " << buf << "\n" << std::flush;
          delete[] buf;
        }
        throw std::runtime_error(std::format("Failed to compile shader: {}/{}",
                                             progName, shaderStage));
      }
      std::sregex_iterator reStart(glslSource.begin(), glslSource.end(),
                                   uniformsReg);
      std::sregex_iterator reEnd;
      for (auto it = reStart; it != reEnd; ++it) {
        const auto type = it->str(1); // in or uniform
        if ("vert" != shaderStage && "in" == type) {
          continue;
        }
        auto varType        = it->str(2);
        auto sizeFromVarNum = std::atoi(it->str(3).c_str());
        sizeFromVarNum      = 0 != sizeFromVarNum ? sizeFromVarNum : 1;
        const auto size     = type == "in" ? sizeFromVarNum : 0;
        const auto name     = it->str(4);
        std::cerr << "found " << type << ": (" << (varType + it->str(3)) << "/"
                  << size << ")/" << std::quoted(name) << "\n"
                  << std::flush;
        prog.locs.emplace(name, GLState::Loc{0, type, varType, size});
      }
      const auto pid = state.programs[progName].id;
      glAttachShader(pid, shader);
      std::cerr << "shader attached\n";
      shadersToDelete.emplace_back(pid, shader);
    }
  }
  for (const auto &[key, _v] : state.programs) {
    auto &program  = state.programs[key];
    const auto pid = program.id;
    glLinkProgram(pid);
    glValidateProgram(pid);
    for (auto &nameToLoc : program.locs) {
      const bool isUniform = nameToLoc.second.type == "uniform";
      const auto locId =
          isUniform ? glGetUniformLocation(pid, nameToLoc.first.c_str())
                    : glGetAttribLocation(pid, nameToLoc.first.c_str());
      nameToLoc.second.loc = locId;
      std::cerr << std::format(
          "attr name: {} type: {} vartype: {} uniform: {} loc: {}/{}\n",
          nameToLoc.first, nameToLoc.second.type, nameToLoc.second.varType,
          isUniform, nameToLoc.second.loc, int(nameToLoc.second));
      if (-1 == locId) {
        std::cerr << std::format(
            "Failed to get {} location: {}. Linker may have elided it.\n\n",
            (isUniform ? "uniform" : "attribute"), nameToLoc.first);
      }
      std::cerr << "got location for " << key << "/" << nameToLoc.first << ": "
                << locId << "\n"
                << std::flush;
    }
  }
  for (const auto [pid, shader] : shadersToDelete) {
    glDetachShader(pid, shader);
    glDeleteShader(shader);
  }
}

inline std::string getSeverity(GLenum severity) {
  switch (severity) {
  case GL_DEBUG_SEVERITY_LOW:
    return "low";
  case GL_DEBUG_SEVERITY_MEDIUM:
    return "medium";
  case GL_DEBUG_SEVERITY_HIGH:
    return "high";
  case GL_DEBUG_SEVERITY_NOTIFICATION:
    return "notice";
  default:
    return std::format("{:x}", severity);
  }
}

// cannot change this interface, so stop linter complaining about it
// NOLINTBEGIN

void debugCb(GLenum source, GLenum type, GLuint id, GLenum severity,
             GLsizei length, const GLchar *message, const void *userParam) {
  // NOLINTEND
  std::fprintf(stderr,
               "GL CALLBACK: %s type = 0x%x, severity = %s, message = %s\n",
               (type == GL_DEBUG_TYPE_ERROR ? "** GL ERROR **" : ""), type,
               getSeverity(severity).c_str(), message);
  std::flush(std::cerr);
  if (type == GL_DEBUG_TYPE_ERROR) {
    throw std::runtime_error(std::string("Got GL error: ") + message);
  }
  if (type == GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR) {
    throw std::runtime_error(std::string("Got GL undefined behavior: ") +
                             message);
  }
#ifndef NDEBUG
  if (type == GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR) {
    throw std::runtime_error(std::string("Got GL deprecation: ") + message);
  }
#endif
}

// NOLINTBEGIN
void debugCbAMD(GLuint id, GLenum type, GLenum severity, GLsizei length,
                const GLchar *message, void *userParam) {
  // NOLINTEND
  const bool isErr =
      type == GL_DEBUG_TYPE_ERROR || type == GL_DEBUG_CATEGORY_API_ERROR_AMD;
  std::fprintf(stderr,
               "GL CALLBACK: %s type = 0x%x, severity = %s, message = %s\n",
               (isErr ? "** GL ERROR **" : ""), type,
               getSeverity(severity).c_str(), message);
  if (isErr) {
    throw std::runtime_error(std::string("Got GL error: ") + message);
  }
  if (type == GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR ||
      type == GL_DEBUG_CATEGORY_UNDEFINED_BEHAVIOR_AMD) {
    throw std::runtime_error(std::string("Got GL undefined behavior: ") +
                             message);
  }
#ifndef NDEBUG
  if (type == GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR ||
      type == GL_DEBUG_CATEGORY_DEPRECATION_AMD) {
    throw std::runtime_error(std::string("Got GL deprecation: ") + message);
  }
#endif
}

void initGL() {

  glewExperimental = GL_TRUE;
  GLenum err       = glewInit();
  if (GLEW_OK != err) {
    throw std::runtime_error(
        std::string("Error initializing GLEW: ") +
        reinterpret_cast<const char *>(glewGetErrorString(err)));
  }

  glEnable(GL_DEBUG_OUTPUT);
  if (GL_TRUE == glIsEnabled(GL_DEBUG_OUTPUT)) {
    if (GL_KHR_debug) {
      glDebugMessageCallback(debugCb, nullptr);
    } else if (GL_ARB_debug_output) {
      glDebugMessageCallbackARB(debugCb, nullptr);
    } else if (GL_AMD_debug_output) {
      glDebugMessageCallbackAMD(debugCbAMD, nullptr);
    }
  } else {
    while (GL_NO_ERROR != glGetError()) {
    };
  }

  // glEnable(GL_CULL_FACE);
  // glEnable(GL_DEPTH_TEST);
  glClearColor(0, 0, 0, 1);
}

void Renderer::operator()(AppState &appState, AutoSDLWindow &window) {

  AutoSDLGL glCtx(window.window);

  initGL();

  // needs to come after initGL so we can initialize GlyphCache
  GLState glState(std::make_shared<GL>());

  setupShaders(glState);
    
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  SDL_GL_SwapWindow(window.window);

  while (appState.alive) {
    const auto start = std::chrono::steady_clock::now();
    // application logic here
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    setupGL(appState, glState);

    while (auto item = appState.renderQueue.pop()) {
      switch (item->type) {
      case RenderItem::Type::NewDoc:
        newDoc(appState, glState, window);
        break;
      case RenderItem::Type::Resize:
        resize(appState);
        break;
      case RenderItem::Type::OpenDoc: {
	auto *ptr = item.get();
        auto *docItem =
            dynamic_cast<RenderItemOpenDoc *>(ptr);
        openDoc(appState, glState, window, docItem->docFile);
        break;
      }
      default:
        break;
      }
    }

    // avoid expensive rendering if we are dead
    if (!appState.alive) {
      break;
    }

    for (const std::shared_ptr<Doc> &doc : glState.docs) {
      doc->draw(glState);
    }

    // swap buffers;
    SDL_GL_SwapWindow(window.window);

    const auto end          = std::chrono::steady_clock::now();
    appState.frameTimeDelta = end - start;
    
    if (appState.profiling) {
      appState.alive = false;
      break;
    }

  }
}
