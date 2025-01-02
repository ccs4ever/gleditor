#ifndef GLEDITOR_QUEUE_H
#define GLEDITOR_QUEUE_H

#include <memory>
#include <mutex>
#include <queue>

template <typename T> class TQueue {
private:
  using value = std::unique_ptr<T>;
  std::queue<value> queue;
  mutable std::mutex mutex;

  bool isEmpty() { return queue.empty(); }

public:
  [[nodiscard]] unsigned long size() const {
    std::lock_guard lock(mutex);
    return queue.size();
  }
  value pop() {
    std::lock_guard lock(mutex);
    if (isEmpty()) {
      return nullptr;
    }
    auto ret = std::move(queue.front());
    queue.pop();
    return ret;
  }

  void push(const T &item) {
    std::lock_guard lock(mutex);
    queue.push(std::make_unique<T>(item));
  }
};

#endif // GLEDITOR_QUEUE_H
