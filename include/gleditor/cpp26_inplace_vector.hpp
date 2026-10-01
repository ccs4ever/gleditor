/**
 * @file cpp26_inplace_vector.hpp
 * @brief C++26 fixed-capacity vector.
 *
 * Separate from cpp26.hpp so the large container implementation is parsed
 * only by code that uses it.
 */
#ifndef GLEDITOR_CPP26_INPLACE_VECTOR_HPP
#define GLEDITOR_CPP26_INPLACE_VECTOR_HPP

#include <gleditor/cpp26_select.hpp>

#if GLEDITOR_CPP26_NATIVE_INPLACE_VECTOR
#include <inplace_vector>
#else
#include <beman/inplace_vector/inplace_vector.hpp>
#endif

namespace gleditor::cpp26 {

#if GLEDITOR_CPP26_NATIVE_INPLACE_VECTOR
using std::inplace_vector;
#else
using beman::inplace_vector::inplace_vector;
#endif

} // namespace gleditor::cpp26

#endif // GLEDITOR_CPP26_INPLACE_VECTOR_HPP
