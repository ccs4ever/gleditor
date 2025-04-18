#include <gleditor/renderer.hpp>              // IWYU pragma: associated
#include <SDL_video.h>                        // for SDL_GL_GetCurrentWindow
#include <gleditor/doc.hpp>                   // for Doc
#include <gleditor/gl/state.hpp>              // for GLState
#include <gleditor/sdl_wrap.hpp>              // for AutoSDLGL, AutoSDLWindow
#include <gleditor/state.hpp>                 // for AppState
#include <glm/ext/matrix_float4x4.hpp>        // for mat4
#include <glm/ext/vector_uint2.hpp>           // for uvec2
#include <bits/chrono.h>                      // for duration, steady_clock
#include <glm/detail/qualifier.hpp>           // for qualifier
#include <glm/detail/type_vec3.hpp>           // for vec
#include <glm/gtc/type_ptr.hpp>
#include <cstdio>                             // for fprintf, stderr
#include <cstdlib>                            // for atoi
#include <filesystem>                         // for path, directory_iterator
#include <format>                             // for format
#include <fstream>                            // for basic_ostream, operator<<
#include <future>                             // for async, launch, future
#include <iomanip>                            // for operator<<, quoted
#include <iostream>                           // for cerr, cout
#include <map>                                // for map
#include <memory>                             // for allocator, shared_ptr
#include <mutex>                              // for lock_guard
#include <regex>                              // for regex_iterator, sregex_...
#include <stdexcept>                          // for runtime_error, logic_error
#include <string>                             // for basic_string, char_traits
#include <thread>                             // for get_id
#include <unordered_map>                      // for unordered_map, _Node_it...
#include <utility>                            // for pair
#include <vector>                             // for vector
#include <atomic>                             // for atomic

#include "GL/glew.h"                          // for GL_RENDERBUFFER, GLenum
#include "SDL_error.h"                        // for SDL_GetError
#include <gleditor/gl/gl.hpp>                 // for GL
#include <gleditor/tqueue.hpp>                // for TQueue

void Renderer::setupGL(const GLState &glState) {

  auto program = glState.programs.at("main");

  glUseProgram(program.id);

  glUniform1i(program["texGlyphCache"], 0);

  {
    std::lock_guard locker(state->view);

    glm::mat4 projection = glm::perspective(glm::radians(state->view.fov),
                                            (float)state->view.screenWidth /
                                                (float)state->view.screenHeight,
                                            0.1F, 1000.0F);
    glm::mat4 view =
        glm::lookAt(state->view.pos, state->view.pos + state->view.front,
                    state->view.upward);

    glUniformMatrix4fv(program["projection"], 1, GL_FALSE,
                       glm::value_ptr(projection));
    glUniformMatrix4fv(program["view"], 1, GL_FALSE, glm::value_ptr(view));
  }
}

void Renderer::resize() {
  auto *win = SDL_GL_GetCurrentWindow();
  if (nullptr == win) {
    throw std::logic_error{
        std::format("gl_getcurrentwindow error: {}", SDL_GetError())};
  }
  int idx = SDL_GetWindowDisplayIndex(win);
  if (idx < 0) {
    throw std::logic_error{
        std::format("getwindowdisplayindex error: {}", SDL_GetError())};
  }
  float ddpi, hdpi, vdpi;
  if (0 != SDL_GetDisplayDPI(idx, &ddpi, &hdpi, &vdpi)) {
    throw std::logic_error{
        std::format("getdisplaydpi error: {}", SDL_GetError())};
  }
  std::cout << std::format("dpis: {:.02f} {:.02f} {:.02f}\n", ddpi, hdpi, vdpi);
  int w, h;
  SDL_GL_GetDrawableSize(SDL_GL_GetCurrentWindow(), &w, &h);
  if (0 == w) {
    std::cout << "width null\n";
    w = state->view.screenWidth;
  }
  if (0 == h) {
    std::cout << "height null\n";
    h = state->view.screenHeight;
  }
  std::cout << "renderer resize: " << w << " " << h << "\n";
  glViewport(0, 0, w, h);
}

void Renderer::newDoc(GLState &glState, AutoSDLWindow &window) {
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
  auto docPtr = Doc::create(getPtr(), glm::mat4(1.0));
  glState.docs.push_back(docPtr->getPtr());
  std::shared_ptr<Doc> doc = docPtr->getPtr();

  std::cerr << "doc use count: " << doc.use_count() << "\n";
}

