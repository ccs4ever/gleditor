#ifndef GLEDITOR_GLSTATE_H
#define GLEDITOR_GLSTATE_H

#include <memory>
#include <unordered_map>
#include <vector>

#include <gleditor/glyphcache/cache.hpp>

class Doc;

struct GLState {
  GLState(std::shared_ptr<GL> aGl) : glyphCache(aGl) {}
  /// State exclusive to the render thread
  std::vector<std::shared_ptr<Doc>> docs;
  struct Loc {
    int loc{};
    std::string type;
    std::string varType;
    int size{};
    operator int() const { return loc; }
  };
  struct Program {
    unsigned int id;
    struct Texture {
      unsigned int id;
    };
    std::vector<Texture> texs;
    std::unordered_map<std::string, Loc> locs;
    Loc &operator[](const std::string &loc) { return locs[loc]; }
    const Loc &operator[](const std::string &loc) const { return locs.at(loc); }
  };
  std::unordered_map<std::string, Program> programs;
  GlyphCache glyphCache;
  ///
};

#endif // GLEDITOR_GLSTATE_H
