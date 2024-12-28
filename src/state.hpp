#ifndef GLEDITOR_STATE_H
#define GLEDITOR_STATE_H

#include "tqueue.hpp"

struct RenderItem;

struct AppState {
  bool alive = true;
  TQueue<RenderItem> renderQueue;
};

#endif // GLEDITOR_STATE_H
