#ifndef GLEDITOR_QUEUE_H
#define GLEDITOR_QUEUE_H

#include <concepts>
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
    std::scoped_lock lock(mutex);
    return queue.size();
  }
  [[nodiscard]] bool empty() const {
    std::scoped_lock lock(mutex);
    return queue.empty();
  }
  value pop() {
    std::scoped_lock lock(mutex);
    if (isEmpty()) {
      return nullptr;
    }
    auto ret = std::move(queue.front());
    queue.pop();
    return ret;
  }

  template <typename U>
    requires std::derived_from<U, T>
  void push(const U &item) {
    std::scoped_lock lock(mutex);
    queue.push(std::make_unique<U>(item));
  }
};

#endif // GLEDITOR_QUEUE_H
