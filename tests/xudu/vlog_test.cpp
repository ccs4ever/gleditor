/**
 * @file vlog_test.cpp
 * @brief Unification as a program over link and value: Vlog §4.
 *
 * The document's claim is that a resolution engine needs no primitive of its
 * own. These tests are what would catch that being false -- each is a case
 * where a conventional Prolog reaches for machinery (a trail, a heap, an
 * occurs check, a binding environment) that is not present here.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include <zigzag/core/arena_manifold.hpp>
#include <zigzag/core/vlog.hpp>

namespace {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::noCell;
using zigzag::Vlog;

struct Engine {
  ArenaManifold arena;
  Vlog v = Vlog::over(arena);
};

TEST(VlogTest, aFreshVariableIsUnboundAndAnAtomIsNot) {
  Engine e;
  const auto x = e.v.makeVar();
  EXPECT_TRUE(e.v.isUnbound(x));

  // The rank membership is what tells an unbound variable from the atom '',
  // which is otherwise the same cell: bare content, no value.
  const auto emptyAtom = e.arena.makeCell("");
  EXPECT_FALSE(e.v.isUnbound(emptyAtom));
  EXPECT_FALSE(e.v.isUnbound(e.arena.makeCell("foo")));
}

TEST(VlogTest, bindingAVariableToAnAtomIsOneLink) {
  Engine e;
  const auto x   = e.v.makeVar();
  const auto foo = e.arena.makeCell("foo");

  ASSERT_TRUE(e.v.unify(x, foo));
  EXPECT_FALSE(e.v.isUnbound(x));
  EXPECT_EQ(e.v.deref(x), foo);
  EXPECT_EQ(e.arena.textOf(e.v.deref(x)), "foo");
}

TEST(VlogTest, theVarVarCaseIsWhatDecidesWhetherBindingIsCorrect) {
  Engine e;
  const auto x = e.v.makeVar();
  const auto y = e.v.makeVar();

  // X = Y with both unbound: nothing to copy, and no pointer installed that a
  // later step would have to overwrite.
  ASSERT_TRUE(e.v.unify(x, y));
  EXPECT_TRUE(e.v.isUnbound(x));
  EXPECT_EQ(e.v.deref(x), e.v.deref(y));

  // Y = foo, and X must now read foo. The clone rank's own headline property
  // is the propagation that makes this work.
  const auto foo = e.arena.makeCell("foo");
  ASSERT_TRUE(e.v.unify(y, foo));
  EXPECT_EQ(e.v.deref(x), foo);
  EXPECT_FALSE(e.v.isUnbound(x));
}

TEST(VlogTest, atomsUnifyByContentAndRefuseWhenTheyDiffer) {
  Engine e;
  EXPECT_TRUE(e.v.unify(e.arena.makeCell("a"), e.arena.makeCell("a")));
  EXPECT_FALSE(e.v.unify(e.arena.makeCell("a"), e.arena.makeCell("b")));
}

TEST(VlogTest, numbersUnifyByTheirBitsAndNeverByParsingText) {
  Engine e;
  // Two cells holding 3.14 are two cells at two addresses -- R6 refuses to
  // make numeric coincidence into quotation -- so equality has to come from
  // the bits, and this is the test that it does.
  EXPECT_TRUE(
      e.v.unify(e.arena.makeScalarCell(3.14), e.arena.makeScalarCell(3.14)));
  EXPECT_FALSE(
      e.v.unify(e.arena.makeScalarCell(3.14), e.arena.makeScalarCell(2.72)));

  // A number and a flag are different things, and reading one as the other is
  // not unification's decision to make.
  EXPECT_FALSE(
      e.v.unify(e.arena.makeScalarCell(1.0), e.arena.makeScalarCell(true)));
  EXPECT_FALSE(e.v.unify(e.arena.makeScalarCell(1.0), e.arena.makeCell("1.0")));
}

TEST(VlogTest, compoundTermsUnifyArgumentwise) {
  Engine e;
  const auto x = e.v.makeVar();
  const auto y = e.v.makeVar();

  // f(X, b) = f(a, Y)
  const auto left  = e.v.makeTerm("f", {x, e.arena.makeCell("b")});
  const auto right = e.v.makeTerm("f", {e.arena.makeCell("a"), y});

  ASSERT_TRUE(e.v.unify(left, right));
  EXPECT_EQ(e.arena.textOf(e.v.deref(x)), "a");
  EXPECT_EQ(e.arena.textOf(e.v.deref(y)), "b");
}

TEST(VlogTest, aFunctorOrArityMismatchFails) {
  Engine e;
  const auto a = e.arena.makeCell("a");
  EXPECT_FALSE(e.v.unify(e.v.makeTerm("f", {a}), e.v.makeTerm("g", {a})));
  EXPECT_FALSE(e.v.unify(e.v.makeTerm("f", {a}), e.v.makeTerm("f", {a, a})));
  // An atom is not a term of arity zero with the same name -- it *is* one, and
  // this is the case that says so.
  EXPECT_TRUE(e.v.unify(e.arena.makeCell("f"), e.arena.makeCell("f")));
}

TEST(VlogTest, nestedTermsRecurse) {
  Engine e;
  const auto x     = e.v.makeVar();
  const auto inner = e.v.makeTerm("g", {e.arena.makeCell("deep")});
  const auto left  = e.v.makeTerm("f", {e.v.makeTerm("g", {x})});

  ASSERT_TRUE(e.v.unify(left, e.v.makeTerm("f", {inner})));
  EXPECT_EQ(e.arena.textOf(e.v.deref(x)), "deep");
}

TEST(VlogTest, occursCheckOffMeansXEqualsFOfXBuildsARationalTerm) {
  Engine e;
  const auto x  = e.v.makeVar();
  const auto fx = e.v.makeTerm("f", {x});

  // Deliberately not ISO (§4.4): a zzstructure is happy with a cycle, and
  // cloneMaster()'s guard -- written for an unrelated reason -- is what stops
  // this spinning rather than a check anyone added for it.
  ASSERT_TRUE(e.v.unify(x, fx));
  EXPECT_NE(e.v.deref(x), noCell);
  EXPECT_EQ(e.arena.textOf(e.v.deref(x)), "f");
}

TEST(VlogTest, aFailedUnificationIsUndoneByReleasingTheMark) {
  Engine e;
  const auto x = e.v.makeVar();
  const auto y = e.v.makeVar();

  // f(X, a) against f(b, c): X binds, then the second argument refuses. The
  // partial binding is left behind on purpose -- unwinding one binding at a
  // time is what §5.2 exists to avoid.
  const auto left = e.v.makeTerm("f", {x, e.arena.makeCell("a")});
  const auto right =
      e.v.makeTerm("f", {e.arena.makeCell("b"), e.arena.makeCell("c")});

  const auto mark = e.arena.mark();
  EXPECT_FALSE(e.v.unify(left, right));
  EXPECT_FALSE(e.v.isUnbound(x));

  e.arena.release(mark);
  EXPECT_TRUE(e.v.isUnbound(x));
  EXPECT_TRUE(e.v.isUnbound(y));
}

TEST(VlogTest, retryingAfterAReleaseBindsTheOtherWay) {
  Engine e;
  const auto x = e.v.makeVar();

  // The shape of a two-clause predicate, without a solver: try, fail, undo,
  // try the next. Neither attempt knows about the other.
  const auto first = e.arena.mark();
  ASSERT_TRUE(e.v.unify(x, e.arena.makeCell("one")));
  EXPECT_EQ(e.arena.textOf(e.v.deref(x)), "one");
  e.arena.release(first);

  const auto second = e.arena.mark();
  ASSERT_TRUE(e.v.unify(x, e.arena.makeCell("two")));
  EXPECT_EQ(e.arena.textOf(e.v.deref(x)), "two");
  e.arena.discard(second);

  // Discarded rather than released: this one succeeded, so the binding stands.
  EXPECT_EQ(e.arena.textOf(e.v.deref(x)), "two");
}

TEST(VlogTest, unifyingACellWithItselfSucceedsWithoutBinding) {
  Engine e;
  const auto x = e.v.makeVar();
  EXPECT_TRUE(e.v.unify(x, x));
  EXPECT_TRUE(e.v.isUnbound(x));
}

} // namespace
