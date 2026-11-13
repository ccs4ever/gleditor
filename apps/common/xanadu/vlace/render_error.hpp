/**
 * @file render_error.hpp
 * @brief The exception a deep render throws when forcing a part of the value
 *        fails, so the evaluation reports that failure (code and location)
 *        instead of showing a placeholder inside an apparently complete result.
 */
#ifndef COMMON_XANADU_VLACE_RENDER_ERROR_HPP
#define COMMON_XANADU_VLACE_RENDER_ERROR_HPP

#include <stdexcept>
#include <utility>

#include "common/xanadu/vlace/compiler.hpp"

namespace xanadu::vlace::lower {

class RenderFailure : public std::runtime_error {
public:
  explicit RenderFailure(EvalError error)
      : std::runtime_error(error.detail), error_(std::move(error)) {}
  [[nodiscard]] const EvalError &error() const noexcept { return error_; }

private:
  EvalError error_;
};

} // namespace xanadu::vlace::lower

#endif // COMMON_XANADU_VLACE_RENDER_ERROR_HPP
