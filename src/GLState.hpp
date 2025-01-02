#ifndef GLEDITOR_GLSTATE_H
#define GLEDITOR_GLSTATE_H

#include <vector>
#include <unordered_map>
#include <memory>

class Doc;

struct GLState {
  /// State exclusive to the render thread
  std::vector<std::shared_ptr<Doc>> docs;
  struct Loc {
    int loc{};
    std::string type;
    int size{};
    operator int() const { return loc; }
  };
  struct Program {
    unsigned int id;
    std::unordered_map<std::string, Loc> locs;
    Loc &operator[](const std::string &loc) { return locs[loc]; }
    const Loc &operator[](const std::string &loc) const { return locs.at(loc); }
  };
  std::unordered_map<std::string, Program> programs;
  ///
};

#endif // GLEDITOR_GLSTATE_H
