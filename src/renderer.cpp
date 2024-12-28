#include "renderer.hpp"

#include "GL/glew.h"
#include "SDLwrap.hpp"
#include <SDL_video.h>
#include "pangomm/attributes.h"
#include "pangomm/attrlist.h"
#include "pangomm/fontdescription.h"
#include <cairomm/context.h>
#include <cairomm/surface.h>
#include <pango/pangocairo.h>
#include <pangomm.h>

void newDoc(AutoSDLWindow &window) {
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
}

void Renderer::operator ()(AppState &state) {

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

  AutoSDLSurface icon("logo.png");

  AutoSDLWindow window("GL Editor", SDL_WINDOWPOS_UNDEFINED,
                       SDL_WINDOWPOS_UNDEFINED, 640, 480,
                       SDL_WINDOW_OPENGL | SDL_WINDOW_MAXIMIZED);

  SDL_SetWindowIcon(window.window, icon.surface);

  AutoSDLGL glCtx(window);

  glewExperimental = 1;
  glewInit();

  glClearColor(0, 0, 0, 1);
  while (state.alive) {
    // application logic here
    glClear(GL_COLOR_BUFFER_BIT);

    while (auto item = state.renderQueue.pop()) {
      switch (item->type) {
      case RenderItem::Type::NewDoc:
        newDoc(window);
        break;
      default:
        break;
      }
    }

    // swap buffers;
    SDL_GL_SwapWindow(window.window);
  }
}

