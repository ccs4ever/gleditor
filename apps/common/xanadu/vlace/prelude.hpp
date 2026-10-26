/**
 * @file prelude.hpp
 * @brief The vlace `std:fun` prelude as vlace source
 *        (design/vlace-functional-language.md §7.4).
 *
 * These are the names a fresh program expects in scope without an explicit
 * `import`. Rather than wire each as a C++ native, the prelude is written in
 * vlace itself (value declarations) and prepended to a program before parsing,
 * so the evaluator's existing ADT/case/laziness/letrec/arithmetic machinery is
 * all that runs them -- §7.2's "every std:fun symbol is a routine" realised as
 * surface source for the Stage 6 tree-walker.
 *
 * Only language features already evaluated are used: lambdas, application,
 * `case`, cons/`[]`, tuples, arithmetic, comparison, and top-level recursion
 * (a top-level value decl captures `globals_`, so a definition may call
 * itself and its siblings). Each definition is a single value decl terminated
 * by `;` so the whole block parses as a decl sequence ahead of a program's own
 * declarations and trailing expression.
 *
 * Full Hindley-Milner typing of the prelude is a later increment; a program
 * that prepends it should evaluate with type-checking off (the compiler path),
 * which is why the prelude is a Compiler input rather than a Driver one.
 */
#ifndef COMMON_XANADU_VLACE_PRELUDE_HPP
#define COMMON_XANADU_VLACE_PRELUDE_HPP

#include <string_view>

namespace xanadu::vlace {

/// The prelude, as vlace declarations. Prepend to a program's source (before
/// its own decls and trailing expression) and parse the whole.
inline constexpr std::string_view kPrelude = R"VLACE(
id x = x ;
const x y = x ;
flip f x y = f y x ;
compose f g x = f (g x) ;

fst p = case p of (a, b) => a ;
snd p = case p of (a, b) => b ;

not b = if b then False else True ;
and_ a b = if a then b else False ;
or_ a b = if a then True else b ;

map f xs = case xs of
  [] => [] ;
  (h : t) => f h : map f t ;

filter p xs = case xs of
  [] => [] ;
  (h : t) => if p h then h : filter p t else filter p t ;

find p xs = case xs of
  [] => None ;
  (h : t) => if p h then Some h else find p t ;

foldr f z xs = case xs of
  [] => z ;
  (h : t) => f h (foldr f z t) ;

foldl f z xs = case xs of
  [] => z ;
  (h : t) => foldl f (f z h) t ;

length xs = case xs of
  [] => 0 ;
  (h : t) => 1 + length t ;

append xs ys = case xs of
  [] => ys ;
  (h : t) => h : append t ys ;

reverse xs = foldl (flip cons) [] xs ;

cons h t = h : t ;

take n xs = if n <= 0 then [] else case xs of
  [] => [] ;
  (h : t) => h : take (n - 1) t ;

drop n xs = if n <= 0 then xs else case xs of
  [] => [] ;
  (h : t) => drop (n - 1) t ;

repeat x = x : repeat x ;

iterate f x = x : iterate f (f x) ;
)VLACE";

} // namespace xanadu::vlace

#endif // COMMON_XANADU_VLACE_PRELUDE_HPP
