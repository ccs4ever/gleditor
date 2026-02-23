#ifndef GLEDITOR_GLSTATE_H
#define GLEDITOR_GLSTATE_H

#include <gleditor/gl/gl.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <gleditor/glyphcache/cache.hpp>

class Doc;

struct GLState {
  explicit GLState(const std::shared_ptr<GL>& aGl) : glyphCache(aGl) {}
  /// State exclusive to the render thread
  std::vector<std::shared_ptr<Doc>> docs;
  struct Loc {
    GLint loc{};
    std::string type;
    std::string varType;
    int size{};
    operator GLint() const { return loc; }
  };
  struct Program {
    GLuint id{};
    struct Texture {
      GLuint id{};
      operator GLuint() const { return id; }
    };
    std::vector<Texture> texs;
    std::unordered_map<std::string, Loc> locs;
    Loc &operator[](const char* const loc) { return locs[loc]; }
    Loc &operator[](const std::string &loc) { return locs[loc]; }
    const Loc& operator[](const char* const loc) const { return locs.at(loc); }
    const Loc& operator[](const std::string &loc) const { return locs.at(loc); }
    operator GLuint() const { return id; }
  };
  std::unordered_map<std::string, Program> programs;
  Program &operator[](const char* const program) { return programs[program]; }
  Program &operator[](const std::string &program) { return programs[program]; }
  const Program& operator[](const char* const program) const { return programs.at(program); }
  const Program& operator[](const std::string &program) const { return programs.at(program); }
  GlyphCache glyphCache;
  ///
};

#endif // GLEDITOR_GLSTATE_H
