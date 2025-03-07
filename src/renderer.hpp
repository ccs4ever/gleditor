#ifndef GLEDITOR_RENDERER_H
#define GLEDITOR_RENDERER_H

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

#include "GLState.hpp"
#include "log.hpp"
#include "state.hpp"

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

class Renderer : public Loggable, public std::enable_shared_from_this<Renderer> {
private:
  std::mutex mtx;
  AppStateRef state;
  std::thread::id renderThreadId;
  // token to keep anything other than Renderer::create from using our constructor
  struct Private {
    explicit Private() = default;
  };
protected:
  void openDoc(GLState &glState, AutoSDLWindow &window, std::string &fileName);
  void newDoc(GLState &glState, AutoSDLWindow &window);
  void setupGL(const GLState &glState);
  void resize();
  bool update(GLState &glState, AutoSDLWindow &window);
public:
  static std::shared_ptr<Renderer> create(AppStateRef appState) {
    return std::make_shared<Renderer>(appState, Private());
  }
  std::shared_ptr<Renderer> getPtr() { return shared_from_this(); }
  Renderer(AppStateRef state, [[maybe_unused]] Private _priv) : state(std::move(state)) {}
  void operator()(AutoSDLWindow &window);
  void run(std::invocable auto fun) {
    if (std::this_thread::get_id() == renderThreadId) {
      fun();
    } else {
      state->renderQueue.push(RenderItemRun(fun));
    }
  }
  std::string_view defaultFontName() { return state->defaultFontName; }

};

using RendererRef = std::shared_ptr<Renderer>;

#endif // GLEDITOR_RENDERER_H
