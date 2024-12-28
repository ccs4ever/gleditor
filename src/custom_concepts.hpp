#ifndef GLEDITOR_CUSTOM_CONCEPTS_H
#define GLEDITOR_CUSTOM_CONCEPTS_H

#include <concepts>

template<typename T>
concept Positive = requires(T& typ) {
  typ > 0;
};

template<typename T>
concept PositiveInt = std::is_integral_v<T> && Positive<T>;


#endif // GLEDITOR_CUSTOM_CONCEPTS_H