void Renderer::openDoc(GLState &glState, AutoSDLWindow &window,
                       std::string &fileName) {
  auto docPtr = Doc::create(getPtr(), glm::mat4(1.0), fileName);
  static std::future<void> fut = std::async(
      std::launch::async, [&glState, docPtr] { docPtr->makePages(glState); });
  glState.docs.push_back(docPtr->getPtr());
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

void setupShaders(GLState &glState) {

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
      if (!glState.programs.contains(progName)) {
        unsigned int pid = glCreateProgram();
        if (0 == pid) {
          throw std::runtime_error("Failed to create GL program object");
        }
        std::cerr << "creating program name/id: " << progName << "/" << pid
                  << "\n";
        glState.programs.emplace(progName, GLState::Program{pid, {}});
      }
      auto &prog = glState.programs[progName];
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
      const auto pid = glState.programs[progName].id;
      glAttachShader(pid, shader);
      std::cerr << "shader attached\n";
      shadersToDelete.emplace_back(pid, shader);
    }
  }
  for (const auto &[key, _v] : glState.programs) {
    auto &program  = glState.programs[key];
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

void Renderer::initGL() {

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

  glGenFramebuffers(1, &pickingFBO);
  glGenRenderbuffers(1, &colorRBO);
  glGenRenderbuffers(1, &pickingRBO);
  glGenRenderbuffers(1, &depthRBO);

  AutoFBO bindFbo(this, GL_FRAMEBUFFER);

  glBindRenderbuffer(GL_RENDERBUFFER, colorRBO);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA, state->view.screenWidth,
                        state->view.screenHeight);
  glBindRenderbuffer(GL_RENDERBUFFER, pickingRBO);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_RG32UI, state->view.screenWidth,
                        state->view.screenHeight);
  glBindRenderbuffer(GL_RENDERBUFFER, depthRBO);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT,
                        state->view.screenWidth, state->view.screenHeight);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);

  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + 0,
                            GL_RENDERBUFFER, colorRBO);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + 1,
                            GL_RENDERBUFFER, pickingRBO);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, depthRBO);

  const auto check = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (GL_FRAMEBUFFER_COMPLETE != check) {
    throw std::runtime_error(
        std::format("Picking FBO is incomplete: {}", check));
  }

  glEnable(GL_CULL_FACE);
  glEnable(GL_DEPTH_TEST);
  glFrontFace(GL_CW);
  glClearColor(0, 0, 0, 1);
}

bool Renderer::update(GLState &glState, AutoSDLWindow &window) {
  // std::cout << "calling update\n" << std::flush;
  static auto fullStart = std::chrono::steady_clock::now();
  const auto start      = std::chrono::steady_clock::now();

  AutoFBO fbo(this, GL_FRAMEBUFFER);

  // application logic here
  /*glDrawBuffer(GL_COLOR_ATTACHMENT1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glDrawBuffer(GL_COLOR_ATTACHMENT0);*/
  /*std::array<unsigned int, 4> zero = {0,0,0,0};
  glClearBufferuiv(GL_COLOR, pickingRBO, zero.data());*/
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

  setupGL(glState);

  // avoid expensive rendering if we are dead
  if (!state->alive) {
    return false;
  }

  for (const std::shared_ptr<Doc> &doc : glState.docs) {
    doc->draw(glState);
  }

  glBindFramebuffer(GL_READ_FRAMEBUFFER, pickingFBO);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  const int w = state->view.screenWidth;
  const int h = state->view.screenHeight;
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glDrawBuffer(GL_BACK);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);

  // swap buffers;
  SDL_GL_SwapWindow(window.window);

  glm::uvec2 tag(0, 0);
  glClampColor(GL_CLAMP_READ_COLOR, GL_FALSE);
  glReadBuffer(GL_COLOR_ATTACHMENT1);
  glReadPixels(state->mouseX, state->mouseY, 1, 1, GL_RG_INTEGER,
               GL_UNSIGNED_INT, glm::value_ptr(tag));

  if (tag.r != 0 || tag.g != 0) {
    std::cout << std::format("tagged object: {:x} {:x}\n", tag.r, tag.g);
  }

  const auto end        = std::chrono::steady_clock::now();
  state->frameTimeDelta = end - start;

  if (state->profiling) {
    state->alive = false;
  }
  return state->alive;
}

void Renderer::operator()(AutoSDLWindow &window) {

  this->renderThreadId = std::this_thread::get_id();

  AutoSDLGL glCtx(window.window);

  initGL();

  // needs to come after initGL so we can initialize GlyphCache
  GLState glState(std::make_shared<GL>());

  setupShaders(glState);

  resize();

  while (state->alive) {

    // still want to update once even if we don't have anything in the render
    // queue
    if (!update(glState, window)) {
      break;
    }
    while (auto item = renderQueue.pop()) {
      switch (item->type) {
      case RenderItem::Type::NewDoc:
        newDoc(glState, window);
        break;
      case RenderItem::Type::Resize:
        resize();
        break;
      case RenderItem::Type::OpenDoc: {
        auto *docItem = dynamic_cast<RenderItemOpenDoc *>(item.get());
        openDoc(glState, window, docItem->docFile);
        break;
      }
      case RenderItem::Type::Run: {
        auto *runItem = dynamic_cast<RenderItemRun *>(item.get());
        (*runItem)();
        break;
      }
      default:
        break;
      }
      if (!update(glState, window)) {
        break;
      }
    }
  }
}
