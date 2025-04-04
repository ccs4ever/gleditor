#ifndef GLEDITOR_RENDERER_H
#define GLEDITOR_RENDERER_H

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

#include "GLState.hpp"
#include "gl/GL.hpp"
#include "log.hpp"
#include "state.hpp"
#include "vao_supports.hpp"

struct AutoSDLWindow;

struct RenderItem {
  enum class Type : std::uint8_t {
    NewDoc,
    Resize,
    OpenDoc,
    Run,
  };
  Type type;

  RenderItem(Type type) : type(type) {}
  virtual ~RenderItem() = default;
};
struct RenderItemNewDoc : public RenderItem {
  RenderItemNewDoc() : RenderItem(RenderItem::Type::NewDoc) {}
  ~RenderItemNewDoc() override = default;
};

struct RenderItemResize : public RenderItem {
  int width, height;
  RenderItemResize(int width, int height)
      : RenderItem(RenderItem::Type::Resize), width(width), height(height) {}
  ~RenderItemResize() override = default;
};

struct RenderItemOpenDoc : public RenderItem {
  std::string docFile;
  RenderItemOpenDoc(std::string fileName)
      : RenderItem(RenderItem::Type::OpenDoc), docFile(std::move(fileName)) {}
  ~RenderItemOpenDoc() override = default;
};

struct RenderItemRun : public RenderItem {
  std::function<void()> fun;
  RenderItemRun(std::invocable auto fun)
      : RenderItem(RenderItem::Type::Run), fun(fun) {}
  ~RenderItemRun() override = default;
  void operator()() const { fun(); }
};

class AbstractRenderer : public Loggable,
                         public std::enable_shared_from_this<AbstractRenderer> {
protected:
  std::mutex mtx;
  TQueue<RenderItem> renderQueue;
  AppStateRef state;
  std::thread::id renderThreadId;
  // token to keep anything other than *Renderer::create from using our
  // constructor
  struct Private {
    explicit Private() = default;
  };

public:
  struct AutoProgram {
    const AbstractRenderer *renderer;
    AutoProgram(const AbstractRenderer *aRenderer, const std::string &progName)
        : renderer(aRenderer) {
      renderer->useProgram(progName);
    }
    ~AutoProgram() { renderer->clearProgram(); }
  };
  virtual void useProgram(const std::string &progName) const = 0;
  virtual void clearProgram() const                          = 0;
  std::shared_ptr<AbstractRenderer> getPtr() { return shared_from_this(); }
  AbstractRenderer(AppStateRef aState, [[maybe_unused]] Private _priv)
      : state(std::move(aState)) {}
  ~AbstractRenderer() override = default;
  static std::shared_ptr<AbstractRenderer> create(AppStateRef appState) {
    return std::make_shared<AbstractRenderer>(appState, Private());
  }
  void operator()(AutoSDLWindow &window);
  void run(std::invocable auto fun) {
    if (std::this_thread::get_id() == renderThreadId) {
      fun();
    } else {
      renderQueue.push(RenderItemRun(fun));
    }
  }
  void run(std::invocable<AbstractRenderer *> auto fun) {
    if (std::this_thread::get_id() == renderThreadId) {
      fun(this);
    } else {
      renderQueue.push(RenderItemRun(std::bind(fun, this)));
    }
  }
  template <typename Item>
    requires std::derived_from<Item, RenderItem>
  void push(const Item &item) {
    renderQueue.push(item);
  }
  std::string_view defaultFontName() const { return state->defaultFontName; }
};

using AbstractRendererRef = std::shared_ptr<AbstractRenderer>;

class Renderer : public AbstractRenderer {

protected:
  unsigned int pickingFBO, pickingRBO, colorRBO, depthRBO;
  void openDoc(GLState &glState, AutoSDLWindow &window, std::string &fileName);
  void newDoc(GLState &glState, AutoSDLWindow &window);
  void setupGL(const GLState &glState);
  void resize();
  bool update(GLState &glState, AutoSDLWindow &window);
  void initGL();

public:
  template <typename VBORow> struct AutoVAO {
    const VAOSupports<VBORow> *support;
    AutoVAO(const VAOSupports<VBORow> *aSupport) : support(aSupport) {
      support->bindVAO();
    }

    ~AutoVAO() { VAOSupports<VBORow>::clearVAO(); }
  };
  struct AutoFBO {
    const Renderer *renderer;
    GLenum target;
    AutoFBO(const Renderer *aRenderer, GLenum aTarget)
        : renderer(aRenderer), target(aTarget) {
      renderer->bindFBO(target);
    }
    ~AutoFBO() { Renderer::clearFBO(target); }
  };
  void bindFBO(GLenum target) const {
    glBindFramebuffer(target, pickingFBO);
    std::array<unsigned int, 2> arr = {GL_COLOR_ATTACHMENT0,
                                       GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, arr.data());
  }
  static void clearFBO(GLenum target) { glBindFramebuffer(target, 0); }
};

#endif // GLEDITOR_RENDERER_H
